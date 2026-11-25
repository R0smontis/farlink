/*
 * @Author: DI JUNKUN
 * @Date: 2026-11-25
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _CAPTURED_MOUSE_QUEUE_H_
#define _CAPTURED_MOUSE_QUEUE_H_

#include <deque>
#include <mutex>
#include <utility>

namespace crossdesk {

struct CapturedMouseEvent {
  enum class Kind { delta, button, exit };
  Kind kind = Kind::delta;
  double dx = 0;
  double dy = 0;
  int button = 0;
  bool down = false;
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
