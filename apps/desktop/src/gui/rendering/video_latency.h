/*
 * @Author: DI JUNKUN
 * @Date: 2026-09-26
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _VIDEO_LATENCY_H_
#define _VIDEO_LATENCY_H_

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace crossdesk {

class VideoLatencyStats;

// Emit the single per-session controller-side first-frame and teardown
// observability lines. Free functions keep the stats object free of logging
// dependencies. The summary's codec/fps tier are resolved by the caller,
// which runs where the owning session is still alive.
void LogVideoFirstFramePresented(const VideoLatencyStats& stats,
                                 std::chrono::steady_clock::time_point now);
void LogVideoSessionSummary(const VideoLatencyStats& stats,
                            std::chrono::steady_clock::time_point now,
                            const std::string& codec, int fps_tier);


enum class VideoFrameStage : uint8_t {
  received,
  accepted,
  replaced,
  busy,
  committed,
  gpu_completed,
  presented,
  count,
};

// Travels with the exact queued pixels, including native frames. Its clock is
// translated once at the receive callback; rendering needs no live MiniRtcPeerPtr.
struct VideoLatencyFrame {
  using Clock = std::chrono::steady_clock;
  std::shared_ptr<VideoLatencyStats> stats;
  uint64_t id = 0;
  Clock::time_point capture_time{};
  bool capture_valid = true;
  Clock::time_point received_time{};
  void MarkPresented(Clock::time_point now = Clock::now()) const;
  // GPU completion/submission are estimates: neither confirms screen display.
  void MarkEstimated(Clock::time_point now = Clock::now()) const;
  // Fallback for renderers which expose submission but no presentation time.
  void MarkSubmitted(Clock::time_point now = Clock::now()) const;
  void MarkStage(VideoFrameStage stage, Clock::time_point now = Clock::now(),
                 double drawable_wait_ms = 0) const;
  // Translate a host-clock event, excluding any callback scheduling delay.
  static std::optional<Clock::time_point> FromHostTime(
      double event_time_s, double host_now_s,
      Clock::time_point now = Clock::now()) {
    const double age = host_now_s - event_time_s;
    if (event_time_s <= 0 || !std::isfinite(age) || age < 0 || age > 5)
      return std::nullopt;
    return now - std::chrono::duration_cast<Clock::duration>(
                     std::chrono::duration<double>(age));
  }
};

class VideoLatencyStats
    : public std::enable_shared_from_this<VideoLatencyStats> {
 public:
  using Clock = VideoLatencyFrame::Clock;
  // Per-session observability context for controller-side frame reporting.
  // Plain data only: a presented-handler callback can outlive the owning
  // session through a queued frame, so nothing here may reference it.
  struct SessionObservability {
    std::string remote_id;
    Clock::time_point connect_time{};
    // The session-end summary is emitted from whichever teardown path runs
    // first (user disconnect or process exit); this consumes exactly once.
    std::atomic<bool> summary_emitted{false};
    std::atomic<int64_t> connected_since_epoch_ns{-1};
    std::atomic<int> frame_width{0};
    std::atomic<int> frame_height{0};

    // Mirrored from the transport report when it arrives; empty means the
    // codec was not yet reported. The report names the codec implementation
    // (e.g. "VideoToolbox (H.265)"); the observability lines want the wire
    // codec family, so it is normalized here to the H265/H264 spelling the
    // transport summary uses.
    void StoreCodec(const std::string& codec) {
      std::lock_guard lock(codec_mutex_);
      codec_ = NormalizeCodecName(codec);
    }
    std::string LoadCodec() const {
      std::lock_guard lock(codec_mutex_);
      return codec_;
    }

    static std::string NormalizeCodecName(const std::string& name) {
      if (name.find("H.265") != std::string::npos ||
          name.find("H265") != std::string::npos ||
          name.find("HEVC") != std::string::npos ||
          name.find("hevc") != std::string::npos)
        return "H265";
      if (name.find("H.264") != std::string::npos ||
          name.find("H264") != std::string::npos ||
          name.find("h264") != std::string::npos)
        return "H264";
      if (name.find("AV1") != std::string::npos) return "AV1";
      if (name.find("VP9") != std::string::npos) return "VP9";
      if (name.find("VP8") != std::string::npos) return "VP8";
      return name;
    }

   private:
    mutable std::mutex codec_mutex_;
    std::string codec_;
  };

  struct Snapshot {
    double average_ms;
    size_t samples;
    bool estimated;
  };

  // Intervals between observed distinct render endpoints, not an assertion
  // that pixels should have changed. Static/source-idle video can also have
  // gaps. Only explicit visibility/session boundaries stop the measurement.
  static constexpr double kFrameGapThresholdMs = 200;
  struct FrameGapSnapshot {
    size_t intervals = 0;
    double interval_total_ms = 0;
    double interval_max_ms = 0;
    size_t gap_count = 0;
    double gap_total_ms = 0;
    double gap_max_ms = 0;
    double ongoing_gap_ms = 0;
  };

  // Non-cumulative buckets: <=2, (2,8], (8,16], (16,33], >33 ms.
  // Keep distributions bounded; a mean alone conceals paired arrivals.
  struct CadenceTiming {
    std::array<uint64_t, 5> buckets{};
    uint64_t samples = 0;
    double total_ms = 0;
    double max_ms = 0;

    void Add(double ms) {
      if (!std::isfinite(ms) || ms < 0) return;
      constexpr std::array<double, 4> upper_ms{2, 8, 16, 33};
      size_t bucket = 0;
      while (bucket < upper_ms.size() && ms > upper_ms[bucket]) ++bucket;
      ++buckets[bucket];
      ++samples;
      total_ms += ms;
      max_ms = std::max(max_ms, ms);
    }
    double Average() const { return samples ? total_ms / samples : 0; }
  };

  struct PipelineSnapshot {
    std::array<uint64_t, static_cast<size_t>(VideoFrameStage::count)> frames{};
    uint64_t render_ticks = 0;
    uint64_t timer_late_samples = 0;
    double timer_late_total_ms = 0;
    double timer_late_max_ms = 0;
    uint64_t commit_timing_samples = 0;
    double receive_to_commit_total_ms = 0;
    double receive_to_commit_max_ms = 0;
    double drawable_wait_total_ms = 0;
    double drawable_wait_max_ms = 0;
    CadenceTiming arrival_intervals;
    CadenceTiming receive_to_accept;
    CadenceTiming pending_at_replace;
    CadenceTiming pending_at_commit;
    uint64_t arrival_out_of_order = 0;
    uint64_t scheduler_ticks = 0;
    uint64_t scheduler_skipped_deadlines = 0;
    double scheduler_late_total_ms = 0;
    double scheduler_late_max_ms = 0;
    uint64_t arrival_wakeups = 0;
    double wake_queue_total_ms = 0;
    double wake_queue_max_ms = 0;
  };

  struct DiagnosticsSnapshot {
    std::array<FrameGapSnapshot, 2> gaps{};
    PipelineSnapshot pipeline;
    double window_ms = 0;
    int stream_index = 0;
    bool final = false;
  };

  std::optional<DiagnosticsSnapshot> SetFrameGapTrackingEnabled(
      bool enabled, int stream_index = 0, Clock::time_point now = Clock::now()) {
    std::lock_guard lock(mutex_);
    if (gap_tracking_enabled_ == enabled && gap_stream_index_ == stream_index)
      return std::nullopt;
    auto tail = TakeDiagnosticsLocked(now, true);
    gap_tracking_enabled_ = enabled;
    gap_stream_index_ = stream_index;
    gap_min_frame_id_ = issued_id_;
    gap_states_ = {};
    pipeline_ = {};
    last_received_time_.reset();
    diagnostic_window_started_ = now;
    return tail;
  }

  std::optional<DiagnosticsSnapshot> TakeDiagnostics(
      Clock::time_point now = Clock::now()) {
    std::lock_guard lock(mutex_);
    return TakeDiagnosticsLocked(now, false);
  }

  void RecordRenderTick(std::optional<double> timer_lateness_ms) {
    std::lock_guard lock(mutex_);
    if (!gap_tracking_enabled_) return;
    ++pipeline_.render_ticks;
    if (!timer_lateness_ms || !std::isfinite(*timer_lateness_ms)) return;
    const double late_ms = std::max(0.0, *timer_lateness_ms);
    ++pipeline_.timer_late_samples;
    pipeline_.timer_late_total_ms += late_ms;
    pipeline_.timer_late_max_ms = std::max(pipeline_.timer_late_max_ms, late_ms);
  }

  // Count every due scheduler tick, including ticks with no dirty frame. The
  // existing render_ticks count only presentation attempts and are not Hz.
  void RecordSchedulerTick(double late_ms, uint64_t skipped_deadlines) {
    std::lock_guard lock(mutex_);
    if (!gap_tracking_enabled_ || !std::isfinite(late_ms)) return;
    late_ms = std::max(0.0, late_ms);
    ++pipeline_.scheduler_ticks;
    pipeline_.scheduler_skipped_deadlines += skipped_deadlines;
    pipeline_.scheduler_late_total_ms += late_ms;
    pipeline_.scheduler_late_max_ms =
        std::max(pipeline_.scheduler_late_max_ms, late_ms);
  }

  void RecordFrameWake(double queued_ms) {
    std::lock_guard lock(mutex_);
    if (!gap_tracking_enabled_ || !std::isfinite(queued_ms)) return;
    queued_ms = std::max(0.0, queued_ms);
    ++pipeline_.arrival_wakeups;
    pipeline_.wake_queue_total_ms += queued_ms;
    pipeline_.wake_queue_max_ms = std::max(pipeline_.wake_queue_max_ms, queued_ms);
  }

  void RecordStage(const VideoLatencyFrame& frame, VideoFrameStage stage,
                   Clock::time_point now, double drawable_wait_ms = 0) {
    std::lock_guard lock(mutex_);
    RecordStageLocked(frame, stage, now, drawable_wait_ms);
  }

  // Index 0 uses actual presentation times; index 1 uses GPU/submission
  // estimates. Never combine the two or count an estimate upgrade twice.
  std::array<FrameGapSnapshot, 2> TakeFrameGapStats(
      Clock::time_point now = Clock::now()) {
    std::lock_guard lock(mutex_);
    std::array<FrameGapSnapshot, 2> result{};
    if (!gap_tracking_enabled_) return result;
    for (size_t i = 0; i < gap_states_.size(); ++i) {
      auto& state = gap_states_[i];
      result[i] = state.interval;
      if (state.last_id != 0) {
        const double age_ms =
            std::chrono::duration<double, std::milli>(now - state.last_time)
                .count();
        if (age_ms > kFrameGapThresholdMs)
          result[i].ongoing_gap_ms = age_ms;
      }
      state.interval = {};
    }
    return result;
  }

  VideoLatencyFrame Frame(uint64_t captured_us, int64_t local_now_us,
                          Clock::time_point now = Clock::now()) {
    const bool valid_capture = captured_us != 0 && local_now_us > 0 &&
        captured_us <= uint64_t(local_now_us) &&
        uint64_t(local_now_us) - captured_us <= 5'000'000;
    std::lock_guard lock(mutex_);
    // Presentation cadence needs only the local endpoint clock. Keep issuing
    // frame identities when cross-machine capture-time mapping is unavailable.
    VideoLatencyFrame frame{shared_from_this(), ++issued_id_,
            valid_capture
                ? now - std::chrono::microseconds(local_now_us - captured_us)
                : now,
            valid_capture, now};
    RecordStageLocked(frame, VideoFrameStage::received, now, 0);
    return frame;
  }

  // Cumulative session counters survive Reset/diagnostics windows. They are
  // non-destructive observability, incremented only in the real frame path:
  // frames_decoded where decoded frames enter the app, frames_presented in the
  // presentation recording path below.
  void RecordDecodedFrame(int width, int height) {
    if (frames_decoded_total_.fetch_add(1, std::memory_order_relaxed) == 0)
      first_decoded_time_ns_.store(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              Clock::now().time_since_epoch()).count(), std::memory_order_relaxed);
    if (observability_ && width > 0 && height > 0) {
      observability_->frame_width.store(width, std::memory_order_relaxed);
      observability_->frame_height.store(height, std::memory_order_relaxed);
    }
  }

  uint64_t FramesDecodedTotal() const {
    return frames_decoded_total_.load(std::memory_order_relaxed);
  }

  uint64_t FramesPresentedTotal() const {
    return frames_presented_total_.load(std::memory_order_relaxed);
  }

  // Zero until the first frame of each kind.
  int64_t FirstDecodedTimeNs() const {
    return first_decoded_time_ns_.load(std::memory_order_relaxed);
  }

  int64_t FirstPresentedTimeNs() const {
    return first_presented_time_ns_.load(std::memory_order_relaxed);
  }

  void SetSessionObservability(std::shared_ptr<SessionObservability> context) {
    observability_ = std::move(context);
  }

  std::shared_ptr<SessionObservability> GetSessionObservability() const {
    return observability_;
  }

  // First real presentation is consumed exactly once per session.
  bool ConsumeFirstPresented() {
    return !first_presented_consumed_.exchange(true, std::memory_order_acq_rel);
  }

  void Record(const VideoLatencyFrame& frame, Clock::time_point now,
              bool estimated = false) {
    const double ms =
        std::chrono::duration<double, std::milli>(now - frame.capture_time)
            .count();
    std::lock_guard lock(mutex_);
    if (!estimated) {
      // Cumulative per-session presentations, deduped like the gap tracker:
      // redraws and out-of-order delivery must not count twice.
      if (frame.id > last_presented_id_) {
        last_presented_id_ = frame.id;
        if (frames_presented_total_.fetch_add(1, std::memory_order_relaxed) == 0)
          first_presented_time_ns_.store(
              std::chrono::duration_cast<std::chrono::nanoseconds>(
                  now.time_since_epoch()).count(), std::memory_order_relaxed);
      }
      RecordStageLocked(frame, VideoFrameStage::presented, now, 0);
    }
    if (gap_tracking_enabled_ && frame.id > gap_min_frame_id_) {
      auto& gap = gap_states_[estimated ? 1 : 0];
      // Redraws, estimate upgrades, and out-of-order callback delivery must
      // not add another interval or move the latest endpoint backwards.
      if (frame.id > gap.last_id &&
          (gap.last_id == 0 || now > gap.last_time)) {
        if (gap.last_id != 0) {
          const double interval_ms =
              std::chrono::duration<double, std::milli>(now - gap.last_time)
                  .count();
          auto& value = gap.interval;
          ++value.intervals;
          value.interval_total_ms += interval_ms;
          value.interval_max_ms = std::max(value.interval_max_ms, interval_ms);
          if (interval_ms > kFrameGapThresholdMs) {
            ++value.gap_count;
            value.gap_total_ms += interval_ms;
            value.gap_max_ms = std::max(value.gap_max_ms, interval_ms);
          }
        }
        gap.last_id = frame.id;
        gap.last_time = now;
      }
    }
    if (!frame.capture_valid || ms < 0 || ms > 5000) return;
    if (frame.id <= last_id_) {
      // A presentation callback can arrive after GPU completion, including
      // callbacks for later frames. Upgrade its estimate without adding a
      // second sample. Never revive a pruned/reset frame or overwrite a real
      // presentation with a redraw or an estimate.
      if (estimated) return;
      auto sample = std::find_if(
          samples_.begin(), samples_.end(),
          [&](const Sample& value) { return value.id == frame.id; });
      if (sample == samples_.end() || !sample->estimated) return;
      *sample = {frame.id, now, ms, false};
    } else {
      last_id_ = frame.id;
      samples_.push_back({frame.id, now, ms, estimated});
    }
    Prune(now);
  }

  std::optional<Snapshot> Get(Clock::time_point now = Clock::now()) {
    std::lock_guard lock(mutex_);
    Prune(now);
    if (samples_.empty()) return std::nullopt;
    double presented_sum = 0;
    double estimated_sum = 0;
    size_t presented_count = 0;
    for (const auto& sample : samples_) {
      if (sample.estimated) {
        estimated_sum += sample.ms;
      } else {
        presented_sum += sample.ms;
        ++presented_count;
      }
    }
    // Prefer actual presentations in the rolling window. Do not mix GPU and
    // screen endpoints, or let the newest not-yet-presented frame force an
    // otherwise precise measurement to be labelled approximate.
    if (presented_count != 0)
      return Snapshot{presented_sum / presented_count, presented_count, false};
    return Snapshot{estimated_sum / samples_.size(), samples_.size(), true};
  }

  std::optional<DiagnosticsSnapshot> Reset(Clock::time_point now = Clock::now()) {
    std::lock_guard lock(mutex_);
    auto tail = TakeDiagnosticsLocked(now, true);
    samples_.clear();
    last_id_ = issued_id_;  // Invalidate frames still held by a renderer.
    gap_min_frame_id_ = issued_id_;
    gap_tracking_enabled_ = false;
    gap_states_ = {};
    pipeline_ = {};
    last_received_time_.reset();
    return tail;
  }

 private:
  std::optional<DiagnosticsSnapshot> TakeDiagnosticsLocked(
      Clock::time_point now, bool final) {
    if (!gap_tracking_enabled_) return std::nullopt;
    DiagnosticsSnapshot result;
    result.pipeline = pipeline_;
    result.stream_index = gap_stream_index_;
    result.final = final;
    result.window_ms = std::max(0.0, std::chrono::duration<double, std::milli>(
        now - diagnostic_window_started_).count());
    for (size_t i = 0; i < gap_states_.size(); ++i) {
      auto& state = gap_states_[i];
      result.gaps[i] = state.interval;
      if (state.last_id != 0) {
        const double age_ms = std::chrono::duration<double, std::milli>(
            now - state.last_time).count();
        if (age_ms > kFrameGapThresholdMs) result.gaps[i].ongoing_gap_ms = age_ms;
      }
      state.interval = {};
    }
    pipeline_ = {};
    diagnostic_window_started_ = now;
    return result;
  }

  void RecordStageLocked(const VideoLatencyFrame& frame, VideoFrameStage stage,
                         Clock::time_point now, double drawable_wait_ms) {
    if (!gap_tracking_enabled_ || frame.id <= gap_min_frame_id_ ||
        frame.id > issued_id_) return;
    const auto index = static_cast<size_t>(stage);
    if (index >= pipeline_.frames.size()) return;
    // Bounded identities allow completion callbacks to arrive out of order,
    // while cached-frame redraws never count as fresh video. Stale callbacks
    // cannot replace a newer identity occupying the same slot.
    auto& seen = pipeline_seen_[frame.id % pipeline_seen_.size()];
    if (seen.id > frame.id) return;
    if (seen.id != frame.id) seen = {frame.id, 0};
    const uint32_t bit = uint32_t{1} << index;
    if (seen.stages & bit) return;
    seen.stages |= bit;
    ++pipeline_.frames[index];
    if (stage == VideoFrameStage::received) {
      if (last_received_time_ && now < *last_received_time_) {
        // Callback timestamps can be delivered out of order after preview
        // conversion. Do not interpret a negative interval as a burst.
        ++pipeline_.arrival_out_of_order;
      } else {
        if (last_received_time_) {
          pipeline_.arrival_intervals.Add(
              std::chrono::duration<double, std::milli>(
                  now - *last_received_time_).count());
        }
        last_received_time_ = now;
      }
    } else if (stage == VideoFrameStage::accepted) {
      seen.accepted_at = now;
      if (frame.received_time != Clock::time_point{}) {
        pipeline_.receive_to_accept.Add(
            std::chrono::duration<double, std::milli>(
                now - frame.received_time).count());
      }
    } else if (seen.accepted_at &&
               (stage == VideoFrameStage::replaced ||
                stage == VideoFrameStage::committed)) {
      auto& pending = stage == VideoFrameStage::replaced
                          ? pipeline_.pending_at_replace
                          : pipeline_.pending_at_commit;
      pending.Add(std::chrono::duration<double, std::milli>(
          now - *seen.accepted_at).count());
    }
    if (stage == VideoFrameStage::committed) {
      if (frame.received_time != Clock::time_point{} &&
          now >= frame.received_time) {
        const double delay_ms = std::chrono::duration<double, std::milli>(
            now - frame.received_time).count();
        ++pipeline_.commit_timing_samples;
        pipeline_.receive_to_commit_total_ms += delay_ms;
        pipeline_.receive_to_commit_max_ms =
            std::max(pipeline_.receive_to_commit_max_ms, delay_ms);
      }
      if (std::isfinite(drawable_wait_ms) && drawable_wait_ms >= 0) {
        pipeline_.drawable_wait_total_ms += drawable_wait_ms;
        pipeline_.drawable_wait_max_ms =
            std::max(pipeline_.drawable_wait_max_ms, drawable_wait_ms);
      }
    }
  }

  void Prune(Clock::time_point now) {
    // Upgrades can change timestamps out of order.
    samples_.erase(std::remove_if(samples_.begin(), samples_.end(),
                                  [&](const Sample& sample) {
                                    return now - sample.time >
                                           std::chrono::seconds(1);
                                  }),
                   samples_.end());
    while (samples_.size() > 240) samples_.pop_front();
  }
  struct Sample {
    uint64_t id;
    Clock::time_point time;
    double ms;
    bool estimated;
  };
  struct FrameGapState {
    uint64_t last_id = 0;
    Clock::time_point last_time{};
    FrameGapSnapshot interval;
  };
  std::mutex mutex_;
  uint64_t issued_id_ = 0;
  uint64_t last_id_ = 0;
  // Guards the cumulative presented counter against redraw/delivery duplicates.
  uint64_t last_presented_id_ = 0;
  std::deque<Sample> samples_;
  bool gap_tracking_enabled_ = false;
  int gap_stream_index_ = 0;
  uint64_t gap_min_frame_id_ = 0;
  std::array<FrameGapState, 2> gap_states_{};
  PipelineSnapshot pipeline_;
  std::optional<Clock::time_point> last_received_time_;
  struct PipelineIdentity {
    uint64_t id = 0;
    uint32_t stages = 0;
    std::optional<Clock::time_point> accepted_at;
  };
  std::array<PipelineIdentity, 1024> pipeline_seen_{};
  Clock::time_point diagnostic_window_started_{};
  // Outside mutex_: cumulative per-session observability. Atomics because the
  // decode callback, render thread and UI thread touch them concurrently.
  std::atomic<uint64_t> frames_decoded_total_{0};
  std::atomic<uint64_t> frames_presented_total_{0};
  // Nanosecond epoch of the first decoded / presented frame; the summary fps
  // is anchored here so connect and negotiation latency do not dilute the
  // steady-state rate.
  std::atomic<int64_t> first_decoded_time_ns_{0};
  std::atomic<int64_t> first_presented_time_ns_{0};
  std::atomic<bool> first_presented_consumed_{false};
  std::shared_ptr<SessionObservability> observability_;
};

inline void VideoLatencyFrame::MarkSubmitted(Clock::time_point now) const {
  MarkEstimated(now);
}

inline void VideoLatencyFrame::MarkEstimated(Clock::time_point now) const {
  if (stats) stats->Record(*this, now, true);
}

inline void VideoLatencyFrame::MarkPresented(Clock::time_point now) const {
  if (stats) stats->Record(*this, now);
}

inline void VideoLatencyFrame::MarkStage(VideoFrameStage stage,
                                       Clock::time_point now,
                                       double drawable_wait_ms) const {
  if (stats) stats->RecordStage(*this, stage, now, drawable_wait_ms);
}
}  // namespace crossdesk

#endif
