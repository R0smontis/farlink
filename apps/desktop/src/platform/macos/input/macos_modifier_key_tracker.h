/*
 * @Author: Farlink contributors
 * @Date: 2026-11-27
 * Copyright (c) 2026 by Farlink contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef _MACOS_MODIFIER_KEY_TRACKER_H_
#define _MACOS_MODIFIER_KEY_TRACKER_H_

#include <cstdint>

namespace crossdesk {

// Side-aware modifier edge tracker for the macOS keyboard capture tap.
//
// macOS reports modifier keys as kCGEventFlagsChanged: one event per physical
// key transition, carrying the keycode of the key that moved and the combined
// post-transition flags. The combined flag bits cannot distinguish left from
// right, so a tracker keyed on the bits alone cannot represent "left shift
// released while right shift is still held" — the bit never changes, no edge
// is emitted, and the peer ends up with a stuck left shift. The wire format
// carries side-specific virtual keys (0xA0/0xA1 shift, 0xA2/0xA3 control,
// 0xA4/0xA5 option, 0x5B/0x5C command), so both sides must be tracked
// independently here.
//
// Direction disambiguation uses two facts: the event keycode names the exact
// key that transitioned, and transitions of one physical key strictly
// alternate. When the type's combined bit is clear the key (and every key of
// that type) is necessarily up, which also resynchronises state after a tap
// disable swallowed events. Caps lock is a latch, not a hold, and keeps
// bit-edge semantics.
//
// Pure logic shared by the capture backend and its unit test; no CoreGraphics
// types. Mac keycodes and flag masks are passed in as plain integers.
class MacModifierKeyTracker {
 public:
  // macOS keycodes for the physical modifier keys.
  static constexpr int kKeyCapsLock = 0x39;
  static constexpr int kKeyLeftShift = 0x38;
  static constexpr int kKeyRightShift = 0x3C;
  static constexpr int kKeyLeftControl = 0x3B;
  static constexpr int kKeyRightControl = 0x3E;
  static constexpr int kKeyLeftOption = 0x3A;
  static constexpr int kKeyRightOption = 0x3D;
  static constexpr int kKeyLeftCommand = 0x37;
  static constexpr int kKeyRightCommand = 0x36;

  // CGEventFlags bits, mirrored so the logic carries no CoreGraphics
  // dependency: kCGEventFlagMaskAlphaShift/Shift/Control/Alternate/Command.
  static constexpr uint64_t kMaskCapsLock = 1u << 16;
  static constexpr uint64_t kMaskShift = 1u << 17;
  static constexpr uint64_t kMaskControl = 1u << 18;
  static constexpr uint64_t kMaskAlternate = 1u << 19;
  static constexpr uint64_t kMaskCommand = 1u << 20;

  struct Emission {
    int vk = 0;
    bool is_down = false;
  };

  // One emission per event at most: a flags-changed event describes exactly
  // one physical key transitioning. vk is the Windows-style side-specific
  // code (0xA0-0xA5, 0x5B/0x5C) or 0x14 for caps lock. Returns false when the
  // event carries no state change worth forwarding (unknown key, or an edge
  // the tracker already believes happened — e.g. the release of a key whose
  // press predates the hook).
  bool Update(int mac_keycode, uint64_t flags, Emission& out) {
    switch (mac_keycode) {
      case kKeyCapsLock:
        return UpdateCapsLock(flags, out);
      case kKeyLeftShift:
        return UpdateHoldKey(left_shift_, 0xA0, right_shift_, flags, kMaskShift, out);
      case kKeyRightShift:
        return UpdateHoldKey(right_shift_, 0xA1, left_shift_, flags, kMaskShift, out);
      case kKeyLeftControl:
        return UpdateHoldKey(left_control_, 0xA2, right_control_, flags, kMaskControl, out);
      case kKeyRightControl:
        return UpdateHoldKey(right_control_, 0xA3, left_control_, flags, kMaskControl, out);
      case kKeyLeftOption:
        return UpdateHoldKey(left_option_, 0xA4, right_option_, flags, kMaskAlternate, out);
      case kKeyRightOption:
        return UpdateHoldKey(right_option_, 0xA5, left_option_, flags, kMaskAlternate, out);
      case kKeyLeftCommand:
        return UpdateHoldKey(left_command_, 0x5B, right_command_, flags, kMaskCommand, out);
      case kKeyRightCommand:
        return UpdateHoldKey(right_command_, 0x5C, left_command_, flags, kMaskCommand, out);
      default:
        return false;
    }
  }

  // Drops all tracked state. On re-hook the peer is force-released through
  // the controller anyway, so stale pressed state here must never seed the
  // new session.
  void Clear() {
    caps_lock_ = false;
    caps_lock_seeded_ = false;
    left_shift_ = right_shift_ = false;
    left_control_ = right_control_ = false;
    left_option_ = right_option_ = false;
    left_command_ = right_command_ = false;
  }

  // Seeds the latch state at hook time from CGEventSourceFlagsState. Only
  // caps lock can be seeded reliably: it is a latched bit, while the hold
  // modifiers' bits say nothing about which side is down, and seeding both
  // sides would fabricate presses the peer never sees releases for.
  void SeedCapsLock(bool latched) {
    caps_lock_ = latched;
    caps_lock_seeded_ = true;
  }

 private:
  bool UpdateCapsLock(uint64_t flags, Emission& out) {
    const bool latched = (flags & kMaskCapsLock) != 0;
    // The very first event after hooking always reports an edge when the
    // seed could not observe a pre-existing latch transition — but the
    // latched bit itself is reliable, so an unchanged bit is never an edge.
    if (caps_lock_seeded_ && latched == caps_lock_) return false;
    caps_lock_ = latched;
    caps_lock_seeded_ = true;
    out = {0x14, latched};  // VK_CAPITAL, down while latched like before.
    return true;
  }

  // own: tracked state of the key in the event. other: tracked state of the
  // same-type key on the opposite side. Direction rules:
  //  - bit clear: the key is necessarily up; the press either never happened
  //    or was missed. Emit a release only if the key was tracked down, and
  //    force-release the other side too — its bit can never clear while it
  //    is held, so its tracked-down state is certainly stale.
  //  - bit set, own tracked up: a press (the bit may already have been set
  //    by the other side; alternation still identifies the transition).
  //  - bit set, own tracked down: the release of a key whose other side is
  //    still held — the only way a release event can arrive with the bit
  //    set.
  bool UpdateHoldKey(bool& own, int vk, bool& other, uint64_t flags,
                     uint64_t mask, Emission& out) {
    const bool bit_set = (flags & mask) != 0;
    if (!bit_set) {
      bool emitted = false;
      if (own) {
        own = false;
        out = {vk, false};
        emitted = true;
      }
      // The other side cannot physically be down either; resync silently
      // (its release cannot be emitted here — one event carries one key —
      // but a stuck tracked state would corrupt every future direction
      // decision of this type).
      other = false;
      return emitted;
    }
    if (!own) {
      own = true;
      out = {vk, true};
      return true;
    }
    own = false;
    out = {vk, false};
    return true;
  }

  bool caps_lock_ = false;
  bool caps_lock_seeded_ = false;
  bool left_shift_ = false;
  bool right_shift_ = false;
  bool left_control_ = false;
  bool right_control_ = false;
  bool left_option_ = false;
  bool right_option_ = false;
  bool left_command_ = false;
  bool right_command_ = false;
};

}  // namespace crossdesk

#endif
