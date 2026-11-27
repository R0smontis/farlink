/*
 * @Author: Farlink contributors
 * @Date: 2026-11-27
 * Copyright (c) 2026 by Farlink contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

// Wave 6D per-stage latency attribution tests. Three deterministic areas:
//
//  1. VideoLatencyStats decode/render rolling windows: exact mean/p95 over
//     the 60-sample ring, invalid/zero timestamp safety, estimate-vs-true
//     dedup by frame identity, Reset() isolation.
//  2. ClassifyLatencyBottleneck across all five regimes (none, network,
//     jitter, decode, render), including unavailable-input handling.
//  3. RemoteInputDispatcher: 10,000-event FIFO click-boundary ordering plus
//     the queued->applied microsecond latency microbenchmark, verifying the
//     wake-outside-lock fast lane keeps p50 dispatch latency in the tens of
//     microseconds.
//
// Everything is driven by fake monotonic timestamps or real wall time only
// where the benchmark requires it; no GPU, network, or codec is involved.

#include "rendering/video_latency.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

#include "features/input/remote_input_dispatcher.h"

namespace {

using crossdesk::LatencyBottleneck;
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

Clock::time_point At(double ms) {
  return Clock::time_point(std::chrono::duration_cast<Clock::duration>(
      std::chrono::duration<double, std::milli>(ms)));
}

// Feed one frame through the real FrameWithDecode + Record path with fully
// controlled timestamps: the frame is "received" at receive_ms and rendered
// (presented endpoint) at render_ms. Decode cost is supplied directly, as
// the transport would measure it.
void DriveFrame(std::shared_ptr<VideoLatencyStats> stats, uint64_t captured_us,
                int64_t local_now_us, double decode_ms, double receive_ms,
                double render_ms) {
  auto frame = stats->FrameWithDecode(captured_us, local_now_us, decode_ms,
                                      At(receive_ms));
  stats->Record(frame, At(render_ms), false);
}

// ---------------------------------------------------------------------------
// Decode window: mean/p95 exactness and invalid-input safety
// ---------------------------------------------------------------------------

void TestDecodeWindowStats() {
  auto stats = std::make_shared<VideoLatencyStats>();
  // 30 frames at exactly 2.0 ms decode, then 30 at 4.0 ms.
  for (int i = 0; i < 30; ++i)
    DriveFrame(stats, 0, -1, 2.0, i * 16.0, i * 16.0 + 8.0);
  for (int i = 30; i < 60; ++i)
    DriveFrame(stats, 0, -1, 4.0, i * 16.0, i * 16.0 + 8.0);
  const auto snap = stats->TakeStageSnapshot();
  Expect(snap.decode.samples == 60, "decode window holds 60 samples");
  ExpectNear(snap.decode.mean_ms, 3.0, 1e-9, "decode mean of 2/4 mix");
  Expect(snap.decode.max_ms >= 4.0, "decode max at least 4ms");
  // p95 nearest-rank of 60 samples: rank index 56 (0-based); 30 samples at
  // 2.0 and 30 at 4.0 -> sorted positions 30..59 are 4.0 -> p95 == 4.0.
  ExpectNear(snap.decode.p95_ms, 4.0, 1e-9, "decode p95 exact order statistic");
}

void TestDecodeWindowInvalidInput() {
  auto stats = std::make_shared<VideoLatencyStats>();
  // Negative decode (missing timestamps) must never enter the window.
  DriveFrame(stats, 0, -1, -1, 0.0, 8.0);
  DriveFrame(stats, 0, -1, -100.0, 16.0, 24.0);
  auto snap = stats->TakeStageSnapshot();
  Expect(snap.decode.samples == 0, "negative decode samples are rejected");
  Expect(snap.decode.mean_ms < 0, "empty decode mean is -1 sentinel");
  // NaN/inf must be rejected too.
  DriveFrame(stats, 0, -1, NAN, 32.0, 40.0);
  DriveFrame(stats, 0, -1, INFINITY, 48.0, 56.0);
  snap = stats->TakeStageSnapshot();
  Expect(snap.decode.samples == 0, "non-finite decode samples are rejected");
  // A zero decode is legitimate (instant hardware decode) and must count.
  DriveFrame(stats, 0, -1, 0.0, 64.0, 72.0);
  snap = stats->TakeStageSnapshot();
  Expect(snap.decode.samples == 1, "zero decode is a valid measurement");
  ExpectNear(snap.decode.mean_ms, 0.0, 1e-9, "zero decode mean is zero");
}

void TestDecodeWindowEviction() {
  auto stats = std::make_shared<VideoLatencyStats>();
  // 60 samples at 1.0 ms, then 10 at 9.0 ms: the ring evicts the oldest 10.
  for (int i = 0; i < 60; ++i)
    DriveFrame(stats, 0, -1, 1.0, i * 16.0, i * 16.0 + 8.0);
  for (int i = 60; i < 70; ++i)
    DriveFrame(stats, 0, -1, 9.0, i * 16.0, i * 16.0 + 8.0);
  const auto snap = stats->TakeStageSnapshot();
  Expect(snap.decode.samples == 60, "decode ring stays at capacity");
  // 50 at 1.0 + 10 at 9.0 -> mean = (50 + 90) / 60 = 140/60
  ExpectNear(snap.decode.mean_ms, 140.0 / 60.0, 1e-9,
             "decode ring evicts oldest samples");
}

// ---------------------------------------------------------------------------
// Render window: endpoint timing, dedup by frame identity
// ---------------------------------------------------------------------------

void TestRenderWindowStats() {
  auto stats = std::shared_ptr<VideoLatencyStats>(new VideoLatencyStats());
  // Frame received at t=100 presented at t=104 -> exactly 4.0 ms render.
  auto frame = stats->FrameWithDecode(0, -1, 2.0, At(100.0));
  stats->Record(frame, At(104.0), false);
  auto snap = stats->TakeStageSnapshot();
  Expect(snap.render.samples == 1, "one render sample recorded");
  ExpectNear(snap.render.mean_ms, 4.0, 1e-9, "render mean exact");
  ExpectNear(snap.render.p95_ms, 4.0, 1e-9, "render p95 of single sample");

  // Estimate upgrade path: GPU-completion estimate at t=102 for a second
  // frame received at t=116, then true presentation at t=121. The estimate
  // (2 ms) must be REPLACED by the true presentation (5 ms), not added.
  auto frame2 = stats->FrameWithDecode(0, -1, 2.0, At(116.0));
  stats->Record(frame2, At(118.0), true);   // estimate: 2 ms
  stats->Record(frame2, At(121.0), false);  // true: 5 ms replaces it
  snap = stats->TakeStageSnapshot();
  Expect(snap.render.samples == 2, "estimate upgrade does not double-count");
  // mean of 4.0 and 5.0
  ExpectNear(snap.render.mean_ms, 4.5, 1e-9, "true endpoint replaces estimate");
}

void TestRenderWindowResetIsolation() {
  auto stats = std::make_shared<VideoLatencyStats>();
  DriveFrame(stats, 0, -1, 3.0, 100.0, 106.0);
  stats->Reset(At(200.0));
  const auto snap = stats->TakeStageSnapshot();
  Expect(snap.decode.samples == 0, "reset clears decode window");
  Expect(snap.render.samples == 0, "reset clears render window");
}

// ---------------------------------------------------------------------------
// ClassifyLatencyBottleneck: all five regimes
// ---------------------------------------------------------------------------

void TestClassifyBottleneck() {
  using crossdesk::ClassifyLatencyBottleneck;
  // Healthy interactive pipeline: nothing dominates.
  Expect(ClassifyLatencyBottleneck(10, 3, 2, 4) == LatencyBottleneck::none,
         "healthy pipeline is bottleneck-free");
  Expect(ClassifyLatencyBottleneck(24.9, 7.9, 5.9, 9.9) ==
             LatencyBottleneck::none,
         "just under every threshold stays none");
  // Network-bound: RTT dominates its 25ms budget by the largest excess.
  Expect(ClassifyLatencyBottleneck(80, 2, 2, 4) ==
             LatencyBottleneck::network,
         "high RTT is network-bound");
  // Jitter-bound: jitter target dominates even with moderate RTT.
  Expect(ClassifyLatencyBottleneck(30, 40, 2, 4) ==
             LatencyBottleneck::jitter,
         "large jitter target is jitter-bound");
  // Decode-bound: decoder stall dominates.
  Expect(ClassifyLatencyBottleneck(10, 2, 30, 4) == LatencyBottleneck::decode,
         "slow decode is decode-bound");
  // Render-bound: presentation path dominates.
  Expect(ClassifyLatencyBottleneck(10, 2, 2, 35) == LatencyBottleneck::render,
         "slow render is render-bound");
  // Largest excess wins even when several stages are degraded: RTT 40 (15
  // over) vs decode 30 (24 over) -> decode must win.
  Expect(ClassifyLatencyBottleneck(40, 2, 30, 4) == LatencyBottleneck::decode,
         "largest excess over own budget wins, not absolute value");
  // Unavailable inputs never dominate.
  Expect(ClassifyLatencyBottleneck(-1, -1, -1, -1) == LatencyBottleneck::none,
         "all-unavailable inputs are bottleneck-free");
  Expect(ClassifyLatencyBottleneck(-1, -1, 30, 2) == LatencyBottleneck::decode,
         "negative RTT/jitter do not mask a decode stall");
}

}  // namespace

int main() {
  TestDecodeWindowStats();
  TestDecodeWindowInvalidInput();
  TestDecodeWindowEviction();
  TestRenderWindowStats();
  TestRenderWindowResetIsolation();
  TestClassifyBottleneck();

  // -------------------------------------------------------------------------
  // Dispatcher FIFO ordering + queue-latency microbenchmark
  // -------------------------------------------------------------------------
  {
    using crossdesk::InputOutcome;
    using crossdesk::RemoteAction;
    using crossdesk::RemoteInputDispatcher;
    std::atomic<uint64_t> applied_count{0};
    std::vector<int> order;
    std::mutex order_mutex;
    RemoteInputDispatcher dispatcher(
        [&](const std::string&, const RemoteAction& action) {
          ++applied_count;
          std::lock_guard<std::mutex> guard(order_mutex);
          order.push_back(static_cast<int>(action.m.x));
          return InputOutcome::applied;
        },
        [](const std::string&) { return InputOutcome::applied; });
    dispatcher.Connect("peer");
    constexpr int kEvents = 10000;
    for (int i = 0; i < kEvents; ++i) {
      RemoteAction action{};
      action.type = crossdesk::ControlType::mouse;
      // Alternate clicks (exact FIFO-sensitive) with absolute moves to
      // distinct coordinates so coalescing cannot legitimately merge them:
      // clicks never coalesce, and moves to different x are still merged
      // latest-wins ONLY when adjacent in the queue, so the click sequence
      // must appear in order in the applied stream.
      if (i % 2 == 0) {
        action.m.flag = crossdesk::MouseFlag::left_down;
      } else {
        action.m.flag = crossdesk::MouseFlag::left_up;
      }
      action.m.x = i;  // sequence marker
      action.m.y = 0;
      dispatcher.Push("peer", action);
      // Pace the flood just enough for the worker to drain: the per-
      // controller queue is capped at 128 pending by design (overflow is a
      // recovery path, not input), and this benchmark measures the queue
      // latency of events that ARE dispatched, not backpressure. Even-paced
      // pushing still exercises the enqueue/dequeue fast lane 10,000 times.
      if (i % 32 == 31)
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    dispatcher.WaitIdle();
    Expect(applied_count.load() == kEvents,
           "every click event is applied exactly once");
    {
      std::lock_guard<std::mutex> guard(order_mutex);
      bool ordered = order.size() == static_cast<size_t>(kEvents);
      for (size_t i = 0; ordered && i < order.size(); ++i)
        ordered = order[i] == static_cast<int>(i);
      Expect(ordered, "click boundary FIFO order is exact across 10k events");
    }
    const auto stats = dispatcher.TakeQueueLatencyStats();
    std::cout << "input queue latency: samples=" << stats.samples
              << " mean_us=" << stats.mean_us << " p50_us=" << stats.p50_us
              << " p95_us=" << stats.p95_us << " max_us=" << stats.max_us
              << '\n';
    Expect(stats.samples >= static_cast<uint64_t>(kEvents / 2),
           "queue latency stats cover the event stream");
    // The fast-lane target: median host dispatch queue latency under 50 us.
    // CI machines jitter, so the hard assertion is on the median only and
    // generous; the printed p95/max are the diagnostic evidence.
    Expect(stats.p50_us < 50,
           "wake-outside-lock fast lane keeps p50 queue latency under 50us");
    dispatcher.Stop();
  }

  if (failures == 0) {
    std::cout << "stage_latency_attribution_test: all tests passed\n";
    return 0;
  }
  std::cerr << failures << " failure(s)\n";
  return 1;
}
