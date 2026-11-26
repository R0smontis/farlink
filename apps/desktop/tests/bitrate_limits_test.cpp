/*
 * Copyright (c) 2026 Farlink contributors
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */

// Deterministic unit test for the D2 high-bitrate ceiling.
//
//  1. The pure helpers in minirtc::bitrate_limits.h — default cap values, the
//     MINIRTC_MAX_*_BITRATE_BPS parsing contract (injected as raw strings so
//     the test never mutates the process environment), StartingBitrates
//     validation against the raised ceiling, ClampEncoderTargetBitrate,
//     EncoderPeakBitrate and EncoderOvershootPercent including oversized and
//     invalid inputs.
//
//  2. The live CongestionControl object: the constructor must seed
//     TargetRateConstraints.max_data_rate at the process ceiling, and
//     SetMaxTargetBitrate must propagate to that same max_data_rate_ — the
//     exact regression spec D2 identified, where the runtime setter touched
//     probe allocation only and the estimate stayed clamped at the compile-
//     time constant. The propagation is observed through the read-only
//     max_data_rate_for_testing() accessor after driving the first process
//     tick, which is what publishes the initial constraints.

#include <cstdint>
#include <cstdio>

#include "bitrate_limits.h"
#include "congestion_control.h"

namespace {

using minirtc::StartingBitrates;
using minirtc::webrtc::DataRate;
using minirtc::webrtc::Timestamp;

int g_failures = 0;

void Expect(bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++g_failures;
  }
}

void ExpectEq(int64_t actual, int64_t expected, const char* message) {
  if (actual != expected) {
    std::fprintf(stderr, "FAIL: %s (actual=%lld expected=%lld)\n", message,
                 static_cast<long long>(actual),
                 static_cast<long long>(expected));
    ++g_failures;
  }
}

// ---------------------------------------------------------------------------
// 1. D2 default ceilings.
// ---------------------------------------------------------------------------
void TestDefaultCaps() {
  ExpectEq(minirtc::kDefaultMaxNetworkBitrateBps, 80'000'000,
           "network ceiling default is 80 Mbps");
  ExpectEq(minirtc::kDefaultMaxEncoderBitrateBps, 100'000'000,
           "encoder ceiling default is 100 Mbps");
  // The process-resolve functions agree with the compile-time constants when
  // no override env var is set (the harness environment never sets them).
  ExpectEq(minirtc::MaxNetworkBitrateCap(), 80'000'000,
           "network cap resolves to default without env");
  ExpectEq(minirtc::MaxEncoderBitrateCap(), 100'000'000,
           "encoder cap resolves to default without env");
}

// ---------------------------------------------------------------------------
// 2. Env parsing contract, injected as raw strings.
// ---------------------------------------------------------------------------
void TestParseBitrateCapEnv() {
  constexpr int kFallback = 80'000'000;
  // Absent/empty keeps the fallback.
  ExpectEq(minirtc::ParseBitrateCapEnv(nullptr, kFallback), kFallback,
           "nullptr env keeps fallback");
  ExpectEq(minirtc::ParseBitrateCapEnv("", kFallback), kFallback,
           "empty env keeps fallback");
  // The 20 Mbps control arm parses exactly.
  ExpectEq(minirtc::ParseBitrateCapEnv("20000000", kFallback), 20'000'000,
           "20M control arm parses exactly");
  ExpectEq(minirtc::ParseBitrateCapEnv("100000000", kFallback), 100'000'000,
           "100M parses exactly");
  // Out-of-range and malformed values are rejected as configuration errors.
  ExpectEq(minirtc::ParseBitrateCapEnv("999999", kFallback), kFallback,
           "below 1 Mbps rejected");
  ExpectEq(minirtc::ParseBitrateCapEnv("500000001", kFallback), kFallback,
           "above 500 Mbps rejected");
  ExpectEq(minirtc::ParseBitrateCapEnv("abc", kFallback), kFallback,
           "non-numeric rejected");
  ExpectEq(minirtc::ParseBitrateCapEnv("20000000x", kFallback), kFallback,
           "trailing garbage rejected");
  ExpectEq(minirtc::ParseBitrateCapEnv("-5000000", kFallback), kFallback,
           "negative rejected");
  // Boundary values are accepted.
  ExpectEq(minirtc::ParseBitrateCapEnv("1000000", kFallback), 1'000'000,
           "exactly 1 Mbps accepted");
  ExpectEq(minirtc::ParseBitrateCapEnv("500000000", kFallback), 500'000'000,
           "exactly 500 Mbps accepted");
}

