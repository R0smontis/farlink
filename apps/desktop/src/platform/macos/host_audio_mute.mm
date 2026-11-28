/*
 * @Author: Farlink contributors
 * @Date: 2026-11-27
 * Copyright (c) 2026 by Farlink contributors.
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * macOS backend for HostAudioMute: drives kAudioDevicePropertyMute on the
 * current default output device. The original mute state and the device
 * identity are captured before the first write, so restoration targets the
 * exact device that was muted — even if the user switches the default
 * output mid-session, the machine cannot be left in a wrong state: the
 * original device gets its original state back, and no other device is
 * ever written.
 *
 * The state machine itself (what to capture, when to write, idempotency)
 * is the platform-independent HostMutePolicy; this file supplies only the
 * CoreAudio device IO. Device keys are decimal AudioDeviceID strings —
 * opaque tokens the policy never interprets.
 */

#import <CoreAudio/CoreAudio.h>
#import <Foundation/Foundation.h>

#include "host_audio_mute.h"

#include <string>

#include "rd_log.h"

namespace crossdesk {

namespace {

class CoreAudioMuteBackend {
 public:
  bool GetDefaultDevice(std::string& key) {
    AudioDeviceID device = kAudioObjectUnknown;
    AudioObjectPropertyAddress address{
        kAudioHardwarePropertyDefaultOutputDevice,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain};
    UInt32 size = sizeof(device);
    const OSStatus status = AudioObjectGetPropertyData(
        kAudioObjectSystemObject, &address, 0, nullptr, &size, &device);
    if (status != noErr || device == kAudioObjectUnknown) {
      LOG_WARN("Host audio mute: no default output device");
      return false;
    }
    key = std::to_string(static_cast<unsigned long long>(device));
    return true;
  }

  bool SupportsMute(const std::string& key) {
    const AudioDeviceID device = DeviceFromKey(key);
    if (device == kAudioObjectUnknown) return false;
    AudioObjectPropertyAddress address{
        kAudioDevicePropertyMute,
        kAudioDevicePropertyScopeOutput,
        kAudioObjectPropertyElementMain};
    // Most analog/USB outputs implement mute; some HDMI routes do not.
    // Absence of the property is a capability answer, not an error — the
    // machine is simply left untouched.
    const bool supported = AudioObjectHasProperty(device, &address) != 0;
    if (!supported) {
      LOG_INFO("Host audio mute: default output device does not support mute");
    }
    return supported;
  }

  bool ReadMute(const std::string& key, bool& muted) {
    const AudioDeviceID device = DeviceFromKey(key);
    if (device == kAudioObjectUnknown) return false;
    AudioObjectPropertyAddress address{
        kAudioDevicePropertyMute,
        kAudioObjectPropertyScopeOutput,
        kAudioObjectPropertyElementMain};
    UInt32 value = 0;
    UInt32 size = sizeof(value);
    const OSStatus status = AudioObjectGetPropertyData(
        device, &address, 0, nullptr, &size, &value);
    if (status != noErr) {
      LOG_WARN("Host audio mute: could not read the current mute state");
      return false;
    }
    muted = value != 0;
    return true;
  }

  // One write path serves both Mute() and Restore(); the log line names
  // the exact target state so a restore failure can never be misread as
  // a mute failure or vice versa.
  bool WriteMute(const std::string& key, bool muted) {
    const AudioDeviceID device = DeviceFromKey(key);
    if (device == kAudioObjectUnknown) {
      LOG_WARN(
          "Host audio mute: pinned output device is gone, cannot write mute={}",
          muted);
      return false;
    }
    AudioObjectPropertyAddress address{
        kAudioDevicePropertyMute,
        kAudioDevicePropertyScopeOutput,
        kAudioObjectPropertyElementMain};
    if (!AudioObjectHasProperty(device, &address)) {
      LOG_WARN(
          "Host audio mute: device has no mute control, cannot write mute={}",
          muted);
      return false;
    }
    UInt32 value = muted ? 1u : 0u;
    if (AudioObjectSetPropertyData(device, &address, 0, nullptr, sizeof(value),
                                   &value) != noErr) {
      LOG_WARN("Host audio mute: mute write failed (target mute={})", muted);
      return false;
    }
    return true;
  }

 private:
  static AudioDeviceID DeviceFromKey(const std::string& key) {
    if (key.empty()) return kAudioObjectUnknown;
    unsigned long long value = 0;
    try {
      value = std::stoull(key);
    } catch (...) {
      return kAudioObjectUnknown;
    }
    return static_cast<AudioDeviceID>(value);
  }
};

}  // namespace

HostAudioMute::HostAudioMute() = default;

HostAudioMute::~HostAudioMute() { Restore(); }

bool HostAudioMute::Supported() { return true; }

bool HostAudioMute::Mute() {
  CoreAudioMuteBackend backend;
  return policy_.Mute(backend);
}

void HostAudioMute::Restore() {
  CoreAudioMuteBackend backend;
  policy_.Restore(backend);
}

}  // namespace crossdesk
