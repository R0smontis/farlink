/*
 * @Author: DI JUNKUN
 * @Date: 2026-10-22
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _REMOTE_INPUT_DISPATCHER_H_
#define _REMOTE_INPUT_DISPATCHER_H_

#include <remote_action.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "input_outcome.h"

namespace crossdesk {

// Serializes native input, recovery and display resets off the RTC thread.
class RemoteInputDispatcher {
 public:
  using Apply =
      std::function<InputOutcome(const std::string&, const RemoteAction&)>;
  using Release = std::function<InputOutcome(const std::string&)>;
  using Report =
      std::function<void(const std::string&, int64_t, int64_t, InputOutcome)>;
  using ResetDisplay = std::function<InputOutcome()>;
  // Optional targeted adapter; legacy callers fall back to full Release.
  using ReleaseMouse =
      std::function<InputOutcome(const std::string&, unsigned)>;
  using Completion = std::function<void(InputOutcome)>;
  enum class Result { queued, coalesced, rejected, overflow };
  static constexpr size_t kMaxPendingPerController = 128;

  RemoteInputDispatcher(Apply apply, Release release, Report report = {},
                        ResetDisplay reset_display = {},
                        ReleaseMouse release_mouse = {});
  ~RemoteInputDispatcher();
  static bool Accepts(const RemoteAction& action);
  void Connect(const std::string& id);
  Result Push(const std::string& id, const RemoteAction& action,
              Completion completion = {});
  void Disconnect(const std::string& id);

  // Scheduled recovery is not idle work. Check HasPendingReleases() as well,
  // or Stop() before destroying the devices used by callbacks.
  void WaitIdle();
  bool HasPendingReleases();
  // Cancels pending input/timers and makes a final bounded cleanup attempt.
  void Stop();

 private:
  using Clock = std::chrono::steady_clock;
  enum class ItemKind { input, cleanup, reset_display };
  struct Item {
    std::string id;
    std::optional<RemoteAction> action;  // Present only for ordinary input.
    Clock::time_point queued_at;
    ItemKind kind = ItemKind::cleanup;
    // Zero requests full controller cleanup; otherwise only these mouse
    // buttons.
    unsigned mouse_mask = 0;
    Completion completion;
    bool IsReset() const { return kind == ItemKind::reset_display; }
    bool IsInput() const { return kind == ItemKind::input; }
    bool IsCleanup() const { return kind == ItemKind::cleanup; }
  };
  struct Retry {
    Clock::time_point due;
    std::chrono::milliseconds delay{100};
    unsigned mouse_mask = 0;
  };
  struct LogState {
    Clock::time_point slow{};
    std::array<Clock::time_point, 2> failure{};
  };

  static bool IsMove(const RemoteAction& action);
  static bool IsPixelScroll(const RemoteAction& action);
  static unsigned MouseUpMask(const RemoteAction& action);
  void RemoveResetsLocked();
  void QueueReleaseLocked(const std::string& id);
  void QueueMouseReleaseLocked(const std::string& id, unsigned mask);
  void RemoveQueuedCleanupLocked(const std::string& id);
  void ScheduleRetryLocked(const std::string& id, unsigned mouse_mask,
                           Clock::time_point finished);
  void CompleteCleanupLocked(const Item& item, InputOutcome outcome,
                             Clock::time_point finished);
  std::optional<Item> NextLocked();
  InputOutcome Execute(const Item& item);
  bool ShouldReportLocked(const Item& item, Clock::time_point now,
                          int64_t queued_ms, int64_t execution_ms,
                          InputOutcome outcome);
  void Run();
  // Execute a queued display reset on the dedicated reset thread. Resetting
  // the display mode can take hundreds of milliseconds on Windows while the
  // driver re-enumerates; running it on the input worker would stall every
  // controller's mouse/keyboard behind it (head-of-line blocking). The reset
  // has no ordering relationship with per-controller input — it only ever
  // runs after the last controller disconnects and is cancelled when a new
  // controller connects — so it is safe to run concurrently with input.
  void RunReset(Item item);

 public:
  // Host input dispatch queue-latency statistics: time from Push() enqueue
  // to the worker picking the item up, in microseconds. Lock-free atomics,
  // updated once per dequeued input item on the worker thread; readers are
  // diagnostics/tests. The p50 is exact over a small atomic ring of the most
  // recent samples — input rates are high enough that the last 64 samples
  // cover well under a second of interactive use, exactly the window where a
  // user perceives a stall.
  struct QueueLatencyStats {
    static constexpr size_t kWindowSize = 64;
    std::atomic<uint64_t> samples{0};
    std::atomic<uint64_t> total_us{0};
    std::atomic<uint64_t> max_us{0};
    // Ring of recent per-sample microseconds. Individual slots are written
    // only by the worker thread; readers may observe a slightly stale but
    // always fully-written value because each slot is a single atomic.
    std::array<std::atomic<uint64_t>, kWindowSize> ring{};

    void Record(uint64_t us) {
      const uint64_t n = samples.fetch_add(1, std::memory_order_relaxed);
      total_us.fetch_add(us, std::memory_order_relaxed);
      uint64_t current_max = max_us.load(std::memory_order_relaxed);
      while (us > current_max &&
             !max_us.compare_exchange_weak(current_max, us,
                                           std::memory_order_relaxed)) {
      }
      ring[n % kWindowSize].store(us, std::memory_order_relaxed);
    }

    struct Snapshot {
      uint64_t samples = 0;
      double mean_us = 0;
      uint64_t max_us = 0;
      uint64_t p50_us = 0;
      uint64_t p95_us = 0;
    };
    Snapshot Get() const {
      Snapshot result;
      const uint64_t n = samples.load(std::memory_order_relaxed);
      result.samples = n;
      if (n == 0) return result;
      result.mean_us =
          static_cast<double>(total_us.load(std::memory_order_relaxed)) / n;
      result.max_us = max_us.load(std::memory_order_relaxed);
      const size_t count = static_cast<size_t>(std::min<uint64_t>(n, kWindowSize));
      std::array<uint64_t, kWindowSize> values{};
      for (size_t i = 0; i < count; ++i)
        values[i] = ring[i].load(std::memory_order_relaxed);
      std::sort(values.begin(), values.begin() + count);
      result.p50_us = values[(count - 1) / 2];
      result.p95_us = values[std::min<size_t>(
          count - 1, static_cast<size_t>(count * 95 / 100))];
      return result;
    }
  };
  // Snapshot of the cumulative input queue-latency statistics. Exposed for
  // diagnostics and the latency microbenchmark; read-only, lock-free.
  QueueLatencyStats::Snapshot TakeQueueLatencyStats() const {
    return queue_latency_.Get();
  }

 private:


  Apply apply_;
  Release release_;
  Report report_;
  ResetDisplay reset_display_;
  ReleaseMouse release_mouse_;
  std::mutex mutex_;
  std::condition_variable ready_, idle_;
  std::deque<Item> pending_;
  std::unordered_set<std::string> connected_, blocked_;
  std::unordered_map<std::string, size_t> pending_counts_;
  std::unordered_map<std::string, Retry> retries_;
  std::unordered_map<std::string, LogState> logs_;
  bool last_was_retry_ = false;
  bool stopping_ = false;
  bool executing_ = false;
  std::once_flag stop_once_;
  std::thread worker_;
  // Dedicated display-reset lane: at most one reset runs at a time. The
  // worker hands reset items to this thread instead of executing them
  // inline, so a slow driver-mode change cannot stall input behind it.
  // reset_mutex_ alone guards the thread handle and the busy flag; the
  // handoff uses the item itself, which is trivially copyable state.
  std::mutex reset_mutex_;
  std::thread reset_worker_;
  std::atomic<bool> reset_running_{false};
  std::atomic<int> reset_active_count_{0};
  QueueLatencyStats queue_latency_;
};

}  // namespace crossdesk

#endif
