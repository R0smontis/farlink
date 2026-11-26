/*
 * @Author: DI JUNKUN
 * @Date: 2023-12-14
 * Copyright (c) 2023 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _MOUSE_CONTROLLER_H_
#define _MOUSE_CONTROLLER_H_

#include <vector>
#include <atomic>
#include <mutex>
#include <chrono>

#include <remote_action.h>

#include "device_controller.h"
#include "display_info.h"

namespace crossdesk {

class PlatformMouseController final : public MouseController {
 public:
  PlatformMouseController();
  virtual ~PlatformMouseController();

 public:
  virtual int Init(std::vector<DisplayInfo> display_info_list);
  virtual int Destroy();
  virtual int SendMouseCommand(RemoteAction remote_action, int display_index);
  void UpdateDisplayInfoList(
      const std::vector<DisplayInfo>& display_info_list) override;
  unsigned ReleasePressedButtons(unsigned mask = 7) override;

 private:
  std::vector<DisplayInfo> display_info_list_;
  std::mutex display_info_mutex_;
  std::atomic<unsigned> pressed_buttons_{0};

  // move_relative injection state, mirroring the macOS host semantics
  // (platform/macos/input/mouse/mouse_controller.cpp:139-202): wire dx/dy
  // are fractions of the target display's width/height, converted to
  // physical pixels here. The anchor tracks the logical cursor position in
  // double precision across a burst so fractional motion is not lost;
  // Windows relative SendInput moves the cursor by whole pixels, so the
  // sub-pixel remainder stays in the carry fields and is resynced against
  // the physical cursor whenever injection has been idle long enough for
  // local input or absolute positioning to have moved it.
  static constexpr std::chrono::milliseconds kRelativeAnchorMaxAge{500};
  bool relative_anchor_valid_ = false;
  double relative_anchor_x_ = 0;
  double relative_anchor_y_ = 0;
  double relative_carry_x_ = 0;
  double relative_carry_y_ = 0;
  std::chrono::steady_clock::time_point relative_anchor_time_{};
};
}  // namespace crossdesk
#endif
