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

namespace crossdesk {

struct BitrateCapPreset {
  // Ordered exactly as the selector row renders: index 0 is Auto (no cap —
  // the encoder/congestion ceiling applies, 80 Mbps by default), the rest
  // are explicit ceilings in Mbps.
  static constexpr int kCount = 5;
  static constexpr int kAutoIndex = 0;
  static constexpr int kMbps[kCount] = {0, 8, 20, 50, 80};

  // Wire representation of "no cap": MiniRtcUpdateVideoBitrateLimit treats 0
  // as "remove the limit", and the subscription max_bitrate_bps field uses
  // the same zero-restores-default convention.
  static constexpr int kNoCapBps = 0;

  // Highest negotiated value the peer side accepts on the wire
  // (preview_subscription.h rejects max_bitrate_bps above 100 Mbps).
  static constexpr int kWireMaxBps = 100'000'000;

  static bool ValidIndex(int index) { return index >= 0 && index < kCount; }

  // Bits per second for one selector index. Unknown or out-of-range input
  // maps to the Auto ceiling contract (0), never to an accidental cap.
  static int BpsForIndex(int index) {
    if (!ValidIndex(index)) return kNoCapBps;
    return kMbps[index] * 1'000'000;
  }

  // Selector index for a persisted bits-per-second value. 0 means Auto.
  // Unknown values (written by another build, or hand-edited) fall back to
  // Auto rather than silently inventing a cap the user never chose.
  static int IndexForBps(int bps) {
    if (bps <= 0) return kAutoIndex;
    for (int index = 1; index < kCount; ++index) {
      if (kMbps[index] * 1'000'000 == bps) return index;
    }
    return kAutoIndex;
  }

  // True when the value may be persisted or put on the wire: either no cap,
  // or exactly one of the explicit presets. Anything else (negative,
  // non-preset, above the wire maximum) is rejected before persistence.
  static bool ValidBps(int bps) {
    if (bps == kNoCapBps) return true;
    for (int index = 1; index < kCount; ++index) {
      if (kMbps[index] * 1'000'000 == bps) return true;
    }
    return false;
  }

  // Clamps a wire-bound value into the range the peer accepts. Negative
  // becomes no-cap; values above the wire maximum fold down to it. The
  // selector itself never produces these, but the subscription merge path
  // handles values negotiated before a cap choice existed.
  static int ClampWireBps(int bps) {
    if (bps < 0) return kNoCapBps;
    if (bps > kWireMaxBps) return kWireMaxBps;
    return bps;
  }
};

}  // namespace crossdesk

#endif  // CROSSDESK_GUI_BITRATE_CAP_PRESET_H_
