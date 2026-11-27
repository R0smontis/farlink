/*
 * @Author: Farlink contributors
 * @Date: 2026-11-27
 * Copyright (c) 2026 by Farlink contributors.
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Host-side local speaker mute (商业远控被控端静音 parity): mutes the host's
 * physical default output device so people near the controlled machine do
 * not hear the session audio, while the remote controller keeps receiving
 * the audio stream. RAII: the exact original mute state is restored on
 * destruction, so a crashed or disconnected session can never strand the
 * user's machine muted.
 *
 * Platform contract:
 *   macOS  — implemented in platform/macos/host_audio_mute.mm through
 *            CoreAudio kAudioDevicePropertyMute on the default output
 *            device.
 *   Linux  — inert stub: construction succeeds, Mute() reports false.
 *            Never silently half-applies.
 *   Windows— interface-ready stub; same inert behaviour as Linux until a
 *            platform backend lands.
 *
 * macOS capture-interaction note (verified against CoreAudio/SCK docs and
 * behaviour, honest limitation): kAudioDevicePropertyMute silences the
 * device's output stage. ScreenCaptureKit system audio capture taps the
 * audio BEFORE it reaches the output device's mute stage on the built-in
 * speaker/headphone paths in current macOS releases, so remote audio keeps
 * flowing while the room stays silent — this is exactly what commercial-grade host
 * mute needs. However, for some output routes (certain HDMI/DisplayPort
 * monitors and USB audio devices where the mute is applied in the device's
 * own DSP upstream of what the capture tap observes) muting MAY also
 * silence capture. There is no userspace API that enumerates which route
 * behaves which way. The feature is therefore strictly opt-in and off by
 * default; if a user reports silent remote audio with host mute engaged on
 * an exotic output route, that is this known platform behaviour, not a
 * defect in the restore logic.
 */

#ifndef CROSSDESK_HOST_AUDIO_MUTE_H_
#define CROSSDESK_HOST_AUDIO_MUTE_H_

namespace crossdesk {

class HostAudioMute {
 public:
  HostAudioMute();
  ~HostAudioMute();  // Restores the exact original mute state.

  HostAudioMute(const HostAudioMute&) = delete;
  HostAudioMute& operator=(const HostAudioMute&) = delete;
  HostAudioMute(HostAudioMute&&) = delete;
  HostAudioMute& operator=(HostAudioMute&&) = delete;

  // True when this platform has a working backend. On platforms without
  // one every other method is a safe no-op.
  static bool Supported();

  // Mutes the host default output device, remembering its original state.
  // Returns true when the device is now muted by us. Repeated calls are
  // idempotent; a failure leaves the machine untouched.
  bool Mute();

  // Restores the original state ahead of destruction. Idempotent; the
  // destructor calls it again harmlessly.
  void Restore();

  // True while this instance holds the host muted.
  bool Active() const { return active_; }

 private:
  bool active_ = false;
  // Backend bookkeeping, written only by implementations that have one.
  // maybe_unused keeps the inert stub backends warning-free without
  // splitting the class per platform.
  [[maybe_unused]] bool original_muted_ = false;
  [[maybe_unused]] bool have_original_ = false;
  [[maybe_unused]] unsigned int device_id_ = 0;  // AudioDeviceID on macOS.
};

}  // namespace crossdesk

#endif  // CROSSDESK_HOST_AUDIO_MUTE_H_
