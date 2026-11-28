/*
 * @Author: Farlink contributors
 * @Date: 2026-11-28
 * Copyright (c) 2026 by Farlink contributors.
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Windows backend for HostAudioMute: drives IAudioEndpointVolume::SetMute
 * on the default render endpoint (eRender/eConsole) through the Core Audio
 * MMDevice API.
 *
 * Pinning contract: IMMDevice instances are transient COM objects, but the
 * endpoint id string (IMMDevice::GetId) is stable for the lifetime of the
 * device on the machine. The id is captured at mute time and stored in the
 * policy as a UTF-8 key; restore re-resolves that exact endpoint through
 * IMMDeviceEnumerator::GetDevice. If the user plugs in headphones and the
 * default output changes mid-session, the original speaker still gets its
 * original state back and the new device is never written; if the original
 * device is gone, the restore write fails cleanly and nothing else is
 * touched.
 *
 * COM apartment contract: Mute()/Restore() may run on MiniRTC's data
 * callback thread or a disconnect cleanup thread, whose COM state is
 * unknown — uninitialized, MTA, or STA. Every public backend operation
 * scopes its own CoInitializeEx for the WHOLE operation: a successful
 * S_OK/S_FALSE initialization is balanced with CoUninitialize only after
 * every interface pointer created inside the operation has been released,
 * while RPC_E_CHANGED_MODE means the thread already runs COM in a
 * different (STA) apartment — MMDevice activation is apartment-agnostic
 * for these in-proc objects, so the operation proceeds in the existing
 * apartment and nothing is uninitialized (same pattern as
 * winrt_apartment.h). All COM objects are created, used and released
 * inside one call scope, so no interface pointer ever crosses a thread or
 * apartment boundary, and none outlives the COM initialization it depends
 * on.
 *
 * The state machine itself (what to capture, when to write, idempotency)
 * is the platform-independent HostMutePolicy; this file supplies only the
 * Core Audio device IO.
 */

#include "host_audio_mute.h"

#include <Mmdeviceapi.h>
#include <combaseapi.h>
#include <endpointvolume.h>
#include <wrl/client.h>

#include <string>

#include "rd_log.h"

using Microsoft::WRL::ComPtr;

namespace crossdesk {

namespace {

// Balances exactly one successful CoInitializeEx on this thread. An
// existing STA (RPC_E_CHANGED_MODE) is valid for MMDevice activation but
// is never uninitialized — it belongs to whoever initialized it.
class ScopedComInit {
 public:
  ScopedComInit() : result_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
  ~ScopedComInit() {
    if (SUCCEEDED(result_)) CoUninitialize();
  }
  ScopedComInit(const ScopedComInit&) = delete;
  ScopedComInit& operator=(const ScopedComInit&) = delete;

  bool ready() const {
    return SUCCEEDED(result_) || result_ == RPC_E_CHANGED_MODE;
  }
  HRESULT result() const { return result_; }

 private:
  HRESULT result_;
};

std::string WideToUtf8(const wchar_t* wide) {
  if (!wide) return {};
  const int len = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0,
                                      nullptr, nullptr);
  if (len <= 1) return {};
  std::string out(static_cast<size_t>(len - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), len, nullptr, nullptr);
  return out;
}

std::wstring Utf8ToWide(const std::string& utf8) {
  if (utf8.empty()) return {};
  const int len =
      MultiByteToWideChar(CP_UTF8, 0, utf8.data(), -1, nullptr, 0);
  if (len <= 1) return {};
  std::wstring out(static_cast<size_t>(len - 1), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.data(), -1, out.data(), len);
  return out;
}

// One fresh MMDeviceEnumerator per operation. Assumes COM is already
// initialized on this thread by the caller's ScopedComInit.
ComPtr<IMMDeviceEnumerator> CreateEnumerator(HRESULT* hr_out) {
  ComPtr<IMMDeviceEnumerator> enumerator;
  const HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                      CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
  if (hr_out) *hr_out = hr;
  if (FAILED(hr)) return nullptr;
  return enumerator;
}

// Resolves an endpoint id key to a live device. A null result is the
// normal unplugged-mid-session answer, not an error worth more than the
// caller's own log line.
ComPtr<IMMDevice> ResolveEndpoint(IMMDeviceEnumerator* enumerator,
                                  const std::string& key) {
  const std::wstring wide_id = Utf8ToWide(key);
  if (wide_id.empty()) return nullptr;
  ComPtr<IMMDevice> device;
  if (FAILED(enumerator->GetDevice(wide_id.c_str(), &device))) return nullptr;
  return device;
}

ComPtr<IAudioEndpointVolume> ActivateEndpointVolume(IMMDevice* device,
                                                    HRESULT* hr_out) {
  ComPtr<IAudioEndpointVolume> volume;
  const HRESULT hr = device->Activate(__uuidof(IAudioEndpointVolume),
                                      CLSCTX_ALL, nullptr, &volume);
  if (hr_out) *hr_out = hr;
  if (FAILED(hr)) return nullptr;
  return volume;
}

// Full fresh activation chain for one pinned-key operation: enumerator,
// endpoint resolved by id, volume interface activated. Assumes COM is
// initialized by the caller. Nothing is cached anywhere, so no interface
// pointer ever crosses an apartment or outlives the operation.
bool ActivateByKey(const std::string& key, ComPtr<IAudioEndpointVolume>* out,
                   bool log_activation_failure) {
  HRESULT hr = S_OK;
  ComPtr<IMMDeviceEnumerator> enumerator = CreateEnumerator(&hr);
  if (!enumerator) {
    LOG_WARN("Host audio mute: MMDeviceEnumerator creation failed, hr=0x{:08X}",
             static_cast<uint32_t>(hr));
    return false;
  }
  ComPtr<IMMDevice> device = ResolveEndpoint(enumerator.Get(), key);
  if (!device) return false;
  ComPtr<IAudioEndpointVolume> volume = ActivateEndpointVolume(device.Get(), &hr);
  if (!volume) {
    if (log_activation_failure) {
      LOG_WARN("Host audio mute: endpoint volume activation failed, hr=0x{:08X}",
               static_cast<uint32_t>(hr));
    }
    return false;
  }
  *out = volume;
  return true;
}

class WindowsMuteBackend {
 public:
  bool GetDefaultDevice(std::string& key) {
    ScopedComInit com;
    if (!com.ready()) {
      LOG_WARN("Host audio mute: COM initialization failed, hr=0x{:08X}",
               static_cast<uint32_t>(com.result()));
      return false;
    }
    HRESULT hr = S_OK;
    ComPtr<IMMDeviceEnumerator> enumerator = CreateEnumerator(&hr);
    if (!enumerator) {
      LOG_WARN("Host audio mute: MMDeviceEnumerator creation failed, hr=0x{:08X}",
               static_cast<uint32_t>(hr));
      return false;
    }
    ComPtr<IMMDevice> device;
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    if (FAILED(hr)) {
      // E_NOTFOUND: the machine has no render endpoint at all — a
      // capability answer, not an error worth warning about loudly.
      LOG_INFO("Host audio mute: no default render endpoint, hr=0x{:08X}",
               static_cast<uint32_t>(hr));
      return false;
    }
    LPWSTR id = nullptr;
    hr = device->GetId(&id);
    if (FAILED(hr) || !id) {
      LOG_WARN("Host audio mute: endpoint id query failed, hr=0x{:08X}",
               static_cast<uint32_t>(hr));
      return false;
    }
    key = WideToUtf8(id);
    CoTaskMemFree(id);
    return !key.empty();
  }

