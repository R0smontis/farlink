#include "platform/macos/gui/metal_video_renderer.h"

#import <AppKit/AppKit.h>
#import <CoreVideo/CVMetalTextureCache.h>
#import <Metal/Metal.h>
#import <QuartzCore/CABase.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/CATransaction.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <utility>

#include "platform/machine_identity.h"
#include "platform/macos/video_pixel_format.h"
#include "platform/macos/gui/metal_cas_shader_source.h"
#include "platform/window_drag.h"
#include "rd_log.h"

// Input belongs to the Slint surface and the native window frame.
@interface CrossDeskDecorationView : NSView
@end

@implementation CrossDeskDecorationView
- (NSView*)hitTest:(NSPoint)point {
  return nil;
}
@end

@interface CrossDeskMetalVideoView : CrossDeskDecorationView
@property(nonatomic, readonly) CAMetalLayer* surface;
- (NSSize)updateViewport;
@end

@interface CrossDeskStreamTitleView : CrossDeskDecorationView
@property(nonatomic, copy) BOOL (^windowLayoutHandler)(NSWindow* window);
@property(nonatomic, copy) void (^surfaceInvalidationHandler)(void);
@property(nonatomic, copy) void (^resizeVideoHandler)(NSWindow*);
@property(nonatomic) BOOL windowLayoutPending;
- (instancetype)initWithWindow:(NSWindow*)window;
@end

@implementation CrossDeskMetalVideoView

- (instancetype)initWithFrame:(NSRect)frame {
  self = [super initWithFrame:frame];
  if (self) {
    self.wantsLayer = YES;
    self.layer.backgroundColor = NSColor.blackColor.CGColor;
    self.layer.masksToBounds = YES;
    _surface = [CAMetalLayer layer];
    _surface.contentsGravity = kCAGravityResizeAspect;
    [self.layer addSublayer:_surface];
  }
  return self;
}

- (void)layout {
  [super layout];
  [self updateViewport];
}

- (NSSize)updateViewport {
  const NSSize content = self.window.contentView.bounds.size;
  bool transitioning = false;
  const CGFloat inset = crossdesk::StreamWindowVideoTransitionInset(
      (__bridge void*)self, content.width, content.height, &transitioning);
  NSRect viewport = self.bounds;
  viewport.size.height = std::max<CGFloat>(1, NSHeight(viewport) - inset);
  if (!NSEqualRects(_surface.frame, viewport)) {
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    _surface.frame = viewport;
    [CATransaction commit];
  }
  // Use one raster size throughout the zoom, including when no new video arrives.
  return transitioning && self.window.screen ? self.window.screen.frame.size : viewport.size;
}

- (BOOL)isOpaque {
  return YES;
}

@end

@implementation CrossDeskStreamTitleView {
  NSTextField* _titleLabel;
  NSView* _titlebarBackground;
  __weak NSWindow* _streamWindow;
}

- (instancetype)initWithWindow:(NSWindow*)window {
  NSString* title = window.title.length > 0 ? window.title : [NSString stringWithUTF8String:crossdesk::platform::kMachineAppName];
  NSFont* font = [NSFont boldSystemFontOfSize:13.0];
  const CGFloat text_width =
      ceil([title sizeWithAttributes:@{NSFontAttributeName : font}].width);
  self = [super initWithFrame:NSMakeRect(0.0, 0.0, text_width + 10.0, 28.0)];
  if (self != nil) {
    _titleLabel = [NSTextField labelWithString:title];
    _titleLabel.font = font;
    _titleLabel.lineBreakMode = NSLineBreakByClipping;
    _titleLabel.maximumNumberOfLines = 1;
    [self addSubview:_titleLabel];

    // Keep the white fill inside the titlebar. AppKit can expose the window
    // backing behind the floating controls while it composites two Space sizes.
    NSView* content = window.contentView;
    NSView* frame = content.superview;
    _titlebarBackground = [[CrossDeskDecorationView alloc] initWithFrame:NSZeroRect];
    _titlebarBackground.wantsLayer = YES;
    _titlebarBackground.layer.backgroundColor = NSColor.whiteColor.CGColor;
    _titlebarBackground.translatesAutoresizingMaskIntoConstraints = NO;
    _titlebarBackground.alphaValue =
        (window.styleMask & NSWindowStyleMaskFullScreen) ? 0 : 1;
    [frame addSubview:_titlebarBackground positioned:NSWindowBelow relativeTo:nil];
    [NSLayoutConstraint activateConstraints:@[
        [_titlebarBackground.leadingAnchor constraintEqualToAnchor:frame.leadingAnchor],
        [_titlebarBackground.trailingAnchor constraintEqualToAnchor:frame.trailingAnchor],
        [_titlebarBackground.topAnchor constraintEqualToAnchor:frame.topAnchor],
        [_titlebarBackground.bottomAnchor constraintEqualToAnchor:content.topAnchor],
    ]];

    // AppKit can move the title accessory to a toolbar window in fullscreen.
    // Keep observing the window that owns the video surface.
    _streamWindow = window;
    self.windowLayoutPending = YES;
    NSNotificationCenter* center = NSNotificationCenter.defaultCenter;
    [center addObserver:self selector:@selector(windowActivationChanged:)
                   name:NSWindowDidBecomeKeyNotification object:window];
    [center addObserver:self selector:@selector(windowActivationChanged:)
                   name:NSWindowDidResignKeyNotification object:window];
    [center addObserver:self selector:@selector(windowDidResize:)
                   name:NSWindowDidResizeNotification object:window];
    [center addObserver:self selector:@selector(windowDidUpdate:)
                   name:NSWindowDidUpdateNotification object:window];
    [center addObserver:self selector:@selector(windowBackingPropertiesChanged:)
                   name:NSWindowDidChangeBackingPropertiesNotification object:window];
    [center addObserver:self selector:@selector(windowDidToggleFullscreen:)
                   name:NSWindowDidEnterFullScreenNotification object:window];
    [center addObserver:self selector:@selector(windowWillExitFullscreen:)
                   name:NSWindowWillExitFullScreenNotification object:window];
    [center addObserver:self selector:@selector(windowDidToggleFullscreen:)
                   name:NSWindowDidExitFullScreenNotification object:window];
  }
  return self;
}

- (BOOL)applyWindowLayout {
  NSWindow* window = _streamWindow;
  return window && self.windowLayoutHandler && self.windowLayoutHandler(window);
}

- (void)viewDidMoveToWindow {
  [super viewDidMoveToWindow];
  self.windowLayoutPending = YES;
  [self invalidateVideoSurface];
  [self updateTitleColor];
}

- (void)viewDidChangeEffectiveAppearance {
  [super viewDidChangeEffectiveAppearance];
  [self updateTitleColor];
  [self invalidateVideoSurface];
  self.windowLayoutPending = ![self applyWindowLayout];
}

- (void)windowActivationChanged:(NSNotification*)notification {
  (void)notification;
  [self updateTitleColor];
}

- (void)windowDidResize:(NSNotification*)notification {
  if (self.resizeVideoHandler) self.resizeVideoHandler(notification.object);
  // The compositor scales the existing image during the Space animation.
  // The titlebar and cached frame only need refreshing at its final size.
  if (crossdesk::IsStreamWindowFullscreenTransitioning(
          (__bridge void*)_streamWindow.contentView)) return;
  [self invalidateVideoSurface];
  self.windowLayoutPending = YES;
  [self applyWindowLayout];
}

- (void)windowWillExitFullscreen:(NSNotification*)notification {
  // Restore the titlebar fill as the control window shrinks into place.
  _titlebarBackground.alphaValue = 0;
  [NSAnimationContext runAnimationGroup:^(NSAnimationContext* context) {
    context.duration = 0.16;
    _titlebarBackground.animator.alphaValue = 1;
  } completionHandler:nil];
}

- (void)windowDidToggleFullscreen:(NSNotification*)notification {
  _titlebarBackground.alphaValue =
      (_streamWindow.styleMask & NSWindowStyleMaskFullScreen) ? 0 : 1;
  [self invalidateVideoSurface];
  self.windowLayoutPending = ![self applyWindowLayout];
}

- (void)windowBackingPropertiesChanged:(NSNotification*)notification {
  (void)notification;
  [self invalidateVideoSurface];
}

- (void)windowDidUpdate:(NSNotification*)notification {
  NSString *title = _streamWindow.title;
  if (title && ![_titleLabel.stringValue isEqualToString:title]) {
    _titleLabel.stringValue = title;
    [self setFrameSize:NSMakeSize(_titleLabel.fittingSize.width + 10, self.frame.size.height)];
    self.needsLayout = YES;
  }
  if (!self.windowLayoutPending) return;
  self.windowLayoutPending = ![self applyWindowLayout];
  [self invalidateVideoSurface];
}

- (void)invalidateVideoSurface {
  if (self.surfaceInvalidationHandler != nil) {
    self.surfaceInvalidationHandler();
  }
}

- (void)updateTitleColor {
  // When the fullscreen titlebar is revealed, AppKit hosts the accessory
  // in a separate window with the system toolbar appearance.
  if (self.window && self.window != _streamWindow) {
    _titleLabel.textColor = NSColor.labelColor;
  } else {
    _titleLabel.textColor = _streamWindow.isKeyWindow
                                ? NSColor.blackColor
                                : [NSColor colorWithWhite:0.45 alpha:1.0];
  }
}

