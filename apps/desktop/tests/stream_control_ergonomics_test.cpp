/*
 * @Author: Farlink contributors
 * @Date: 2026-11-27
 * Copyright (c) 2026 by Farlink contributors.
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Deterministic unit test for the Wave-4C stream control ergonomics:
 *
 *  BitrateCapPreset — every preset's index<->bps round-trip, Auto == 0,
 *    invalid-index/unknown-bps fallback to Auto, wire clamping, validation
 *    of exactly the preset set.
 *
 *  StreamHotkeyMatcher — Ctrl+Alt+Right/Left/F recognition, display cycling
 *    wrap-around with 1/2/5 displays, auto-repeat suppression while the
 *    completing key is held, wrong-modifier rejection, unrelated-key priming
 *    reset, key-up swallow tracking, and the matcher reset contract.
 */

#include "features/settings/bitrate_cap_preset.h"
#include "features/input/stream_hotkey_matcher.h"

#include <iostream>

namespace {

using crossdesk::BitrateCapPreset;
using crossdesk::StreamHotkey;
using crossdesk::StreamHotkeyMatcher;

int failures = 0;

void Expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

void TestBitrateCapPresets() {
  // Default tiers: Auto (0), 原画 30M, 超清 14M, 高清 8M.
  Expect(BitrateCapPreset::BpsForIndex(0) == 0, "Auto preset must be 0 bps (no cap)");
  Expect(BitrateCapPreset::BpsForIndex(1) == 30'000'000, "index 1 must be 30 Mbps");
  Expect(BitrateCapPreset::BpsForIndex(2) == 14'000'000, "index 2 must be 14 Mbps");
  Expect(BitrateCapPreset::BpsForIndex(3) == 8'000'000, "index 3 must be 8 Mbps");

  // Index validity.
  Expect(BitrateCapPreset::ValidIndex(0), "index 0 valid");
  Expect(BitrateCapPreset::ValidIndex(3), "index 3 valid");
  Expect(!BitrateCapPreset::ValidIndex(-1), "index -1 invalid");
  Expect(!BitrateCapPreset::ValidIndex(4), "index 4 invalid");

  // Invalid indices fall back to the no-cap contract, never an accidental cap.
  Expect(BitrateCapPreset::BpsForIndex(-1) == 0, "negative index yields no cap");
  Expect(BitrateCapPreset::BpsForIndex(4) == 0, "overflow index yields no cap");
  Expect(BitrateCapPreset::BpsForIndex(1000) == 0, "large index yields no cap");

  // bps -> index round-trip for every default tier.
  for (int index = 0; index < BitrateCapPreset::kCount; ++index) {
    const int bps = BitrateCapPreset::BpsForIndex(index);
    Expect(BitrateCapPreset::IndexForBps(bps) == index,
           "bps->index round-trip must be exact for every default tier");
  }

  // Custom tier round-trip.
  auto custom = BitrateCapPreset::DefaultTiers();
  custom.push_back({"蓝光", 50});
  Expect(BitrateCapPreset::BpsForIndex(custom, 4) == 50'000'000, "custom 50M bps");
  Expect(BitrateCapPreset::IndexForBps(custom, 50'000'000) == 4, "custom 50M index");

  // Auto round-trips through zero; unknown bps falls back to Auto.
  Expect(BitrateCapPreset::IndexForBps(0) == BitrateCapPreset::kAutoIndex,
         "0 bps must map to the Auto index");
  Expect(BitrateCapPreset::IndexForBps(7'999'999) == BitrateCapPreset::kAutoIndex,
         "near-miss bps falls back to Auto");
  Expect(BitrateCapPreset::IndexForBps(-5) == BitrateCapPreset::kAutoIndex,
         "negative bps falls back to Auto");

  // Validation accepts 0 or any integer Mbps in [1, 100].
  Expect(BitrateCapPreset::ValidBps(0), "0 bps valid (Auto)");
  Expect(BitrateCapPreset::ValidBps(8'000'000), "8M valid");
  Expect(BitrateCapPreset::ValidBps(14'000'000), "14M valid");
  Expect(BitrateCapPreset::ValidBps(30'000'000), "30M valid");
  Expect(BitrateCapPreset::ValidBps(50'000'000), "50M valid");
  Expect(BitrateCapPreset::ValidBps(100'000'000), "100M valid");
  Expect(!BitrateCapPreset::ValidBps(1), "1 bps invalid");
  Expect(!BitrateCapPreset::ValidBps(101'000'000), "101M invalid");
  Expect(!BitrateCapPreset::ValidBps(-1), "negative invalid");

  // Wire clamping: negative becomes no-cap, over-max folds to the max.
  Expect(BitrateCapPreset::ClampWireBps(-100) == 0, "negative clamps to no cap");
  Expect(BitrateCapPreset::ClampWireBps(0) == 0, "zero stays zero");
  Expect(BitrateCapPreset::ClampWireBps(14'000'000) == 14'000'000, "in-range unchanged");
  Expect(BitrateCapPreset::ClampWireBps(200'000'000) == BitrateCapPreset::kWireMaxBps,
         "over-max clamps to the wire maximum");
}

// Feed one edge into the matcher and return the fired hotkey.
StreamHotkey Feed(StreamHotkeyMatcher& matcher, int vk, bool down) {
  return matcher.Update(vk, down);
}

void PressCtrlAlt(StreamHotkeyMatcher& matcher) {
  Feed(matcher, StreamHotkeyMatcher::kVkControlLeft, true);
  Feed(matcher, StreamHotkeyMatcher::kVkAltLeft, true);
}

void ReleaseCtrlAlt(StreamHotkeyMatcher& matcher) {
  Feed(matcher, StreamHotkeyMatcher::kVkControlLeft, false);
  Feed(matcher, StreamHotkeyMatcher::kVkAltLeft, false);
}

void TestHotkeyRecognition() {
  {
    StreamHotkeyMatcher matcher;
    PressCtrlAlt(matcher);
    Expect(Feed(matcher, StreamHotkeyMatcher::kVkRight, true) == StreamHotkey::kNextDisplay,
           "Ctrl+Alt+Right must fire NextDisplay");
  }
  {
    StreamHotkeyMatcher matcher;
    PressCtrlAlt(matcher);
    Expect(Feed(matcher, StreamHotkeyMatcher::kVkLeft, true) == StreamHotkey::kPrevDisplay,
           "Ctrl+Alt+Left must fire PrevDisplay");
  }
  {
    StreamHotkeyMatcher matcher;
    PressCtrlAlt(matcher);
    Expect(Feed(matcher, StreamHotkeyMatcher::kVkF, true) == StreamHotkey::kToggleFullscreen,
           "Ctrl+Alt+F must fire ToggleFullscreen");
  }
  // Right-side modifiers count exactly like left-side ones.
  {
    StreamHotkeyMatcher matcher;
    Feed(matcher, StreamHotkeyMatcher::kVkControlRight, true);
    Feed(matcher, StreamHotkeyMatcher::kVkAltRight, true);
    Expect(Feed(matcher, StreamHotkeyMatcher::kVkF, true) == StreamHotkey::kToggleFullscreen,
           "right-side Ctrl+Alt must complete the chord");
  }
}

void TestWrongModifierRejection() {
  // No modifiers at all.
  {
    StreamHotkeyMatcher matcher;
    Expect(Feed(matcher, StreamHotkeyMatcher::kVkF, true) == StreamHotkey::kNone,
           "bare F must not fire");
    Expect(Feed(matcher, StreamHotkeyMatcher::kVkRight, true) == StreamHotkey::kNone,
           "bare Right must not fire");
  }
  // Ctrl alone.
  {
    StreamHotkeyMatcher matcher;
    Feed(matcher, StreamHotkeyMatcher::kVkControlLeft, true);
    Expect(Feed(matcher, StreamHotkeyMatcher::kVkF, true) == StreamHotkey::kNone,
           "Ctrl+F without Alt must not fire");
  }
  // Alt alone.
  {
    StreamHotkeyMatcher matcher;
    Feed(matcher, StreamHotkeyMatcher::kVkAltLeft, true);
    Expect(Feed(matcher, StreamHotkeyMatcher::kVkRight, true) == StreamHotkey::kNone,
           "Alt+Right without Ctrl must not fire");
  }
  // Key-ups never fire, even with the chord modifiers held.
  {
    StreamHotkeyMatcher matcher;
    PressCtrlAlt(matcher);
    Expect(Feed(matcher, StreamHotkeyMatcher::kVkF, false) == StreamHotkey::kNone,
           "F key-up must never fire");
  }
}

void TestRepeatSuppression() {
  StreamHotkeyMatcher matcher;
  PressCtrlAlt(matcher);
  Expect(Feed(matcher, StreamHotkeyMatcher::kVkF, true) == StreamHotkey::kToggleFullscreen,
         "first F press fires");
  // Auto-repeat: repeated key-downs while F stays held must not retrigger.
  Expect(Feed(matcher, StreamHotkeyMatcher::kVkF, true) == StreamHotkey::kNone,
         "auto-repeat F must not retrigger fullscreen");
  Expect(Feed(matcher, StreamHotkeyMatcher::kVkF, true) == StreamHotkey::kNone,
         "further auto-repeats must not retrigger either");
  // After the key-up the chord can fire again — a fresh physical press.
  Feed(matcher, StreamHotkeyMatcher::kVkF, false);
  Expect(Feed(matcher, StreamHotkeyMatcher::kVkF, true) == StreamHotkey::kToggleFullscreen,
         "a fresh press after release must fire again");
}

void TestSwallowTracking() {
  StreamHotkeyMatcher matcher;
  PressCtrlAlt(matcher);
  Feed(matcher, StreamHotkeyMatcher::kVkRight, true);
  // The completing key is swallowed while held: repeats and the key-up.
  Expect(matcher.SwallowsKey(StreamHotkeyMatcher::kVkRight),
         "completing key must be swallowed while held");
  Expect(!matcher.SwallowsKey(StreamHotkeyMatcher::kVkLeft),
         "other keys are not swallowed");
  Expect(!matcher.SwallowsKey(StreamHotkeyMatcher::kVkF),
         "uninvolved chord keys are not swallowed");
  // Modifier keys are never swallowed: the peer expects them as input.
  Expect(!matcher.SwallowsKey(StreamHotkeyMatcher::kVkControlLeft),
         "Ctrl must never be swallowed");
  Expect(!matcher.SwallowsKey(StreamHotkeyMatcher::kVkAltLeft),
         "Alt must never be swallowed");
  // The key-up ends the swallow state (and is itself consumed by the caller
  // querying SwallowsKey before Update — mirrored here).
  Expect(matcher.SwallowsKey(StreamHotkeyMatcher::kVkRight),
         "the ending key-up is itself still swallowed");
  Feed(matcher, StreamHotkeyMatcher::kVkRight, false);
  Expect(!matcher.SwallowsKey(StreamHotkeyMatcher::kVkRight),
         "swallow state ends after the key-up");
  Expect(!matcher.Swallowed(), "nothing swallowed after full release");
}

void TestPrimingCancellation() {
  // An unrelated key inside the chord cancels priming for stale repeats,
  // but a genuinely fresh chord press afterwards still fires (this models
  // the platform typematic behavior where pressing another key stops the
  // original key's auto-repeat).
  {
    StreamHotkeyMatcher matcher;
    PressCtrlAlt(matcher);
    Expect(Feed(matcher, 'X', true) == StreamHotkey::kNone,
           "unrelated key inside the chord does not fire");
    Expect(Feed(matcher, StreamHotkeyMatcher::kVkF, true) == StreamHotkey::kToggleFullscreen,
           "chord key after an unrelated key still fires as a fresh press");
  }
  // Releasing a modifier mid-sequence disarms: the completing key no longer
  // forms the chord.
  {
    StreamHotkeyMatcher matcher;
    PressCtrlAlt(matcher);
    Feed(matcher, StreamHotkeyMatcher::kVkAltLeft, false);
    Expect(Feed(matcher, StreamHotkeyMatcher::kVkF, true) == StreamHotkey::kNone,
           "chord key after Alt release must not fire");
  }
  // Reset clears all state: a chord completed across a reset cannot fire
  // from stale modifier memory.
  {
    StreamHotkeyMatcher matcher;
    PressCtrlAlt(matcher);
    matcher.Reset();
    Expect(Feed(matcher, StreamHotkeyMatcher::kVkF, true) == StreamHotkey::kNone,
           "reset forgets held modifiers");
    Expect(!matcher.Swallowed(), "reset clears the swallow state");
  }
}

void TestDisplayCycling() {
  // Single display: cycling is a no-op in both directions.
  Expect(StreamHotkeyMatcher::CycleDisplay(0, 1, true) == 0,
         "single display forward stays at 0");
  Expect(StreamHotkeyMatcher::CycleDisplay(0, 1, false) == 0,
         "single display backward stays at 0");
  Expect(StreamHotkeyMatcher::CycleDisplay(0, 0, true) == 0,
         "zero displays stay at 0");

  // Two displays: forward toggles, backward toggles inversely.
  Expect(StreamHotkeyMatcher::CycleDisplay(0, 2, true) == 1, "2 displays: 0 -> 1 forward");
  Expect(StreamHotkeyMatcher::CycleDisplay(1, 2, true) == 0, "2 displays: 1 wraps to 0 forward");
  Expect(StreamHotkeyMatcher::CycleDisplay(0, 2, false) == 1, "2 displays: 0 wraps to 1 backward");
  Expect(StreamHotkeyMatcher::CycleDisplay(1, 2, false) == 0, "2 displays: 1 -> 0 backward");

  // Five displays: walk the full ring in both directions.
  for (int current = 0; current < 5; ++current) {
    Expect(StreamHotkeyMatcher::CycleDisplay(current, 5, true) == (current + 1) % 5,
           "5 displays forward cycling must advance modulo 5");
    Expect(StreamHotkeyMatcher::CycleDisplay(current, 5, false) == (current + 4) % 5,
           "5 displays backward cycling must retreat modulo 5");
  }
  // Wrap endpoints explicitly.
  Expect(StreamHotkeyMatcher::CycleDisplay(4, 5, true) == 0, "last forward wraps to first");
  Expect(StreamHotkeyMatcher::CycleDisplay(0, 5, false) == 4, "first backward wraps to last");

  // Out-of-range current indices are returned unchanged, never wrapped into
  // a misleading valid display.
  Expect(StreamHotkeyMatcher::CycleDisplay(7, 5, true) == 7,
         "out-of-range index stays unchanged");
  Expect(StreamHotkeyMatcher::CycleDisplay(-1, 5, false) == -1,
         "negative index stays unchanged");
}

void TestFullChordSequence() {
  // A realistic sequence: modifiers down, chord fires, key-up swallowed via
  // the contract, modifiers released cleanly; a second chord then fires
  // independently.
  StreamHotkeyMatcher matcher;
  struct {
    int vk;
    bool down;
  } sequence[] = {
      {StreamHotkeyMatcher::kVkControlLeft, true},
      {StreamHotkeyMatcher::kVkAltLeft, true},
  };
  for (const auto& edge : sequence) Feed(matcher, edge.vk, edge.down);
  Expect(Feed(matcher, StreamHotkeyMatcher::kVkLeft, true) == StreamHotkey::kPrevDisplay,
         "first chord fires");
  // Simulate the consumer swallowing the held key and its release.
  Expect(matcher.SwallowsKey(StreamHotkeyMatcher::kVkLeft), "left swallowed while held");
  Feed(matcher, StreamHotkeyMatcher::kVkLeft, false);
  ReleaseCtrlAlt(matcher);
  Expect(!matcher.Swallowed(), "fully released after the sequence");
  // A completely new chord cycle works.
  PressCtrlAlt(matcher);
  Expect(Feed(matcher, StreamHotkeyMatcher::kVkRight, true) == StreamHotkey::kNextDisplay,
         "a fresh chord cycle fires independently");
}

}  // namespace

int main() {
  TestBitrateCapPresets();
  TestHotkeyRecognition();
  TestWrongModifierRejection();
  TestRepeatSuppression();
  TestSwallowTracking();
  TestPrimingCancellation();
  TestDisplayCycling();
  TestFullChordSequence();

  if (failures == 0) {
    std::cout << "stream_control_ergonomics_test: all tests passed\n";
    return 0;
  }
  std::cerr << "stream_control_ergonomics_test: " << failures << " failure(s)\n";
  return 1;
}