  bool SupportsMute(const std::string& key) {
    ScopedComInit com;
    if (!com.ready()) {
      LOG_WARN("Host audio mute: COM initialization failed, hr=0x{:08X}",
               static_cast<uint32_t>(com.result()));
      return false;
    }
    ComPtr<IAudioEndpointVolume> volume;
    if (!ActivateByKey(key, &volume, true)) return false;
    // Capability rule, per the IAudioEndpointVolume documentation: where the
    // adapter has no hardware mute control the interface IMPLEMENTS mute in
    // software, so SetMute is defined behaviour on every render endpoint the
    // interface activates on. Activation + a successful GetMute are therefore
    // the whole capability test — gating on QueryHardwareSupport's
    // ENDPOINT_HARDWARE_SUPPORT_MUTE bit would falsely refuse devices whose
    // mute is software-implemented and perfectly functional. The hardware
    // mask is still read, once, as observability for the exotic-route caveat
    // documented in host_audio_mute.h (driver-applied mute upstream of the
    // loopback tap is a hardware-path behaviour).
    DWORD support = 0;
    BOOL value = FALSE;
    const HRESULT hr_support = volume->QueryHardwareSupport(&support);
    const HRESULT hr_mute = volume->GetMute(&value);
    if (FAILED(hr_mute)) {
      LOG_WARN("Host audio mute: GetMute failed, hr=0x{:08X}",
               static_cast<uint32_t>(hr_mute));
      return false;
    }
    if (SUCCEEDED(hr_support) && (support & ENDPOINT_HARDWARE_SUPPORT_MUTE) == 0) {
      LOG_INFO("Host audio mute: endpoint mute is software-implemented (no hardware mute control)");
    }
    return true;
  }

  bool ReadMute(const std::string& key, bool& muted) {
    ScopedComInit com;
    if (!com.ready()) {
      LOG_WARN("Host audio mute: COM initialization failed, hr=0x{:08X}",
               static_cast<uint32_t>(com.result()));
      return false;
    }
    ComPtr<IAudioEndpointVolume> volume;
    if (!ActivateByKey(key, &volume, true)) return false;
    BOOL value = FALSE;
    const HRESULT hr = volume->GetMute(&value);
    if (FAILED(hr)) {
      LOG_WARN("Host audio mute: GetMute failed, hr=0x{:08X}",
               static_cast<uint32_t>(hr));
      return false;
    }
    muted = value != FALSE;
    return true;
  }

  bool WriteMute(const std::string& key, bool muted) {
    ScopedComInit com;
    if (!com.ready()) {
      LOG_WARN("Host audio mute: COM initialization failed, hr=0x{:08X}",
               static_cast<uint32_t>(com.result()));
      return false;
    }
    ComPtr<IAudioEndpointVolume> volume;
    if (!ActivateByKey(key, &volume, false)) {
      LOG_WARN("Host audio mute: pinned endpoint unavailable, cannot write mute={}",
               muted);
      return false;
    }
    // nullptr event context: the change is endpoint-level, not attributed
    // to any audio session. That is the documented usage for a deliberate
    // endpoint mute.
    const HRESULT hr = volume->SetMute(muted ? TRUE : FALSE, nullptr);
    if (FAILED(hr)) {
      LOG_WARN("Host audio mute: SetMute({}) failed, hr=0x{:08X}", muted,
               static_cast<uint32_t>(hr));
      return false;
    }
    return true;
  }
};

}  // namespace

HostAudioMute::HostAudioMute() = default;

HostAudioMute::~HostAudioMute() { Restore(); }

bool HostAudioMute::Supported() { return true; }

bool HostAudioMute::Mute() {
  WindowsMuteBackend backend;
  return policy_.Mute(backend);
}

void HostAudioMute::Restore() {
  WindowsMuteBackend backend;
  policy_.Restore(backend);
}

}  // namespace crossdesk