- (void)layout {
  [super layout];
  const NSSize title_size = _titleLabel.fittingSize;
  _titleLabel.frame = NSMakeRect(
      8.0, floor((self.bounds.size.height - title_size.height) * 0.5),
      title_size.width, title_size.height);
}

- (BOOL)mouseDownCanMoveWindow {
  return YES;
}

- (void)dealloc {
  [[NSNotificationCenter defaultCenter] removeObserver:self];
  [_titlebarBackground removeFromSuperview];
}

@end

namespace crossdesk {
namespace {

constexpr size_t kFrameSlotCount = 3;

class NativeVideoFrameRef {
 public:
  NativeVideoFrameRef() = default;

  explicit NativeVideoFrameRef(const MiniRtcNativeVideoFrame* frame) {
    if (frame && frame->owner && frame->retain && frame->release) {
      frame_ = *frame;
      frame_.struct_size = sizeof(frame_);
      frame_.retain(frame_.owner);
      valid_ = true;
    }
  }

  NativeVideoFrameRef(const NativeVideoFrameRef& other)
      : NativeVideoFrameRef(other.Get()) {}

  NativeVideoFrameRef(NativeVideoFrameRef&& other) noexcept
      : frame_(other.frame_), valid_(std::exchange(other.valid_, false)) {
    other.frame_ = {};
  }

  NativeVideoFrameRef& operator=(NativeVideoFrameRef other) noexcept {
    Swap(other);
    return *this;
  }

  ~NativeVideoFrameRef() { Reset(); }

  void Reset() {
    if (valid_ && frame_.owner && frame_.release) {
      frame_.release(frame_.owner);
    }
    frame_ = {};
    valid_ = false;
  }

  void Swap(NativeVideoFrameRef& other) noexcept {
    std::swap(frame_, other.frame_);
    std::swap(valid_, other.valid_);
  }

  const MiniRtcNativeVideoFrame* Get() const {
    return valid_ ? &frame_ : nullptr;
  }
  explicit operator bool() const { return valid_; }

 private:
  MiniRtcNativeVideoFrame frame_{};
  bool valid_ = false;
};

const MiniRtcNativeVideoFrame* GetMacNativeFrame(
    const MiniRtcNativeVideoFrame& frame) {
  const MiniRtcNativeVideoFrame* native = &frame;
  if (native->struct_size < static_cast<uint32_t>(sizeof(*native)) ||
      native->type != MiniRtcNativeVideoFrameCVPixelBuffer ||
      !native->payload.cv_pixel_buffer || !native->owner || !native->retain ||
      !native->release || !native->copy_to_nv12 || native->width == 0 ||
      native->height == 0 || (native->width & 1U) != 0 ||
      (native->height & 1U) != 0 ||
      native->width > static_cast<uint32_t>(std::numeric_limits<int>::max()) ||
      native->height >
          static_cast<uint32_t>(std::numeric_limits<int>::max())) {
    return nullptr;
  }
  return native;
}

// Slint's winit backend creates AppKit windows with transparent backing so
// transparent Slint items can be composited. Keep the content out of the
// titlebar. The stream uses a black backing under its video and titlebar views.
bool ConfigureNativeWindowChrome(NSWindow* window,
                                 BOOL titlebar_transparent) {
  if (window == nil ||
      (window.styleMask & NSWindowStyleMaskTitled) == 0) {
    return false;
  }

  bool layout_changed = false;
  if ((window.styleMask & NSWindowStyleMaskFullSizeContentView) != 0) {
    window.styleMask &= ~NSWindowStyleMaskFullSizeContentView;
    layout_changed = true;
  }
  if (window.titlebarAppearsTransparent != titlebar_transparent) {
    window.titlebarAppearsTransparent = titlebar_transparent;
    layout_changed = true;
  }
  window.opaque = YES;
  window.backgroundColor =
      titlebar_transparent ? NSColor.blackColor : NSColor.windowBackgroundColor;

  // Removing FullSizeContentView changes the titlebar's layout guide. Commit
  // the public titlebar-accessory layout before the next frame is presented.
  if (layout_changed) {
    NSView* frame_view = window.contentView.superview;
    frame_view.needsLayout = YES;
    [frame_view layoutSubtreeIfNeeded];
  }
  return (window.styleMask & NSWindowStyleMaskFullSizeContentView) == 0 &&
         window.titlebarAppearsTransparent == titlebar_transparent &&
         window.opaque;
}

bool RestoreNativeWindowChrome(NSWindow* window) {
  return ConfigureNativeWindowChrome(window, NO);
}

bool ConfigureStreamWindowChrome(NSWindow* window) {
  if (window.styleMask & NSWindowStyleMaskFullScreen) {
    window.backgroundColor = NSColor.blackColor;
    return true;
  }
  return ConfigureNativeWindowChrome(window, YES);
}

size_t AlignUp(size_t value, size_t alignment) {
  if (alignment <= 1) {
    return value;
  }
  return ((value + alignment - 1) / alignment) * alignment;
}

// Parses FARLINK_METAL_SHARPEN: unset/empty means the automatic gate
// (sharpen only when scaling or chroma-subsampling softens the picture),
// "0" forces plain bilinear, any other value in (0, 1] forces that strength.
// Returns a negative number when automatic gating applies.
float EnvSharpenOverride() {
  static const float override_strength = [] {
    const char* env = std::getenv("FARLINK_METAL_SHARPEN");
    if (!env || env[0] == '\0') return -1.0f;
    char* end = nullptr;
    const float parsed = std::strtof(env, &end);
    if (end == env || *end != '\0' || !(parsed >= 0.0f)) return -1.0f;
    return std::min(parsed, 1.0f);
  }();
  return override_strength;
}

// Automatic contrast-adaptive sharpening strength. Upscaling and 4:2:0
// chroma subsampling both soften one-pixel strokes and subpixel text
// antialiasing; an exact pixel-1:1 lossless 4:4:4 mapping has nothing to
// restore and stays pure bilinear so flat regions remain bit-exact.
float ResolveSharpenStrength(double raster_width, double raster_height,
                             int source_width, int source_height,
                             bool subsampled_chroma) {
  const float env = EnvSharpenOverride();
  if (env >= 0.0f) return env;
  if (source_width <= 0 || source_height <= 0) return 0.0f;
  const bool scaled = std::abs(raster_width - source_width) > 0.5 ||
                      std::abs(raster_height - source_height) > 0.5;
  if (scaled) return 0.45f;
  return subsampled_chroma ? 0.20f : 0.0f;
}

bool EnvSyncPresentEnabled() {
  static const bool enabled = [] {
    const char* env = std::getenv("FARLINK_METAL_SYNC_PRESENT");
    return env && env[0] == '1';
  }();
  return enabled;
}

enum class SlotUse {
  available,
  pending,
  in_flight,
};

struct Nv12Color {
  uint32_t full_range = 0;
  uint32_t bt709 = 0;
  float uv_center = 128.0f / 255.0f;
  // Mirrors the shader struct exactly. Zero reproduces plain bilinear
  // sampling; the render path derives the strength from scaling and
  // subsampling (see ResolveSharpenStrength in DrawFrame).
  float sharpness = 0.0f;
};

struct FrameSlot {
  Nv12Color color;
  id<MTLBuffer> y_buffer = nil;
  id<MTLBuffer> uv_buffer = nil;
  id<MTLTexture> y_texture = nil;
  id<MTLTexture> uv_texture = nil;
  CVMetalTextureRef y_cv_texture = nullptr;
  CVMetalTextureRef uv_cv_texture = nullptr;
  NativeVideoFrameRef native_frame;
  size_t y_stride = 0;
  size_t uv_stride = 0;
  int width = 0;
  int height = 0;
  std::string remote_id;
  uint64_t sequence = 0;
  VideoLatencyFrame timing;
  SlotUse use = SlotUse::available;
  bool valid = false;

  // 4:2:0 chroma subsampling softens text even at 1:1; 4:4:4 lossless does
  // not. Drives the automatic sharpening gate.
  bool subsampled_chroma = true;

  void ReleaseResources() {
    y_texture = nil;
    uv_texture = nil;
    if (y_cv_texture) {
      CFRelease(y_cv_texture);
      y_cv_texture = nullptr;
    }
    if (uv_cv_texture) {
      CFRelease(uv_cv_texture);
      uv_cv_texture = nullptr;
    }
    native_frame.Reset();
    color = {};
    y_buffer = nil;
    uv_buffer = nil;
    y_stride = 0;
    uv_stride = 0;
    width = 0;
    height = 0;
    valid = false;
  }

