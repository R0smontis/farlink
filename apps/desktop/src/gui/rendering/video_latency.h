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
#include <vector>

namespace crossdesk {
// Dominant latency contributor for the per-stage breakdown. Ordered by the
// pipeline direction so a reader can scan left-to-right; the enum value is
// serialized into machine-readable teardown/diagnostic output, so existing
// members must never be renumbered.
enum class LatencyBottleneck : uint8_t {
  none = 0,
  network = 1,
  jitter = 2,
  decode = 3,
  render = 4,
};

// Classify the dominant per-stage contributor of a degraded stream. `rtt_ms`
// is the smoothed transport round-trip, `jitter_target_ms` the live jitter
// buffer target delay, `decode_ms` the decoder completion minus reassembly
// time, `render_ms` the decode-callback to presentation time. Every input is
// a rolling mean in milliseconds; negative inputs are treated as unavailable
// and never dominate.
//
// Returns LatencyBottleneck::none when the pipeline is healthy across every
// measured stage — the exact thresholds mirror the reference-grade interactive
// budget: sub-25 ms RTT, sub-8 ms jitter target, sub-6 ms decode and sub-10 ms
// render leave no single stage worth flagging. Otherwise the stage with the
// largest absolute contribution over its own healthy budget wins, so a 40 ms
// RTT does not mask a 30 ms decode stall on the same stream.
inline LatencyBottleneck ClassifyLatencyBottleneck(double rtt_ms,
                                                   double jitter_target_ms,
                                                   double decode_ms,
                                                   double render_ms) {
  constexpr double kHealthyRttMs = 25;
  constexpr double kHealthyJitterMs = 8;
  constexpr double kHealthyDecodeMs = 6;
  constexpr double kHealthyRenderMs = 10;
  const double rtt_excess = rtt_ms >= 0 ? rtt_ms - kHealthyRttMs : -1;
  const double jitter_excess =
      jitter_target_ms >= 0 ? jitter_target_ms - kHealthyJitterMs : -1;
  const double decode_excess =
      decode_ms >= 0 ? decode_ms - kHealthyDecodeMs : -1;
  const double render_excess =
      render_ms >= 0 ? render_ms - kHealthyRenderMs : -1;
  double worst = 0;
  LatencyBottleneck result = LatencyBottleneck::none;
  const auto consider = [&](double excess, LatencyBottleneck kind) {
    if (excess > worst) {
      worst = excess;
      result = kind;
    }
  };
  consider(rtt_excess, LatencyBottleneck::network);
  consider(jitter_excess, LatencyBottleneck::jitter);
  consider(decode_excess, LatencyBottleneck::decode);
  consider(render_excess, LatencyBottleneck::render);
  return result;
}

inline const char* LatencyBottleneckName(LatencyBottleneck kind) {
  switch (kind) {
    case LatencyBottleneck::network: return "network";
    case LatencyBottleneck::jitter: return "jitter";
    case LatencyBottleneck::decode: return "decode";
    case LatencyBottleneck::render: return "render";
    case LatencyBottleneck::none: break;
  }
  return "none";
}


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
  // Decode stage duration in milliseconds, measured inside minirtc as
  // (decoded_timestamp - received_timestamp) of the arriving frame: the
  // exact span from completed RTP reassembly to completed hardware/software
  // decode, including any decoder queue wait. Negative when the transport
  // did not supply both timestamps for this frame (pre-calibration frames,
  // or a backend that never reports them); such frames simply never feed
  // the decode rolling window. Immutable once the frame is constructed.
  double decode_ms = -1;
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

