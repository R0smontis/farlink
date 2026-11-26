/*
 * @Author: Farlink contributors
 * @Date: 2026-11-27
 * Copyright (c) 2026 by Farlink contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

// Fixed test identity for targets that link diagnostic_export.cpp without
// the full application build-metadata translation unit.
#include "build_identity.h"

namespace crossdesk {

const BuildIdentity& GetBuildIdentity() {
  static constexpr BuildIdentity identity{"test-rev", "clean", "test-mr",
                                          "clean"};
  return identity;
}

}  // namespace crossdesk
