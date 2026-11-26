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
  // Pre-generate a small ring of distinct realistic frames once, so frame
  // generation cost is amortized out of the measured feed loop. Uncapped feed
  // (below) submits back-to-back; if generation stayed inline it, not the
  // encoder, would become the bottleneck at large sizes and the probe would
  // measure memcpy/hash throughput instead of encode throughput. 8 distinct
  // frames are enough that the motion estimator cannot collapse the stream
  // into a single repeated frame, while staying small enough to stay cache
  // resident. Content per frame is the production-like mix used before: a
  // gradient base, a moving sharp-edged block grid, and a per-pixel
  // pseudo-random high-frequency component.
  constexpr int kFrameRing = 8;
  std::vector<std::vector<uint8_t>> ring(kFrameRing,
                                         std::vector<uint8_t>(frame_size));
  for (int seq = 0; seq < kFrameRing; ++seq) {
    std::vector<uint8_t>& nv12 = ring[seq];
    for (int row = 0; row < height; row += 1) {
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
    std::memset(nv12.data() + static_cast<size_t>(width) * height, 0x80,
                frame_size - static_cast<size_t>(width) * height);
  }

  auto steady = [] {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
  };

  // Uncapped saturated feed: keep up to kGate frames in flight and submit a
  // new frame the instant a completion frees a slot, so the hardware encoder
  // pipeline is continuously full at every resolution. The completion spacing
  // then measures pure back-to-back encode service time rather than any
  // submit cadence — this is what removes the lower-bound (">") ambiguity the
  // fps-paced feed had at sizes the encoder could keep up with. The gate of 2
  // mirrors the production transport admission cap (kMaxEncodeFramesInFlight),
  // so the measurement reproduces the pipeline depth the real stream sees.
  constexpr int kGate = 2;
  for (int seq = 0; seq < frames; ++seq) {
    // Block until a slot is free. Spin with a short yield: completion gaps are
    // 1-12ms, so sleeping longer than a fraction of that would leave the
    // pipeline idle and understate throughput. The encoder itself never
    // blocks in Encode(); backpressure is the in-flight count.
    while (encoder.FramesInFlight() >= kGate) {
      std::this_thread::yield();
    }
    std::vector<uint8_t>& nv12 = ring[seq % kFrameRing];
    minirtc::RawFrame frame(nv12.data(), frame_size, width, height);
    int in_flight = encoder.FramesInFlight();
    int cur_max = result->max_in_flight.load();
    while (in_flight > cur_max &&
           !result->max_in_flight.compare_exchange_weak(cur_max, in_flight)) {
    }
    encoder.Encode(frame, [result](const minirtc::EncodedFrame& encoded) {
      const int64_t done = std::chrono::duration_cast<std::chrono::microseconds>(
                               std::chrono::steady_clock::now().time_since_epoch())
                               .count();
      if (encoded.Size() > 0) {
        std::lock_guard<std::mutex> lock(result->mutex);
        result->completion_us.push_back(done);
      }
      result->completed.fetch_add(1);
      return 0;
    });
  }

  // Drain: wait for completions or timeout.
  const int64_t drain_deadline = steady() + 5000000;
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
// whole investigation lives in the 1-12ms spacing range, so integer-ms
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
    std::vector<int64_t> comp;
    {
      std::lock_guard<std::mutex> lock(result.mutex);
      comp = result.completion_us;
    }
    // Uncapped saturated feed keeps the encoder pipeline continuously full, so
    // the inter-completion spacing in steady state is the pure back-to-back
    // encode service time at this resolution. Report it directly (microseconds
    // per frame) together with the derived throughput in fps and megapixels
    // per second; trim the first and last quartile of completions to drop the
    // pipeline fill/drain transient.
    int gap_p50_us = 0, gap_min_us = 0, gap_max_us = 0;
    if (comp.size() > 8) {
      std::sort(comp.begin(), comp.end());
      std::vector<int64_t> gaps;
      const size_t lo = comp.size() / 4, hi = comp.size() * 3 / 4;
      for (size_t i = lo + 1; i < hi; ++i) gaps.push_back(comp[i] - comp[i - 1]);
      if (!gaps.empty()) {
        gap_p50_us = PercentileUs(gaps, 50);
        std::sort(gaps.begin(), gaps.end());
        gap_min_us = static_cast<int>(gaps.front());
        gap_max_us = static_cast<int>(gaps.back());
      }
    }
    const double mpx = static_cast<double>(w) * h / 1e6;
    const double throughput_fps = gap_p50_us > 0 ? 1e6 / gap_p50_us : 0.0;
    const double throughput_mpx_s = throughput_fps * mpx;
    std::printf(
        "{\"width\":%d,\"height\":%d,\"fps\":%d,\"codec\":\"%s\","
        "\"completed\":%d,\"gap_p50_us\":%d,\"gap_min_us\":%d,\"gap_max_us\":%d,"
        "\"throughput_fps\":%.2f,\"throughput_mpx_s\":%.1f,"
        "\"max_in_flight\":%d}\n",
        w, h, fps, hevc ? "h265" : "h264", result.completed.load(),
        gap_p50_us, gap_min_us, gap_max_us, throughput_fps, throughput_mpx_s,
        result.max_in_flight.load());
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