  // Session-cumulative inter-presentation gap measurement (D8). Unlike the
  // windowed FrameGapSnapshot above — which is reset on every diagnostics read
  // and only active while gap tracking is explicitly enabled — this accumulator
  // runs for the whole session alongside the always-on presented counter, so a
  // teardown summary can report the actual elapsed monotonic time between
  // successive presentations of the same stream. It measures only the local
  // endpoint clock of this process: it is NOT a capture-to-photon latency, it
  // does not decompose into pipeline stages, and it does not use any remote
  // clock. A gap over the freeze threshold means two presentations were far
  // apart in time, not that the user saw jank — static or source-idle content
  // legitimately presents rarely, so long gaps are expected there and are not
  // a defect signal on their own. All memory is fixed-size; nothing is
  // allocated or logged per frame.
  struct InterPresentGapStats {
    // Distinct inter-presentation intervals observed. A session that presented
    // fewer than two frames has zero intervals and is reported distinctly
    // (never averaged as if it had a cadence).
    uint64_t intervals = 0;
    double interval_total_ms = 0;
    double interval_max_ms = 0;
    // Freeze events: intervals strictly greater than the threshold. Exactly
    // 200 ms is not a freeze. gap_total_ms/gap_max_ms cover freeze intervals
    // only, so mean freeze duration is gap_total_ms / gap_count.
    uint64_t gap_count = 0;
    double gap_total_ms = 0;
    double gap_max_ms = 0;
    // Bounded histogram over interval duration for an approximate p95 without
    // storing per-frame values. Bucket edges in ms, exponential so both the
    // normal sub-frame cadence and multi-second freezes resolve with a few
    // percent of width per bucket near the median. counts[k] holds intervals
    // in (edges[k-1], edges[k]]; the last bucket is the overflow above the top
    // edge. Fixed size, O(1) per frame.
    static constexpr std::array<double, 12> kBucketEdgesMs{
        4, 8, 12, 17, 25, 34, 50, 100, 200, 400, 1000, 2000};
    std::array<uint64_t, kBucketEdgesMs.size() + 1> buckets{};

    void Add(double interval_ms) {
      if (!std::isfinite(interval_ms) || interval_ms < 0) return;
      ++intervals;
      interval_total_ms += interval_ms;
      interval_max_ms = std::max(interval_max_ms, interval_ms);
      if (interval_ms > kFrameGapThresholdMs) {
        // Strictly greater-than: an interval of exactly the threshold is not
        // a freeze.
        ++gap_count;
        gap_total_ms += interval_ms;
        gap_max_ms = std::max(gap_max_ms, interval_ms);
      }
      size_t bucket = 0;
      while (bucket < kBucketEdgesMs.size() && interval_ms > kBucketEdgesMs[bucket])
        ++bucket;
      ++buckets[bucket];
    }

    double MeanIntervalMs() const {
      return intervals ? interval_total_ms / intervals : 0;
    }
    double MeanFreezeMs() const {
      return gap_count ? gap_total_ms / gap_count : 0;
    }

    // Approximate p95 interval derived from the bucket histogram: the upper
    // edge of the first bucket whose cumulative count reaches the 95th
    // percentile. Because it is a bucket upper edge it is an upper bound on
    // the true p95, never an under-estimate; the width of the containing
    // bucket is its resolution. Returns 0 when there are no intervals.
    double ApproxP95IntervalMs() const {
      if (intervals == 0) return 0;
      const double rank = 0.95 * intervals;
      uint64_t cumulative = 0;
      for (size_t i = 0; i < buckets.size(); ++i) {
        cumulative += buckets[i];
        if (cumulative >= rank) {
          return i < kBucketEdgesMs.size() ? kBucketEdgesMs[i]
                                           : interval_max_ms;
        }
      }
      return interval_max_ms;
    }
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

  // Rolling per-stage window shared by the decode and render measurements.
  // Fixed 60-sample ring (roughly one second at 60 fps, two at 30) holding
  // exact values, so the mean is exact within the window and the p95 is the
  // exact order statistic of the window — not a bucket upper bound. Thread
  // safety: one writer thread per instance (decode callback for decode,
  // render/present callbacks for render) and any number of readers, all
  // serialized through a tiny internal mutex. Critical sections are a handful
  // of stores/copies of a 480-byte array; they never allocate, never log and
  // never block on IO, so even a contended reader costs nanoseconds, not a
  // frame.
  class StageWindow {
   public:
    static constexpr size_t kCapacity = 60;

    // Decode-stage recording: samples are independent per frame, so a plain
    // append is correct.
    void Add(double ms) {
      if (!std::isfinite(ms) || ms < 0) return;
      std::lock_guard lock(mutex_);
      AppendLocked(ms);
    }

