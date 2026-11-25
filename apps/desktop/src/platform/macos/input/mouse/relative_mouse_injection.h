/*
 * @Author: Farlink contributors
 * @Date: 2026-11-25
 * Copyright (c) 2026 by Farlink contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef _RELATIVE_MOUSE_INJECTION_H_
#define _RELATIVE_MOUSE_INJECTION_H_

#include <algorithm>
#include <cmath>

namespace crossdesk {

enum class RelativeDragState { none, left, right, other };

// Pure state machine turning wire displacements into Core Graphics event
// parameters. Kept free of framework calls so the rounding and clamping
// behaviour is unit-testable.
//
// Deltas arrive in physical pixels of the controlled display (see
// remote_action.h). The wire preserves sub-pixel fractions and so does this
// accumulator: integer rounding applies only to the residual carry, never to
// the sum, so N fractional moves cannot drift from their expected total.
class RelativeMouseMotion {
 public:
  struct Event {
    double point_x = 0;
    double point_y = 0;
    // Integer deltas for kCGMouseEventDeltaX/Y. These always carry the full
    // accumulated integer motion, clamped or not: a game reading them must
    // keep turning the camera when the host cursor is pinned at the display
    // edge, exactly like a physical mouse that keeps moving against the edge
    // of a mouse pad. Fractional remainder stays in the accumulator.
    int64_t delta_x = 0;
    int64_t delta_y = 0;
  };

  // Clamp keeps the injected cursor point inside the display it is relative
  // to. Display bounds are exclusive on the right/bottom edge, matching the
  // absolute injection path. Clamping applies to the point only, never to
  // the delta fields.
  struct Bounds {
    double left = 0;
    double top = 0;
    double right = 0;
    double bottom = 0;
  };

  static constexpr double kMaxDeltaPerEvent = 32767.0;

  // Advances the cursor by the fractional displacement and returns the event
  // parameters. |current_x/current_y| is the tracked cursor position just
  // before injection. The point clamps at the display bounds while the
  // integer deltas pass through unclamped — the display edge is not game
  // geometry, and swallowing motion there would stop camera movement in any
  // game whose own cursor is captured or centred.
  Event Update(double dx, double dy, double current_x, double current_y,
               const Bounds& bounds) {
    carry_x_ += std::clamp(dx, -kMaxDeltaPerEvent, kMaxDeltaPerEvent);
    carry_y_ += std::clamp(dy, -kMaxDeltaPerEvent, kMaxDeltaPerEvent);
    Event event;
    event.delta_x = static_cast<int64_t>(std::llround(carry_x_));
    event.delta_y = static_cast<int64_t>(std::llround(carry_y_));
    carry_x_ -= static_cast<double>(event.delta_x);
    carry_y_ -= static_cast<double>(event.delta_y);
    const double max_x =
        std::nextafter(bounds.right, bounds.left);
    const double max_y =
        std::nextafter(bounds.bottom, bounds.top);
    const double target_x = current_x + static_cast<double>(event.delta_x);
    const double target_y = current_y + static_cast<double>(event.delta_y);
    event.point_x =
        std::clamp(target_x, bounds.left, std::max(max_x, bounds.left));
    event.point_y =
        std::clamp(target_y, bounds.top, std::max(max_y, bounds.top));
    return event;
  }

  void Reset() {
    carry_x_ = 0;
    carry_y_ = 0;
  }

  double carry_x() const { return carry_x_; }
  double carry_y() const { return carry_y_; }

 private:
  double carry_x_ = 0;
  double carry_y_ = 0;
};

// Maps the active drag state to the Core Graphics event type/button pair the
// injected relative move must use. Games with a held button read dragged
// events; plain moves must stay unpressed.
struct RelativeDragEvent {
  int event_type;   // CGEventType, stored as int to stay header-only.
  int mouse_button; // CGMouseButton, same reasoning.
};

constexpr RelativeDragEvent RelativeMoveEventType(RelativeDragState state) {
  switch (state) {
    case RelativeDragState::left:
      // kCGEventLeftMouseDragged / kCGMouseButtonLeft.
      return {6, 0};
    case RelativeDragState::right:
      // kCGEventRightMouseDragged / kCGMouseButtonRight.
      return {7, 1};
    case RelativeDragState::other:
      // kCGEventOtherMouseDragged / kCGMouseButtonCenter.
      return {27, 2};
    default:
      // kCGEventMouseMoved / kCGMouseButtonLeft (button unused for moves).
      return {5, 0};
  }
}

}  // namespace crossdesk

#endif
