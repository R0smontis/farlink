/*
 * Copyright (c) 2026 Farlink contributors
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */

// Direct VideoToolbox encode-throughput probe. Drives the real encoder with
// raw NV12 frames at a controlled rate and measures the submit->output-callback
// latency of every frame, isolating the encoder from the transport, pacing and
// capture so per-resolution encode cost can be compared cleanly. Prints one
// JSON line per resolution.
//
// This exists to answer why some resolutions encode slower than others despite
// fewer pixels: it reports the per-frame encode latency distribution directly
// instead of inferring it from transport drops.

#if defined(__APPLE__)

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include <algorithm>

#include "media_codec.h"
#include "raw_frame.h"
#include "clock/system_clock.h"
#include "video_toolbox_encoder.h"

namespace {

struct Result {
  std::vector<int64_t> latencies_us;
  std::vector<int64_t> completion_us;
  std::atomic<int> completed{0};
  std::atomic<int> max_in_flight{0};
  std::mutex mutex;
  // Session-configuration readback captured before teardown; empty when the
  // encoder exposes no live session.
  std::string vt_config;
};

// Encode `frames` NV12 frames of width x height at roughly `fps`, measuring the
// submit->output latency of each frame by tagging the submission time in a
// parallel array indexed by a rolling sequence. Returns false on encoder
// failure.
bool ProbeResolution(int width, int height, int fps, int frames, bool hevc,
                     Result* result) {
  auto clock = std::make_shared<minirtc::SystemClock>();
  minirtc::VideoToolboxEncoder encoder(
      clock, hevc ? minirtc::VideoCodecType::H265 : minirtc::VideoCodecType::H264);

  minirtc::MediaCodecConfig config;
  config.init_width = width;
  config.init_height = height;
  config.max_frame_rate = fps;
  config.average_bitrate = 20000000;
  config.max_bitrate = 30000000;
  config.video_degradation_preference =
      MiniRtcVideoDegradationPreference::MaintainFrameRate;
  if (encoder.Init(config) != 0) {
    std::fprintf(stderr, "encoder init failed at %dx%d\n", width, height);
    return false;
  }

  const size_t frame_size = static_cast<size_t>(width) * height * 3 / 2;
  std::vector<uint8_t> nv12(frame_size);
  // Moderately compressible noise so rate control stays in budget.
  for (int row = 0; row < height; ++row) {
    for (int col = 0; col < width; ++col) {
      nv12[static_cast<size_t>(row) * width + col] =
          static_cast<uint8_t>(64 + (((col + row) >> 5) & 0x3f));
    }
  }
  std::memset(nv12.data() + width * height, 0x80, frame_size - width * height);

  std::vector<int64_t> submit_time(frames, 0);
  std::atomic<int> next_seq{0};
  auto steady = [] {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
  };

  const int64_t interval_us = 1000000 / fps;
  int64_t next_slot = steady();
  for (int seq = 0; seq < frames; ++seq) {
    // The input must stress the encoder honestly: a coarse scrolling block
    // pattern moves as one rigid translation, which the hardware motion
    // estimator collapses into near-zero residual work, overstating
    // throughput. Production desktop content is mixed — static regions, local
    // text-like detail, fine high-frequency structure — so reproduce that with
    // a gradient base, a moving sharp-edged block grid, and a per-pixel
    // pseudo-random high-frequency component that re-randomizes every frame.
    // Deterministic per (row, col, seq) hashing keeps generation cheap while
    // forcing real motion-compensated residual coding on every macroblock.
    for (int row = 0; row < height; row += 2) {
      uint8_t* yline = nv12.data() + static_cast<size_t>(row) * width;
      for (int col = 0; col < width; ++col) {
        const int base = 64 + (((col + row) >> 5) & 0x3f);
        const uint32_t hash = static_cast<uint32_t>(
            (col * 2654435761u) ^ (row * 40503u) ^ (seq * 6291469u));
        const int detail = static_cast<int>((hash >> 8) & 0x1f) - 16;
        int value = base + detail;
        const int bx = (col + seq * 5) >> 6;
        const int by = (row + seq * 3) >> 6;
        if ((((bx * 31 + by * 17) >> 2) & 7) == 0 &&
            ((col + seq * 5) & 63) < 40 && ((row + seq * 3) & 63) < 22)
          value = ((bx + by) & 1 ? 200 : 30) + detail;
        yline[col] = static_cast<uint8_t>(value < 16 ? 16 : value > 235 ? 235 : value);
      }
    }
    minirtc::RawFrame frame(nv12.data(), frame_size, width, height);
    const int64_t submit = steady();
    submit_time[seq] = submit;
    int in_flight = encoder.FramesInFlight();
    int cur_max = result->max_in_flight.load();
    while (in_flight > cur_max &&
           !result->max_in_flight.compare_exchange_weak(cur_max, in_flight)) {
    }
    encoder.Encode(frame, [result, submit](const minirtc::EncodedFrame& encoded) {
      const int64_t done = std::chrono::duration_cast<std::chrono::microseconds>(
                               std::chrono::steady_clock::now().time_since_epoch())
                               .count();
      if (encoded.Size() > 0) {
        std::lock_guard<std::mutex> lock(result->mutex);
        result->latencies_us.push_back(done - submit);
        result->completion_us.push_back(done);
      }
      result->completed.fetch_add(1);
      return 0;
    });
    next_slot += interval_us;
    const int64_t remaining = next_slot - steady();
    if (remaining > 0) std::this_thread::sleep_for(std::chrono::microseconds(remaining));
  }

  // Drain: wait for completions or timeout.
  const int64_t drain_deadline = steady() + 3000000;
  while (result->completed.load() < frames && steady() < drain_deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  // Capture the live session configuration while the session still exists;
  // the encoder teardown below would invalidate it.
  result->vt_config = encoder.DumpSessionConfiguration();
  return true;
}

int Percentile(std::vector<int64_t> values, int pct) {
  if (values.empty()) return 0;
  std::sort(values.begin(), values.end());
  size_t idx = static_cast<size_t>((pct / 100.0) * (values.size() - 1));
  return static_cast<int>(values[idx] / 1000);
}

// Percentile keeping sub-millisecond precision: returns microseconds. The
// whole investigation lives in the 4-12ms spacing range, so integer-ms
// rounding would hide exactly the fast/slow split being measured.
int PercentileUs(std::vector<int64_t> values, int pct) {
  if (values.empty()) return 0;
  std::sort(values.begin(), values.end());
  size_t idx = static_cast<size_t>((pct / 100.0) * (values.size() - 1));
  return static_cast<int>(values[idx]);
}

}  // namespace

int main(int argc, char** argv) {
  int fps = 144;
  int frames = 200;
  bool hevc = true;
  std::vector<std::pair<int, int>> resolutions;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--fps" && i + 1 < argc) fps = std::atoi(argv[++i]);
    else if (arg == "--frames" && i + 1 < argc) frames = std::atoi(argv[++i]);
    else if (arg == "--h264") hevc = false;
    else if (arg == "--res" && i + 1 < argc) {
      int w = 0, h = 0;
      if (std::sscanf(argv[++i], "%dx%d", &w, &h) == 2) resolutions.push_back({w, h});
    }
  }
  if (resolutions.empty()) resolutions = {{1920, 1080}, {1512, 982}};

