/*
 * @Author: DI JUNKUN
 * @Date: 2026-09-26
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _CONNECTION_LATENCY_H_
#define _CONNECTION_LATENCY_H_

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cmath>
#include <mutex>
#include <optional>

namespace crossdesk::gui_detail {

// Presentation-only smoothing. The transport's raw RTT remains available for
// recovery, congestion control and diagnostics. A median of three rejects a
// single delayed report but follows two consecutive samples at the new RTT.
class ConnectionLatencyStats {
 public:
  using Clock = std::chrono::steady_clock;

  void Update(double rtt_ms, Clock::time_point now = Clock::now()) {
    if (!std::isfinite(rtt_ms) || rtt_ms < 0 || rtt_ms > 2000) return;
    std::lock_guard lock(mutex_);
    if (count_ && now < updated_) return;
    if (!Fresh(now)) count_ = next_ = 0;
    samples_[next_] = rtt_ms;
    next_ = (next_ + 1) % samples_.size();
    if (count_ < samples_.size()) ++count_;
    updated_ = now;
  }

  std::optional<double> Get(Clock::time_point now = Clock::now()) const {
    std::lock_guard lock(mutex_);
    if (!Fresh(now)) return std::nullopt;
    if (count_ == 1) return samples_[0];
    if (count_ == 2) return std::min(samples_[0], samples_[1]);
    const auto [a, b, c] = samples_;
    return std::max(std::min(a, b), std::min(std::max(a, b), c));
  }

  void Reset() {
    std::lock_guard lock(mutex_);
    count_ = next_ = 0;
  }

 private:
  bool Fresh(Clock::time_point now) const {
    // The UI may snapshot `now` just before the report thread updates us.
    // A newer valid report must not cause a one-frame unavailable flicker.
    return count_ != 0 && now - updated_ < std::chrono::seconds(3);
  }
  mutable std::mutex mutex_;
  std::array<double, 3> samples_{};
  std::size_t count_ = 0;
  std::size_t next_ = 0;
  Clock::time_point updated_{};
};

}  // namespace crossdesk::gui_detail

#endif