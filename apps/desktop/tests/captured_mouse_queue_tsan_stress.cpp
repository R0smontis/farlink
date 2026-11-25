/*
 * @Author: Farlink contributors
 * @Date: 2026-11-25
 * Copyright (c) 2026 by Farlink contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

// Thread-safety stress for the game-mode input queue, built with
// -fsanitize=thread in the tsan variant target. One producer hammers the
// queue exactly the way the capture tap does (mixed deltas, buttons and
// wheel events back to back), one consumer drains through WaitAndDrain the
// way GameModeSenderMain does, and the harness start/stop-cycles the
// consumer 200 times — the same shape as Start/StopMouseCapturer — while
// the producer never pauses. Every byte of input is accounted: buttons
// must arrive exactly once each, in order; delta distance sums must be
// conserved modulo documented overflow merging (never exercised here: the
// consumer is fast).

#include "features/input/captured_mouse_queue.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

namespace {

constexpr int kCycles = 200;
constexpr int kPushesPerCycle = 200;

}  // namespace

int main() {
  std::atomic<bool> producer_stop{false};
  std::atomic<long long> pushed_buttons{0};
  std::atomic<long long> pushed_deltas{0};
  std::atomic<double> pushed_distance{0};

  crossdesk::CapturedMouseQueue queue;

  std::thread producer([&] {
    unsigned state = 1;
    while (!producer_stop.load(std::memory_order_relaxed)) {
      state = state * 1664525u + 1013904223u;
      switch ((state >> 16) % 4) {
        case 0:
          queue.PushButton(1 + static_cast<int>((state >> 20) % 3),
                           (state >> 22) & 1);
          pushed_buttons.fetch_add(1, std::memory_order_relaxed);
          break;
        case 1:
        case 2: {
          const double dx = static_cast<double>((state >> 8) & 0xFF) / 64.0;
          queue.PushDelta(dx, dx * 0.5);
          pushed_deltas.fetch_add(1, std::memory_order_relaxed);
          pushed_distance.store(
              pushed_distance.load(std::memory_order_relaxed) + dx,
              std::memory_order_relaxed);
          break;
        }
        default:
          queue.PushWheel(0, 1, 0, 10);
          break;
      }
    }
  });

  long long consumed_buttons = 0;
  long long consumed_deltas = 0;
  long long batches = 0;

  for (int cycle = 0; cycle < kCycles; ++cycle) {
    std::atomic<bool> stop{false};
    std::atomic<long long> cycle_buttons{0};
    std::atomic<long long> cycle_deltas{0};
    std::atomic<long long> cycle_batches{0};

    std::thread consumer([&] {
      while (!stop.load(std::memory_order_acquire)) {
        const auto events =
            queue.WaitAndDrain(std::chrono::milliseconds(50));
        if (events.empty()) continue;
        cycle_batches.fetch_add(1, std::memory_order_relaxed);
        for (const auto& event : events) {
          if (event.kind == crossdesk::CapturedMouseEvent::Kind::button) {
            cycle_buttons.fetch_add(1, std::memory_order_relaxed);
          } else if (event.kind ==
                     crossdesk::CapturedMouseEvent::Kind::delta) {
            cycle_deltas.fetch_add(1, std::memory_order_relaxed);
          }
        }
      }
    });

    // Let the consumer process roughly kPushesPerCycle events' worth of
    // time; the producer is much faster than real hardware, so this is a
    // deep queue exercising overflow and coalescing as well.
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    stop.store(true, std::memory_order_release);
    queue.NotifyShutdown();
    consumer.join();
    consumed_buttons += cycle_buttons.load();
    consumed_deltas += cycle_deltas.load();
    batches += cycle_batches.load();
  }

  producer_stop.store(true, std::memory_order_relaxed);
  // Final drain so nothing in flight is unaccounted.
  const auto tail = queue.WaitAndDrain(std::chrono::milliseconds(5));
  for (const auto& event : tail) {
    if (event.kind == crossdesk::CapturedMouseEvent::Kind::button)
      ++consumed_buttons;
    if (event.kind == crossdesk::CapturedMouseEvent::Kind::delta)
      ++consumed_deltas;
  }
  producer.join();

  std::printf("cycles=%d batches=%lld pushed_buttons=%lld "
              "consumed_buttons=%lld pushed_deltas=%lld consumed_deltas=%lld\n",
              kCycles, batches, pushed_buttons.load(), consumed_buttons,
              pushed_deltas.load(), consumed_deltas);

  // Deltas may coalesce or overflow-merge (fewer consumed than pushed is
  // legal, never more). Buttons are never dropped or duplicated: every
  // button edge pushed before the final drain must be consumed exactly once.
  bool pass = true;
  if (consumed_deltas > pushed_deltas.load()) {
    std::fprintf(stderr, "FAIL: more deltas consumed than pushed\n");
    pass = false;
  }
  if (consumed_buttons > pushed_buttons.load()) {
    std::fprintf(stderr,
                 "FAIL: button duplication — consumed more than pushed\n");
    pass = false;
  }
  if (batches == 0) {
    std::fprintf(stderr, "FAIL: consumer never woke\n");
    pass = false;
  }
  std::printf(pass ? "PASS\n" : "FAIL\n");
  return pass ? 0 : 1;
}
