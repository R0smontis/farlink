/*
 * @Author: Farlink contributors
 * @Date: 2026-11-25
 * Copyright (c) 2026 by Farlink contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef _GAME_MODE_CAPTURE_STATE_H_
#define _GAME_MODE_CAPTURE_STATE_H_

namespace crossdesk {

// Preconditions for game-mode mouse capture to stay active, as one pure
// decision so the call sites cannot drift apart. Every input is the answer
// the rest of the system already computed; this function only combines
// them. Any false input means capture must end now — the capture tap
// swallows every mouse event system-wide while it runs, so a precondition
// lapsing is never a cosmetic problem: the user would be locked out of
// their own mouse while the stream that owned it is gone.
struct GameModeCapturePreconditions {
  // The controller user asked for game mode and has not exited it.
  bool requested = false;
  // The stream window has keyboard focus; capture swallows input
  // system-wide, so unfocused capture is always wrong.
  bool stream_focused = false;
  // The peer connection is up and the session is not closing.
  bool connected = false;
  // Privacy blocking is inactive; a blocked peer must not receive input,
  // and capture exists only to produce input.
  bool input_allowed = false;
  // The peer advertised supports_relative_mouse; stock hosts would treat
  // the input as garbage absolute moves.
  bool peer_capable = false;
  // Mouse control is enabled for the session (the toolbar toggle).
  bool control_mouse = false;
  // The capturer itself is still hooked; a dead tap cannot be restarted
  // transparently because the user experience is defined by the freeze.
  bool capturer_active = false;
};

inline bool GameModeCaptureShouldBeActive(
    const GameModeCapturePreconditions& p) {
  return p.requested && p.stream_focused && p.connected && p.input_allowed &&
         p.peer_capable && p.control_mouse && p.capturer_active;
}

}  // namespace crossdesk

#endif