  ~FrameSlot() { ReleaseResources(); }
};

struct SharedFrameState {
  mutable std::mutex mutex;
  std::array<FrameSlot, kFrameSlotCount> slots;
  std::string selected_stream;
  uint64_t next_sequence = 1;
  uint64_t content_generation = 1;
  // Last stream/content generation actually committed to the layer. Guarded
  // by mutex because both the main thread (transition redraws) and the
  // render queue (steady-state presentation) update them.
  std::string rendered_stream;
  uint64_t rendered_content_generation = 0;
};

bool AllocateSlot(id<MTLDevice> device, FrameSlot& slot, int width,
                  int height) {
  if (slot.y_buffer != nil && slot.uv_buffer != nil &&
      slot.y_texture != nil && slot.uv_texture != nil &&
      slot.width == width && slot.height == height) {
    return true;
  }

  slot.ReleaseResources();

  const size_t y_alignment = std::max<size_t>(
      1, [device minimumLinearTextureAlignmentForPixelFormat:MTLPixelFormatR8Unorm]);
  const size_t uv_alignment = std::max<size_t>(
      1, [device minimumLinearTextureAlignmentForPixelFormat:MTLPixelFormatRG8Unorm]);
  slot.y_stride = AlignUp(static_cast<size_t>(width), y_alignment);
  slot.uv_stride = AlignUp(static_cast<size_t>(width), uv_alignment);

  const size_t y_length = slot.y_stride * static_cast<size_t>(height);
  const size_t uv_length =
      slot.uv_stride * static_cast<size_t>(height / 2);
  const MTLResourceOptions options =
      MTLResourceStorageModeShared | MTLResourceCPUCacheModeWriteCombined;
  slot.y_buffer = [device newBufferWithLength:y_length options:options];
  slot.uv_buffer = [device newBufferWithLength:uv_length options:options];
  if (slot.y_buffer == nil || slot.uv_buffer == nil) {
    slot.ReleaseResources();
    return false;
  }

  MTLTextureDescriptor* y_descriptor =
      [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm
                                                         width:width
                                                        height:height
                                                     mipmapped:NO];
  y_descriptor.storageMode = MTLStorageModeShared;
  y_descriptor.usage = MTLTextureUsageShaderRead;
  slot.y_texture = [slot.y_buffer newTextureWithDescriptor:y_descriptor
                                                   offset:0
                                              bytesPerRow:slot.y_stride];

  MTLTextureDescriptor* uv_descriptor =
      [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRG8Unorm
                                                         width:width / 2
                                                        height:height / 2
                                                     mipmapped:NO];
  uv_descriptor.storageMode = MTLStorageModeShared;
  uv_descriptor.usage = MTLTextureUsageShaderRead;
  slot.uv_texture = [slot.uv_buffer newTextureWithDescriptor:uv_descriptor
                                                     offset:0
                                                bytesPerRow:slot.uv_stride];
  if (slot.y_texture == nil || slot.uv_texture == nil) {
    slot.ReleaseResources();
    return false;
  }

  slot.width = width;
  slot.height = height;
  return true;
}

// The exact shader source compiled by the production renderer lives in
// metal_cas_shader_source.h so the offscreen GPU verification test
// (apps/desktop/tests/metal_cas_renderer_test.mm) compiles the same text the
// app runs instead of a copy.
}  // namespace

struct MacMetalVideoRenderer::Impl {
  id<MTLDevice> device = nil;
  id<MTLCommandQueue> command_queue = nil;
  id<MTLRenderPipelineState> pipeline = nil;
  CVMetalTextureCacheRef texture_cache = nullptr;
  std::shared_ptr<SharedFrameState> frames =
      std::make_shared<SharedFrameState>();
  NSView* slint_view = nil;
  CrossDeskMetalVideoView* video_view = nil;
  NSLayoutConstraint* video_top = nil;
  NSTitlebarAccessoryViewController* title_accessory_controller = nil;
  CAMetalLayer* metal_layer = nil;
  struct DrawableRequest {
    id<CAMetalDrawable> drawable = nil;
    double wait_ms = 0;
    bool complete = false;
  };
  std::shared_ptr<DrawableRequest> drawable_request;
  std::function<void()> drawable_ready;
  std::atomic<bool> attached{false};
  std::shared_ptr<std::atomic<bool>> native_surface_needs_redraw =
      std::make_shared<std::atomic<bool>>(true);
  std::atomic<bool> native_path_logged{false};
  std::atomic<bool> native_fallback_logged{false};

  // Everything an off-main-thread presentation needs, in one refcounted
  // object. Render-queue blocks capture this shared_ptr instead of touching
  // Impl/AppKit, so teardown never has to drain the queue synchronously: a
  // block that runs after detach simply observes attached==false and exits.
  // CAMetalLayer is retained by the capture, so even a detached layer cannot
  // dangle; nextDrawable on it either returns nil or renders into a surface
  // WindowServer no longer composites, which is harmless.
  struct AsyncRenderState {
    id<MTLCommandQueue> command_queue = nil;
    id<MTLRenderPipelineState> pipeline = nil;
    // Written once on the main thread during Attach before attached flips
    // true; read-only afterwards. Retained by the async block capture.
    CAMetalLayer* __strong metal_layer = nil;
    std::shared_ptr<SharedFrameState> frames;
    std::shared_ptr<std::atomic<bool>> native_surface_needs_redraw;
    std::shared_ptr<std::atomic<bool>> detached =
        std::make_shared<std::atomic<bool>>(false);
    std::atomic<double> raster_width{0};
    std::atomic<double> raster_height{0};
    // Main-thread-cached AppKit facts the render queue must never read
    // directly. visibility gates the latency statistics exactly like the
    // synchronous path's window reads did; transitioning routes presentation
    // through the legacy main-thread drawable handoff while a fullscreen
    // Space animation holds both drawables.
    std::atomic<bool> visible{false};
    std::atomic<bool> transitioning{false};
    // Coalescing flag for queued presentations on this attachment. Lives in
    // the shared state because render-queue blocks must never touch Impl
    // members after teardown: the blocks capture this state object only.
    std::atomic<bool> render_queued{false};
    // Latest presented-frame bookkeeping harvested by the UI thread so
    // RenderOutcome-driven accounting (resolution label, new-frame count)
    // stays exact even though presentation no longer runs on it. The
    // latency statistics themselves (frames_presented, inter-present, the
    // "First frame presented" line) are recorded inside the command-buffer
    // completion and drawable presented handlers and need no harvesting.
    mutable std::mutex outcome_mutex;
    uint64_t outcome_sequence = 0;
    int outcome_width = 0;
    int outcome_height = 0;
    uint64_t outcome_count = 0;
  };

  // Dedicated user-interactive serial queue owning every steady-state frame
  // presentation. CAMetalLayer.nextDrawable blocks while both drawables are
  // in flight in WindowServer; on the Slint main thread that stall froze
  // input delivery and UI animations for up to a full display interval.
  // Apple documents nextDrawable and MTLCommandBuffer encoding as thread-
  // safe off the main thread, so the blocking wait lives here while AppKit
  // geometry reads stay on the main thread. render_queued coalesces
  // requests: the newest decoded frame is selected after nextDrawable
  // returns, never a queued copy.
  dispatch_queue_t render_queue = nullptr;
  // Guards async_state replacement at attach/detach. Readers on the decode
  // threads copy the shared_ptr under this mutex; the state object itself
  // is immutable once published.
  mutable std::mutex async_state_mutex;
  std::shared_ptr<AsyncRenderState> async_state =
      std::make_shared<AsyncRenderState>();
  // Set by PublishAsyncGeometry on the main thread once the layer geometry
  // is known to match the window; cleared on detach. Guards the direct
  // decode-thread dispatch so the queue never presents into a stale size.
  std::atomic<bool> geometry_valid{false};

  std::shared_ptr<AsyncRenderState> GetAsyncState() const {
    std::lock_guard lock(async_state_mutex);
    return async_state;
  }
  // Last async-render outcome count the UI thread has already reported
  // through RenderLatest. Main thread only; compared against the mutex-
  // guarded outcome_count inside the async state.
  uint64_t harvested_outcome_count = 0;

  Impl() {
    @autoreleasepool {
      render_queue = dispatch_queue_create("CrossDesk.MetalVideoRender",
                                           DISPATCH_QUEUE_SERIAL);
      dispatch_set_target_queue(
          render_queue,
          dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0));
      device = MTLCreateSystemDefaultDevice();
      if (device == nil) {
        LOG_ERROR("Metal video renderer unavailable: no Metal device");
        return;
      }
      command_queue = [device newCommandQueue];
      if (command_queue == nil) {
        LOG_ERROR("Metal video renderer unavailable: command queue creation failed");
        return;
      }
      const CVReturn cache_status = CVMetalTextureCacheCreate(
          kCFAllocatorDefault, nullptr, device, nullptr, &texture_cache);
      if (cache_status != kCVReturnSuccess || !texture_cache) {
        LOG_WARN("Metal video renderer could not create CV texture cache: {}",
                 cache_status);
      }

      NSError* library_error = nil;
      id<MTLLibrary> library =
          [device newLibraryWithSource:ProductionNv12ShaderSource()
                               options:nil
                                 error:&library_error];
      if (library == nil) {
        const char* message = library_error.localizedDescription.UTF8String;
        LOG_ERROR("Metal NV12 shader compilation failed: {}",
                  message ? message : "unknown error");
        return;
      }

      id<MTLFunction> vertex = [library newFunctionWithName:@"nv12_vertex"];
      id<MTLFunction> fragment =
          [library newFunctionWithName:@"nv12_fragment"];
      MTLRenderPipelineDescriptor* descriptor =
          [[MTLRenderPipelineDescriptor alloc] init];
      descriptor.vertexFunction = vertex;
      descriptor.fragmentFunction = fragment;
      descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;

      NSError* pipeline_error = nil;
      pipeline = [device newRenderPipelineStateWithDescriptor:descriptor
                                                        error:&pipeline_error];
      if (pipeline == nil) {
        const char* message = pipeline_error.localizedDescription.UTF8String;
        LOG_ERROR("Metal NV12 pipeline creation failed: {}",
                  message ? message : "unknown error");
      }
      async_state->command_queue = command_queue;
      async_state->pipeline = pipeline;
      async_state->frames = frames;
      async_state->native_surface_needs_redraw = native_surface_needs_redraw;
    }
  }

