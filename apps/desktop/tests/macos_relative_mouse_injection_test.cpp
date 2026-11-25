/*
 * @Author: Farlink contributors
 * @Date: 2026-11-25
 * Copyright (c) 2026 by Farlink contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "mouse/relative_mouse_injection.h"
#include "mouse/relative_mouse_capture.h"

#include <cmath>
#include <iostream>

namespace {

bool Expect(bool condition, const char* message) {
  if (condition) return true;
  std::cerr << message << '\n';
  return false;
}

constexpr crossdesk::RelativeMouseMotion::Bounds kBounds{0, 0, 2560, 1600};

}  // namespace

int main() {
  bool ok = true;

  // Delta accumulation: N fractional moves must land within sub-pixel
  // tolerance of their exact sum. This is the property FPS look depends on;
  // integer-per-event rounding would lose up to half a pixel per event.
  {
    crossdesk::RelativeMouseMotion motion;
    double x = 1000, y = 800;
    double injected_x = 0, injected_y = 0;
    constexpr int kMoves = 1000;
    for (int i = 0; i < kMoves; ++i) {
      const auto event = motion.Update(0.4, -0.3, x, y, kBounds);
      x = event.point_x;
      y = event.point_y;
      injected_x += event.delta_x;
      injected_y += event.delta_y;
    }
    ok &= Expect(std::fabs(injected_x - kMoves * 0.4) <= 1.0,
                 "accumulated x deltas drifted beyond one pixel over 1000 moves");
    ok &= Expect(std::fabs(injected_y - kMoves * -0.3) <= 1.0,
                 "accumulated y deltas drifted beyond one pixel over 1000 moves");
    ok &= Expect(std::fabs((x - 1000) - kMoves * 0.4) <= 1.0,
                 "cursor position drifted from the injected delta sum");
  }

  // The sum of emitted integer deltas must match the exact fractional sum
  // within one quantum of rounding per axis, even with awkward fractions.
  {
    crossdesk::RelativeMouseMotion motion;
    double x = 500, y = 500;
    double total = 0;
    double emitted = 0;
    for (int i = 1; i <= 733; ++i) {
      const double step = (i % 7) * 0.17 - 0.42;
      total += step;
      const auto event = motion.Update(step, 0, x, y, kBounds);
      x = event.point_x;
      emitted += event.delta_x;
    }
    ok &= Expect(std::fabs(emitted - total) <= 1.0,
                 "irregular fractional deltas must sum losslessly");
  }

  // Display edge: the point pins at the bound but the deltas keep carrying
  // the full unclamped integer motion, exactly like a physical mouse that
  // keeps moving against the edge of the pad. Games reading
  // kCGMouseEventDeltaX/Y must keep turning the camera; fractional carry is
  // preserved, so reversal is not slingshot either — the accumulator only
  // ever holds sub-pixel remainder, and N edge-pinned moves still sum to
  // the exact input total.
  {
    crossdesk::RelativeMouseMotion motion;
    double x = 2559, y = 800;
    auto event = motion.Update(100, 0, x, y, kBounds);
    ok &= Expect(event.point_x < 2560 && event.point_x >= 2559,
                 "edge motion must pin the point on the display edge");
    ok &= Expect(event.delta_x == 100,
                 "edge motion must not clamp the carried delta");
    const double edge = event.point_x;
    // Keep pushing right; the point stays pinned, deltas keep flowing.
    double emitted = 100.0;
    constexpr int kPinned = 50;
    for (int i = 0; i < kPinned; ++i) {
      event = motion.Update(100, 0, event.point_x, 800, kBounds);
      emitted += event.delta_x;
    }
    ok &= Expect(event.point_x == edge,
                 "pinned motion must leave the point exactly on the edge");
    ok &= Expect(std::fabs(emitted - 100.0 * (kPinned + 1)) <= 1.0,
                 "pinned deltas must sum to the unclamped total within 1px");
    // Reverse with a small step: it must apply immediately, unattenuated.
    event = motion.Update(-5, 0, event.point_x, 800, kBounds);
    ok &= Expect(std::fabs(event.delta_x - -5) <= 1 &&
                     event.point_x <= edge - 4,
                 "reversing off the edge must apply the delta immediately");
  }

  // Zero motion produces no event but keeps the sub-pixel carry.
  {
    crossdesk::RelativeMouseMotion motion;
    auto event = motion.Update(0.3, 0, 100, 100, kBounds);
    ok &= Expect(event.delta_x == 0 && event.delta_y == 0,
                 "sub-quantum motion must not emit integer deltas");
    event = motion.Update(0.3, 0, 100, 100, kBounds);
    ok &= Expect(event.delta_x == 1,
                 "two sub-quantum motions must merge into one pixel");
  }

  // Extreme single-event deltas are capped instead of teleporting.
  {
    crossdesk::RelativeMouseMotion motion;
    const auto event = motion.Update(1e9, -1e9, 100, 100, kBounds);
    ok &= Expect(event.point_x < 2560 && event.point_x >= 0 &&
                     event.point_y < 1600 && event.point_y >= 0,
                 "extreme deltas must stay inside the display");
  }

  // Dragged event type mapping: dragging with a button held must emit the
  // dragged types games read, plain moves must not.
  {
    const auto plain = crossdesk::RelativeMoveEventType(crossdesk::RelativeDragState::none);
    const auto left = crossdesk::RelativeMoveEventType(crossdesk::RelativeDragState::left);
    const auto right = crossdesk::RelativeMoveEventType(crossdesk::RelativeDragState::right);
    const auto other = crossdesk::RelativeMoveEventType(crossdesk::RelativeDragState::other);
    ok &= Expect(plain.event_type == 5 && left.event_type == 6 &&
                     right.event_type == 7 && other.event_type == 27 &&
                     left.mouse_button == 0 && right.mouse_button == 1 &&
                     other.mouse_button == 2,
                 "dragged event type mapping changed");
  }

  // Escape chord: Ctrl+Alt+G fires once per press, survives auto-repeat,
  // and requires both modifiers. The completing key-down, its repeats and
  // the matching key-up are marked swallowed so the capturer never leaks
  // them to the peer.
  {
    crossdesk::GameModeExitChord chord;
    ok &= Expect(!chord.Update('G', true), "G alone must not trigger the escape");
    chord.Reset();
    chord.Update(0xA2, true);   // left ctrl down
    ok &= Expect(!chord.Update('G', true), "ctrl+G without alt must not trigger");
    ok &= Expect(!chord.SwallowsKey('G'), "no chord means nothing is swallowed");
    chord.Update(0xA4, true);   // left alt down
    ok &= Expect(chord.Update('G', true), "ctrl+alt+G must trigger the escape");
    ok &= Expect(chord.SwallowsKey('G'),
                 "the completing key-down must be swallowed");
    ok &= Expect(!chord.Update('G', true), "auto-repeat must not retrigger");
    ok &= Expect(chord.SwallowsKey('G'),
                 "auto-repeats of a held chord key must stay swallowed");
    ok &= Expect(chord.SwallowsKey('G'),
                 "the matching key-up must be swallowed before Update");
    chord.Update('G', false);   // release G
    ok &= Expect(!chord.SwallowsKey('G'),
                 "after the key-up the swallow state must clear");
    chord.Update(0xA2, false);  // release ctrl
    ok &= Expect(!chord.Update('G', true), "chord without ctrl must not trigger");
    chord.Update(0xA2, true);
    chord.Update('G', false);
    ok &= Expect(chord.Update('G', true), "a fresh press must trigger again");
    chord.Update('G', false);
  }

  // Delta scaling: device points convert into display pixels; invalid scale
  // must reject the event rather than inject garbage.
  {
    double dx = 0, dy = 0;
    ok &= Expect(crossdesk::RelativeDeltaScale::Convert(2, 3, 2.0, 2.0, dx, dy) &&
                     dx == 4 && dy == 6,
                 "delta scaling must multiply device points by the ratio");
    ok &= Expect(!crossdesk::RelativeDeltaScale::Convert(2, 3, 0, 2.0, dx, dy),
                 "zero scale must reject the event");
    ok &= Expect(!crossdesk::RelativeDeltaScale::Convert(2, 3, NAN, 2.0, dx, dy),
                 "non-finite scale must reject the event");
  }

  return ok ? 0 : 1;
}
