/*
 * @Author: Farlink contributors
 * @Date: 2026-11-26
 * Copyright (c) 2026 by Farlink contributors.
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Round-trip proof for the persisted video-codec setting, the one piece of
 * user-facing video configuration the fork both exposes in the UI and stores.
 * The codec selector in the main settings dialog (main_window.slint) writes
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

  // 2. An invalid stored value sanitizes to the safe H.264 default. The codec
  //    is validated on load as 0 <= codec <= H265, so an out-of-range value
  //    must not survive.
  SeedIni(config, "[Settings]\nvideo_encode_format = 99\n");
  {
    ConfigCenter reader(config);
    Expect(reader.GetVideoEncodeFormat() == MiniRtcVideoCodec::H264,
           "out-of-range stored codec must fall back to H264");
  }

  // 3. A fresh profile (key absent) uses the platform default: HEVC on Apple,
  //    H.264 elsewhere. Either way it must be a valid, decodable codec.
  fs::remove(config);
  {
    ConfigCenter fresh(config);
#if defined(__APPLE__)
    Expect(fresh.GetVideoEncodeFormat() == MiniRtcVideoCodec::H265,
           "Apple fresh profile must default to HEVC");
#else
    Expect(fresh.GetVideoEncodeFormat() == MiniRtcVideoCodec::H264,
           "non-Apple fresh profile must default to H264");
#endif
  }

  fs::remove_all(dir);
  if (failures == 0) {
    std::cout << "config_video_codec_roundtrip_test: all checks passed\n";
    return 0;
  }
  std::cerr << "config_video_codec_roundtrip_test: " << failures << " failure(s)\n";
  return 1;
}