  ~Impl() {
    DetachViews();
    // Mark the shared async state detached so any render-queue block still
    // waiting inside nextDrawable exits immediately after it returns instead
    // of committing into a torn-down surface. The state object itself is
    // captured by those blocks, so resetting it here cannot free memory the
    // queue still uses.
    async_state->detached->store(true, std::memory_order_release);
    frames.reset();
    if (texture_cache) {
      CVMetalTextureCacheFlush(texture_cache, 0);
      CFRelease(texture_cache);
      texture_cache = nullptr;
    }
  }

  bool Ready() const {
    return device != nil && command_queue != nil && pipeline != nil;
  }

  // WindowServer can hold both drawables during a fullscreen Space change.
  // Keep the window thread free to animate and leave the latest frame queued.
  id<CAMetalDrawable> TakeTransitionDrawable(double& wait_ms) {
    if (drawable_request) {
      if (!drawable_request->complete) return nil;
      auto ready = std::move(drawable_request);
      id<CAMetalDrawable> drawable = ready->drawable;
      const CGSize size = metal_layer.drawableSize;
      if (drawable && drawable.texture.width == size.width &&
          drawable.texture.height == size.height) {
        wait_ms = ready->wait_ms;
        return drawable;
      }
    }
    drawable_request = std::make_shared<DrawableRequest>();
    std::weak_ptr<DrawableRequest> weak_request = drawable_request;
    CAMetalLayer* layer = metal_layer;
    const auto notify = drawable_ready;
    const auto dirty = native_surface_needs_redraw;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), ^{
      @autoreleasepool {
        const auto start = VideoLatencyFrame::Clock::now();
        id<CAMetalDrawable> drawable = [layer nextDrawable];
        const double elapsed = std::chrono::duration<double, std::milli>(
            VideoLatencyFrame::Clock::now() - start).count();
        dispatch_async(dispatch_get_main_queue(), ^{
          // Detaching cancels this request, including a queued UI wakeup.
          if (auto request = weak_request.lock()) {
            request->drawable = drawable;
            request->wait_ms = elapsed;
            request->complete = true;
            dirty->store(true, std::memory_order_release);
            if (drawable && notify) notify();
          }
        });
      }
    });
    return nil;
  }

  // Main-thread-only teardown. The shared async state is marked detached
  // first, so a render-queue block already waiting inside nextDrawable exits
  // as soon as the call returns instead of committing into a torn-down
  // surface. The queue is never drained synchronously: a drain could
  // deadlock if a queue block is inside nextDrawable waiting on WindowServer
  // while the main thread holds window state WindowServer needs. Blocks
  // capture the state object, so replacing it here cannot free memory the
  // queue still uses. In-flight frame slots release through their command-
  // buffer completion handlers.
  void DetachViews() {
    @autoreleasepool {
      attached.store(false, std::memory_order_release);
      geometry_valid.store(false, std::memory_order_release);
      // The state object is immutable once published by Attach, so blocks
      // running on the render queue never race with teardown writes here:
      // they only read the detached flag. A re-attach builds a fresh state.
      async_state->detached->store(true, std::memory_order_release);
      drawable_request.reset();
      NSWindow* window = slint_view.window;
      ((CrossDeskStreamTitleView*)title_accessory_controller.view).resizeVideoHandler = nil;
      if (video_view != nil) {
        [video_view removeFromSuperview];
      }
      if (title_accessory_controller != nil && window != nil) {
        const NSUInteger index = [window.titlebarAccessoryViewControllers
            indexOfObjectIdenticalTo:title_accessory_controller];
        if (index != NSNotFound) {
          [window removeTitlebarAccessoryViewControllerAtIndex:index];
        }
      }
      title_accessory_controller = nil;
      if (window != nil && !IsStreamWindowFullscreen((__bridge void *)slint_view)) {
        RestoreNativeWindowChrome(window);
        window.titleVisibility = NSWindowTitleVisible;
        NSView* frame_view = window.contentView.superview;
        frame_view.needsLayout = YES;
        [frame_view layoutSubtreeIfNeeded];
      }
      video_top = nil;
      video_view = nil;
      slint_view = nil;
      metal_layer = nil;
      {
        std::lock_guard lock(frames->mutex);
        frames->rendered_stream.clear();
        frames->rendered_content_generation = 0;
      }
      native_surface_needs_redraw->store(true, std::memory_order_release);
    }
  }


  void RestoreVideoViewOrder() {
    if (slint_view == nil || video_view == nil) {
      return;
    }
    NSView* parent = slint_view.superview;
    if (parent == nil || video_view.superview != parent) {
      return;
    }

    NSArray<NSView*>* siblings = parent.subviews;
    const NSUInteger video_index = [siblings indexOfObjectIdenticalTo:video_view];
    const NSUInteger slint_index = [siblings indexOfObjectIdenticalTo:slint_view];
    if (video_index != NSNotFound && slint_index != NSNotFound &&
        video_index + 1 == slint_index) {
      return;
    }

    // AppKit may insert an appearance material view between these siblings
    // while switching themes. Keep the video immediately below Slint so that
    // native window chrome stays below the video instead of covering it.
    [parent addSubview:video_view
             positioned:NSWindowBelow
             relativeTo:slint_view];
    native_surface_needs_redraw->store(true, std::memory_order_release);
  }

  CGSize UpdateVideoGeometry(double top_inset_points) {
    if (slint_view == nil || video_view == nil || metal_layer == nil) {
      return CGSizeZero;
    }
    NSView* parent = slint_view.superview;
    if (parent == nil) {
      return CGSizeZero;
    }
    RestoreVideoViewOrder();

    const CGFloat inset = std::clamp<CGFloat>(
        top_inset_points, 0.0, NSHeight(slint_view.bounds));
    if (video_top.constant != inset) {
      video_top.constant = inset;
      [parent layoutSubtreeIfNeeded];
    }

    const CGFloat scale = std::max<CGFloat>(
        1.0, slint_view.window ? slint_view.window.backingScaleFactor : 1.0);
    metal_layer.contentsScale = scale;
    const NSSize limit = [video_view updateViewport];
    return CGSizeMake(limit.width * scale, limit.height * scale);
  }

  // Publishes the AppKit facts the render queue is not allowed to read into
  // the async state, then caches the raster geometry for direct submission-
  // driven presentation. Main thread only.
  void PublishAsyncGeometry(CGSize raster_limit) {
    async_state->raster_width.store(raster_limit.width,
                                    std::memory_order_release);
    async_state->raster_height.store(raster_limit.height,
                                     std::memory_order_release);
    bool visible = false;
    bool transitioning = false;
    if (slint_view != nil) {
      NSWindow* window = slint_view.window;
      visible = window != nil && window.isVisible && !window.isMiniaturized &&
          !(video_view != nil && video_view.isHiddenOrHasHiddenAncestor) &&
          (window.occlusionState & NSWindowOcclusionStateVisible) != 0;
      transitioning =
          IsStreamWindowFullscreenTransitioning((__bridge void*)slint_view);
    }
    async_state->visible.store(visible, std::memory_order_release);
    async_state->transitioning.store(transitioning, std::memory_order_release);
    geometry_valid.store(raster_limit.width > 0 && raster_limit.height > 0,
                         std::memory_order_release);
  }

  // Thread-agnostic presentation core. Accesses only the mutex-guarded frame
  // state, the thread-safe CAMetalLayer and the Metal objects inside the
  // immutable async state: never AppKit, never Impl members. May run on the
  // main thread (sync/transition fallback) or on the render queue.
  static MacMetalVideoRenderer::RenderOutcome RenderFrameCore(
      const std::shared_ptr<AsyncRenderState>& state,
      const std::string& remote_id, double raster_width, double raster_height,
      bool allow_cached_frame, id<CAMetalDrawable> prebound_drawable,
      double prebound_wait_ms);

  // Dispatches one coalesced presentation onto the render queue. Safe from
  // any thread. The block captures the immutable async state, never Impl, so
  // teardown neither drains the queue nor risks a dangling this.
  void RequestAsyncRender(const std::string& remote_id) {
    // Copy the shared_ptr under its mutex: Attach replaces it on the main
    // thread while decode threads read it here.
    const std::shared_ptr<AsyncRenderState> state = GetAsyncState();
    if (!attached.load(std::memory_order_acquire) ||
        !geometry_valid.load(std::memory_order_acquire) ||
        state->detached->load(std::memory_order_acquire) ||
        state->transitioning.load(std::memory_order_acquire) ||
        EnvSyncPresentEnabled() || render_queue == nullptr ||
        state->metal_layer == nil) {
      return;
    }
    if (state->render_queued.exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    // Copy the stream id: the caller's reference may dangle by the time
    // the queue runs. The block captures only refcounted or owned objects.
    const std::string stream_id = remote_id;
    dispatch_async(render_queue, ^{
      @autoreleasepool {
        // Clear before rendering, not after: a frame submitted mid-render
        // then observes the cleared flag and dispatches its own trailing
        // pass, so the newest frame is never stranded behind a render that
        // started before it arrived. The serial queue orders the passes.
        state->render_queued.store(false, std::memory_order_release);
        // Re-check inside the queue: detach may have landed while this block
        // waited behind an in-flight presentation.
        if (!state->detached->load(std::memory_order_acquire)) {
          const double width =
              state->raster_width.load(std::memory_order_acquire);
          const double height =
              state->raster_height.load(std::memory_order_acquire);
          if (width > 0 && height > 0) {
            RenderFrameCore(state, stream_id, width, height, true, nil, 0);
          }
        }
      }
    });
  }
};

