/*
 * @Author: Farlink contributors
 * @Date: 2026-11-25
 * Copyright (c) 2026 by Farlink contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "features/input/game_mode_capture_state.h"

#include <iostream>

namespace {

bool Expect(bool condition, const char* message) {
  if (condition) return true;
  std::cerr << message << '\n';
  return false;
}

crossdesk::GameModeCapturePreconditions AllTrue() {
  crossdesk::GameModeCapturePreconditions p;
  p.requested = true;
  p.stream_focused = true;
  p.connected = true;
  p.input_allowed = true;
  p.peer_capable = true;
  p.control_mouse = true;
  p.capturer_active = true;
  return p;
}

}  // namespace

int main() {
  bool ok = true;

  ok &= Expect(crossdesk::GameModeCaptureShouldBeActive(AllTrue()),
               "all preconditions met must keep capture active");

  // Every single precondition failing alone must end capture: the tap
  // swallows mouse input system-wide, so there is no precondition lax
  // enough to lapse safely.
  {
    auto p = AllTrue();
    p.requested = false;
    ok &= Expect(!crossdesk::GameModeCaptureShouldBeActive(p),
                 "exited game mode must end capture");
  }
  {
    auto p = AllTrue();
    p.stream_focused = false;
    ok &= Expect(!crossdesk::GameModeCaptureShouldBeActive(p),
                 "focus loss must end capture");
  }
  {
    auto p = AllTrue();
    p.connected = false;
    ok &= Expect(!crossdesk::GameModeCaptureShouldBeActive(p),
                 "disconnect must end capture");
  }
  {
    auto p = AllTrue();
    p.input_allowed = false;
    ok &= Expect(!crossdesk::GameModeCaptureShouldBeActive(p),
                 "privacy input block must end capture");
  }
  {
    auto p = AllTrue();
    p.peer_capable = false;
    ok &= Expect(!crossdesk::GameModeCaptureShouldBeActive(p),
                 "incapable peer must end capture");
  }
  {
    auto p = AllTrue();
    p.control_mouse = false;
    ok &= Expect(!crossdesk::GameModeCaptureShouldBeActive(p),
                 "mouse control release must end capture");
  }
  {
    auto p = AllTrue();
    p.capturer_active = false;
    ok &= Expect(!crossdesk::GameModeCaptureShouldBeActive(p),
                 "dead capturer must not count as active capture");
  }

  return ok ? 0 : 1;
}
