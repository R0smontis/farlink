/*
 * Copyright (c) 2026 Farlink contributors
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */

// Deterministic simulation test for the encoder-throughput-ceiling re-probe
// state machine. A full ICE/DTLS/SRTP transport test cannot throttle the
// hardware encoder deterministically, so this harness drives the real
// production pieces — minirtc::EncoderThroughputCeiling (the ceiling/backoff
// state machine compiled into the transport) and the minirtc::VideoAdaptationPolicy
// gate predicates the controller uses verbatim — through a scripted millisecond
// timeline with a controllable simulated encoder.
//
// The simulation replicates the exact state transitions of
// IceTransportController::MaybeDegradeResolutionOnEncodeTime:
//   * upgrade-probe launch is blocked while the target area reaches the
//     ceiling unless the re-probe backoff has expired; saturation at the lower
//     resolution defers the deadline instead of launching;
//   * a probe that measures too low a frame rate rolls back to its base
//     resolution, re-learns the ceiling and advances the backoff ladder;
//   * a probe that sustains the configured frame rate at or above the ceiling
//     clears the ceiling and resets the ladder;
//   * encode-saturation downgrades learn the ceiling.
// What is asserted is the emergent timing contract, not the implementation:
// after an artificial throttle is lifted the stream returns to full
// resolution within the backoff bound, and a permanently slow encoder is
// re-probed at a bounded rate with each failed probe costing at most about
// one second of degraded frame rate.

#include <cstdint>
#include <cstdio>
#include <vector>

#include "encoder_throughput_ceiling.h"
#include "video_adaptation_policy.h"

namespace {

using minirtc::EncoderThroughputCeiling;
using minirtc::VideoAdaptationPolicy;

int g_failures = 0;

void Expect(bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++g_failures;
  }
}

// Resolution ladder (strict 16:9) mirroring the transport's strict-aspect
// ladder for a 1920x1080 source. Indices ascend with area.
struct Rung {
  int width;
  int height;
};
constexpr Rung kLadder[] = {
    {640, 360}, {960, 540}, {1280, 720}, {1600, 900}, {1920, 1080}};
constexpr int kLadderCount = sizeof(kLadder) / sizeof(kLadder[0]);
constexpr int kTopRung = kLadderCount - 1;

int64_t Area(const Rung& r) {
  return static_cast<int64_t>(r.width) * r.height;
}

// Simulated encoder throughput model. `throttled` emulates the transient or
// permanent overload: while throttled the encoder cannot sustain the
// configured frame rate above `throttle_max_rung`; unthrottled it sustains
// every rung. The configured frame rate is 60 fps, so the upgrade health bar
// is 55 fps (VideoAdaptationPolicy::UpgradeFrameRate(60)).
struct SimEncoder {
  bool throttled = false;
  int throttle_max_rung = 1;  // While throttled, rungs above this collapse.

  // Encoded frame rate the encoder sustains at a rung. A rung within capacity
  // delivers the full 60 fps; an over-capacity rung collapses to 15 fps, far
  // below the 55 fps upgrade bar and the 45 fps downgrade floor.
  int SustainedFps(int rung) const {
    const int capacity = throttled ? throttle_max_rung : kTopRung;
    return rung <= capacity ? 60 : 15;
  }
};

// Probe-machinery constants mirrored from the transport controller. A ceiling
// re-probe uses the fast measurement window of 1000 ms; rollback happens on
// the first encoded frame at or after that window, so a failed re-probe costs
// one second of degraded frame rate.
constexpr int64_t kProbeMeasureMs = 1000;
constexpr int kConfiguredFps = 60;
constexpr bool kBalanced = false;
// Conservative stand-in for the controller's upgrade cooldown plus sustained
// healthy-frame-rate requirement between probe trials.
constexpr int64_t kProbeCycleGapMs = 3000;

// One probe trial. Active while the controller measures the encoder at the
// target rung.
struct Probe {
  bool active = false;
  int base_rung = 0;
  int target_rung = 0;
  int64_t measurement_started_ms = 0;
};

