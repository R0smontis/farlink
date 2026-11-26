/*
 * Host-side lifecycle provisioner for the macOS virtual display.
 *
 * Mirrors the Windows VirtualDisplayProvisioner pattern, but accounts for
 * macOS 27.x platform realities (see mac_virtual_display.h for details):
 *
 *   - Teardown is unreliable on macOS 27.x. Releasing the provisioner does
 *     NOT guarantee the display leaves the system. We therefore adopt a
 *     create-once, keep-alive strategy: the provisioner creates the display
 *     the first time it is needed and intentionally does NOT destroy it
 *     even when the last session ends. The display is cheap when idle
 *     (no capture clients attached) and removing it would not actually
 *     work anyway.
 *
 *   - Reconfigure is effectively unsupported on macOS 27.x because the
 *     active display mode cannot be changed after creation. Callers must
 *     decide the resolution at the time of first creation.
 *
 *   - The provisioner is a process-lifetime singleton owner. Destruction
 *     of the provisioner instance does release the underlying ObjC object,
 *     but the practical effect on the system display depends on the OS
 *     build.
 *
 * Threading: provisioner methods are NOT thread-safe. The host capture
 * management thread is expected to be the sole caller.
 */

#ifndef CROSSDESK_MAC_VIRTUAL_DISPLAY_PROVISIONER_H_
#define CROSSDESK_MAC_VIRTUAL_DISPLAY_PROVISIONER_H_

#include <cstdint>
#include <string>

#include "mac_virtual_display.h"

namespace crossdesk {

class MacVirtualDisplayProvisioner {
 public:
  MacVirtualDisplayProvisioner() = default;
  ~MacVirtualDisplayProvisioner() = default;
  MacVirtualDisplayProvisioner(const MacVirtualDisplayProvisioner&) = delete;
  MacVirtualDisplayProvisioner& operator=(
      const MacVirtualDisplayProvisioner&) = delete;

  // Returns true iff the runtime supports CGVirtualDisplay at all. Does
  // not create anything.
  static bool RuntimeSupported() {
    return MacVirtualDisplay::IsRuntimeSupported();
  }

  // Returns true iff teardown is reliable on this macOS build. When false
  // (macOS 27.x), callers should treat the display as a session-long
  // commitment and surface appropriate UX.
  static bool TeardownReliable() {
    return MacVirtualDisplay::TeardownReliable();
  }

  // Acquire the virtual display with the given spec. If the display is
  // already active, returns true without changing anything (even if the
  // requested spec differs — reconfigure is unreliable on macOS 27.x; use
  // RequiresSpec() to detect the mismatch).
  //
  // Errors are reported via |error| when non-null.
  bool Acquire(const MacVirtualDisplaySpec& spec, std::string* error);

  // Best-effort release. On macOS 27.x this drops our reference but the
  // display will typically persist in the system until logout. Callers
  // should generally NOT call this in response to session-end events;
  // prefer to keep the display alive for the process lifetime.
  void Release();

  bool active() const { return display_.active(); }
  uint32_t display_id() const { return display_.display_id(); }
  const MacVirtualDisplaySpec& spec() const { return display_.spec(); }

  // Returns true iff the currently active display was created with a
  // different logical resolution than the given one. Useful for detecting
  // when the controller's preferred resolution no longer matches what we
  // have. The caller can use this to surface a "host restart required to
  // apply the new resolution" message.
  bool RequiresSpecMismatch(uint32_t width, uint32_t height) const;

 private:
  MacVirtualDisplay display_;
};

}  // namespace crossdesk

#endif  // CROSSDESK_MAC_VIRTUAL_DISPLAY_PROVISIONER_H_
