/*
 * @Author: Farlink contributors
 * @Date: 2026-11-25
 * Copyright (c) 2026 by Farlink contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "features/input/game_mode_buttons.h"

#include <iostream>

namespace {

bool Expect(bool condition, const char* message) {
  if (condition) return true;
  std::cerr << message << '\n';
  return false;
}

}  // namespace

int main() {
  bool ok = true;

  // The exit-time contract: whatever was reported down without a matching
  // up must be exactly the set the caller releases.
  {
    crossdesk::GameModePressedButtons buttons;
    buttons.Update(1, true);   // left down
    buttons.Update(2, true);   // right down
    buttons.Update(1, false);  // left up
    const auto pressed = buttons.TakePressed();
    ok &= Expect(pressed.size() == 1 && pressed.count(2) == 1,
                 "only the still-held button must be released at exit");
    ok &= Expect(buttons.TakePressed().empty(),
                 "take must clear the tracker so a second exit releases "
                 "nothing");
  }

  // Ups without a prior down are irrelevant; duplicate downs are one press.
  {
    crossdesk::GameModePressedButtons buttons;
    buttons.Update(1, false);  // stray up
    buttons.Update(3, true);   // middle down twice
    buttons.Update(3, true);
    const auto pressed = buttons.TakePressed();
    ok &= Expect(pressed.size() == 1 && pressed.count(3) == 1,
                 "stray ups and duplicate downs must not corrupt tracking");
  }

  // Out-of-range buttons are ignored, matching the send path's encoding.
  {
    crossdesk::GameModePressedButtons buttons;
    buttons.Update(0, true);
    buttons.Update(4, true);
    buttons.Update(-1, true);
    ok &= Expect(buttons.empty(),
                 "buttons outside the Slint encoding must be ignored");
  }

  // A full press cycle ends empty: nothing to release when input was
  // cleanly lifted before the exit.
  {
    crossdesk::GameModePressedButtons buttons;
    for (int button = 1; button <= 3; ++button) {
      buttons.Update(button, true);
      buttons.Update(button, false);
    }
    ok &= Expect(buttons.empty(),
                 "fully released input must leave nothing to clean up");
  }

  return ok ? 0 : 1;
}
