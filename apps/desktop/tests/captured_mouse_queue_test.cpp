/*
 * @Author: Farlink contributors
 * @Date: 2026-11-25
 * Copyright (c) 2026 by Farlink contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "features/input/captured_mouse_queue.h"

#include <chrono>
#include <cmath>
#include <iostream>

namespace {

bool Expect(bool condition, const char* message) {
  if (condition) return true;
  std::cerr << message << '\n';
  return false;
}

using crossdesk::CapturedMouseEvent;
using crossdesk::CapturedMouseQueue;

// Total distance carried by every delta event in the batch.
std::pair<double, double> Distance(
    const std::deque<CapturedMouseEvent>& events) {
  double x = 0, y = 0;
  for (const auto& event : events) {
    if (event.kind == CapturedMouseEvent::Kind::delta) {
      x += event.dx;
      y += event.dy;
    }
  }
  return {x, y};
}

}  // namespace

int main() {
  bool ok = true;

  // Overflow must cost ordering granularity, never distance: the merged
  // total across the surviving queue is exactly the pushed total.
  {
    CapturedMouseQueue queue;
    double total_x = 0, total_y = 0;
    // Flood past kMaxEvents (1024) without draining.
    for (int i = 0; i < 3000; ++i) {
      const double dx = 1.0 + (i % 7) * 0.125;
      const double dy = -0.5;
      queue.PushDelta(dx, dy);
      total_x += dx;
      total_y += dy;
    }
    const auto events = queue.WaitAndDrain(std::chrono::milliseconds(1));
    const auto [x, y] = Distance(events);
    ok &= Expect(std::fabs(x - total_x) < 1e-6,
                 "overflow must conserve horizontal distance");
    ok &= Expect(std::fabs(y - total_y) < 1e-6,
                 "overflow must conserve vertical distance");
  }

  // Overflow with a button edge at the front: the dropped delta folds into
  // the NEW push (there is no earlier delta to merge into), preserving both
  // distance and the button edge itself.
  {
    CapturedMouseQueue queue;
    // Fill the queue to capacity with a button at the front, so the oldest
    // event is not a delta: button first, then alternating fill.
    queue.PushButton(1, true);
    // Trailing deltas coalesce into one event, so force separation with
    // alternating buttons — the point here is only that a button sits at
    // the front when overflow hits.
    for (int i = 0; i < 1100; ++i) {
      queue.PushDelta(2.0, 0);
      queue.PushButton(1, i % 2 == 0);
    }
    double total_x = 2.0 * 1100;
    const auto events = queue.WaitAndDrain(std::chrono::milliseconds(1));
    const auto [x, y] = Distance(events);
    ok &= Expect(std::fabs(x - total_x) < 1e-6,
                 "overflow across button edges must conserve distance");
    bool saw_button = false;
    for (const auto& event : events) {
      if (event.kind == CapturedMouseEvent::Kind::button) saw_button = true;
    }
    ok &= Expect(saw_button, "button edges must survive overflow");
    (void)y;
  }

  // Back-pressure coalescing: consecutive deltas merge into the trailing
  // one, losslessly.
  {
    CapturedMouseQueue queue;
    queue.PushDelta(0.3, 0.1);
    queue.PushDelta(0.4, 0.2);
    const auto events = queue.WaitAndDrain(std::chrono::milliseconds(1));
    ok &= Expect(events.size() == 1,
                 "back-to-back deltas must coalesce into one event");
    const auto [x, y] = Distance(events);
    ok &= Expect(std::fabs(x - 0.7) < 1e-12 && std::fabs(y - 0.3) < 1e-12,
                 "coalesced delta must carry the exact sum");
  }

  // A button edge between deltas blocks coalescing across it.
  {
    CapturedMouseQueue queue;
    queue.PushDelta(1.0, 0);
    queue.PushButton(1, true);
    queue.PushDelta(2.0, 0);
    const auto events = queue.WaitAndDrain(std::chrono::milliseconds(1));
    ok &= Expect(events.size() == 3,
                 "button edges must preserve delta ordering boundaries");
  }

  return ok ? 0 : 1;
}