// Faithful discrete-time simulation of the controller's adaptation loop as it
// interacts with the ceiling. The loop is evaluated once per millisecond of
// simulated time, matching the per-encoded-frame cadence closely enough for
// second-scale assertions while keeping the run deterministic and fast.
struct Simulation {
  SimEncoder encoder;
  EncoderThroughputCeiling ceiling;
  Probe probe;
  int current_rung = kTopRung;
  int64_t now_ms = 0;
  // The controller will not launch an upgrade probe within upgrade_cooldown_ms
  // of the last resolution change, and additionally requires a sustained
  // healthy frame rate first. Modelled conservatively as a single per-probe
  // cycle gap of 3 s after every probe completes or rolls back.
  int64_t next_probe_allowed_ms = 0;

  // Observability for assertions.
  int probe_launches = 0;
  int64_t degraded_ms = 0;      // Time spent at a rung the encoder cannot sustain.
  int64_t first_full_recovery_ms = -1;  // First time the top rung is sustained after lift.
  std::vector<int64_t> probe_launch_times;

  int UpgradeBarFps() const {
    return VideoAdaptationPolicy::UpgradeFrameRate(kConfiguredFps, kBalanced);
  }

  // Operating above the encoder's sustainable rung right now.
  bool OverCapacity() const {
    return encoder.SustainedFps(current_rung) < UpgradeBarFps();
  }

  // Is the encoder currently saturated at the rung it is operating on? This
  // mirrors the controller's encoded-frame-rate collapse term of
  // IsEncoderSaturated: queue/backlog terms only appear while operating at an
  // over-capacity rung, which the model captures through the measured frame
  // rate.
  bool EncoderSaturatedAtCurrentRung() const {
    const int measured_fps = encoder.SustainedFps(current_rung);
    return VideoAdaptationPolicy::IsEncoderSaturated(
        false, false, false, false, false,
        /*encoded_frame_rate_ready=*/true, measured_fps, UpgradeBarFps());
  }

  // The next higher rung the controller would trial from `rung`, or -1.
  int NextHigherRung(int rung) const {
    return rung + 1 < kLadderCount ? rung + 1 : -1;
  }

  // Controller probe-failure transition: roll back to base, re-learn the
  // ceiling, and advance the backoff ladder when the trial was at or above
  // the remembered ceiling.
  void FailProbe() {
    const Rung failed = kLadder[probe.target_rung];
    const bool trial_at_or_above_ceiling =
        VideoAdaptationPolicy::IsBlockedByEncoderThroughputCeiling(
            Area(failed), ceiling.Width(), ceiling.Height());
    current_rung = probe.base_rung;
    ceiling.Set(failed.width, failed.height, encoder.SustainedFps(probe.target_rung),
                now_ms);
    if (trial_at_or_above_ceiling) {
      ceiling.BackoffReprobe(now_ms);
    }
    probe.active = false;
    next_probe_allowed_ms = now_ms + kProbeCycleGapMs;
  }

  // Controller probe-success transition: stay at the target rung and clear
  // the ceiling when the trial reached or exceeded it.
  void SucceedProbe() {
    const Rung target = kLadder[probe.target_rung];
    current_rung = probe.target_rung;
    if (VideoAdaptationPolicy::IsBlockedByEncoderThroughputCeiling(
            Area(target), ceiling.Width(), ceiling.Height())) {
      ceiling.Clear();
    }
    probe.active = false;
    next_probe_allowed_ms = now_ms + kProbeCycleGapMs;
    if (current_rung == kTopRung && !encoder.throttled &&
        first_full_recovery_ms < 0) {
      first_full_recovery_ms = now_ms;
    }
  }

  // Encode-saturation downgrade transition: leave the over-capacity rung for
  // the next lower one and learn the ceiling at the size being left.
  void SaturationDowngrade() {
    const Rung leaving = kLadder[current_rung];
    ceiling.Set(leaving.width, leaving.height, encoder.SustainedFps(current_rung),
                now_ms);
    if (current_rung > 0) --current_rung;
  }

