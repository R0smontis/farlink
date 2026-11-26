/*
 * @Author: Farlink contributors
 * @Date: 2026-11-27
 * Copyright (c) 2026 by Farlink contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef _SCREEN_LOCK_MONITOR_H_
#define _SCREEN_LOCK_MONITOR_H_

#include <chrono>
#include <functional>

#include "screen_lock.h"

namespace crossdesk {

// Edge-detecting poller around QueryScreenLock(). The platform query can be
// expensive (Linux performs two blocking DBus roundtrips), so the monitor
// throttles OS calls to a configurable interval; callers may invoke Poll()
// as often as they like.
//
// State machine:
//   - locked/unlocked edges are detected only across consecutive known
//     states. An unknown sample never generates an edge and never moves the
//     last-known state, so a transient query failure fails open: capture
//     keeps forwarding, which is correct because a genuinely locked host is
//     reported by every platform implementation as a stable `locked`, not as
//     flickering `unknown`.
//   - The first known sample after construction or after unknown samples
//     establishes the baseline without generating an edge. Only a transition
//     between two consecutive known-and-different states is an edge.
//
// Single-threaded contract: Poll() must run on one thread (in production,
// the device-interaction tick). Clock and query function are injectable so
// unit tests are deterministic and never touch the OS.
class ScreenLockMonitor {
 public:
  using Clock = std::chrono::steady_clock;
  using QueryFunc = std::function<ScreenLockState()>;

  struct Transition {
    // True on the poll that observes unlocked -> locked.
    bool became_locked = false;
    // True exactly once on the first poll that observes locked -> unlocked.
    // The caller treats this as the force-IDR trigger; it is an edge, never
    // a level, so a slow consumer cannot double-fire it.
    bool became_unlocked = false;
  };

  explicit ScreenLockMonitor(
      std::chrono::milliseconds interval = std::chrono::milliseconds(1000),
      QueryFunc query = &QueryScreenLock)
      : interval_(interval), query_(std::move(query)) {}
  Transition Poll(Clock::time_point now = Clock::now()) {
    Transition result;
    // queried_once_ is separate from the timestamp: the zero time_point is a
    // legitimate clock reading, so it cannot double as the never-queried
    // sentinel.
    if (queried_once_ && now - last_query_ < interval_) return result;
    queried_once_ = true;
    last_query_ = now;
    const ScreenLockState state = query_ ? query_() : QueryScreenLock();
    if (state == ScreenLockState::unknown) return result;
    if (last_known_ != ScreenLockState::unknown && state != last_known_) {
      result.became_locked = state == ScreenLockState::locked;
      result.became_unlocked = state == ScreenLockState::unlocked;
    }
    last_known_ = state;
    return result;
  }

  // Last known state. `unknown` before the first successful query and never
  // regresses to unknown afterwards: unknown samples mean "no information",
  // so the last definite answer is the best available one.
  bool IsLocked() const { return last_known_ == ScreenLockState::locked; }
  ScreenLockState LastKnownState() const { return last_known_; }

  // Reset the throttle so the next Poll() queries immediately, e.g. when the
  // first controller connects and the host needs a fresh baseline. Edges are
  // still computed against the last known state; only the clock is forgiven.
  void ResetThrottle() { queried_once_ = false; }

 private:
  std::chrono::milliseconds interval_;
  QueryFunc query_;
  bool queried_once_ = false;
  Clock::time_point last_query_{};
  ScreenLockState last_known_ = ScreenLockState::unknown;
};

}  // namespace crossdesk

#endif