// ---------------------------------------------------------------------------
// 3. StartingBitrates validation against the raised ceiling.
// ---------------------------------------------------------------------------
void TestStartingBitrates() {
  // Direct starts at 8 Mbps: the D2 A/B (wan/D2_BITRATE_AB.md cell 3)
  // measured 8M+probe equal-or-smoother than 2.5M+probe on a constrained
  // 4 Mbps WAN across every aggregate metric, so the default was raised.
  // The relay start stays at the conservative 1.5 Mbps.
  StartingBitrates rates;
  ExpectEq(rates.direct_bps, 8'000'000, "direct start default is 8 Mbps");
  ExpectEq(rates.relay_bps, 1'500'000, "relay start default stays 1.5 Mbps");

  // Values that were invalid under the old 20 Mbps ceiling are valid now.
  Expect(StartingBitrates::IsValid(8'000'000), "8 Mbps start is valid");
  Expect(StartingBitrates::IsValid(50'000'000), "50 Mbps start is valid");
  Expect(StartingBitrates::IsValid(80'000'000),
         "start exactly at the ceiling is valid");
  Expect(!StartingBitrates::IsValid(80'000'001),
         "start above the ceiling is invalid");
  Expect(!StartingBitrates::IsValid(299'999), "below 300k invalid");
  Expect(!StartingBitrates::IsValid(0), "zero invalid");
  Expect(!StartingBitrates::IsValid(-1), "negative invalid");

  // Normalize accepts an 8 Mbps direct start (the A/B variant) and rejects
  // oversized garbage, falling back to defaults per-field.
  const auto normalized = StartingBitrates::Normalize(8'000'000, 100'000'000);
  ExpectEq(normalized.direct_bps, 8'000'000,
           "Normalize keeps a valid 8 Mbps direct start");
  ExpectEq(normalized.relay_bps, 1'500'000,
           "Normalize rejects oversized relay start back to default");

  // Parse round-trips valid strings and falls back on invalid ones.
  ExpectEq(StartingBitrates::Parse("8000000", 2'500'000), 8'000'000,
           "Parse accepts 8 Mbps");
  ExpectEq(StartingBitrates::Parse("99999999", 2'500'000), 2'500'000,
           "Parse rejects oversized value to fallback");
  ExpectEq(StartingBitrates::Parse("", 2'500'000), 2'500'000,
           "Parse empty falls back");
}

// ---------------------------------------------------------------------------
// 4. Encoder clamp helpers.
// ---------------------------------------------------------------------------
void TestEncoderClampHelpers() {
  constexpr int kMax = 100'000'000;
  // ClampEncoderTargetBitrate caps at max but never raises.
  ExpectEq(minirtc::ClampEncoderTargetBitrate(60'000'000, kMax), 60'000'000,
           "target below max passes through");
  ExpectEq(minirtc::ClampEncoderTargetBitrate(150'000'000, kMax), kMax,
           "target above max clamps to max");
  ExpectEq(minirtc::ClampEncoderTargetBitrate(kMax, kMax), kMax,
           "target exactly at max passes through");

  // EncoderPeakBitrate allows 50% overshoot, bounded by the cap.
  ExpectEq(minirtc::EncoderPeakBitrate(40'000'000, kMax), 60'000'000,
           "peak is 1.5x target when it fits");
  ExpectEq(minirtc::EncoderPeakBitrate(80'000'000, kMax), kMax,
           "peak clamps at max");
  ExpectEq(minirtc::EncoderPeakBitrate(kMax, kMax), kMax,
           "peak at max stays at max");

  // EncoderOvershootPercent: zero when the target saturates the cap, otherwise
  // the smaller of the headroom percentage and the 50% design overshoot.
  ExpectEq(minirtc::EncoderOvershootPercent(0, kMax), 0,
           "zero target has zero overshoot");
  ExpectEq(minirtc::EncoderOvershootPercent(kMax, kMax), 0,
           "saturated target has zero overshoot");
  ExpectEq(minirtc::EncoderOvershootPercent(40'000'000, kMax), 50,
           "ample headroom keeps the 50 percent design overshoot");
  ExpectEq(minirtc::EncoderOvershootPercent(80'000'000, kMax), 25,
           "tight headroom limits overshoot to what fits");
}

// ---------------------------------------------------------------------------
// 5. Live CongestionControl: construction and SetMaxTargetBitrate propagation.
// ---------------------------------------------------------------------------
void TestCongestionControlMaxDataRate() {
  minirtc::CongestionControl cc;

  // Drive the first process tick; this is what publishes initial_constraints_
  // into the live min/max data rates via ResetConstraints.
  minirtc::webrtc::ProcessInterval tick;
  tick.at_time = Timestamp::Millis(0);
  cc.OnNetworkAvailability({Timestamp::Millis(0), true});
  cc.OnProcessInterval(tick);

  // The constructor must seed max_data_rate at the D2 process ceiling, not at
  // the legacy 20 Mbps compile-time constant.
  ExpectEq(cc.max_data_rate_for_testing().bps(), 80'000'000,
           "initial max_data_rate is the 80 Mbps D2 ceiling");

  // Raising the limit toward a LAN budget must propagate to max_data_rate_ —
  // the regression this test exists to pin down.
  cc.SetMaxTargetBitrate(DataRate::BitsPerSec(80'000'000));
  ExpectEq(cc.max_data_rate_for_testing().bps(), 80'000'000,
           "SetMaxTargetBitrate(80M) reaches max_data_rate");

  // Reducing the ceiling propagates downward too (constrained-budget demand).
  cc.SetMaxTargetBitrate(DataRate::BitsPerSec(12'000'000));
  ExpectEq(cc.max_data_rate_for_testing().bps(), 12'000'000,
           "SetMaxTargetBitrate(12M) lowers max_data_rate");

  // Degenerate inputs are rejected: zero, negative, infinity must not move the
  // ceiling. Repeating the current value is a documented no-op.
  cc.SetMaxTargetBitrate(DataRate::Zero());
  ExpectEq(cc.max_data_rate_for_testing().bps(), 12'000'000,
           "zero target rejected");
  cc.SetMaxTargetBitrate(DataRate::PlusInfinity());
  ExpectEq(cc.max_data_rate_for_testing().bps(), 12'000'000,
           "infinite target rejected");

  // The min-bitrate floor clamp: pushing max below the 300k minimum re-raises
  // it to the minimum rather than inverting the range.
  cc.SetMaxTargetBitrate(DataRate::BitsPerSec(100'000));
  Expect(cc.max_data_rate_for_testing().bps() >= 300'000,
         "max never drops below the minimum bitrate floor");
}

}  // namespace

int main() {
  TestDefaultCaps();
  TestParseBitrateCapEnv();
  TestStartingBitrates();
  TestEncoderClampHelpers();
  TestCongestionControlMaxDataRate();

  if (g_failures == 0) {
    std::printf("PASS: all bitrate-limits scenarios\n");
    return 0;
  }
  std::fprintf(stderr, "FAIL: %d expectation(s) failed\n", g_failures);
  return 1;
}