  // Advance the simulation by one millisecond.
  void Step() {
    ++now_ms;

    // Track degraded time: operating at a rung the encoder cannot sustain.
    if (encoder.SustainedFps(current_rung) < UpgradeBarFps()) {
      ++degraded_ms;
    }

    if (probe.active) {
      // The probe measurement starts when the encoder applies the new size;
      // the model applies it instantly, so measurement runs from launch.
      const int64_t duration = now_ms - probe.measurement_started_ms;
      if (duration >= kProbeMeasureMs) {
        const int measured_fps = encoder.SustainedFps(probe.target_rung);
        if (measured_fps < UpgradeBarFps()) {
          FailProbe();
        } else {
          SucceedProbe();
        }
      }
      return;
    }

    // While saturated at the current rung the controller downgrades instead
    // of probing. This is how the ceiling is first learned under throttle.
    if (EncoderSaturatedAtCurrentRung()) {
      SaturationDowngrade();
      return;
    }

    // Healthy at the current rung: consider an upgrade probe toward the next
    // higher rung. The controller's upgrade cooldown and sustained-health
    // gate between trials is modelled as kProbeCycleGapMs.
    if (now_ms < next_probe_allowed_ms) return;
    const int next = NextHigherRung(current_rung);
    if (next < 0) return;
    const Rung target = kLadder[next];

    if (VideoAdaptationPolicy::IsBlockedByEncoderThroughputCeiling(
            Area(target), ceiling.Width(), ceiling.Height())) {
      if (!VideoAdaptationPolicy::IsEncoderThroughputReprobeDue(
              now_ms, ceiling.ReprobeMs())) {
        return;  // Backoff has not expired; stay at the current rung.
      }
      if (EncoderSaturatedAtCurrentRung()) {
        ceiling.DeferReprobe(now_ms);
        return;
      }
      // Re-probe launch past the expired backoff.
    }

    // Launch the upgrade probe (ordinary or ceiling re-probe; both use the
    // same machinery, and a ceiling re-probe uses the fast 1000 ms window
    // mirrored by kProbeMeasureMs).
    probe.active = true;
    probe.base_rung = current_rung;
    probe.target_rung = next;
    probe.measurement_started_ms = now_ms;
    ++probe_launches;
    probe_launch_times.push_back(now_ms);
    // Operating at the trial rung begins now, so degraded time accrues from
    // the next step if the trial is over capacity.
    current_rung = next;
  }

  void Run(int64_t duration_ms) {
    for (int64_t i = 0; i < duration_ms; ++i) Step();
  }
};

