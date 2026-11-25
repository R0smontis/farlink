/*
 * @Author: Farlink contributors
 * @Date: 2026-11-25
 * Copyright (c) 2026 by Farlink contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

// Runtime verification for display-edge behaviour: drives the real
// PlatformMouseController with relative moves that push the cursor hard
// into the right display edge, while a listen-only event tap observes the
// injected events. PASS requires: the cursor position pins at the edge,
// injected events keep carrying non-zero kCGMouseEventDeltaX after pinning,
// and the pinned delta sum matches the unclamped input total. The user's
// cursor is restored afterwards.

#include "mouse/mouse_controller.h"

#include <ApplicationServices/ApplicationServices.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

#include "../src/platform/macos/input/mac_injected_event.h"
#include "rd_log.h"

namespace {

std::atomic<double> observed_delta_sum{0};
std::atomic<int> observed_pinned_events{0};
std::atomic<int> observed_total_events{0};
std::atomic<double> edge_x{0};

CGEventRef EdgeObserverCallback(CGEventTapProxy proxy, CGEventType type,
                                CGEventRef event, void* user_info) {
  (void)proxy;
  (void)user_info;
  if (type != kCGEventMouseMoved && type != kCGEventLeftMouseDragged &&
      type != kCGEventRightMouseDragged && type != kCGEventOtherMouseDragged) {
    return event;
  }
  // Only our own injected events count; anything else in the session is
  // noise from the physical mouse.
  if (CGEventGetIntegerValueField(event, kCGEventSourceUserData) !=
      crossdesk::kCrossDeskInjectedEvent) {
    return event;
  }
  const double dx = static_cast<double>(
      CGEventGetIntegerValueField(event, kCGMouseEventDeltaX));
  const double x = CGEventGetLocation(event).x;
  observed_delta_sum.store(observed_delta_sum.load() + dx);
  observed_total_events.fetch_add(1);
  // Pinned = the event's own position already sits at the edge while the
  // delta says keep going.
  if (x >= edge_x.load() - 1.0 && dx > 0) {
    observed_pinned_events.fetch_add(1);
  }
  return event;
}

CGPoint CurrentCursorPosition() {
  const auto event = CGEventCreate(nullptr);
  if (!event) return CGPointMake(-1, -1);
  const auto point = CGEventGetLocation(event);
  CFRelease(event);
  return point;
}

}  // namespace

int main() {
  if (!AXIsProcessTrusted()) {
    std::fprintf(stderr,
                 "Accessibility permission is required to inject input\n");
    return 2;
  }

  const CGDirectDisplayID display = CGMainDisplayID();
  const CGRect bounds = CGDisplayBounds(display);
  // Right edge in Core Graphics is exclusive for clamping purposes.
  edge_x.store(bounds.origin.x + bounds.size.width);
  crossdesk::DisplayInfo info("Main", static_cast<int>(bounds.origin.x),
                              static_cast<int>(bounds.origin.y),
                              static_cast<int>(bounds.origin.x + bounds.size.width),
                              static_cast<int>(bounds.origin.y + bounds.size.height));

  crossdesk::PlatformMouseController controller;
  if (controller.Init({info}) != 0) {
    std::fprintf(stderr, "controller init failed\n");
    return 1;
  }

  // Listen-only tap on the same thread is not possible while injecting
  // synchronously, so observe from a dedicated run-loop thread.
  CFMachPortRef tap = CGEventTapCreate(
      kCGSessionEventTap, kCGHeadInsertEventTap, kCGEventTapOptionListenOnly,
      (1 << kCGEventMouseMoved) | (1 << kCGEventLeftMouseDragged) |
          (1 << kCGEventRightMouseDragged) | (1 << kCGEventOtherMouseDragged),
      EdgeObserverCallback, nullptr);
  if (!tap) {
    std::fprintf(stderr, "listen-only tap creation failed\n");
    return 1;
  }
  CFRunLoopSourceRef source =
      CFMachPortCreateRunLoopSource(kCFAllocatorDefault, tap, 0);
  std::atomic<CFRunLoopRef> observer_loop{nullptr};
  std::thread observer([&] {
    CFRunLoopRef loop = CFRunLoopGetCurrent();
    observer_loop.store(loop);
    CFRunLoopAddSource(loop, source, kCFRunLoopCommonModes);
    CGEventTapEnable(tap, true);
    CFRunLoopRun();
  });
  while (!observer_loop.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  const CGPoint original = CurrentCursorPosition();
  // Start near the right edge minus a short distance so only a few moves
  // are needed before pinning; the bulk of the burst then runs pinned.
  const CGPoint start = CGPointMake(bounds.origin.x + bounds.size.width - 200,
                                    bounds.origin.y + bounds.size.height / 2);
  CGWarpMouseCursorPosition(start);
  std::this_thread::sleep_for(std::chrono::milliseconds(150));

  // 200 moves of +20pt each in wire fractions: 4000pt total against a
  // remaining 200pt of runway, so ~95% of the burst executes pinned at the
  // edge. dx on the wire is a fraction of the display width now, so the
  // step size divides by the width in points.
  constexpr int kMoves = 200;
  constexpr double kStepPt = 20.0;
  const double width_pt = bounds.size.width;
  const double step_fraction = kStepPt / width_pt;
  for (int i = 0; i < kMoves; ++i) {
    crossdesk::RemoteAction action{};
    action.type = crossdesk::ControlType::mouse;
    action.m.x = 0.5f;
    action.m.y = 0.5f;
    action.m.s = 0;
    action.m.flag = crossdesk::MouseFlag::move_relative;
    action.m.dx = static_cast<float>(step_fraction);
    action.m.dy = 0;
    controller.SendMouseCommand(action, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  const CGPoint finish = CurrentCursorPosition();
  CGWarpMouseCursorPosition(original);

  CFRunLoopStop(observer_loop.load());
  observer.join();
  CFRelease(source);
  CFRelease(tap);

  // The unclamped input total in points is exactly moves x step: the wire
  // fraction and the display width cancel by construction.
  const double total_expected = kMoves * kStepPt;
  const double observed = observed_delta_sum.load();
  const int pinned = observed_pinned_events.load();
  const int total_events = observed_total_events.load();
  const double pinned_x = finish.x;
  const double right_inclusive = bounds.origin.x + bounds.size.width - 1.0;

  std::printf("moves=%d observed_events=%d pinned_events=%d "
              "delta_sum=%.0f/%.0f final_x=%.1f edge_x=%.1f\n",
              kMoves, total_events, pinned, observed, total_expected,
              pinned_x, right_inclusive);

  bool pass = true;
  // Position pinned at the right edge (within clamping tolerance).
  if (pinned_x < right_inclusive - 2.0) {
    std::fprintf(stderr, "FAIL: cursor did not pin at the right edge\n");
    pass = false;
  }
  // The listen-only tap must have seen the burst at all.
  if (total_events < kMoves / 2) {
    std::fprintf(stderr, "FAIL: observer saw too few injected events\n");
    pass = false;
  }
  // Non-zero deltas while pinned: the core property under test.
  if (pinned < kMoves / 2) {
    std::fprintf(stderr,
                 "FAIL: pinned events with non-zero delta are missing\n");
    pass = false;
  }
  // Sum of deltas equals the unclamped input total within rounding.
  if (std::fabs(observed - total_expected) > kMoves * 0.5) {
    std::fprintf(stderr,
                 "FAIL: delta sum drifted from the unclamped total\n");
    pass = false;
  }
  std::printf(pass ? "PASS\n" : "FAIL\n");
  return pass ? 0 : 1;
}
