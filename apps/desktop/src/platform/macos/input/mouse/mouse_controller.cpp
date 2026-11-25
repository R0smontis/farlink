#include "mouse_controller.h"
#include "../mac_injected_event.h"
#include "relative_mouse_injection.h"

#include <remote_action.h>

#include <ApplicationServices/ApplicationServices.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>

#include "rd_log.h"

namespace crossdesk {
namespace {

constexpr auto kDoubleClickInterval = std::chrono::milliseconds(500);
constexpr int kDoubleClickMaxDistance = 8;
constexpr int kMaxClickState = 3;

bool IsWithinClickDistance(int x1, int y1, int x2, int y2) {
  return std::abs(x1 - x2) <= kDoubleClickMaxDistance &&
         std::abs(y1 - y2) <= kDoubleClickMaxDistance;
}

void SetClickState(CGEventRef event, int click_state) {
  if (!event) {
    return;
  }

  CGEventSetIntegerValueField(
      event, kCGMouseEventClickState,
      std::max(1, std::min(click_state, kMaxClickState)));
}

}  // namespace

PlatformMouseController::PlatformMouseController() {}

PlatformMouseController::~PlatformMouseController() {}

int PlatformMouseController::Init(std::vector<DisplayInfo> display_info_list) {
  display_info_list_ = display_info_list;

  return 0;
}

int PlatformMouseController::Destroy() {
  return ReleasePressedButtons() == 7 ? 0 : -1;
}

unsigned PlatformMouseController::ReleasePressedButtons(unsigned mask) {
  mask &= 7;
  const unsigned pressed = pressed_buttons_ & mask;
  if (!pressed) return mask;
  const auto cursor = CGEventCreate(nullptr);
  if (!cursor) return mask & ~pressed;
  const auto point = CGEventGetLocation(cursor);
  CFRelease(cursor);
  const CGEventType types[] = {kCGEventLeftMouseUp, kCGEventRightMouseUp,
                               kCGEventOtherMouseUp};
  const CGMouseButton buttons[] = {kCGMouseButtonLeft, kCGMouseButtonRight,
                                   kCGMouseButtonCenter};
  for (unsigned i = 0; i < 3; ++i) {
    const unsigned bit = 1u << i;
    if (!(pressed & bit)) continue;
    const auto event = CGEventCreateMouseEvent(nullptr, types[i], point, buttons[i]);
    if (!event) continue;
    CGEventSetIntegerValueField(event, kCGEventSourceUserData, kCrossDeskInjectedEvent);
    CGEventPost(kCGHIDEventTap, event);
    CFRelease(event);
    pressed_buttons_ &= ~bit;
    if (i == 0) { left_dragging_ = false; left_click_tracker_ = {}; }
    if (i == 1) { right_dragging_ = false; right_click_tracker_ = {}; }
    if (i == 2) middle_click_tracker_ = {};
  }
  return mask & ~pressed_buttons_;
}

void PlatformMouseController::UpdateDisplayInfoList(
    const std::vector<DisplayInfo>& display_info_list) {
  if (!display_info_list.empty()) {
    display_info_list_ = display_info_list;
  }
}

int PlatformMouseController::BeginClick(ClickTracker& tracker, int x, int y) {
  const auto now = std::chrono::steady_clock::now();
  const bool continues_previous_click =
      tracker.has_last_down &&
      now - tracker.last_down_time <= kDoubleClickInterval &&
      IsWithinClickDistance(tracker.last_down_x, tracker.last_down_y, x, y);

  tracker.click_state = continues_previous_click
                            ? std::min(tracker.click_state + 1, kMaxClickState)
                            : 1;
  tracker.active_click_state = tracker.click_state;
  tracker.has_last_down = true;
  tracker.last_down_time = now;
  tracker.last_down_x = x;
  tracker.last_down_y = y;

  return tracker.active_click_state;
}

int PlatformMouseController::EndClick(ClickTracker& tracker, int x, int y) {
  const int click_state = tracker.active_click_state;
  if (!IsWithinClickDistance(tracker.last_down_x, tracker.last_down_y, x, y)) {
    tracker.has_last_down = false;
    tracker.click_state = 0;
    tracker.active_click_state = 1;
  }

  return click_state;
}

int PlatformMouseController::SendMouseCommand(RemoteAction remote_action,
                                      int display_index) {
  if (remote_action.type != ControlType::mouse) {
    return 0;
  }

  if (display_index < 0 ||
      display_index >= static_cast<int>(display_info_list_.size())) {
    LOG_WARN("Mouse command skipped, invalid display_index={}, displays={}",
             display_index, display_info_list_.size());
    return -1;
  }

  const DisplayInfo& display_info = display_info_list_[display_index];
  if (display_info.width <= 0 || display_info.height <= 0) {
    LOG_WARN("Mouse command skipped, invalid display geometry: {}x{}",
             display_info.width, display_info.height);
    return -1;
  }

  if (remote_action.m.flag == MouseFlag::move_relative) {
    relative_mode_active_ = true;
    // Injected positions do not apply atomically, so reading the cursor back
    // inside a burst returns a stale location and swallows motion. While the
    // stream is live the tracked anchor is authoritative; it is resynced to
    // the physical cursor whenever injection has been idle long enough for
    // local input devices or earlier absolute positioning to have moved it.
    double base_x = 0, base_y = 0;
    const auto now = std::chrono::steady_clock::now();
    if (relative_anchor_valid_ &&
        now - relative_anchor_time_ < kRelativeAnchorMaxAge) {
      base_x = relative_anchor_x_;
      base_y = relative_anchor_y_;
    } else {
      const auto cursor = CGEventCreate(nullptr);
      if (!cursor) return -1;
      const auto current = CGEventGetLocation(cursor);
      CFRelease(cursor);
      base_x = current.x;
      base_y = current.y;
      // A fresh anchor must not carry rounding debt from a different
      // coordinate frame.
      relative_motion_.Reset();
    }
    const RelativeMouseMotion::Bounds bounds{
        static_cast<double>(display_info.left),
        static_cast<double>(display_info.top),
        static_cast<double>(display_info.right),
        static_cast<double>(display_info.bottom)};
    // Wire deltas are fractions of this display's width/height — the same
    // resolution-independent unit the absolute path uses — so injection
    // multiplies by the display size in points. Nothing here depends on the
    // encoded stream's pixel dimensions: resolution adaptation can resize
    // the video mid-session and sensitivity does not move. The conversion
    // runs before the accumulator, which keeps sub-point remainders
    // lossless across events.
    const double width_pt =
        static_cast<double>(display_info.right) - display_info.left;
    const double height_pt =
        static_cast<double>(display_info.bottom) - display_info.top;
    const auto motion = relative_motion_.Update(
        remote_action.m.dx * width_pt, remote_action.m.dy * height_pt,
        base_x, base_y, bounds);
    if (motion.delta_x == 0 && motion.delta_y == 0) return 0;
    const bool other_dragging = (pressed_buttons_ & 4) != 0;
    const auto drag = RelativeMoveEventType(
        left_dragging_ ? RelativeDragState::left
        : right_dragging_ ? RelativeDragState::right
        : other_dragging ? RelativeDragState::other
                         : RelativeDragState::none);
    CGEventRef move_event = CGEventCreateMouseEvent(
        NULL, static_cast<CGEventType>(drag.event_type),
        CGPointMake(motion.point_x, motion.point_y),
        static_cast<CGMouseButton>(drag.mouse_button));
    if (!move_event) return -1;
    // Games reading raw deltas consume these fields; the integer part of the
    // displacement moved the cursor above while the fraction stays in the
    // accumulator, so both views of the motion agree losslessly.
    CGEventSetIntegerValueField(move_event, kCGMouseEventDeltaX,
                                motion.delta_x);
    CGEventSetIntegerValueField(move_event, kCGMouseEventDeltaY,
                                motion.delta_y);
    CGEventSetIntegerValueField(move_event, kCGEventSourceUserData,
                                kCrossDeskInjectedEvent);
    CGEventPost(kCGHIDEventTap, move_event);
    CFRelease(move_event);
    relative_anchor_x_ = motion.point_x;
    relative_anchor_y_ = motion.point_y;
    relative_anchor_time_ = now;
    relative_anchor_valid_ = true;
    return 0;
  }
  // An absolute move ends relative positioning; clicks and the wheel keep
  // whatever mode the most recent move established. Only the absolute move
  // invalidates the tracked anchor: it repositions through the physical
  // cursor, so the next relative burst must resync from it. Clicks and wheel
  // events do not move the cursor, and the anchor refresh below keeps them
  // consistent with the tracked position instead.
  if (remote_action.m.flag == MouseFlag::move) {
    relative_mode_active_ = false;
    relative_anchor_valid_ = false;
  }

  const double normalized_x =
      std::clamp(static_cast<double>(remote_action.m.x), 0.0, 1.0);
  const double normalized_y =
      std::clamp(static_cast<double>(remote_action.m.y), 0.0, 1.0);
  // Keep the coordinate continuous until it reaches Core Graphics. This
  // avoids turning a one-pixel rounding error into a visible offset when the
  // client zooms the remote image. The upper bound remains just inside the
  // display because CG display rectangles use an exclusive right/bottom edge.
  const double max_x = std::nextafter(static_cast<double>(display_info.right),
                                      static_cast<double>(display_info.left));
  const double max_y = std::nextafter(static_cast<double>(display_info.bottom),
                                      static_cast<double>(display_info.top));
  const double mouse_pos_x = std::clamp(
      display_info.left + normalized_x * display_info.width,
      static_cast<double>(display_info.left), max_x);
  const double mouse_pos_y = std::clamp(
      display_info.top + normalized_y * display_info.height,
      static_cast<double>(display_info.top), max_y);
  CGPoint mouse_point = CGPointMake(mouse_pos_x, mouse_pos_y);
  double effective_pos_x = mouse_pos_x;
  double effective_pos_y = mouse_pos_y;
  if (relative_mode_active_) {
    // Inside a relative burst CGEventGetLocation still returns the
    // pre-burst position, so a click positioned there would yank the cursor
    // backwards. The tracked anchor is authoritative while it is valid and
    // fresh — the same rule the move path uses; the live read is only the
    // fallback for clicks arriving after the stream went idle.
    bool anchored = false;
    const auto now = std::chrono::steady_clock::now();
    if (relative_anchor_valid_ &&
        now - relative_anchor_time_ < kRelativeAnchorMaxAge) {
      effective_pos_x = relative_anchor_x_;
      effective_pos_y = relative_anchor_y_;
      anchored = true;
    } else if (const auto cursor = CGEventCreate(nullptr)) {
      const auto current = CGEventGetLocation(cursor);
      CFRelease(cursor);
      effective_pos_x = current.x;
      effective_pos_y = current.y;
    }
    mouse_point = CGPointMake(effective_pos_x, effective_pos_y);
    if (anchored) {
      // The click carries this position, so it becomes part of the tracked
      // state: refresh the anchor instead of letting it go stale.
      relative_anchor_time_ = now;
    }
  }
  const int tracked_mouse_pos_x = static_cast<int>(std::lround(effective_pos_x));
  const int tracked_mouse_pos_y = static_cast<int>(std::lround(effective_pos_y));

  CGEventRef mouse_event = nullptr;
  CGEventType mouse_type;
  CGMouseButton mouse_button;
  int click_state = 1;

  switch (remote_action.m.flag) {
    case MouseFlag::left_down:
      mouse_type = kCGEventLeftMouseDown;
      click_state = BeginClick(left_click_tracker_, tracked_mouse_pos_x,
                               tracked_mouse_pos_y);
      mouse_event = CGEventCreateMouseEvent(NULL, mouse_type, mouse_point,
                                            kCGMouseButtonLeft);
      SetClickState(mouse_event, click_state);
      break;
    case MouseFlag::left_up:
      mouse_type = kCGEventLeftMouseUp;
      click_state = EndClick(left_click_tracker_, tracked_mouse_pos_x,
                             tracked_mouse_pos_y);
      mouse_event = CGEventCreateMouseEvent(NULL, mouse_type, mouse_point,
                                            kCGMouseButtonLeft);
      SetClickState(mouse_event, click_state);
      break;
    case MouseFlag::right_down:
      mouse_type = kCGEventRightMouseDown;
      click_state = BeginClick(right_click_tracker_, tracked_mouse_pos_x,
                               tracked_mouse_pos_y);
      mouse_event = CGEventCreateMouseEvent(NULL, mouse_type, mouse_point,
                                            kCGMouseButtonRight);
      SetClickState(mouse_event, click_state);
      break;
    case MouseFlag::right_up:
      mouse_type = kCGEventRightMouseUp;
      click_state = EndClick(right_click_tracker_, tracked_mouse_pos_x,
                             tracked_mouse_pos_y);
      mouse_event = CGEventCreateMouseEvent(NULL, mouse_type, mouse_point,
                                            kCGMouseButtonRight);
      SetClickState(mouse_event, click_state);
      break;
    case MouseFlag::middle_down:
      mouse_type = kCGEventOtherMouseDown;
      click_state = BeginClick(middle_click_tracker_, tracked_mouse_pos_x,
                               tracked_mouse_pos_y);
      mouse_event = CGEventCreateMouseEvent(NULL, mouse_type, mouse_point,
                                            kCGMouseButtonCenter);
      SetClickState(mouse_event, click_state);
      break;
    case MouseFlag::middle_up:
      mouse_type = kCGEventOtherMouseUp;
      click_state = EndClick(middle_click_tracker_, tracked_mouse_pos_x,
                             tracked_mouse_pos_y);
      mouse_event = CGEventCreateMouseEvent(NULL, mouse_type, mouse_point,
                                            kCGMouseButtonCenter);
      SetClickState(mouse_event, click_state);
      break;
    case MouseFlag::wheel_vertical:
      mouse_event = CGEventCreateScrollWheelEvent(NULL, kCGScrollEventUnitLine,
                                                  2, remote_action.m.s, 0);
      break;
    case MouseFlag::wheel_horizontal:
      mouse_event = CGEventCreateScrollWheelEvent(NULL, kCGScrollEventUnitLine,
                                                  2, 0, remote_action.m.s);
      break;
    default:
      if (left_dragging_) {
        mouse_type = kCGEventLeftMouseDragged;
        mouse_button = kCGMouseButtonLeft;
      } else if (right_dragging_) {
        mouse_type = kCGEventRightMouseDragged;
        mouse_button = kCGMouseButtonRight;
      } else {
        mouse_type = kCGEventMouseMoved;
        mouse_button = kCGMouseButtonLeft;
      }

      mouse_event =
          CGEventCreateMouseEvent(NULL, mouse_type, mouse_point, mouse_button);
      break;
  }

  if (mouse_event) {
    CGEventSetIntegerValueField(mouse_event, kCGEventSourceUserData,
                                kCrossDeskInjectedEvent);
    CGEventPost(kCGHIDEventTap, mouse_event);
    CFRelease(mouse_event);
    const int flag = static_cast<int>(remote_action.m.flag);
    if (flag >= 1 && flag <= 6) {
      const unsigned bit = 1u << ((flag - 1) / 2);
      if (flag & 1) pressed_buttons_ |= bit;
      else pressed_buttons_ &= ~bit;
      left_dragging_ = (pressed_buttons_ & 1) != 0;
      right_dragging_ = (pressed_buttons_ & 2) != 0;
    }
  } else {
    return -1;
  }

  return 0;
}
}  // namespace crossdesk