MacMetalVideoRenderer::MacMetalVideoRenderer()
    : impl_(std::make_unique<Impl>()) {}

MacMetalVideoRenderer::~MacMetalVideoRenderer() = default;

bool MacMetalVideoRenderer::IsReady() const {
  return impl_ && impl_->Ready();
}

bool MacMetalVideoRenderer::IsActive() const {
  return IsReady() && IsAttached();
}

bool MacMetalVideoRenderer::SetSelectedStream(std::string remote_id) {
  if (!impl_) {
    return false;
  }
  std::lock_guard lock(impl_->frames->mutex);
  if (impl_->frames->selected_stream == remote_id) {
    return false;
  }
  impl_->frames->selected_stream = std::move(remote_id);
  ++impl_->frames->content_generation;
  for (auto& slot : impl_->frames->slots) {
    if (slot.use == SlotUse::pending) {
      slot.use = SlotUse::available;
    }
  }
  impl_->native_surface_needs_redraw->store(true,
                                            std::memory_order_release);
  return true;
}

void MacMetalVideoRenderer::DiscardStream(std::string_view remote_id) {
  if (!impl_ || remote_id.empty()) {
    return;
  }
  std::lock_guard lock(impl_->frames->mutex);
  bool discarded = false;
  for (auto& slot : impl_->frames->slots) {
    if (!slot.valid || slot.remote_id != remote_id) {
      continue;
    }
    slot.valid = false;
    if (slot.use == SlotUse::pending) {
      slot.use = SlotUse::available;
    }
    discarded = true;
  }
  if (discarded || impl_->frames->selected_stream == remote_id) {
    ++impl_->frames->content_generation;
    impl_->native_surface_needs_redraw->store(true,
                                              std::memory_order_release);
  }
}

MacMetalVideoRenderer::SubmitResult MacMetalVideoRenderer::SubmitNv12(std::string_view remote_id,
                                                                      const uint8_t* data,
                                                                      size_t size, int width,
                                                                      int height,
                                                                      VideoLatencyFrame timing) {
  return SubmitNv12Internal(remote_id, data, size, width, height, true, timing);
}

MacMetalVideoRenderer::SubmitResult MacMetalVideoRenderer::SubmitNativeFrame(
    std::string_view remote_id, const MiniRtcNativeVideoFrame& frame, VideoLatencyFrame timing) {
  if (!IsReady()) {
    return SubmitResult::failed;
  }
  const MiniRtcNativeVideoFrame* native = GetMacNativeFrame(frame);
  if (!native) {
    return SubmitResult::failed;
  }

  {
    std::lock_guard lock(impl_->frames->mutex);
    if (impl_->frames->selected_stream != remote_id) {
      return SubmitResult::not_selected;
    }
  }

  const int width = static_cast<int>(native->width);
  const int height = static_cast<int>(native->height);
  auto pixel_buffer = static_cast<CVPixelBufferRef>(native->payload.cv_pixel_buffer);
  const OSType pixel_format = CVPixelBufferGetPixelFormatType(pixel_buffer);
  const bool yuv444 = IsMacYuv444(pixel_format);
  const bool xf44 = pixel_format == kCVPixelFormatType_444YpCbCr10BiPlanarFullRange;
  const auto submit_cpu_fallback = [&]() {
    if (yuv444) {
      static std::once_flag notice;
      std::call_once(notice, [] { LOG_WARN("Metal could not upload a 4:4:4 frame; dropping it"); });
      return SubmitResult::dropped;
    }
    if (!impl_->native_fallback_logged.exchange(true,
                                                std::memory_order_acq_rel)) {
      LOG_WARN("macOS native CVPixelBuffer upload unavailable; using CPU NV12 "
               "fallback");
    }
    const size_t y_size = static_cast<size_t>(width) * height;
    std::vector<unsigned char> packed_nv12(y_size + y_size / 2);
    if (native->copy_to_nv12(native->owner, packed_nv12.data(),
                             packed_nv12.size()) != 0) {
      return SubmitResult::failed;
    }
    return SubmitNv12(remote_id, packed_nv12.data(), packed_nv12.size(), width, height, timing);
  };

  if (!CVPixelBufferIsPlanar(pixel_buffer) ||
      CVPixelBufferGetPlaneCount(pixel_buffer) < 2 ||
      CVPixelBufferGetWidth(pixel_buffer) != native->width ||
      CVPixelBufferGetHeight(pixel_buffer) != native->height ||
      (!yuv444 && !IsMacNv12(pixel_format))) {
    return submit_cpu_fallback();
  }

  NativeVideoFrameRef retained(native);
  if (!retained || !impl_->texture_cache) {
    return submit_cpu_fallback();
  }

  CVMetalTextureRef y_cv_texture = nullptr;
  CVReturn y_status = CVMetalTextureCacheCreateTextureFromImage(
      kCFAllocatorDefault, impl_->texture_cache, pixel_buffer, nullptr,
      xf44 ? MTLPixelFormatR16Unorm : MTLPixelFormatR8Unorm, native->width, native->height, 0,
      &y_cv_texture);
  if (y_status != kCVReturnSuccess || !y_cv_texture) {
    return submit_cpu_fallback();
  }

  CVMetalTextureRef uv_cv_texture = nullptr;
  CVReturn uv_status = CVMetalTextureCacheCreateTextureFromImage(
      kCFAllocatorDefault, impl_->texture_cache, pixel_buffer, nullptr,
      xf44 ? MTLPixelFormatRG16Unorm : MTLPixelFormatRG8Unorm,
      yuv444 ? native->width : native->width / 2U, yuv444 ? native->height : native->height / 2U, 1,
      &uv_cv_texture);
  if (uv_status != kCVReturnSuccess || !uv_cv_texture) {
    CFRelease(y_cv_texture);
    return submit_cpu_fallback();
  }

  id<MTLTexture> y_texture = CVMetalTextureGetTexture(y_cv_texture);
  id<MTLTexture> uv_texture = CVMetalTextureGetTexture(uv_cv_texture);
  if (y_texture == nil || uv_texture == nil) {
    CFRelease(y_cv_texture);
    CFRelease(uv_cv_texture);
    return submit_cpu_fallback();
  }

  auto& frames = *impl_->frames;
  std::lock_guard lock(frames.mutex);
  if (frames.selected_stream != remote_id) {
    CFRelease(y_cv_texture);
    CFRelease(uv_cv_texture);
    return SubmitResult::not_selected;
  }

  // Latest-frame-only queue: replace an unconsumed decoded frame to bound
  // latency, while never recycling a frame still referenced by the GPU.
  FrameSlot* target = nullptr;
  for (auto& slot : frames.slots) {
    if (slot.use == SlotUse::pending) {
      target = &slot;
      break;
    }
  }
  if (!target) {
    uint64_t oldest_sequence = std::numeric_limits<uint64_t>::max();
    for (auto& slot : frames.slots) {
      if (slot.use != SlotUse::available) {
        continue;
      }
      if (!slot.valid) {
        target = &slot;
        break;
      }
      if (slot.sequence < oldest_sequence) {
        oldest_sequence = slot.sequence;
        target = &slot;
      }
    }
  }
  if (!target) {
    CFRelease(y_cv_texture);
    CFRelease(uv_cv_texture);
    timing.MarkStage(VideoFrameStage::busy);
    return SubmitResult::dropped;
  }

  if (target->use == SlotUse::pending) {
    target->timing.MarkStage(VideoFrameStage::replaced);
  }
  target->ReleaseResources();
  target->y_cv_texture = y_cv_texture;
  target->uv_cv_texture = uv_cv_texture;
  target->y_texture = y_texture;
  target->uv_texture = uv_texture;
  target->native_frame = std::move(retained);
  CFTypeRef matrix = CVBufferCopyAttachment(pixel_buffer, kCVImageBufferYCbCrMatrixKey, nullptr);
  // Normalized coordinates work for both subsampled and full-size UV. xf44
  // samples are MSB-aligned in R16/RG16Unorm: neutral is 512 << 6, not the
  // 8-bit shader's 128/255. No studio-swing expansion for full-range YUV.
  target->color = {xf44 || pixel_format == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange,
                   matrix && CFEqual(matrix, kCVImageBufferYCbCrMatrix_ITU_R_709_2),
                   xf44 ? 32768.0f / 65535.0f : 128.0f / 255.0f};
  if (matrix) CFRelease(matrix);
  target->width = width;
  target->subsampled_chroma = !yuv444;
  target->height = height;
  target->remote_id.assign(remote_id);
  target->sequence = frames.next_sequence++;
  target->timing = timing;
  target->use = SlotUse::pending;
  target->valid = true;
  timing.MarkStage(VideoFrameStage::accepted);
  if (!impl_->native_path_logged.exchange(true, std::memory_order_acq_rel)) {
    LOG_INFO("macOS VideoToolbox-to-Metal native YUV path enabled");
  }
  // Present directly from the decode thread: the render queue blocks on
  // nextDrawable instead of the Slint main thread, and selects this frame
  // after the drawable arrives. dispatch_async never blocks, so calling it
  // with the frame mutex held cannot deadlock.
  impl_->RequestAsyncRender(std::string(remote_id));
  return SubmitResult::submitted;
}

MacMetalVideoRenderer::SubmitResult MacMetalVideoRenderer::SubmitCachedNv12(
    std::string_view remote_id, const uint8_t* data, size_t size, int width, int height,
    VideoLatencyFrame timing) {
  return SubmitNv12Internal(remote_id, data, size, width, height, false, timing);
}

