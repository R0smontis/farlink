/*
 * @Author: Farlink contributors
 * @Date: 2026-11-27
 * Copyright (c) 2026 by Farlink contributors.
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Privacy cover style checks. Covers the pure helpers in
 * privacy_cover_style.h (string round-trip, the exact migration resolution
 * ConfigCenter::Load runs, aspect-fill crop math). When built through xmake
 * with CROSSDESK_TEST_CONFIG_CENTER the same file additionally drives the
 * real ConfigCenter INI round-trip; standalone clang++ builds skip that
 * section because ConfigCenter links the autostart/daemon stack.
 */

#include "privacy/privacy_cover_style.h"

#include <cmath>
#include <iostream>
#include <string>
#include <tuple>

#ifdef CROSSDESK_TEST_CONFIG_CENTER
#include "config_center.h"
#include <filesystem>
#include <fstream>
#include <unistd.h>
#endif

namespace {

using crossdesk::AspectFillRect;
using crossdesk::ComputeAspectFillRect;
using crossdesk::MigratePrivacyCoverStyle;
using crossdesk::PrivacyCoverStyle;
using crossdesk::PrivacyCoverStyleFromString;
using crossdesk::PrivacyCoverStyleToString;
using crossdesk::PrivacyCoverStyleValid;
using crossdesk::ResolvePrivacyCoverStyle;

int failures = 0;

void Expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

bool Close(double a, double b) { return std::fabs(a - b) < 1e-9; }

void TestStringRoundTrip() {
  Expect(PrivacyCoverStyleFromString("branded") == PrivacyCoverStyle::kBranded,
         "branded parses to kBranded");
  Expect(PrivacyCoverStyleFromString("black") == PrivacyCoverStyle::kBlack,
         "black parses to kBlack");
  Expect(PrivacyCoverStyleFromString("wallpaper") == PrivacyCoverStyle::kWallpaper,
         "wallpaper parses to kWallpaper");
  Expect(std::string(PrivacyCoverStyleToString(PrivacyCoverStyle::kBranded)) == "branded",
         "kBranded formats as branded");
  Expect(std::string(PrivacyCoverStyleToString(PrivacyCoverStyle::kBlack)) == "black",
         "kBlack formats as black");
  Expect(std::string(PrivacyCoverStyleToString(PrivacyCoverStyle::kWallpaper)) == "wallpaper",
         "kWallpaper formats as wallpaper");
  for (auto style : {PrivacyCoverStyle::kBranded, PrivacyCoverStyle::kBlack,
                     PrivacyCoverStyle::kWallpaper}) {
    Expect(PrivacyCoverStyleFromString(PrivacyCoverStyleToString(style)) == style,
           "every style survives the string round-trip");
  }
  Expect(PrivacyCoverStyleFromString("") == PrivacyCoverStyle::kBranded,
         "empty string falls back to branded");
  Expect(PrivacyCoverStyleFromString("neon-pink") == PrivacyCoverStyle::kBranded,
         "unknown strings fall back to branded");
  Expect(PrivacyCoverStyleValid(0) && PrivacyCoverStyleValid(1) &&
             PrivacyCoverStyleValid(2),
         "0..2 are valid style values");
  Expect(!PrivacyCoverStyleValid(3) && !PrivacyCoverStyleValid(-1),
         "out-of-range style values are rejected");
}

void TestLegacyMigration() {
  // A pre-style caller only sets black_screen. The enum stays at its zero
  // default and the boolean must still select the black cover.
  Expect(ResolvePrivacyCoverStyle(0, true) == PrivacyCoverStyle::kBlack,
         "legacy black_screen=true migrates to kBlack");
  Expect(ResolvePrivacyCoverStyle(0, false) == PrivacyCoverStyle::kBranded,
         "legacy black_screen=false stays branded");
  // Once the enum carries a real choice it is authoritative over the bool.
  Expect(ResolvePrivacyCoverStyle(2, false) == PrivacyCoverStyle::kWallpaper,
         "explicit wallpaper wins over black_screen=false");
  Expect(ResolvePrivacyCoverStyle(1, false) == PrivacyCoverStyle::kBlack,
         "explicit black wins over black_screen=false");
  Expect(ResolvePrivacyCoverStyle(2, true) == PrivacyCoverStyle::kWallpaper,
         "explicit wallpaper even wins over a stale black_screen=true");
  Expect(ResolvePrivacyCoverStyle(99, true) == PrivacyCoverStyle::kBlack,
         "garbage enum values defer to the legacy boolean");
  Expect(ResolvePrivacyCoverStyle(99, false) == PrivacyCoverStyle::kBranded,
         "garbage enum alone falls back to branded");

  // The persisted-profile resolution ConfigCenter::Load runs verbatim.
  Expect(MigratePrivacyCoverStyle(nullptr, false) == PrivacyCoverStyle::kBranded,
         "missing key + clean profile keeps the branded cover");
  Expect(MigratePrivacyCoverStyle(nullptr, true) == PrivacyCoverStyle::kBlack,
         "missing key + legacy black_screen=true migrates to the black cover");
  Expect(MigratePrivacyCoverStyle("", true) == PrivacyCoverStyle::kBlack,
         "empty key still migrates through the legacy boolean");
  Expect(MigratePrivacyCoverStyle("wallpaper", false) == PrivacyCoverStyle::kWallpaper,
         "persisted wallpaper key is authoritative");
  Expect(MigratePrivacyCoverStyle("wallpaper", true) == PrivacyCoverStyle::kWallpaper,
         "persisted wallpaper key overrides a stale legacy boolean");
  Expect(MigratePrivacyCoverStyle("black", false) == PrivacyCoverStyle::kBlack,
         "persisted black key is authoritative");
  Expect(MigratePrivacyCoverStyle("branded", true) == PrivacyCoverStyle::kBranded,
         "an explicit branded key beats even the legacy boolean");
  Expect(MigratePrivacyCoverStyle("neon", true) == PrivacyCoverStyle::kBlack,
         "unknown strings must not clobber the migrated black default");
  Expect(MigratePrivacyCoverStyle("neon", false) == PrivacyCoverStyle::kBranded,
         "unknown strings on a clean profile keep the branded default");
}

void TestAspectFill() {
  // Exact aspect match reproduces the full source with no crop.
  {
    const AspectFillRect r = ComputeAspectFillRect(1920, 1080, 1280, 720);
    Expect(Close(r.x, 0) && Close(r.y, 0) && Close(r.w, 1920) && Close(r.h, 1080),
           "exact aspect match must keep the full frame");
  }
  // Source wider than destination: crop left/right symmetrically, full height.
  {
    const AspectFillRect r = ComputeAspectFillRect(2560, 1080, 1920, 1080);
    Expect(Close(r.h, 1080), "wide source keeps full height");
    Expect(Close(r.w, 1920), "wide source crops to destination width ratio");
    Expect(Close(r.x, (2560 - 1920) / 2) && Close(r.y, 0),
           "wide source crop is horizontally centered");
  }
  // Source taller than destination: crop top/bottom symmetrically, full width.
  {
    const AspectFillRect r = ComputeAspectFillRect(1920, 2160, 1920, 1080);
    Expect(Close(r.w, 1920), "tall source keeps full width");
    Expect(Close(r.h, 1080), "tall source crops to destination height ratio");
    Expect(Close(r.x, 0) && Close(r.y, (2160 - 1080) / 2),
           "tall source crop is vertically centered");
  }
  // Scaled destinations preserve the crop ratio (5K wallpaper on a 1440p screen).
  {
    const AspectFillRect r = ComputeAspectFillRect(5120, 2880, 2560, 1440);
    Expect(Close(r.x, 0) && Close(r.y, 0) && Close(r.w, 5120) && Close(r.h, 2880),
           "same-aspect scaling keeps the full frame");
  }
  {
    // 21:9 wallpaper on a 16:9 screen.
    const AspectFillRect r = ComputeAspectFillRect(3440, 1440, 1920, 1080);
    const double visible_w = 1920.0 / (1080.0 / 1440.0);
    Expect(Close(r.h, 1440) && Close(r.w, visible_w) &&
               Close(r.x, (3440 - visible_w) / 2) && Close(r.y, 0),
           "ultrawide on 16:9 crops the sides symmetrically");
  }
  // Degenerate input never crashes and yields a zero rect.
  for (auto [sw, sh, dw, dh] :
       {std::tuple<double, double, double, double>{0, 100, 100, 100},
        {100, 0, 100, 100}, {100, 100, 0, 100}, {100, 100, 100, 0},
        {-5, 100, 100, 100}, {100, 100, -5, 100}, {0, 0, 0, 0}}) {
    const AspectFillRect r = ComputeAspectFillRect(sw, sh, dw, dh);
    Expect(Close(r.x, 0) && Close(r.y, 0) && Close(r.w, 0) && Close(r.h, 0),
           "degenerate input must yield a zero rect");
  }
}

#ifdef CROSSDESK_TEST_CONFIG_CENTER
using crossdesk::ConfigCenter;

void SeedIni(const std::string& path, const std::string& body) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << body;
}

