/*
 * @Author: Farlink contributors
 * @Date: 2026-11-27
 * Copyright (c) 2026 by Farlink contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

// End-to-end emission test for the D8 inter-presentation gap metric. Unlike
// inter_present_gap_test (which validates the accumulator math in isolation),
// this drives the real Record() path with fake monotonic timestamps AND the
// real LogVideoSessionSummary() teardown path, then reads back the actual log
// file the process wrote and asserts the emitted "inter_present" line carries
// the values the fed timestamps imply. This is the closest deterministic
// verification possible without a networked session: the exact production
// serializer and emission guard run, only the clock and the peer are fake.

#include "rendering/video_latency.h"

#include <chrono>
#include <cmath>
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
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

Clock::time_point At(double ms) {
  return Clock::time_point(
      std::chrono::duration_cast<Clock::duration>(
          std::chrono::duration<double, std::milli>(ms)));
}

VideoLatencyFrame Frame(std::shared_ptr<VideoLatencyStats> stats, uint64_t id) {
  VideoLatencyFrame frame;
  frame.stats = std::move(stats);
  frame.id = id;
  frame.capture_valid = false;
  return frame;
}

void Present(std::shared_ptr<VideoLatencyStats> stats, uint64_t id, double ms) {
  stats->Record(Frame(stats, id), At(ms), false);
}

// Extract the value of key=... from the inter_present block of the metrics
// line, or empty when absent.
std::string Field(const std::string& line, const std::string& key) {
  const std::string token = key + "=";
  const auto pos = line.find(token);
  if (pos == std::string::npos) return {};
  const auto start = pos + token.size();
  const auto end = line.find_first_of(" \n", start);
  return line.substr(start, end == std::string::npos ? end : end - start);
}

}  // namespace

int main() {
  namespace fs = std::filesystem;
  const fs::path log_dir = fs::temp_directory_path() / "farlink_ip_emit_test";
  std::error_code ec;
  fs::remove_all(log_dir, ec);
  fs::create_directories(log_dir, ec);
  crossdesk::InitLogger(log_dir.string(), "emit_test");

  auto stats = std::make_shared<VideoLatencyStats>();
  auto context = std::make_shared<VideoLatencyStats::SessionObservability>();
  context->remote_id = "emit-test";
  // Non-zero connect time: a zero time_point is the uninitialized sentinel
  // that makes the summary bail out before emitting anything.
  context->connect_time = At(1.0);
  stats->SetSessionObservability(context);

  // Cadence: 100 presentations 16ms apart (1600ms span) then one 400ms freeze
  // and a final frame, all on the fake clock. 99 normal intervals of 16ms,
  // then a 400ms interval (the single freeze), so 100 intervals total.
  uint64_t id = 1;
  double t = 1000.0;
  Present(stats, id++, t);
  for (int i = 0; i < 99; ++i) {
    t += 16.0;
    Present(stats, id++, t);
  }
  t += 400.0;  // the freeze interval
  Present(stats, id++, t);
  // t is now 1000 + 99*16 + 400 = 1000 + 1584 + 400 = 2984; window = 1984ms.

  // Teardown at the last presentation time emits the single summary pair.
  crossdesk::LogVideoSessionSummary(*stats, At(t), "H265", 60);

  // Read back the emitted log file.
  std::ifstream in((log_dir / "emit_test.log").string());
  std::stringstream buffer;
  buffer << in.rdbuf();
  const std::string log = buffer.str();

  const auto pos = log.find("Session metrics (controller)");
  Expect(pos != std::string::npos, "Session metrics line emitted");
  if (pos == std::string::npos) return 1;
  const auto line_end = log.find('\n', pos);
  const std::string line = log.substr(pos, line_end - pos);

  Expect(line.find("inter_present intervals=") != std::string::npos,
         "inter_present block present in metrics line: " + line);
  Expect(Field(line, "intervals") == "100",
         "intervals == 100, got " + Field(line, "intervals"));
  Expect(Field(line, "freeze_count") == "1",
         "freeze_count == 1, got " + Field(line, "freeze_count"));
  Expect(Field(line, "freeze_max_ms") == "400.00",
         "freeze_max_ms == 400.00, got " + Field(line, "freeze_max_ms"));
  Expect(Field(line, "max_ms") == "400.00",
         "max_ms == 400.00, got " + Field(line, "max_ms"));
  Expect(Field(line, "threshold_ms") == "200",
         "threshold_ms == 200, got " + Field(line, "threshold_ms"));
  // window = last - first = 2984 - 1000 = 1984ms.
  Expect(Field(line, "window_ms") == "1984",
         "window_ms == 1984, got " + Field(line, "window_ms"));
  // freezes/min = 1 * 60000 / 1984 = 30.242.
  Expect(Field(line, "freezes_per_min") == "30.242",
         "freezes_per_min == 30.242, got " + Field(line, "freezes_per_min"));
  // mean of 99*16 + 1*400 over 100 intervals = (1584 + 400)/100 = 19.84.
  Expect(Field(line, "mean_ms") == "19.84",
         "mean_ms == 19.84, got " + Field(line, "mean_ms"));

  // A second teardown call must be a no-op (consume-once guard): the metrics
  // line appears exactly once even if the summary is requested again.
  crossdesk::LogVideoSessionSummary(*stats, At(t), "H265", 60);
  std::ifstream in2((log_dir / "emit_test.log").string());
  std::stringstream buffer2;
  buffer2 << in2.rdbuf();
  const std::string log2 = buffer2.str();
  size_t count = 0;
  for (size_t p = log2.find("Session metrics (controller)"); p != std::string::npos;
       p = log2.find("Session metrics (controller)", p + 1)) {
    ++count;
  }
  Expect(count == 1, "metrics line emitted exactly once (consume-once), got " +
                         std::to_string(count));

  // A session with a single presentation must emit the explicit none marker
  // rather than a misleading all-zero block.
  {
    auto single = std::make_shared<VideoLatencyStats>();
    auto ctx2 = std::make_shared<VideoLatencyStats::SessionObservability>();
    ctx2->remote_id = "single";
    ctx2->connect_time = At(1.0);
    single->SetSessionObservability(ctx2);
    Present(single, 1, 500.0);
    crossdesk::LogVideoSessionSummary(*single, At(500.0), "H265", 60);
  }
  std::ifstream in3((log_dir / "emit_test.log").string());
  std::stringstream buffer3;
  buffer3 << in3.rdbuf();
  const std::string log3 = buffer3.str();
  Expect(log3.find("inter_present=none") != std::string::npos,
         "single-presentation session emits inter_present=none");

  fs::remove_all(log_dir, ec);
  if (failures == 0) {
    std::cout << "inter_present_emission_test: all assertions passed\n";
    return 0;
  }
  std::cerr << "inter_present_emission_test: " << failures
            << " assertion(s) failed\n";
  return 1;
}
