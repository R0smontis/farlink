/*
 * @Author: Farlink contributors
 * @Date: 2026-11-25
 * Copyright (c) 2026 by Farlink contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

// Latency benchmark for game-mode input delivery. Measures the time from a
// queue push (what the capture tap does) to consumption under two drain
// models, ping-pong style — one event in flight at a time, so per-event
// correlation is exact:
//   tick  — the old model: a 16 ms UI timer drains everything queued.
//   woken — the new model: a sender thread blocked in WaitAndDrain wakes on
//           the push notification.
// Reports p50/p95/max microseconds over >=1000 events for both models.

#include "features/input/captured_mouse_queue.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct Stats {
  double p50 = 0;
  double p95 = 0;
  double max = 0;
};

Stats Percentiles(std::vector<double> samples) {
  std::sort(samples.begin(), samples.end());
  Stats s;
  if (samples.empty()) return s;
  s.p50 = samples[samples.size() / 2];
  s.p95 = samples[static_cast<size_t>(samples.size() * 0.95)];
  s.max = samples.back();
  return s;
}

constexpr int kEvents = 1200;

}  // namespace

int main() {
  // ---- tick model: drain every 16 ms like the Slint UI timer ----
  {
    crossdesk::CapturedMouseQueue queue;
    std::atomic<bool> stop{false};
    std::atomic<int> consumed{0};
    std::vector<double> latencies;
    latencies.reserve(kEvents);

    std::thread consumer([&] {
      while (!stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
        const auto events = queue.WaitAndDrain(std::chrono::milliseconds(0));
        if (!events.empty()) {
          consumed.fetch_add(static_cast<int>(events.size()));
        }
      }
    });

    for (int i = 0; i < kEvents; ++i) {
      const int before = consumed.load();
      const auto t0 = Clock::now();
      queue.PushDelta(1.0, 1.0);
      // Ping-pong: wait until the consumer drains this event. Coalescing can
      // only merge with a queued predecessor, and there is none here.
      while (consumed.load() == before) {
        std::this_thread::yield();
      }
      const auto t1 = Clock::now();
      latencies.push_back(
          std::chrono::duration<double, std::micro>(t1 - t0).count());
    }
    stop.store(true);
    consumer.join();
    const Stats s = Percentiles(latencies);
    std::printf("tick   model: n=%d p50=%.0fus p95=%.0fus max=%.0fus\n",
                kEvents, s.p50, s.p95, s.max);
  }

  // ---- woken model: sender thread in WaitAndDrain ----
  {
    crossdesk::CapturedMouseQueue queue;
    std::atomic<bool> stop{false};
    std::atomic<int> consumed{0};
    std::vector<double> latencies;
    latencies.reserve(kEvents);

    std::thread sender([&] {
      while (!stop.load()) {
        const auto events = queue.WaitAndDrain(std::chrono::milliseconds(100));
        if (!events.empty()) {
          consumed.fetch_add(static_cast<int>(events.size()));
        }
      }
    });

    for (int i = 0; i < kEvents; ++i) {
      const int before = consumed.load();
      const auto t0 = Clock::now();
      queue.PushDelta(1.0, 1.0);
      while (consumed.load() == before) {
        std::this_thread::yield();
      }
      const auto t1 = Clock::now();
      latencies.push_back(
          std::chrono::duration<double, std::micro>(t1 - t0).count());
    }
    stop.store(true);
    queue.NotifyShutdown();
    sender.join();
    const Stats s = Percentiles(latencies);
    std::printf("woken  model: n=%d p50=%.0fus p95=%.0fus max=%.0fus\n",
                kEvents, s.p50, s.p95, s.max);
  }

  return 0;
}