void TestConfigPersistence(const std::string& config) {
  namespace fs = std::filesystem;

  // 1. Style round-trip through the INI file.
  fs::remove(config);
  {
    ConfigCenter writer(config);
    if (writer.SetPrivacyCoverStyle(
            static_cast<int>(PrivacyCoverStyle::kWallpaper)) != 0) {
      std::cerr << "FAIL: SetPrivacyCoverStyle(wallpaper) must succeed\n";
      ++failures;
    }
    Expect(writer.IsPrivacyBlackScreen() == false,
           "wallpaper style must keep the legacy black flag clear");
  }
  {
    ConfigCenter reader(config);
    Expect(reader.GetPrivacyCoverStyle() ==
               static_cast<int>(PrivacyCoverStyle::kWallpaper),
           "wallpaper style must survive a reload");
    Expect(reader.IsPrivacyBlackScreen() == false,
           "reloaded wallpaper style must keep the black flag clear");
  }

  // 2. Black style keeps the legacy boolean in sync for downgraded builds.
  {
    ConfigCenter writer(config);
    if (writer.SetPrivacyCoverStyle(static_cast<int>(PrivacyCoverStyle::kBlack)) != 0) {
      std::cerr << "FAIL: SetPrivacyCoverStyle(black) must succeed\n";
      ++failures;
    }
    Expect(writer.IsPrivacyBlackScreen(),
           "black style must set the legacy black flag");
  }
  {
    ConfigCenter reader(config);
    Expect(reader.GetPrivacyCoverStyle() == static_cast<int>(PrivacyCoverStyle::kBlack),
           "black style must survive a reload");
    Expect(reader.IsPrivacyBlackScreen(),
           "reloaded black style must report the legacy black flag");
  }

  // 3. Legacy migration: a pre-style profile with only the boolean set.
  SeedIni(config, "[Settings]\nprivacy_black_screen = true\n");
  {
    ConfigCenter reader(config);
    Expect(reader.GetPrivacyCoverStyle() == static_cast<int>(PrivacyCoverStyle::kBlack),
           "legacy black_screen=true profile migrates to the black cover");
    Expect(reader.IsPrivacyBlackScreen(),
           "legacy migration keeps the black flag set");
  }
  SeedIni(config, "[Settings]\nprivacy_black_screen = false\n");
  {
    ConfigCenter reader(config);
    Expect(reader.GetPrivacyCoverStyle() ==
               static_cast<int>(PrivacyCoverStyle::kBranded),
           "legacy black_screen=false profile keeps the branded cover");
  }

  // 4. Fresh profiles default to the historical branded cover.
  fs::remove(config);
  {
    ConfigCenter fresh(config);
    Expect(fresh.GetPrivacyCoverStyle() ==
               static_cast<int>(PrivacyCoverStyle::kBranded),
           "fresh profile must default to the branded cover");
    Expect(!fresh.IsPrivacyBlackScreen(),
           "fresh profile must default the black flag off");
  }

  // 5. The legacy toggle drives the style for old UI flows.
  {
    ConfigCenter writer(config);
    if (writer.SetPrivacyBlackScreen(true) != 0) {
      std::cerr << "FAIL: SetPrivacyBlackScreen(true) must succeed\n";
      ++failures;
    }
    Expect(writer.GetPrivacyCoverStyle() == static_cast<int>(PrivacyCoverStyle::kBlack),
           "legacy toggle-on must select the black cover");
    if (writer.SetPrivacyBlackScreen(false) != 0) {
      std::cerr << "FAIL: SetPrivacyBlackScreen(false) must succeed\n";
      ++failures;
    }
    Expect(writer.GetPrivacyCoverStyle() ==
               static_cast<int>(PrivacyCoverStyle::kBranded),
           "legacy toggle-off must return to the branded cover");
  }

  // 6. Invalid style values are rejected without corrupting persistence.
  {
    ConfigCenter writer(config);
    Expect(writer.SetPrivacyCoverStyle(99) != 0,
           "out-of-range style must be rejected");
    Expect(writer.SetPrivacyCoverStyle(-1) != 0,
           "negative style must be rejected");
  }

  // 7. An unknown persisted string keeps the migrated default.
  SeedIni(config,
          "[Settings]\nprivacy_black_screen = true\nprivacy_cover_style = neon\n");
  {
    ConfigCenter reader(config);
    Expect(reader.GetPrivacyCoverStyle() == static_cast<int>(PrivacyCoverStyle::kBlack),
           "unknown style string must not clobber the migrated black default");
  }
}
#endif  // CROSSDESK_TEST_CONFIG_CENTER

}  // namespace

int main() {
  TestStringRoundTrip();
  TestLegacyMigration();
  TestAspectFill();

#ifdef CROSSDESK_TEST_CONFIG_CENTER
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() /
      ("crossdesk_cover_style_test_" + std::to_string(::getpid()));
  fs::create_directories(dir);
  TestConfigPersistence((dir / "config.ini").string());
  std::error_code ec;
  fs::remove_all(dir, ec);
#endif

  if (failures) {
    std::cerr << failures << " failure(s)\n";
    return 1;
  }
  std::cout << "privacy_cover_style_test: all checks passed\n";
  return 0;
}
