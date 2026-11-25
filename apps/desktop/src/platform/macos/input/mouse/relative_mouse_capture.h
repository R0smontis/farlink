/*
 * @Author: Farlink contributors
 * @Date: 2026-11-25
 * Copyright (c) 2026 by Farlink contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef _RELATIVE_MOUSE_CAPTURE_H_
#define _RELATIVE_MOUSE_CAPTURE_H_

#include <cmath>

namespace crossdesk {

// Key code of the always-available game-mode escape shortcut: Ctrl+Alt+G.
// Window events reaching the capturer expose Windows-style virtual keys, the
// same encoding the keyboard capture path forwards.
inline constexpr int kGameModeExitVk = 'G';

// Tracks Ctrl/Alt modifier state from captured key events and recognises the
// game-mode escape chord. Pure logic, shared by the capture backend and its
// unit test. Both left and right variants of a modifier count; the chord must
// not retrigger while held (auto-repeat) nor fire when extra keys are pressed
// after the fact.
class GameModeExitChord {
 public:
  // Query order matters: call SwallowsKey for the current event BEFORE
  // Update, because the G key-up both ends the swallow state and is itself
  // swallowed.
  //
  // vk uses the Windows virtual-key encoding the wire format already uses:
  // 0xA2/0xA3 control, 0xA4/0xA5 alt (macOS Option maps there through the
  // existing CGKeyCodeToVkCode table).
  bool Update(int vk, bool is_down) {
    switch (vk) {
      case 0xA2:
      case 0xA3:
        ctrl_ = is_down;
        break;
      case 0xA4:
      case 0xA5:
        alt_ = is_down;
        break;
      default:
        break;
    }
    if (!is_down) {
      primed_ = false;
      if (vk == kGameModeExitVk) swallowing_ = false;
      return false;
    }
    const bool chord = ctrl_ && alt_ && vk == kGameModeExitVk;
    if (chord && !primed_) {
      primed_ = true;
      swallowing_ = true;
      return true;
    }
    if (!chord) primed_ = false;
    return false;
  }

  // True while the G of a triggered chord is still physically held. The
  // key-down that completed the chord, its auto-repeats and its matching
  // key-up all belong to the local escape, never to the peer.
  bool SwallowsKey(int vk) const {
    return vk == kGameModeExitVk && swallowing_;
  }

  void Reset() {
    ctrl_ = false;
    alt_ = false;
    primed_ = false;
    swallowing_ = false;
  }

 private:
  bool ctrl_ = false;
  bool alt_ = false;
  bool primed_ = false;
  bool swallowing_ = false;
};

// Converts a captured delta (points, the post-acceleration unit
// kCGMouseEventDeltaX/Y uses) into physical pixels of the controlled remote
// display. The wire unit is host display pixels, so the capture side must
// scale by the ratio between the local display's backing scale and the
// remote video's pixel density relative to its rendering.
struct RelativeDeltaScale {
  // scale = (remote video pixels per local point along x) — the controller
  // computes it from the video geometry and the window scale factor. Values
  // must be finite and positive; the caller drops the event otherwise.
  static bool Convert(double raw_dx, double raw_dy, double scale_x,
                      double scale_y, double& out_dx, double& out_dy) {
    if (!std::isfinite(raw_dx) || !std::isfinite(raw_dy) ||
        !std::isfinite(scale_x) || !std::isfinite(scale_y) || scale_x <= 0 ||
        scale_y <= 0) {
      return false;
    }
    out_dx = raw_dx * scale_x;
    out_dy = raw_dy * scale_y;
    return std::isfinite(out_dx) && std::isfinite(out_dy);
  }
};

}  // namespace crossdesk

#endif