    // Render-stage recording keyed by frame identity: an estimate upgrade or
    // a true presentation arriving after a GPU-completion estimate replaces
    // the earlier sample for the same frame instead of double-counting it.
    // A true presentation always wins over an estimate; a later estimate
    // never overwrites the truer presented value.
    void Add(uint64_t frame_id, double ms, bool estimated) {
      if (!std::isfinite(ms) || ms < 0 || frame_id == 0) return;
      std::lock_guard lock(mutex_);
      // The ring is tiny; a linear scan for the same frame id is faster than
      // any index structure and bounded by 60 iterations worst-case.
      for (size_t i = 0; i < size_; ++i) {
        auto& slot = ring_[(head_ + i) % kCapacity];
        if (slot.frame_id != frame_id) continue;
        if (!estimated || slot.estimated) {
          // Upgrade in place: prefer the true endpoint, keep the first value
          // when the new sample is not strictly better information.
          if (!slot.estimated && estimated) return;
          slot.ms = ms;
          slot.estimated = estimated;
        }
        return;
      }
      AppendLocked(ms, frame_id, estimated);
    }

    struct Snapshot {
      size_t samples = 0;
      double mean_ms = -1;  // -1 when no sample was ever recorded
      double p95_ms = -1;
      double max_ms = -1;
    };

    Snapshot Get() const {
      std::lock_guard lock(mutex_);
      Snapshot result;
      if (size_ == 0) return result;
      std::array<double, kCapacity> values;
      double total = 0;
      double max_value = 0;
      for (size_t i = 0; i < size_; ++i) {
        const double ms = ring_[(head_ + i) % kCapacity].ms;
        values[i] = ms;
        total += ms;
        max_value = std::max(max_value, ms);
      }
      result.samples = size_;
      result.mean_ms = total / size_;
      result.max_ms = max_value;
      // Exact p95 of the window: the ceil(0.95 * n)-th smallest sample. With
      // n <= 60 the selection cost is trivial and bounded; using nearest-rank
      // keeps the statistic honest instead of interpolating a distribution
      // this small.
      std::sort(values.begin(), values.begin() + size_);
      const size_t rank =
          std::min(size_ - 1, static_cast<size_t>(
                                  std::ceil(0.95 * size_)) -
                                  1);
      result.p95_ms = values[rank];
      return result;
    }

    void Reset() {
      std::lock_guard lock(mutex_);
      head_ = 0;
      size_ = 0;
    }

   private:
    void AppendLocked(double ms, uint64_t frame_id = 0, bool estimated = false) {
      if (size_ < kCapacity) {
        ring_[(head_ + size_) % kCapacity] = {frame_id, ms, estimated};
        ++size_;
      } else {
        ring_[head_] = {frame_id, ms, estimated};
        head_ = (head_ + 1) % kCapacity;
      }
    }

    struct Slot {
      uint64_t frame_id = 0;
      double ms = 0;
      bool estimated = false;
    };
    mutable std::mutex mutex_;
    std::array<Slot, kCapacity> ring_{};
    size_t head_ = 0;
    size_t size_ = 0;
  };

  // Copy-out snapshot of both per-stage rolling windows. Read by the HUD once
  // per second and once at teardown; never allocated per frame.
  struct StageSnapshot {
    StageWindow::Snapshot decode;
    StageWindow::Snapshot render;
  };

  StageSnapshot TakeStageSnapshot() const {
    StageSnapshot result;
    result.decode = decode_window_.Get();
    result.render = render_window_.Get();
    return result;
  }

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
    return FrameWithDecode(captured_us, local_now_us, -1, now);
  }

