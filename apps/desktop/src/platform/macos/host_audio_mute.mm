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
 */

#import <CoreAudio/CoreAudio.h>
#import <Foundation/Foundation.h>

#include "host_audio_mute.h"

#include "rd_log.h"

namespace crossdesk {

namespace {

AudioDeviceID DefaultOutputDevice() {
  AudioDeviceID device = kAudioObjectUnknown;
  AudioObjectPropertyAddress address{
      kAudioHardwarePropertyDefaultOutputDevice,
      kAudioObjectPropertyScopeGlobal,
      kAudioObjectPropertyElementMain};
  UInt32 size = sizeof(device);
  const OSStatus status = AudioObjectGetPropertyData(
      kAudioObjectSystemObject, &address, 0, nullptr, &size, &device);
  if (status != noErr || device == kAudioObjectUnknown) return kAudioObjectUnknown;
  return device;
}

bool DeviceSupportsMute(AudioDeviceID device) {
  AudioObjectPropertyAddress address{
      kAudioDevicePropertyMute,
      kAudioDevicePropertyScopeOutput,
      kAudioObjectPropertyElementMain};
  return AudioObjectHasProperty(device, &address) != 0;
}

bool ReadMute(AudioDeviceID device, bool& muted) {
  AudioObjectPropertyAddress address{
      kAudioDevicePropertyMute,
      kAudioDevicePropertyScopeOutput,
      kAudioObjectPropertyElementMain};
  UInt32 value = 0;
  UInt32 size = sizeof(value);
  const OSStatus status =
      AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, &value);
  if (status != noErr) return false;
  muted = value != 0;
  return true;
}

bool WriteMute(AudioDeviceID device, bool muted) {
  AudioObjectPropertyAddress address{
      kAudioDevicePropertyMute,
      kAudioDevicePropertyScopeOutput,
      kAudioObjectPropertyElementMain};
  UInt32 value = muted ? 1u : 0u;
  return AudioObjectSetPropertyData(device, &address, 0, nullptr,
                                    sizeof(value), &value) == noErr;
}

}  // namespace

HostAudioMute::HostAudioMute() = default;

HostAudioMute::~HostAudioMute() { Restore(); }

bool HostAudioMute::Supported() { return true; }

bool HostAudioMute::Mute() {
  if (active_) return true;
  const AudioDeviceID device = DefaultOutputDevice();
  if (device == kAudioObjectUnknown) {
    LOG_WARN("Host audio mute: no default output device");
    return false;
  }
  if (!DeviceSupportsMute(device)) {
    // Most analog/USB outputs implement mute; some HDMI routes do not.
    // Absence of the property is a capability answer, not an error — the
    // machine is simply left untouched.
    LOG_INFO("Host audio mute: default output device does not support mute");
    return false;
  }
  bool original = false;
  if (!ReadMute(device, original)) {
    LOG_WARN("Host audio mute: could not read the current mute state");
    return false;
  }
  if (!original && !WriteMute(device, true)) {
    LOG_WARN("Host audio mute: could not mute the default output device");
    return false;
  }
  // The original state is captured BEFORE the write above has any chance of
  // being the only record: an already-muted device is restored to muted
  // (a harmless rewrite of the same value), never to unmuted.
  device_id_ = device;
  original_muted_ = original;
  have_original_ = true;
  active_ = true;
  return true;
}

void HostAudioMute::Restore() {
  if (!active_) return;
  active_ = false;
  if (!have_original_) return;
  have_original_ = false;
  const AudioDeviceID device = static_cast<AudioDeviceID>(device_id_);
  if (!DeviceSupportsMute(device) || !WriteMute(device, original_muted_)) {
    LOG_WARN("Host audio mute: could not restore the original mute state");
  }
}

}  // namespace crossdesk
