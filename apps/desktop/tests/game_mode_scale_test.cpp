/*
 * @Author: Farlink contributors
 * @Date: 2026-11-25
 * Copyright (c) 2026 by Farlink contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "features/input/game_mode_scale.h"

#include <cmath>
#include <iostream>

namespace {

bool Expect(bool condition, const char* message) {
  if (condition) return true;
  std::cerr << message << '\n';
  return false;
}

bool Near(double a, double b, double eps = 1e-12) {
  return std::fabs(a - b) <= eps;
}

}  // namespace

int main() {
  bool ok = true;

  // Wider-than-video area: pillarboxing — the height is the constraint, so
  // the drawn rect is narrower than the area horizontally. x normalises by
  // the drawn width, not the element width, or the axes would skew.
  {
    crossdesk::GameModeScale scale;
    // 1000x1000 area, video 16:9 (1920x1080). fit = min(1000/1920,
    // 1000/1080) = 1000/1920; drawn = 1000 x 562.5.
    ok &= Expect(crossdesk::ComputeGameModeScale(1000, 1000, 1920, 1080, 100,
                                                 scale),
                 "wider-than case must compute");
    ok &= Expect(Near(scale.scale_x, 1.0 / 1000.0),
                 "wider-than: x scale must use the drawn width 1000");
    ok &= Expect(Near(scale.scale_y, 1.0 / 562.5),
                 "wider-than: y scale must use the drawn height 562.5");
    ok &= Expect(scale.scale_x != scale.scale_y,
                 "wider-than: axes must differ — aspect is per-axis");
  }

  // Taller-than-video area: letterboxing — the width is the constraint.
  {
    crossdesk::GameModeScale scale;
    // 1000x1000 area, video 9:16-ish portrait (1080x1920). fit = min(
    // 1000/1080, 1000/1920) = 1000/1920; drawn = 562.5 x 1000.
    ok &= Expect(crossdesk::ComputeGameModeScale(1000, 1000, 1080, 1920, 100,
                                                 scale),
                 "taller-than case must compute");
    ok &= Expect(Near(scale.scale_x, 1.0 / 562.5),
                 "taller-than: x scale must use the drawn width 562.5");
    ok &= Expect(Near(scale.scale_y, 1.0 / 1000.0),
                 "taller-than: y scale must use the drawn height 1000");
  }

  // Equal aspect: the drawn rect fills the area exactly and both axes
  // normalise by the matching element extent — uniform scale.
  {
    crossdesk::GameModeScale scale;
    ok &= Expect(crossdesk::ComputeGameModeScale(1600, 900, 1920, 1080, 100,
                                                 scale),
                 "equal-aspect case must compute");
    ok &= Expect(Near(scale.scale_x, 1.0 / 1600.0),
                 "equal-aspect: x scale must use the full area width");
    ok &= Expect(Near(scale.scale_y, 1.0 / 900.0),
                 "equal-aspect: y scale must use the full area height");
  }

  // Sensitivity scales both axes linearly.
  {
    crossdesk::GameModeScale full, half;
    crossdesk::ComputeGameModeScale(1600, 900, 1920, 1080, 100, full);
    crossdesk::ComputeGameModeScale(1600, 900, 1920, 1080, 50, half);
    ok &= Expect(Near(half.scale_x, full.scale_x * 0.5),
                 "half sensitivity must halve the x scale");
    ok &= Expect(Near(half.scale_y, full.scale_y * 0.5),
                 "half sensitivity must halve the y scale");
  }

  // Resolution independence: the same display encoded at different stream
  // resolutions but the SAME aspect produces the same scale — this is the
  // entire point of the normalised unit.
  {
    crossdesk::GameModeScale hd, uhd;
    crossdesk::ComputeGameModeScale(1600, 900, 1920, 1080, 100, hd);
    crossdesk::ComputeGameModeScale(1600, 900, 3840, 2160, 100, uhd);
    ok &= Expect(Near(hd.scale_x, uhd.scale_x) &&
                     Near(hd.scale_y, uhd.scale_y),
                 "same aspect at different resolutions must scale identically");
  }

  // Failure cases: zero or negative inputs must fail so the caller keeps
  // the previous scale rather than exiting or injecting garbage.
  {
    crossdesk::GameModeScale scale;
    ok &= Expect(!crossdesk::ComputeGameModeScale(0, 900, 1920, 1080, 100,
                                                  scale),
                 "zero area width must fail");
    ok &= Expect(!crossdesk::ComputeGameModeScale(1600, 900, 0, 1080, 100,
                                                  scale),
                 "zero video width must fail");
    ok &= Expect(!crossdesk::ComputeGameModeScale(1600, 900, 1920, 1080, 0,
                                                  scale),
                 "zero sensitivity must fail");
    ok &= Expect(!crossdesk::ComputeGameModeScale(NAN, 900, 1920, 1080, 100,
                                                  scale),
                 "non-finite area must fail");
  }

  return ok ? 0 : 1;
}