  // Same as Frame() but additionally carries the transport-measured decode
  // stage duration in milliseconds (decoded_timestamp - received_timestamp
  // of the arriving MiniRtc frame). Pass a negative value when either
  // timestamp was unavailable — the frame then simply never enters the
  // decode rolling window. The decode sample is recorded here, once, at
  // frame creation: it is a property of the transport pipeline that has
  // already completed by the time this callback runs, so it does not depend
  // on any later render endpoint.
  VideoLatencyFrame FrameWithDecode(uint64_t captured_us,
                                    int64_t local_now_us,
                                    double decode_ms,
                                    Clock::time_point now = Clock::now()) {
    const bool valid_capture = captured_us != 0 && local_now_us > 0 &&
        captured_us <= uint64_t(local_now_us) &&
        uint64_t(local_now_us) - captured_us <= 5'000'000;
    if (decode_ms >= 0 && std::isfinite(decode_ms)) {
      // Atomic rolling accumulator: the decode callback thread is the only
      // writer, HUD and teardown readers never block the frame path.
      decode_window_.Add(decode_ms);
    }
    std::lock_guard lock(mutex_);
    // Presentation cadence needs only the local endpoint clock. Keep issuing
    // frame identities when cross-machine capture-time mapping is unavailable.
    VideoLatencyFrame frame{shared_from_this(), ++issued_id_,
            valid_capture
                ? now - std::chrono::microseconds(local_now_us - captured_us)
                : now,
            valid_capture, now, decode_ms};
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
    if (width > 0 && height > 0) {
      const int64_t sec = std::chrono::duration_cast<std::chrono::seconds>(
                              Clock::now().time_since_epoch())
                              .count();
      std::lock_guard lock(persecond_mutex_);
      if (resolution_series_.empty() ||
          resolution_series_.back().width != width ||
          resolution_series_.back().height != height) {
        resolution_series_.push_back({sec, width, height});
      }
    }
  }

  struct ResolutionEvent {
    int64_t sec;
    int width;
    int height;
  };

