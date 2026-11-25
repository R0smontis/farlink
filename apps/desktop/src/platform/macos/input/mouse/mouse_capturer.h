/*
 * @Author: DI JUNKUN
 * @Date: 2026-11-25
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
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

// Scaled device displacement in physical pixels of the controlled display.
using OnMouseDelta = void (*)(double, double, void*);
// Mouse button edges observed by the capture tap. button uses the Slint
// encoding shared with the input pipeline (left 1, right 2, middle 3).
using OnMouseButton = void (*)(int, bool, void*);
using OnGameModeExit = void (*)(void*);

// macOS game-mode capture: a session event tap observing raw mouse deltas
// while the cursor is hidden and frozen. Deltas are read from
// kCGMouseEventDeltaX/Y rather than derived from pointer positions, so they
// never saturate at window or screen edges.
//
// The cursor is locked by warping to its current position and then calling
// CGAssociateMouseAndMouseCursorPosition(false), instead of dissociating
// alone: the warp parks the hardware cursor at the position the user already
// perceives, so re-associating keeps it exactly there with zero jump and the
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
  // into physical pixels of the controlled remote display.
  int Hook(OnMouseDelta on_delta, OnMouseButton on_button,
           OnGameModeExit on_exit, void* user_ptr, double scale_x,
           double scale_y, CGPoint freeze_at);
  // Restores cursor association and visibility; safe without a live Hook.
  int Unhook();

  bool active() const { return active_.load(std::memory_order_acquire); }
  void SetScale(double scale_x, double scale_y);

  // Called from the event tap thread. Deliver* forward to the registered
  // callbacks; they never block on UI state.
  void DeliverDelta(double raw_dx, double raw_dy);
  void DeliverButton(int button, bool down);
  void DeliverExit();
  void ReenableTap();
  bool WithinWarpSuppression(CGEventRef event) const;

  GameModeExitChord exit_chord_;

 private:
  void RunLoopThreadMain();
  void StopRunLoop();
  void FreezeCursorAtCurrentPosition();
  void RestoreCursor();

  std::mutex state_mutex_;
  CFMachPortRef event_tap_ = nullptr;
  CFRunLoopSourceRef run_loop_source_ = nullptr;
  CFRunLoopRef run_loop_ = nullptr;
  std::thread run_loop_thread_;
  std::atomic<bool> active_{false};
  std::atomic<bool> tap_ready_{false};
  std::atomic<bool> tap_failed_{false};

  OnMouseDelta on_delta_ = nullptr;
  OnMouseButton on_button_ = nullptr;
  OnGameModeExit on_exit_ = nullptr;
  void* user_ptr_ = nullptr;
  std::atomic<double> scale_x_{1.0};
  std::atomic<double> scale_y_{1.0};

  CGPoint frozen_point_{};
  CGPoint restore_point_{};
  bool cursor_hidden_ = false;
  bool cursor_dissociated_ = false;
  CGDirectDisplayID hidden_display_ = 0;
  std::chrono::steady_clock::time_point warp_suppress_until_{};
};

}  // namespace crossdesk

#endif
