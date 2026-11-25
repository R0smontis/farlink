/*
 * @Author: DI JUNKUN
 * @Date: 2023-12-14
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _MOUSE_CONTROLLER_H_
#define _MOUSE_CONTROLLER_H_

#include <chrono>
#include <vector>

#include <remote_action.h>

#include "device_controller.h"
#include "display_info.h"
#include "relative_mouse_injection.h"

namespace crossdesk {

class PlatformMouseController final : public MouseController {
 public:
  PlatformMouseController();
  ~PlatformMouseController() override;

 public:
  int Init(std::vector<DisplayInfo> display_info_list) override;
  int Destroy() override;
  int SendMouseCommand(RemoteAction remote_action, int display_index) override;
  unsigned ReleasePressedButtons(unsigned mask = 7) override;
  void UpdateDisplayInfoList(
      const std::vector<DisplayInfo>& display_info_list) override;

 private:
  struct ClickTracker {
    bool has_last_down = false;
    std::chrono::steady_clock::time_point last_down_time{};
    int last_down_x = 0;
    int last_down_y = 0;
    int click_state = 0;
    int active_click_state = 1;
  };

  int BeginClick(ClickTracker& tracker, int x, int y);
  int EndClick(ClickTracker& tracker, int x, int y);

  std::vector<DisplayInfo> display_info_list_;
  bool left_dragging_ = false;
  bool right_dragging_ = false;
  unsigned pressed_buttons_ = 0;
  ClickTracker left_click_tracker_;
  ClickTracker right_click_tracker_;
  ClickTracker middle_click_tracker_;
  // Sub-pixel carry for move_relative; the two coordinate systems never mix
  // because relative mode ends on the next absolute move.
  RelativeMouseMotion relative_motion_;
  // True while the session's positioning is driven by relative deltas: the
  // controller cannot know where the cursor is, so clicks and the wheel are
  // anchored at the live position instead of the stale normalized target.
  bool relative_mode_active_ = false;
  // Authoritative cursor position during a relative burst. Injected cursor
  // moves do not apply atomically, so CGEventGetLocation read back inside a
  // burst lags the injected positions and would swallow motion. The anchor
  // resyncs to the physical cursor after this much injection idle time.
  static constexpr std::chrono::milliseconds kRelativeAnchorMaxAge{500};
  bool relative_anchor_valid_ = false;
  double relative_anchor_x_ = 0;
  double relative_anchor_y_ = 0;
  std::chrono::steady_clock::time_point relative_anchor_time_{};
};
}  // namespace crossdesk
#endif
