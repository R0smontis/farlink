/*
 * Copyright (c) 2026 Farlink contributors
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */

// Deterministic unit test for the D6a dynamic FPS-cap degradation ladder.
//
// Two layers are exercised:
//
//  1. The pure policy predicates in minirtc::VideoAdaptationPolicy — the exact
//     functions IceTransportController calls verbatim. These are compile-time
//     pure integer/enum logic, so they are asserted directly: rung walk order,
//     per-preference floors, the high-FPS relevance gate, the capture-limited
//     guard, and the MINIRTC_FPS_CAP_LADDER parsing contract.
//
//  2. The emergent state-machine contract. A full ICE/DTLS/SRTP transport
//     cannot drive the encoder deterministically, so a scripted millisecond
//     timeline replicates the controller's EvaluateFpsCapLadder branching using
//     the same policy predicates and the same cooldown/hold constants. What is
//     asserted is the behavioral contract, not the implementation: step-down is
//     cadence-gated by the cooldown, step-up demands the longer healthy hold
//     and the longer cooldown, a 0 Hz idle screen freezes the ladder entirely,
//     and MaintainFrameRate never steps below 60 while Balanced/MaintainResolution
//     may reach 48 before resolution is touched.

#include <cstdint>
#include <cstdio>

#include "video_adaptation_policy.h"

namespace {

using minirtc::VideoAdaptationPolicy;
// MiniRtcVideoDegradationPreference is declared in the global namespace by
// minirtc.h; the policy predicates reference it unqualified inside namespace
// minirtc, so the test uses it unqualified too.

int g_failures = 0;

void Expect(bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++g_failures;
  }
}

constexpr auto kMFR = MiniRtcVideoDegradationPreference::MaintainFrameRate;
constexpr auto kMR = MiniRtcVideoDegradationPreference::MaintainResolution;
constexpr auto kBAL = MiniRtcVideoDegradationPreference::Balanced;

// ---------------------------------------------------------------------------
// 1. Pure policy: rung walk order.
// ---------------------------------------------------------------------------
void TestLowerRungs() {
  Expect(VideoAdaptationPolicy::NextLowerFrameRateCap(144) == 120,
         "144 -> 120");
  Expect(VideoAdaptationPolicy::NextLowerFrameRateCap(121) == 120,
         "anything above 120 steps to 120");
  Expect(VideoAdaptationPolicy::NextLowerFrameRateCap(120) == 96,
         "120 -> 96");
  Expect(VideoAdaptationPolicy::NextLowerFrameRateCap(97) == 96,
         "anything above 96 steps to 96");
  Expect(VideoAdaptationPolicy::NextLowerFrameRateCap(96) == 60,
         "96 -> 60");
  Expect(VideoAdaptationPolicy::NextLowerFrameRateCap(61) == 60,
         "anything above 60 steps to 60");
  Expect(VideoAdaptationPolicy::NextLowerFrameRateCap(60) == 48,
         "60 -> 48");
  Expect(VideoAdaptationPolicy::NextLowerFrameRateCap(49) == 48,
         "anything above 48 steps to 48");
  Expect(VideoAdaptationPolicy::NextLowerFrameRateCap(48) == 0,
         "48 terminates the ladder (resolution must adapt)");
  Expect(VideoAdaptationPolicy::NextLowerFrameRateCap(30) == 0,
         "below the ladder entirely: no temporal step");
}

void TestHigherRungs() {
  // From the bottom rung the walk returns towards the configured ceiling.
  Expect(VideoAdaptationPolicy::NextHigherFrameRateCap(48, 144) == 60,
         "48 climbs to the 60 floor first");
  Expect(VideoAdaptationPolicy::NextHigherFrameRateCap(60, 144) == 96,
         "60 -> 96");
  Expect(VideoAdaptationPolicy::NextHigherFrameRateCap(96, 144) == 120,
         "96 -> 120");
  Expect(VideoAdaptationPolicy::NextHigherFrameRateCap(120, 144) == 144,
         "120 -> back to configured 144");
  Expect(VideoAdaptationPolicy::NextHigherFrameRateCap(144, 144) == 0,
         "already at the ceiling: no step");
  // A 120-configured session caps the climb at 120.
  Expect(VideoAdaptationPolicy::NextHigherFrameRateCap(60, 120) == 96,
         "60 -> 96 even when configured is 120");
  Expect(VideoAdaptationPolicy::NextHigherFrameRateCap(96, 120) == 120,
         "96 -> 120 when configured is 120");
  Expect(VideoAdaptationPolicy::NextHigherFrameRateCap(120, 120) == 0,
         "at configured 120: no step");
  // Configured below 60 (degenerate): the first recovery step is the ceiling.
  Expect(VideoAdaptationPolicy::NextHigherFrameRateCap(48, 60) == 60,
         "48 -> configured 60 when ceiling is 60");
}

