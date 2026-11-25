/*
 * @Author: Farlink contributors
 * @Date: 2026-11-25
 * Copyright (c) 2026 by Farlink contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef _GAME_MODE_BUTTONS_H_
#define _GAME_MODE_BUTTONS_H_

#include <set>

namespace crossdesk {

// Pure tracking of which mouse buttons the controller has reported down
// during game-mode capture without a matching up. Game mode can end at any
// moment — escape chord, focus loss, disconnect, capability loss, quit —
// and whatever is still pressed at that point must be released on the host
// or the remote machine is left with a stuck button. Kept free of locks and
// UI types so the tracking is unit-testable; the caller serializes access.
class GameModePressedButtons {
 public:
  // Records one captured edge. Buttons outside the Slint encoding range
  // (left 1, right 2, middle 3) are ignored, matching the send path.
  void Update(int button, bool down) {
    if (button < 1 || button > 3) return;
    if (down) {
      pressed_.insert(button);
    } else {
      pressed_.erase(button);
    }
  }

  // Transfers the still-pressed set out and clears the tracker: exactly the
  // transition game-mode exit needs, where the returned buttons are the ones
  // the caller must send ups for.
  std::set<int> TakePressed() {
    const std::set<int> out = pressed_;
    pressed_.clear();
    return out;
  }

  bool empty() const { return pressed_.empty(); }

 private:
  std::set<int> pressed_;
};

}  // namespace crossdesk

#endif
