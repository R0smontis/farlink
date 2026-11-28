/*
 *  Copyright (c) 2024 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "screen_capturer_sck.h"

#include <AppKit/AppKit.h>
#include <ApplicationServices/ApplicationServices.h>
#include <CoreGraphics/CoreGraphics.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/graphics/IOGraphicsLib.h>
#include <IOSurface/IOSurface.h>
#include <ScreenCaptureKit/ScreenCaptureKit.h>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <vector>
#include "display_info.h"
#include <display_stream_id.h>
#include "rd_log.h"
#include "sdr_color.h"
#include "privacy_controller.h"
#include "../privacy/privacy_capture_state.h"
#include "../video_pixel_format.h"
#include <algorithm>

using namespace crossdesk;

class ScreenCapturerSckImpl;

static const int kFullDesktopScreenId = -1;
static constexpr auto kCaptureStartTimeout = std::chrono::seconds(5);
// Zombie detector budget, measured from startCapture completion
// (ConfigurationFinished), not from issuing the operation: the completion
// itself keeps the kCaptureStartTimeout budget above. Distribution across
// 44 healthy starts in the round-8/acceptance logs: completion->first frame
// median 3 ms, p90 7 ms, max 52 ms; no frame ever arrived after 100 ms. All
// 39 observed zombies (start accepted, frames never produced) stayed silent
// for the full detector window. 1 s is 20x the worst healthy start and still
// shrinks a zombie retry cycle from 5+2 s to 1+2 s.
static constexpr auto kFirstFrameTimeout = std::chrono::seconds(1);

// ScreenCaptureKit buffers output frames up to SCStreamConfiguration.queueDepth
// before dropping. The default is 8: whenever encode admission or a multi-peer
// callback briefly costs more than one frame interval, stale frames pile up in
// that queue and every subsequent delivery carries up to 8 frames (~130 ms at
// 60 fps) of hidden capture lag.
//
// The pool-retention invariant sets the safe minimum. The capture pipeline
// retains each delivered CVPixelBuffer downstream: the transport admits at
// most kMaxEncodeFramesInFlight = 2 frames into VideoToolbox, and
// VTCompressionSession retains each source pixel buffer until its output
// callback runs. Because SCStream serves sample buffers from a backing pool
// sized by queueDepth, queueDepth = 1 or 2 can starve the stream outright —
// every pool buffer is still retained by in-flight encode work when the next
// display interval arrives, so SCStream stalls instead of capturing. A depth
// of 3 covers both encode-retained buffers plus one buffer the capture
// callback itself is still dispatching, while capping worst-case capture
// queue latency at 3 frames instead of 8. FARLINK_SCK_QUEUE_DEPTH overrides
// the depth for A/B measurement; values below 3 risk the pool starvation
// described above.
static NSInteger ResolveSckQueueDepth() {
  constexpr NSInteger kDefaultQueueDepth = 3;
  const char* env = std::getenv("FARLINK_SCK_QUEUE_DEPTH");
  if (!env || env[0] == '\0') return kDefaultQueueDepth;
  char* end = nullptr;
  const long parsed = std::strtol(env, &end, 10);
  if (end == env || *end != '\0' || parsed < 1 || parsed > 8) {
    return kDefaultQueueDepth;
  }
  return static_cast<NSInteger>(parsed);
}

// Capture-side colorimetry opt-out. The default stream configuration tags
// the desktop as full-range BT.709 (420f/xf44 + ITU_R_709_2), matching both
// what macOS actually renders and what VideoToolbox/Metal already consume
// losslessly; 420f preserves all 256 luma codes instead of crushing them
// into the 220-code 16..235 studio swing, and BT.709 is the exact matrix the
// sRGB desktop was rasterized in. FARLINK_MAC_CAPTURE_LIMITED_601=1 restores
// the legacy limited-range BT.601 (420v + ITU_R_601_4) stream for A/B
// comparison or a consumer that cannot read the tagged attachments.
static bool MacCaptureLimited601Enabled() {
  static const bool enabled = [] {
    const char* env = std::getenv("FARLINK_MAC_CAPTURE_LIMITED_601");
    return env && env[0] == '1';
  }();
  return enabled;
}

// The 4:2:0 stream pixel format. Full-range 420f keeps the sRGB desktop's
// 256 luma codes intact through capture; VideoToolbox accepts it natively
// and writes video_full_range_flag=1 plus the BT.709 VUI into the bitstream
// straight from the buffer attachments.
static OSType ResolveSck420PixelFormat() {
  return MacCaptureLimited601Enabled()
             ? kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange
             : kCVPixelFormatType_420YpCbCr8BiPlanarFullRange;
}

static CFStringRef ResolveSckColorMatrix() {
  return MacCaptureLimited601Enabled()
             ? kCGDisplayStreamYCbCrMatrix_ITU_R_601_4
             : kCGDisplayStreamYCbCrMatrix_ITU_R_709_2;
}

static std::string NSErrorToString(NSError *error) {
  if (!error) {
    return "";
  }

  const char *description = [error.localizedDescription UTF8String];
  const char *domain = [error.domain UTF8String];
  // The failure reason often carries the actual cause where the localized
  // description is only a generic translation. userInfo is intentionally
  // omitted: its multi-line formatting breaks single-line log greppability.
  const char *reason = [error.localizedFailureReason UTF8String];
  return std::string(domain ? domain : "NSError") + ":" +
      std::to_string(error.code) + " " + (description ? description : "") +
      (reason ? std::string(" reason=") + reason : "");
}

static void RetainCapturedPixelBuffer(void *owner) {
  if (owner) {
    CVPixelBufferRetain(static_cast<CVPixelBufferRef>(owner));
  }
}

static void ReleaseCapturedPixelBuffer(void *owner) {
  if (owner) {
    CVPixelBufferRelease(static_cast<CVPixelBufferRef>(owner));
  }
}

static int CopyCapturedPixelBufferToNv12(void *owner, uint8_t *destination,
                                         size_t destination_size) {
  auto pixel_buffer = static_cast<CVPixelBufferRef>(owner);
  if (!pixel_buffer || !destination ||
      !CVPixelBufferIsPlanar(pixel_buffer) ||
      CVPixelBufferGetPlaneCount(pixel_buffer) < 2) {
    return -1;
  }
  const OSType pixel_format = CVPixelBufferGetPixelFormatType(pixel_buffer);
  if (IsMacYuv444(pixel_format)) {
    static std::once_flag notice;
    std::call_once(notice, [] { LOG_WARN("4:4:4 capture requires native CVPixelBuffer input"); });
    return -1;  // Neither 4:4:4 plane has NV12's layout.
  }
  if (!IsMacNv12(pixel_format)) return -1;

  const size_t width = CVPixelBufferGetWidth(pixel_buffer);
  const size_t height = CVPixelBufferGetHeight(pixel_buffer);
  const size_t required_size = width * height * 3U / 2U;
  if (destination_size < required_size ||
      CVPixelBufferLockBaseAddress(pixel_buffer,
                                   kCVPixelBufferLock_ReadOnly) !=
          kCVReturnSuccess) {
    return -1;
  }

  const auto *y_plane = static_cast<const uint8_t *>(
      CVPixelBufferGetBaseAddressOfPlane(pixel_buffer, 0));
  const auto *uv_plane = static_cast<const uint8_t *>(
      CVPixelBufferGetBaseAddressOfPlane(pixel_buffer, 1));
  const size_t y_stride = CVPixelBufferGetBytesPerRowOfPlane(pixel_buffer, 0);
  const size_t uv_stride = CVPixelBufferGetBytesPerRowOfPlane(pixel_buffer, 1);
  int result = 0;
  if (!y_plane || !uv_plane || y_stride < width || uv_stride < width) {
    result = -1;
  } else {
    for (size_t row = 0; row < height; ++row) {
      std::memcpy(destination + row * width, y_plane + row * y_stride, width);
    }
    uint8_t *uv_destination = destination + width * height;
    for (size_t row = 0; row < height / 2U; ++row) {
      std::memcpy(uv_destination + row * width,
                  uv_plane + row * uv_stride, width);
    }
  }
  CVPixelBufferUnlockBaseAddress(pixel_buffer, kCVPixelBufferLock_ReadOnly);
  if (result == 0) {
    CFTypeRef matrix = CVBufferCopyAttachment(
        pixel_buffer, kCVImageBufferYCbCrMatrixKey, nullptr);
    const minirtc::SdrColor color{
        pixel_format == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange,
        matrix && CFEqual(matrix, kCVImageBufferYCbCrMatrix_ITU_R_709_2)};
    if (matrix) CFRelease(matrix);
    color.Normalize(destination, width, height);
  }
  return result;
}

// The ScreenCaptureKit API was available in macOS 12.3, but full-screen capture
// was reported to be broken before macOS 13 - see http://crbug.com/40234870.
// Also, the `SCContentFilter` fields `contentRect` and `pointPixelScale` were
// introduced in macOS 14.
API_AVAILABLE(macos(14.0))
@interface SckHelper : NSObject <SCStreamDelegate, SCStreamOutput>

- (instancetype)initWithCapturer:(ScreenCapturerSckImpl *)capturer;

- (void)onShareableContentCreated:(SCShareableContent *)content generation:(uint64_t)generation
                       revision:(uint64_t)revision;

// Called just before the capturer is destroyed. This avoids a dangling pointer,
// and prevents any new calls into a deleted capturer. If any method-call on the
// capturer is currently running on a different thread, this blocks until it
// completes.
- (void)releaseCapturer;
- (void)privacyCoversChanged:(NSNotification*)notification;
- (void)scheduleHealthCheck:(uint64_t)session;
- (void)configurationFinished:(SCStream*)stream generation:(uint64_t)generation
                     revision:(uint64_t)revision complete:(BOOL)complete
                configuration:(SCStreamConfiguration*)configuration error:(NSError*)error;
@end

class API_AVAILABLE(macos(14.0)) ScreenCapturerSckImpl : public ScreenCapturer {
 public:
  explicit ScreenCapturerSckImpl();

  ScreenCapturerSckImpl(const ScreenCapturerSckImpl &) = delete;
  ScreenCapturerSckImpl &operator=(const ScreenCapturerSckImpl &) = delete;
  ~ScreenCapturerSckImpl();

 public:
  int Init(const int fps, cb_desktop_data cb) override;
  void SetPrivacyController(PrivacyController* privacy) override { privacy_ = privacy; }
  void SetTrueColorEnabled(bool enabled) override;

  int Start(bool show_cursor) override;

  int SwitchTo(int monitor_index) override;

  int Destroy() override;

  int Stop() override;

  int Pause(int monitor_index) override { return 0; }

  int Resume(int monitor_index) override { return 0; }

  std::vector<DisplayInfo> GetDisplayInfoList() override {
    std::lock_guard<std::mutex> lock(lock_);
    return display_info_list_;
  }
  int ResetToInitialMonitor() override;

 private:
  std::vector<DisplayInfo> display_info_list_;
  std::map<int, CGDirectDisplayID> display_id_map_;
  int fps_ = 60;
  bool show_cursor_ = false;
  bool capture_requested_ = false;
  // Start() accepts an asynchronous request. Only its completion marks the
  // native stream running; recovery stays active until Stop()/Destroy().
  bool stream_running_ = false;
  bool received_frame_ = false;
  bool recovery_pending_ = false;
  unsigned recovery_attempt_ = 0;
  uint64_t capture_session_ = 0;
  using Clock = std::chrono::steady_clock;
  Clock::time_point capture_attempt_started_{};
  Clock::time_point configuration_started_{};
  // Armed when startCapture completes successfully (ConfigurationFinished);
  // the zombie detector measures completion->first-frame silence from here.
  Clock::time_point stream_started_at_{};
  Clock::time_point enumeration_started_{};
  bool enumeration_in_flight_ = false;
  bool enumeration_stall_logged_ = false;
  // Cached shareable content. SCShareableContent is immutable and the filter
  // construction retains its SCDisplay/SCWindow objects, so retries within a
  // few seconds can safely reuse one enumeration instead of repeating the
  // multi-second window-list round trip. Keyed on the privacy revision: a
  // cover change bumps the revision and thereby forces fresh enumeration,
  // which the exclusion logic depends on. Screen-parameter notifications
  // invalidate the entry explicitly.
  SCShareableContent* __strong cached_content_ = nil;
  uint64_t cached_content_revision_ = 0;
  Clock::time_point cached_content_time_{};

  Clock::time_point suspended_since_{};
  Clock::time_point next_recovery_{};
  bool invalid_stream_id_logged_ = false;
  bool native_output_logged_ = false;
  PrivacyController* privacy_ = nullptr;
  uint64_t configuration_generation_ = 0;
  bool configuration_in_flight_ = false;
  bool reconfigure_pending_ = false;
  SCStreamConfiguration* __strong applied_configuration_ = nil;
  bool true_color_enabled_ = false;

 public:
  // Called by SckHelper when shareable content is returned by ScreenCaptureKit. `content` will be
  // nil if an error occurred. May run on an arbitrary thread.
  void OnShareableContentCreated(SCShareableContent *content, uint64_t generation,
                                uint64_t revision);
  void ConfigurationFinished(SCStream* source, uint64_t generation, uint64_t revision,
                             bool complete, SCStreamConfiguration* configuration,
                             NSError* error);
  void RetryPrivacyFilter();
  bool CheckCaptureHealth(uint64_t session);
  // Screen geometry/topology changed: the cached window and display lists are
  // no longer trustworthy, so the next configuration must enumerate again.
  void CaptureConfigurationChanged() {
    {
      std::lock_guard<std::mutex> lock(lock_);
      cached_content_ = nil;
    }
    StartOrReconfigureCapturer();
  }
  // Privacy covers changed. The cover list publication always bumps the
  // privacy revision, which already invalidates the revision-keyed content
  // cache; no explicit invalidation is needed here.
  void PrivacyCoversChanged() { StartOrReconfigureCapturer(); }
  void CaptureStopped(SCStream* source, NSError* error);
  void CaptureSuspended(SCStream* source);
  // Called by SckHelper to notify of a newly captured frame. May run on an arbitrary thread.
  void OnNewCVPixelBuffer(SCStream* source, CVPixelBufferRef pixelBuffer);

 private:
  // Called when starting the capturer or the configuration has changed (either from a
  // SwitchTo() call, or the screen-resolution has changed). This tells SCK to fetch new
  // shareable content, and the completion-handler will either start a new stream, or reconfigure
  // the existing stream. Runs on the caller's thread.
  void StartOrReconfigureCapturer();
  void RecoverCapture(uint64_t generation, const std::string& reason,
                      SCStream* stopped_stream = nil, bool retry = true);
  // Helper object to receive Objective-C callbacks from ScreenCaptureKit and call into this C++
  // object. The helper may outlive this C++ instance, if a completion-handler is passed to
  // ScreenCaptureKit APIs and the C++ object is deleted before the handler executes.
  SckHelper *__strong helper_ = nil;
  // Callback for returning captured frames to the caller. ScreenCaptureKit may
  // invoke it on its sample-handler queue.
  cb_desktop_data _on_data = nullptr;
  // Guards some variables that may be accessed on different threads.
  std::mutex lock_;
  // Provides captured desktop frames.
  SCStream *__strong stream_ = nil;
  // Currently selected display, or 0 if the full desktop is selected. This capturer does not
  // support full-desktop capture, and will fall back to the first display.
  CGDirectDisplayID current_display_ = 0;
  int initial_monitor_index_ = 0;
  int current_monitor_index_ = 0;
  std::string current_stream_id_;
};

static std::string NSStringToUtf8(NSString *value) {
  if (!value || value.length == 0) return "";
  const char *utf8 = value.UTF8String;
  return utf8 ? utf8 : "";
}

static std::string GetAppKitDisplayName(CGDirectDisplayID display_id) {
  if (@available(macOS 10.15, *)) {
    @autoreleasepool {
      for (NSScreen *screen in NSScreen.screens) {
        NSNumber *screen_number = screen.deviceDescription[@"NSScreenNumber"];
        if (screen_number && screen_number.unsignedIntValue == display_id) {
          return NSStringToUtf8(screen.localizedName);
        }
      }
    }
  }
  return "";
}

std::string GetDisplayName(CGDirectDisplayID display_id) {
  // IODisplayConnect is no longer exposed for some external displays on
  // modern Apple Silicon Macs. NSScreen is the system-owned source for the
  // user-visible name and maps directly to CGDirectDisplayID.
  std::string appkit_name = GetAppKitDisplayName(display_id);
  if (!appkit_name.empty()) return appkit_name;

  // Keep the IOKit path for older macOS versions and display drivers.
  io_iterator_t iter;
  io_service_t serv = 0, matched_serv = 0;

  CFMutableDictionaryRef matching = IOServiceMatching("IODisplayConnect");
  if (IOServiceGetMatchingServices(kIOMainPortDefault, matching, &iter) != KERN_SUCCESS) {
    return "";
  }

  while ((serv = IOIteratorNext(iter)) != 0) {
    CFDictionaryRef info = IODisplayCreateInfoDictionary(serv, kIODisplayOnlyPreferredName);
    if (info) {
      CFNumberRef vendorID = (CFNumberRef)CFDictionaryGetValue(info, CFSTR(kDisplayVendorID));
      CFNumberRef productID = (CFNumberRef)CFDictionaryGetValue(info, CFSTR(kDisplayProductID));
      uint32_t vID = 0, pID = 0;
      if (vendorID && productID && CFNumberGetValue(vendorID, kCFNumberIntType, &vID) &&
          CFNumberGetValue(productID, kCFNumberIntType, &pID) &&
          vID == CGDisplayVendorNumber(display_id) && pID == CGDisplayModelNumber(display_id)) {
        matched_serv = serv;
        CFRelease(info);
        break;
      }
      CFRelease(info);
    }
    IOObjectRelease(serv);
  }
  IOObjectRelease(iter);

  if (!matched_serv) return "";

  CFDictionaryRef display_info =
      IODisplayCreateInfoDictionary(matched_serv, kIODisplayOnlyPreferredName);
  IOObjectRelease(matched_serv);
  if (!display_info) return "";

  CFDictionaryRef product_name_dict =
      (CFDictionaryRef)CFDictionaryGetValue(display_info, CFSTR(kDisplayProductName));
  std::string result;
  if (product_name_dict) {
    CFIndex count = CFDictionaryGetCount(product_name_dict);
    if (count > 0) {
      std::vector<const void *> keys(count);
      std::vector<const void *> values(count);
      CFDictionaryGetKeysAndValues(product_name_dict, keys.data(), values.data());
      CFStringRef name_ref = (CFStringRef)values[0];
      if (name_ref) {
        CFIndex maxSize =
            CFStringGetMaximumSizeForEncoding(CFStringGetLength(name_ref), kCFStringEncodingUTF8) +
            1;
        std::vector<char> buffer(maxSize);
        if (CFStringGetCString(name_ref, buffer.data(), buffer.size(), kCFStringEncodingUTF8)) {
          result = buffer.data();
        }
      }
    }
  }
  CFRelease(display_info);
  return result;
}

ScreenCapturerSckImpl::ScreenCapturerSckImpl() {
  helper_ = [[SckHelper alloc] initWithCapturer:this];
}

ScreenCapturerSckImpl::~ScreenCapturerSckImpl() {
  Destroy();
}

static std::vector<CGDirectDisplayID> GetOnlineCaptureDisplays() {
  uint32_t count = 0;
  if (CGGetOnlineDisplayList(0, nullptr, &count) != kCGErrorSuccess || !count)
    return {};

  std::vector<CGDirectDisplayID> displays(count);
  if (CGGetOnlineDisplayList(count, displays.data(), &count) != kCGErrorSuccess)
    return {};
  displays.resize(count);
  displays.erase(std::remove_if(displays.begin(), displays.end(),
                                [](CGDirectDisplayID id) {
                                  return CGDisplayMirrorsDisplay(id) != kCGNullDirectDisplay;
                                }),
                 displays.end());
  return displays;
}

int ScreenCapturerSckImpl::Init(const int fps, cb_desktop_data cb) {
  if (!cb) {
    LOG_ERROR("Screen capturer callback is null");
    return -1;
  }

  _on_data = cb;
  fps_ = fps > 0 ? fps : 60;
  display_info_list_.clear();
  display_id_map_.clear();
  native_output_logged_ = false;

  if (@available(macOS 10.15, *)) {
    bool has_permission = CGPreflightScreenCaptureAccess();
    if (!has_permission) {
      LOG_ERROR("Screen recording permission not granted");
      return -1;
    }
  }

  dispatch_semaphore_t sema = dispatch_semaphore_create(0);
  __block SCShareableContent *content = nil;
  __block NSError *capture_error = nil;

  [SCShareableContent
      getShareableContentWithCompletionHandler:^(SCShareableContent *result, NSError *error) {
        if (error) {
          capture_error = error;
          LOG_ERROR("Failed to get shareable content: {}", NSErrorToString(error));
        } else {
          content = result;
        }
        dispatch_semaphore_signal(sema);
      }];
  const bool completed = dispatch_semaphore_wait(
      sema, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)) == 0;
  if (completed && [capture_error.domain isEqualToString:SCStreamErrorDomain] &&
      capture_error.code == SCStreamErrorUserDeclined) {
    return -1;
  }

  // Seed the retry cache with the startup enumeration while it is fresh.
  // Startup enumerations complete in ~100ms on an idle process; session-time
  // enumerations can take seconds, so a session starting shortly after launch
  // should never pay for another one. The same revision keying and validity
  // window apply, so privacy cover changes and expiry force re-enumeration
  // exactly as they do for capture-time seeding.
  if (completed && !capture_error && content) {
    std::lock_guard<std::mutex> lock(lock_);
    cached_content_ = content;
    cached_content_revision_ = GetMacPrivacyCaptureState().revision;
    cached_content_time_ = Clock::now();
  }

  std::vector<CGDirectDisplayID> display_ids;
  if (completed && !capture_error) {
    for (SCDisplay *display in content.displays)
      display_ids.push_back(display.displayID);
  }

  if (display_ids.empty()) {
    // Sleeping displays can disappear from ScreenCaptureKit before a peer
    // negotiates its streams. CoreGraphics can still enumerate online outputs.
    display_ids = GetOnlineCaptureDisplays();
    LOG_WARN("ScreenCaptureKit display enumeration unavailable; found {} online displays",
             display_ids.size());
  }
  if (display_ids.empty()) {
    // Reserve Display1 even when WindowServer exposes no physical display.
    // The first successful refresh binds this slot to its real ID and bounds.
    display_id_map_[0] = kCGNullDirectDisplay;
    display_info_list_.emplace_back(nullptr, MakeDisplayStreamId(0), true,
                                    0, 0, 0, 0);
  }
  for (CGDirectDisplayID display_id : display_ids) {
    CGRect bounds = CGDisplayBounds(display_id);
    bool is_primary = CGDisplayIsMain(display_id);

    std::string name = GetDisplayName(display_id);
    if (name.empty()) {
      name = MakeDisplayStreamId(display_info_list_.size());
    }

    LOG_INFO("macOS display discovered: index={}, display_id={}, name='{}'",
             display_info_list_.size(), display_id, name);

    DisplayInfo info((void *)(uintptr_t)display_id, name, is_primary,
                     static_cast<int>(bounds.origin.x), static_cast<int>(bounds.origin.y),
                     static_cast<int>(bounds.origin.x + bounds.size.width),
                     static_cast<int>(bounds.origin.y + bounds.size.height));
    display_info_list_.push_back(info);
    display_id_map_[display_info_list_.size() - 1] = display_id;
  }

  initial_monitor_index_ = 0;
  current_monitor_index_ = initial_monitor_index_;
  current_display_ = display_id_map_[current_monitor_index_];
  current_stream_id_ = MakeDisplayStreamId(current_monitor_index_);
  return 0;
}

int ScreenCapturerSckImpl::Start(bool show_cursor) {
  SckHelper* helper;
  uint64_t session;
  {
    std::lock_guard<std::mutex> lock(lock_);
    if (display_info_list_.empty() || !helper_) {
      LOG_ERROR("Cannot start capturer: display info not initialized");
      return -1;
    }
    if (capture_requested_) return 0;
    show_cursor_ = show_cursor;
    capture_requested_ = true;
    stream_running_ = received_frame_ = recovery_pending_ = false;
    recovery_attempt_ = 0;
    suspended_since_ = {};
    capture_attempt_started_ = {};
    enumeration_stall_logged_ = false;
    session = ++capture_session_;
    helper = helper_;
    invalid_stream_id_logged_ = false;
    native_output_logged_ = false;
  }
  [helper scheduleHealthCheck:session];
  StartOrReconfigureCapturer();
  return 0;
}

int ScreenCapturerSckImpl::SwitchTo(int monitor_index) {
  bool should_reconfigure = false;
  {
    std::lock_guard<std::mutex> lock(lock_);
    auto display_it = display_id_map_.find(monitor_index);
    if (display_it == display_id_map_.end() || monitor_index < 0 ||
        monitor_index >= static_cast<int>(display_info_list_.size())) {
      LOG_WARN("SwitchTo skipped, invalid monitor_index={}, displays={}",
               monitor_index, display_id_map_.size());
      return -1;
    }
    current_monitor_index_ = monitor_index;
    current_display_ = display_it->second;
    current_stream_id_ = MakeDisplayStreamId(monitor_index);
    should_reconfigure = capture_requested_;
  }
  if (should_reconfigure) {
    StartOrReconfigureCapturer();
  }
  return 0;
}

int ScreenCapturerSckImpl::ResetToInitialMonitor() {
  const int target = initial_monitor_index_;
  bool should_reconfigure = false;
  {
    std::lock_guard<std::mutex> lock(lock_);
    if (display_info_list_.empty()) return -1;
    auto display_it = display_id_map_.find(target);
    if (display_it == display_id_map_.end()) {
      LOG_WARN("ResetToInitialMonitor skipped, invalid monitor_index={}", target);
      return -1;
    }
    const CGDirectDisplayID target_display = display_it->second;
    if (current_display_ == target_display &&
        current_monitor_index_ == target) {
      return 0;
    }
    current_monitor_index_ = target;
    current_display_ = target_display;
    current_stream_id_ = MakeDisplayStreamId(target);
    should_reconfigure = capture_requested_ && stream_ != nil;
  }

  // Resetting session state must not create a capture stream. Preserve the
  // selected monitor for the next Start(), and only reconfigure an active one.
  if (should_reconfigure) {
    StartOrReconfigureCapturer();
  }
  return 0;
}

void ScreenCapturerSckImpl::SetTrueColorEnabled(bool enabled) {
  {
    std::lock_guard<std::mutex> lock(lock_);
    if (true_color_enabled_ == enabled) return;
    true_color_enabled_ = enabled;
    if (!capture_requested_) return;
  }
  StartOrReconfigureCapturer();
}

int ScreenCapturerSckImpl::Destroy() {
  Stop();
  SckHelper *helper_to_release = nil;
  {
    std::lock_guard<std::mutex> lock(lock_);
    current_display_ = 0;
    current_stream_id_.clear();
    _on_data = nullptr;
    helper_to_release = helper_;
    helper_ = nil;
  }

  [helper_to_release releaseCapturer];

  return 0;
}

int ScreenCapturerSckImpl::Stop() {
  std::lock_guard<std::mutex> lock(lock_);
  capture_requested_ = false;
  stream_running_ = received_frame_ = recovery_pending_ = false;
  recovery_attempt_ = 0;
  suspended_since_ = {};
  ++capture_session_;
  configuration_in_flight_ = reconfigure_pending_ = false;
  enumeration_in_flight_ = false;
  capture_attempt_started_ = {};
  applied_configuration_ = nil;
  ++configuration_generation_;
  if (stream_) {
    LOG_INFO("Stopping stream");
    [stream_ stopCaptureWithCompletionHandler:nil];
    stream_ = nil;
  }
  if (privacy_) privacy_->CaptureChanged(false);

  return 0;
}

void ScreenCapturerSckImpl::OnShareableContentCreated(SCShareableContent *content,
                                                     uint64_t generation, uint64_t revision) {
  {
    std::lock_guard<std::mutex> lock(lock_);
    // Cache even before the generation check: a completion that arrives after
    // its attempt was abandoned is useless for that generation, but its
    // content cost seconds to enumerate and remains perfectly valid input for
    // the retry. The revision key keeps stale-revision entries unusable, and
    // screen-parameter changes still invalidate explicitly.
    if (capture_requested_ && content && content != cached_content_) {
      // The timestamp is only refreshed for genuinely new content: reusing
      // the cached object must not extend its validity window, or retries
      // could keep it alive forever.
      cached_content_ = content;
      cached_content_revision_ = revision;
      cached_content_time_ = Clock::now();
    }
    if (!capture_requested_ || generation != configuration_generation_) {
      LOG_INFO("Ignoring stale ScreenCaptureKit display refresh after stop");
      return;
    }
    configuration_in_flight_ = false;
    enumeration_in_flight_ = false;
  }

  // The window list belongs to the privacy revision present when the request
  // was issued. A newer cover must be enumerated by a new request.
  if (GetMacPrivacyCaptureState().revision != revision) {
    StartOrReconfigureCapturer();
    return;
  }

  if (!content) {
    RecoverCapture(generation, "getShareableContent failed");
    return;
  }

  if (!content.displays || content.displays.count == 0) {
    RecoverCapture(generation, "getShareableContent returned no displays");
    return;
  }

  SCDisplay *captured_display = nil;
  bool show_cursor = false;
  {
    std::lock_guard<std::mutex> lock(lock_);
    if (!capture_requested_ || generation != configuration_generation_) return;

    int logical_index = current_monitor_index_;
    if (logical_index < 0 ||
        logical_index >= static_cast<int>(display_info_list_.size())) {
      logical_index = 0;
    }

    SCDisplay *logical_fallback = nil;
    int display_index = 0;
    for (SCDisplay *display in content.displays) {
      if (display_index == logical_index) logical_fallback = display;
      if (current_display_ != 0 && current_display_ == display.displayID) {
        captured_display = display;
        break;
      }
      ++display_index;
    }
    if (!captured_display) {
      captured_display = logical_fallback ? logical_fallback
                                          : content.displays.firstObject;
      if (!logical_fallback) logical_index = 0;
    }

    if (captured_display) {
      const CGDirectDisplayID old_display = current_display_;
      const CGDirectDisplayID new_display = captured_display.displayID;
      const std::string stable_stream_id = ResolveDisplayStreamId(
          current_stream_id_.c_str(), display_info_list_.size(), logical_index);
      if (stable_stream_id.empty()) {
        LOG_ERROR("Cannot map macOS display {} to a registered video stream",
                  new_display);
        return;
      }

      current_monitor_index_ = logical_index;
      current_display_ = new_display;
      current_stream_id_ = stable_stream_id;
      display_id_map_[logical_index] = new_display;

      CGRect bounds = CGDisplayBounds(new_display);
      auto& info = display_info_list_[logical_index];
      info.handle = (void *)(uintptr_t)new_display;
      info.is_primary = CGDisplayIsMain(new_display);
      info.left = static_cast<int>(bounds.origin.x);
      info.top = static_cast<int>(bounds.origin.y);
      info.right = static_cast<int>(bounds.origin.x + bounds.size.width);
      info.bottom = static_cast<int>(bounds.origin.y + bounds.size.height);
      info.width = info.right - info.left;
      info.height = info.bottom - info.top;
      const std::string refreshed_name = GetDisplayName(new_display);
      if (!refreshed_name.empty()) {
        info.name = refreshed_name;
      }

      if (old_display != new_display) {
        LOG_INFO("macOS display mapping refreshed: slot={}, old_id={}, "
                 "new_id={}, stream='{}'",
                 logical_index, old_display, new_display, stable_stream_id);
      }
    }
    show_cursor = show_cursor_;
  }

  if (!captured_display) {
    RecoverCapture(generation, "Failed to find valid display");
    return;
  }

  const auto privacy_capture = GetMacPrivacyCaptureState();
  // The cursor sprite is included only when this stream embeds the cursor.
  // Other controllers receive the unchanged cursor metadata separately.
  auto excluded_ids = privacy_capture.excluded_windows;
  if (!show_cursor && privacy_capture.cursor_window)
    excluded_ids.push_back(privacy_capture.cursor_window);
  NSMutableArray<SCWindow*>* excluded = [NSMutableArray array];
  for (SCWindow* window in content.windows) {
    if (std::find(excluded_ids.begin(), excluded_ids.end(), window.windowID) != excluded_ids.end())
      [excluded addObject:window];
  }
  // Newly registered transparent covers may need another enumeration. Never
  // acknowledge a partial exclusion or remove an active cover's filter.
  const bool privacy_filter_complete = excluded.count == excluded_ids.size();
  if (!privacy_filter_complete) {
    // This enumeration provably missed windows the privacy system expects.
    // The retries below exist to pick up covers WindowServer is still
    // registering, so they must re-enumerate: drop the cached entry instead
    // of letting them reuse this stale window list.
    std::lock_guard<std::mutex> lock(lock_);
    if (cached_content_ == content) cached_content_ = nil;
  }
  LOG_INFO("macOS privacy filter: generation={}, revision={}, requested={}, excluded={}",
           generation, privacy_capture.revision, excluded_ids.size(),
           excluded.count);
  if (!privacy_filter_complete) {
    bool have_stream;
    { std::lock_guard lock(lock_); have_stream = stream_ != nil; }
    if (privacy_capture.applied_revision == privacy_capture.revision) {
      if (privacy_) privacy_->Fail("ScreenCaptureKit could not enumerate every privacy cover");
      return; // Preserve the existing exclusion until recovery removes covers.
    }
    if (have_stream) {
      RetryPrivacyFilter();
      return; // Transparent covers are still being registered by WindowServer.
    }
    // Start ordinary capture even if a newly created, still-transparent cover
    // is not enumerable yet. Retry exclusion after start; never acknowledge it.
    [excluded removeAllObjects];
  }
  SCContentFilter *filter = [[SCContentFilter alloc] initWithDisplay:captured_display
                                                    excludingWindows:excluded];
  if (!filter) {
    RecoverCapture(generation, "Failed to create SCContentFilter");
    return;
  }

  SCStreamConfiguration *config = [[SCStreamConfiguration alloc] init];
  if (!config) {
    RecoverCapture(generation, "Failed to create SCStreamConfiguration");
    return;
  }

  // BT.709 is the exact matrix the sRGB desktop is rasterized in; tagging the
  // capture stream with ITU_R_601_4 forced ScreenCaptureKit to convert reds/
  // greens through a legacy SDTV matrix before encode, a visible color shift.
  config.colorMatrix = ResolveSckColorMatrix();
  config.colorSpaceName = kCGColorSpaceSRGB;
  config.showsCursor = show_cursor;
  config.width = filter.contentRect.size.width * filter.pointPixelScale;
  config.height = filter.contentRect.size.height * filter.pointPixelScale;
  config.captureResolution = SCCaptureResolutionAutomatic;
  config.minimumFrameInterval = CMTimeMake(1, fps_);
  // Bound capture-side queueing. macOS defaults to a queueDepth of 8, which
  // lets stale frames accumulate hidden capture lag whenever the downstream
  // pipeline briefly falls behind; 3 is the safe minimum given the encode
  // pool retention documented at ResolveSckQueueDepth. Include the depth in
  // the live-stream comparison below so an env change applies on restart.
  config.queueDepth = ResolveSckQueueDepth();

  std::unique_lock<std::mutex> lock(lock_);
  if (!capture_requested_ || generation != configuration_generation_) return;
  if (configuration_in_flight_) { reconfigure_pending_ = true; return; }
  // A pixel-format change takes the updateConfiguration branch on a live stream.
  // 420f (full-range) carries all 256 luma codes through capture; the legacy
  // 420v video-range choice crushed 0..255 into the 16..235 studio swing
  // (220 distinct levels) before the frame ever reached the encoder, which
  // quantizes dark-mode UI gradients into visible banding. VideoToolbox
  // accepts 420f natively and emits self-describing VUI (full_range=1,
  // BT.709) straight from the pixel buffer attachments; CopyCapturedPixel-
  // BufferToNv12 and the Metal renderer both read those same attachments.
  config.pixelFormat = true_color_enabled_ ? kCVPixelFormatType_444YpCbCr10BiPlanarFullRange
                                           : ResolveSck420PixelFormat();
  if (GetMacPrivacyCaptureState().revision != revision) {
    lock.unlock();
    RetryPrivacyFilter();
    return;
  }
  configuration_in_flight_ = true;
  // Arm the health clocks only now that the stream operation is actually
  // being issued. Content enumeration — which can cost seconds inside this
  // process — must never consume the stream-start budget.
  configuration_started_ = Clock::now();
  capture_attempt_started_ = Clock::now();

  if (stream_) {
    SckHelper* helper = helper_;
    SCStream* source = stream_;
    auto finished = ^(NSError* error) {
      [helper configurationFinished:source generation:generation revision:revision
                            complete:privacy_filter_complete configuration:config error:error];
    };
    // Apply configuration before its filter and acknowledge the final update.
    // Privacy-only toggles do not need a configuration change.
    const bool same_config = applied_configuration_ &&
        applied_configuration_.width == config.width &&
        applied_configuration_.height == config.height &&
        applied_configuration_.showsCursor == config.showsCursor &&
        applied_configuration_.pixelFormat == config.pixelFormat &&
        applied_configuration_.queueDepth == config.queueDepth &&
        CMTimeCompare(applied_configuration_.minimumFrameInterval, config.minimumFrameInterval) == 0;
    if (same_config) {
      [source updateContentFilter:filter completionHandler:finished];
    } else {
      [source updateConfiguration:config completionHandler:^(NSError* error) {
        if (error) finished(error);
        else [source updateContentFilter:filter completionHandler:finished];
      }];
    }
  } else {
    stream_ = [[SCStream alloc] initWithFilter:filter configuration:config delegate:helper_];

    // TODO: crbug.com/327458809 - Choose an appropriate sampleHandlerQueue for
    // best performance.
    NSError *add_stream_output_error = nil;
    dispatch_queue_t queue = dispatch_queue_create("ScreenCaptureKit.Queue", DISPATCH_QUEUE_SERIAL);
    bool add_stream_output_result = [stream_ addStreamOutput:helper_
                                                        type:SCStreamOutputTypeScreen
                                          sampleHandlerQueue:queue
                                                       error:&add_stream_output_error];

    if (!add_stream_output_result) {
      lock.unlock();
      RecoverCapture(generation, "addStreamOutput failed: " +
                                    NSErrorToString(add_stream_output_error));
      return;
    }

    SckHelper* helper = helper_;
    SCStream* source = stream_;
    auto handler = ^(NSError *error) {
      [helper configurationFinished:source generation:generation revision:revision
                            complete:privacy_filter_complete configuration:config error:error];
    };

    [stream_ startCaptureWithCompletionHandler:handler];
  }
}

void ScreenCapturerSckImpl::RetryPrivacyFilter() {
  SckHelper* helper;
  {
    std::lock_guard lock(lock_);
    if (!capture_requested_) return;
    helper = helper_;
  }
  // The retained helper safely ignores this callback after Destroy().
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 50 * NSEC_PER_MSEC),
                 dispatch_get_main_queue(), ^{ [helper privacyCoversChanged:nil]; });
}

void ScreenCapturerSckImpl::ConfigurationFinished(SCStream* source, uint64_t generation,
                                                  uint64_t revision, bool complete,
                                                  SCStreamConfiguration* configuration,
                                                  NSError* error) {
  bool refresh = false;
  {
    std::lock_guard lock(lock_);
    if (!capture_requested_ || source != stream_ ||
        generation != configuration_generation_) return;
    configuration_in_flight_ = false;
    const auto current = GetMacPrivacyCaptureState();
    refresh = reconfigure_pending_ || current.revision != revision;
    reconfigure_pending_ = false;
    if (!error) {
      stream_running_ = true;
      if (!received_frame_) stream_started_at_ = Clock::now();
      applied_configuration_ = configuration;
      if (privacy_) privacy_->CaptureChanged(true);
      if (complete && !refresh && generation == configuration_generation_)
        AcknowledgeMacPrivacyCapture(revision);
      refresh = refresh || !complete;
    }
    LOG_INFO("macOS privacy filter completed: generation={}, revision={}, complete={}, refresh={}, error={}",
             generation, revision, complete, refresh, NSErrorToString(error));
  }
  if (error) {
    RecoverCapture(generation, "ScreenCaptureKit configuration failed: " +
                                  NSErrorToString(error));
    return;
  }
  if (refresh) RetryPrivacyFilter();
}

void ScreenCapturerSckImpl::RecoverCapture(uint64_t generation,
                                          const std::string& reason,
                                          SCStream* stopped_stream, bool retry) {
  SCStream* old_stream;
  {
    std::lock_guard lock(lock_);
    // A stop invalidates the whole stream, including any newer configuration
    // request. Enumeration/operation failures only invalidate their generation.
    if (!capture_requested_ ||
        (stopped_stream ? stopped_stream != stream_
                        : generation != configuration_generation_)) return;
    old_stream = stream_;
    stream_ = nil;
    stream_running_ = received_frame_ = false;
    configuration_in_flight_ = reconfigure_pending_ = false;
    enumeration_in_flight_ = false;
    capture_attempt_started_ = {};
    stream_started_at_ = {};
    applied_configuration_ = nil;
    suspended_since_ = {};
    ++configuration_generation_; // Revoke pending completions and frames.
    recovery_pending_ = retry;
    if (retry) {
      const auto delay = std::chrono::seconds(
          std::min(1u << std::min(recovery_attempt_, 3u), 5u));
      recovery_attempt_ = std::min(recovery_attempt_ + 1, 4u);
      next_recovery_ = Clock::now() + delay;
      LOG_WARN("ScreenCaptureKit recovery: {}; retry in {} s", reason, delay.count());
    } else {
      capture_requested_ = false;
      ++capture_session_;
      LOG_INFO("ScreenCaptureKit capture ended: {}", reason);
    }
    // A replacement stream must never inherit an acknowledged privacy filter.
    if (privacy_) {
      privacy_->Fail(reason);
      privacy_->CaptureChanged(false);
    }
  }
  [old_stream stopCaptureWithCompletionHandler:nil];
}

bool ScreenCapturerSckImpl::CheckCaptureHealth(uint64_t session) {
  uint64_t generation;
  bool retry = false;
  const char* failure = nullptr;
  {
    std::lock_guard lock(lock_);
    if (!capture_requested_ || session != capture_session_) return false;
    generation = configuration_generation_;
    const auto now = Clock::now();
    if (recovery_pending_) {
      if (now >= next_recovery_) {
        recovery_pending_ = false;
        capture_attempt_started_ = {};
        retry = true;
      }
    } else if (enumeration_in_flight_) {
      // The enumeration is a distinct operation with its own budget. It must
      // not consume the stream-start budget, and a stalled enumeration must
      // not wedge recovery forever either: abandoning the attempt revokes
      // the late completion through the generation bump and the retry can
      // re-enumerate or reuse a cached result.
      if (now - enumeration_started_ >= kCaptureStartTimeout) {
        failure = "ScreenCaptureKit content enumeration timed out";
      } else if (!enumeration_stall_logged_ &&
                 now - enumeration_started_ >= std::chrono::seconds(1)) {
        enumeration_stall_logged_ = true;
        LOG_INFO("ScreenCaptureKit content enumeration still pending after 1 s");
      }
    } else if (configuration_in_flight_ &&
               now - configuration_started_ >= kCaptureStartTimeout) {
      failure = "ScreenCaptureKit configuration timed out";
    } else if (stream_running_ && !received_frame_ &&
               stream_started_at_ != Clock::time_point{} &&
               now - stream_started_at_ >= kFirstFrameTimeout) {
      failure = "ScreenCaptureKit stream produced no frames after starting";
    } else if ((!stream_running_ || !received_frame_) &&
               capture_attempt_started_ != Clock::time_point{} &&
               now - capture_attempt_started_ >= kCaptureStartTimeout) {
      // Catch-all for attempts that die without completing or entering
      // recovery (for example a filter-construction failure path): this keeps
      // the original whole-attempt budget. The short budget above only
      // applies once the stream start has actually completed.
      failure = "ScreenCaptureKit did not deliver its first frame";
    } else if (suspended_since_ != Clock::time_point{} &&
               now - suspended_since_ >= kCaptureStartTimeout) {
      failure = "ScreenCaptureKit frame updates remained suspended";
    }
  }
  if (failure) RecoverCapture(generation, failure);
  if (retry) StartOrReconfigureCapturer();
  return true;
}

void ScreenCapturerSckImpl::CaptureStopped(SCStream* source, NSError* error) {
  if (!source) return;
  // The system's explicit Stop Sharing action cancels capture until the next
  // connection. Only unexpected/system interruptions should restart it.
  const bool user_stopped = [error.domain isEqualToString:SCStreamErrorDomain] &&
      error.code == SCStreamErrorUserStopped;
  RecoverCapture(0, "ScreenCaptureKit stream stopped: " + NSErrorToString(error),
                 source, !user_stopped);
}

void ScreenCapturerSckImpl::CaptureSuspended(SCStream* source) {
  std::lock_guard lock(lock_);
  if (capture_requested_ && source == stream_ &&
      suspended_since_ == Clock::time_point{}) {
    suspended_since_ = Clock::now();
  }
}

void ScreenCapturerSckImpl::OnNewCVPixelBuffer(SCStream* source,
                                              CVPixelBufferRef pixelBuffer) {
  if (!pixelBuffer) {
    return;
  }

  const size_t width = CVPixelBufferGetWidth(pixelBuffer);
  const size_t height = CVPixelBufferGetHeight(pixelBuffer);
  const OSType pixel_format = CVPixelBufferGetPixelFormatType(pixelBuffer);
  const bool nv12 = IsMacNv12(pixel_format);
  constexpr size_t max_size = std::numeric_limits<int>::max();
  if (width == 0 || height == 0 || width > max_size || height > max_size ||
      (width & 1U) || (height & 1U) || !CVPixelBufferIsPlanar(pixelBuffer) ||
      CVPixelBufferGetPlaneCount(pixelBuffer) < 2 || (!nv12 && !IsMacYuv444(pixel_format))) {
    LOG_ERROR("Invalid CVPixelBuffer: width={}, height={}, format={}, planes={}",
              width, height, pixel_format,
              CVPixelBufferGetPlaneCount(pixelBuffer));
    return;
  }

  // MiniRtcVideoFrame::size is a logical packed NV12 extent, even for native
  // descriptors. Never use it to copy 4:4:4 storage: xf44/444v stay native.
  const size_t pixels = width * height;
  const size_t logical_size = pixels + pixels / 2;
  if (logical_size > max_size) {
    LOG_ERROR("Captured frame is too large: {}x{}", width, height);
    return;
  }

  cb_desktop_data callback;
  std::string stream_id;
  bool log_native_output = false;
  {
    std::lock_guard<std::mutex> lock(lock_);
    if (!_on_data || !capture_requested_ || source != stream_) {
      return;
    }
    stream_id = ResolveDisplayStreamId(current_stream_id_.c_str(),
                                       display_info_list_.size(),
                                       current_monitor_index_);
    if (stream_id.empty()) {
      if (!invalid_stream_id_logged_) {
        LOG_ERROR("Dropping macOS frames without a registered stream id, "
                  "display_id={}",
                  current_display_);
        invalid_stream_id_logged_ = true;
      }
      return;
    }
    invalid_stream_id_logged_ = false;
    received_frame_ = true;
    suspended_since_ = {};
    recovery_attempt_ = 0;
    callback = _on_data;
    if (!native_output_logged_) {
      native_output_logged_ = true;
      log_native_output = true;
    }
  }

  MiniRtcNativeVideoFrame native_frame{};
  native_frame.struct_size = sizeof(native_frame);
  native_frame.type = MiniRtcNativeVideoFrameCVPixelBuffer;
  native_frame.width = static_cast<uint32_t>(width);
  native_frame.height = static_cast<uint32_t>(height);
  native_frame.payload.cv_pixel_buffer = pixelBuffer;
  native_frame.owner = pixelBuffer;
  native_frame.retain = &RetainCapturedPixelBuffer;
  native_frame.release = &ReleaseCapturedPixelBuffer;
  native_frame.copy_to_nv12 = &CopyCapturedPixelBufferToNv12;
  if (log_native_output) {
    LOG_INFO("macOS ScreenCaptureKit native CVPixelBuffer output enabled");
  }
  callback(nullptr, static_cast<int>(logical_size), static_cast<int>(width),
           static_cast<int>(height), stream_id.c_str(), &native_frame);
}

void ScreenCapturerSckImpl::StartOrReconfigureCapturer() {
  SckHelper *local_helper;
  uint64_t generation;
  uint64_t revision;
  SCShareableContent* cached = nil;
  {
    std::lock_guard lock(lock_);
    if (!capture_requested_ || recovery_pending_) return;
    if (configuration_in_flight_) { reconfigure_pending_ = true; return; }
    // Include content enumeration in the in-flight operation so screen-change
    // notifications cannot keep superseding a slow request while locked.
    configuration_in_flight_ = true;
    // Arm both clocks at the start of the attempt. They are re-armed when the
    // stream operation is actually issued, so a slow enumeration never eats
    // the stream-start budget. While enumeration is in flight the health
    // check's enumeration branch takes precedence over these clocks; should
    // an attempt die without issuing a stream operation or entering recovery,
    // the attempt-start first-frame clock remains the catch-all that restarts
    // the capture.
    configuration_started_ = Clock::now();
    capture_attempt_started_ = Clock::now();
    reconfigure_pending_ = false;
    local_helper = helper_;
    generation = ++configuration_generation_;
    revision = GetMacPrivacyCaptureState().revision;
    // The window/display topology does not change meaningfully within seconds,
    // and one in-process enumeration can cost multiple seconds of synchronous
    // LaunchServices round trips. Reuse the immutable SCShareableContent for
    // retries inside the validity window. The entry is keyed on the privacy
    // revision, so any cover change still forces a fresh enumeration, exactly
    // as the exclusion logic requires. Screen-parameter notifications clear
    // the entry explicitly in CaptureConfigurationChanged().
    if (cached_content_ && cached_content_revision_ == revision &&
        Clock::now() - cached_content_time_ <= std::chrono::seconds(15)) {
      cached = cached_content_;
    } else {
      // The configuration/first-frame health clocks must measure the stream
      // operation, not the enumeration: they are armed in
      // OnShareableContentCreated when the stream operation is issued. The
      // enumeration itself is bounded by its own watchdog in
      // CheckCaptureHealth and must never eat the stream-start budget.
      enumeration_started_ = Clock::now();
      enumeration_in_flight_ = true;
    }
  }
  if (!local_helper) {
    LOG_ERROR("Cannot reconfigure capturer: helper is null");
    return;
  }

  if (!CGPreflightScreenCaptureAccess()) {
    RecoverCapture(generation, "Screen recording permission not granted");
    return;
  }

  if (cached) {
    // Dispatch through the completion queue like a real enumeration callback
    // so ordering and the capturer-lock deferral pattern stay identical.
    [local_helper onShareableContentCreated:cached generation:generation revision:revision];
    return;
  }

  const auto enumeration_requested = Clock::now();
  auto handler = ^(SCShareableContent *content, NSError *error) {
    const auto enumeration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now() - enumeration_requested).count();
    if (enumeration_ms >= 1000) {
      LOG_INFO("ScreenCaptureKit content enumeration took {} ms", enumeration_ms);
    }
    if (error) {
      LOG_ERROR("getShareableContent failed: {}", NSErrorToString(error));
      [local_helper onShareableContentCreated:nil generation:generation revision:revision];
      return;
    }
    [local_helper onShareableContentCreated:content generation:generation revision:revision];
  };
  [SCShareableContent getShareableContentExcludingDesktopWindows:NO
                                           onScreenWindowsOnly:NO
                                             completionHandler:handler];
}

@implementation SckHelper {
  // This lock is to prevent the capturer being destroyed while an instance
  // method is still running on another thread.
  std::mutex _capturer_lock;
  ScreenCapturerSckImpl *_capturer;
  dispatch_queue_t _completion_queue;
}

- (instancetype)initWithCapturer:(ScreenCapturerSckImpl *)capturer {
  self = [super init];
  if (self) {
    _capturer = capturer;
    _completion_queue = dispatch_queue_create("Farlink.ScreenCaptureKit.Completions",
                                              DISPATCH_QUEUE_SERIAL);
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(privacyCoversChanged:)
        name:@"CrossDeskPrivacyCoversChanged" object:nil];
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(screenParametersChanged:)
        name:NSApplicationDidChangeScreenParametersNotification object:nil];
  }
  return self;
}

- (void)privacyCoversChanged:(NSNotification*)notification {
  // Cover publications always bump the privacy revision, which by itself
  // invalidates the revision-keyed shareable-content cache. Avoid the
  // stronger screen-parameters invalidation here: a cover toggle must not
  // cost another multi-second enumeration beyond the required one.
  std::lock_guard<std::mutex> lock(_capturer_lock);
  if (_capturer) _capturer->PrivacyCoversChanged();
}
- (void)scheduleHealthCheck:(uint64_t)session {
  // Use the completion queue rather than the UI queue: capture recovery must
  // also progress while the app is in the background or the screen is locked.
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC),
                 _completion_queue, ^{
    std::lock_guard<std::mutex> lock(self->_capturer_lock);
    if (self->_capturer && self->_capturer->CheckCaptureHealth(session))
      [self scheduleHealthCheck:session];
  });
}
- (void)screenParametersChanged:(NSNotification*)notification {
  // Defer until the mode switch has completed, then fetch fresh SCDisplay
  // geometry and update the stream's pixel dimensions and input bounds.
  dispatch_async(_completion_queue, ^{
    std::lock_guard<std::mutex> lock(self->_capturer_lock);
    if (self->_capturer) self->_capturer->CaptureConfigurationChanged();
  });
}
- (void)configurationFinished:(SCStream*)stream generation:(uint64_t)generation
                     revision:(uint64_t)revision complete:(BOOL)complete
                configuration:(SCStreamConfiguration*)configuration error:(NSError*)error {
  // SCK may call a completion inline (notably updateContentFilter when nothing
  // changed). The caller can still hold both _capturer_lock and the capturer's
  // state lock. Always defer before acquiring either lock, including errors.
  // The block retains this helper; releaseCapturer revokes queued callbacks
  // before the C++ capturer is destroyed.
  dispatch_async(_completion_queue, ^{
    std::lock_guard<std::mutex> lock(self->_capturer_lock);
    if (self->_capturer)
      self->_capturer->ConfigurationFinished(stream, generation, revision, complete,
                                             configuration, error);
  });
}
- (void)onShareableContentCreated:(SCShareableContent *)content generation:(uint64_t)generation
                       revision:(uint64_t)revision {
  dispatch_async(_completion_queue, ^{
    std::lock_guard<std::mutex> lock(self->_capturer_lock);
    if (self->_capturer)
      self->_capturer->OnShareableContentCreated(content, generation, revision);
  });
}

- (void)stream:(SCStream *)stream
    didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
                   ofType:(SCStreamOutputType)type {
  if (type != SCStreamOutputTypeScreen || !CMSampleBufferIsValid(sampleBuffer)) return;
  CFArrayRef attachmentsArray = CMSampleBufferGetSampleAttachmentsArray(sampleBuffer, false);
  if (!attachmentsArray || CFArrayGetCount(attachmentsArray) == 0) {
    return;
  }

  CFDictionaryRef attachment =
      static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(attachmentsArray, 0));
  NSNumber* status = (__bridge NSNumber*)CFDictionaryGetValue(
      attachment, (__bridge CFStringRef)SCStreamFrameInfoStatus);
  if (!status) return;

  std::lock_guard<std::mutex> lock(_capturer_lock);
  if (!_capturer) return;
  // Status-only buffers may have no image. Observe suspension before asking
  // for a pixel buffer; ordinary idle frames must not trigger recovery.
  switch (status.integerValue) {
    case SCFrameStatusBlank:
    case SCFrameStatusSuspended:
    case SCFrameStatusStopped:
      _capturer->CaptureSuspended(stream);
      return;
    case SCFrameStatusComplete:
      break;
    default:
      return;
  }
  // The sample buffer owns its image for the duration of this callback.
  _capturer->OnNewCVPixelBuffer(stream, CMSampleBufferGetImageBuffer(sampleBuffer));
}

- (void)releaseCapturer {
  [NSNotificationCenter.defaultCenter removeObserver:self];
  std::lock_guard<std::mutex> lock(_capturer_lock);
  _capturer = nullptr;
}

- (void)stream:(SCStream*)stream didStopWithError:(NSError*)error {
  dispatch_async(_completion_queue, ^{
    std::lock_guard<std::mutex> lock(self->_capturer_lock);
    if (self->_capturer) self->_capturer->CaptureStopped(stream, error);
  });
}

@end

std::unique_ptr<ScreenCapturer> ScreenCapturerSck::CreateScreenCapturerSck() {
  return std::make_unique<ScreenCapturerSckImpl>();
}