// ---------------------------------------------------------------------------
// 2. Pure policy: per-preference floors and the step-down gate.
// ---------------------------------------------------------------------------
void TestFloors() {
  Expect(VideoAdaptationPolicy::FrameRateCapFloor(kMFR) == 60,
         "MaintainFrameRate floor is 60");
  Expect(VideoAdaptationPolicy::FrameRateCapFloor(kMR) == 48,
         "MaintainResolution floor is 48");
  Expect(VideoAdaptationPolicy::FrameRateCapFloor(kBAL) == 48,
         "Balanced floor is 48");
}

void TestStepDownGate() {
  // High-FPS sessions step down before resolution in every mode while above
  // the floor.
  for (auto pref : {kMFR, kMR, kBAL}) {
    Expect(VideoAdaptationPolicy::ShouldStepDownFpsCapBeforeResolution(
               144, 144, pref, false),
           "144-configured session steps down at 144");
    Expect(VideoAdaptationPolicy::ShouldStepDownFpsCapBeforeResolution(
               120, 96, pref, false),
           "steps down at 96 towards 60");
  }
  // MaintainFrameRate stops at 60: at cap 60 the next rung (48) is below its
  // floor, so the gate closes and resolution must take over.
  Expect(!VideoAdaptationPolicy::ShouldStepDownFpsCapBeforeResolution(
             144, 60, kMFR, false),
         "MaintainFrameRate never steps below 60");
  // Balanced and MaintainResolution still have the 48 rung available at cap 60.
  Expect(VideoAdaptationPolicy::ShouldStepDownFpsCapBeforeResolution(
             144, 60, kMR, false),
         "MaintainResolution steps 60 -> 48");
  Expect(VideoAdaptationPolicy::ShouldStepDownFpsCapBeforeResolution(
             144, 60, kBAL, false),
         "Balanced steps 60 -> 48");
  // At 48 the ladder is exhausted for every mode.
  for (auto pref : {kMFR, kMR, kBAL}) {
    Expect(!VideoAdaptationPolicy::ShouldStepDownFpsCapBeforeResolution(
               144, 48, pref, false),
           "at 48 the ladder is exhausted");
  }
  // A 60 fps (or lower) configured session has no temporal headroom at all.
  for (auto pref : {kMFR, kMR, kBAL}) {
    Expect(!VideoAdaptationPolicy::ShouldStepDownFpsCapBeforeResolution(
               60, 60, pref, false),
           "60-configured session never uses the ladder");
    Expect(!VideoAdaptationPolicy::ShouldStepDownFpsCapBeforeResolution(
               30, 30, pref, false),
           "30-configured session never uses the ladder");
  }
  // Capture-limited: lowering the cap cannot create input frames, so the gate
  // closes regardless of headroom.
  Expect(!VideoAdaptationPolicy::ShouldStepDownFpsCapBeforeResolution(
             144, 144, kMFR, true),
         "capture-limited blocks the step even with headroom");
  Expect(!VideoAdaptationPolicy::ShouldStepDownFpsCapBeforeResolution(
             144, 96, kMR, true),
         "capture-limited blocks Balanced/Resolution too");
}

void TestRelevanceAndStepUp() {
  Expect(VideoAdaptationPolicy::IsFpsCapLadderRelevant(144), "144 relevant");
  Expect(VideoAdaptationPolicy::IsFpsCapLadderRelevant(120), "120 relevant");
  Expect(VideoAdaptationPolicy::IsFpsCapLadderRelevant(61), "61 relevant");
  Expect(!VideoAdaptationPolicy::IsFpsCapLadderRelevant(60), "60 not relevant");
  Expect(!VideoAdaptationPolicy::IsFpsCapLadderRelevant(30), "30 not relevant");

  Expect(VideoAdaptationPolicy::ShouldStepUpFpsCap(144, 60),
         "step up available from 60 in a 144 session");
  Expect(VideoAdaptationPolicy::ShouldStepUpFpsCap(144, 120),
         "step up available from 120 in a 144 session");
  Expect(!VideoAdaptationPolicy::ShouldStepUpFpsCap(144, 144),
         "no step up at the ceiling");
  Expect(!VideoAdaptationPolicy::ShouldStepUpFpsCap(60, 48),
         "60-configured session never steps up (not relevant)");
}

