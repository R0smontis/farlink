/*
 * @Author: Farlink contributors
 * @Date: 2026-11-28
 * Copyright (c) 2026 by Farlink contributors.
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Platform-independent state machine behind HostAudioMute: which device was
 * muted, what its original state was, and exactly when writes happen. The
 * policy is header-only and free of every platform dependency so the unit
 * test can drive it with a mock backend and verify 100% of the mute/restore
 * logic — idempotency, read-before-write ordering, device pinning, the
 * already-muted edge case — with zero side effects on any real machine.
 *
 * Backend concept (duck-typed, supplied per call by the platform backend):
 *
 *   struct Backend {
 *     // Resolves the machine's current default output device to an opaque
 *     // key. Returns false when no usable default output device exists.
 *     bool GetDefaultDevice(std::string& key);
 *     // Capability answer only: false means the device exposes no mute
 *     // control at all (never an error — the machine is left untouched).
 *     bool SupportsMute(const std::string& key);
 *     // Reads/writes the mute state of the device identified by key, which
 *     // may be pinned from an earlier call and no longer be the default.
 *     // False means the operation failed (device gone, driver error); the
 *     // backend logs the platform-specific cause.
 *     bool ReadMute(const std::string& key, bool& muted);
 *     bool WriteMute(const std::string& key, bool muted);
 *   };
 *
 * The key is opaque to the policy: a decimal AudioDeviceID on macOS, the
 * UTF-8-encoded MMDevice endpoint id on Windows. Keys are only ever
 * produced by GetDefaultDevice and handed back to the same backend.
 */

#ifndef CROSSDESK_HOST_AUDIO_MUTE_POLICY_H_
#define CROSSDESK_HOST_AUDIO_MUTE_POLICY_H_

#include <string>
#include <utility>

namespace crossdesk {

class HostMutePolicy {
 public:
  bool active() const { return active_; }

  // Mutes the current default output device through backend, remembering
  // the device key and its original state. Idempotent: repeated calls while
  // active are a no-op success. On any failure nothing is written and the
  // machine is left exactly as found.
  //
  // Ordering invariant: the original state is READ before any write, and
  // the bookkeeping is recorded only after every fallible step has
  // succeeded. An already-muted device needs no write at all — its original
  // state is simply recorded as muted, so restore can never unmute a device
  // the user had muted before the session.
  template <typename Backend>
  bool Mute(Backend& backend) {
    if (active_) return true;
    std::string key;
    if (!backend.GetDefaultDevice(key)) return false;
    if (!backend.SupportsMute(key)) return false;
    bool original = false;
    if (!backend.ReadMute(key, original)) return false;
    if (!original && !backend.WriteMute(key, true)) return false;
    device_key_ = std::move(key);
    original_muted_ = original;
    have_original_ = true;
    active_ = true;
    return true;
  }

  // Restores the pinned device's original state. Idempotent and
  // failure-tolerant by design: restore is a best-effort write to the exact
  // device that was muted, never to whatever happens to be the default now.
  // If the pinned device was unplugged mid-session the write fails inside
  // the backend (which logs it) and no other device is ever touched.
  template <typename Backend>
  void Restore(Backend& backend) {
    if (!active_) return;
    active_ = false;
    if (!have_original_) return;
    have_original_ = false;
    backend.WriteMute(device_key_, original_muted_);
    device_key_.clear();
  }

 private:
  bool active_ = false;
  bool original_muted_ = false;
  bool have_original_ = false;
  std::string device_key_;
};

}  // namespace crossdesk

#endif  // CROSSDESK_HOST_AUDIO_MUTE_POLICY_H_
