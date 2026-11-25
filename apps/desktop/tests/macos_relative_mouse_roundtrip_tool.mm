/*
 * @Author: DI JUNKUN
 * @Date: 2026-11-25
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

// Runtime verification for relative injection: drives the real
// PlatformMouseController with ~1000 relative moves and reads the cursor
// position back through CGEventGetLocation. This physically moves the
// user's cursor; it restores the original position afterwards and completes
// in well under three seconds.

#include "mouse/mouse_controller.h"

#include <ApplicationServices/ApplicationServices.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include "rd_log.h"

namespace {

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

  // Discover the main display bounds from Core Graphics rather than the
  // capture pipeline: the injection path only needs the virtual display
  // rectangle it clamps against.
  const CGDirectDisplayID display = CGMainDisplayID();
  const CGRect bounds = CGDisplayBounds(display);
  crossdesk::DisplayInfo info("Main", static_cast<int>(bounds.origin.x),
                              static_cast<int>(bounds.origin.y),
                              static_cast<int>(bounds.origin.x + bounds.size.width),
                              static_cast<int>(bounds.origin.y + bounds.size.height));

  crossdesk::PlatformMouseController controller;
  if (controller.Init({info}) != 0) {
    std::fprintf(stderr, "controller init failed\n");
    return 1;
  }

  const CGPoint original = CurrentCursorPosition();
  // Start near the centre so the random walk almost never touches the
  // clamped edges; edge behaviour is covered by the unit test instead.
  const CGPoint centre = CGPointMake(
      bounds.origin.x + bounds.size.width / 2,
      bounds.origin.y + bounds.size.height / 2);
  CGWarpMouseCursorPosition(centre);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Deterministic sweep pattern: 10 sweeps of 100 fractional steps each,
  // alternating direction per sweep. Total path length is ~5000px while the
  // walk stays well inside the display, so clamping never interferes and any
  // lost or merged motion directly unbalances the endpoint.
  double expected_dx = 0, expected_dy = 0;
  unsigned state = 0x12345678u;
  constexpr int kMoves = 1000;
  const auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < kMoves; ++i) {
    state = state * 1664525u + 1013904223u;
    const double unit = static_cast<double>((state >> 8) & 0xFFFF) / 65536.0;
    const double magnitude = 1.5 + unit * 6.0;
    const int sweep = i / 100;
    const double sign = (sweep % 2 == 0) ? 1.0 : -1.0;
    const double dx = sign * magnitude;
    const double dy = sign * magnitude * 0.5;
    expected_dx += dx;
    expected_dy += dy;

    crossdesk::RemoteAction action{};
    action.type = crossdesk::ControlType::mouse;
    action.m.x = 0.5f;
    action.m.y = 0.5f;
    action.m.s = 0;
    action.m.flag = crossdesk::MouseFlag::move_relative;
    action.m.dx = static_cast<float>(dx);
    action.m.dy = static_cast<float>(dy);
    controller.SendMouseCommand(action, 0);
  }
  // The final events still need to drain through the HID system. Injected
  // moves do not apply atomically; settling here is what makes the read-back
  // converged rather than mid-burst.
  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  const CGPoint finish = CurrentCursorPosition();
  const double actual_dx = finish.x - centre.x;
  const double actual_dy = finish.y - centre.y;
  const double expected = std::hypot(expected_dx, expected_dy);
  const double error = std::hypot(actual_dx - expected_dx,
                                  actual_dy - expected_dy);
  const double relative_error =
      expected > 1e-6 ? error / expected : error;

  // Restore before reporting so the user's cursor is parked again even if
  // the verdict fails.
  CGWarpMouseCursorPosition(original);
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start);

  std::printf("moves=%d expected=(%.1f,%.1f) actual=(%.1f,%.1f) "
              "error=%.2fpx (%.2f%%) elapsed=%lldms\n",
              kMoves, expected_dx, expected_dy, actual_dx, actual_dy, error,
              relative_error * 100.0, static_cast<long long>(elapsed.count()));
  if (elapsed.count() > 3000) {
    std::fprintf(stderr, "verification exceeded the 3 second budget\n");
    return 1;
  }
  if (relative_error >= 0.05) {
    std::fprintf(stderr, "relative injection error %.2f%% >= 5%%\n",
                 relative_error * 100.0);
    return 1;
  }
  return 0;
}