// Scenario 1 — recovery after a transient throttle. The encoder starts
// throttled above rung 1 (960x540), so operating at full resolution is
// impossible and the ceiling is learned. Partway through, the throttle lifts.
// The stream must return to full resolution within the maximum backoff bound
// of the lift, plus probe measurement slack.
void TestRecoveryAfterThrottleLift() {
  std::fprintf(stderr, "[test] recovery_after_throttle_lift\n");
  Simulation sim;
  sim.encoder.throttled = true;
  // Start at the top rung so the throttle immediately saturates the encoder
  // and the ceiling is learned through the downgrade path, exactly as a
  // thermal spike mid-session would.
  sim.current_rung = kTopRung;

  // Phase 1: throttled for 20 s. The ceiling must be learned and the stream
  // driven down at or below the throttle capacity.
  sim.Run(20000);
  Expect(sim.ceiling.IsSet(), "ceiling should be learned under throttle");
  Expect(sim.current_rung <= sim.encoder.throttle_max_rung + 1,
         "stream should have downgraded out of the saturated rungs");

  // Phase 2: stay throttled for another 150 s so several failed re-probes
  // advance the ladder to its cap. The stream must never stay at full
  // resolution while the throttle holds.
  const int64_t phase2_start = sim.now_ms;
  sim.Run(150000);
  Expect(sim.encoder.throttled, "throttle still engaged in phase 2");
  Expect(sim.current_rung <= sim.encoder.throttle_max_rung + 1,
         "stream must not sit at full resolution while throttled");
  const int64_t probes_phase2 = [&] {
    int64_t n = 0;
    for (int64_t t : sim.probe_launch_times)
      if (t >= phase2_start) ++n;
    return n;
  }();
  // 150 s at a maximum 120 s backoff cannot produce more than a handful of
  // probes; this is a loose upper bound checked tightly in scenario 2.
  Expect(probes_phase2 <= 6, "throttled probe rate should stay bounded");

  // Phase 3: lift the throttle. Recovery to full resolution is bounded by the
  // remaining re-probe backoff (at most one maximum backoff interval) plus the
  // graduated climb: one probe cycle per rung on the way back to the top. The
  // backoff dominates the bound; the climb allowance accounts for the probe
  // windows and inter-probe cooldowns the controller enforces.
  const int64_t lift_ms = sim.now_ms;
  sim.encoder.throttled = false;
  // Worst-case climb: from the lowest rung to the top is kTopRung successful
  // probe cycles, each costing the measurement window plus the cycle gap.
  const int64_t climb_allowance_ms =
      kTopRung * (kProbeMeasureMs + kProbeCycleGapMs);
  const int64_t recovery_bound_ms =
      EncoderThroughputCeiling::kReprobeMaxBackoffMs + climb_allowance_ms;
  sim.Run(recovery_bound_ms + 5000);
  Expect(sim.first_full_recovery_ms >= 0,
         "stream should return to full resolution after the throttle lifts");
  if (sim.first_full_recovery_ms >= 0) {
    const int64_t recovery_time = sim.first_full_recovery_ms - lift_ms;
    std::fprintf(stderr, "  recovery_time_ms=%lld (bound=%lld)\n",
                 static_cast<long long>(recovery_time),
                 static_cast<long long>(recovery_bound_ms));
    Expect(recovery_time <= recovery_bound_ms,
           "recovery must complete within the backoff bound plus the climb cost");
  }
  Expect(!sim.ceiling.IsSet(),
         "successful re-probe should clear the ceiling");
}

