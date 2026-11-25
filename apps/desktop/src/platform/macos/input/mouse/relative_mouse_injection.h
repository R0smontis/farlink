/*
 * @Author: DI JUNKUN
 * @Date: 2026-11-25
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _RELATIVE_MOUSE_INJECTION_H_
#define _RELATIVE_MOUSE_INJECTION_H_

#include <algorithm>
#include <cmath>

namespace crossdesk {

enum class RelativeDragState { none, left, right };

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
    // Integer device deltas for kCGMouseEventDeltaX/Y. Games reading raw
    // deltas expect pixel units and ignore fractional remainder, which stays
    // in the accumulator instead of being dropped.
    int64_t delta_x = 0;
    int64_t delta_y = 0;
  };

  // Clamp keeps the injected cursor inside the display it is relative to.
  // Display bounds are exclusive on the right/bottom edge, matching the
  // absolute injection path.
  struct Bounds {
    double left = 0;
    double top = 0;
    double right = 0;
    double bottom = 0;
  };

  static constexpr double kMaxDeltaPerEvent = 32767.0;

  // Advances the cursor by the fractional displacement and returns the event
  // parameters. |current_x/current_y| is the live cursor position read just
  // before injection; tracking it (instead of an internal copy) means other
  // mouse sources cannot silently desynchronise the clamp. Out-of-bounds
  // motion is clamped away rather than accumulated: a wall in an FPS must
  // behave like a wall, not like a stored offset.
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
    double target_x = current_x + static_cast<double>(event.delta_x);
    double target_y = current_y + static_cast<double>(event.delta_y);
    const double clamped_x =
        std::clamp(target_x, bounds.left, std::max(max_x, bounds.left));
    const double clamped_y =
        std::clamp(target_y, bounds.top, std::max(max_y, bounds.top));
    if (clamped_x != target_x) {
      event.delta_x = static_cast<int64_t>(std::llround(clamped_x - current_x));
      carry_x_ = 0;
    }
    if (clamped_y != target_y) {
      event.delta_y = static_cast<int64_t>(std::llround(clamped_y - current_y));
      carry_y_ = 0;
    }
    event.point_x = clamped_x;
    event.point_y = clamped_y;
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
    default:
      // kCGEventMouseMoved / kCGMouseButtonLeft (button unused for moves).
      return {5, 0};
  }
}

}  // namespace crossdesk

#endif
