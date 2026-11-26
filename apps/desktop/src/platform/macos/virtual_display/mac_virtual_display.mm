/*
 * macOS CGVirtualDisplay RAII wrapper — implementation.
 *
 * Compilation strategy: we forward-declare the shape of the private
 * classes as @interfaces with the same name and link ONLY against public
 * CoreGraphics/Foundation. The classes are looked up at runtime via
 * NSClassFromString. If Apple ever removes them, IsRuntimeSupported()
 * returns false and every other entry point fails cleanly — no crash, no
 * link error.
 *
 * Memory management: compiled with -fobjc-arc. ARC needs compile-time
 * method signatures to insert correct retain/release calls, which is why
 * we must NOT use raw objc_msgSend for object-returning methods. Direct
 * method calls on typed id variables are required.
 *
 * Reference for the property list (matches the runtime introspection on
 * macOS 27.2.1):
 *   CGVirtualDisplay: displayID, name, vendorID, productID, serialNumber,
 *     serialNum, sizeInMillimeters, maxPixelsWide, maxPixelsHigh,
 *     redPrimary, greenPrimary, bluePrimary, whitePoint, queue,
 *     terminationHandler, hiDPI, rotation, modes, applySettings:,
 *     initWithDescriptor:
 */

#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

#include "mac_virtual_display.h"
#include "mac_virtual_display_policy.h"

// Logging strategy: production builds link the real rd_log (spdlog-backed).
// The standalone smoke test compiles this file without access to spdlog;
// when CROSSDESK_MAC_VIRTUAL_DISPLAY_NO_RD_LOG is defined we fall back to a
// minimal runtime formatter that handles exactly the {} and {:x} specifiers
// this file uses. Both paths emit identical-looking output.
#ifdef CROSSDESK_MAC_VIRTUAL_DISPLAY_NO_RD_LOG
#include <cstdio>
#include <sstream>
#include <type_traits>

namespace macvd_test_log {

inline void FormatInto(std::string& out, const char* fmt, size_t& pos) {
  while (fmt[pos] != '\0') out += fmt[pos++];
}

template <typename T, typename... Rest>
void FormatInto(std::string& out, const char* fmt, size_t& pos, T&& value,
                Rest&&... rest) {
  while (fmt[pos] != '\0') {
    if (fmt[pos] == '{' && fmt[pos + 1] == '}') {
      std::ostringstream oss;
      if constexpr (std::is_same_v<std::decay_t<T>, bool>) {
        oss << (value ? "true" : "false");
      } else {
        oss << value;
      }
      out += oss.str();
      pos += 2;
      FormatInto(out, fmt, pos, std::forward<Rest>(rest)...);
      return;
    }
    if (fmt[pos] == '{' && fmt[pos + 1] == ':' && fmt[pos + 2] == 'x' &&
        fmt[pos + 3] == '}') {
      std::ostringstream oss;
      oss << std::hex;
      if constexpr (std::is_integral_v<std::decay_t<T>>) {
        oss << static_cast<unsigned long long>(value);
      } else {
        oss << value;
      }
      out += oss.str();
      pos += 4;
      FormatInto(out, fmt, pos, std::forward<Rest>(rest)...);
      return;
    }
    out += fmt[pos++];
  }
}

template <typename... Args>
inline void Log(const char* level, const char* fmt, Args&&... args) {
  std::string rendered;
  size_t pos = 0;
  FormatInto(rendered, fmt, pos, std::forward<Args>(args)...);
  std::fprintf(stderr, "[%s] %s\n", level, rendered.c_str());
}

}  // namespace macvd_test_log

#define LOG_INFO(...) ::macvd_test_log::Log("info", __VA_ARGS__)
#define LOG_WARN(...) ::macvd_test_log::Log("warn", __VA_ARGS__)
#else
#include "rd_log.h"
#endif

// Forward declarations matching the private class shape. These are typing
// aids ONLY — they carry no linkage. We deliberately do NOT use @class
// alone because we need the method signatures visible to ARC.
@interface CGVirtualDisplayMode : NSObject
@property(readonly, nonatomic) NSUInteger width;
@property(readonly, nonatomic) NSUInteger height;
@property(readonly, nonatomic) double refreshRate;
- (instancetype)initWithWidth:(NSUInteger)width
                       height:(NSUInteger)height
                  refreshRate:(double)refreshRate;
@end

@interface CGVirtualDisplaySettings : NSObject
@property(nonatomic) unsigned int hiDPI;
@property(retain, nonatomic) NSArray<CGVirtualDisplayMode*>* modes;
@end

