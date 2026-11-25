/*
 * @Author: Farlink contributors
 * @Date: 2026-11-25
 * Copyright (c) 2026 by Farlink contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef _MOUSE_CAPTURER_H_
#define _MOUSE_CAPTURER_H_

#include <ApplicationServices/ApplicationServices.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

#include "relative_mouse_capture.h"

namespace crossdesk {

// Scaled device displacement in the resolution-independent wire unit:
// fraction of the controlled display's width/height (see remote_action.h).
using OnMouseDelta = void (*)(double, double, void*);
// Mouse button edges observed by the capture tap. button uses the Slint
// encoding shared with the input pipeline (left 1, right 2, middle 3).
using OnMouseButton = void (*)(int, bool, void*);
// Wheel rotation in the units the Slint scroll path already forwards:
// line notches (axis 2 horizontal, axis 1 vertical) and continuous point
// deltas, so the consumer can reproduce the existing scroll conversion.
using OnMouseWheel = void (*)(double, double, double, double, void*);
using OnGameModeExit = void (*)(void*);

// macOS game-mode capture: a session event tap observing mouse deltas while
// the cursor is hidden and frozen. Deltas are read from
// kCGMouseEventDeltaX/Y rather than derived from pointer positions, so they
// never saturate at window or screen edges.
//
// The cursor is locked by warping it to the freeze point chosen by the
// caller (the centre of the streamed video) and then calling
// CGAssociateMouseAndMouseCursorPosition(false), instead of dissociating
// alone: the warp parks the hardware cursor at a position inside the stream
// window, so re-associating keeps it exactly there with zero jump and the
// freeze cannot drift from device motion that was already in flight.
class PlatformMouseCapturer {
 public:
  PlatformMouseCapturer();
  ~PlatformMouseCapturer();

  PlatformMouseCapturer(const PlatformMouseCapturer&) = delete;
  PlatformMouseCapturer& operator=(const PlatformMouseCapturer&) = delete;

  // Starts the capture tap and freezes the cursor. The hardware cursor
  // warps to freeze_at (the logical centre of the streamed video, so scroll
  // and hover events still land inside the stream window) and returns to
  // its original position on Unhook. scale_x/scale_y convert device points
  // into the wire unit (fraction of the controlled display per axis).
  int Hook(OnMouseDelta on_delta, OnMouseButton on_button,
           OnMouseWheel on_wheel, OnGameModeExit on_exit, void* user_ptr,
           double scale_x, double scale_y, CGPoint freeze_at);
  // Restores cursor association and visibility; safe without a live Hook.
  int Unhook();

  bool active() const { return active_.load(std::memory_order_acquire); }
  void SetScale(double scale_x, double scale_y);

  // Called from the event tap thread. Deliver* forward to the registered
  // callbacks; they never block on UI state.
  void DeliverDelta(double raw_dx, double raw_dy);
  void DeliverButton(int button, bool down);
  void DeliverWheel(double line_dx, double line_dy, double point_dx,
                    double point_dy);
  void DeliverExit();
  void ReenableTap();
  // Chord-triggered release on the tap thread itself: flips the tap into
  // pass-through and restores the cursor immediately, so a stalled UI
  // thread can never leave the user with a frozen, hidden, swallowed
  // mouse. The UI-side exit still flows through DeliverExit afterwards.
  void ReleaseOnTapThread();
  bool passthrough() const { return passthrough_.load(std::memory_order_acquire); }
  // Key-event half of the escape chord, factored out of the tap callback so
  // verification can drive it directly: synthetic keyboard events do not
  // flow through macOS session event taps, so no test can post a physical
  // chord. Returns true when the event must be swallowed. On chord
  // completion this runs the tap-thread release itself.
  bool HandleKeyEvent(int vk, bool is_down);

  GameModeExitChord exit_chord_;

 private:
  void RunLoopThreadMain();
  void StopRunLoop();
  void FreezeCursor();
  void RestoreCursor();

  std::mutex state_mutex_;
  CFMachPortRef event_tap_ = nullptr;
  CFRunLoopSourceRef run_loop_source_ = nullptr;
  CFRunLoopRef run_loop_ = nullptr;
  std::thread run_loop_thread_;
  std::atomic<bool> active_{false};
  std::atomic<bool> tap_ready_{false};
  std::atomic<bool> tap_failed_{false};
  // Set under state_mutex_ when shutdown is requested before the run loop
  // starts; the thread checks it before entering CFRunLoopRun.
  bool stop_requested_ = false;

  OnMouseDelta on_delta_ = nullptr;
  OnMouseButton on_button_ = nullptr;
  OnMouseWheel on_wheel_ = nullptr;
  OnGameModeExit on_exit_ = nullptr;
  void* user_ptr_ = nullptr;
  std::atomic<double> scale_x_{1.0};
  std::atomic<double> scale_y_{1.0};

  // Set on the tap thread the moment the escape chord completes: every
  // later event passes through untouched even before the UI reacts.
  std::atomic<bool> passthrough_{false};

  // Cursor state is touched by the Hook/Unhook callers AND by the tap
  // thread on chord release, so these fields are mutex-guarded and the
  // restore paths are idempotent.
  std::mutex cursor_mutex_;
  CGPoint frozen_point_{};
  CGPoint restore_point_{};
  bool cursor_hidden_ = false;
  bool cursor_dissociated_ = false;
  CGDirectDisplayID hidden_display_ = 0;
};

}  // namespace crossdesk

#endif
