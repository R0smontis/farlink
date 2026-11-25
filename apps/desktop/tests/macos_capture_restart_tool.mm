/*
 * @Author: Farlink contributors
 * @Date: 2026-11-25
 * Copyright (c) 2026 by Farlink contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

// Runtime verification for capture restart after the escape chord. Repeats
// 50 times, with no UI tick anywhere in the loop:
//   hook the real capturer -> verify posted moves are swallowed ->
//   complete the chord through HandleKeyEvent (pass-through begins, exactly
//   as on the tap thread) -> immediately re-Hook the way the fixed
//   StartMouseCapturer does -> verify the tap is live again: moves are
//   swallowed and the capturer reports no pass-through.
//
// Before the fix this cycle broke two ways: StartMouseCapturer took the
// active() early-return against a pass-through tap (game mode "on" with
// events flowing freely and no sender thread), and Hook returned 0 without
// hooking when the run loop thread was still joinable. Posted mouse moves
// reach head-insert session taps, so the observer created BEFORE the
// capturer sits behind it in the chain and sees exactly what survives the
// swallow.

#include "mouse/mouse_capturer.h"

#include <ApplicationServices/ApplicationServices.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

namespace {

std::atomic<int> exits{0};
std::atomic<int> observed_moves{0};

void OnDelta(double, double, void*) {}
void OnButton(int, bool, void*) {}
void OnWheel(double, double, double, double, void*) {}
void OnExit(void*) { exits.fetch_add(1); }

// Created BEFORE the capturer hooks, so the capturer's head-insert tap sits
// ahead of it in the chain: events the capturer swallows never arrive here.
CGEventRef Observer(CGEventTapProxy, CGEventType type, CGEventRef event,
                    void*) {
  if (type == kCGEventMouseMoved) observed_moves.fetch_add(1);
  return event;
}

void PostMove(double x, double y) {
  CGEventRef move =
      CGEventCreateMouseEvent(nullptr, kCGEventMouseMoved, CGPointMake(x, y),
                              kCGMouseButtonLeft);
  CGEventPost(kCGHIDEventTap, move);
  CFRelease(move);
}

bool MovesSwallowed() {
  observed_moves.store(0);
  PostMove(710, 510);
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  return observed_moves.load() == 0;
}

}  // namespace

int main() {
  if (!AXIsProcessTrusted()) {
    std::fprintf(stderr, "accessibility required\n");
    return 2;
  }

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

  constexpr int kCycles = 50;
  int failures = 0;
  int first_failure = -1;
  crossdesk::PlatformMouseCapturer capturer;

  for (int cycle = 0; cycle < kCycles; ++cycle) {
    if (capturer.Hook(OnDelta, OnButton, OnWheel, OnExit, nullptr, 1.0, 1.0,
                      CGPointMake(700, 500)) != 0) {
      std::fprintf(stderr, "cycle %d: hook failed\n", cycle);
      ++failures;
      if (first_failure < 0) first_failure = cycle;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    const bool swallowed_live = MovesSwallowed();

    // Complete the chord exactly the way the tap thread does.
    capturer.exit_chord_.Update(0xA2, true);
    capturer.exit_chord_.Update(0xA4, true);
    capturer.HandleKeyEvent('G', true);
    capturer.HandleKeyEvent('G', false);
    capturer.exit_chord_.Update(0xA4, false);
    capturer.exit_chord_.Update(0xA2, false);

    if (!capturer.passthrough()) {
      std::fprintf(stderr, "cycle %d: chord did not start pass-through\n",
                   cycle);
      ++failures;
      if (first_failure < 0) first_failure = cycle;
      capturer.Unhook();
      break;
    }

    // IMMEDIATE re-hook, no settling: this is the start-after-chord path.
    if (capturer.Hook(OnDelta, OnButton, OnWheel, OnExit, nullptr, 1.0, 1.0,
                      CGPointMake(700, 500)) != 0) {
      std::fprintf(stderr, "cycle %d: re-hook after chord failed\n", cycle);
      ++failures;
      if (first_failure < 0) first_failure = cycle;
      capturer.Unhook();
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    const bool pass_through_after = capturer.passthrough();
    const bool swallowed_again = MovesSwallowed();

    if (!swallowed_live || pass_through_after || !swallowed_again) {
      std::fprintf(stderr,
                   "cycle %d: swallowed_live=%d pass_through_after=%d "
                   "swallowed_again=%d\n",
                   cycle, swallowed_live ? 1 : 0, pass_through_after ? 1 : 0,
                   swallowed_again ? 1 : 0);
      ++failures;
      if (first_failure < 0) first_failure = cycle;
    }
    capturer.Unhook();
  }

  CFRunLoopStop(observer_loop.load());
  observer_thread.join();
  CFRelease(observer_source);
  CFRelease(observer_tap);

  std::printf("cycles=%d failures=%d first_failure=%d exits=%d\n", kCycles,
              failures, first_failure, exits.load());
  const bool pass = failures == 0;
  std::printf(pass ? "PASS\n" : "FAIL\n");
  return pass ? 0 : 1;
}