// ---------------------------------------------------------------------------
// 3. Pure policy: env kill-switch parsing contract.
// ---------------------------------------------------------------------------
void TestEnvParsing() {
  Expect(VideoAdaptationPolicy::IsFpsCapLadderEnabled(nullptr),
         "unset env defaults to enabled");
  Expect(VideoAdaptationPolicy::IsFpsCapLadderEnabled("1"),
         "=1 enabled");
  Expect(VideoAdaptationPolicy::IsFpsCapLadderEnabled("yes"),
         "any non-zero string stays enabled");
  Expect(!VideoAdaptationPolicy::IsFpsCapLadderEnabled("0"),
         "=0 disables (exact A/B)");
  Expect(VideoAdaptationPolicy::IsFpsCapLadderEnabled("00"),
         "only the exact string \"0\" disables; \"00\" stays enabled");
  Expect(VideoAdaptationPolicy::IsFpsCapLadderEnabled(""),
         "empty string is not \"0\": stays enabled");
}

// ---------------------------------------------------------------------------
// 4. Emergent state-machine contract.
//
// Replicates the branching of IceTransportController::EvaluateFpsCapLadder
// using the same policy predicates and constants, driven over a scripted
// timeline. This is the same simulation-harness pattern as
// encoder_ceiling_reprobe_test: it asserts the hysteresis/cooldown/idle-guard
// contract, not the implementation.
// ---------------------------------------------------------------------------
struct LadderSim {
  int configured = 144;
  int cap = 144;
  int64_t last_transition_ms = 0;
  int64_t healthy_since_ms = 0;
  bool last_transition_initialized = false;

  // Mirrors EvaluateFpsCapLadder. Returns the new cap when a transition fires,
  // or the unchanged cap otherwise.
  int Tick(bool pressure, bool capture_limited, bool zero_hz_idle,
           bool encoded_ready, int measured_fps, int64_t now_ms,
           MiniRtcVideoDegradationPreference pref) {
    const int upgrade_bar =
        VideoAdaptationPolicy::UpgradeFrameRate(cap, false);
    const bool healthy_now = encoded_ready && measured_fps >= upgrade_bar;

    if (zero_hz_idle) return cap;  // idle freezes the ladder entirely

    const bool stepdown_cooled =
        !last_transition_initialized ||
        now_ms - last_transition_ms >=
            VideoAdaptationPolicy::kFpsCapStepDownCooldownMs;
    if (pressure && stepdown_cooled &&
        VideoAdaptationPolicy::ShouldStepDownFpsCapBeforeResolution(
            configured, cap, pref, capture_limited)) {
      cap = VideoAdaptationPolicy::NextLowerFrameRateCap(cap);
      last_transition_ms = now_ms;
      last_transition_initialized = true;
      healthy_since_ms = 0;
      return cap;
    }

    if (healthy_now) {
      if (healthy_since_ms == 0) healthy_since_ms = now_ms;
    } else {
      healthy_since_ms = 0;
    }
    const bool stepup_cooled =
        !last_transition_initialized ||
        now_ms - last_transition_ms >=
            VideoAdaptationPolicy::kFpsCapStepUpCooldownMs;
    const bool stepup_held =
        healthy_since_ms > 0 &&
        now_ms - healthy_since_ms >= VideoAdaptationPolicy::kFpsCapStepUpHoldMs;
    if (!pressure && healthy_now && stepup_held && stepup_cooled &&
        !capture_limited &&
        VideoAdaptationPolicy::ShouldStepUpFpsCap(configured, cap)) {
      cap = VideoAdaptationPolicy::NextHigherFrameRateCap(cap, configured);
      last_transition_ms = now_ms;
      last_transition_initialized = true;
      healthy_since_ms = 0;
      return cap;
    }
    return cap;
  }
};

void TestStepDownCooldownHysteresis() {
  LadderSim sim;
  // Sustained pressure: first tick steps down immediately (no prior
  // transition), but the cooldown must prevent a second step within 3 s.
  int cap = sim.Tick(true, false, false, true, 100, 1000, kMFR);
  Expect(cap == 120, "first pressure tick steps 144 -> 120");
  cap = sim.Tick(true, false, false, true, 80, 2000, kMFR);
  Expect(cap == 120, "cooldown blocks a second step 1 s later");
  cap = sim.Tick(true, false, false, true, 80, 3999, kMFR);
  Expect(cap == 120, "cooldown still blocks at t=+2999 ms");
  cap = sim.Tick(true, false, false, true, 80, 4001, kMFR);
  Expect(cap == 96, "after the cooldown pressure steps 120 -> 96");
}