@interface CGVirtualDisplayDescriptor : NSObject
@property(retain, nonatomic) NSString* name;
@property(nonatomic) NSUInteger maxPixelsWide;
@property(nonatomic) NSUInteger maxPixelsHigh;
@property(nonatomic) CGSize sizeInMillimeters;
@property(nonatomic) unsigned int serialNum;
@property(nonatomic) unsigned int productID;
@property(nonatomic) unsigned int vendorID;
@property(copy, nonatomic) void (^terminationHandler)(id, id);
@property(retain, nonatomic) dispatch_queue_t queue;
@end

@interface CGVirtualDisplay : NSObject
@property(readonly, nonatomic) CGDirectDisplayID displayID;
- (instancetype)initWithDescriptor:(CGVirtualDisplayDescriptor*)descriptor;
- (BOOL)applySettings:(CGVirtualDisplaySettings*)settings;
@end

namespace crossdesk {

namespace {

// Runtime class lookup. Returns nil if Apple has removed the class. The
// static initialization is thread-safe (C++11 magic statics) and cheap
// because NSClassFromString is a hash lookup in the ObjC runtime.
Class VirtualDisplayClass() {
  static Class cls = NSClassFromString(@"CGVirtualDisplay");
  return cls;
}
Class VirtualDisplayDescriptorClass() {
  static Class cls = NSClassFromString(@"CGVirtualDisplayDescriptor");
  return cls;
}
Class VirtualDisplayModeClass() {
  static Class cls = NSClassFromString(@"CGVirtualDisplayMode");
  return cls;
}
Class VirtualDisplaySettingsClass() {
  static Class cls = NSClassFromString(@"CGVirtualDisplaySettings");
  return cls;
}

// Verify the instance responds to the selectors we depend on. This guards
// against Apple renaming or removing methods while keeping the class.
bool CheckSelectors() {
  Class vd = VirtualDisplayClass();
  Class vdd = VirtualDisplayDescriptorClass();
  Class vdm = VirtualDisplayModeClass();
  Class vds = VirtualDisplaySettingsClass();
  if (!vd || !vdd || !vdm || !vds) return false;

  if (![vd instancesRespondToSelector:@selector(initWithDescriptor:)]) return false;
  if (![vd instancesRespondToSelector:@selector(applySettings:)]) return false;
  if (![vd instancesRespondToSelector:@selector(displayID)]) return false;

  if (![vdd instancesRespondToSelector:@selector(init)]) return false;
  if (![vdd instancesRespondToSelector:@selector(setName:)]) return false;
  if (![vdd instancesRespondToSelector:@selector(setMaxPixelsWide:)]) return false;
  if (![vdd instancesRespondToSelector:@selector(setMaxPixelsHigh:)]) return false;
  if (![vdd instancesRespondToSelector:@selector(setSizeInMillimeters:)]) return false;
  if (![vdd instancesRespondToSelector:@selector(setSerialNum:)]) return false;
  if (![vdd instancesRespondToSelector:@selector(setVendorID:)]) return false;
  if (![vdd instancesRespondToSelector:@selector(setProductID:)]) return false;

  if (![vdm instancesRespondToSelector:@selector(initWithWidth:height:refreshRate:)])
    return false;

  if (![vds instancesRespondToSelector:@selector(init)]) return false;
  if (![vds instancesRespondToSelector:@selector(setHiDPI:)]) return false;
  if (![vds instancesRespondToSelector:@selector(setModes:)]) return false;

  return true;
}

// Pump the current runloop for up to |max_ms| milliseconds in small
// increments so WindowServer can process virtual display plug/unplug
// asynchronously. Returns immediately if the runloop has no sources.
void DrainRunloopFor(uint32_t max_ms) {
  const uint32_t step_ms = 20;
  uint32_t waited = 0;
  while (waited < max_ms) {
    NSDate* deadline = [NSDate dateWithTimeIntervalSinceNow:step_ms / 1000.0];
    [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode beforeDate:deadline];
    waited += step_ms;
  }
}

// Compute a plausible physical size in millimetres for a given logical
// resolution. WindowServer uses this for PPI calculation; the exact value
// matters for whether macOS recognises the display as Retina-capable.
// HiDPI requires >= ~200 PPI, standard displays sit around 80-110 PPI.
CGSize SizeInMillimeters(uint32_t width, uint32_t height, bool hi_dpi,
                         uint32_t backing_w, uint32_t backing_h) {
  (void)width;
  (void)height;
  const double ppi = hi_dpi ? 220.0 : 96.0;
  const double mm_per_inch = 25.4;
  return CGSizeMake(backing_w * mm_per_inch / ppi,
                    backing_h * mm_per_inch / ppi);
}

// Vendor/product/serial triple that identifies Farlink virtual displays in
// System Settings. Vendor 0x4652 = "FR" (Farlink), product 0x4C4B = "LK".
//
// IMPORTANT DESIGN DECISION — serial number strategy:
//   WindowServer treats (vendorID, productID, serialNum) as a unique key:
//   attempting to create a second virtual display with a triple that is
//   already registered returns nil from initWithDescriptor: (verified on
//   macOS 27.2.1). A process that crashes without releasing its virtual
//   display leaves the display attached, and any future process using the
//   same serial will fail to create one.
//
//   We therefore derive the serial from the process PID and a random
//   component, guaranteeing uniqueness across processes and across
//   restarts. The trade-off is that macOS display arrangement preferences
//   will not persist across host restarts — acceptable because the virtual
//   display is primarily for headless capture, where arrangement is
//   irrelevant (there is no physical display to arrange against).
constexpr unsigned int kFarlinkVendorID = 0x4652;
constexpr unsigned int kFarlinkProductID = 0x4C4B;

unsigned int GenerateSerialNum() {
  // Mix PID (low 16 bits) with a random high component. arc4random is
  // always available on macOS and does not require seeding.
  const unsigned int pid_part = static_cast<unsigned int>(getpid()) & 0xFFFF;
  const unsigned int rand_part = arc4random() & 0xFFFF0000u;
  // Ensure the low bit is set so the serial is never zero (some tooling
  // treats serial=0 as "unspecified").
  return (rand_part | pid_part | 0x1u);
}

}  // namespace

struct MacVirtualDisplay::Impl {
  // Strong references under ARC. nil'ed in Destroy() to trigger teardown.
  CGVirtualDisplay* display = nil;
  CGVirtualDisplayDescriptor* descriptor = nil;
  CGVirtualDisplaySettings* settings = nil;
  // Dedicated serial queue for termination handler callbacks. Using the
  // main queue would risk deadlock if Destroy() is invoked from a thread
  // that already owns the main runloop; a private queue avoids that.
  dispatch_queue_t queue = nullptr;
};

MacVirtualDisplay::~MacVirtualDisplay() {
  Destroy();
  if (impl_) {
    if (impl_->queue) {
      // dispatch_release is implicit under ARC when targeting macOS 10.8+.
      impl_->queue = nullptr;
    }
    delete impl_;
    impl_ = nullptr;
  }
}

bool MacVirtualDisplay::IsRuntimeSupported() {
  return CheckSelectors();
}

bool MacVirtualDisplay::TeardownReliable() {
  // Empirically verified on macOS 27.2.1 (build 26B131): virtual display
  // teardown is broken. Destroying the object does NOT remove the display
  // from CGGetActiveDisplayList, and even clean process exit leaves the
  // display attached. WindowServer logs CGXVirtualDisplayDestroy entering
  // and exiting with "not found". This contradicts community-verified
  // behavior on macOS 11 through 15.
  //
  // We detect the affected range by inspecting the operating system
  // version. Until Apple fixes the defect we report unreliability for
  // macOS 27.x. Earlier versions (macOS 11-15) are believed to be reliable
  // based on community consensus, though we have not independently
  // re-verified every minor release.
  static const bool reliable = [] {
    NSOperatingSystemVersion version =
        [[NSProcessInfo processInfo] operatingSystemVersion];
    // macOS versioning: majorVersion is 27 for the current 27.x series
    // (post-Tahoe, 2025+). 27.x is affected. Pre-27 we assume reliable.
    if (version.majorVersion >= 27) {
      return false;
    }
    return true;
  }();
  return reliable;
}

bool MacVirtualDisplay::Create(const MacVirtualDisplaySpec& raw_spec,
                               std::string* error) {
  if (active()) {
    if (error) *error = "virtual display already active";
    return false;
  }
  if (!IsRuntimeSupported()) {
    if (error)
      *error = "CGVirtualDisplay runtime unavailable on this macOS build";
    return false;
  }

  // Sanitize the request through policy. The caller may pass arbitrary
  // values; we clamp them into the supported envelope.
  const auto clamped = mac_virtual_display_policy::ClampSpec(
      raw_spec.width, raw_spec.height, raw_spec.refresh_rate,
      raw_spec.hi_dpi);
  if (clamped.adjusted) {
    LOG_INFO(
        "MacVirtualDisplay: clamped request {}x{}@{}Hz hi_dpi={} -> "
        "{}x{}@{}Hz hi_dpi={}",
        raw_spec.width, raw_spec.height, raw_spec.refresh_rate,
        raw_spec.hi_dpi, clamped.width, clamped.height,
        clamped.refresh_rate, clamped.hi_dpi);
  }

  // Lazily allocate impl state.
  if (!impl_) {
    impl_ = new Impl();
    impl_->queue = dispatch_queue_create(
        "com.farlink.mac-virtual-display", DISPATCH_QUEUE_SERIAL);
  }

  @autoreleasepool {
    Class vdesc_cls = VirtualDisplayDescriptorClass();
    Class vdisp_cls = VirtualDisplayClass();
    Class vmode_cls = VirtualDisplayModeClass();
    Class vset_cls = VirtualDisplaySettingsClass();

    // Compute backing (physical) resolution. HiDPI doubles both dimensions.
    const uint32_t backing_w =
        clamped.hi_dpi ? clamped.width * 2 : clamped.width;
    const uint32_t backing_h =
        clamped.hi_dpi ? clamped.height * 2 : clamped.height;

    // Build the descriptor.
    CGVirtualDisplayDescriptor* desc = [[vdesc_cls alloc] init];
    if (!desc) {
      if (error) *error = "failed to allocate CGVirtualDisplayDescriptor";
      return false;
    }
    desc.name =
        [NSString stringWithUTF8String:raw_spec.name.empty()
                                           ? "Farlink Virtual Display"
                                           : raw_spec.name.c_str()];
    // Allocate a generous maxPixels envelope rather than the exact backing
    // resolution. The descriptor's maxPixels* fields cap what resolutions
    // can be applied later via Reconfigure() — they cannot be raised after
    // creation. Because teardown is unreliable on macOS 27.x and the
    // display object is effectively a session-long singleton, callers may
    // want to reconfigure to a larger size later. Reserving headroom at
    // creation time is cheap (the framebuffer is allocated lazily per
    // applied mode, not eagerly at the envelope size).
    //
    // We cap at the absolute policy maximum to avoid pathological GPU
    // memory pressure on 8 GB Macs.
    const uint32_t envelope_w =
        std::max(backing_w, mac_virtual_display_policy::kMaxWidth);
    const uint32_t envelope_h =
        std::max(backing_h, mac_virtual_display_policy::kMaxHeight);
    desc.maxPixelsWide = envelope_w;
    desc.maxPixelsHigh = envelope_h;
    desc.sizeInMillimeters = SizeInMillimeters(
        clamped.width, clamped.height, clamped.hi_dpi, backing_w, backing_h);
    desc.vendorID = kFarlinkVendorID;
    desc.productID = kFarlinkProductID;
    desc.serialNum = GenerateSerialNum();
    desc.queue = impl_->queue;

    // Set up a termination handler that simply logs. WindowServer invokes
    // this if the display is terminated externally (e.g. by another
    // management tool, or if WindowServer itself tears down). The first
    // argument is the opaque termination reason, the second the display
    // object itself; we don't depend on their exact types.
    desc.terminationHandler = ^(id /*reason*/, id /*display*/) {
      LOG_WARN(
          "MacVirtualDisplay: external termination of virtual display "
          "(WindowServer or peer) — capture may drop");
    };

    // Allocate the display object. The object does not yet represent a real
    // display; that only happens after applySettings: succeeds.
    CGVirtualDisplay* display =
        [[vdisp_cls alloc] initWithDescriptor:desc];
    if (!display) {
      if (error)
        *error =
            "initWithDescriptor: returned nil — SPI blocked or system limit";
      return false;
    }

    // Build the mode. CGVirtualDisplayMode's initWithWidth:height:refreshRate:
    // takes the *logical* dimensions when HiDPI is in use; macOS uses the
    // hiDPI flag in settings + maxPixels* in the descriptor to derive the
    // backing resolution. We add BOTH a HiDPI anchor mode at the backing
    // resolution and the logical mode when hi_dpi is true, matching the
    // community-documented pattern. For non-HiDPI we add just the single
    // logical mode.
    NSMutableArray<CGVirtualDisplayMode*>* modes = [NSMutableArray array];
    if (clamped.hi_dpi) {
      CGVirtualDisplayMode* anchor =
          [[vmode_cls alloc] initWithWidth:backing_w
                                    height:backing_h
                               refreshRate:clamped.refresh_rate];
      if (anchor) [modes addObject:anchor];
    }
    CGVirtualDisplayMode* logical =
        [[vmode_cls alloc] initWithWidth:clamped.width
                                  height:clamped.height
                             refreshRate:clamped.refresh_rate];
    if (!logical) {
      if (error) *error = "failed to allocate CGVirtualDisplayMode";
      return false;
    }
    [modes addObject:logical];

    CGVirtualDisplaySettings* settings = [[vset_cls alloc] init];
    if (!settings) {
      if (error) *error = "failed to allocate CGVirtualDisplaySettings";
      return false;
    }
    settings.hiDPI = clamped.hi_dpi ? 1u : 0u;
    settings.modes = modes;

    // Apply settings. This is synchronous in the ObjC sense but the
    // WindowServer-side plug-in is asynchronous; the displayID is however
    // valid immediately after a successful return.
    if (![display applySettings:settings]) {
      if (error)
        *error = "applySettings: rejected — unsupported mode or hiDPI combo";
      return false;
    }

    const CGDirectDisplayID vd_id = display.displayID;
    if (vd_id == 0 || vd_id == kCGNullDirectDisplay) {
      if (error) *error = "displayID invalid after applySettings:";
      return false;
    }

    // Retain into impl (ARC strong references). After this point the
    // display is owned by *this* until Destroy().
    impl_->display = display;
    impl_->descriptor = desc;
    impl_->settings = settings;
    display_id_ = vd_id;
    spec_ = raw_spec;
    spec_.width = clamped.width;
    spec_.height = clamped.height;
    spec_.refresh_rate = clamped.refresh_rate;
    spec_.hi_dpi = clamped.hi_dpi;

    LOG_INFO(
        "MacVirtualDisplay: created id=0x{:x} logical={}x{}@{}Hz "
        "backing={}x{} hi_dpi={}",
        vd_id, clamped.width, clamped.height, clamped.refresh_rate,
        backing_w, backing_h, clamped.hi_dpi);

    // Give WindowServer a brief window to complete the plug-in. This is not
    // strictly required for correctness (the displayID is already valid),
    // but reduces the race window for callers that immediately query
    // CGGetActiveDisplayList.
    DrainRunloopFor(80);
    return true;
  }
}

bool MacVirtualDisplay::Reconfigure(const MacVirtualDisplaySpec& raw_spec,
                                    std::string* error) {
  // IMPORTANT — macOS 27.x behavior (verified on 27.2.1):
  //
  // Calling applySettings: with new modes DOES update the display's mode
  // list. However, the display's *active* mode does not change, and
  // explicit attempts to switch via CGDisplaySetDisplayMode or
  // CGConfigureDisplayWithDisplayMode fail with kCGErrorCannotComplete
  // (1014). The display's resolution is effectively immutable after
  // creation on macOS 27.x.
  //
  // Therefore this function is honest about its limitations:
  //   - If the display is not active, fall through to Create().
  //   - If the requested spec matches the current spec, succeed.
  //   - On macOS 27.x, otherwise fail with a descriptive error. Callers
  //     that genuinely need a different resolution must destroy the host
  //     process and restart — the leaked display persists until logout
  //     regardless.
  //   - On pre-macOS-27 systems we attempt the applySettings path. The
  //     displayID is preserved either way.

  if (!active()) {
    return Create(raw_spec, error);
  }

  // Clamp the new spec.
  const auto clamped = mac_virtual_display_policy::ClampSpec(
      raw_spec.width, raw_spec.height, raw_spec.refresh_rate,
      raw_spec.hi_dpi);

  // If the requested spec is identical to the current one, skip.
  if (clamped.width == spec_.width && clamped.height == spec_.height &&
      clamped.refresh_rate == spec_.refresh_rate &&
      clamped.hi_dpi == spec_.hi_dpi) {
    return true;
  }

  // On macOS 27.x the active mode is immutable after creation — even
  // though the mode-list update would technically succeed, the resolution
  // the user sees does not change. Fail honestly rather than pretend.
  if (!TeardownReliable()) {
    if (error) {
      *error =
          "macOS 27.x does not support changing the resolution of an "
          "existing virtual display (CGDisplaySetDisplayMode fails with "
          "kCGErrorCannotComplete). Destroy the host process and recreate "
          "the display at the desired resolution; the leaked display will "
          "persist until logout.";
    }
    LOG_WARN(
        "MacVirtualDisplay: reconfigure {}x{} -> {}x{} rejected — macOS "
        "27.x does not support mode switching on virtual displays",
        spec_.width, spec_.height, clamped.width, clamped.height);
    return false;
  }


  if (!IsRuntimeSupported() || !impl_ || !impl_->display) {
    if (error) *error = "virtual display object unavailable";
    return false;
  }

  @autoreleasepool {
    Class vmode_cls = VirtualDisplayModeClass();
    Class vset_cls = VirtualDisplaySettingsClass();

    // Compute backing resolution.
    const uint32_t backing_w =
        clamped.hi_dpi ? clamped.width * 2 : clamped.width;
    const uint32_t backing_h =
        clamped.hi_dpi ? clamped.height * 2 : clamped.height;

    // The descriptor's maxPixels* fields cap what the display can do. If
    // the new backing resolution exceeds them, applySettings will reject
    // the modes. We cannot mutate the descriptor post-creation — the only
    // solution is to fail honestly. Callers that need a larger envelope
    // must pass a generous initial size at Create() time.
    // Note: we do not currently introspect the existing descriptor because
    // the property getters are not guaranteed stable across macOS versions.
    // Instead we optimistically apply and report failure if it does not
    // take.

    // Build new mode list.
    NSMutableArray<CGVirtualDisplayMode*>* modes = [NSMutableArray array];
    if (clamped.hi_dpi) {
      CGVirtualDisplayMode* anchor =
          [[vmode_cls alloc] initWithWidth:backing_w
                                    height:backing_h
                               refreshRate:clamped.refresh_rate];
      if (anchor) [modes addObject:anchor];
    }
    CGVirtualDisplayMode* logical =
        [[vmode_cls alloc] initWithWidth:clamped.width
                                  height:clamped.height
                             refreshRate:clamped.refresh_rate];
    if (!logical) {
      if (error) *error = "failed to allocate CGVirtualDisplayMode";
      return false;
    }
    [modes addObject:logical];

    CGVirtualDisplaySettings* settings = [[vset_cls alloc] init];
    if (!settings) {
      if (error) *error = "failed to allocate CGVirtualDisplaySettings";
      return false;
    }
    settings.hiDPI = clamped.hi_dpi ? 1u : 0u;
    settings.modes = modes;

    if (![impl_->display applySettings:settings]) {
      if (error)
        *error =
            "applySettings: rejected — resolution may exceed the original "
            "descriptor's maxPixels envelope";
      return false;
    }

    // Replace the retained settings object so the previous one is released.
    impl_->settings = settings;
    spec_ = raw_spec;
    spec_.width = clamped.width;
    spec_.height = clamped.height;
    spec_.refresh_rate = clamped.refresh_rate;
    spec_.hi_dpi = clamped.hi_dpi;

    LOG_INFO(
        "MacVirtualDisplay: reconfigured id=0x{:x} logical={}x{}@{}Hz "
        "backing={}x{} hi_dpi={}",
        display_id_, clamped.width, clamped.height, clamped.refresh_rate,
        backing_w, backing_h, clamped.hi_dpi);

    DrainRunloopFor(80);
    return true;
  }
}

void MacVirtualDisplay::Destroy() {
  if (!impl_) {
    display_id_ = 0;
    return;
  }
  const bool had_display = (impl_->display != nil);
  const uint32_t old_id = display_id_;

  // nil out in dependency order: settings/modes are children, display is
  // the root. ARC will release each; once the display object itself is
  // released, WindowServer receives a no-senders notification and starts
  // asynchronous teardown.
  @autoreleasepool {
    impl_->settings = nil;
    impl_->descriptor = nil;
    impl_->display = nil;
  }
  display_id_ = 0;

  if (had_display) {
    // Pump the runloop briefly so the WindowServer no-senders notification
    // is delivered before we return. Best-effort only.
    DrainRunloopFor(150);
    if (TeardownReliable()) {
      LOG_INFO("MacVirtualDisplay: destroyed id=0x{:x}", old_id);
    } else {
      // Honest reporting on macOS 27.x: the display object is gone from
      // our perspective, but the system-level display very likely
      // persists. Because the serial is unique per process, a future
      // Create() call in this process will allocate a fresh display
      // rather than collide with the leaked one.
      LOG_WARN(
          "MacVirtualDisplay: released id=0x{:x} but macOS 27.x teardown "
          "defect means the display may persist in the system until "
          "logout/login",
          old_id);
    }
  }
}

}  // namespace crossdesk