  std::vector<ResolutionEvent> ResolutionSeries() const {
    std::lock_guard lock(persecond_mutex_);
    return resolution_series_;
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
  bool FirstPresentedConsumed() const {
    return first_presented_consumed_.load(std::memory_order_acquire);
  }

  // Per-second wall-clock histogram of true presentations, for machine-
  // readable session measurement (fps per second, freezes). True
  // presentations only — never estimates, redraws, or out-of-order
  // deliveries: those do not represent a fresh frame reaching the screen.
  // Buckets are retained for the whole session; the harness runs are
  // minutes-long, so at one bucket per second this stays tiny.
  void RecordPresentedWallSecond(Clock::time_point now) {
    const int64_t sec = std::chrono::duration_cast<std::chrono::seconds>(
                            now.time_since_epoch())
                            .count();
    std::lock_guard lock(persecond_mutex_);
    if (persecond_first_sec_ < 0) {
      persecond_first_sec_ = sec;
    }
    const int64_t index = sec - persecond_first_sec_;
    if (index < 0) return;
    if (static_cast<size_t>(index) >= persecond_counts_.size()) {
      persecond_counts_.resize(static_cast<size_t>(index) + 1, 0);
    }
    ++persecond_counts_[static_cast<size_t>(index)];
  }

  struct PerSecondSnapshot {
    // Wall-clock epoch (seconds, steady-clock domain) of bucket 0; -1 when
    // nothing was ever presented. Counts[i] is the number of true
    // presentations in second (first_sec + i); missing trailing seconds are
    // zero-padded by the resize above, so gaps inside the vector are real
    // zero-presentation seconds.
    int64_t first_sec = -1;
    std::vector<uint32_t> counts;
  };

  PerSecondSnapshot TakePerSecondSnapshot() const {
    std::lock_guard lock(persecond_mutex_);
    PerSecondSnapshot result;
    result.first_sec = persecond_first_sec_;
    result.counts = persecond_counts_;
    return result;
  }
  // Session-cumulative inter-presentation gap statistics, copied out under
  // mutex_. Read once at teardown; a copy is returned because the struct is
  // small and fixed-size. The first/last presentation timestamps that define
  // the freezes-per-minute denominator are reported separately via
  // InterPresentWindow.
  InterPresentGapStats TakeInterPresentGapStats() const {
    std::lock_guard lock(mutex_);
    return inter_present_;
  }

  // The monotonic endpoints bounding the measured intervals: the time of the
  // first and last counted presentation. The span between them is the exact
  // window the inter-presentation intervals cover — the correct denominator
  // for a freezes-per-minute rate — because intervals can only exist between
  // two presentations. Both empty when nothing was ever presented. Silence
  // after the last presentation is outside this window and unmeasured.
  struct InterPresentWindow {
    std::optional<Clock::time_point> first;
    std::optional<Clock::time_point> last;
  };
  InterPresentWindow TakeInterPresentWindow() const {
    std::lock_guard lock(mutex_);
    InterPresentWindow window;
    window.first = first_present_time_;
    window.last = last_present_time_;
    return window;
  }

  void Record(const VideoLatencyFrame& frame, Clock::time_point now,
              bool estimated = false) {
    const double ms =
        std::chrono::duration<double, std::milli>(now - frame.capture_time)
            .count();
    // Render stage: receive-callback entry to this render endpoint. Recorded
    // for every endpoint kind — true presentation, GPU-completion estimate
    // and bare submission — because the HUD answers "where does the time go
    // NOW"; restricting to true presentations would leave the metric empty
    // on renderers that expose only submission. Deduped by frame identity so
    // a redraw or an estimate upgrade cannot double-count the same frame,
    // matching the presented-counter contract. Out-of-order callbacks with a
    // negative span are clamped to zero rather than dropped: the frame did
    // traverse the render path, only its endpoint clock arrived early.
    if (frame.received_time != Clock::time_point{} && frame.id != 0) {
      const double render_ms = std::max(
          0.0, std::chrono::duration<double, std::milli>(
                   now - frame.received_time).count());
      render_window_.Add(frame.id, render_ms, estimated);
    }
    std::lock_guard lock(mutex_);
    if (!estimated) {
      // Cumulative per-session presentations, deduped like the gap tracker:
      // redraws and out-of-order delivery must not count twice.
      if (frame.id > last_presented_id_) {
        last_presented_id_ = frame.id;
        // Inter-presentation gap: the actual elapsed monotonic time since the
        // previous counted fresh presentation of this stream. Out-of-order
        // callback delivery can move `now` backwards across frames; like the
        // windowed gap tracker, such a delivery is not a real (negative)
        // interval, so the endpoint only advances forward and the spurious
        // interval is skipped. Sub-millisecond intervals are recorded as-is.
        if (last_present_time_ && now > *last_present_time_) {
          inter_present_.Add(std::chrono::duration<double, std::milli>(
                                 now - *last_present_time_)
                                 .count());
        }
        if (!first_present_time_) first_present_time_ = now;
        if (!last_present_time_ || now > *last_present_time_) {
          last_present_time_ = now;
        }
        if (frames_presented_total_.fetch_add(1, std::memory_order_relaxed) == 0)
          first_presented_time_ns_.store(
              std::chrono::duration_cast<std::chrono::nanoseconds>(
                  now.time_since_epoch()).count(), std::memory_order_relaxed);
        RecordPresentedWallSecond(now);
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
    decode_window_.Reset();
    render_window_.Reset();
    std::lock_guard lock(mutex_);
    auto tail = TakeDiagnosticsLocked(now, true);
    samples_.clear();
    last_id_ = issued_id_;  // Invalidate frames still held by a renderer.
    // A reset restarts the inter-presentation measurement too: carrying the
    // pre-reset endpoint across the boundary would fabricate a gap spanning the
    // reset that no real pair of presentations produced.
    inter_present_ = {};
    first_present_time_.reset();
    last_present_time_.reset();
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
  // Mutable so const snapshot accessors (TakeInterPresentGapStats and friends)
  // can lock it; locking never changes observable state. Same idiom as
  // persecond_mutex_ and codec_mutex_ below.
  mutable std::mutex mutex_;
  uint64_t issued_id_ = 0;
  uint64_t last_id_ = 0;
  // Guards the cumulative presented counter against redraw/delivery duplicates.
  uint64_t last_presented_id_ = 0;
  // Session-cumulative inter-presentation gap state, guarded by mutex_ like the
  // presented counter it extends. last_present_time_ is the monotonic endpoint
  // of the most recently counted fresh presentation; unset until the first one,
  // so a session needs two presentations before any interval exists.
  // first_present_time_ is the endpoint of the very first counted presentation
  // and never moves; the span first->last is the exact window the measured
  // intervals cover, which is the denominator of the freezes-per-minute rate.
  // Reset() clears all three so a reset session restarts the measurement
  // rather than carrying a stale endpoint across the boundary.
  InterPresentGapStats inter_present_;
  std::optional<Clock::time_point> first_present_time_;
  std::optional<Clock::time_point> last_present_time_;
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

 public:
  // Zero-allocation atomic counters for render-stage observability. They are
  // independent of the rolling windows above so a renderer running on a
  // dedicated display-link/render thread can update them without touching the
  // latency mutex. All counters are monotonic per VideoLatencyStats instance
  // (which on macOS lives for the whole controller session) and are read once
  // at session teardown by LogVideoSessionSummary. Platform renderers that
  // never observe a stage leave its counter at zero; zero is a fact, not an
  // absence, so a reader can distinguish "never happened" from "not emitted".
  struct RenderStages {
    // Submit-side counters, written by the decode/network thread in
    // SubmitNativeFrame/SubmitNv12.
    std::atomic<uint64_t> submitted{0};       // frames accepted into a slot
    std::atomic<uint64_t> replaced{0};        // pending slot overwritten
    std::atomic<uint64_t> busy_drop{0};       // no free slot; frame dropped
    // Commit-side counters, written on the render queue / display-link thread.
    std::atomic<uint64_t> commits{0};         // command buffers committed
    std::atomic<uint64_t> commit_invisible{0}; // commits with visible_at_submit=false
    // Presented-handler counters, written by the drawable completion callback.
    std::atomic<uint64_t> present_cb{0};       // presentedHandler invocations
    std::atomic<uint64_t> present_zero_time{0};// presentedTime == 0 (never displayed)
    std::atomic<uint64_t> present_invisible{0};// visible_at_submit=false in handler
    std::atomic<uint64_t> presented{0};        // MarkPresented actually counted
    // Display-link pacing counters (macOS only).
    std::atomic<uint64_t> dl_ticks{0};        // CAMetalDisplayLink callbacks
    std::atomic<uint64_t> dl_pauses{0};       // link transitioned to paused
    std::atomic<uint64_t> dl_unpauses{0};     // unpause perform-blocks enqueued
    // Window occlusion transitions observed on the stream window (main thread).
    std::atomic<uint64_t> occlusion_transitions{0};
  };

  // Renderers do not own this object, so the stats object exposes the counter
  // block directly. Writers increment individual atomics; readers copy a
  // consistent snapshot field-by-field (uint64 atomics are tear-free on every
  // supported platform).
  RenderStages& MutableRenderStages() { return render_stages_; }
  const RenderStages& GetRenderStages() const { return render_stages_; }

 private:
  RenderStages render_stages_;
  // Per-second presentation histogram; separate mutex so the render hot
  // path never contends with harness snapshot reads. Record() holds mutex_
  // while calling RecordPresentedWallSecond, which takes only
  // persecond_mutex_; the snapshot reader takes only persecond_mutex_, so
  // there is no lock-order inversion.
  mutable std::mutex persecond_mutex_;
  int64_t persecond_first_sec_ = -1;
  std::vector<uint32_t> persecond_counts_;
  // Decoded-resolution change series, same steady-clock-second epoch as the
  // fps histogram; guarded by persecond_mutex_ as documented above.
  std::vector<ResolutionEvent> resolution_series_;
  // Per-stage rolling windows. Deliberately OUTSIDE mutex_: the decode
  // callback thread writes decode_window_ once per frame before touching
  // mutex_ at all, and the render/present callback writes render_window_
  // before acquiring mutex_ in Record(). Each window carries its own tiny
  // internal mutex, so the hot frame path never serializes the decode and
  // render stages against each other or behind the much larger pipeline
  // state lock. Reset() clears both so a fresh session never reads a stage
  // sample produced before the reset boundary.
  StageWindow decode_window_;
  StageWindow render_window_;
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
