/*
 * @Author: Farlink contributors
 * @Date: 2026-11-25
 * Copyright (c) 2026 by Farlink contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "mouse_capturer.h"

#include <chrono>

#include <atomic>

#include "../mac_injected_event.h"
#include "rd_log.h"

namespace crossdesk {

namespace {

int SlintButtonFromCg(CGMouseButton button) {
  switch (button) {
    case kCGMouseButtonLeft:
      return 1;
    case kCGMouseButtonRight:
      return 2;
    case kCGMouseButtonCenter:
      return 3;
    default:
      return 0;
  }
}

// Translates the macOS key codes involved in the escape chord into the
// Windows virtual keys GameModeExitChord tracks. Everything else is -1.
int VkFromMacKeyCode(CGKeyCode key_code) {
  switch (key_code) {
    case 0x3B:
      return 0xA2;  // left control
    case 0x3E:
      return 0xA3;  // right control
    case 0x3A:
      return 0xA4;  // left option
    case 0x3D:
      return 0xA5;  // right option
    case 0x05:
      return 'G';
    default:
      return -1;
  }
}

}  // namespace

// The callback is file-static because CGEventTapCreate requires a plain
// function pointer; state lives on the capturer instance.
static CGEventRef MouseCaptureCallback(CGEventTapProxy proxy, CGEventType type,
                                       CGEventRef event, void* user_info) {
  (void)proxy;
  auto* capturer = static_cast<PlatformMouseCapturer*>(user_info);
  if (!capturer) return event;
  // Re-enable a tap the system disabled; game mode has no other way to
  // observe motion while the cursor is frozen.
  if (type == kCGEventTapDisabledByTimeout ||
      type == kCGEventTapDisabledByUserInput) {
    capturer->ReenableTap();
    return event;
  }
  // The escape chord released capture on this very thread: nothing is
  // swallowed any more, even if the UI thread never drains the exit.
  if (capturer->passthrough()) {
    return event;
  }
  // Events this process injected carry the shared tag. Capturing them would
  // feed host output back into the controller on a loopback machine (and in
  // any A-controls-B-controls-C chain), so they pass through untouched like
  // the keyboard capturer already does for its own injection.
  if (CGEventGetIntegerValueField(event, kCGEventSourceUserData) ==
      kCrossDeskInjectedEvent) {
    return event;
  }

  switch (type) {
    case kCGEventMouseMoved:
    case kCGEventLeftMouseDragged:
    case kCGEventRightMouseDragged:
    case kCGEventOtherMouseDragged: {
      // Post-acceleration deltas in points: present on mouse events
      // regardless of cursor position or association, so they never saturate
      // at screen edges.
      const double raw_dx = static_cast<double>(
          CGEventGetIntegerValueField(event, kCGMouseEventDeltaX));
      const double raw_dy = static_cast<double>(
          CGEventGetIntegerValueField(event, kCGMouseEventDeltaY));
      if ((raw_dx != 0 || raw_dy != 0)) {
        capturer->DeliverDelta(raw_dx, raw_dy);
      }
      return nullptr;
    }
    case kCGEventLeftMouseDown:
    case kCGEventRightMouseDown:
    case kCGEventOtherMouseDown:
    case kCGEventLeftMouseUp:
    case kCGEventRightMouseUp:
    case kCGEventOtherMouseUp: {
      const int slint_button = SlintButtonFromCg(static_cast<CGMouseButton>(
          CGEventGetIntegerValueField(event, kCGMouseEventButtonNumber)));
      const bool down = type == kCGEventLeftMouseDown ||
                        type == kCGEventRightMouseDown ||
                        type == kCGEventOtherMouseDown;
      if (slint_button != 0) capturer->DeliverButton(slint_button, down);
      return nullptr;
    }
    case kCGEventScrollWheel: {
      // The wheel cannot rely on hit-testing under a frozen, hidden cursor;
      // deliver it explicitly and never let it reach the local desktop.
      const int64_t vertical = CGEventGetIntegerValueField(
          event, kCGScrollWheelEventDeltaAxis1);
      const int64_t horizontal = CGEventGetIntegerValueField(
          event, kCGScrollWheelEventDeltaAxis2);
      const double pt_vertical = CGEventGetDoubleValueField(
          event, kCGScrollWheelEventPointDeltaAxis1);
      const double pt_horizontal = CGEventGetDoubleValueField(
          event, kCGScrollWheelEventPointDeltaAxis2);
      capturer->DeliverWheel(static_cast<double>(horizontal),
                             static_cast<double>(vertical), pt_horizontal,
                             pt_vertical);
      return nullptr;
    }
    case kCGEventKeyDown:
    case kCGEventKeyUp: {
      const int vk = VkFromMacKeyCode(static_cast<CGKeyCode>(
          CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode)));
      if (vk >= 0 && capturer->HandleKeyEvent(vk, type == kCGEventKeyDown)) {
        return nullptr;
      }
      return event;
    }
    case kCGEventFlagsChanged: {
      const int vk = VkFromMacKeyCode(static_cast<CGKeyCode>(
          CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode)));
      if (vk == 0xA2 || vk == 0xA3 || vk == 0xA4 || vk == 0xA5) {
        const CGEventFlags flags = CGEventGetFlags(event);
        const bool down = (vk == 0xA2 || vk == 0xA3)
                              ? (flags & kCGEventFlagMaskControl) != 0
                              : (flags & kCGEventFlagMaskAlternate) != 0;
        capturer->exit_chord_.Update(vk, down);
      }
      return event;
    }
    default:
      return event;
  }
}

PlatformMouseCapturer::PlatformMouseCapturer() = default;

PlatformMouseCapturer::~PlatformMouseCapturer() { Unhook(); }

void PlatformMouseCapturer::SetScale(double scale_x, double scale_y) {
  scale_x_.store(scale_x, std::memory_order_release);
  scale_y_.store(scale_y, std::memory_order_release);
}

void PlatformMouseCapturer::DeliverDelta(double raw_dx, double raw_dy) {
  if (!on_delta_) return;
  const double sx = scale_x_.load(std::memory_order_acquire);
  const double sy = scale_y_.load(std::memory_order_acquire);
  double dx = 0, dy = 0;
  if (!RelativeDeltaScale::Convert(raw_dx, raw_dy, sx, sy, dx, dy)) return;
  on_delta_(dx, dy, user_ptr_);
}

void PlatformMouseCapturer::DeliverButton(int button, bool down) {
  if (on_button_) on_button_(button, down, user_ptr_);
}

void PlatformMouseCapturer::DeliverWheel(double line_dx, double line_dy,
                                         double point_dx, double point_dy) {
  if (on_wheel_) on_wheel_(line_dx, line_dy, point_dx, point_dy, user_ptr_);
}

void PlatformMouseCapturer::DeliverExit() {
  if (on_exit_) on_exit_(user_ptr_);
}

bool PlatformMouseCapturer::HandleKeyEvent(int vk, bool is_down) {
  // Typing continues through the keyboard capturer's own tap, which forwards
  // and suppresses keys independently; the escape chord only needs to be
  // observed here. But the chord key itself is local: the G that completes
  // Ctrl+Alt+G, its auto-repeats and its matching key-up must never reach
  // the peer through the keyboard path.
  if (exit_chord_.SwallowsKey(vk)) {
    exit_chord_.Update(vk, is_down);
    return true;
  }
  if (exit_chord_.Update(vk, is_down)) {
    // Release capture synchronously on the calling (tap) thread before
    // anything else: passthrough plus cursor restore cannot wait for the UI
    // thread, which may be busy or hung. The queued exit is only for
    // UI-side state cleanup.
    ReleaseOnTapThread();
    DeliverExit();
    // The key-down that completed the chord is swallowed as well.
    return true;
  }
  return false;
}

void PlatformMouseCapturer::ReenableTap() {
  std::lock_guard lock(state_mutex_);
  if (event_tap_) CGEventTapEnable(event_tap_, true);
}

void PlatformMouseCapturer::FreezeCursor() {
  std::lock_guard lock(cursor_mutex_);
  CGEventRef current = CGEventCreate(nullptr);
  if (!current) return;
  restore_point_ = CGEventGetLocation(current);
  CFRelease(current);
  // Park the hardware cursor at the freeze location before dissociating.
  // Warp-then-freeze keeps the re-associated position continuous; pure
  // dissociation with no warp can drift by device motion already in flight.
  CGWarpMouseCursorPosition(frozen_point_);
  // Cursor hiding is per-display: hide on the display the freeze point sits
  // on. Falling back to the main display mirrors what single-display code
  // paths do everywhere else in the system.
  if (!cursor_hidden_) {
    CGDirectDisplayID displays[8] = {};
    uint32_t display_count = 0;
    hidden_display_ = kCGDirectMainDisplay;
    if (CGGetDisplaysWithPoint(frozen_point_, 8, displays, &display_count) ==
            kCGErrorSuccess &&
        display_count > 0) {
      hidden_display_ = displays[0];
    }
    CGDisplayHideCursor(hidden_display_);
    cursor_hidden_ = true;
  }
  if (CGAssociateMouseAndMouseCursorPosition(false) == kCGErrorSuccess) {
    cursor_dissociated_ = true;
  }
}

void PlatformMouseCapturer::RestoreCursor() {
  std::lock_guard lock(cursor_mutex_);
  // CGDisplayHideCursor is refcounted per display, so the hidden flag is
  // what keeps paired hide/show balanced when the tap thread and Unhook
  // both run this path.
  if (cursor_dissociated_) {
    // Return the hardware cursor to where the user originally grabbed game
    // mode before re-associating; without this warp the re-associated
    // cursor resumes at the freeze point, which is not where the user left
    // it.
    CGWarpMouseCursorPosition(restore_point_);
    CGAssociateMouseAndMouseCursorPosition(true);
    cursor_dissociated_ = false;
  }
  if (cursor_hidden_) {
    CGDisplayShowCursor(hidden_display_);
    cursor_hidden_ = false;
  }
}

void PlatformMouseCapturer::ReleaseOnTapThread() {
  // Idempotent: chord auto-repeat or a late Unhook can interleave with
  // this in any order and the outcome is the same single restore.
  passthrough_.store(true, std::memory_order_release);
  RestoreCursor();
}

int PlatformMouseCapturer::Hook(OnMouseDelta on_delta, OnMouseButton on_button,
                                OnMouseWheel on_wheel, OnGameModeExit on_exit,
                                void* user_ptr, double scale_x, double scale_y,
                                CGPoint freeze_at) {
  // Returning 0 must always mean the tap is live and swallowing. A stale
  // capture — pass-through after the escape chord, or a half-joined run
  // loop — is torn down here rather than reported as success, because the
  // caller wires its sender thread on the assumption that events are
  // captured.
  if (active_.load(std::memory_order_acquire) ||
      run_loop_thread_.joinable()) {
    Unhook();
  }
  if (!std::isfinite(scale_x) || !std::isfinite(scale_y) || scale_x <= 0 ||
      scale_y <= 0) {
    LOG_WARN("Mouse capturer rejected invalid scale {}x{}", scale_x, scale_y);
    return -1;
  }
  on_delta_ = on_delta;
  on_button_ = on_button;
  on_wheel_ = on_wheel;
  on_exit_ = on_exit;
  user_ptr_ = user_ptr;
  SetScale(scale_x, scale_y);
  exit_chord_.Reset();
  // Reset the outcome bits before starting the thread: a previous failed
  // start must not poison this attempt's wait loop.
  tap_ready_.store(false, std::memory_order_release);
  tap_failed_.store(false, std::memory_order_release);
  {
    std::lock_guard lock(state_mutex_);
    stop_requested_ = false;
  }

  passthrough_.store(false, std::memory_order_release);
  {
    std::lock_guard lock(cursor_mutex_);
    frozen_point_ = freeze_at;
  }
  FreezeCursor();
  run_loop_thread_ = std::thread([this] { RunLoopThreadMain(); });

  // Hook's caller relies on capture being live once control returns. The run
  // loop thread publishes the outcome through tap_ready_/tap_failed_.
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
  while (!tap_ready_.load(std::memory_order_acquire) &&
         !tap_failed_.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  if (!tap_ready_.load(std::memory_order_acquire)) {
    LOG_ERROR("Mouse capture tap failed to start");
    {
      std::lock_guard lock(state_mutex_);
      stop_requested_ = true;
    }
    StopRunLoop();
    if (run_loop_thread_.joinable()) run_loop_thread_.join();
    RestoreCursor();
    tap_failed_.store(false, std::memory_order_release);
    return -1;
  }
  active_.store(true, std::memory_order_release);
  return 0;
}

void PlatformMouseCapturer::RunLoopThreadMain() {
  const CGEventMask mask =
      (1 << kCGEventMouseMoved) | (1 << kCGEventLeftMouseDragged) |
      (1 << kCGEventRightMouseDragged) | (1 << kCGEventOtherMouseDragged) |
      (1 << kCGEventLeftMouseDown) | (1 << kCGEventLeftMouseUp) |
      (1 << kCGEventRightMouseDown) | (1 << kCGEventRightMouseUp) |
      (1 << kCGEventOtherMouseDown) | (1 << kCGEventOtherMouseUp) |
      (1 << kCGEventScrollWheel) | (1 << kCGEventKeyDown) |
      (1 << kCGEventKeyUp) | (1 << kCGEventFlagsChanged);
  CFMachPortRef tap = CGEventTapCreate(
      kCGSessionEventTap, kCGHeadInsertEventTap, kCGEventTapOptionDefault,
      mask, MouseCaptureCallback, this);
  if (!tap) {
    LOG_ERROR("Mouse capturer CGEventTapCreate failed");
    tap_failed_.store(true, std::memory_order_release);
    return;
  }
  CFRunLoopSourceRef source =
      CFMachPortCreateRunLoopSource(kCFAllocatorDefault, tap, 0);
  if (!source) {
    CFRelease(tap);
    tap_failed_.store(true, std::memory_order_release);
    return;
  }
  CFRunLoopRef run_loop = CFRunLoopGetCurrent();
  {
    std::lock_guard lock(state_mutex_);
    event_tap_ = tap;
    run_loop_source_ = source;
    run_loop_ = run_loop;
    if (stop_requested_) {
      // The start timed out before this thread finished setting up. Skip
      // the run loop entirely so the joiner is never stuck behind a loop
      // that was never stopped.
      CFRunLoopRemoveSource(run_loop, source, kCFRunLoopCommonModes);
      CFRelease(source);
      CFRelease(tap);
      run_loop_source_ = nullptr;
      event_tap_ = nullptr;
      run_loop_ = nullptr;
      tap_failed_.store(true, std::memory_order_release);
      return;
    }
  }
  CFRunLoopAddSource(run_loop, source, kCFRunLoopCommonModes);
  CGEventTapEnable(tap, true);
  tap_ready_.store(true, std::memory_order_release);
  CFRunLoopRun();
  // Teardown on the same thread that owned the run loop source.
  CGEventTapEnable(tap, false);
  CFRunLoopRemoveSource(run_loop, source, kCFRunLoopCommonModes);
  {
    std::lock_guard lock(state_mutex_);
    CFRelease(source);
    CFRelease(tap);
    run_loop_source_ = nullptr;
    event_tap_ = nullptr;
    run_loop_ = nullptr;
  }
}

void PlatformMouseCapturer::StopRunLoop() {
  CFRunLoopRef run_loop;
  {
    std::lock_guard lock(state_mutex_);
    stop_requested_ = true;
    run_loop = run_loop_;
  }
  if (run_loop) {
    CFRunLoopStop(run_loop);
    CFRunLoopWakeUp(run_loop);
  }
}

int PlatformMouseCapturer::Unhook() {
  const bool was_active = active_.load(std::memory_order_acquire) ||
                          run_loop_thread_.joinable();
  StopRunLoop();
  if (run_loop_thread_.joinable()) run_loop_thread_.join();
  RestoreCursor();
  active_.store(false, std::memory_order_release);
  tap_ready_.store(false, std::memory_order_release);
  tap_failed_.store(false, std::memory_order_release);
  on_delta_ = nullptr;
  on_button_ = nullptr;
  on_wheel_ = nullptr;
  on_exit_ = nullptr;
  user_ptr_ = nullptr;
  exit_chord_.Reset();
  if (was_active) LOG_INFO("Stop mouse capturer");
  return 0;
}

}  // namespace crossdesk
