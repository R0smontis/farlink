/*
 * @Author: Farlink contributors
 * @Date: 2026-11-27
 * Copyright (c) 2026 by Farlink contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

// Regression test for the wave-6 session-teardown ordering defect: the
// connection-status disconnect handler used to run VideoLatencyStats::Reset()
// before any consumer emitted LogVideoSessionSummary, so the once-per-session
// metrics line always read wiped rolling windows (decode_samples=0,
// render_samples=0, inter_present=none) even though the frame path had fed
// the windows for the whole session. The fix emits the summary BEFORE the
// reset in the disconnect handler; ResetRemoteSessionResources' later
// emission is consumed as a no-op. This test drives both orderings against
// the real emission path and asserts:
//   1. summary-before-reset yields populated windows in the log line;
//   2. reset-before-summary (the old ordering) reproduces the empty windows,
//      pinning exactly what the ordering contract protects;
//   3. a second emission after the first is consumed and adds no second line.

#include "rendering/video_latency.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>

#include "rd_log.h"

namespace {

using crossdesk::VideoLatencyFrame;
using crossdesk::VideoLatencyStats;
using Clock = VideoLatencyStats::Clock;

int failures = 0;

void Expect(bool condition, const std::string& message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAIL: " << message << "\n";
  }
}

Clock::time_point At(double ms) {
  return Clock::time_point(
      std::chrono::microseconds(static_cast<int64_t>(ms * 1000.0)));
}

std::string Field(const std::string& line, const std::string& key) {
  const std::string token = key + "=";
  const auto pos = line.find(token);
  if (pos == std::string::npos) return {};
  const auto start = pos + token.size();
  const auto end = line.find_first_of(" \n", start);
  return line.substr(start, end == std::string::npos ? end : end - start);
}

std::shared_ptr<VideoLatencyStats> MakeFedStats(int frames) {
  auto stats = std::make_shared<VideoLatencyStats>();
  auto context = std::make_shared<VideoLatencyStats::SessionObservability>();
  context->remote_id = "ordering-test";
  context->connect_time = At(1.0);
  stats->SetSessionObservability(context);
  double t = 1000.0;
  for (int i = 0; i < frames; ++i) {
    t += 16.0;
    // Feed the decode window exactly like the production decode callback:
    // FrameWithDecode with a positive decode latency.
    auto frame = stats->FrameWithDecode(
        /*captured_us=*/static_cast<uint64_t>(t * 1000.0) - 5000,
        /*local_now_us=*/static_cast<int64_t>(t * 1000.0),
        /*decode_ms=*/3.5, /*now=*/At(t));
    // Feed inter-present + render windows through the real record path.
    stats->Record(std::move(frame), At(t), false);
  }
  return stats;
}

std::string ReadLog(const std::filesystem::path& path) {
  std::ifstream in(path.string());
  std::stringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

std::string MetricsLine(const std::string& log, int occurrence) {
  size_t pos = std::string::npos;
  size_t from = 0;
  for (int i = 0; i < occurrence; ++i) {
    pos = log.find("Session metrics (controller)", from);
    if (pos == std::string::npos) return {};
    from = pos + 1;
  }
  const auto end = log.find('\n', pos);
  return log.substr(pos, end - pos);
}

}  // namespace

int main() {
  namespace fs = std::filesystem;
  const fs::path log_dir = fs::temp_directory_path() / "farlink_ordering_test";
  std::error_code ec;
  fs::remove_all(log_dir, ec);
  fs::create_directories(log_dir, ec);
  crossdesk::InitLogger(log_dir.string(), "ordering_test");
  const fs::path log_path = log_dir / "ordering_test.log";

  // Case 1: the fixed ordering — summary BEFORE reset — must carry the
  // rolling-window data the session accumulated.
  {
    auto stats = MakeFedStats(120);
    crossdesk::LogVideoSessionSummary(*stats, At(3000.0), "H265", 60);
    stats->Reset();
  }

  std::string log = ReadLog(log_path);
  const std::string line1 = MetricsLine(log, 1);
  Expect(!line1.empty(), "case 1: metrics line emitted");
  Expect(Field(line1, "decode_samples") == "60",
         "case 1: decode_samples == 60, got " + Field(line1, "decode_samples"));
  Expect(line1.find("decode_mean_ms=none") == std::string::npos,
         "case 1: decode window populated: " + line1);
  Expect(line1.find("inter_present=none") == std::string::npos,
         "case 1: inter_present populated: " + line1);

  // Case 2: the pre-fix ordering — reset BEFORE summary — must show the
  // wiped windows, pinning the regression this ordering contract prevents.
  {
    auto stats = MakeFedStats(120);
    stats->Reset();
    crossdesk::LogVideoSessionSummary(*stats, At(3000.0), "H265", 60);
  }

  log = ReadLog(log_path);
  const std::string line2 = MetricsLine(log, 2);
  Expect(!line2.empty(), "case 2: metrics line emitted");
  Expect(Field(line2, "decode_samples") == "0",
         "case 2: decode_samples == 0 after reset, got " +
             Field(line2, "decode_samples"));
  Expect(line2.find("inter_present=none") != std::string::npos,
         "case 2: inter_present wiped after reset: " + line2);

  // Case 3: consume-once — the second emission (from the later teardown
  // path) must not produce another summary line for the same session.
  {
    auto stats = MakeFedStats(30);
    crossdesk::LogVideoSessionSummary(*stats, At(2000.0), "H265", 60);
    stats->Reset();
    crossdesk::LogVideoSessionSummary(*stats, At(2100.0), "H265", 60);
  }

  log = ReadLog(log_path);
  size_t count = 0;
  size_t from = 0;
  while ((from = log.find("Session metrics (controller)", from)) !=
         std::string::npos) {
    ++count;
    ++from;
  }
  Expect(count == 3,
         "exactly three metrics lines total (one per case), got " +
             std::to_string(count));

  if (failures != 0) {
    std::cerr << failures << " expectation(s) failed\n";
    return 1;
  }
  std::cout << "session_teardown_ordering_test: all expectations passed\n";
  return 0;
}
