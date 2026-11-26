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
#include "resolution_adapter.h"
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
    const Rung base = kLadder[probe.base_rung];
    const bool trial_at_or_above_ceiling =
        VideoAdaptationPolicy::IsBlockedByEncoderThroughputCeiling(
            Area(failed), ceiling.BaseArea());
    current_rung = probe.base_rung;
    ceiling.Set(failed.width, failed.height, base.width, base.height,
                encoder.SustainedFps(probe.target_rung), now_ms);
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
            Area(target), ceiling.BaseArea())) {
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
    // The downgrade target is the next lower rung, the largest size still
    // considered safe; at the floor there is no lower rung, so the boundary
    // falls back to the size being left itself.
    const Rung base = kLadder[current_rung > 0 ? current_rung - 1 : 0];
    const bool has_lower = current_rung > 0;
    ceiling.Set(leaving.width, leaving.height, has_lower ? base.width : 0,
                has_lower ? base.height : 0, encoder.SustainedFps(current_rung),
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
            Area(target), ceiling.BaseArea())) {
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
  ceiling.Set(1280, 720, 0, 0, 15, 0);
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
  ceiling.Set(1920, 1080, 0, 0, 15, 1000);
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
  ceiling.Set(1920, 1080, 0, 0, 15, 0);
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

// Scenario 7 — round-6 exact regression: the ceiling gate must bucket the
// even-rounding variants of a failed rung. Before the round-6 fix the
// comparison was strict (target_area >= ceiling area). The 1458x946 failure
// re-probed at the rounding-slip sizes 1456x946 and 1456x944 — both a few
// pixels below the remembered size — slipped past the strict gate, bypassed
// the backoff state machine entirely, and re-fired on the short
// ordinary-upgrade cadence, which produced the observed 29/29/29 s undamped
// oscillation between the sustainable rung and the unreachable one. The
// round-6 fix used a 7/8-of-ceiling-area ratio, but a fixed ratio cannot in
// general separate the epsilon rounding variants from genuinely smaller
// adjacent rungs where the ladder step compresses at the floor or a quality
// cap (see ceiling_gate_ladder_properties). The gate now blocks any target
// strictly above the largest-safe base area — the rung below the failed
// size — which must catch both variants while leaving the genuinely lower
// ladder rungs free.
void TestRungGranularGateRegression() {
  std::fprintf(stderr, "[test] rung_granular_gate_regression\n");
  using P = minirtc::VideoAdaptationPolicy;

  // Exact gate bucketing at the round-6 ceiling 1458x946, whose largest-safe
  // base is the rung below at 1284x832.
  constexpr int64_t kBaseArea = static_cast<int64_t>(1284) * 832;
  Expect(P::IsBlockedByEncoderThroughputCeiling(
             static_cast<int64_t>(1456) * 946, kBaseArea),
         "rounding variant 1456x946 must be treated as the failed rung");
  Expect(P::IsBlockedByEncoderThroughputCeiling(
             static_cast<int64_t>(1456) * 944, kBaseArea),
         "rounding variant 1456x944 must be treated as the failed rung");
  Expect(!P::IsBlockedByEncoderThroughputCeiling(
             static_cast<int64_t>(1284) * 832, kBaseArea),
         "the base rung itself must stay unblocked");
  Expect(!P::IsBlockedByEncoderThroughputCeiling(
             static_cast<int64_t>(1130) * 732, kBaseArea),
         "two full rungs below must stay unblocked");

  // A strict area comparison against the failed size is exactly what let the
  // slip through: both variants sit below the remembered failed area
  // 1458x946, so a strict-area gate cannot catch them.
  Expect(static_cast<int64_t>(1456) * 946 <
             static_cast<int64_t>(1458) * 946 &&
         static_cast<int64_t>(1456) * 944 <
             static_cast<int64_t>(1458) * 946,
         "the regression variants are strictly smaller than the failed size, "
         "so a strict-area gate cannot catch them");

  // Backoff escalation under rounding variants, driving the real state
  // machine through the controller's probe-failure transition. The first
  // failure at 1458x946 learns the ceiling with the base 30 s re-probe arm;
  // every consecutive re-probe arrives as a rounding variant of the same
  // rung, must still be recognised as at-or-above the ceiling, and must
  // advance the exponential ladder 30 -> 60 -> 120 -> 120 s. Before the fix
  // the variants slipped the gate, BackoffReprobe was never reached, and the
  // re-probes kept firing on the undamped ~30 s cadence.
  EncoderThroughputCeiling ceiling;
  constexpr int64_t kT0 = 100000;

  // First probe fails at the full rung size, rolling back to the rung below
  // at 1284x832: fresh evidence arms the ladder at the base interval. No
  // backoff yet — this is the learning failure, not a re-probe of a known
  // ceiling.
  ceiling.Set(1458, 946, 1284, 832, 15, kT0);
  Expect(ceiling.ReprobeMs() - kT0 == EncoderThroughputCeiling::kReprobeBaseMs,
         "fresh ceiling evidence arms the first re-probe at +30 s");
  Expect(ceiling.ReprobeStep() == 0, "ladder starts at step 0");

  struct Variant {
    int width;
    int height;
  };
  // Consecutive failed re-probes arriving as even-rounding variants of the
  // same rung: the controller re-computes the target from the ladder and
  // FitVideoResolution re-rounds it, so the encoded failure size jitters by
  // a pixel pair around the remembered ceiling.
  constexpr Variant kVariants[] = {
      {1456, 946}, {1456, 944}, {1458, 944}, {1456, 946}};
  constexpr int64_t kExpectedIntervals[] = {60000, 120000, 120000, 120000};
  int64_t now = kT0;
  for (int i = 0; i < 4; ++i) {
    // The re-probe launches when the deadline expires and fails at the
    // rounding-variant size.
    now = ceiling.ReprobeMs();
    const int64_t failed_area =
        static_cast<int64_t>(kVariants[i].width) * kVariants[i].height;
    const bool trial_at_or_above_ceiling =
        P::IsBlockedByEncoderThroughputCeiling(failed_area,
                                               ceiling.BaseArea());
    Expect(trial_at_or_above_ceiling,
           "rounding-variant re-probe must be treated as at-or-above the "
           "ceiling so the backoff ladder advances");
    if (!trial_at_or_above_ceiling) break;

    // Controller failure transition: re-learn the ceiling at the size that
    // actually failed (rolling back to the same rung below at 1284x832), then
    // advance the backoff because the trial was at or above the remembered
    // ceiling.
    ceiling.Set(kVariants[i].width, kVariants[i].height, 1284, 832, 15, now);
    ceiling.BackoffReprobe(now);

    const int64_t interval = ceiling.ReprobeMs() - now;
    std::fprintf(stderr,
                 "  reprobe %d: failed=%dx%d next_interval_ms=%lld\n", i + 1,
                 kVariants[i].width, kVariants[i].height,
                 static_cast<long long>(interval));
    Expect(interval == kExpectedIntervals[i],
           i == 0
               ? "first failed re-probe escalates the interval to 60 s"
               : (i == 1
                      ? "second failed re-probe escalates the interval to 120 s"
                      : "the ladder stays capped at 120 s"));
  }
  Expect(ceiling.ReprobeStep() == EncoderThroughputCeiling::kReprobeMaxStep,
         "the ladder reaches its cap after consecutive variant failures");
  Expect(EncoderThroughputCeiling::kReprobeMaxBackoffMs == 120000,
         "the cap matches the 120 s ladder top");

  // Regression witness: had the variants slipped the gate (strict-area
  // comparison), none of the BackoffReprobe calls above would have been
  // reached and every interval would have stayed at the 30 s base arm —
  // the 29/29/29 s oscillation. Assert the escalation actually happened:
  // the final interval is the cap, four times the base.
  Expect(ceiling.ReprobeMs() - now == 4 * EncoderThroughputCeiling::kReprobeBaseMs,
         "escalation reached 4x the base interval; a slipped gate would "
         "have left every interval at 30 s");
}
// Scenario 8 — ladder property test: for the real minirtc::ResolutionAdapter
// ladder, the ceiling gate must exactly separate adjacent rungs. The gate
// blocks any target strictly above the ceiling's base area, where the base
// is the rung directly below the failed size — the size the stream rolled
// back to or downgraded to. For every ceiling rung R reachable by the
// downward ladder walk, with L the rung directly below it (the base the
// controller passes when it learns the ceiling at R), the mechanism depends
// on three invariants:
//   1. L itself must NOT be blocked when the ceiling base is L — the
//      transport sits at L after the rollback and the gate must not bucket
//      it into the failed rung;
//   2. stepping back up from L must reach a size U that IS blocked —
//      otherwise the re-probe of the failed rung escapes the backoff
//      machinery and the undamped oscillation returns. GetNextHigherResolution
//      is bounded below at current_area + 1, so U is always strictly above L
//      and this holds by construction; asserting it guards the ladder
//      against a regression to a non-monotonic step;
//   3. the failed rung R and its even-rounding variants (a couple of pixels
//      narrower in one dimension) must all remain strictly above L — this is
//      what buckets the rounding variants into the failed rung. Where the
//      ladder step compresses (the clamped floor, a quality cap) R and L are
//      close, so this is the invariant most at risk and the one the round-6
//      fixed-ratio gate could not satisfy.
// The test is deliberately strict: every compressed step that breaks an
// invariant is counted as a violation and reported with exact geometry,
// because the floor and cap clamping classes are where the step compresses.
// A missing upward step ({-1,-1}) is only acceptable inside the cap shadow
// (verified per case); anything else means the ladder itself is broken.
// Up to 20 violations print with full detail. If the gate semantics or the
// ladder geometry ever change, the exact failing numbers are the decision
// input.
void TestCeilingGateLadderProperties() {
  std::fprintf(stderr, "[test] ceiling_gate_ladder_properties\n");
  using P = minirtc::VideoAdaptationPolicy;

  struct SourceSize {
    int width;
    int height;
  };
  constexpr SourceSize kSources[] = {{3024, 1964}, {2560, 1440}, {3840, 2160},
                                     {1920, 1080}, {1366, 768}, {3440, 1440},
                                     {1080, 1920}};
  struct QualityCase {
    MiniRtcVideoQuality quality;
    const char* name;
    int max_pixels;
  };
  // High is the uncapped-in-practice default (4K budget, above every test
  // source); Medium and Low clamp the ladder at 1080p/720p.
  constexpr QualityCase kQualities[] = {
      {MiniRtcVideoQualityHigh, "high", 3840 * 2160},
      {MiniRtcVideoQualityMedium, "medium", 1920 * 1080},
      {MiniRtcVideoQualityLow, "low", 1280 * 720}};
  // The ladder floor area: the smallest bitrate-limit rung is 320x180.
  constexpr int64_t kFloorArea = 320 * 180;

  int violations = 0;
  int floor_band_cases = 0;
  int cap_clamped_cases = 0;
  int pairs_checked = 0;
  for (const SourceSize& src : kSources) {
    for (const QualityCase& qual : kQualities) {
      minirtc::ResolutionAdapter adapter(qual.quality, kConfiguredFps);
      // The quality-cap window for this source: GetResolution and
      // GetNextHigherResolution both clamp their target at min(top
      // bitrate-limit tier area, source area). Used diagnostically to tag
      // cap-truncated steps in the violation output.
      const auto bitrate_limits = adapter.GetBitrateLimits();
      Expect(!bitrate_limits.empty(),
             "the adapter must expose bitrate-limit tiers for every quality");
      const int64_t cap_max_area = static_cast<int64_t>(std::min<int64_t>(
          static_cast<int64_t>(bitrate_limits.back().width) *
              bitrate_limits.back().height,
          static_cast<int64_t>(src.width) * src.height));

      // Walk the ladder downward from native, collecting every rung.
      std::vector<std::pair<int, int>> rungs;
      rungs.push_back({src.width, src.height});
      int cur_w = src.width;
      int cur_h = src.height;
      for (int guard = 0; guard < 64; ++guard) {
        const auto next =
            adapter.GetNextLowerResolution(cur_w, cur_h, src.width, src.height);
        if (next.first <= 0) break;
        rungs.push_back(next);
        cur_w = next.first;
        cur_h = next.second;
      }

      for (size_t i = 0; i + 1 < rungs.size(); ++i) {
        const auto R = rungs[i];      // ceiling rung (the failed size)
        const auto L = rungs[i + 1];  // the rung directly below: the ceiling base
        const int64_t r_area = static_cast<int64_t>(R.first) * R.second;
        const int64_t l_area = static_cast<int64_t>(L.first) * L.second;
        const auto U =
            adapter.GetNextHigherResolution(L.first, L.second, src.width,
                                            src.height);
        ++pairs_checked;

        // Invariant 1: the rung directly below the ceiling — the base itself
        // — must stay free, or the transport could not sit where the rollback
        // left it.
        if (P::IsBlockedByEncoderThroughputCeiling(l_area, l_area)) {
          ++violations;
          if (violations <= 20) {
            std::fprintf(stderr,
                         "  VIOLATION-base-blocked: src=%dx%d q=%s R=%dx%d "
                         "L=%dx%d(%lld) — the rollback base must stay free\n",
                         src.width, src.height, qual.name, R.first, R.second,
                         L.first, L.second, static_cast<long long>(l_area));
          }
        }

        // Invariant 3: the failed rung R and its even-rounding variants must
        // all be blocked (strictly above the base L). Where the ladder step
        // compresses — the clamped floor or a quality cap — R and L are
        // close, so a wide rounding variant of R can collapse onto or below
        // L and escape the bucket. The variants probe up to two even pixels
        // narrower in either dimension, the jitter FitVideoResolution
        // actually produces.
        const int variant_widths[] = {R.first, R.first - 2};
        const int variant_heights[] = {R.second, R.second - 2};
        for (int vw : variant_widths) {
          for (int vh : variant_heights) {
            if (vw <= 0 || vh <= 0) continue;
            const int64_t variant_area = static_cast<int64_t>(vw) * vh;
            if (!P::IsBlockedByEncoderThroughputCeiling(variant_area, l_area)) {
              const char* cause =
                  r_area <= 2 * kFloorArea     ? "floor-clamped-step"
                  : r_area > cap_max_area       ? "cap-truncated-step"
                                                : "open-ladder";
              ++violations;
              if (r_area <= 2 * kFloorArea) ++floor_band_cases;
              if (violations <= 20) {
                std::fprintf(stderr,
                             "  VIOLATION-variant-not-blocked[%s]: src=%dx%d "
                             "q=%s R=%dx%d(%lld) L=%dx%d(%lld) variant=%dx%d(%lld) "
                             "gap_R_minus_L=%lld cap_max_area=%lld\n",
                             cause, src.width, src.height, qual.name, R.first,
                             R.second, static_cast<long long>(r_area), L.first,
                             L.second, static_cast<long long>(l_area), vw, vh,
                             static_cast<long long>(variant_area),
                             static_cast<long long>(r_area - l_area),
                             static_cast<long long>(cap_max_area));
              }
            }
          }
        }

        // Invariant 2: stepping back up from L must be blocked, or the
        // re-probe of the failed rung escapes the backoff machinery.
        if (U.first <= 0) {
          // returns {-1,-1} inside the cap shadow: either L is already at
          // or above the cap window, or L sits close enough below it that
          // the geometric upscale target (l_area / 0.88^2) overshoots the
          // cap and the clamped fit collapses onto the cap boundary,
          // leaving no size strictly above L.
          ++cap_clamped_cases;
          const int64_t ideal_upscale_area = static_cast<int64_t>(
              std::llround(static_cast<double>(l_area) / (0.88 * 0.88)));
          Expect(l_area >= cap_max_area || ideal_upscale_area >= cap_max_area,
                 "a missing upward step must be the quality-cap clamp "
                 "shadow, not a ladder break in the open ladder");
          continue;
        }
        const int64_t u_area = static_cast<int64_t>(U.first) * U.second;
        if (!P::IsBlockedByEncoderThroughputCeiling(u_area, l_area)) {
          // GetNextHigherResolution is bounded below at current_area + 1, so
          // U is always strictly above L and this should be impossible. A
          // failure here means the ladder step is no longer monotonic — a
          // genuine ladder break, not a threshold problem.
          ++violations;
          if (violations <= 20) {
            std::fprintf(stderr,
                         "  VIOLATION-stepup-not-blocked[ladder-non-monotonic]: "
                         "src=%dx%d q=%s R=%dx%d(%lld) L=%dx%d(%lld) U=%dx%d(%lld) "
                         "cap_max_area=%lld\n",
                         src.width, src.height, qual.name, R.first, R.second,
                         static_cast<long long>(r_area), L.first, L.second,
                         static_cast<long long>(l_area), U.first, U.second,
                         static_cast<long long>(u_area),
                         static_cast<long long>(cap_max_area));
          }
        }
      }
    }
  }

  std::fprintf(stderr,
               "  pairs_checked=%d violations=%d floor_band_cases=%d "
               "cap_clamped_cases=%d\n",
               pairs_checked, violations, floor_band_cases,
               cap_clamped_cases);
  Expect(pairs_checked > 100,
         "the ladder walk must cover a meaningful number of rung pairs");
  Expect(violations == 0,
         "every non-degenerate rung pair must satisfy both gate properties");
}


}  // namespace

int main() {
  TestProbeVerdictPolicy();
  TestRungGranularGateRegression();
  TestCeilingGateLadderProperties();
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
