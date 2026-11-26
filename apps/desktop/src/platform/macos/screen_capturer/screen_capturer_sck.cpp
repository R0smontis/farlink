#include "screen_capturer_sck.h"

#include <CoreGraphics/CoreGraphics.h>

#include "rd_log.h"

namespace crossdesk {

ScreenCapturerSck::ScreenCapturerSck() {}
ScreenCapturerSck::~ScreenCapturerSck() {}

void ScreenCapturerSck::SetPrivacyController(PrivacyController* privacy) {
  privacy_ = privacy;
  if (screen_capturer_sck_impl_)
    screen_capturer_sck_impl_->SetPrivacyController(privacy);
}

void ScreenCapturerSck::SetTrueColorEnabled(bool enabled) {
  true_color_enabled_ = enabled;
  if (screen_capturer_sck_impl_)
    screen_capturer_sck_impl_->SetTrueColorEnabled(enabled);
}

int ScreenCapturerSck::Init(const int fps, cb_desktop_data cb) {
  if (cb) {
    on_data_ = cb;
  } else {
    LOG_ERROR("cb is null");
    return -1;
  }

  screen_capturer_sck_impl_ = CreateScreenCapturerSck();
  screen_capturer_sck_impl_->SetPrivacyController(privacy_);
  screen_capturer_sck_impl_->SetTrueColorEnabled(true_color_enabled_);
  const int ret = screen_capturer_sck_impl_->Init(fps, on_data_);
  if (ret != 0) {
    screen_capturer_sck_impl_.reset();
    return ret;
  }

  return 0;
}

int ScreenCapturerSck::Destroy() {
  if (screen_capturer_sck_impl_) {
    screen_capturer_sck_impl_->Destroy();
  }
  return 0;
}

int ScreenCapturerSck::Start(bool show_cursor) {
  if (!screen_capturer_sck_impl_) {
    return -1;
  }

  // Provision the virtual display before capture starts. This is a no-op
  // when the feature is disabled, the runtime unsupported, or physical
  // displays are present. Failure is logged but never blocks capture.
  MaybeProvisionVirtualDisplay();

  return screen_capturer_sck_impl_->Start(show_cursor);
}

int ScreenCapturerSck::Stop() {
  if (screen_capturer_sck_impl_) {
    screen_capturer_sck_impl_->Stop();
  }
  return 0;
}

int ScreenCapturerSck::Pause(int monitor_index) {
  if (screen_capturer_sck_impl_) {
    return screen_capturer_sck_impl_->Pause(monitor_index);
  }
  return 0;
}

int ScreenCapturerSck::Resume(int monitor_index) {
  if (screen_capturer_sck_impl_) {
    return screen_capturer_sck_impl_->Resume(monitor_index);
  }
  return 0;
}

int ScreenCapturerSck::SwitchTo(int monitor_index) {
  if (screen_capturer_sck_impl_) {
    return screen_capturer_sck_impl_->SwitchTo(monitor_index);
  }

  return -1;
}

int ScreenCapturerSck::ResetToInitialMonitor() {
  if (screen_capturer_sck_impl_) {
    return screen_capturer_sck_impl_->ResetToInitialMonitor();
  }
  return -1;
}

std::vector<DisplayInfo> ScreenCapturerSck::GetDisplayInfoList() {
  if (screen_capturer_sck_impl_) {
    return screen_capturer_sck_impl_->GetDisplayInfoList();
  }

  return std::vector<DisplayInfo>();
}

void ScreenCapturerSck::OnFrame() {}

void ScreenCapturerSck::CleanUp() {}

// ── Virtual display lifecycle ───────────────────────────────────────────

uint32_t ScreenCapturerSck::CountActivePhysicalDisplays() const {
  CGDirectDisplayID ids[32] = {0};
  uint32_t n = 0;
  if (CGGetActiveDisplayList(32, ids, &n) != kCGErrorSuccess) {
    return 0;
  }
  // Count displays that are NOT ours. Virtual displays created by other
  // processes cannot easily be distinguished from physical ones, but our
  // own can be excluded by id. For headless-detection purposes, an
  // approximation is acceptable: any display counts as presence.
  const uint32_t own_id = virtual_display_.display_id();
  uint32_t physical = 0;
  for (uint32_t i = 0; i < n; ++i) {
    if (own_id != 0 && ids[i] == own_id) continue;
    ++physical;
  }
  return physical;
}

void ScreenCapturerSck::MaybeProvisionVirtualDisplay() {
  if (!virtual_display_enabled_) return;
  if (virtual_display_provisioned_) return;
  if (!MacVirtualDisplay::IsRuntimeSupported()) {
    LOG_WARN(
        "macOS virtual display: CGVirtualDisplay runtime unavailable on "
        "this macOS build — feature disabled");
    return;
  }

  const uint32_t physical = CountActivePhysicalDisplays();
  if (physical > 0) {
    LOG_INFO(
        "macOS virtual display: {} physical display(s) present, skipping "
        "creation",
        physical);
    // Mark as provisioned so we don't re-check on every Start. A session
    // that starts headful stays headful from the capturer's perspective.
    virtual_display_provisioned_ = true;
    return;
  }

  // Headless — create the virtual display. The spec is whatever the host
  // configured; the provisioner handles clamping internally.
  std::string error;
  if (!virtual_display_.Acquire(virtual_display_spec_, &error)) {
    LOG_WARN("macOS virtual display: acquire failed — {}", error);
    return;
  }
  virtual_display_provisioned_ = true;
  LOG_INFO(
      "macOS virtual display: provisioned id=0x{:x} logical={}x{}@{}Hz "
      "hi_dpi={}",
      virtual_display_.display_id(), virtual_display_.spec().width,
      virtual_display_.spec().height, virtual_display_.spec().refresh_rate,
      virtual_display_.spec().hi_dpi);
}

}  // namespace crossdesk
