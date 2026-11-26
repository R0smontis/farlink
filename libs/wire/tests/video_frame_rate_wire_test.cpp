/*
 * Copyright (c) 2026 Farlink contributors
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include <iostream>
#include <string>

#include <nlohmann/json.hpp>
#include <preview_subscription.h>
#include <remote_action.h>
#include <video_frame_rate.h>

namespace {

bool Expect(bool condition, const char* message) {
  if (condition) return true;
  std::cerr << message << '\n';
  return false;
}

// Build a minimal valid interactive deskhub_preview subscription JSON with the
// given frame_rate, mirroring PreviewSubscription::Interactive plus a request id.
nlohmann::json SubscriptionJson(int frame_rate) {
  return {{"type", "deskhub_preview"},
          {"version", 1},
          {"request_id", 1},
          {"fps", 60},
          {"quality", 2},
          {"frame_rate", frame_rate},
          {"preference", 1},
          {"audio", false}};
}

crossdesk::RemoteAction VideoSettingsJson(int frame_rate) {
  crossdesk::RemoteAction action{};
  action.type = crossdesk::ControlType::video_settings;
  action.vs = {2, frame_rate, 1, 0, false};
  return action;
}

}  // namespace

int main() {
  bool ok = true;

  // Tier table and membership.
  ok &= Expect(crossdesk::kVideoFrameRateTierCount == 5,
               "frame-rate tier count changed");
  ok &= Expect(crossdesk::kMaxVideoFrameRate == 144,
               "max frame-rate tier changed");
  for (int tier : {30, 60, 90, 120, 144})
    ok &= Expect(crossdesk::IsValidVideoFrameRate(tier),
                 "supported frame-rate tier rejected");
  for (int invalid : {0, -1, 15, 45, 61, 100, 145, 240})
    ok &= Expect(!crossdesk::IsValidVideoFrameRate(invalid),
                 "invalid frame-rate tier accepted");

  // PreviewSubscription::Parse accepts every tier and rejects non-tiers.
  for (int tier : {30, 60, 90, 120, 144}) {
    const auto parsed =
        crossdesk::PreviewSubscription::Parse(SubscriptionJson(tier));
    ok &= Expect(parsed.has_value() && parsed->frame_rate == tier,
                 "preview subscription tier not parsed");
  }
  for (int invalid : {0, -1, 61, 145}) {
    auto json = SubscriptionJson(invalid);
    if (invalid < 0) json["frame_rate"] = invalid;
    ok &= Expect(!crossdesk::PreviewSubscription::Parse(json).has_value(),
                 "preview subscription invalid tier accepted");
  }

  // ValidVideoSettings accepts every tier and rejects non-tiers.
  for (int tier : {30, 60, 90, 120, 144}) {
    crossdesk::VideoSettings settings{2, tier, 1, 0, false};
    ok &= Expect(crossdesk::ValidVideoSettings(settings),
                 "video settings tier rejected");
  }
  for (int invalid : {0, -1, 61, 145}) {
    crossdesk::VideoSettings settings{2, invalid, 1, 0, false};
    ok &= Expect(!crossdesk::ValidVideoSettings(settings),
                 "video settings invalid tier accepted");
  }

  // video_settings wire round trip preserves high tiers.
  for (int tier : {30, 60, 90, 120, 144}) {
    auto action = VideoSettingsJson(tier);
    const auto message = action.to_json();
    ok &= Expect(!message.empty(), "video settings failed to serialize");
    crossdesk::RemoteAction parsed{};
    ok &= Expect(parsed.from_json(message) &&
                     parsed.vs.frame_rate == tier,
                 "video settings high tier round trip changed");
  }
  // A non-tier frame rate never serializes.
  {
    auto action = VideoSettingsJson(61);
    ok &= Expect(action.to_json().empty(),
                 "video settings invalid tier serialized");
  }

  // Send-side negotiation: without the peer capability every tier above 60
  // clamps to the legacy 30/60 format; with it the tier passes through.
  for (int tier : {90, 120, 144}) {
    ok &= Expect(crossdesk::NegotiatedVideoFrameRate(tier, false) == 60,
                 "high tier not clamped without capability");
    ok &= Expect(crossdesk::NegotiatedVideoFrameRate(tier, true) == tier,
                 "high tier not passed through with capability");
  }
  ok &= Expect(crossdesk::NegotiatedVideoFrameRate(30, false) == 30 &&
                   crossdesk::NegotiatedVideoFrameRate(60, false) == 60,
               "legacy tiers changed without capability");
  ok &= Expect(crossdesk::NegotiatedVideoFrameRate(30, true) == 30 &&
                   crossdesk::NegotiatedVideoFrameRate(60, true) == 60,
               "legacy tiers changed with capability");
  // Unknown values fall back to the legacy binary mapping either way.
  ok &= Expect(crossdesk::NegotiatedVideoFrameRate(45, true) == 60 &&
                   crossdesk::NegotiatedVideoFrameRate(45, false) == 60,
               "unknown frame rate did not fall back to legacy mapping");
  ok &= Expect(crossdesk::NegotiatedVideoFrameRate(20, true) == 30,
               "low frame rate did not clamp to 30");

  // Index mapping used by the settings UI.
  ok &= Expect(crossdesk::VideoFrameRateIndex(30) == 0 &&
                   crossdesk::VideoFrameRateIndex(60) == 1 &&
                   crossdesk::VideoFrameRateIndex(90) == 2 &&
                   crossdesk::VideoFrameRateIndex(120) == 3 &&
                   crossdesk::VideoFrameRateIndex(144) == 4,
               "frame-rate index mapping changed");
  ok &= Expect(crossdesk::VideoFrameRateIndex(45) == 1,
               "unknown frame rate did not fall back to the 60 index");
  ok &= Expect(crossdesk::VideoFrameRateForIndex(0) == 30 &&
                   crossdesk::VideoFrameRateForIndex(1) == 60 &&
                   crossdesk::VideoFrameRateForIndex(2) == 90 &&
                   crossdesk::VideoFrameRateForIndex(3) == 120 &&
                   crossdesk::VideoFrameRateForIndex(4) == 144,
               "frame-rate for index mapping changed");
  ok &= Expect(crossdesk::VideoFrameRateForIndex(-1) == 60 &&
                   crossdesk::VideoFrameRateForIndex(5) == 60,
               "out-of-range index did not fall back to 60");

  // HostInfo capability round trip: supports_high_fps survives serialization
  // and defaults to false for a legacy payload that omits the key.
  {
    crossdesk::RemoteAction host = crossdesk::MakeHostInformation(
        "host", {}, false, "1.0.0", crossdesk::HostPlatform::MacOS);
    host.i.supports_high_fps = true;
    const auto message = host.to_json();
    ok &= Expect(message.find("supports_high_fps") != std::string::npos,
                 "supports_high_fps not serialized");
    crossdesk::RemoteAction parsed{};
    ok &= Expect(parsed.from_json(message) && parsed.i.supports_high_fps,
                 "supports_high_fps round trip lost");
    crossdesk::FreeRemoteAction(parsed);
    crossdesk::FreeRemoteAction(host);
  }
  {
    // A legacy payload without the key must parse with the capability false.
    const std::string legacy =
        "{\"type\":3,\"host_info\":{\"host_name\":\"h\",\"display_num\":0,"
        "\"displays\":[]}}";
    crossdesk::RemoteAction parsed{};
    ok &= Expect(parsed.from_json(legacy) && !parsed.i.supports_high_fps,
                 "omitted supports_high_fps did not default to false");
    crossdesk::FreeRemoteAction(parsed);
  }

  // Degradation preference is a pure function of the frame rate: above 60 fps
  // the stream maintains frame rate (gives up resolution first), at or below
  // 60 fps it maintains resolution (gives up frames first). The boundary is
  // exact and the wire values are 0 = maintain frame rate, 1 = maintain
  // resolution.
  for (int high : {90, 120, 144})
    ok &= Expect(crossdesk::DegradationPreferenceForFrameRate(high) == 0,
                 "high frame rate did not select maintain frame rate");
  for (int low : {30, 60})
    ok &= Expect(crossdesk::DegradationPreferenceForFrameRate(low) == 1,
                 "low frame rate did not select maintain resolution");
  // 60 is the boundary and stays maintain-resolution; anything above flips.
  ok &= Expect(crossdesk::DegradationPreferenceForFrameRate(60) == 1 &&
                   crossdesk::DegradationPreferenceForFrameRate(61) == 0,
               "60 fps degradation boundary moved");

  // --- Display-refresh-aware tier selection (peer x refresh x requested) ---
  // Fractional panel rates round to their nominal tier: NTSC-style 59.94 is a
  // 60 Hz panel, 119.88 is a 120 Hz panel, so the panel is credited with the
  // tier it is sold as rather than the one below its exact rate.
  ok &= Expect(crossdesk::MaxVideoFrameRateForRefresh(59.94) == 60 &&
                   crossdesk::MaxVideoFrameRateForRefresh(60.0) == 60,
               "60 Hz-class panel must cap at 60");
  ok &= Expect(crossdesk::MaxVideoFrameRateForRefresh(119.88) == 120 &&
                   crossdesk::MaxVideoFrameRateForRefresh(120.0) == 120,
               "120 Hz-class panel must cap at 120");
  ok &= Expect(crossdesk::MaxVideoFrameRateForRefresh(143.86) == 144 &&
                   crossdesk::MaxVideoFrameRateForRefresh(144.0) == 144,
               "144 Hz-class panel must cap at 144");
  ok &= Expect(crossdesk::MaxVideoFrameRateForRefresh(75.0) == 60 &&
                   crossdesk::MaxVideoFrameRateForRefresh(100.0) == 90,
               "odd panel rates must cap at the highest tier not above them");
  ok &= Expect(crossdesk::MaxVideoFrameRateForRefresh(165.0) == 144,
               "panel faster than any tier must cap at the top tier");
  ok &= Expect(crossdesk::MaxVideoFrameRateForRefresh(0.0) == 144 &&
                   crossdesk::MaxVideoFrameRateForRefresh(-1.0) == 144,
               "unknown refresh must impose no restriction");
  ok &= Expect(crossdesk::MaxVideoFrameRateForRefresh(24.0) == 30,
               "panel slower than every tier still offers the base tier");

  // 30/60 are always offered regardless of capability or panel; high tiers
  // need both the peer capability and a panel that can present them.
  for (int legacy : {30, 60}) {
    ok &= Expect(crossdesk::VideoFrameRateTierOffered(legacy, false, 60.0),
                 "legacy tier must always be offered");
  }
  ok &= Expect(!crossdesk::VideoFrameRateTierOffered(120, false, 144.0),
               "high tier offered without peer capability");
  ok &= Expect(!crossdesk::VideoFrameRateTierOffered(120, true, 60.0),
               "high tier offered on a panel too slow for it");
  ok &= Expect(crossdesk::VideoFrameRateTierOffered(120, true, 120.0),
               "120 tier must be offered on a 120 Hz panel with a capable peer");
  ok &= Expect(crossdesk::VideoFrameRateTierOffered(120, true, 119.88),
               "120 tier must be offered on a fractional 120 Hz panel");
  ok &= Expect(crossdesk::VideoFrameRateTierOffered(144, true, 0.0),
               "unknown refresh must not restrict offered tiers");
  ok &= Expect(!crossdesk::VideoFrameRateTierOffered(61, true, 144.0),
               "invalid tier must never be offered");

  // Effective tier: requested when offered, else the highest offered tier.
  ok &= Expect(crossdesk::EffectiveVideoFrameRate(144, true, 144.0) == 144,
               "capable 144 request on a 144 panel must be honoured");
  ok &= Expect(crossdesk::EffectiveVideoFrameRate(144, true, 60.0) == 60,
               "144 request on a 60 Hz panel must drop to 60");
  ok &= Expect(crossdesk::EffectiveVideoFrameRate(144, true, 120.0) == 120,
               "144 request on a 120 Hz panel must drop to 120, not 60");
  ok &= Expect(crossdesk::EffectiveVideoFrameRate(144, false, 144.0) == 60,
               "high request without peer capability must drop to 60");
  ok &= Expect(crossdesk::EffectiveVideoFrameRate(90, true, 119.88) == 90,
               "90 request on a 120 panel must be honoured");
  ok &= Expect(crossdesk::EffectiveVideoFrameRate(60, true, 60.0) == 60,
               "60 must always be selectable");
  ok &= Expect(crossdesk::EffectiveVideoFrameRate(30, false, 60.0) == 30,
               "30 must remain selectable on a stock peer");
  ok &= Expect(crossdesk::EffectiveVideoFrameRate(0, true, 144.0) == 60,
               "invalid request must fall back to 60");
  ok &= Expect(crossdesk::EffectiveVideoFrameRate(144, true, 0.0) == 144,
               "unknown refresh must honour a capable request");
  return ok ? 0 : 1;
}
