/*
 * Pure C++ deterministic policy for the macOS virtual display.
 *
 * Everything in this header is platform-agnostic and unit-testable: no
 * CoreGraphics, no Objective-C, no I/O. It answers three questions:
 *
 *   1. What is a valid MacVirtualDisplaySpec? (clamping, alignment)
 *   2. Given a desired logical size (e.g. controller viewport) what spec
 *      should we use? (HiDPI choice, even-dimension alignment, sizing to
 *      the supported envelope)
 *   3. How does the host-side lifecycle state machine transition? (create
 *      on demand / when headless, destroy when last session disconnects)
 *
 * The ObjC++ wrapper (mac_virtual_display.mm) consumes these decisions; the
 * smoke test exercises them end-to-end; the policy unit test exercises them
 * deterministically.
 */

#ifndef CROSSDESK_MAC_VIRTUAL_DISPLAY_POLICY_H_
#define CROSSDESK_MAC_VIRTUAL_DISPLAY_POLICY_H_

#include <algorithm>
#include <cstdint>

namespace crossdesk {
namespace mac_virtual_display_policy {

// Hard envelope supported by CGVirtualDisplay. The lower bound reflects the
// smallest size WindowServer accepts without complaint (smaller requests get
// clamped or rejected). The upper bound matches 5K Retina at 2x backing —
// large enough for any realistic controller window, small enough that the
// framebuffer allocation stays well under memory pressure on 8 GB Macs.
inline constexpr uint32_t kMinWidth = 800;
inline constexpr uint32_t kMinHeight = 600;
inline constexpr uint32_t kMaxWidth = 5120;
inline constexpr uint32_t kMaxHeight = 2880;

// H.264/H.265 hardware encoders require even dimensions in almost all
// pixel formats; more importantly several decode+display pipelines assume
// 2-byte-aligned strides. We round *down* to the nearest even value rather
// than rejecting outright, matching what the Windows usbmmidd path does
// implicitly through its fixed mode list.
inline constexpr uint32_t AlignEvenDown(uint32_t v) { return v & ~1u; }

// Conservative set of refresh rates CGVirtualDisplay has been observed to
// accept on macOS 11..27. Anything outside this set is clamped to the
// nearest entry. We intentionally exclude exotic rates (75, 90, 144 etc.)
// because WindowServer will accept the mode object but the resulting
// display does not always produce frames reliably under capture.
inline constexpr double kSupportedRefreshRates[] = {30.0, 60.0, 120.0};
inline constexpr size_t kSupportedRefreshRateCount =
    sizeof(kSupportedRefreshRates) / sizeof(kSupportedRefreshRates[0]);
inline constexpr double kDefaultRefreshRate = 60.0;

struct ClampResult {
  uint32_t width = 0;
  uint32_t height = 0;
  double refresh_rate = kDefaultRefreshRate;
  bool hi_dpi = false;
  // true iff any input field was modified — useful for logging.
  bool adjusted = false;
};

// Clamp a raw request into the supported envelope. Rules, in order:
//   1. HiDPI requests double the *pixel* cost; the logical width/height
//      fields are unchanged but the backing store may not exceed the
//      envelope either, so the maximum logical size under HiDPI is half.
//   2. Width/height are clamped to [min, max] then aligned down to even.
//      If even alignment pushes below the minimum, the minimum is used
//      (both kMinWidth and kMinHeight are already even).
//   3. Refresh rate snaps to the nearest entry in kSupportedRefreshRates.
//   4. Zero or negative input falls back to the smallest valid value.
inline ClampResult ClampSpec(uint32_t width, uint32_t height,
                             double refresh_rate, bool hi_dpi) {
  ClampResult out;
  out.hi_dpi = hi_dpi;

  const uint32_t max_w = hi_dpi ? kMaxWidth / 2 : kMaxWidth;
  const uint32_t max_h = hi_dpi ? kMaxHeight / 2 : kMaxHeight;

  uint32_t w = width == 0 ? kMinWidth : width;
  uint32_t h = height == 0 ? kMinHeight : height;
  w = std::clamp(w, kMinWidth, max_w);
  h = std::clamp(h, kMinHeight, max_h);
  w = std::max(kMinWidth, AlignEvenDown(w));
  h = std::max(kMinHeight, AlignEvenDown(h));

  double best_rate = kDefaultRefreshRate;
  double best_delta = 1e9;
  for (size_t i = 0; i < kSupportedRefreshRateCount; ++i) {
    const double delta =
        refresh_rate > kSupportedRefreshRates[i]
            ? refresh_rate - kSupportedRefreshRates[i]
            : kSupportedRefreshRates[i] - refresh_rate;
    if (delta < best_delta) {
      best_delta = delta;
      best_rate = kSupportedRefreshRates[i];
    }
  }

  out.width = w;
  out.height = h;
  out.refresh_rate = best_rate;
  out.adjusted = (w != width) || (h != height) ||
                 (best_rate != refresh_rate);
  return out;
}

// Choose a logical spec given a controller viewport. The heuristic mirrors
// what the Windows path effectively does through its fixed-mode-list policy
// (largest mode that fits inside the request): we match the requested size
// after clamping rather than picking the closest preset.
//
// HiDPI selection rule: opt-in only. We never silently pick HiDPI because
// it quadruples memory bandwidth; users that want a retina virtual display
// set the flag explicitly. If the controller is itself retina and the
// request is large enough that physical pixel density matters, the caller
// can pass prefer_hi_dpi=true.
inline ClampResult SpecForControllerViewport(uint32_t viewport_w,
                                             uint32_t viewport_h,
                                             double refresh_rate,
                                             bool prefer_hi_dpi) {
  return ClampSpec(viewport_w, viewport_h, refresh_rate, prefer_hi_dpi);
}

// Lifecycle state machine for the host-side provisioner.
//
// The Windows pattern is: when capture starts on an unattended/headless
// host, provision a virtual display; when capture stops (and there are no
// physical displays), release it. On macOS the trigger is slightly
// different because clamshell MacBooks and headless Mac minis still
// enumerate zero active displays only in specific power states. We model
// the decision as a pure function so the state machine is testable.
enum class LifecycleState {
  // No virtual display. Default.
  kIdle,
  // A create has been requested but not yet confirmed by WindowServer.
  // We do not currently use this state because creation is synchronous
  // in the Apple API; reserved for future async handoff.
  kProvisioning,
  // Display is created and active.
  kActive,
  // Display is being torn down. Also synchronous in practice.
  kReleasing,
};

enum class LifecycleEvent {
  // A remote session has started capturing this host.
  kSessionCaptureStarted,
  // A remote session has stopped capturing (or disconnected).
  kSessionCaptureStopped,
  // The host process is about to exit.
  kProcessExiting,
};

struct LifecycleInput {
  LifecycleState state = LifecycleState::kIdle;
  LifecycleEvent event = LifecycleEvent::kSessionCaptureStarted;
  // Number of currently active remote capture sessions, after the event.
  uint32_t active_sessions = 0;
  // Number of physical displays currently enumerated, after the event.
  uint32_t physical_displays = 0;
  // true iff the controller explicitly requested a virtual display (toggle
  // in stream/host settings). When false, we only provision on headless.
  bool explicit_request = false;
};

enum class LifecycleAction {
  kNone,
  kCreate,
  kDestroy,
};

struct LifecycleOutput {
  LifecycleState next_state = LifecycleState::kIdle;
  LifecycleAction action = LifecycleAction::kNone;
};

// Pure transition function. Invariants:
//   - We never destroy a display while active_sessions > 0.
//   - We never create a second display when one is already active.
//   - explicit_request=true overrides the physical-displays check.
//   - kProcessExiting always transitions to kIdle with kDestroy if active.
inline LifecycleOutput Step(const LifecycleInput& in) {
  LifecycleOutput out;
  out.next_state = in.state;

  const bool need_display =
      in.explicit_request || (in.physical_displays == 0);

  switch (in.event) {
    case LifecycleEvent::kSessionCaptureStarted:
      if (in.state == LifecycleState::kIdle && need_display) {
        out.next_state = LifecycleState::kActive;
        out.action = LifecycleAction::kCreate;
      }
      break;
    case LifecycleEvent::kSessionCaptureStopped:
      if (in.state == LifecycleState::kActive && in.active_sessions == 0) {
        out.next_state = LifecycleState::kIdle;
        out.action = LifecycleAction::kDestroy;
      }
      break;
    case LifecycleEvent::kProcessExiting:
      if (in.state == LifecycleState::kActive) {
        out.next_state = LifecycleState::kIdle;
        out.action = LifecycleAction::kDestroy;
      } else {
        out.next_state = LifecycleState::kIdle;
        out.action = LifecycleAction::kNone;
      }
      break;
  }
  return out;
}

}  // namespace mac_virtual_display_policy
}  // namespace crossdesk

#endif  // CROSSDESK_MAC_VIRTUAL_DISPLAY_POLICY_H_
