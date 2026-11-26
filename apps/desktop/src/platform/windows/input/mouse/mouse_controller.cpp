#include "mouse_controller.h"

#include <Windows.h>
#include <remote_action.h>

#include <algorithm>
#include <cmath>

#include "rd_log.h"
#include "windows_input_injector.h"
#include "windows_input_marker.h"
#include "windows_thread_dpi.h"

namespace crossdesk {

PlatformMouseController::PlatformMouseController() {}

PlatformMouseController::~PlatformMouseController() { Destroy(); }

int PlatformMouseController::Init(std::vector<DisplayInfo> display_info_list) {
  std::lock_guard<std::mutex> lock(display_info_mutex_);
  display_info_list_ = display_info_list;

  return 0;
}

void PlatformMouseController::UpdateDisplayInfoList(
    const std::vector<DisplayInfo>& display_info_list) {
  if (display_info_list.empty()) return;
  std::lock_guard<std::mutex> lock(display_info_mutex_);
  if (std::equal(display_info_list.begin(), display_info_list.end(),
                 display_info_list_.begin(), display_info_list_.end(),
                 [](const auto& a, const auto& b) {
                   return a.handle == b.handle && a.left == b.left &&
                          a.top == b.top && a.width == b.width &&
                          a.height == b.height;
                 })) return;
  display_info_list_ = display_info_list;
  LOG_INFO("Windows mouse display geometry updated: displays={}",
           display_info_list_.size());
}

int PlatformMouseController::Destroy() {
  return ReleasePressedButtons() == 7 ? 0 : -1;
}

unsigned PlatformMouseController::ReleasePressedButtons(unsigned mask) {
  mask &= 7;
  const unsigned pressed = pressed_buttons_.load() & mask;
  if (!pressed) return mask;
  INPUT input{};
  input.type = INPUT_MOUSE;
  input.mi.dwExtraInfo = kInjectedMouseInputMarker;
  if (pressed & 1) input.mi.dwFlags |= MOUSEEVENTF_LEFTUP;
  if (pressed & 2) input.mi.dwFlags |= MOUSEEVENTF_RIGHTUP;
  if (pressed & 4) input.mi.dwFlags |= MOUSEEVENTF_MIDDLEUP;
  if (SendInputOnUserDesktop(input) != 1) {
    LOG_WARN("Release remote mouse buttons failed, error={}", GetLastError());
    return mask & ~pressed;
  }
  pressed_buttons_.fetch_and(~pressed);
  return mask;
}

int PlatformMouseController::SendMouseCommand(RemoteAction remote_action,
                                              int display_index) {
  std::unique_lock<std::mutex> geometry_lock(display_info_mutex_);
  if (display_index < 0 ||
      display_index >= static_cast<int>(display_info_list_.size()) ||
      display_info_list_[display_index].width <= 0 ||
      display_info_list_[display_index].height <= 0) {
    LOG_WARN("Mouse command skipped, invalid display_index={}, displays={}",
             display_index, display_info_list_.size());
    return -1;
  }

  const auto display = display_info_list_[display_index];
  geometry_lock.unlock();

  INPUT ip = {0};
  ScopedWindowsPhysicalCoordinates physical_coordinates;

  if (remote_action.type != ControlType::mouse) return 0;
  if (!std::isfinite(remote_action.m.x) || !std::isfinite(remote_action.m.y))
    return -1;

  if (remote_action.m.flag == MouseFlag::move_relative) {
    // Wire dx/dy are fractions of this display's width/height (the same
    // resolution-independent unit the absolute x/y uses, remote_action.h:66-74)
    // — identical to what the macOS host multiplies by its display size in
    // points (platform/macos/input/mouse/mouse_controller.cpp:169-176). Here
    // the display size is physical pixels because SendInput relative moves
    // are pixel units. Nothing depends on the encoded stream's resolution,
    // so mid-session resolution adaptation cannot change sensitivity.
    if (!std::isfinite(remote_action.m.dx) || !std::isfinite(remote_action.m.dy))
      return -1;
    // Per-event clamp identical to the macOS accumulator's
    // (relative_mouse_injection.h:kMaxDeltaPerEvent), applied before the
    // pixel conversion so sensitivity to display size matches exactly.
    constexpr double kMaxDeltaPerEvent = 32767.0;
    const double dx_px = std::clamp(
        static_cast<double>(remote_action.m.dx) * display.width,
        -kMaxDeltaPerEvent, kMaxDeltaPerEvent);
    const double dy_px = std::clamp(
        static_cast<double>(remote_action.m.dy) * display.height,
        -kMaxDeltaPerEvent, kMaxDeltaPerEvent);
    // Injected relative moves do not report their resulting position, so
    // the tracked anchor is authoritative within a burst and resyncs to the
    // physical cursor once injection has been idle long enough for local
    // input devices to have moved it — the macOS anchor rule
    // (mouse_controller.cpp:143-161). The sub-pixel remainder carries
    // losslessly in double across events: rounding applies per-event to the
    // residual, never to the sum, so N fractional moves cannot drift.
    double base_x = 0, base_y = 0;
    const auto now = std::chrono::steady_clock::now();
    if (relative_anchor_valid_ &&
        now - relative_anchor_time_ < kRelativeAnchorMaxAge) {
      base_x = relative_anchor_x_;
      base_y = relative_anchor_y_;
    } else {
      POINT cursor{};
      if (!GetCursorPos(&cursor)) return -1;
      base_x = cursor.x;
      base_y = cursor.y;
      // A fresh anchor must not carry rounding debt from a different frame.
      relative_carry_x_ = 0;
      relative_carry_y_ = 0;
    }
    relative_carry_x_ += dx_px;
    relative_carry_y_ += dy_px;
    const auto move_x = static_cast<LONG>(std::lround(relative_carry_x_));
    const auto move_y = static_cast<LONG>(std::lround(relative_carry_y_));
    relative_carry_x_ -= static_cast<double>(move_x);
    relative_carry_y_ -= static_cast<double>(move_y);
    if (move_x == 0 && move_y == 0) {
      relative_anchor_x_ = base_x;
      relative_anchor_y_ = base_y;
      relative_anchor_time_ = now;
      relative_anchor_valid_ = true;
      return 0;
    }
    ip.type = INPUT_MOUSE;
    ip.mi.dx = move_x;
    ip.mi.dy = move_y;
    // MOUSEEVENTF_MOVE without MOUSEEVENTF_ABSOLUTE: dx/dy are relative
    // pixel units, no virtual-desk normalization. Games consuming raw input
    // read the same relative motion through the HID path; the desktop
    // cursor moves by the same amount and is clamped by Windows itself at
    // the virtual screen edge, exactly like a physical mouse.
    ip.mi.dwFlags = MOUSEEVENTF_MOVE;
    ip.mi.dwExtraInfo = kInjectedMouseInputMarker;
    const UINT rel_sent = SendInputOnUserDesktop(ip);
    if (rel_sent != 1) {
      const DWORD error = GetLastError();
      LOG_WARN("SendInput failed for relative mouse dx={}, dy={}, err={}",
               move_x, move_y, error);
      SetLastError(error);
      return -1;
    }
    relative_anchor_x_ = base_x + static_cast<double>(move_x);
    relative_anchor_y_ = base_y + static_cast<double>(move_y);
    relative_anchor_time_ = now;
    relative_anchor_valid_ = true;
    return 0;
  }
  // An absolute move ends relative positioning; clicks and the wheel keep
  // whatever mode the most recent move established. Only the absolute move
  // invalidates the tracked anchor: it repositions through the physical
  // cursor, so the next relative burst must resync from it — same rule as
  // the macOS host (mouse_controller.cpp:194-203).
  if (remote_action.m.flag == MouseFlag::move) {
    relative_anchor_valid_ = false;
  }

  ip.type = INPUT_MOUSE;
  ip.mi.dx = (LONG)(std::clamp(remote_action.m.x, 0.0f, 1.0f) *
                    (display.width - 1)) + display.left;
  ip.mi.dy = (LONG)(std::clamp(remote_action.m.y, 0.0f, 1.0f) *
                    (display.height - 1)) + display.top;

  switch (remote_action.m.flag) {
    case MouseFlag::left_down:
      ip.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
      break;
    case MouseFlag::left_up:
      ip.mi.dwFlags = MOUSEEVENTF_LEFTUP;
      break;
    case MouseFlag::right_down:
      ip.mi.dwFlags = MOUSEEVENTF_RIGHTDOWN;
      break;
    case MouseFlag::right_up:
      ip.mi.dwFlags = MOUSEEVENTF_RIGHTUP;
      break;
    case MouseFlag::middle_down:
      ip.mi.dwFlags = MOUSEEVENTF_MIDDLEDOWN;
      break;
    case MouseFlag::middle_up:
      ip.mi.dwFlags = MOUSEEVENTF_MIDDLEUP;
      break;
    case MouseFlag::wheel_vertical:
      ip.mi.dwFlags = MOUSEEVENTF_WHEEL;
      ip.mi.mouseData = remote_action.m.s * 120;
      break;
    case MouseFlag::wheel_horizontal:
      ip.mi.dwFlags = MOUSEEVENTF_HWHEEL;
      ip.mi.mouseData = remote_action.m.s * 120;
      break;
    default:
      break;
  }

  const int virtual_left = GetSystemMetrics(SM_XVIRTUALSCREEN);
  const int virtual_top = GetSystemMetrics(SM_YVIRTUALSCREEN);
  const int virtual_width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
  const int virtual_height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
  if (virtual_width <= 0 || virtual_height <= 0) {
    LOG_WARN("Remote mouse skipped: invalid virtual desktop geometry");
    return -1;
  }
  // Pixel centres in the virtual desktop, including negative monitor origins.
  // SetCursorPos cannot carry dwExtraInfo through the privacy mouse hook.
  const int virtual_width_range = (std::max)(1, virtual_width - 1);
  const int virtual_height_range = (std::max)(1, virtual_height - 1);
  ip.mi.dx = static_cast<LONG>(std::clamp<int64_t>(
      (static_cast<int64_t>(ip.mi.dx - virtual_left) * 65535 +
       virtual_width_range / 2) /
          virtual_width_range,
      0, 65535));
  ip.mi.dy = static_cast<LONG>(std::clamp<int64_t>(
      (static_cast<int64_t>(ip.mi.dy - virtual_top) * 65535 +
       virtual_height_range / 2) /
          virtual_height_range,
      0, 65535));
  ip.mi.dwFlags |=
      MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
  ip.mi.dwExtraInfo = kInjectedMouseInputMarker;
  const UINT sent = SendInputOnUserDesktop(ip);
  if (sent != 1) {
    const DWORD error = GetLastError();
    LOG_WARN("SendInput failed for mouse x={}, y={}, wheel={}, flag={}, err={}",
             ip.mi.dx, ip.mi.dy, remote_action.m.s,
             static_cast<int>(remote_action.m.flag), error);
    SetLastError(error);
    return -1;
  }
  if (ip.mi.dwFlags & MOUSEEVENTF_LEFTDOWN) pressed_buttons_.fetch_or(1);
  if (ip.mi.dwFlags & MOUSEEVENTF_RIGHTDOWN) pressed_buttons_.fetch_or(2);
  if (ip.mi.dwFlags & MOUSEEVENTF_MIDDLEDOWN) pressed_buttons_.fetch_or(4);
  if (ip.mi.dwFlags & MOUSEEVENTF_LEFTUP) pressed_buttons_.fetch_and(~1u);
  if (ip.mi.dwFlags & MOUSEEVENTF_RIGHTUP) pressed_buttons_.fetch_and(~2u);
  if (ip.mi.dwFlags & MOUSEEVENTF_MIDDLEUP) pressed_buttons_.fetch_and(~4u);

  return 0;
}
}  // namespace crossdesk