// Scenario 2 — a permanently slow encoder. The throttle never lifts, so every
// re-probe fails. Over a long window the probe rate must stay bounded by the
// capped backoff, and each failed probe must cost at most about one second of
// degraded frame rate.
void TestBoundedProbeRateForPermanentEncoder() {
  std::fprintf(stderr, "[test] bounded_probe_rate_for_permanent_encoder\n");
  Simulation sim;
  sim.encoder.throttled = true;
  sim.current_rung = kTopRung;

  // Let the ceiling be learned and the ladder reach its cap: run five minutes.
  const int64_t window_ms = 300000;
  sim.Run(window_ms);

  Expect(sim.ceiling.IsSet(), "ceiling must stay set for a permanent encoder");
  // The ladder cap means probes are at least kReprobeMaxBackoffMs apart once
  // the cap is reached. Count probes in the final two minutes, well past the
  // ladder warm-up (30 + 60 s of sub-cap backoffs).
  const int64_t tail_start = window_ms - 120000;
  int probes_in_tail = 0;
  for (int64_t t : sim.probe_launch_times)
    if (t >= tail_start) ++probes_in_tail;
  std::fprintf(stderr, "  probes_in_final_120s=%d total_probes=%d\n",
               probes_in_tail, sim.probe_launches);
  // At most one probe per 120 s backoff interval, plus one boundary probe.
  Expect(probes_in_tail <= 2,
         "permanent encoder must be re-probed at most about once per cap interval");

  // Probe spacing once the ladder is capped: consecutive probes at the cap
  // must be at least kReprobeMaxBackoffMs apart.
  for (size_t i = 1; i < sim.probe_launch_times.size(); ++i) {
    const int64_t gap =
        sim.probe_launch_times[i] - sim.probe_launch_times[i - 1];
    // Early probes can be closer because the ladder is still warm; only the
    // capped spacing is a hard requirement. Once gaps reach the cap they must
    // never shrink below it again.
    if (sim.probe_launch_times[i - 1] > 150000) {
      Expect(gap >= EncoderThroughputCeiling::kReprobeMaxBackoffMs,
             "capped probes must be spaced by at least the maximum backoff");
    }
  }

  // Degraded-time budget: every failed probe degrades the frame rate for the
  // measurement window (~1 s) before rollback. Additionally the initial
  // learning downgrade costs a brief saturated period. Assert the per-probe
  // cost bound using the probes in the tail window, where each probe is a
  // ceiling re-probe using the fast measurement window.
  const int64_t degraded_per_probe_bound = kProbeMeasureMs + 200;  // rollback slack
  std::fprintf(stderr, "  degraded_ms_total=%lld degraded_per_probe_bound_ms=%lld\n",
               static_cast<long long>(sim.degraded_ms),
               static_cast<long long>(degraded_per_probe_bound));
  // Over the whole run the degraded time cannot exceed the initial learning
  // episode plus one fast-probe window per launch.
  const int64_t initial_learning_budget_ms = 5000;
  Expect(sim.degraded_ms <=
             initial_learning_budget_ms +
                 sim.probe_launches * degraded_per_probe_bound,
         "each failed probe must cost at most about one second of degraded fps");

  // The stream must never remain at full resolution: after every rollback the
  // current rung is within the encoder's throttle capacity or one rung above
  // (the last rung that still probes upward).
  Expect(sim.current_rung < kTopRung,
         "permanent encoder must not end the session at full resolution");
}

// Scenario 3 — probe-cost bound for a single re-probe. Directly verifies that
// a re-probe that fails rolls back within the fast measurement window, so the
// degraded frame rate episode is at most about one second.
void TestSingleReprobeRollbackCost() {
  std::fprintf(stderr, "[test] single_reprobe_rollback_cost\n");
  Simulation sim;
  sim.encoder.throttled = true;
  sim.current_rung = kTopRung;

  // Learn the ceiling and settle at the highest sustainable rung.
  sim.Run(30000);
  Expect(sim.ceiling.IsSet(), "ceiling learned");
  Expect(!sim.probe.active, "no probe active after settling");

  // Measure the length of the very next degraded episode caused by a re-probe.
  // Advance until a re-probe launches, then count how long the stream spends
  // at the over-capacity trial rung.
  int64_t launch_ms = -1;
  const int64_t search_end = sim.now_ms + 200000;
  while (sim.now_ms < search_end) {
    const int before = sim.probe_launches;
    sim.Step();
    if (sim.probe_launches > before) {
      launch_ms = sim.now_ms;
      break;
    }
  }
  Expect(launch_ms >= 0, "a re-probe should launch once the backoff expires");
  if (launch_ms < 0) return;

  // Count consecutive milliseconds spent at the over-capacity trial rung.
  int64_t episode_ms = 0;
  while (sim.probe.active || sim.OverCapacity()) {
    sim.Step();
    ++episode_ms;
    if (episode_ms > 10000) break;  // safety
  }
  std::fprintf(stderr, "  reprobe_degraded_episode_ms=%lld\n",
               static_cast<long long>(episode_ms));
  Expect(episode_ms <= kProbeMeasureMs + 200,
         "a failed re-probe must roll back within about one second");
}


