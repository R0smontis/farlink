/*
 * @Author: Farlink contributors
 * @Date: 2026-11-25
 * Copyright (c) 2026 by Farlink contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

// Runtime verification for tap-thread chord release: hooks the real
// PlatformMouseCapturer, drives the Ctrl+Alt+G chord through the exact code
// the tap callback runs, and asserts — WITHOUT draining the exit queue —
// that afterwards pass-through is active, posted moves flow past the
// capturer to the rest of the session, and the cursor is visible and
// associated again. This is the stalled-UI scenario: the queue is never
// drained, so everything that happens must have happened on the thread that
// observed the chord.
//
// macOS facts that shape the design, each verified by probe:
//   1. Synthetic keyboard events never reach session event taps at all, so
//      no test can post a physical chord. HandleKeyEvent is the factored key
//      half of the tap callback; driving it exercises the same code
//      including the synchronous release.
//   2. Posted mouse events reach head-insert session taps but not
//      tail-append ones, and head-insert taps order newest-first: an
//      observer created BEFORE the capturer therefore sits behind it in the
//      chain and sees exactly what survives the capturer — nothing while it
//      swallows, everything once it passes through.
//   3. Posted moves reposition the cursor even while it is dissociated, so
//      cursor position cannot evidence swallowing; event flow through the
//      tap chain can.

#include "mouse/mouse_capturer.h"

#include <ApplicationServices/ApplicationServices.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

namespace {

std::atomic<int> exits{0};
std::atomic<int> observed_moves{0};

void OnDelta(double, double, void*) {}
void OnButton(int, bool, void*) {}
void OnWheel(double, double, double, double, void*) {}
void OnExit(void*) { exits.fetch_add(1); }

// Listen-only observer. It is created BEFORE the capturer hooks, so the
// capturer's head-insert tap sits ahead of it: events the capturer swallows
// never arrive here, events it passes through do.
CGEventRef Observer(CGEventTapProxy, CGEventType type, CGEventRef event,
                    void*) {
  if (type == kCGEventMouseMoved) observed_moves.fetch_add(1);
  return event;
}

CGPoint CurrentCursorPosition() {
  CGEventRef event = CGEventCreate(nullptr);
  const CGPoint pos = CGEventGetLocation(event);
  CFRelease(event);
  return pos;
}

void PostMove(double x, double y) {
  CGEventRef move =
      CGEventCreateMouseEvent(nullptr, kCGEventMouseMoved, CGPointMake(x, y),
                              kCGMouseButtonLeft);
  CGEventPost(kCGHIDEventTap, move);
  CFRelease(move);
}

}  // namespace

int main() {
  if (!AXIsProcessTrusted()) {
    std::fprintf(stderr, "accessibility required\n");
    return 2;
  }

  // Observer FIRST — see the design notes above.
  CFMachPortRef observer_tap = CGEventTapCreate(
      kCGSessionEventTap, kCGHeadInsertEventTap, kCGEventTapOptionListenOnly,
      (1 << kCGEventMouseMoved), Observer, nullptr);
  if (!observer_tap) {
    std::fprintf(stderr, "observer tap creation failed\n");
    return 1;
  }
  CFRunLoopSourceRef observer_source =
      CFMachPortCreateRunLoopSource(kCFAllocatorDefault, observer_tap, 0);
  std::atomic<CFRunLoopRef> observer_loop{nullptr};
  std::thread observer_thread([&] {
    CFRunLoopRef loop = CFRunLoopGetCurrent();
    observer_loop.store(loop);
    CFRunLoopAddSource(loop, observer_source, kCFRunLoopCommonModes);
    CGEventTapEnable(observer_tap, true);
    CFRunLoopRun();
  });
  while (!observer_loop.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  crossdesk::PlatformMouseCapturer capturer;
  if (capturer.Hook(OnDelta, OnButton, OnWheel, OnExit, nullptr, 1.0, 1.0,
                    CGPointMake(700, 500)) != 0) {
    std::fprintf(stderr, "hook failed\n");
    return 1;
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(150));

  // Negative control: while captured, posted moves are swallowed and the
  // observer behind the capturer must see nothing.
  observed_moves.store(0);
  PostMove(900, 700);
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  const bool swallowed_during_capture = observed_moves.load() == 0;

  // Drive the chord through the capturer's key path directly: modifiers in
  // Windows-vk space exactly as the flagsChanged branch decodes them, then
  // the completing G through HandleKeyEvent.
  capturer.exit_chord_.Update(0xA2, true);  // left control down
  capturer.exit_chord_.Update(0xA4, true);  // left alt down
  const bool completing_swallowed = capturer.HandleKeyEvent('G', true);
  capturer.HandleKeyEvent('G', false);
  capturer.exit_chord_.Update(0xA4, false);
  capturer.exit_chord_.Update(0xA2, false);

  // The queue is deliberately never drained: everything asserted below had
  // to happen synchronously when the chord completed.
  const bool pass_through = capturer.passthrough();

  // Pass-through: a posted move must now flow past the capturer to the
  // observer behind it.
  observed_moves.store(0);
  PostMove(720, 520);
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  const bool events_flowing = observed_moves.load() > 0;

  // Cursor re-associated: an explicit warp must track.
  CGWarpMouseCursorPosition(CGPointMake(730, 530));
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  const CGPoint warped_pos = CurrentCursorPosition();
  const bool warp_tracks =
      std::abs(warped_pos.x - 730) < 5 && std::abs(warped_pos.y - 530) < 5;

  capturer.Unhook();
  CFRunLoopStop(observer_loop.load());
  observer_thread.join();
  CFRelease(observer_source);
  CFRelease(observer_tap);

  std::printf("passthrough=%d swallowed_during_capture=%d events_flowing=%d "
              "warp_tracks=%d exit_queued=%d chord_swallowed=%d\n",
              pass_through ? 1 : 0, swallowed_during_capture ? 1 : 0,
              events_flowing ? 1 : 0, warp_tracks ? 1 : 0, exits.load(),
              completing_swallowed ? 1 : 0);
  const bool pass = pass_through && swallowed_during_capture &&
                    events_flowing && warp_tracks && exits.load() == 1 &&
                    completing_swallowed;
  std::printf(pass ? "PASS\n" : "FAIL\n");
  return pass ? 0 : 1;
}
