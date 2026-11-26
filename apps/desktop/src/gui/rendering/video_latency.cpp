/* @Author: Farlink contributors
 * @Date: 2026-11-26
 * Copyright (c) 2026 by Farlink contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "rendering/video_latency.h"

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
  // sessions where the report never landed before teardown.
  std::string resolved = codec;
  if (resolved.empty()) resolved = context->LoadCodec();
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
  LOG_INFO(
      "Session metrics (controller): remote={} {} resolutions={}",
      context->remote_id,
      fps_series.empty() ? "fps_seconds=none" : "fps_seconds " + fps_series,
      resolutions.empty() ? "none" : resolutions);
}

}  // namespace crossdesk
