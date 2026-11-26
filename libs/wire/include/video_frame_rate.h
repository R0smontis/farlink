/*
 * Copyright (c) 2026 Farlink contributors
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef _VIDEO_FRAME_RATE_H_
#define _VIDEO_FRAME_RATE_H_

#include <optional>

namespace crossdesk {

// Capture/encode frame-rate tiers understood by the wire protocol, in
// ascending order. 30/60 are the only tiers a legacy peer accepts; the higher
// tiers require the peer to advertise supports_high_fps. The interactive
// subscription's fps marker is independent of these tiers.
inline constexpr int kVideoFrameRateTiers[] = {30, 60, 90, 120, 144};
inline constexpr int kVideoFrameRateTierCount =
    sizeof(kVideoFrameRateTiers) / sizeof(kVideoFrameRateTiers[0]);
inline constexpr int kMaxVideoFrameRate =
    kVideoFrameRateTiers[kVideoFrameRateTierCount - 1];

// True when the frame rate is one of the supported tiers.
inline bool IsValidVideoFrameRate(int frame_rate) {
  for (int index = 0; index < kVideoFrameRateTierCount; ++index)
    if (kVideoFrameRateTiers[index] == frame_rate) return true;
  return false;
}

// Highest tier the connection may use. Without the peer's high-frame-rate
// capability the wire format is limited to the legacy 30/60 values, so a
// capable local setting clamps down to 60 rather than being rejected.
inline int ClampVideoFrameRateToLegacy(int frame_rate) {
  return frame_rate <= 30 ? 30 : 60;
}

// Resolve the frame rate a sender may put on the wire for a peer. Tiers above
// 60 require the peer's high-frame-rate capability; without it the value
// clamps to the legacy 30/60 format so a stock peer sees exactly the bytes it
// understands today.
inline int NegotiatedVideoFrameRate(int frame_rate, bool peer_supports_high_fps) {
  if (!IsValidVideoFrameRate(frame_rate))
    return ClampVideoFrameRateToLegacy(frame_rate);
  if (!peer_supports_high_fps && frame_rate > 60)
    return 60;
  return frame_rate;
}

// Index into kVideoFrameRateTiers for a frame rate, used by the settings UI.
// Unknown values fall back to the 60 fps entry, matching the legacy binary
// 30/60 toggle where anything but 30 selected 60.
inline int VideoFrameRateIndex(int frame_rate) {
  for (int index = 0; index < kVideoFrameRateTierCount; ++index)
    if (kVideoFrameRateTiers[index] == frame_rate) return index;
  return 1;
}

// Frame rate for a settings-UI index; out-of-range indices select 60 fps,
// again matching the legacy non-30 behavior.
inline int VideoFrameRateForIndex(int index) {
  if (index < 0 || index >= kVideoFrameRateTierCount) return 60;
  return kVideoFrameRateTiers[index];
}

// Highest tier a display refresh rate (Hz) can actually present. Fractional
// rates round to their nominal tier — 59.94 (NTSC 60000/1001) rounds to 60,
// 119.88 to 120 — so a panel is credited with the tier it is sold as, not the
// tier just below its exact rate. A non-positive refresh rate means the rate
// is unknown and imposes no restriction, so it returns the top tier.
inline int MaxVideoFrameRateForRefresh(double refresh_hz) {
  if (!(refresh_hz > 0.0)) return kMaxVideoFrameRate;
  // Round half up: 59.94 -> 60, 119.88 -> 120, exact 60/90/120/144 unchanged.
  const long rounded = static_cast<long>(refresh_hz + 0.5);
  int best = kVideoFrameRateTiers[0];
  for (int index = 0; index < kVideoFrameRateTierCount; ++index) {
    if (kVideoFrameRateTiers[index] <= rounded) best = kVideoFrameRateTiers[index];
  }
  return best;
}

// Whether a tier is offered to the user. The 30/60 tiers are always available
// (they are the legacy baseline every peer accepts). A higher tier requires
// both that the peer advertised high-frame-rate support AND that the display
// the stream window is on can present it. display_refresh_hz <= 0 means the
// rate is unknown, which imposes no restriction beyond the peer capability.
inline bool VideoFrameRateTierOffered(int frame_rate, bool peer_supports_high_fps,
                                      double display_refresh_hz) {
  if (!IsValidVideoFrameRate(frame_rate)) return false;
  if (frame_rate <= 60) return true;
  if (!peer_supports_high_fps) return false;
  return frame_rate <= MaxVideoFrameRateForRefresh(display_refresh_hz);
}

// The effective tier to run at: the requested tier if it is offered, otherwise
// the highest tier that is. 60 is always offered, so the result is never
// invalid. This is the single decision point shared by session start (the
// persisted preference) and re-evaluation when the window changes displays.
inline int EffectiveVideoFrameRate(int requested, bool peer_supports_high_fps,
                                   double display_refresh_hz) {
  if (!IsValidVideoFrameRate(requested)) requested = 60;
  if (VideoFrameRateTierOffered(requested, peer_supports_high_fps, display_refresh_hz))
    return requested;
  for (int index = kVideoFrameRateTierCount - 1; index >= 0; --index) {
    const int tier = kVideoFrameRateTiers[index];
    if (VideoFrameRateTierOffered(tier, peer_supports_high_fps, display_refresh_hz))
      return tier;
  }
  return 60;
}

// Decide whether the connection should re-request its capture tier, and to
// what, when a constraint it depends on (display refresh or peer capability)
// has changed. This is the pure decision the stream sync loop drives; it is
// extracted here so the convergence behaviour is testable without the loop.
//
//   requested — the per-session tier the user asked for (seeded from the
//       stored preference at session start, replaced only by a chip choice in
//       that session). Never clamped; the source of intent.
//   active — the tier currently requested on the wire (video_settings).
//   pending / supported — whether a settings request is already in flight /
//       whether the peer accepts video-settings requests at all.
//   peer_supports_high_fps, display_refresh_hz — the constraints.
//
// Returns the tier to request, or nullopt when nothing should be sent.
//
// Convergence / no-loop / no-fight argument, in terms of state:
//   * The target is EffectiveVideoFrameRate(requested, …) — a pure function of
//     intent and constraint, independent of active. Because requested is never
//     rewritten by this decision, the target does not move as a consequence of
//     a re-request, so the sequence converges instead of oscillating.
//   * A re-request is emitted only when effective != active. The send updates
//     the active tier toward effective, so the next evaluation sees
//     effective == active and returns nullopt: at most one re-request per
//     state, never a loop.
//   * While a request is in flight (pending) or the peer cannot accept one,
//     nullopt is returned and the caller leaves the constraint marked changed,
//     so the evaluation re-fires when the reply arrives rather than being
//     dropped — the in-flight deferral.
//   * It never fights the user because the user is the only writer of
//     requested: a chip choice sets requested to exactly what the user picked,
//     so effective already reflects the user's intent and the loop has nothing
//     to correct. A low explicit choice (e.g. 30) is always offered, so
//     effective == active and the loop stays out of the way across later
//     display or capability changes.
inline std::optional<int> VideoFrameRateRerequestDecision(
    int requested, int active, bool pending, bool supported,
    bool peer_supports_high_fps, double display_refresh_hz) {
  if (pending || !supported) return std::nullopt;
  const int effective =
      EffectiveVideoFrameRate(requested, peer_supports_high_fps, display_refresh_hz);
  if (effective == active) return std::nullopt;
  return effective;
}

// Single decision point for how a stream degrades under load, derived purely
// from its negotiated frame rate. At or below 60 fps the encoder can generally
// sustain the native desktop resolution, so spatial detail is worth preserving
// and the stream drops frames rather than downscaling. Above 60 fps the viewer
// explicitly chose temporal smoothness; sustaining that cadence at high
// resolution is beyond the hardware encoder, so the stream gives up resolution
// before it gives up frames. Returns the wire preference value (0 = maintain
// frame rate, 1 = maintain resolution) so it can be cast straight to
// MiniRtcVideoDegradationPreference at the library boundary. Pure function of
// the frame rate only — every host that honours high-frame-rate negotiation
// derives the preference here rather than hard-coding one, so the behaviour
// stays consistent across bootstrap and renegotiation.
inline int DegradationPreferenceForFrameRate(int frame_rate) {
  return frame_rate > 60 ? 0 /* maintain frame rate */ : 1 /* maintain resolution */;
}

}  // namespace crossdesk

#endif
