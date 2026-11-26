/*
 * @Author: DI JUNKUN
 * @Date: 2024-11-22
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _KEYBOARD_CAPTURER_H_
#define _KEYBOARD_CAPTURER_H_

#include <ApplicationServices/ApplicationServices.h>

#include <atomic>

#include "device_controller.h"
#include "macos_keyboard_modifier_state.h"
#include "macos_modifier_key_tracker.h"
#include "../mouse/relative_mouse_capture.h"

namespace crossdesk {

class PlatformKeyboardCapturer final : public KeyboardCapturer {
 public:
  // Local-shortcut policy of the capture tap. SendToRemote swallows keys
  // into the remote session (the historical behaviour); PreferLocal
  // observes them in listen-only mode, so local chords (Cmd+Space,
  // Cmd+Tab, …) keep working on the controller while copies still reach
  // the peer. Switching requires re-creating the tap: the listen-only
  // option is fixed at CGEventTapCreate time.
  enum class ShortcutMode { kSendToRemote, kPreferLocal };

  PlatformKeyboardCapturer();
  ~PlatformKeyboardCapturer() override;

 public:
  int Hook(OnKeyAction on_key_action, void* user_ptr) override;
  int Unhook() override;
  bool SupportsTextInput() const override { return true; }
  int SendTextCommand(const std::string& text) override;
  int SendKeyboardCommand(int key_code, bool is_down,
                          uint32_t scan_code = 0,
                          bool extended = false) override;

  void SetShortcutMode(ShortcutMode mode) { shortcut_mode_ = mode; }
  ShortcutMode GetShortcutMode() const { return shortcut_mode_; }
  // While game mode is active the local escape chord Ctrl+Alt+G belongs to
  // the controller: the key-down that completes it, its auto-repeats and
  // its key-up must never reach the peer through this tap. The mouse
  // capturer performs the actual release; this tap only suppresses.
  void SetGameModeActive(bool active) {
    game_mode_active_.store(active, std::memory_order_release);
    if (!active) exit_chord_.Reset();
  }
  // Re-enables a tap the system disabled by timeout or user input; safe
  // to call from the tap callback thread.
  void ReenableTap();

 private:
  CFMachPortRef event_tap_ = nullptr;
  CFRunLoopSourceRef run_loop_source_ = nullptr;
  ShortcutMode shortcut_mode_ = ShortcutMode::kSendToRemote;

 public:
  // Written only on the tap thread and at hook/unhook time; the tracker is
  // the side-aware replacement for the type-collapsed flag booleans this
  // class used to expose.
  MacModifierKeyTracker modifier_tracker_;
  MacKeyboardModifierState injected_modifier_state_;
  GameModeExitChord exit_chord_;
  std::atomic<bool> game_mode_active_{false};
  int fn_key_code_ = 0x3F;
};
}  // namespace crossdesk
#endif
