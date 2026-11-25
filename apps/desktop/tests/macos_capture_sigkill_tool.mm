/*
 * @Author: Farlink contributors
 * @Date: 2026-11-25
 * Copyright (c) 2026 by Farlink contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

// Helper process for the SIGKILL capture-cleanup verification.
//
// Capture mode (no arguments): hides and dissociates the cursor, installs a
// swallow-all mouse tap, prints READY and waits. The driver then SIGKILLs
// it. Cleanup of everything the captive held is WindowServer-side and
// documented per-connection: the event tap is a mach port the dead process
// owned, and cursor hide/association are per-application state — all of it
// dies with the connection.
//
// Probe mode (--probe): proves recovery from OUTSIDE the killed process by
// posting a tagged mouse move and confirming a listen-only tap observes it
// — i.e. the dead captive's swallowing tap no longer intercepts — and that
// CGWarpMouseCursorPosition still tracks the posted position, i.e. cursor
// association is functional again.

#include <ApplicationServices/ApplicationServices.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <unistd.h>

namespace {

constexpr int64_t kProbeTag = 0x50524f42454521;

std::atomic<bool> observed{false};

CGEventRef SwallowAll(CGEventTapProxy, CGEventType type, CGEventRef event,
                      void*) {
  if (type == kCGEventTapDisabledByTimeout ||
      type == kCGEventTapDisabledByUserInput) {
    return event;
  }
  return nullptr;
}

CGEventRef ObserveProbe(CGEventTapProxy, CGEventType type, CGEventRef event,
                        void*) {
  if (type == kCGEventTapDisabledByTimeout ||
      type == kCGEventTapDisabledByUserInput) {
    return event;
  }
  if (type == kCGEventMouseMoved &&
      CGEventGetIntegerValueField(event, kCGEventSourceUserData) == kProbeTag) {
    observed.store(true);
  }
  return event;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 2 && std::strcmp(argv[1], "--probe") == 0) {
    if (!AXIsProcessTrusted()) {
      std::fprintf(stderr, "accessibility required\n");
      return 2;
    }
    CFMachPortRef tap = CGEventTapCreate(
        kCGSessionEventTap, kCGHeadInsertEventTap, kCGEventTapOptionListenOnly,
        (1 << kCGEventMouseMoved), ObserveProbe, nullptr);
    if (!tap) {
      std::fprintf(stderr, "probe tap creation failed\n");
      return 1;
    }
    CFRunLoopSourceRef source =
        CFMachPortCreateRunLoopSource(kCFAllocatorDefault, tap, 0);
    std::atomic<CFRunLoopRef> loop{nullptr};
    std::thread observer([&] {
      CFRunLoopRef l = CFRunLoopGetCurrent();
      loop.store(l);
      CFRunLoopAddSource(l, source, kCFRunLoopCommonModes);
      CGEventTapEnable(tap, true);
      CFRunLoopRun();
    });
    while (!loop.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Post a tagged move. If the dead captive's swallow-all tap survived,
    // this event never reaches the listen-only observer.
    CGEventRef move = CGEventCreateMouseEvent(nullptr, kCGEventMouseMoved,
                                              CGPointMake(700, 500),
                                              kCGMouseButtonLeft);
    CGEventSetIntegerValueField(move, kCGEventSourceUserData, kProbeTag);
    CGEventPost(kCGHIDEventTap, move);
    CFRelease(move);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    // Association probe: warp the cursor and read the position back.
    // A system stuck in the captive's dissociated state would not track.
    CGWarpMouseCursorPosition(CGPointMake(701, 501));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CGEventRef posEvent = CGEventCreate(nullptr);
    const CGPoint pos = CGEventGetLocation(posEvent);
    CFRelease(posEvent);
    const bool warp_ok = std::abs(pos.x - 701) < 5 && std::abs(pos.y - 501) < 5;

    CFRunLoopStop(loop.load());
    observer.join();
    CFRelease(source);
    CFRelease(tap);

    const bool pass = observed.load() && warp_ok;
    std::printf("probe events_flowing=%d cursor_tracks_warp=%d %s\n",
                observed.load() ? 1 : 0, warp_ok ? 1 : 0,
                pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
  }

  if (!AXIsProcessTrusted()) {
    std::fprintf(stderr, "accessibility required\n");
    return 2;
  }

  // Capture mode: hide + dissociate + swallow-all tap, then wait to die.
  CGDisplayHideCursor(kCGDirectMainDisplay);
  CGAssociateMouseAndMouseCursorPosition(false);

  const CGEventMask mask =
      (1 << kCGEventMouseMoved) | (1 << kCGEventLeftMouseDown) |
      (1 << kCGEventLeftMouseUp) | (1 << kCGEventRightMouseDown) |
      (1 << kCGEventRightMouseUp);
  CFMachPortRef tap = CGEventTapCreate(
      kCGSessionEventTap, kCGHeadInsertEventTap, kCGEventTapOptionDefault,
      mask, SwallowAll, nullptr);
  if (!tap) {
    std::fprintf(stderr, "tap creation failed\n");
    return 1;
  }
  CFRunLoopSourceRef source =
      CFMachPortCreateRunLoopSource(kCFAllocatorDefault, tap, 0);
  CFRunLoopAddSource(CFRunLoopGetCurrent(), source, kCFRunLoopCommonModes);
  CGEventTapEnable(tap, true);
  std::printf("READY pid=%d\n", getpid());
  std::fflush(stdout);
  CFRunLoopRun();
  return 0;
}
