/*
 * @Author: Farlink contributors
 * @Date: 2026-11-27
 * Copyright (c) 2026 by Farlink contributors.
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * In-session bitrate cap presets (commercial control-center parity). Pure logic:
 * index <-> bits-per-second conversion, validation and clamping for both the
 * UI selector and the persisted per-device preference. No transport, no UI —
 * the wire encoding is MiniRtcUpdateVideoBitrateLimit / the subscription
 * max_bitrate_bps field, where 0 restores normal congestion control.
 */

#ifndef CROSSDESK_GUI_BITRATE_CAP_PRESET_H_
#define CROSSDESK_GUI_BITRATE_CAP_PRESET_H_

#include <algorithm>
#include <string>
#include <vector>

namespace crossdesk {

struct BitrateTier {
  std::string name;
  int mbps = 0;  // 0 = Auto (no cap); 1..100 = explicit Mbps ceiling
};

struct BitrateCapPreset {
  // Default tiers matching the reference control center:
  // Auto (no cap), 原画 30M, 超清 14M, 高清 8M.
  static constexpr int kAutoIndex = 0;
  static constexpr int kNoCapBps = 0;
  static constexpr int kMinExplicitMbps = 1;
  static constexpr int kMaxExplicitMbps = 100;
  static constexpr int kWireMaxBps = kMaxExplicitMbps * 1'000'000;
  static constexpr int kMaxCustomTiers = 12;

  static std::vector<BitrateTier> DefaultTiers() {
    return {
        {"自动（原画）", 0},
        {"原画", 30},
        {"超清", 14},
        {"高清", 8},
    };
  }

  static bool ValidMbps(int mbps) {
    return mbps == 0 || (mbps >= kMinExplicitMbps && mbps <= kMaxExplicitMbps);
  }

  static bool ValidBps(int bps) {
    if (bps == kNoCapBps) return true;
    if (bps < kMinExplicitMbps * 1'000'000 || bps > kWireMaxBps) return false;
    return (bps % 1'000'000) == 0;
  }

  static std::string FormatTierLabel(const BitrateTier& tier) {
    if (tier.mbps <= 0) return tier.name.empty() ? "自动（原画）" : tier.name;
    const std::string base = tier.name.empty() ? "自定义" : tier.name;
    return base + " " + std::to_string(tier.mbps) + "M";
  }

  static int IndexForBps(const std::vector<BitrateTier>& tiers, int bps) {
    if (bps <= 0) return kAutoIndex;
    const int mbps = bps / 1'000'000;
    for (int i = 0; i < static_cast<int>(tiers.size()); ++i) {
      if (tiers[i].mbps == mbps && tiers[i].mbps > 0) return i;
    }
    return kAutoIndex;
  }

  static int BpsForIndex(const std::vector<BitrateTier>& tiers, int index) {
    if (index < 0 || index >= static_cast<int>(tiers.size())) return kNoCapBps;
    const int mbps = tiers[index].mbps;
    if (mbps <= 0) return kNoCapBps;
    return std::clamp(mbps, kMinExplicitMbps, kMaxExplicitMbps) * 1'000'000;
  }

  // Backward-compatible helpers against the default tier list.
  static constexpr int kCount = 4;
  static bool ValidIndex(int index) { return index >= 0 && index < kCount; }
  static int BpsForIndex(int index) { return BpsForIndex(DefaultTiers(), index); }
  static int IndexForBps(int bps) { return IndexForBps(DefaultTiers(), bps); }

  static int ClampWireBps(int bps) {
    if (bps < 0) return kNoCapBps;
    if (bps > kWireMaxBps) return kWireMaxBps;
    return bps;
  }
};

}  // namespace crossdesk

#endif  // CROSSDESK_GUI_BITRATE_CAP_PRESET_H_