  for (auto [w, h] : resolutions) {
    Result result;
    if (!ProbeResolution(w, h, fps, frames, hevc, &result)) continue;
    std::vector<int64_t> lat;
    std::vector<int64_t> comp;
    {
      std::lock_guard<std::mutex> lock(result.mutex);
      lat = result.latencies_us;
      comp = result.completion_us;
    }
    // True encoder throughput, isolated from submit cadence. When the encoder
    // is the bottleneck, frames queue inside VideoToolbox and output
    // completions reflect back-to-back hardware encode time. When the encoder
    // is faster than the feed, completions track the (equal) submit cadence
    // and throughput can only be reported as a lower bound. The discriminator
    // is queueing: the submit-to-output latency of a saturated pipeline grows
    // with pipeline depth, so encode-service-time ≈ inter-completion gap of
    // frames whose latency exceeds one frame interval. Empirically the two
    // regimes separate cleanly on this machine: saturated frames cluster at
    // 35-95ms latency, cadence-fed frames at 7-15ms.
    int steady_gap_us = 0;
    int saturated_gap_us = 0;
    int64_t queued_frames = 0;
    if (comp.size() > 8) {
      // Pair completions with their latencies by index: both vectors are
      // appended under the same mutex in each output callback, so they stay
      // aligned frame-for-frame in callback order.
      std::vector<std::pair<int64_t, int64_t>> pairs;
      const size_t n = std::min(comp.size(), lat.size());
      pairs.reserve(n);
      for (size_t i = 0; i < n; ++i) pairs.emplace_back(comp[i], lat[i]);
      std::sort(pairs.begin(), pairs.end(),
                [](const auto& a, const auto& b) { return a.first < b.first; });
      std::vector<int64_t> gaps;
      std::vector<int64_t> saturated;
      const int64_t frame_interval_us = 1000000 / fps;
      const size_t lo = pairs.size() / 4, hi = pairs.size() * 3 / 4;
      for (size_t i = lo + 1; i < hi; ++i) {
        const int64_t gap = pairs[i].first - pairs[i - 1].first;
        gaps.push_back(gap);
        // A frame whose latency exceeds 1.5 frame intervals was waiting in
        // the encoder queue, so its completion spacing measures pure encode
        // service time rather than submit cadence.
        if (pairs[i].second > frame_interval_us * 3 / 2 &&
            pairs[i - 1].second > frame_interval_us * 3 / 2) {
          saturated.push_back(gap);
          ++queued_frames;
        }
      }
      steady_gap_us = PercentileUs(gaps, 50);
      if (!saturated.empty()) saturated_gap_us = PercentileUs(saturated, 50);
    }
    std::printf(
        "{\"width\":%d,\"height\":%d,\"fps\":%d,\"codec\":\"%s\","
        "\"completed\":%d,\"enc_p50_ms\":%d,\"enc_p95_ms\":%d,\"enc_p99_ms\":%d,"
        "\"max_in_flight\":%d,\"steady_gap_us\":%d,"
        "\"saturated_gap_us\":%d,\"saturated_samples\":%lld}\n",
        w, h, fps, hevc ? "h265" : "h264", result.completed.load(),
        Percentile(lat, 50), Percentile(lat, 95), Percentile(lat, 99),
        result.max_in_flight.load(), steady_gap_us,
        saturated_gap_us, static_cast<long long>(queued_frames));
    std::fflush(stdout);
    // Session-configuration readback for the throughput-anomaly comparison:
    // identical fields across resolutions prove VideoToolbox applies the same
    // session config everywhere and throughput differences come from the size
    // itself, not divergent driver-side configuration.
    const std::string dump = result.vt_config;
    if (!dump.empty()) {
      std::printf("{\"width\":%d,\"height\":%d,\"vt_config\":\"%s\"}\n", w, h,
                  dump.c_str());
      std::fflush(stdout);
    }
  }
  return 0;
}

#else
int main() { return 0; }
#endif