MacMetalVideoRenderer::SubmitResult MacMetalVideoRenderer::SubmitNv12Internal(
    std::string_view remote_id, const uint8_t* data, size_t size, int width, int height,
    bool replace_pending, VideoLatencyFrame timing) {
  if (!IsReady()) {
    return SubmitResult::failed;
  }

  auto& frames = *impl_->frames;
  std::lock_guard lock(frames.mutex);
  if (frames.selected_stream != remote_id) {
    return SubmitResult::not_selected;
  }
  if (data == nullptr || width <= 0 || height <= 0 || (width & 1) != 0 ||
      (height & 1) != 0) {
    return SubmitResult::failed;
  }
  const size_t y_size = static_cast<size_t>(width) * height;
  const size_t required_size = y_size + y_size / 2;
  if (size < required_size) {
    return SubmitResult::failed;
  }

  FrameSlot* target = nullptr;
  if (replace_pending) {
    for (auto& slot : frames.slots) {
      if (slot.use == SlotUse::pending) {
        target = &slot;
        break;
      }
    }
  } else {
    for (const auto& slot : frames.slots) {
      if (slot.valid && slot.remote_id == remote_id &&
          (slot.use == SlotUse::pending ||
           slot.use == SlotUse::in_flight)) {
        return SubmitResult::dropped;
      }
    }
  }
  if (target == nullptr) {
    uint64_t oldest_sequence = std::numeric_limits<uint64_t>::max();
    for (auto& slot : frames.slots) {
      if (slot.use != SlotUse::available) {
        continue;
      }
      if (!slot.valid) {
        target = &slot;
        break;
      }
      if (slot.sequence < oldest_sequence) {
        oldest_sequence = slot.sequence;
        target = &slot;
      }
    }
  }
  if (target == nullptr) {
    timing.MarkStage(VideoFrameStage::busy);
    return SubmitResult::dropped;
  }
  const VideoLatencyFrame replaced_timing =
      target->use == SlotUse::pending ? target->timing : VideoLatencyFrame{};
  if (!AllocateSlot(impl_->device, *target, width, height)) {
    return SubmitResult::failed;
  }
  replaced_timing.MarkStage(VideoFrameStage::replaced);

  auto* y_destination = static_cast<uint8_t*>(target->y_buffer.contents);
  auto* uv_destination = static_cast<uint8_t*>(target->uv_buffer.contents);
  const uint8_t* uv_source = data + y_size;
  for (int row = 0; row < height; ++row) {
    std::memcpy(y_destination + static_cast<size_t>(row) * target->y_stride,
                data + static_cast<size_t>(row) * width, width);
  }
  for (int row = 0; row < height / 2; ++row) {
    std::memcpy(uv_destination + static_cast<size_t>(row) * target->uv_stride,
                uv_source + static_cast<size_t>(row) * width, width);
  }

  target->remote_id.assign(remote_id);
  target->sequence = frames.next_sequence++;
  target->timing = timing;
  target->use = SlotUse::pending;
  target->valid = true;
  timing.MarkStage(VideoFrameStage::accepted);
  impl_->RequestAsyncRender(std::string(remote_id));
  return SubmitResult::submitted;
}

bool MacMetalVideoRenderer::Attach(void* opaque_slint_view,
                                   std::function<void()> drawable_ready) {
  if (!IsReady() || opaque_slint_view == nullptr) {
    return false;
  }
  @autoreleasepool {
    NSView* requested_view = (__bridge NSView*)opaque_slint_view;
    if (impl_->slint_view == requested_view && impl_->video_view != nil &&
        impl_->video_view.superview != nil) {
      if (!IsStreamWindowFullscreen(opaque_slint_view)) {
        ConfigureStreamWindowChrome(requested_view.window);
        requested_view.window.titleVisibility = NSWindowTitleHidden;
      }
      return true;
    }
    NSWindow* window = requested_view.window;
    if (requested_view.superview == nil || window == nil) {
      return false;
    }

    impl_->DetachViews();
    impl_->drawable_ready = std::move(drawable_ready);
    ConfigureStreamWindowChrome(window);
    impl_->slint_view = requested_view;
    impl_->slint_view.wantsLayer = YES;
    impl_->slint_view.layer.opaque = NO;
    impl_->slint_view.layer.backgroundColor = NSColor.clearColor.CGColor;

    impl_->video_view =
        [[CrossDeskMetalVideoView alloc] initWithFrame:requested_view.frame];
    impl_->video_view.translatesAutoresizingMaskIntoConstraints = NO;
    impl_->metal_layer = impl_->video_view.surface;
    impl_->metal_layer.device = impl_->device;
    impl_->metal_layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    impl_->metal_layer.framebufferOnly = YES;
    impl_->metal_layer.opaque = YES;
    impl_->metal_layer.backgroundColor = NSColor.blackColor.CGColor;
    // Two drawables keep one frame in the compositor while the next renders.
    // The default of three lets a slow GPU frame sit an extra display interval
    // before it reaches the screen, which is pure added latency for live video.
    impl_->metal_layer.maximumDrawableCount = 2;
    impl_->metal_layer.displaySyncEnabled = YES;
    // nextDrawable must never spuriously return nil after one second while
    // the render queue is the only waiter: during heavy WindowServer
    // pressure a frame could otherwise be dropped silently. The queue
    // teardown contract does not depend on a timeout — detached is checked
    // immediately after the wait returns.
    impl_->metal_layer.allowsNextDrawableTimeout = NO;
    // Publish a fresh immutable async state for this attachment. The old
    // state was marked detached by DetachViews; blocks still referencing it
    // exit on their own.
    auto fresh_state = std::make_shared<Impl::AsyncRenderState>();
    fresh_state->command_queue = impl_->command_queue;
    fresh_state->pipeline = impl_->pipeline;
    fresh_state->frames = impl_->frames;
    fresh_state->native_surface_needs_redraw = impl_->native_surface_needs_redraw;
    fresh_state->metal_layer = impl_->metal_layer;
    {
      std::lock_guard lock(impl_->async_state_mutex);
      impl_->async_state = fresh_state;
    }
    impl_->harvested_outcome_count = 0;
    fresh_state->render_queued.store(false, std::memory_order_release);

    [requested_view.superview addSubview:impl_->video_view
                               positioned:NSWindowBelow
                               relativeTo:requested_view];
    // The clipping view always covers the same content area as the Slint overlay.
    NSView* video = impl_->video_view;
    impl_->video_top =
        [video.topAnchor constraintEqualToAnchor:requested_view.topAnchor];
    [NSLayoutConstraint activateConstraints:@[
        impl_->video_top,
        [video.bottomAnchor constraintEqualToAnchor:requested_view.bottomAnchor],
        [video.leadingAnchor constraintEqualToAnchor:requested_view.leadingAnchor],
        [video.trailingAnchor constraintEqualToAnchor:requested_view.trailingAnchor],
    ]];

    impl_->title_accessory_controller =
        [[NSTitlebarAccessoryViewController alloc] init];
    impl_->title_accessory_controller.layoutAttribute =
        NSLayoutAttributeLeading;
    CrossDeskStreamTitleView* title_view =
        [[CrossDeskStreamTitleView alloc] initWithWindow:window];
    const auto surface_dirty_flag = impl_->native_surface_needs_redraw;
    title_view.surfaceInvalidationHandler = ^{
      surface_dirty_flag->store(true, std::memory_order_release);
    };
    title_view.resizeVideoHandler = ^(NSWindow* resized_window) {
      // Native zoom runs a nested AppKit loop. Resize notifications continue
      // while the usual main-queue wakeup for a decoded frame is waiting.
      std::string rendered_stream;
      {
        std::lock_guard lock(impl_->frames->mutex);
        rendered_stream = impl_->frames->rendered_stream;
      }
      if (rendered_stream.empty() || resized_window.isMiniaturized ||
          !(resized_window.occlusionState & NSWindowOcclusionStateVisible)) return;
      const CGSize size = impl_->metal_layer.drawableSize;
      DrawFrame(rendered_stream, size.width, size.height, false);
    };
    impl_->title_accessory_controller.view = title_view;
    [window addTitlebarAccessoryViewController:
                impl_->title_accessory_controller];
    window.titleVisibility = NSWindowTitleHidden;

    // Adding the title accessory changes AppKit's content layout guide. Commit
    // it before sizing the video so a newly-created light window does not keep
    // the transparent full-size titlebar geometry from winit.
    NSView* frame_view = window.contentView.superview;
    frame_view.needsLayout = YES;
    [frame_view layoutSubtreeIfNeeded];
    impl_->RestoreVideoViewOrder();

    // Let AppKit move the accessory together with the standard window
    // buttons, including when the user reveals the fullscreen titlebar.
    title_view.windowLayoutHandler = ^BOOL(NSWindow* window) {
      const bool restored = ConfigureStreamWindowChrome(window);
      window.titleVisibility = NSWindowTitleHidden;
      return restored && window.titleVisibility == NSWindowTitleHidden;
    };
    impl_->UpdateVideoGeometry(0.0);
    impl_->native_surface_needs_redraw->store(true,
                                              std::memory_order_release);
    const bool attached = impl_->video_view.superview != nil;
    impl_->attached.store(attached, std::memory_order_release);
    return attached;
  }
}

void MacMetalVideoRenderer::Detach() {
  if (impl_) {
    impl_->DetachViews();
  }
}

