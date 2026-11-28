/*
 * Copyright (c) 2026 Farlink contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include <chrono>
#include <cstdio>

#include "alr_detector.h"
#include "probe_controller.h"
#include "runtime/connection_latency.h"
#include "video_encoder_rate_control.h"

namespace {
using crossdesk::gui_detail::ConnectionLatencyStats;
using minirtc::ShouldApplyEncoderBitrate;
using namespace minirtc::webrtc;
using namespace std::chrono_literals;

int failures = 0;
void Expect(bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
  }
}

void TestCodecWrites() {
  constexpr int rate = 10'000'000;
  for (int ms = 25; ms <= 1000; ms += 25) {
    const int noise = ms % 50 == 0 ? 1 : -1;
    Expect(!ShouldApplyEncoderBitrate(rate + noise, rate, ms, false),
           "estimator rounding must not reconfigure the codec");
  }
  Expect(!ShouldApplyEncoderBitrate(11'000'000, rate, 25, false),
         "a brief recovery cannot immediately undo the previous reduction");
  Expect(ShouldApplyEncoderBitrate(11'000'000, rate, 250, false),
         "an increased allocation can reach the codec after the hold");
  Expect(ShouldApplyEncoderBitrate(9'000'000, rate, 25, false),
         "congestion reduction bypasses the hold");
  Expect(ShouldApplyEncoderBitrate(rate - 1, rate, 25, true),
         "an explicit lower cap bypasses the deadband");
  Expect(!ShouldApplyEncoderBitrate(0, rate, 500, true),
         "exhausted budget pauses admission instead of writing zero");
}

void TestDirectProbe() {
  AlrDetector alr;
  alr.SetEstimatedBitrate(8'000'000);
  alr.OnBytesSent(1200, 1000);
  alr.OnBytesSent(1200, 1500);
  Expect(alr.GetApplicationLimitedRegionStartTime() == 1500,
         "ALR timers must use the transport's timestamp domain");

  ProbeController probe(false);
  const auto estimate = DataRate::BitsPerSec(8'000'000);
  const auto at = [](int ms) { return Timestamp::Millis(ms); };
  const auto cause = BandwidthLimitedCause::kDelayBasedLimited;
  (void)probe.SetBitrates(DataRate::BitsPerSec(300'000), estimate,
                         DataRate::BitsPerSec(80'000'000), at(1000));
  const auto startup = probe.OnNetworkAvailability({at(1100), true});
  Expect(!startup.empty(), "steady-state policy must not disable direct startup probing");
  if (startup.empty()) return;
  probe.OnProbeAborted(startup.back().id);
  probe.EnablePeriodicAlrProbing(true);
  probe.SetAlrStartTime(at(1500));
  probe.OnTransportFeedback(at(6500), TimeDelta::Millis(10));
  probe.SetTransportLossSample(100, 12);
  Expect(probe.Process(at(6500)).empty(),
         "fresh settled losses must block direct probing");
  probe.SetTransportLossSample(100, 0);
  const auto step = probe.Process(at(6525));
  Expect(step.size() == 1, "direct ALR emits just one probe cluster");
  if (step.size() != 1) return;
  Expect(step.front().target_data_rate.bps() == 10'000'000,
         "direct ALR probes only 25 percent above the current estimate");
  probe.OnProbeSent(step.front().id, at(6530));
  probe.OnProbeResult(step.front().id, 1.0);
  Expect(probe.SetEstimatedBitrate(step.front().target_data_rate, cause,
                                   at(6550)).empty(),
         "a successful direct ALR probe must not start an exponential chain");
}

void TestRttDisplay() {
  ConnectionLatencyStats rtt;
  const auto start = ConnectionLatencyStats::Clock::time_point{};
  rtt.Update(8, start);
  rtt.Update(110, start + 1s);
  rtt.Update(9, start + 2s);
  Expect(rtt.Get(start + 2s) == 9, "one delayed report must not swing HUD RTT");
  rtt.Update(43, start + 3s);
  rtt.Update(44, start + 4s);
  Expect(rtt.Get(start + 4s) == 43, "sustained RTT changes must reach the HUD");
  rtt.Update(-1, start + 5s);
  Expect(!rtt.Get(start + 7s), "missing replies must not renew stale RTT");
  rtt.Update(7, start + 8s);
  Expect(rtt.Get(start + 8s) == 7, "a fresh sample must not reuse the expired window");
}
}  // namespace

int main() {
  TestCodecWrites();
  TestDirectProbe();
  TestRttDisplay();
  if (!failures) std::puts("stream_scheduling_test: all tests passed");
  return failures != 0;
}
