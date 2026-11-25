/*
 * @Author: DI JUNKUN
 * @Date: 2026-11-25
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _VIDEO_FRAME_RATE_H_
#define _VIDEO_FRAME_RATE_H_

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