bool MacMetalVideoRenderer::IsAttached() const {
  return impl_ && impl_->attached.load(std::memory_order_acquire);
}

bool MacMetalVideoRenderer::NeedsSurfaceRedraw() const {
  if (!IsReady() || !IsAttached()) {
    return false;
  }

  @autoreleasepool {
    NSWindow* window = impl_->slint_view.window;
    if (window == nil || !window.isVisible || window.isMiniaturized) {
      return false;
    }
  }

  return impl_->native_surface_needs_redraw->load(
      std::memory_order_acquire);
}

MacMetalVideoRenderer::RenderOutcome MacMetalVideoRenderer::RenderLatest(
    std::string_view remote_id, double top_inset_points) {
  if (!IsReady() || !IsAttached()) {
    return {RenderResult::failed};
  }

  @autoreleasepool {
    // Geometry and the cached visibility/transition facts are AppKit reads
    // and stay on the main thread. Publishing them here is what arms the
    // render queue for direct submission-driven presentation.
    const CGSize raster_limit = impl_->UpdateVideoGeometry(top_inset_points);
    impl_->PublishAsyncGeometry(raster_limit);
    if (raster_limit.width <= 0 || raster_limit.height <= 0) {
      return {RenderResult::idle};
    }
    const bool transitioning =
        impl_->drawable_request ||
        IsStreamWindowFullscreenTransitioning((__bridge void*)impl_->slint_view);
    // Fullscreen Space transitions keep the legacy main-thread handoff:
    // WindowServer can hold both drawables for the whole animation, and the
    // UI thread must stay free to drive it. Steady-state presentation lives
    // on the render queue; the sync-present env override forces the legacy
    // inline path for debugging.
    if (!transitioning && !EnvSyncPresentEnabled()) {
      impl_->RequestAsyncRender(std::string(remote_id));
      // The queue owns presentation now. Harvest the newest outcome it
      // recorded so the UI bookkeeping (has_frame, resolution label,
      // new-frame sequence accounting) observes real presentations rather
      // than the request. Latency statistics never pass through here: the
      // command-buffer completion and drawable presented handlers record
      // them exactly once per committed frame.
      std::lock_guard lock(impl_->async_state->outcome_mutex);
      if (impl_->async_state->outcome_count !=
          impl_->harvested_outcome_count) {
        impl_->harvested_outcome_count = impl_->async_state->outcome_count;
        return {RenderResult::rendered, impl_->async_state->outcome_width,
                impl_->async_state->outcome_height,
                impl_->async_state->outcome_sequence};
      }
      return {RenderResult::idle};
    }
    return DrawFrame(remote_id, raster_limit.width, raster_limit.height, true);
  }
}

// Main-thread presentation for the fullscreen-transition handoff and the
// FARLINK_METAL_SYNC_PRESENT=1 debugging fallback. Steady-state frames are
// presented on the render queue through RequestAsyncRender instead, so a
// busy WindowServer drawable pool never blocks input or UI delivery here.
MacMetalVideoRenderer::RenderOutcome MacMetalVideoRenderer::DrawFrame(
    std::string_view remote_id, double raster_width, double raster_height,
    bool allow_cached_frame) {
  if (raster_width <= 0 || raster_height <= 0) return {RenderResult::idle};
  @autoreleasepool {
    id<CAMetalDrawable> drawable = nil;
    double drawable_wait_ms = 0;
    if (impl_->drawable_request ||
        IsStreamWindowFullscreenTransitioning((__bridge void*)impl_->slint_view)) {
      drawable = impl_->TakeTransitionDrawable(drawable_wait_ms);
      if (!drawable) return {RenderResult::idle};
    }
    return impl_->RenderFrameCore(impl_->async_state, std::string(remote_id),
                                  raster_width, raster_height,
                                  allow_cached_frame, drawable, drawable_wait_ms);
  }
}

