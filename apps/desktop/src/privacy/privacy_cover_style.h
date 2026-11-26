/* Copyright (c) 2026 by DI JUNKUN, All Rights Reserved. */
#ifndef _PRIVACY_COVER_STYLE_H_
#define _PRIVACY_COVER_STYLE_H_

#include <cstring>
#include <string>

namespace crossdesk {

// Cross-platform privacy cover appearance. kBranded keeps the historical
// logo plus unlock-instructions cover, kBlack is a plain black screen and
// kWallpaper imitates the host desktop wallpaper so bystanders see nothing
// unusual. Values are persisted in config.ini; never renumber them.
enum class PrivacyCoverStyle : int {
  kBranded = 0,
  kBlack = 1,
  kWallpaper = 2,
};

inline const char* PrivacyCoverStyleToString(PrivacyCoverStyle style) {
  switch (style) {
    case PrivacyCoverStyle::kBlack:
      return "black";
    case PrivacyCoverStyle::kWallpaper:
      return "wallpaper";
    case PrivacyCoverStyle::kBranded:
    default:
      return "branded";
  }
}

// Accepts the persisted spellings plus a few forgiving aliases. Anything
// unknown yields kBranded, the historical default cover.
inline PrivacyCoverStyle PrivacyCoverStyleFromString(const std::string& value) {
  if (value == "black") return PrivacyCoverStyle::kBlack;
  if (value == "wallpaper" || value == "desktop" || value == "fake-desktop")
    return PrivacyCoverStyle::kWallpaper;
  return PrivacyCoverStyle::kBranded;
}

inline bool PrivacyCoverStyleValid(int style) {
  return style == static_cast<int>(PrivacyCoverStyle::kBranded) ||
         style == static_cast<int>(PrivacyCoverStyle::kBlack) ||
         style == static_cast<int>(PrivacyCoverStyle::kWallpaper);
}

// Legacy profiles predate the style enum and only carry the black_screen
// boolean. The enum field is authoritative once set to a non-default style;
// otherwise the boolean still selects the black cover, so old callers that
// only know black_screen keep working and old config files migrate exactly.
inline PrivacyCoverStyle ResolvePrivacyCoverStyle(int cover_style,
                                                  bool legacy_black_screen) {
  if (PrivacyCoverStyleValid(cover_style) &&
      cover_style != static_cast<int>(PrivacyCoverStyle::kBranded))
    return static_cast<PrivacyCoverStyle>(cover_style);
  if (legacy_black_screen) return PrivacyCoverStyle::kBlack;
  return PrivacyCoverStyle::kBranded;
}

// One-shot persisted-profile resolution: the stored style string is
// authoritative once recognized; missing, empty or unknown strings fall back
// to the legacy black_screen boolean so pre-style profiles migrate exactly.
// ConfigCenter::Load() is a thin INI read over this helper.
inline PrivacyCoverStyle MigratePrivacyCoverStyle(const char* persisted_style,
                                                  bool legacy_black_screen) {
  if (persisted_style && persisted_style[0]) {
    const PrivacyCoverStyle parsed = PrivacyCoverStyleFromString(persisted_style);
    const bool recognized = parsed != PrivacyCoverStyle::kBranded ||
                            std::string(persisted_style) == "branded";
    if (recognized) return parsed;
  }
  return legacy_black_screen ? PrivacyCoverStyle::kBlack
                             : PrivacyCoverStyle::kBranded;
}

struct AspectFillRect {
  double x = 0;
  double y = 0;
  double w = 0;
  double h = 0;
};

// Center-crop rectangle in source-pixel coordinates for aspect-fill scaling,
// matching how macOS draws its desktop picture in "Fill Screen" mode: scale
// the source until both destination dimensions are covered, then crop the
// overflow symmetrically. Degenerate input yields a zero rect.
inline AspectFillRect ComputeAspectFillRect(double src_w, double src_h,
                                            double dst_w, double dst_h) {
  AspectFillRect rect;
  if (src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) return rect;
  const double scale_x = dst_w / src_w;
  const double scale_y = dst_h / src_h;
  const double scale = scale_x > scale_y ? scale_x : scale_y;
  rect.w = dst_w / scale;
  rect.h = dst_h / scale;
  // Clamp fp noise: an exact aspect match must reproduce the full source.
  if (rect.w > src_w) rect.w = src_w;
  if (rect.h > src_h) rect.h = src_h;
  rect.x = (src_w - rect.w) / 2;
  rect.y = (src_h - rect.h) / 2;
  return rect;
}

}  // namespace crossdesk

#endif  // _PRIVACY_COVER_STYLE_H_
