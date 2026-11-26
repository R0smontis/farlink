/*
 * Host-side lifecycle provisioner — implementation.
 *
 * See the header for the contract. This translation unit is thin: most of
 * the heavy lifting lives in MacVirtualDisplay. The provisioner layer adds
 * the lifecycle policy documented in mac_virtual_display_policy.h.
 */

#import <CoreGraphics/CoreGraphics.h>

#include "mac_virtual_display_provisioner.h"

#include "mac_virtual_display_policy.h"
#include "rd_log.h"

namespace crossdesk {

bool MacVirtualDisplayProvisioner::Acquire(const MacVirtualDisplaySpec& spec,
                                           std::string* error) {
  if (display_.active()) {
    // Already have a display. Reconfigure is unreliable on macOS 27.x; we
    // intentionally do not attempt it here. Callers that need a different
    // resolution must restart the host process.
    if (spec.width != display_.spec().width ||
        spec.height != display_.spec().height) {
      LOG_WARN(
          "MacVirtualDisplayProvisioner: requested {}x{} but active display "
          "is {}x{}; restart the host process to apply a new resolution",
          spec.width, spec.height, display_.spec().width,
          display_.spec().height);
    }
    return true;
  }

  if (!MacVirtualDisplay::IsRuntimeSupported()) {
    if (error)
      *error = "CGVirtualDisplay is not available on this macOS build";
    return false;
  }

  if (!MacVirtualDisplay::TeardownReliable()) {
    LOG_WARN(
        "MacVirtualDisplayProvisioner: macOS 27.x teardown defect active — "
        "the virtual display will persist until logout/login");
  }

  return display_.Create(spec, error);
}

void MacVirtualDisplayProvisioner::Release() {
  if (!display_.active()) return;
  // We honor the call but acknowledge the platform reality. The underlying
  // MacVirtualDisplay::Destroy is best-effort and logs appropriately.
  display_.Destroy();
}

bool MacVirtualDisplayProvisioner::RequiresSpecMismatch(uint32_t width,
                                                        uint32_t height) const {
  if (!display_.active()) return false;
  return display_.spec().width != width || display_.spec().height != height;
}

}  // namespace crossdesk