// Thread-agnostic presentation core shared by the render queue and the
// main-thread fallback paths. Everything it touches is either mutex-guarded
// or documented thread-safe off the main thread (CAMetalLayer, MTLCommandQueue).
// When |prebound_drawable| is non-nil the caller already owns a drawable from
// the fullscreen-transition handoff and no new nextDrawable call is made.
MacMetalVideoRenderer::RenderOutcome MacMetalVideoRenderer::Impl::RenderFrameCore(
    const std::shared_ptr<AsyncRenderState>& state,
    const std::string& remote_id, double raster_width, double raster_height,
    bool allow_cached_frame, id<CAMetalDrawable> prebound_drawable,
    double prebound_wait_ms) {
  using RenderResult = MacMetalVideoRenderer::RenderResult;
  using RenderOutcome = MacMetalVideoRenderer::RenderOutcome;
  if (raster_width <= 0 || raster_height <= 0 || state->metal_layer == nil) {
    return {RenderResult::idle};
  }
  @autoreleasepool {
    CAMetalLayer* layer = state->metal_layer;
    auto& frames = *state->frames;
    size_t slot_index = kFrameSlotCount;
    uint64_t sequence = 0;
    int source_width = 0;
    int source_height = 0;
    bool subsampled_chroma = true;
    bool matching_frame_in_flight = false;
    uint64_t content_generation = 0;
    bool redraw_cached_frame = false;
    // Called only while holding frames.mutex. Select again after nextDrawable:
    // a decoder callback may replace the pending slot while that call waits.
    const auto select_latest = [&] {
      slot_index = kFrameSlotCount;
      sequence = 0;
      matching_frame_in_flight = false;
      for (size_t index = 0; index < frames.slots.size(); ++index) {
        const auto& slot = frames.slots[index];
        matching_frame_in_flight |= slot.valid &&
            slot.use == SlotUse::in_flight && slot.remote_id == remote_id;
        if (slot.valid && slot.use == SlotUse::pending &&
            slot.remote_id == remote_id &&
            (slot_index == kFrameSlotCount || slot.sequence > sequence)) {
          slot_index = index;
          sequence = slot.sequence;
        }
      }
      if (slot_index == kFrameSlotCount && redraw_cached_frame) {
        for (size_t index = 0; index < frames.slots.size(); ++index) {
          const auto& slot = frames.slots[index];
          if (slot.valid && slot.use == SlotUse::available &&
              slot.remote_id == remote_id &&
              (slot_index == kFrameSlotCount || slot.sequence > sequence)) {
            slot_index = index;
            sequence = slot.sequence;
          }
        }
      }
    };
    {
      std::lock_guard lock(frames.mutex);
      if (frames.selected_stream != remote_id) {
        return {RenderResult::idle};
      }
      content_generation = frames.content_generation;
      const bool stream_changed = frames.rendered_stream != remote_id;
      redraw_cached_frame = allow_cached_frame && (stream_changed ||
          state->native_surface_needs_redraw->load(std::memory_order_acquire) ||
          frames.rendered_content_generation != content_generation);
      select_latest();
      if (slot_index == kFrameSlotCount && !redraw_cached_frame) {
        return {RenderResult::idle};
      }
      if (slot_index != kFrameSlotCount) {
        source_width = frames.slots[slot_index].width;
        source_height = frames.slots[slot_index].height;
        subsampled_chroma = frames.slots[slot_index].subsampled_chroma;
      }
    }

    // Upscaling sharpening must rasterize at real target pixels: capping the
    // drawable at the source size would hand WindowServer a small layer to
    // bilinear-stretch, bypassing the CAS shader entirely. With sharpening
    // active the layer renders at aspect-preserving Retina resolution
    // (rounded to even pixels, capped at the viewport raster size); at 1:1
    // pixel-exact or with sharpening off the historical source-size drawable
    // keeps letterboxing in the compositor, which can scale the last
    // presented image with the window even while the remote desktop is idle.
    float sharpen = 0.0f;
    if (source_width > 0 && source_height > 0) {
      sharpen = ResolveSharpenStrength(raster_width, raster_height,
                                       source_width, source_height,
                                       subsampled_chroma);
      double scale;
      if (sharpen > 0.0f) {
        scale = std::min(raster_width / source_width,
                         raster_height / source_height);
        // Never upscale the backing store beyond 2x the source: past that
        // point the CAS pass cost grows quadratically for no additional
        // acutance, and the compositor can cover the remainder.
        scale = std::min(scale, 2.0);
      } else {
        scale = std::min({1.0, raster_width / source_width,
                          raster_height / source_height});
      }
      // Even dimensions keep chroma-subsampled texel centres aligned with
      // the reconstruction grid.
      const double even_w = std::max(2.0, 2.0 * std::floor(source_width * scale / 2.0 + 0.5));
      const double even_h = std::max(2.0, 2.0 * std::floor(source_height * scale / 2.0 + 0.5));
      const CGSize size = CGSizeMake(even_w, even_h);
      if (!CGSizeEqualToSize(layer.drawableSize, size))
        layer.drawableSize = size;
    }
    double drawable_wait_ms = prebound_wait_ms;
    id<CAMetalDrawable> drawable = prebound_drawable;
    if (!drawable) {
      const auto start = VideoLatencyFrame::Clock::now();
      drawable = [layer nextDrawable];
      drawable_wait_ms = std::chrono::duration<double, std::milli>(
          VideoLatencyFrame::Clock::now() - start).count();
      if (!drawable) {
        state->native_surface_needs_redraw->store(true, std::memory_order_release);
        // A hidden or minimized window yields no drawable; that is the idle
        // condition the legacy path reported, not a failure.
        if (!state->visible.load(std::memory_order_acquire)) {
          return {RenderResult::idle};
        }
        return {RenderResult::failed};
      }
    }
    // Teardown may have landed while nextDrawable blocked: never commit into
    // a torn-down surface.
    if (state->detached->load(std::memory_order_acquire)) {
      return {RenderResult::idle};
    }

    id<MTLTexture> y_texture = nil;
    id<MTLTexture> uv_texture = nil;
    Nv12Color color;
    VideoLatencyFrame timing;
    {
      std::lock_guard lock(frames.mutex);
      // Discard/reselect can invalidate a stream even if its name is reused.
      // Never carry the old stream's clear or texture into the new generation.
      if (frames.selected_stream != remote_id ||
          frames.content_generation != content_generation) {
        state->native_surface_needs_redraw->store(true, std::memory_order_release);
        return {RenderResult::idle};
      }
      select_latest();
      if (slot_index != kFrameSlotCount) {
        auto& slot = frames.slots[slot_index];
        if (slot.width != source_width || slot.height != source_height) {
          state->native_surface_needs_redraw->store(true, std::memory_order_release);
          return {RenderResult::idle};
        }
        slot.use = SlotUse::in_flight;
        y_texture = slot.y_texture;
        uv_texture = slot.uv_texture;
        timing = slot.timing;
        color = slot.color;
        color.sharpness = sharpen;
      }
    }
    if (slot_index == kFrameSlotCount) {
      if (!redraw_cached_frame) return {RenderResult::idle};
      MTLRenderPassDescriptor* clear_pass =
          [MTLRenderPassDescriptor renderPassDescriptor];
      clear_pass.colorAttachments[0].texture = drawable.texture;
      clear_pass.colorAttachments[0].loadAction = MTLLoadActionClear;
      clear_pass.colorAttachments[0].storeAction = MTLStoreActionStore;
      clear_pass.colorAttachments[0].clearColor =
          MTLClearColorMake(0.0, 0.0, 0.0, 1.0);
      id<MTLCommandBuffer> clear_command =
          [state->command_queue commandBuffer];
      id<MTLRenderCommandEncoder> clear_encoder =
          [clear_command renderCommandEncoderWithDescriptor:clear_pass];
      if (clear_command == nil || clear_encoder == nil) {
        state->native_surface_needs_redraw->store(true, std::memory_order_release);
        return {RenderResult::failed};
      }
      [clear_encoder endEncoding];
      [clear_command presentDrawable:drawable];
      [clear_command commit];
      {
        std::lock_guard lock(frames.mutex);
        frames.rendered_stream.assign(remote_id);
        frames.rendered_content_generation = content_generation;
      }
      state->native_surface_needs_redraw->store(
          matching_frame_in_flight, std::memory_order_release);
      return {RenderResult::empty};
    }

    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = drawable.texture;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);

    id<MTLCommandBuffer> command_buffer = [state->command_queue commandBuffer];
    id<MTLRenderCommandEncoder> encoder =
        [command_buffer renderCommandEncoderWithDescriptor:pass];
    if (command_buffer == nil || encoder == nil) {
      std::lock_guard lock(frames.mutex);
      auto& slot = frames.slots[slot_index];
      if (slot.sequence == sequence && slot.use == SlotUse::in_flight) {
        slot.use = SlotUse::available;
      }
      state->native_surface_needs_redraw->store(true, std::memory_order_release);
      return {RenderResult::failed};
    }

    [encoder setRenderPipelineState:state->pipeline];
    [encoder setFragmentBytes:&color length:sizeof(color) atIndex:0];
    [encoder setFragmentTexture:y_texture atIndex:0];
    [encoder setFragmentTexture:uv_texture atIndex:1];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip
                vertexStart:0
                vertexCount:4];
    [encoder endEncoding];

    // AppKit window occlusion cannot be read off the main thread, so both
    // the queue and the synchronous paths share the cached visibility the
    // main thread publishes on every geometry pass. Latency statistics keep
    // their exact visible-at-submit semantics.
    const bool visible_at_submit =
        state->visible.load(std::memory_order_acquire);
    std::shared_ptr<SharedFrameState> completion_frames = state->frames;
    [command_buffer addCompletedHandler:^(id<MTLCommandBuffer> completed) {
      {
        std::lock_guard lock(completion_frames->mutex);
        auto& slot = completion_frames->slots[slot_index];
        if (slot.sequence == sequence && slot.use == SlotUse::in_flight) {
          slot.use = SlotUse::available;
        }
      }
      // Some macOS/display environments return zero for every presentedTime,
      // even while the video is visible. Keep a labelled GPU-end estimate so
      // that missing presentation feedback does not leave latency blank. A
      // real presentedTime takes precedence in VideoLatencyStats. Never use
      // callback arrival time, which can be delayed by seconds.
      const auto now = VideoLatencyFrame::Clock::now();
      const double gpu_end = completed.GPUEndTime;
      const auto rendered_at = VideoLatencyFrame::FromHostTime(
          gpu_end, CACurrentMediaTime(), now);
      const bool gpu_completed =
          completed.status == MTLCommandBufferStatusCompleted;
      if (gpu_completed && visible_at_submit) {
        timing.MarkStage(VideoFrameStage::gpu_completed, now);
      }
      if (gpu_completed && visible_at_submit && rendered_at) {
        timing.MarkEstimated(*rendered_at);
      }
    }];
    [drawable addPresentedHandler:^(id<MTLDrawable> presented) {
      // presentedTime uses the Core Animation host clock. Translate its age
      // into our steady clock, excluding callback scheduling delay. Capturing
      // timing by value keeps this safe after a session/renderer is destroyed.
      const double presented_time = presented.presentedTime;
      const auto now = VideoLatencyFrame::Clock::now();
      const double host_now = CACurrentMediaTime();
      const auto presented_at = VideoLatencyFrame::FromHostTime(
          presented_time, host_now, now);
      if (visible_at_submit && presented_at) {
        timing.MarkPresented(*presented_at);
        // TRUE presentation is the drawable presented handler. Exactly once
        // per session, report ms since connect request/Ice ready.
        if (timing.stats && timing.stats->ConsumeFirstPresented()) {
          LogVideoFirstFramePresented(*timing.stats, *presented_at);
        }
      }
    }];
    [command_buffer presentDrawable:drawable];
    timing.MarkStage(VideoFrameStage::committed, VideoLatencyFrame::Clock::now(),
                     drawable_wait_ms);
    [command_buffer commit];
    {
      std::lock_guard lock(frames.mutex);
      frames.rendered_stream.assign(remote_id);
      frames.rendered_content_generation = content_generation;
    }
    // Feed the UI-thread bookkeeping the presentation it can no longer
    // observe directly: the resolution label and the new-frame count stay
    // exact even though presentation runs on the render queue.
    {
      std::lock_guard lock(state->outcome_mutex);
      state->outcome_sequence = sequence;
      state->outcome_width = source_width;
      state->outcome_height = source_height;
      ++state->outcome_count;
    }
    state->native_surface_needs_redraw->store(false, std::memory_order_release);
    return {RenderResult::rendered, source_width, source_height, sequence};
  }
}

bool MacMetalVideoRenderer::CopyLatestNv12(
    std::string_view remote_id, std::vector<unsigned char>* output,
    int* width, int* height) const {
  if (!impl_ || output == nullptr || width == nullptr || height == nullptr) {
    return false;
  }

  NativeVideoFrameRef native_frame;
  {
    std::lock_guard lock(impl_->frames->mutex);
    const FrameSlot* newest = nullptr;
    for (const auto& slot : impl_->frames->slots) {
      if (!slot.valid || slot.remote_id != remote_id || slot.sequence == 0 ||
          (!slot.native_frame &&
           (slot.y_buffer == nil || slot.uv_buffer == nil))) {
        continue;
      }
      if (newest == nullptr || slot.sequence > newest->sequence) {
        newest = &slot;
      }
    }
    if (newest == nullptr || newest->width <= 0 || newest->height <= 0) {
      return false;
    }

    *width = newest->width;
    *height = newest->height;
    native_frame = newest->native_frame;
    if (!native_frame) {
      const size_t y_size =
          static_cast<size_t>(newest->width) * newest->height;
      output->resize(y_size + y_size / 2);
      auto* y_source =
          static_cast<const uint8_t*>(newest->y_buffer.contents);
      auto* uv_source =
          static_cast<const uint8_t*>(newest->uv_buffer.contents);
      for (int row = 0; row < newest->height; ++row) {
        std::memcpy(output->data() +
                        static_cast<size_t>(row) * newest->width,
                    y_source + static_cast<size_t>(row) * newest->y_stride,
                    newest->width);
      }
      uint8_t* uv_destination = output->data() + y_size;
      for (int row = 0; row < newest->height / 2; ++row) {
        std::memcpy(uv_destination +
                        static_cast<size_t>(row) * newest->width,
                    uv_source + static_cast<size_t>(row) * newest->uv_stride,
                    newest->width);
      }
      return true;
    }
  }

  const MiniRtcNativeVideoFrame* native = native_frame.Get();
  const auto buffer = static_cast<CVPixelBufferRef>(native->payload.cv_pixel_buffer);
  if (IsMacYuv444(CVPixelBufferGetPixelFormatType(buffer))) return false;
  const size_t y_size = static_cast<size_t>(native->width) * native->height;
  output->resize(y_size + y_size / 2);
  return native->copy_to_nv12(native->owner, output->data(), output->size()) ==
         0;
}

}  // namespace crossdesk
