/*
 * @Author: Farlink contributors
 * @Date: 2026-11-25
 * Copyright (c) 2026 by Farlink contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef _CAPTURED_MOUSE_QUEUE_H_
#define _CAPTURED_MOUSE_QUEUE_H_

#include <deque>
#include <mutex>
#include <utility>

namespace crossdesk {

struct CapturedMouseEvent {
  enum class Kind { delta, button, wheel, exit };
  Kind kind = Kind::delta;
  double dx = 0;
  double dy = 0;
  int button = 0;
  bool down = false;
  // Wheel only: continuous point deltas kept alongside the notch counts in
  // dx/dy so the consumer can reproduce the Slint scroll conversion.
  double point_dx = 0;
  double point_dy = 0;
};

// Single-producer (event tap thread) single-consumer (UI tick) queue for
// captured game-mode mouse input. The tap thread must never block on UI
// state, so delivery here is a plain append; the UI drains everything in one
// lock acquisition per tick. Deltas are summed losslessly in double precision
// at consumption time, which bounds the send rate by the existing input
// cadence without dropping sub-pixel motion.
class CapturedMouseQueue {
 public:
  void PushDelta(double dx, double dy) {
    std::lock_guard lock(mutex_);
    // Coalesce into the queued tail: the peer cannot apply partial deltas
    // faster than the send cadence, and summing in double is lossless for
    // the sub-pixel fractions the wire carries. A button edge between two
    // deltas must stay ordered, so only a trailing pure delta absorbs.
    if (!events_.empty() && events_.back().kind == CapturedMouseEvent::Kind::delta) {
      events_.back().dx += dx;
      events_.back().dy += dy;
      return;
    }
    // Unbounded growth under a stalled consumer is worse than discarding
    // motion; button edges are never dropped, only delta positions merge.
    while (events_.size() >= kMaxEvents &&
           events_.front().kind == CapturedMouseEvent::Kind::delta) {
      events_.pop_front();
    }
    if (events_.size() >= kMaxEvents) return;
    CapturedMouseEvent event{};
    event.kind = CapturedMouseEvent::Kind::delta;
    event.dx = dx;
    event.dy = dy;
    events_.push_back(event);
  }

  void PushButton(int button, bool down) {
    std::lock_guard lock(mutex_);
    CapturedMouseEvent event{};
    event.kind = CapturedMouseEvent::Kind::button;
    event.button = button;
    event.down = down;
    events_.push_back(event);
  }

  void PushWheel(double line_dx, double line_dy, double point_dx,
                 double point_dy) {
    std::lock_guard lock(mutex_);
    // Wheel events coalesce like deltas: the scroll accumulator integrates
    // continuous motion anyway, and ordering against clicks is preserved
    // because only a trailing wheel absorbs.
    if (!events_.empty() &&
        events_.back().kind == CapturedMouseEvent::Kind::wheel) {
      events_.back().dx += line_dx;
      events_.back().dy += line_dy;
      events_.back().point_dx += point_dx;
      events_.back().point_dy += point_dy;
      return;
    }
    if (events_.size() >= kMaxEvents) return;
    CapturedMouseEvent event{};
    event.kind = CapturedMouseEvent::Kind::wheel;
    event.dx = line_dx;
    event.dy = line_dy;
    event.point_dx = point_dx;
    event.point_dy = point_dy;
    events_.push_back(event);
  }

  void PushExit() {
    std::lock_guard lock(mutex_);
    CapturedMouseEvent event{};
    event.kind = CapturedMouseEvent::Kind::exit;
    events_.push_back(event);
  }

  std::deque<CapturedMouseEvent> Drain() {
    std::lock_guard lock(mutex_);
    return std::exchange(events_, {});
  }

  void Clear() {
    std::lock_guard lock(mutex_);
    events_.clear();
  }

 private:
  static constexpr size_t kMaxEvents = 1024;
  std::mutex mutex_;
  std::deque<CapturedMouseEvent> events_;
};

}  // namespace crossdesk

#endif
