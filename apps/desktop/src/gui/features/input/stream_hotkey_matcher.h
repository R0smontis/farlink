/*
 * @Author: Farlink contributors
 * @Date: 2026-11-27
 * Copyright (c) 2026 by Farlink contributors.
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * reference-grade stream control chords. Pure, deterministic key-state tracker
 * recognizing exactly three controller-local chords while a stream window
 * owns input:
 *
 *   Ctrl+Alt+Right  -> next display     (wraps; dead with one display)
 *   Ctrl+Alt+Left   -> previous display (wraps; dead with one display)
 *   Ctrl+Alt+F      -> toggle fullscreen
 *
 * The tracker consumes VK-encoded key edges in the same encoding the wire
 * uses (0xA2/0xA3 control, 0xA4/0xA5 alt, 0x25 left, 0x27 right, 'F').
 * Both capture backends feed identical VK edges: the native CGEventTap on
 * macOS, the low-level hook on Windows, and the Slint window-event fallback
 * through SlintKeyToWindowsVk — so one tracker covers every input path.
 *
 * Swallow contract, mirroring GameModeExitChord: once a chord completes,
 * the completing key's auto-repeats and its key-up belong to the local
 * action, never to the peer. Query SwallowsKey for the current event
 * BEFORE calling Update — the key-up that ends the swallow state is itself
 * swallowed. Update returning an action means the key-down completing the
 * chord was consumed locally and must not be forwarded.
 */

#ifndef CROSSDESK_GUI_STREAM_HOTKEY_MATCHER_H_
#define CROSSDESK_GUI_STREAM_HOTKEY_MATCHER_H_

namespace crossdesk {

enum class StreamHotkey {
  kNone = 0,
  kNextDisplay,
  kPrevDisplay,
  kToggleFullscreen,
};

class StreamHotkeyMatcher {
 public:
  // Wire VK encoding shared by every capture backend.
  static constexpr int kVkControlLeft = 0xA2;
  static constexpr int kVkControlRight = 0xA3;
  static constexpr int kVkAltLeft = 0xA4;
  static constexpr int kVkAltRight = 0xA5;
  static constexpr int kVkLeft = 0x25;
  static constexpr int kVkRight = 0x27;
  static constexpr int kVkF = 'F';

  // Consumes one key edge. Returns the hotkey the completing key-down
  // triggers, or kNone. Key-ups never trigger. Auto-repeat of the
  // completing key does not retrigger: the chord is primed once and must
  // be fully released before it can fire again.
  StreamHotkey Update(int vk, bool is_down) {
    switch (vk) {
      case kVkControlLeft:
      case kVkControlRight:
        ctrl_ = is_down;
        break;
      case kVkAltLeft:
      case kVkAltRight:
        alt_ = is_down;
        break;
      default:
        break;
    }
    if (!is_down) {
      // Any release disarms retriggering once nothing is swallowed anymore;
      // a key released mid-chord cancels the in-progress chord so a stray
      // Ctrl+Alt+Right+letter+Right sequence cannot fire late.
      if (vk == swallowed_key_) swallowed_key_ = 0;
      primed_ = false;
      return StreamHotkey::kNone;
    }
    if (!ctrl_ || !alt_) {
      primed_ = false;
      return StreamHotkey::kNone;
    }
    StreamHotkey action = StreamHotkey::kNone;
    int key = 0;
    switch (vk) {
      case kVkRight:
        action = StreamHotkey::kNextDisplay;
        key = vk;
        break;
      case kVkLeft:
        action = StreamHotkey::kPrevDisplay;
        key = vk;
        break;
      case kVkF:
        action = StreamHotkey::kToggleFullscreen;
        key = vk;
        break;
      default:
        // An unrelated key inside the chord cancels priming exactly like
        // the game-mode escape: Ctrl+Alt+X must not arm a later bare F.
        primed_ = false;
        return StreamHotkey::kNone;
    }
    if (primed_) return StreamHotkey::kNone;
    primed_ = true;
    swallowed_key_ = key;
    return action;
  }

  // True while the completing key of a triggered chord is still physically
  // held: its auto-repeats and its matching key-up are local. The
  // completing key-down itself was already consumed by the Update return.
  bool SwallowsKey(int vk) const {
    return swallowed_key_ != 0 && vk == swallowed_key_;
  }

  // Modifier edges (Ctrl/Alt) are never swallowed: the peer expects them
  // as ordinary input and the chord is defined by the final key alone.
  bool Swallowed() const { return swallowed_key_ != 0; }

  void Reset() {
    ctrl_ = false;
    alt_ = false;
    primed_ = false;
    swallowed_key_ = 0;
  }

  // Display cycling arithmetic, pure so the wrap rule is testable without
  // any session state. count <= 1 keeps the current index — cycling a
  // single display is a no-op, matching commercial remote desktops.
  static int CycleDisplay(int current, int display_count, bool forward) {
    if (display_count <= 1 || current < 0 || current >= display_count) {
      return current;
    }
    return forward ? (current + 1) % display_count
                   : (current + display_count - 1) % display_count;
  }

 private:
  bool ctrl_ = false;
  bool alt_ = false;
  bool primed_ = false;
  int swallowed_key_ = 0;
};

}  // namespace crossdesk

#endif  // CROSSDESK_GUI_STREAM_HOTKEY_MATCHER_H_
