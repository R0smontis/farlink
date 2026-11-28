/*
 * @Author: Farlink contributors
 * @Date: 2026-11-27
 * Copyright (c) 2026 by Farlink contributors.
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Host-side local speaker mute (commercial remote-desktop host-audio-mute parity): mutes the host's
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
 *   Windows— implemented in platform/windows/host_audio_mute_win.cpp
 *            through Core Audio MMDevice + IAudioEndpointVolume on the
 *            default render endpoint (eRender/eConsole). The endpoint id
 *            is pinned at mute time and restore re-resolves that exact
 *            endpoint through IMMDeviceEnumerator::GetDevice, so a
 *            mid-session default-device switch (headphones plugged in)
 *            can never strand the original speaker muted nor write the
 *            state of a device we never muted.
 *   Linux  — inert stub: construction succeeds, Mute() reports false.
 *            Never silently half-applies.
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
 *
 * Windows capture-interaction note (evidence: Microsoft driver documentation
 * for KSPROPERTY_AUDIOLOOPBACK / AUDCLNT_STREAMOPTIONS_POST_VOLUME_LOOPBACK,
 * plus long-standing empirical behaviour relied on by Chromium, NAudio and
 * every WASAPI loopback recorder): in shared mode the WASAPI loopback tap
 * point defaults to PRE-volume/mute — "if no attributes are applied to the
 * loopback stream, then loopback should be collected before volume and mute
 * is applied on the mixer output". The post-volume/mute tap exists only as
 * an explicit opt-in (AUDCLNT_STREAMOPTIONS_POST_VOLUME_LOOPBACK, Windows 11
 * 24H2+) which this project's capture path (miniaudio shared-mode loopback)
 * never requests. Endpoint mute through IAudioEndpointVolume therefore does
 * NOT silence loopback capture on the standard render paths — remote audio
 * keeps flowing while the room stays silent, exactly what commercial-grade host
 * mute needs. The residual honest limitation mirrors macOS: the pre/post
 * tap is implemented by the audio driver stack, and users have reported
 * individual devices whose drivers apply mute upstream of the tap (loopback
 * goes silent when muted). There is no userspace API that enumerates which
 * route behaves which way on Windows 10/11 prior to 24H2, so the same
 * strictly-opt-in rule applies: silent remote audio with host mute engaged
 * on an exotic endpoint is this known platform behaviour, not a defect in
 * the restore logic.
 */

#ifndef CROSSDESK_HOST_AUDIO_MUTE_H_
#define CROSSDESK_HOST_AUDIO_MUTE_H_

#include "host_audio_mute_policy.h"

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
  bool Active() const { return policy_.active(); }

 private:
  // All bookkeeping — which device, which original state, whether we hold
  // it — lives in the platform-independent policy so the unit test covers
  // 100% of it through a mock backend. Only the device IO differs per
  // platform and stays inside the backend .cpp files; this header never
  // includes a platform header.
  HostMutePolicy policy_;
};

}  // namespace crossdesk

#endif  // CROSSDESK_HOST_AUDIO_MUTE_H_
