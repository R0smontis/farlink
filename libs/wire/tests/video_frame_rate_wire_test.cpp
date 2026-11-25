/*
 * @Author: DI JUNKUN
 * @Date: 2026-11-25
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
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

  return ok ? 0 : 1;
}
