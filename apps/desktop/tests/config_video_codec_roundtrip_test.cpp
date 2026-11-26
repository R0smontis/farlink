/*
 * @Author: Farlink contributors
 * @Date: 2026-11-26
 * Copyright (c) 2026 by Farlink contributors.
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Round-trip checks for persisted video settings. The codec selector writes
 * through ConfigCenter::SetVideoEncodeFormat; this test drives the same
 * setter/getter pair against a temporary INI file and confirms:
 *   1. write -> restart-equivalent reload -> the value is restored;
 *   2. an out-of-range value already in the file falls back to the safe
 *      default rather than an invalid codec;
 *   3. on Apple hosts a fresh profile (no key) defaults to HEVC, elsewhere
 *      to H.264.
 * No GUI, no network: ConfigCenter is plain INI I/O.
 */

#include "config_center.h"

#include <video_frame_rate.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>

namespace {

using crossdesk::ConfigCenter;

int failures = 0;

void Expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

// Write a raw INI body so we can seed values the setter would never produce
// (e.g. an out-of-range codec) and confirm how Load() sanitizes them.
void SeedIni(const std::string& path, const std::string& body) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << body;
}

}  // namespace

int main() {
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() /
      ("crossdesk_codec_test_" + std::to_string(::getpid()));
  fs::create_directories(dir);
  const std::string config = (dir / "config.ini").string();

  // 1. Write then reload: the persisted value survives a restart.
  {
    ConfigCenter writer(config);
    if (writer.SetVideoEncodeFormat(MiniRtcVideoCodec::H264) != 0) {
      std::cerr << "FAIL: SetVideoEncodeFormat(H264) must succeed\n";
      ++failures;
    }
    if (writer.SetVideoEncodeFormat(MiniRtcVideoCodec::H265) != 0) {
      std::cerr << "FAIL: SetVideoEncodeFormat(H265) must succeed\n";
      ++failures;
    }
  }
  {
    ConfigCenter reader(config);
    Expect(reader.GetVideoEncodeFormat() == MiniRtcVideoCodec::H265,
           "written H265 must be restored after reload");
  }
  {
    // Overwrite with H264 and confirm the persisted value flips.
    ConfigCenter writer(config);
    writer.SetVideoEncodeFormat(MiniRtcVideoCodec::H264);
  }
  {
    ConfigCenter reader(config);
    Expect(reader.GetVideoEncodeFormat() == MiniRtcVideoCodec::H264,
           "written H264 must be restored after reload");
  }

  // 2. An invalid stored value sanitizes to the SAME platform default a
  //    missing key uses — never a hard-coded H.264. On Apple that is HEVC,
  //    elsewhere H.264. One platform default, one behaviour for both cases.
#if defined(__APPLE__)
  const MiniRtcVideoCodec kPlatformDefault = MiniRtcVideoCodec::H265;
#else
  const MiniRtcVideoCodec kPlatformDefault = MiniRtcVideoCodec::H264;
#endif
  SeedIni(config, "[Settings]\nvideo_encode_format = 99\n");
  {
    ConfigCenter reader(config);
    Expect(reader.GetVideoEncodeFormat() == kPlatformDefault,
           "out-of-range stored codec must fall back to the platform default");
  }

  // 3. A fresh profile (key absent) uses the same platform default.
  fs::remove(config);
  {
    ConfigCenter fresh(config);
    Expect(fresh.GetVideoEncodeFormat() == kPlatformDefault,
           "fresh profile must use the platform default");
  }

  // 4. Frame-rate preference round-trip. The stored preference is the user's
  //    remembered choice and is NEVER rewritten by a capability/display clamp:
  //    it round-trips verbatim, while the session-start rate is derived from it
  //    through EffectiveVideoFrameRate(preference, capability, display).
  fs::remove(config);
  {
    ConfigCenter writer(config);
    if (writer.SetVideoFrameRatePreference(144) != 0) {
      std::cerr << "FAIL: SetVideoFrameRatePreference(144) must succeed\n";
      ++failures;
    }
  }
  {
    ConfigCenter reader(config);
    // Stock peer (no high-fps) + the persisted 144 preference: the session
    // opens at 60 (the highest tier a stock peer offers), but the preference
    // itself stays 144 so a later capable peer/display can use it.
    Expect(reader.GetVideoFrameRatePreference() == 144,
           "stored 144 preference must round-trip unclamped");
    Expect(crossdesk::EffectiveVideoFrameRate(reader.GetVideoFrameRatePreference(),
                                              /*peer_high_fps=*/false, 60.0) == 60,
           "stock peer must clamp the session to 60 even with a 144 preference");
    // Capable peer + 120 Hz panel: session opens at 120.
    Expect(crossdesk::EffectiveVideoFrameRate(reader.GetVideoFrameRatePreference(),
                                              /*peer_high_fps=*/true, 120.0) == 120,
           "capable peer + 120 Hz panel must yield a 120 session");
  }

  // 5. Invalid / missing frame-rate preference falls back to the 60 default.
  SeedIni(config, "[Settings]\nvideo_frame_rate_preference = 999\n");
  {
    ConfigCenter reader(config);
    Expect(reader.GetVideoFrameRatePreference() == 60,
           "invalid frame-rate preference must fall back to 60");
  }
  fs::remove(config);
  {
    ConfigCenter fresh(config);
    Expect(fresh.GetVideoFrameRatePreference() == 60,
           "fresh profile frame-rate preference must default to 60");
  }

  // 6. A non-tier value the setter rejects must not be written.
  fs::remove(config);
  {
    ConfigCenter writer(config);
    Expect(writer.SetVideoFrameRatePreference(75) != 0,
           "SetVideoFrameRatePreference must reject a non-tier value");
    Expect(writer.GetVideoFrameRatePreference() == 60,
           "rejected frame-rate preference must leave the default in place");
  }

  // 7. True color is opt-in. Loading and saving either choice must preserve it.
  SeedIni(config, "[Settings]\nvideo_true_color_444 = 2\n");
  Expect(!ConfigCenter(config).IsVideoTrueColor444(), "invalid 4:4:4 preference must be off");
  fs::remove(config);
  {
    ConfigCenter fresh(config);
    Expect(!fresh.IsVideoTrueColor444(), "fresh profile must use 4:2:0");
    Expect(fresh.SetVideoTrueColor444(true) == 0, "must persist 4:4:4");
  }
  {
    ConfigCenter saved(config);
    Expect(saved.IsVideoTrueColor444(), "4:4:4 must survive reload");
    Expect(saved.Save() == 0, "full save must succeed");
  }
  {
    ConfigCenter saved(config);
    Expect(saved.IsVideoTrueColor444(), "full save must retain 4:4:4");
    Expect(saved.SetVideoTrueColor444(false) == 0, "must persist 4:2:0");
  }
  Expect(!ConfigCenter(config).IsVideoTrueColor444(), "opting out must survive reload");

  fs::remove_all(dir);
  if (failures == 0) {
    std::cout << "config_video_codec_roundtrip_test: all checks passed\n";
    return 0;
  }
  std::cerr << "config_video_codec_roundtrip_test: " << failures << " failure(s)\n";
  return 1;
}