// Scenario 6 — saturation deferral primitive. While the encoder is saturated
// the re-probe deadline is pushed out by one backoff step without advancing
// the ladder, so a probe never launches into known overload yet recovery
// stays bounded by one backoff interval after the pressure clears.
void TestSaturationDeferral() {
  std::fprintf(stderr, "[test] saturation_deferral\n");
  EncoderThroughputCeiling ceiling;
  ceiling.Set(1280, 720, 15, 0);
  Expect(ceiling.ReprobeMs() == 30000, "initial deadline at +30 s");
  // Saturated at t=29 s: the deadline is pushed a full step beyond now.
  ceiling.DeferReprobe(29000);
  Expect(ceiling.ReprobeMs() == 29000 + 30000,
         "deferral pushes the deadline one step beyond now");
  Expect(ceiling.ReprobeStep() == 0,
         "deferral must not advance the ladder: the probe never ran");
  // Repeated saturation keeps pushing the deadline out; it never fires into
  // the overload.
  ceiling.DeferReprobe(58000);
  Expect(ceiling.ReprobeMs() == 58000 + 30000,
         "continued saturation keeps deferring");
  // Once saturation clears at t=88 s the deadline is at most one backoff
  // interval away, so recovery is still bounded.
  Expect(ceiling.ReprobeMs() - 58000 <=
             EncoderThroughputCeiling::kReprobeMaxBackoffMs,
         "recovery after saturation stays within the backoff bound");
  // A failed probe advances the ladder; deferral then uses the larger step.
  ceiling.BackoffReprobe(88000);
  ceiling.BackoffReprobe(88001);
  Expect(ceiling.ReprobeStep() == 2, "ladder at cap after two failures");
  ceiling.DeferReprobe(100000);
  Expect(ceiling.ReprobeMs() == 100000 + 120000,
         "deferral at the cap uses the capped interval");
}

// Scenario 4 — reset on stream reconfiguration. A ceiling learned under
// throttle must clear (with its backoff ladder) when the stream is
// reconfigured, because a new encoder instance re-opens the throughput
// question.
void TestResetOnReconfiguration() {
  std::fprintf(stderr, "[test] reset_on_reconfiguration\n");
  EncoderThroughputCeiling ceiling;
  ceiling.Set(1920, 1080, 15, 1000);
  Expect(ceiling.IsSet(), "ceiling set");
  ceiling.BackoffReprobe(2000);
  ceiling.BackoffReprobe(3000);
  Expect(ceiling.ReprobeStep() == 2, "ladder advanced to cap");
  // Simulate the controller's reconfiguration reset.
  ceiling.Clear();
  Expect(!ceiling.IsSet(), "reconfiguration clears the ceiling");
  Expect(ceiling.ReprobeMs() == 0 && ceiling.ReprobeStep() == 0,
         "reconfiguration resets the backoff ladder");
}

// Scenario 5 — backoff ladder arithmetic. Directly verifies the 30/60/120 s
// capped exponential schedule the transport relies on.
void TestBackoffLadderSchedule() {
  std::fprintf(stderr, "[test] backoff_ladder_schedule\n");
  EncoderThroughputCeiling ceiling;
  ceiling.Set(1920, 1080, 15, 0);
  Expect(ceiling.ReprobeMs() == 30000, "initial re-probe at +30 s");
  Expect(ceiling.BackoffReprobe(30000) == 60000, "first failure backs off 60 s");
  Expect(ceiling.ReprobeMs() == 30000 + 60000, "deadline accumulates");
  Expect(ceiling.BackoffReprobe(90000) == 120000, "second failure backs off 120 s");
  Expect(ceiling.BackoffReprobe(210000) == 120000, "ladder capped at 120 s");
  Expect(EncoderThroughputCeiling::kReprobeMaxBackoffMs == 120000,
         "cap constant matches the 120 s ladder top");
}

