/*
 * @Author: DI JUNKUN
 * @Date: 2024-10-17
 * Copyright (c) 2024 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _SCREEN_CAPTURER_SCK_H_
#define _SCREEN_CAPTURER_SCK_H_

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "screen_capturer.h"


#include "../virtual_display/mac_virtual_display_provisioner.h"

namespace crossdesk {

class ScreenCapturerSck : public ScreenCapturer {
 public:
  ScreenCapturerSck();
  ~ScreenCapturerSck();

 public:
  int Init(const int fps, cb_desktop_data cb) override;
  void SetPrivacyController(PrivacyController* privacy) override;
  void SetTrueColorEnabled(bool enabled) override;
  int Destroy() override;
  int Start(bool show_cursor) override;
  int Stop() override;

  int Pause(int monitor_index) override;
  int Resume(int monitor_index) override;

  int SwitchTo(int monitor_index) override;
  int ResetToInitialMonitor() override;

  std::vector<DisplayInfo> GetDisplayInfoList() override;

  void OnFrame();

 protected:
  void CleanUp();

 private:
  std::unique_ptr<ScreenCapturer> CreateScreenCapturerSck();

 private:
  int _fps;
  cb_desktop_data on_data_;
  unsigned char* nv12_frame_ = nullptr;
  bool inited_ = false;

  // thread
  std::thread capture_thread_;
  std::atomic_bool running_;

 private:
  std::unique_ptr<ScreenCapturer> screen_capturer_sck_impl_;
  PrivacyController* privacy_ = nullptr;
  bool true_color_enabled_ = false;

 public:
  // ── Virtual display lifecycle (macOS 11+) ────────────────────────────
  // macOS counterpart of the Windows usbmmidd provisioning path. The
  // capturer does NOT automatically create a virtual display because on
  // macOS 27.x creation is effectively permanent for the user session
  // (see mac_virtual_display.h). Instead the host runtime opts in:
  //
  //   - SetVirtualDisplayEnabled(true) before Init() requests creation of
  //     a virtual display when capture starts on a headless host (zero
  //     active physical displays) OR when the controller explicitly
  //     requested one.
  //   - SetVirtualDisplaySpec() configures the desired logical resolution
  //     and HiDPI mode. Ignored once the display has been created (macOS
  //     27.x cannot reconfigure).
  //
  // The provisioner instance is process-lifetime. Even if capture stops,
  // the display is intentionally kept alive — destroying it would not
  // remove it from the system on macOS 27.x anyway, and destroying+
  // recreating would risk further leaks.
  void SetVirtualDisplayEnabled(bool enabled) {
    virtual_display_enabled_ = enabled;
  }
  void SetVirtualDisplaySpec(const MacVirtualDisplaySpec& spec) {
    virtual_display_spec_ = spec;
  }
  // Exposed for diagnostics: the active provisioner, or nullptr if the
  // feature is disabled or the runtime unsupported.
  const MacVirtualDisplayProvisioner* virtual_display() const {
    return virtual_display_enabled_ ? &virtual_display_ : nullptr;
  }

 private:
  // Called from Start() before the capturer begins. Creates the virtual
  // display if policy says we need one. Failure is logged but never
  // blocks capture — physical-display capture is the fallback.
  void MaybeProvisionVirtualDisplay();
  // Count active physical (non-virtual) displays. Used to detect headless
  // operation. Excludes our own virtual display if active.
  uint32_t CountActivePhysicalDisplays() const;

  bool virtual_display_enabled_ = false;
  MacVirtualDisplaySpec virtual_display_spec_;
  MacVirtualDisplayProvisioner virtual_display_;
  // Set once the provisioner has been asked to create the display. We
  // never re-ask because reconfigure is unreliable on macOS 27.x.
  bool virtual_display_provisioned_ = false;
};
}  // namespace crossdesk
#endif
