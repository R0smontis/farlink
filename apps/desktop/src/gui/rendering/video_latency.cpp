/* @Author: Farlink contributors
 * @Date: 2026-11-26
 * Copyright (c) 2026 by Farlink contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "rendering/video_latency.h"

#include <cstdio>

#include "rd_log.h"

namespace crossdesk {

namespace {

// Milliseconds between two steady-clock points, or -1 when the anchor was
// never recorded (e.g. no Ice-connected transition was observed).
double MsSince(VideoLatencyStats::Clock::time_point anchor,
               VideoLatencyStats::Clock::time_point now) {
  if (anchor == VideoLatencyStats::Clock::time_point{}) return -1;
  return std::chrono::duration<double, std::milli>(now - anchor).count();
}

double MsSinceEpochNs(int64_t anchor_ns,
                      VideoLatencyStats::Clock::time_point now) {
  if (anchor_ns < 0) return -1;
  const auto anchor =
      VideoLatencyStats::Clock::time_point(std::chrono::nanoseconds(anchor_ns));
  return std::chrono::duration<double, std::milli>(now - anchor).count();
}

// Serialize the per-second presentation histogram compactly: the bare counts
// comma-separated behind the steady-clock epoch of bucket zero, e.g.
// "fps_seconds=start=12345 counts=58,59,0,57". A zero inside the list is a
// real zero-presentation second (freeze); the histogram is cumulative since
// the first presented frame, so the session-end emission alone carries the
// whole series and no intermediate ticks are needed.
std::string SerializePerSecond(const VideoLatencyStats::PerSecondSnapshot& snap) {
  if (snap.first_sec < 0 || snap.counts.empty()) return {};
  std::string out = "start=" + std::to_string(snap.first_sec) + " counts=";
  for (size_t i = 0; i < snap.counts.size(); ++i) {
    if (i) out += ',';
    out += std::to_string(snap.counts[i]);
  }
  return out;
}

// Serialize decoded-resolution changes as "sec:WxH" entries in the same
// steady-clock-second epoch as the fps series, so a reader can align the
// two timelines exactly.
std::string SerializeResolutionSeries(
    const std::vector<VideoLatencyStats::ResolutionEvent>& events) {
  std::string out;
  for (size_t i = 0; i < events.size(); ++i) {
    if (i) out += ',';
    out += std::to_string(events[i].sec) + ":" + std::to_string(events[i].width) +
           "x" + std::to_string(events[i].height);
  }
  return out;
}

// Serialize the session-cumulative inter-presentation gap metric (D8) as a
// single compact key=value block for the machine-readable teardown line. All
// values are facts about the local monotonic endpoint clock of this process:
//   intervals        count of distinct successive-presentation intervals;
//                    zero when fewer than two frames were ever presented, in
//                    which case every rate/mean below is meaningless and the
//                    reader must treat the metric as absent rather than zero.
//   mean_ms          mean interval across all intervals.
//   max_ms           largest single interval (exact).
//   p95_max_ms       upper bound on the 95th-percentile interval, derived
//                    from a fixed bucket histogram; it is a bucket upper edge,
//                    so it never under-estimates the true p95 but may sit one
//                    bucket-width above it.
//   freeze_count     intervals strictly greater than threshold_ms. An interval
//                    of exactly threshold_ms is not a freeze.
//   freeze_mean_ms   mean duration of freeze intervals only.
//   freeze_max_ms    largest freeze interval.
//   freezes_per_min  freeze_count normalised to the measured presentation
//                    window: the exact monotonic span from the first to the
//                    last counted presentation. This is the only defensible
//                    denominator — intervals can only exist between two
//                    presentations, so neither the wall-clock session length
//                    (which includes connect/negotiation before the first
//                    frame) nor any trailing silence after the last frame
//                    (which the metric cannot observe) belongs in it.
//   window_ms        that first-to-last-presentation span, reported so the
//                    reader can re-normalise or sanity-check the rate.
// Returns an empty string when there are no intervals, so the caller emits an
// explicit "none" marker instead of a misleading all-zero block.
std::string SerializeInterPresentGap(
    const VideoLatencyStats::InterPresentGapStats& gap,
    const VideoLatencyStats::InterPresentWindow& window) {
  if (gap.intervals == 0) return {};
  double window_ms = 0;
  if (window.first && window.last && *window.last > *window.first) {
    window_ms = std::chrono::duration<double, std::milli>(*window.last -
                                                          *window.first)
                    .count();
  }
  // freezes/minute over the measured window; zero window (a single instant)
  // cannot support a rate, so report 0 rather than divide by zero.
  const double freezes_per_min =
      window_ms > 0 ? gap.gap_count * 60000.0 / window_ms : 0;
  std::string out = "inter_present intervals=" + std::to_string(gap.intervals);
  char buf[256];
  std::snprintf(buf, sizeof(buf),
                " mean_ms=%.2f max_ms=%.2f p95_max_ms=%.2f"
                " freeze_count=%llu freeze_mean_ms=%.2f freeze_max_ms=%.2f"
                " freezes_per_min=%.3f window_ms=%.0f threshold_ms=%.0f",
                gap.MeanIntervalMs(), gap.interval_max_ms,
                gap.ApproxP95IntervalMs(),
                static_cast<unsigned long long>(gap.gap_count),
                gap.MeanFreezeMs(), gap.gap_max_ms, freezes_per_min, window_ms,
                VideoLatencyStats::kFrameGapThresholdMs);
  out += buf;
  return out;
}


// Serialize the per-stage decode/render rolling windows (D8 stage
// attribution) as a compact key=value block on the same machine-readable
// teardown line. Both windows cover the trailing ~second of the session, so
// they describe the steady-state pipeline the user actually experienced, not
// connect-time decoder warmup. mean/p95/max are exact order statistics of
// the 60-sample ring. A stage with zero recorded samples (backend never
// reported timestamps, or no frame rendered) serializes its mean as "none"
// so a parser can distinguish "unmeasured" from "zero cost".
std::string SerializeStageBreakdown(
    const VideoLatencyStats::StageSnapshot& stage) {
  char buf[320];
  const auto mean_or_none = [](double value, char* out, size_t size) {
    if (value < 0) {
      std::snprintf(out, size, "none");
    } else {
      std::snprintf(out, size, "%.2f", value);
    }
  };
  char decode_mean[24], decode_p95[24], decode_max[24];
  char render_mean[24], render_p95[24], render_max[24];
  mean_or_none(stage.decode.mean_ms, decode_mean, sizeof(decode_mean));
  mean_or_none(stage.decode.p95_ms, decode_p95, sizeof(decode_p95));
  mean_or_none(stage.decode.max_ms, decode_max, sizeof(decode_max));
  mean_or_none(stage.render.mean_ms, render_mean, sizeof(render_mean));
  mean_or_none(stage.render.p95_ms, render_p95, sizeof(render_p95));
  mean_or_none(stage.render.max_ms, render_max, sizeof(render_max));
  std::snprintf(buf, sizeof(buf),
                "stage decode_samples=%zu decode_mean_ms=%s decode_p95_ms=%s"
                " decode_max_ms=%s render_samples=%zu render_mean_ms=%s"
                " render_p95_ms=%s render_max_ms=%s",
                stage.decode.samples, decode_mean, decode_p95, decode_max,
                stage.render.samples, render_mean, render_p95, render_max);
  return buf;
}

}  // namespace

void LogVideoFirstFramePresented(const VideoLatencyStats& stats,
                                 VideoLatencyStats::Clock::time_point now) {
  const auto context = stats.GetSessionObservability();
  // An uninitialized context means this stats object never belonged to an
  // outgoing controller session; there is nothing meaningful to report.
  if (!context ||
      context->connect_time == VideoLatencyStats::Clock::time_point{}) {
    return;
  }
  const double since_connect_ms = MsSince(context->connect_time, now);
  const double since_ready_ms = MsSinceEpochNs(
      context->connected_since_epoch_ns.load(std::memory_order_relaxed), now);
  const int width = context->frame_width.load(std::memory_order_relaxed);
  const int height = context->frame_height.load(std::memory_order_relaxed);
  const std::string codec = context->LoadCodec();
  LOG_INFO(
      "First frame presented: remote={} since_connect_ms={} "
      "since_ready_ms={} resolution={}x{} codec={}",
      context->remote_id, static_cast<int64_t>(since_connect_ms),
      static_cast<int64_t>(since_ready_ms), width, height,
      codec.empty() ? "unknown" : codec);
}

void LogVideoSessionSummary(const VideoLatencyStats& stats,
                            VideoLatencyStats::Clock::time_point now,
                            const std::string& codec, int fps_tier) {
  const auto context = stats.GetSessionObservability();
  if (!context ||
      context->connect_time == VideoLatencyStats::Clock::time_point{}) {
    return;
  }
  if (context->summary_emitted.exchange(true, std::memory_order_acq_rel)) {
    return;
  }
  const double duration_s =
      std::chrono::duration<double>(now - context->connect_time).count();
  const uint64_t decoded = stats.FramesDecodedTotal();
  const uint64_t presented = stats.FramesPresentedTotal();
  // Anchor the average at the first presented frame: connect, negotiation
  // and decoder startup are not frame time, and counting them would dilute
  // the steady-state presentation rate the summary exists to measure. Falls
  // back to the full duration when nothing was ever presented.
  const int64_t first_ns = stats.FirstPresentedTimeNs();
  double fps_window_s = duration_s;
  if (first_ns != 0) {
    const auto first_tp = VideoLatencyStats::Clock::time_point(
        std::chrono::nanoseconds(first_ns));
    fps_window_s = std::chrono::duration<double>(now - first_tp).count();
  }
  const double avg_presented_fps =
      fps_window_s > 0 ? presented / fps_window_s : 0;
  // The transport report is authoritative; fall back to the mirrored copy for
  // sessions where the report never landed before teardown. Normalize the
  // implementation spelling ("VideoToolbox (H.265)") to the codec family the
  // first-frame line reports so both lines carry the same token.
  std::string resolved = codec;
  if (resolved.empty()) resolved = context->LoadCodec();
  if (!resolved.empty()) {
    const std::string normalized =
        VideoLatencyStats::SessionObservability::NormalizeCodecName(resolved);
    if (!normalized.empty()) resolved = normalized;
  }
  LOG_INFO(
      "Session summary (controller): remote={} duration_s={:.1f} "
      "frames_decoded={} frames_presented={} avg_presented_fps={:.1f} "
      "codec={} fps_tier={}",
      context->remote_id, duration_s, decoded, presented, avg_presented_fps,
      resolved.empty() ? "unknown" : resolved, fps_tier);
  // Machine-readable per-session series, emitted once at teardown on the
  // same consume-once guard as the human summary. fps_seconds counts are
  // true presentations per steady-clock second (zero = a freeze second);
  // resolution_series lists decoded-resolution changes as sec:WxH in the
  // same epoch so the two timelines align exactly.
  const auto snap = stats.TakePerSecondSnapshot();
  const std::string fps_series = SerializePerSecond(snap);
  const std::string resolutions = SerializeResolutionSeries(
      stats.ResolutionSeries());
  // Session-cumulative inter-presentation gap metric, on the same line so a
  // reader parsing one teardown record sees every per-session series together.
  // Unlike the coarse fps_seconds histogram — which buckets a whole wall-clock
  // second and can only flag a zero-presentation second as a freeze — this is
  // the actual elapsed monotonic time between successive presentations, so it
  // resolves sub-second freezes and measures real freeze durations. It covers
  // only intervals between two presentations: silence after the final frame is
  // outside its window and stays unmeasured, exactly like the histogram.
  const std::string inter_present = SerializeInterPresentGap(
      stats.TakeInterPresentGapStats(), stats.TakeInterPresentWindow());
  // Per-stage attribution (D8): the decode/render rolling windows identify
  // whether the trailing second of the session was bound on decode or
  // render. Emitted on the same line so one teardown record carries every
  // machine-readable series; the network/jitter legs are already on the
  // transport side of the same log stream.
  const std::string stage = SerializeStageBreakdown(stats.TakeStageSnapshot());
  LOG_INFO(
      "Session metrics (controller): remote={} {} resolutions={} {} {}",
      context->remote_id,
      fps_series.empty() ? "fps_seconds=none" : "fps_seconds " + fps_series,
      resolutions.empty() ? "none" : resolutions,
      inter_present.empty() ? "inter_present=none" : inter_present,
      stage);
  // Render-stage funnel counters, emitted as a separate companion line so the
  // machine-readable metrics line above stays byte-compatible with every
  // existing parser. Each counter is monotonic per session and zero means the
  // stage never fired — comparing them pairwise pinpoints where frames stop
  // being counted:
  //   submitted>0 but commits small      → display link paused / render starved
  //   commits high but present_zero_time → window occluded, never displayed
  //   present_cb high but presented low  → visible_at_submit=false flip
  //   occlusion_transitions rising       → window occlusion churn during session
  const auto& rs = stats.GetRenderStages();
  LOG_INFO(
      "Video render stages (controller): remote={} submitted={} replaced={}"
      " busy_drop={} commits={} commit_invisible={} present_cb={}"
      " present_zero_time={} present_invisible={} presented={}"
      " dl_ticks={} dl_pauses={} dl_unpauses={} occlusion_transitions={}",
      context->remote_id,
      rs.submitted.load(std::memory_order_relaxed),
      rs.replaced.load(std::memory_order_relaxed),
      rs.busy_drop.load(std::memory_order_relaxed),
      rs.commits.load(std::memory_order_relaxed),
      rs.commit_invisible.load(std::memory_order_relaxed),
      rs.present_cb.load(std::memory_order_relaxed),
      rs.present_zero_time.load(std::memory_order_relaxed),
      rs.present_invisible.load(std::memory_order_relaxed),
      rs.presented.load(std::memory_order_relaxed),
      rs.dl_ticks.load(std::memory_order_relaxed),
      rs.dl_pauses.load(std::memory_order_relaxed),
      rs.dl_unpauses.load(std::memory_order_relaxed),
      rs.occlusion_transitions.load(std::memory_order_relaxed));
}

}  // namespace crossdesk