void TestMaintainFrameRateFloorInSim() {
  LadderSim sim;
  // Drive repeated pressure well past every cooldown: MaintainFrameRate must
  // bottom out at 60 and never reach 48.
  int64_t t = 1000;
  for (int i = 0; i < 12; ++i) {
    sim.Tick(true, false, false, true, 30, t, kMFR);
    t += 4000;  // beyond the 3 s step-down cooldown each time
  }
  Expect(sim.cap == 60, "MaintainFrameRate bottoms out at 60, never 48");
}

void TestBalancedReaches48InSim() {
  LadderSim sim;
  int64_t t = 1000;
  for (int i = 0; i < 12; ++i) {
    sim.Tick(true, false, false, true, 30, t, kBAL);
    t += 4000;
  }
  Expect(sim.cap == 48, "Balanced reaches the 48 floor under sustained pressure");
}

void TestZeroHzIdleFreezesLadder() {
  LadderSim sim;
  // Even with sustained pressure, an idle (0 Hz) screen must never step the
  // cap: the encoded-rate measurements are meaningless for capacity.
  int64_t t = 1000;
  for (int i = 0; i < 8; ++i) {
    sim.Tick(true, false, true, true, 30, t, kMFR);
    t += 4000;
  }
  Expect(sim.cap == 144, "0 Hz idle freezes the cap at the configured ceiling");
}

void TestCaptureLimitedGuardInSim() {
  LadderSim sim;
  int64_t t = 1000;
  for (int i = 0; i < 8; ++i) {
    sim.Tick(true, true, false, true, 30, t, kMFR);  // capture_limited=true
    t += 4000;
  }
  Expect(sim.cap == 144, "capture-limited never steps the cap down");
}

void TestStepUpRequiresHoldAndCooldown() {
  LadderSim sim;
  // Step down once under pressure, then go healthy. The climb back up must
  // require BOTH the 4 s healthy hold AND the 8 s step-up cooldown.
  int cap = sim.Tick(true, false, false, true, 100, 1000, kMFR);
  Expect(cap == 120, "pressure steps down to 120 first");

  // Healthy immediately after, but the step-up cooldown (8 s from the last
  // transition at t=1000) dominates the hold (4 s): no step up before t=9000.
  // Encoder comfortably sustaining 120 fps => measured fps high.
  cap = sim.Tick(false, false, false, true, 118, 5000, kMFR);
  Expect(cap == 120, "no step-up at t=+4 s: 8 s cooldown not elapsed");
  cap = sim.Tick(false, false, false, true, 118, 8999, kMFR);
  Expect(cap == 120, "no step-up just before the 8 s cooldown");
  // The healthy hold began at the first healthy tick (t=5000); by t=9001 both
  // the 4 s hold and the 8 s cooldown have elapsed.
  cap = sim.Tick(false, false, false, true, 118, 9001, kMFR);
  Expect(cap == 144, "step-up fires once hold and cooldown both elapse (120->144 directly)");
}

void TestStepUpInterruptedByUnhealthyWindow() {
  LadderSim sim;
  sim.Tick(true, false, false, true, 100, 1000, kMFR);  // down to 120
  // Healthy at t=5000 (hold starts), then a dip at t=7000 resets the hold.
  sim.Tick(false, false, false, true, 118, 5000, kMFR);
  int cap = sim.Tick(false, false, false, true, 40, 7000, kMFR);  // unhealthy
  Expect(cap == 120, "an unhealthy window does not step up");
  // Even after the 8 s cooldown, the hold restart means no immediate step-up.
  cap = sim.Tick(false, false, false, true, 118, 9001, kMFR);
  Expect(cap == 120, "hold restarted by the dip: no step-up yet");
  // Hold restarted at t=9001; 4 s later (and cooldown long past) it climbs.
  cap = sim.Tick(false, false, false, true, 118, 13002, kMFR);
  Expect(cap == 144, "step-up after a fresh uninterrupted healthy hold");
}

}  // namespace

int main() {
  TestLowerRungs();
  TestHigherRungs();
  TestFloors();
  TestStepDownGate();
  TestRelevanceAndStepUp();
  TestEnvParsing();
  TestStepDownCooldownHysteresis();
  TestMaintainFrameRateFloorInSim();
  TestBalancedReaches48InSim();
  TestZeroHzIdleFreezesLadder();
  TestCaptureLimitedGuardInSim();
  TestStepUpRequiresHoldAndCooldown();
  TestStepUpInterruptedByUnhealthyWindow();
  if (g_failures == 0) {
    std::fprintf(stderr, "PASS: all fps-cap ladder scenarios\n");
    return 0;
  }
  std::fprintf(stderr, "FAILED: %d expectation(s)\n", g_failures);
  return 1;
}