// Item-1 unit tests for the pure probe-verdict policy in
// minirtc::VideoAdaptationPolicy. These pin the round-7 root-cause fix: the
// probe measurement must recognise the size the target actually reaches after
// the transport's even-rounding fit (not the raw ladder rung), and a
// conclusive frame-rate shortfall must fail early while a marginal probe runs
// its full window.
void TestProbeVerdictPolicy() {
  std::fprintf(stderr, "[test] probe_verdict_policy\n");
  using P = minirtc::VideoAdaptationPolicy;

  // Root-cause fixture: a 3024x1964 source probing the ladder rung 2242x1456
  // actually encodes at 2240x1456 after FitVideoResolution re-rounds. The
  // measurement must match the fitted size, not the rung.
  const auto fitted = P::ProbeEncodedSize(3024, 1964, 2242, 1456);
  Expect(fitted.first == 2240 && fitted.second == 1456,
         "ProbeEncodedSize returns the even-fit size, not the raw rung");
  Expect(P::IsProbeEncodedFrame(3024, 1964, 2242, 1456, 2240, 1456),
         "encoded frame at the fitted size must be recognised as the probe");
  Expect(!P::IsProbeEncodedFrame(3024, 1964, 2242, 1456, 2242, 1456),
         "the raw rung size never appears on the wire, so it must not match");
  // A frame at the base (pre-upgrade) size is not probe evidence either.
  Expect(!P::IsProbeEncodedFrame(3024, 1964, 2242, 1456, 1972, 1282),
         "frames at the base resolution must not count as probe measurement");
  // Native probe: fitting the source size is the identity, so native matches.
  Expect(P::IsProbeEncodedFrame(3024, 1964, 3024, 1964, 3024, 1964),
         "a native probe target matches the native encoded size");

  // Frame-rate arithmetic rounds half up and is zero before measurement.
  Expect(P::ProbeMeasuredFrameRate(0, 0) == 0, "no measurement yet");
  Expect(P::ProbeMeasuredFrameRate(77, 1000) == 77, "sustained 77 fps");
  Expect(P::ProbeMeasuredFrameRate(38, 500) == 76, "38 frames in 500 ms ~ 76 fps");

  // Conclusive shortfall -> early fail. The configured rate is 120 fps so the
  // upgrade bar is 110 fps (120*11/12); a sustained 76 fps is far below.
  const int bar = P::UpgradeFrameRate(120, false);
  Expect(bar == 110, "120 fps upgrade bar is 110");
  Expect(P::IsProbeFrameRateConclusivelyLow(500, false, 76, bar),
         "76 fps sustained across the 500 ms failure window is conclusive");
  Expect(P::IsProbeFrameRateConclusivelyLow(1000, false, 76, bar),
         "still conclusive beyond the window");

  // Reconfigure/keyframe blip -> no fail. The measurement clock only starts
  // after the first key frame, so before the failure window elapses there is
  // no conclusive evidence even if the rate currently reads low.
  Expect(!P::IsProbeFrameRateConclusivelyLow(499, false, 76, bar),
         "before the failure window a low reading is not yet conclusive");
  Expect(!P::IsProbeFrameRateConclusivelyLow(100, false, 30, bar),
         "an early post-keyframe dip must not be declared a failure");

  // Marginal probe -> full window. A rate at or above the bar never fails, so
  // the probe runs its full measurement before acceptance.
  Expect(!P::IsProbeFrameRateConclusivelyLow(500, false, 110, bar),
         "at the bar is not a failure");
  Expect(!P::IsProbeFrameRateConclusivelyLow(500, false, 120, bar),
         "sustained full rate is not a failure");

  // Sparse capture (balanced mode) must not be attributed to the encoder.
  Expect(!P::IsProbeFrameRateConclusivelyLow(500, true, 76, bar),
         "insufficient capture input is not encoder evidence");
}


}  // namespace

int main() {
  TestProbeVerdictPolicy();
  TestBackoffLadderSchedule();
  TestResetOnReconfiguration();
  TestSaturationDeferral();
  TestSingleReprobeRollbackCost();
  TestRecoveryAfterThrottleLift();
  TestBoundedProbeRateForPermanentEncoder();
  if (g_failures == 0) {
    std::fprintf(stderr, "PASS: all encoder-ceiling reprobe scenarios\n");
    return 0;
  }
  std::fprintf(stderr, "FAILED: %d expectation(s)\n", g_failures);
  return 1;
}
