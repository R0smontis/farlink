/*
 * @Author: Farlink contributors
 * @Date: 2026-11-27
 * Copyright (c) 2026 by Farlink contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

// Deterministic unit test for the side-aware modifier edge tracker. Every
// scenario is a scripted sequence of (mac keycode, combined flags) pairs as
// the capture tap would observe them; the emissions are checked for exact
// vk/direction sequences. The scenarios encode the failure modes of the
// type-collapsed tracking this replaces: both sides of one modifier held
// at once, releases masked by the other side, and tap-disable gaps.

#include "macos_modifier_key_tracker.h"

#include <iostream>
#include <vector>

namespace {

using crossdesk::MacModifierKeyTracker;

int failures = 0;

void Expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

struct Step {
  int keycode;
  uint64_t flags;
  // Expected emission: vk 0 means no emission expected.
  int vk = 0;
  bool down = false;
};

void Run(const char* name, MacModifierKeyTracker& tracker,
         const std::vector<Step>& steps) {
  for (size_t i = 0; i < steps.size(); ++i) {
    const Step& step = steps[i];
    MacModifierKeyTracker::Emission emission;
    const bool emitted = tracker.Update(step.keycode, step.flags, emission);
    if (step.vk == 0) {
      if (emitted) {
        std::cerr << "FAIL: " << name << " step " << i
                  << ": unexpected emission vk=0x" << std::hex << emission.vk
                  << " down=" << emission.is_down << '\n';
        ++failures;
      }
      continue;
    }
    if (!emitted) {
      std::cerr << "FAIL: " << name << " step " << i
                << ": expected emission vk=0x" << std::hex << step.vk
                << " missing\n";
      ++failures;
      continue;
    }
    if (emission.vk != step.vk || emission.is_down != step.down) {
      std::cerr << "FAIL: " << name << " step " << i << ": expected vk=0x"
                << std::hex << step.vk << " down=" << step.down << " got vk=0x"
                << emission.vk << " down=" << emission.is_down << '\n';
      ++failures;
    }
  }
}

constexpr uint64_t S = MacModifierKeyTracker::kMaskShift;
constexpr uint64_t C = MacModifierKeyTracker::kMaskControl;
constexpr uint64_t O = MacModifierKeyTracker::kMaskAlternate;
constexpr uint64_t M = MacModifierKeyTracker::kMaskCommand;
constexpr uint64_t CL = MacModifierKeyTracker::kMaskCapsLock;

constexpr int LSH = MacModifierKeyTracker::kKeyLeftShift;
constexpr int RSH = MacModifierKeyTracker::kKeyRightShift;
constexpr int LCT = MacModifierKeyTracker::kKeyLeftControl;
constexpr int RCT = MacModifierKeyTracker::kKeyRightControl;
constexpr int LOP = MacModifierKeyTracker::kKeyLeftOption;
constexpr int ROP = MacModifierKeyTracker::kKeyRightOption;
constexpr int LCM = MacModifierKeyTracker::kKeyLeftCommand;
constexpr int RCM = MacModifierKeyTracker::kKeyRightCommand;
constexpr int CAPS = MacModifierKeyTracker::kKeyCapsLock;

}  // namespace

int main() {
  {
    // The core failure of type-collapsed tracking: left shift down, right
    // shift down on top, left released while right still held. The combined
    // bit never clears between steps 2 and 3, yet the side-specific release
    // must still be emitted; the final release of the right side clears the
    // bit.
    MacModifierKeyTracker tracker;
    Run("both shift sides overlap", tracker,
        {{LSH, S, 0xA0, true},
         {RSH, S, 0xA1, true},
         {LSH, S, 0xA0, false},   // bit still set: right side holds it
         {RSH, 0, 0xA1, false}}); // bit clears: last side released
  }
  {
    // Option sides overlap the same way shift does.
    MacModifierKeyTracker tracker;
    Run("option sides overlap", tracker,
        {{LOP, O, 0xA4, true}, {ROP, O, 0xA5, true}, {LOP, O, 0xA4, false},
         {ROP, 0, 0xA5, false}});
  }
  {
    // Mirror image: same-side press/release pairs never confuse the other
    // side's tracked state.
    MacModifierKeyTracker tracker;
    Run("left only cycle", tracker,
        {{LSH, S, 0xA0, true}, {LSH, 0, 0xA0, false}});
  }
  {
    // Control and option are independent types; overlapping them must not
    // interact.
    MacModifierKeyTracker tracker;
    Run("control and option independent", tracker,
        {{LCT, C, 0xA2, true},
         {ROP, C | O, 0xA5, true},
         {LCT, O, 0xA2, false},
         {ROP, 0, 0xA5, false}});
  }
  {
    // Left/right command use the Windows-style vk pair.
    MacModifierKeyTracker tracker;
    Run("command sides", tracker,
        {{LCM, M, 0x5B, true}, {RCM, M, 0x5C, true}, {LCM, M, 0x5B, false},
         {RCM, 0, 0x5C, false}});
  }
  {
    // Tap-disable gap: the press of the right shift was never observed
    // (events lost while the tap was disabled). Its release arrives with the
    // bit clear. Nothing was tracked down, so no emission — but the tracker
    // must stay consistent for the next cycle instead of alternating into a
    // phantom press.
    MacModifierKeyTracker tracker;
    Run("missed press resyncs clean", tracker,
        {{RSH, 0, 0, false},       // release of a press we never saw
         {RSH, S, 0xA1, true},     // next real press still registers
         {RSH, 0, 0xA1, false}});
  }
  {
    // Stale other side: left control tracked down, tap drops events, user
    // releases everything. The next observed event — a fresh right-control
    // press — arrives with the bit set; the stale left side must not flip
    // the direction decision. The release of right control then clears the
    // bit and silently resyncs the stale left side.
    MacModifierKeyTracker tracker;
    Run("stale side resynced by clear bit", tracker,
        {{LCT, C, 0xA2, true},
         // (gap: user released left control while the tap was disabled)
         {RCT, C, 0xA3, true},     // bit set + key up -> press, correctly
         {RCT, 0, 0xA3, false},    // release clears bit, resyncs stale left
         {LCT, C, 0xA2, true},     // left side usable again
         {LCT, 0, 0xA2, false}});
  }
  {
    // Caps lock is a latch: each physical press toggles the bit, and the
    // edge is the bit change. Seeding at hook time suppresses the phantom
    // first edge.
    MacModifierKeyTracker tracker;
    tracker.SeedCapsLock(false);
    Run("caps lock latching", tracker,
        {{CAPS, CL, 0x14, true}, {CAPS, 0, 0x14, false}, {CAPS, CL, 0x14, true}});
  }
  {
    // Seeded latched caps: no emission until the user actually toggles.
    MacModifierKeyTracker tracker;
    tracker.SeedCapsLock(true);
    Run("seeded caps no phantom edge", tracker,
        {{CAPS, CL, 0, false},   // re-report of the same bit: no edge
         {CAPS, 0, 0x14, false}});
  }
  {
    // Unknown keys (fn, or anything outside the modifier set) never emit.
    MacModifierKeyTracker tracker;
    Run("unknown key ignored", tracker, {{0x3F, M, 0, false}});
  }
  {
    // Clear() drops everything: a re-hooked tracker must not carry pressed
    // state from the previous capture session.
    MacModifierKeyTracker tracker;
    Run("before clear", tracker, {{LSH, S, 0xA0, true}, {LCM, M | S, 0x5B, true}});
    tracker.Clear();
    MacModifierKeyTracker::Emission emission;
    // The release of the still-physically-held keys must not produce
    // phantom edges after the clear: bit-clear release of untracked keys.
    Expect(!tracker.Update(LSH, 0, emission),
           "clear suppresses stale release edge");
    Expect(!tracker.Update(LCM, 0, emission),
           "clear suppresses stale command release edge");
  }

  if (failures == 0) {
    std::cout << "macos_modifier_key_tracker_test: all assertions passed\n";
    return 0;
  }
  return 1;
}
