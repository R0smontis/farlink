/*
 * @Author: Farlink contributors
 * @Date: 2026-11-25
 * Copyright (c) 2026 by Farlink contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef _GAME_MODE_SCALE_H_
#define _GAME_MODE_SCALE_H_

#include <algorithm>
#include <cmath>

namespace crossdesk {

// Pure math behind the game-mode capture scale: the factor converting a
// captured device-point delta into the wire unit — displacement as a
// fraction of the controlled display's width/height. The controller
// normalises by the rendered video rect in points (the letterboxed drawn
// size per axis, so non-uniform window shapes cannot skew axes) and folds
// in the mouse sensitivity factor. Kept free of UI types so the geometry
// cases are unit-testable.
struct GameModeScale {
  double scale_x = 0;
  double scale_y = 0;
};

// area_width/area_height: the video element's logical size in points.
// video_width/video_height: the frame's pixel dimensions — they only shape
// the letterbox, never the unit. sensitivity_percent: 100 = 1:1.
// Returns false when the inputs cannot produce a finite positive scale;
// the caller keeps whatever scale it had rather than exiting capture.
inline bool ComputeGameModeScale(double area_width, double area_height,
                                 int video_width, int video_height,
                                 int sensitivity_percent,
                                 GameModeScale& out) {
  if (!std::isfinite(area_width) || !std::isfinite(area_height) ||
      !(area_width > 0 && area_height > 0) || video_width <= 0 ||
      video_height <= 0 || sensitivity_percent <= 0) {
    return false;
  }
  const double fit = std::min(area_width / video_width,
                              area_height / video_height);
  const double drawn_width = video_width * fit;
  const double drawn_height = video_height * fit;
  if (!(drawn_width > 0) || !(drawn_height > 0)) return false;
  const double sensitivity = sensitivity_percent / 100.0;
  out.scale_x = sensitivity / drawn_width;
  out.scale_y = sensitivity / drawn_height;
  return std::isfinite(out.scale_x) && std::isfinite(out.scale_y) &&
         out.scale_x > 0 && out.scale_y > 0;
}

}  // namespace crossdesk

#endif
