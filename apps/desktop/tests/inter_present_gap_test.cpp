/*
 * @Author: Farlink contributors
 * @Date: 2026-11-26
 * Copyright (c) 2026 by Farlink contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

// Deterministic unit test for the session-cumulative inter-presentation gap
// metric (D8). Every timestamp is a supplied fake monotonic point — no real
// clock, sleep, GPU or network is involved — so the assertions are exact. The
// test drives the production VideoLatencyStats::Record path with hand-built
// frames rather than reimplementing the accumulator, so it exercises the real
// wiring: frame-id dedup, out-of-order endpoint guarding, reset behaviour and
// the strict freeze threshold. It does not duplicate the render pipeline; it
// only feeds the recorder controlled inputs.

#include "rendering/video_latency.h"

#include <cmath>
#include <iostream>
#include <memory>

namespace {

using crossdesk::VideoLatencyFrame;
using crossdesk::VideoLatencyStats;
using Clock = VideoLatencyStats::Clock;

int failures = 0;

void Expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

void ExpectNear(double actual, double expected, double tolerance,
                const char* message) {
  if (std::fabs(actual - expected) > tolerance) {
    std::cerr << "FAIL: " << message << " (actual=" << actual
              << " expected=" << expected << ")\n";
    ++failures;
  }
}

// A monotonic time point `ms` milliseconds after an arbitrary fixed epoch.
// Using a fixed epoch keeps every interval fully deterministic.
Clock::time_point At(double ms) {
  return Clock::time_point(
      std::chrono::duration_cast<Clock::duration>(
          std::chrono::duration<double, std::milli>(ms)));
}

// Build a minimal fresh-presentation frame with the given id. Only the id and
// stats pointer matter to the inter-present path; capture timing is irrelevant
// here because Record's inter-present block runs before the capture-validity
// early-return, and the test never reads the latency samples.
VideoLatencyFrame Frame(std::shared_ptr<VideoLatencyStats> stats, uint64_t id) {
  VideoLatencyFrame frame;
  frame.stats = std::move(stats);
  frame.id = id;
  frame.capture_valid = false;  // keep the latency-sample path out of the test
  return frame;
}

// Feed one presentation of `id` at `ms` through the real Record path.
void Present(std::shared_ptr<VideoLatencyStats> stats, uint64_t id, double ms) {
  stats->Record(Frame(stats, id), At(ms), false);
}

}  // namespace

int main() {
  // ---- exactly-200ms is not a freeze; strictly-above is -------------------
  {
    auto stats = std::make_shared<VideoLatencyStats>();
    Present(stats, 1, 0.0);
    Present(stats, 2, 200.0);  // exactly the threshold: NOT a freeze
    Present(stats, 3, 400.5);  // 200.5ms: strictly above -> freeze
    const auto gap = stats->TakeInterPresentGapStats();
    Expect(gap.intervals == 2, "two presentations -> two intervals");
    Expect(gap.gap_count == 1,
           "exactly-200ms is not a freeze; 200.5ms is");
    ExpectNear(gap.gap_max_ms, 200.5, 1e-6, "freeze max is the 200.5ms gap");
    ExpectNear(gap.interval_max_ms, 200.5, 1e-6, "interval max");
    // mean of 200.0 and 200.5
    ExpectNear(gap.MeanIntervalMs(), 200.25, 1e-6, "mean interval");
    ExpectNear(gap.MeanFreezeMs(), 200.5, 1e-6, "mean freeze");
  }

  // ---- sub-millisecond gaps count as intervals but never freezes ----------
  {
    auto stats = std::make_shared<VideoLatencyStats>();
    Present(stats, 1, 0.0);
    Present(stats, 2, 0.5);   // 0.5ms
    Present(stats, 3, 1.0);   // 0.5ms
    const auto gap = stats->TakeInterPresentGapStats();
    Expect(gap.intervals == 2, "sub-ms gaps still count as intervals");
    Expect(gap.gap_count == 0, "sub-ms gaps are never freezes");
    ExpectNear(gap.interval_max_ms, 0.5, 1e-6, "sub-ms max recorded as-is");
    ExpectNear(gap.MeanIntervalMs(), 0.5, 1e-6, "sub-ms mean");
  }

  // ---- fewer than two frames produces zero intervals, distinct from zero ---
  {
    auto stats = std::make_shared<VideoLatencyStats>();
    const auto empty = stats->TakeInterPresentGapStats();
    Expect(empty.intervals == 0, "no presentations -> zero intervals");
    Expect(empty.ApproxP95IntervalMs() == 0, "empty p95 is 0");
    Present(stats, 1, 100.0);
    const auto single = stats->TakeInterPresentGapStats();
    Expect(single.intervals == 0,
           "a single presentation yields no interval, not a zero-length one");
    Expect(single.gap_count == 0, "single presentation: no freeze");
    // The window collapses to a single instant, so no rate is meaningful.
    const auto window = stats->TakeInterPresentWindow();
    Expect(window.first.has_value() && window.last.has_value(),
           "single presentation still records both endpoints");
    Expect(*window.first == *window.last,
           "single presentation: first == last endpoint");
  }

  // ---- redraw / duplicate delivery does not count twice -------------------
  {
    auto stats = std::make_shared<VideoLatencyStats>();
    Present(stats, 1, 0.0);
    Present(stats, 2, 100.0);
    Present(stats, 2, 100.0);  // duplicate of frame 2: same id, must be ignored
    Present(stats, 1, 150.0);  // out-of-order older id: must be ignored
    const auto gap = stats->TakeInterPresentGapStats();
    Expect(gap.intervals == 1,
           "duplicate and out-of-order frames add no interval");
    ExpectNear(gap.interval_max_ms, 100.0, 1e-6, "only the real interval");
  }

  // ---- p95 and max against known bucket edges ------------------------------
  {
    auto stats = std::make_shared<VideoLatencyStats>();
    // 19 normal ~16ms intervals then 1 large 300ms freeze. The p95 of 20
    // intervals is the 19th-ranked value; with 19 small + 1 large the large
    // one is the top 5%, so p95 sits in the <=17ms bucket (upper edge 17).
    Present(stats, 1, 0.0);
    double t = 0.0;
    for (int i = 0; i < 19; ++i) {
      t += 16.0;
      Present(stats, static_cast<uint64_t>(i + 2), t);
    }
    t += 300.0;
    Present(stats, 21, t);  // one 300ms freeze
    const auto gap = stats->TakeInterPresentGapStats();
    Expect(gap.intervals == 20, "twenty intervals fed");
    Expect(gap.gap_count == 1, "exactly one freeze");
    ExpectNear(gap.interval_max_ms, 300.0, 1e-6, "max is the freeze (exact)");
    // p95 upper bound is the upper edge of the (16,17] bucket = 17ms, since
    // 95% of 20 = 19 and the first 19 intervals all land in that bucket.
    ExpectNear(gap.ApproxP95IntervalMs(), 17.0, 1e-6,
               "p95 is the 17ms bucket upper edge, never an under-estimate");
  }

  // ---- reset clears state and restarts the measurement --------------------
  {
    auto stats = std::make_shared<VideoLatencyStats>();
    Present(stats, 1, 0.0);
    Present(stats, 2, 100.0);
    Expect(stats->TakeInterPresentGapStats().intervals == 1,
           "one interval before reset");
    stats->Reset(At(100.0));
    const auto cleared = stats->TakeInterPresentGapStats();
    Expect(cleared.intervals == 0, "reset clears intervals");
    const auto window = stats->TakeInterPresentWindow();
    Expect(!window.first.has_value() && !window.last.has_value(),
           "reset clears both endpoints");
    // After reset the next two presentations form a fresh interval measured
    // from the first post-reset endpoint, not a fabricated gap across reset.
    Present(stats, 3, 200.0);
    Present(stats, 4, 250.0);
    const auto restarted = stats->TakeInterPresentGapStats();
    Expect(restarted.intervals == 1, "post-reset measurement restarts");
    ExpectNear(restarted.interval_max_ms, 50.0, 1e-6,
               "post-reset interval is 50ms, not a span across the reset");
  }

  // ---- out-of-order callback time does not move the endpoint backwards ----
  {
    auto stats = std::make_shared<VideoLatencyStats>();
    // Frame ids increase but the supplied callback time for frame 3 goes
    // backwards relative to frame 2. ids must be strictly increasing for the
    // presentation counter, so we model clock regression on a forward id.
    Present(stats, 1, 0.0);
    Present(stats, 2, 100.0);
    // Frame 3 carries an earlier timestamp than frame 2: a negative interval
    // is not real and must be skipped; the endpoint must stay at 100ms.
    stats->Record(Frame(stats, 3), At(50.0), false);
    const auto gap = stats->TakeInterPresentGapStats();
    Expect(gap.intervals == 1, "backward-moving callback adds no interval");
    const auto window = stats->TakeInterPresentWindow();
    Expect(window.last.has_value() && *window.last == At(100.0),
           "endpoint does not move backwards");
  }

  // ---- freezes-per-minute window endpoints bracket exactly the intervals --
  {
    auto stats = std::make_shared<VideoLatencyStats>();
    // Normal sub-threshold interval (100ms) then one large freeze (2900ms);
    // the measured window is the first-to-last span of exactly 3000ms.
    Present(stats, 1, 0.0);
    Present(stats, 2, 100.0);    // 100ms interval: not a freeze
    Present(stats, 3, 3000.0);   // 2900ms interval: the single freeze
    const auto gap = stats->TakeInterPresentGapStats();
    Expect(gap.intervals == 2, "two intervals fed");
    Expect(gap.gap_count == 1, "only the 2900ms interval is a freeze");
    const auto window = stats->TakeInterPresentWindow();
    Expect(window.first.has_value() && *window.first == At(0.0),
           "window first endpoint is the first presentation");
    Expect(window.last.has_value() && *window.last == At(3000.0),
           "window last endpoint is the last presentation");
    // window span = 3000ms; one freeze -> 1 * 60000 / 3000 = 20 freezes/min.
    const double span_ms = std::chrono::duration<double, std::milli>(
                               *window.last - *window.first)
                               .count();
    ExpectNear(span_ms, 3000.0, 1e-6, "measured window span");
    const double per_min = gap.gap_count * 60000.0 / span_ms;
    ExpectNear(per_min, 20.0, 1e-6, "freezes-per-minute denominator");
  }

  if (failures == 0) {
    std::cout << "inter_present_gap_test: all assertions passed\n";
    return 0;
  }
  std::cerr << "inter_present_gap_test: " << failures << " assertion(s) failed\n";
  return 1;
}
