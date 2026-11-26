/*
 * macOS virtual display (CGVirtualDisplay) RAII wrapper.
 *
 * CGVirtualDisplay is an undocumented Apple-private Objective-C class inside
 * CoreGraphics, present since macOS 11. There is no public header; the class
 * is resolved at runtime via NSClassFromString + typed forward declarations,
 * so compilation never depends on private symbols and absence at runtime is
 * a clean capability failure rather than a link error.
 *
 * ═════════════════════════════════════════════════════════════════════════
 * CRITICAL PLATFORM CAVEAT — macOS 27.x teardown defect (verified 27.2.1)
 * ═════════════════════════════════════════════════════════════════════════
 *
 * On macOS 27.2.1 (and possibly earlier 27.x), destroying a CGVirtualDisplay
 * object does NOT remove the corresponding display from the system:
 *
 *   - Releasing all strong references and draining the runloop leaves the
 *     display visible in CGGetActiveDisplayList indefinitely.
 *   - Even clean process exit does not release the display. This contradicts
 *     pre-Tahoe behavior documented in the community (DeskPad, BetterDisplay,
 *     node-mac-virtual-display), where process exit teardown worked.
 *   - WindowServer logs "enter VirtualDisplay(CGXVirtualDisplayDestroy) ...
 *     exit ... not found" — the destroy path runs but the bookkeeping is
 *     broken.
 *   - The private CGXVirtualDisplayDestroy C function exists in CoreDisplay
 *     but is NOT exported, so it cannot be invoked.
 *   - Once (vendorID, productID, serialNum) is registered, any attempt to
 *     create another display with the same triple returns nil from
 *     initWithDescriptor:. The leak therefore blocks subsequent creations.
 *
 * Consequence: on affected systems a virtual display effectively persists
 * for the user's GUI session. Only logout/login or a WindowServer restart
 * clears it. This is a system-level defect outside our control.
 *
 * ENGINEERING RESPONSE:
 *   1. The wrapper exposes the defect honestly: Destroy() is best-effort
 *      and reports whether teardown appears to have taken effect.
 *   2. We generate a unique (vendorID, productID, serialNum) triple per
 *      process, so a leak does not block future processes or restarts.
 *   3. Reconfigure() does NOT use destroy+create. Instead it reuses the
 *      same display object via applySettings:. This is essential on
 *      affected systems because we only get one create per triple.
 *   4. Host integration must treat the display as a session-long singleton:
 *      create once on first need, keep alive for the process lifetime, and
 *      never destroy mid-process. The display is cheap when idle.
 *   5. IsRuntimeSupported() reports API availability. A separate helper
 *      (TeardownReliable()) reports whether we believe teardown works on
 *      the running OS build; until Apple fixes the defect this returns
 *      false on macOS 27.x and callers should surface appropriate UX.
 *
 * Thread affinity: all public methods must be called from the same thread
 * that created the object (or fully serialized against it). Internally we
 * pin WindowServer communication to a dedicated serial dispatch queue;
 * callers do not need a CFRunLoop on their thread, though a running main
 * runloop helps asynchronous plug-in propagate faster.
 */

#ifndef CROSSDESK_MAC_VIRTUAL_DISPLAY_H_
#define CROSSDESK_MAC_VIRTUAL_DISPLAY_H_

#include <cstdint>
#include <string>

namespace crossdesk {

struct MacVirtualDisplaySpec {
  // Logical (point) dimensions. When hi_dpi is true, the physical backing
  // store is 2x the logical size.
  uint32_t width = 1920;
  uint32_t height = 1080;
  // Refresh rate in Hz. 60 is universally supported; 30/120 are commonly
  // accepted. Unsupported values are clamped by policy (see
  // mac_virtual_display_policy.h).
  double refresh_rate = 60.0;
  // true -> HiDPI 2x backing (Retina-style). Framebuffer cost is 4x the
  // logical resolution in bytes.
  bool hi_dpi = false;
  // Display name shown in System Settings > Displays. Keep short.
  std::string name = "Farlink Virtual Display";
};

// RAII handle for a single CGVirtualDisplay. Not copyable, not movable.
//
// PROCESS-LIFETIME SINGLETON PATTERN: because teardown is unreliable on
// macOS 27.x, callers should treat this object as a session-long singleton.
// Create at most one instance per process and keep it alive as long as
// capture may be needed. Destroy() is safe to call but may not actually
// remove the display from the system on affected OS builds.
class MacVirtualDisplay {
 public:
  MacVirtualDisplay() = default;
  ~MacVirtualDisplay();
  MacVirtualDisplay(const MacVirtualDisplay&) = delete;
  MacVirtualDisplay& operator=(const MacVirtualDisplay&) = delete;

  // Runtime capability check: returns true iff all four CGVirtualDisplay
  // classes exist and respond to the selectors we depend on. Does not
  // create anything. Cheap; safe to call any number of times.
  static bool IsRuntimeSupported();

  // Returns true if we believe teardown (Destroy/process exit) actually
  // removes the display from the system on this OS build. As of macOS
  // 27.2.1 this is FALSE — teardown is broken at the WindowServer level.
  // We detect the running build and report honestly. Callers should use
  // this to gate UX: on systems where teardown is broken, the virtual
  // display toggle should be opt-in and the user informed that the display
  // will persist until logout.
  static bool TeardownReliable();

  // Create the virtual display. Returns true on success. On failure,
  // returns false and (if non-null) fills *error with a short English
  // diagnostic.
  //
  // Successful creation means the CGVirtualDisplay object was allocated
  // and applySettings: was accepted by WindowServer. The displayID is
  // valid immediately; actual enumeration in CGGetActiveDisplayList is
  // asynchronous and typically settles within ~100 ms on Apple silicon.
  bool Create(const MacVirtualDisplaySpec& spec, std::string* error = nullptr);

  // Apply a new spec to the existing display. Unlike Destroy+Create, this
  // does not risk leaking a display on macOS 27.x. Internally it calls
  // applySettings: with the new mode list. The displayID does not change.
  //
  // If no display is currently active, this is equivalent to Create().
  //
  // Note: community reports suggest that changing the mode list after
  // creation can sometimes leave the display in an inconsistent state on
  // some macOS versions. We have not independently reproduced this on
  // macOS 27.2.1. Callers should treat Reconfigure() as a best-effort
  // operation and verify the resulting dimensions match expectations by
  // querying CGDisplayPixelsWide/High.
  bool Reconfigure(const MacVirtualDisplaySpec& spec, std::string* error = nullptr);

  // Best-effort teardown. On affected systems (see TeardownReliable) this
  // may not actually remove the display from the system. The object is
  // always reusable after Destroy() returns: active() reports false and
  // Create() can be called again, but on affected systems the previously
  // created display may still be visible in the system.
  //
  // Idempotent. Safe to call multiple times and from the destructor.
  void Destroy();

  bool active() const { return display_id_ != 0; }
  // CGDirectDisplayID of the virtual display, or 0 if inactive. The id is
  // a per-session value assigned by WindowServer; it is not stable across
  // reboots or across Destroy/Create cycles.
  uint32_t display_id() const { return display_id_; }

  // Last spec passed to a successful Create/Reconfigure. Only meaningful
  // when active() is true.
  const MacVirtualDisplaySpec& spec() const { return spec_; }

 private:
  // Pimpl-style opaque state so the header does not leak Objective-C types.
  struct Impl;
  Impl* impl_ = nullptr;
  uint32_t display_id_ = 0;
  MacVirtualDisplaySpec spec_;
};

}  // namespace crossdesk

#endif  // CROSSDESK_MAC_VIRTUAL_DISPLAY_H_
