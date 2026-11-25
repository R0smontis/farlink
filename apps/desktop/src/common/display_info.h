/*
 * @Author: DI JUNKUN
 * @Date: 2025-05-15
 * Copyright (c) 2025 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _DISPLAY_INFO_H_
#define _DISPLAY_INFO_H_

#include <string>

namespace crossdesk {

class DisplayInfo {
 public:
  DisplayInfo(std::string name, int left, int top, int right, int bottom)
      : name(name), left(left), top(top), right(right), bottom(bottom) {
    width = right - left;
    height = bottom - top;
  }
  DisplayInfo(void* handle, std::string name, bool is_primary, int left,
              int top, int right, int bottom)
      : handle(handle),
        name(name),
        is_primary(is_primary),
        left(left),
        top(top),
        right(right),
        bottom(bottom) {
    width = right - left;
    height = bottom - top;
  }
  ~DisplayInfo() {}

  void* handle = nullptr;
  std::string name = "";
  bool is_primary = false;
  int left = 0;
  int top = 0;
  int right = 0;
  int bottom = 0;
  int width = 0;
  int height = 0;
  // Physical pixel dimensions of the captured frame for this display, when
  // the platform backend knows them (macOS: backing-store pixels, which
  // differ from point geometry on Retina). Zero means unknown; consumers
  // converting stream pixels into injection units must fall back to a 1:1
  // ratio in that case.
  int pixel_width = 0;
  int pixel_height = 0;
};
}  // namespace crossdesk
#endif