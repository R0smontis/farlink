#include "platform/macos/gui/metal_video_renderer.h"

#import <AppKit/AppKit.h>
#import <CoreVideo/CVMetalTextureCache.h>
#import <Metal/Metal.h>
#import <QuartzCore/CABase.h>
#import <QuartzCore/CAMetalDisplayLink.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/CATransaction.h>
#import <pthread.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <utility>
#include <thread>

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
// Invoked on the main thread every time the stream window's occlusion state
// changes. The argument is the new NSWindowOcclusionState bitmask; consumers
// use it to keep cached visibility in sync with reality rather than waiting
// for the next per-frame geometry pass. Nil-safe.
@property(nonatomic, copy) void (^occlusionChangedHandler)(NSWindowOcclusionState);
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
    // Occlusion is the single most important visibility input for the render
    // pipeline: NSWindowOcclusionStateVisible clears when the window is fully
    // covered, minimized or on another Space, and macOS stops compositing its
    // contents. Without this observer the only thing updating the cached
    // visibility in the async render state is the per-frame geometry pass,
    // which never runs when the remote desktop is static — exactly the
    // scenario in which occlusion is most likely to silently change.
    [center addObserver:self selector:@selector(windowOcclusionChanged:)
                   name:NSWindowDidChangeOcclusionStateNotification object:window];
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

- (void)windowOcclusionChanged:(NSNotification*)notification {
  (void)notification;
  NSWindow* window = _streamWindow;
  if (window == nil) return;
  const NSWindowOcclusionState state = window.occlusionState;
  if (self.occlusionChangedHandler != nil) {
    self.occlusionChangedHandler(state);
  }
  // Becoming visible again after a fully occluded period does not generate
  // any new video content on its own: the remote desktop may have been static
  // the whole time, and macOS does not guarantee the layer re-composites the
  // last committed frame when the window unoccludes. Invalidate the surface
  // so the very next frame — or the redraw path — repaints the full viewport.
  if ((state & NSWindowOcclusionStateVisible) != 0) {
    [self invalidateVideoSurface];
  }
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

// Moonlight-style phase-locked presentation driver. CAMetalDisplayLink
// delivers each vsync-aligned callback on its own runloop thread with a
// pre-acquired CAMetalDrawable and the exact target presentation timestamp,
// so presentation never pays the erratic nextDrawable wait jitter the plain
// render-queue path can hit under WindowServer pressure.
//
// Zero-idle-wake pacing: the link is created paused and only woken (via
// CFRunLoopPerformBlock + CFRunLoopWakeUp, safe from any thread because the
// driver retains the runloop for its whole lifetime) when a frame submit or
// a surface redraw request arrives. Inside the callback, two consecutive
// ticks that render nothing pause the link again, so a static 0 Hz desktop
// costs zero background GPU/CPU wakeups while active motion stays locked to
// the ProMotion VBlank cadence.
//
// Lifecycle: the driver owns a dedicated QOS_CLASS_USER_INTERACTIVE thread
// spinning a CFRunLoop. The link is created, used, invalidated and released
// entirely on that thread, which serializes the cross-thread unpause blocks
// against teardown by construction. -stop invalidates synchronously, wakes
// the runloop and joins the thread; it runs on the main thread during
// DetachViews before any frame-state locks are taken, so the in-flight
// render callback (which only touches mutex-protected frame state and
// thread-safe Metal objects) can never deadlock it. CAMetalDisplayLink's
// delegate is weak per the SDK header, and every unpause caller holds the
// shared async render state that strongly retains this driver, so neither
// the driver nor its runloop can die underneath a caller.
API_AVAILABLE(macos(14.0))
@interface CrossDeskMetalDisplayLinkDriver : NSObject <CAMetalDisplayLinkDelegate>

// Invoked on the display-link thread with the pre-acquired update drawable.
// Return YES when the tick committed a presentation (frame or redraw), NO
// when there was nothing to present; two consecutive NO ticks pause the
// link. Set before -startWithLayer:.
@property(nonatomic, copy) BOOL (^renderTick)(id<CAMetalDrawable> drawable);

// Optional counters invoked on the display-link thread: tickCounter fires on
// every vsync callback before any rendering decision; pauseCounter fires only
// when the link transitions into the paused state (the two-idle-tick path).
// Both must be trivially cheap — incrementing an atomic or similar — and are
// invoked with no driver lock held, so a non-trivial implementation must do
// its own synchronization. Set before -startWithLayer:.
@property(nonatomic, copy) void (^tickCounter)(void);
@property(nonatomic, copy) void (^pauseCounter)(void);

// Spawns the thread, creates the link paused and arms it on the thread's
// runloop. Synchronous; returns NO when the link could not be created, in
// which case the caller should fall back to immediate-mode presentation.
- (BOOL)startWithLayer:(CAMetalLayer*)layer;

// Invalidates the link on its runloop thread, stops the runloop and joins
// the thread. Idempotent, synchronous, main-thread only. Normally a few
// milliseconds: the thread is idle almost always and CFRunLoopWakeUp
// interrupts its wait immediately.
- (void)stop;

// Safe from any thread. Never blocks; silently no-ops once cancelled.
- (void)unpause;

@end

API_AVAILABLE(macos(14.0))
@implementation CrossDeskMetalDisplayLinkDriver {
  std::thread _thread;
  // Objective-C ivar blocks accept no initializers; the atomics value-
  // initialize to zero/nil/false, which is exactly the state an unstarted
  // driver needs, and the rest is set up in -startWithLayer:.
  std::atomic<CFRunLoopRef> _runloop;
  // Version-0 runloop source used purely as a persistent, lossless wakeup:
  // CFRunLoopSourceSignal latches until the runloop handles the source, so
  // unlike bare CFRunLoopWakeUp (a one-shot mach message consumed even when
  // the receiver is not yet waiting) it can never be lost against the
  // wake-then-sleep race, and the source auto-resets when it fires so the
  // loop never spins hot. CFRunLoopRunInMode with returnAfterSourceHandled
  // treats a fired source — but NOT a processed perform-block — as a reason
  // to return, which is exactly what the cancellation path needs for a
  // sub-millisecond join. Signaling is documented thread-safe.
  std::atomic<CFRunLoopSourceRef> _wake_source;
  std::atomic<bool> _cancelled;
  std::atomic<bool> _started;
  dispatch_semaphore_t _ready;
  CAMetalDisplayLink* _link;   // display-link thread only
  int _idle_ticks;             // display-link thread only
}

// The wake source's perform callback is intentionally empty: the source's
// only job is forcing CFRunLoopRunInMode to return so the loop re-evaluates
// pending perform-blocks and the cancellation flag.
static void CrossDeskRunloopWakePerform(void* info) {
  (void)info;
}

- (void)threadMainWithLayer:(CAMetalLayer*)layer {
  pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
  pthread_setname_np("CrossDesk.MetalDisplayLink");
  @autoreleasepool {
    CFRunLoopRef runloop = CFRunLoopGetCurrent();
    CFRetain(runloop);
    _runloop.store(runloop, std::memory_order_release);

    CFRunLoopSourceContext context = {};
    context.perform = &CrossDeskRunloopWakePerform;
    CFRunLoopSourceRef wake_source =
        CFRunLoopSourceCreate(kCFAllocatorDefault, 0, &context);
    CFRetain(wake_source);
    _wake_source.store(wake_source, std::memory_order_release);
    CFRunLoopAddSource(runloop, wake_source, kCFRunLoopDefaultMode);

    _link = [[CAMetalDisplayLink alloc] initWithMetalLayer:layer];
    if (_link != nil) {
      _link.delegate = self;
      // Latency 1 asks QuartzCore for the earliest drawable acquisition it
      // can phase-lock; the rate range follows Moonlight's vt_metal.mm:
      // never slower than 60 Hz, preferring the full ProMotion 120 Hz
      // cadence so 120 fps motion and 60 fps content on a 120 Hz panel both
      // present at the very next VBlank.
      _link.preferredFrameLatency = 1.0f;
      _link.preferredFrameRateRange = CAFrameRateRangeMake(60.0f, 120.0f, 120.0f);
      _link.paused = YES;
      [_link addToRunLoop:[NSRunLoop currentRunLoop]
                  forMode:NSDefaultRunLoopMode];
    }
    if (_ready != nullptr) {
      dispatch_semaphore_signal(_ready);
    }

    while (!_cancelled.load(std::memory_order_acquire)) {
      @autoreleasepool {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 1.0, true);
      }
    }

    // Normally -stop already invalidated and released the link on this
    // thread; this is the belt-and-braces path for a teardown that skipped
    // -stop, still serialized on the owning thread.
    if (_link != nil) {
      [_link invalidate];
      _link = nil;
    }
    CFRunLoopRemoveSource(runloop, wake_source, kCFRunLoopDefaultMode);
    CFRelease(wake_source);  // balances the creation inside this thread
  }
}

// Kicks the runloop out of its mach wait without ever losing the wakeup:
// the latched source signal survives the race where the bare mach wake is
// consumed before the runloop actually blocks.
- (void)kickRunloop {
  CFRunLoopSourceRef source = _wake_source.load(std::memory_order_acquire);
  CFRunLoopRef runloop = _runloop.load(std::memory_order_acquire);
  if (source != nullptr) {
    CFRunLoopSourceSignal(source);
  }
  if (runloop != nullptr) {
    CFRunLoopWakeUp(runloop);
  }
}

- (BOOL)startWithLayer:(CAMetalLayer*)layer {
  if (layer == nil) {
    return NO;
  }
  bool expected = false;
  if (!_started.compare_exchange_strong(expected, true,
                                        std::memory_order_acq_rel)) {
    return _link != nil;
  }
  _ready = dispatch_semaphore_create(0);
  // std::thread (not NSThread) keeps ownership acyclic: the running thread
  // retains self through its copied closure only until it exits, and -stop
  // joins it before the driver can be released, so no retain cycle and no
  // use-after-free in either direction. A raw pthread still gets a lazy
  // per-thread CFRunLoop, which is all CAMetalDisplayLink needs.
  _thread = std::thread([self, layer]() {
    [self threadMainWithLayer:layer];
  });
  // The semaphore publishes both the runloop and the link with full
  // happens-before ordering; two seconds is a generous bound for thread
  // spawn plus object construction.
  dispatch_semaphore_wait(
      _ready, dispatch_time(DISPATCH_TIME_NOW, 2LL * NSEC_PER_SEC));
  return _link != nil;
}

- (void)stop {
  bool expected = false;
  if (!_cancelled.compare_exchange_strong(expected, true,
                                          std::memory_order_acq_rel)) {
    if (_thread.joinable()) {
      _thread.join();
    }
    return;
  }
  CFRunLoopRef runloop = _runloop.load(std::memory_order_acquire);
  if (runloop != nullptr) {
    // Invalidate on the owning runloop thread and wait for it: because the
    // runloop serializes all blocks, once this block finishes no display
    // link callback is running or will ever run again. The wait can never
    // deadlock — render callbacks only touch mutex-protected frame state
    // and thread-safe Metal objects, never the main thread, and DetachViews
    // calls -stop before taking any frame-state lock.
    dispatch_semaphore_t invalidated = dispatch_semaphore_create(0);
    CFRunLoopPerformBlock(runloop, kCFRunLoopDefaultMode, ^{
      if (self->_link != nil) {
        [self->_link invalidate];
        self->_link = nil;
      }
      dispatch_semaphore_signal(invalidated);
    });
    [self kickRunloop];
    dispatch_semaphore_wait(
        invalidated, dispatch_time(DISPATCH_TIME_NOW, 2LL * NSEC_PER_SEC));
    // The cancellation flag is already set, so the loop exits after its
    // current iteration; the latched source signal forces that iteration to
    // end immediately even against the wake-then-sleep race.
    [self kickRunloop];
  }
  if (_thread.joinable()) {
    _thread.join();
  }
}

- (void)unpause {
  if (_cancelled.load(std::memory_order_acquire)) {
    return;
  }
  CFRunLoopRef runloop = _runloop.load(std::memory_order_acquire);
  if (runloop == nullptr) {
    return;
  }
  // The block reads _link only on the display-link thread, where invalidation
  // and release are serialized against it by the runloop itself: a block that
  // lands before teardown unpauses a live link, a block that lands after
  // finds nil and no-ops, and a block enqueued after the thread exited never
  // runs at all. The runloop object is retained by this driver for the
  // driver's whole lifetime, and every caller holds the async render state
  // that retains the driver, so the perform/wake pair always targets a valid
  // object.
  CFRunLoopPerformBlock(runloop, kCFRunLoopDefaultMode, ^{
    CAMetalDisplayLink* link = self->_link;
    if (link != nil && link.paused) {
      link.paused = NO;
    }
  });
  [self kickRunloop];
}

- (void)metalDisplayLink:(CAMetalDisplayLink*)link
             needsUpdate:(CAMetalDisplayLinkUpdate*)update {
  (void)link;
  // update.drawable arrives pre-acquired by QuartzCore: presenting it costs
  // zero nextDrawable wait. It is never presented when the tick renders
  // nothing — dropping it simply returns it to the pool, whereas committing
  // an unwritten texture would flash garbage.
  if (self.tickCounter != nil) self.tickCounter();
  BOOL rendered = NO;
  if (self.renderTick != nil && update.drawable != nil) {
    rendered = self.renderTick(update.drawable);
  }
  if (rendered) {
    _idle_ticks = 0;
  } else if (++_idle_ticks >= 2) {
    // Two consecutive ticks with no new frame and no redraw mean the remote
    // desktop is static: pause here and let the next submission wake the
    // link, so a 0 Hz desktop costs zero background wakeups.
    _idle_ticks = 0;
    link.paused = YES;
    if (self.pauseCounter != nil) self.pauseCounter();
  }
}

- (void)dealloc {
  // DetachViews always stops the driver before release; this covers a
  // teardown path that skipped it. -stop is thread-safe here because the
  // display-link thread is the one place it can never be invoked from.
  [self stop];
  CFRunLoopRef runloop = _runloop.load(std::memory_order_acquire);
  if (runloop != nullptr) {
    CFRelease(runloop);
    _runloop.store(nullptr, std::memory_order_release);
  }
  CFRunLoopSourceRef source = _wake_source.load(std::memory_order_acquire);
  if (source != nullptr) {
    // Balances the CFRetain taken when the source was published; the create
    // itself is released on the display-link thread after removal.
    CFRelease(source);
    _wake_source.store(nullptr, std::memory_order_release);
  }
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

enum class MetalPacing {
  displaylink,
  immediate,
};

// Presentation pacing select. "displaylink" (default) drives steady-state
// presentation from the vsync-locked CAMetalDisplayLink thread with pre-
// acquired drawables and zero-idle-wake pausing; "immediate" keeps the
// Wave-5C render-queue nextDrawable path that commits as soon as the decode
// callback lands, which is the lower-variance-of-work but higher-jitter-of-
// acquisition mode and remains useful for A/B latency measurement.
MetalPacing ResolveMetalPacing() {
  static const MetalPacing pacing = [] {
    const char* env = std::getenv("FARLINK_METAL_PACING");
    if (env && std::strcmp(env, "immediate") == 0) {
      return MetalPacing::immediate;
    }
    return MetalPacing::displaylink;
  }();
  return pacing;
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
    // Phase-locked presentation driver for this attachment in
    // FARLINK_METAL_PACING=displaylink mode; nil in immediate/sync mode or
    // when link creation failed. Held strongly so the driver (and therefore
    // its runloop) outlives every decode-thread unpause caller, who copies
    // this state under async_state_mutex before touching it. The driver's
    // renderTick block captures this state only weakly, breaking the
    // state -> driver -> block -> state cycle; DetachViews stops the driver
    // synchronously, so by the time the state dies the thread is already
    // joined and dealloc is trivial.
    CrossDeskMetalDisplayLinkDriver* display_link_driver = nil;
    // Latest render-stage counter sink, refreshed on every frame submission.
    // The counters themselves live inside the session's VideoLatencyStats so
    // the teardown line reads them without any harvesting; this weak pointer
    // simply lets non-frame events (display-link ticks, pauses, occlusion
    // transitions) reach the same sink. Weak because the stats object owns the
    // session lifetime and the renderer must never extend it.
    std::weak_ptr<VideoLatencyStats> latency_stats;
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
      // Stop the display-link driver before touching any window or frame
      // state. -stop joins the driver thread synchronously; it cannot
      // deadlock here because nothing below holds a frame-state lock while
      // this runs, and the in-flight display-link callback never waits on
      // the main thread. The driver object itself stays referenced by the
      // detached state — inert because unpause checks the cancellation
      // flag — until the last decode-thread reader releases it.
      if (async_state->display_link_driver != nil) {
        [async_state->display_link_driver stop];
      }
      drawable_request.reset();
      NSWindow* window = slint_view.window;
      ((CrossDeskStreamTitleView*)title_accessory_controller.view).resizeVideoHandler = nil;
      ((CrossDeskStreamTitleView*)title_accessory_controller.view).occlusionChangedHandler = nil;
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
    // Phase-locked pacing: the display-link thread renders at the next
    // VBlank with a pre-acquired drawable, so all a submission has to do is
    // wake the paused link. The unpause is a non-blocking runloop perform;
    // when the link is already running it is a harmless no-op. The driver is
    // only nil in immediate/sync mode or when creation failed at attach, in
    // which case presentation falls through to the immediate render-queue
    // path below exactly as before.
    if (state->display_link_driver != nil) {
      if (const auto stats = state->latency_stats.lock()) {
        stats->MutableRenderStages().dl_unpauses.fetch_add(1, std::memory_order_relaxed);
      }
      [state->display_link_driver unpause];
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
  // Refresh the counter sink on every submission so non-frame events (link
  // ticks, occlusion transitions) keep landing in the live session stats.
  if (timing.stats) {
    const std::shared_ptr<Impl::AsyncRenderState> state = impl_->GetAsyncState();
    state->latency_stats = timing.stats;
  }
  if (!target) {
    CFRelease(y_cv_texture);
    CFRelease(uv_cv_texture);
    timing.MarkStage(VideoFrameStage::busy);
    if (timing.stats) {
      timing.stats->MutableRenderStages().busy_drop.fetch_add(1, std::memory_order_relaxed);
    }
    return SubmitResult::dropped;
  }

  if (target->use == SlotUse::pending) {
    target->timing.MarkStage(VideoFrameStage::replaced);
    if (timing.stats) {
      timing.stats->MutableRenderStages().replaced.fetch_add(1, std::memory_order_relaxed);
    }
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
  if (timing.stats) {
    timing.stats->MutableRenderStages().submitted.fetch_add(1, std::memory_order_relaxed);
  }
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
  if (timing.stats) {
    const std::shared_ptr<Impl::AsyncRenderState> state = impl_->GetAsyncState();
    state->latency_stats = timing.stats;
  }
  if (target == nullptr) {
    timing.MarkStage(VideoFrameStage::busy);
    if (timing.stats) {
      timing.stats->MutableRenderStages().busy_drop.fetch_add(1, std::memory_order_relaxed);
    }
    return SubmitResult::dropped;
  }
  const VideoLatencyFrame replaced_timing =
      target->use == SlotUse::pending ? target->timing : VideoLatencyFrame{};
  if (!AllocateSlot(impl_->device, *target, width, height)) {
    return SubmitResult::failed;
  }
  replaced_timing.MarkStage(VideoFrameStage::replaced);
  if (replaced_timing.stats) {
    replaced_timing.stats->MutableRenderStages().replaced.fetch_add(1, std::memory_order_relaxed);
  }

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
  if (timing.stats) {
    timing.stats->MutableRenderStages().submitted.fetch_add(1, std::memory_order_relaxed);
  }
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
    // CAMetalLayer defaults to NO for a standalone sublayer, but the
    // display-link cadence contract depends on it: commits must reach the
    // compositor at the next VBlank instead of waiting for a CATransaction
    // the video surface never participates in. Pin it explicitly.
    impl_->metal_layer.presentsWithTransaction = NO;
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
    // Phase-locked presentation driver for this attachment. Creation is
    // deferred until the video view sits inside the window hierarchy with
    // layout and geometry applied: a CAMetalDisplayLink created on a layer
    // WindowServer has not composited yet can permanently attach to no
    // display at all. The object stays valid in that state, accepts
    // paused=NO, and never fires a single callback for the rest of the
    // session — recreation on the same layer does not recover it either
    // (wave-6 loopback smoke: ticks=0 across thousands of unpauses with a
    // healthy layer tree in a visible window, in roughly half of the runs;
    // zero latch-ups after this reorder). Nil in immediate/sync mode,
    // leaving the Wave-5C render-queue path as both the fallback and the
    // A/B comparison arm. The renderTick block captures the state weakly —
    // the state strongly retains the driver, and a strong capture here
    // would cycle.
    auto start_display_link_driver = [&]() {
    if (ResolveMetalPacing() == MetalPacing::displaylink &&
        !EnvSyncPresentEnabled()) {
      CrossDeskMetalDisplayLinkDriver* driver =
          [[CrossDeskMetalDisplayLinkDriver alloc] init];
      std::weak_ptr<Impl::AsyncRenderState> weak_state = fresh_state;
      driver.renderTick = ^BOOL(id<CAMetalDrawable> drawable) {
        const std::shared_ptr<Impl::AsyncRenderState> state = weak_state.lock();
        if (!state ||
            state->detached->load(std::memory_order_acquire) ||
            state->transitioning.load(std::memory_order_acquire)) {
          return NO;
        }
        const double width =
            state->raster_width.load(std::memory_order_acquire);
        const double height =
            state->raster_height.load(std::memory_order_acquire);
        if (width <= 0 || height <= 0) {
          return NO;
        }
        std::string stream;
        {
          // The selected stream is the only honest answer to "what should
          // this tick present": submissions for background streams are
          // dropped upstream, and a stream switch must take effect at the
          // very next tick rather than when the stale stream's frames run
          // out.
          std::lock_guard lock(state->frames->mutex);
          stream = state->frames->selected_stream;
        }
        if (stream.empty()) {
          return NO;
        }
        const auto outcome = Impl::RenderFrameCore(
            state, stream, width, height, true, drawable, 0);
        return outcome.result == RenderResult::rendered ||
               outcome.result == RenderResult::empty;
      };
      driver.tickCounter = ^{
        if (const std::shared_ptr<Impl::AsyncRenderState> state = weak_state.lock()) {
          if (const auto stats = state->latency_stats.lock()) {
            stats->MutableRenderStages().dl_ticks.fetch_add(1, std::memory_order_relaxed);
          }
        }
      };
      driver.pauseCounter = ^{
        if (const std::shared_ptr<Impl::AsyncRenderState> state = weak_state.lock()) {
          if (const auto stats = state->latency_stats.lock()) {
            stats->MutableRenderStages().dl_pauses.fetch_add(1, std::memory_order_relaxed);
          }
        }
      };
      if ([driver startWithLayer:impl_->metal_layer]) {
        fresh_state->display_link_driver = driver;
      } else {
        // Link creation failure must never cost the session its video: the
        // immediate render-queue path handles presentation exactly as in
        // Wave 5C, with no driver in the state.
        [driver stop];
        static std::once_flag pacing_fallback_logged;
        std::call_once(pacing_fallback_logged, [] {
          LOG_WARN("CAMetalDisplayLink unavailable; using immediate "
                   "render-queue presentation");
        });
      }
    }
    };
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
    title_view.occlusionChangedHandler = ^(NSWindowOcclusionState occlusion_state) {
      // Main thread. The render queue and display-link thread never read
      // AppKit directly, so this callback is the only path that can push the
      // fresh occlusion fact into the async state outside the per-frame
      // geometry pass — critical when the remote desktop is static and no
      // RenderLatest is running.
      const bool occluded = (occlusion_state & NSWindowOcclusionStateVisible) == 0;
      LOG_INFO("Stream window occlusion state changed: {}",
               occluded ? "occluded" : "visible");
      if (const std::shared_ptr<Impl::AsyncRenderState> state = impl_->GetAsyncState()) {
        if (const auto stats = state->latency_stats.lock()) {
          stats->MutableRenderStages().occlusion_transitions.fetch_add(1, std::memory_order_relaxed);
        }
        // Recompute visibility exactly like PublishAsyncGeometry does. The
        // raster size is unchanged (the occlusion transition carries no
        // resize), so writing only the visibility/transitioning bits is
        // sufficient — no geometry_valid churn.
        bool visible = false;
        bool transitioning = false;
        if (impl_->slint_view != nil) {
          NSWindow* slint_window = impl_->slint_view.window;
          visible = slint_window != nil && slint_window.isVisible &&
              !slint_window.isMiniaturized &&
              !(impl_->video_view != nil &&
                impl_->video_view.isHiddenOrHasHiddenAncestor) &&
              (occlusion_state & NSWindowOcclusionStateVisible) != 0;
          transitioning =
              IsStreamWindowFullscreenTransitioning((__bridge void*)impl_->slint_view);
        }
        state->visible.store(visible, std::memory_order_release);
        state->transitioning.store(transitioning, std::memory_order_release);
        // Wake the display link on un-occlusion: the link is often paused
        // after two idle ticks (the remote desktop is static), so without
        // this nudge the surface can sit unpainted until the next frame
        // arrives even though the user is looking at the window again.
        if (!occluded && state->display_link_driver != nil) {
          [state->display_link_driver unpause];
        }
      }
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
    const CGSize initial_raster = impl_->UpdateVideoGeometry(0.0);
    // The surface now sits inside the window hierarchy with real geometry;
    // creating the display link earlier than this races WindowServer's
    // first compositing pass for the layer and can latch the link onto no
    // display for the rest of the session.
    start_display_link_driver();
    {
      std::lock_guard lock(impl_->async_state_mutex);
      impl_->async_state = fresh_state;
    }
    // Publish the freshly measured raster geometry, occlusion state and
    // fullscreen-transition flag into the new async state immediately. Until
    // this point fresh_state->visible is false and raster_width/height are
    // zero, so a decode thread submitting the very first frame before the
    // main thread's first RenderLatest would either skip the render queue
    // dispatch (geometry_valid=false) or count the presentation against a
    // stale invisible state. Publishing here means the first submission sees
    // the real window state from the moment Attach returns.
    impl_->PublishAsyncGeometry(initial_raster);
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
      if (timing.stats) {
        auto& stages = timing.stats->MutableRenderStages();
        stages.present_cb.fetch_add(1, std::memory_order_relaxed);
        if (presented_time <= 0) {
          stages.present_zero_time.fetch_add(1, std::memory_order_relaxed);
        }
        if (!visible_at_submit) {
          stages.present_invisible.fetch_add(1, std::memory_order_relaxed);
        }
      }
      if (visible_at_submit && presented_at) {
        if (timing.stats) {
          timing.stats->MutableRenderStages().presented.fetch_add(1, std::memory_order_relaxed);
        }
        timing.MarkPresented(*presented_at);
        // TRUE presentation is the drawable presented handler. Exactly once
        // per session, report ms since connect request/Ice ready. The codec
        // mirror races this line: the first transport stats sample carrying
        // the decoder name typically lands a few tens of milliseconds after
        // the first presentation, so an immediate emission prints
        // codec=unknown in about half of all sessions. Defer emission while
        // the codec is still unknown, up to a 1500 ms grace measured from
        // the first presentation; past the grace emit with whatever is
        // known so the line is never lost.
        if (timing.stats && !timing.stats->FirstPresentedConsumed()) {
          const auto observability = timing.stats->GetSessionObservability();
          const bool codec_known =
              observability && !observability->LoadCodec().empty();
          const auto first_ns = timing.stats->FirstPresentedTimeNs();
          const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
              presented_at->time_since_epoch()).count();
          const bool grace_expired = first_ns > 0 &&
              now_ns - first_ns > 1500LL * 1000 * 1000;
          if ((codec_known || grace_expired) &&
              timing.stats->ConsumeFirstPresented()) {
            LogVideoFirstFramePresented(*timing.stats, *presented_at);
          }
        }
      }
    }];
    [command_buffer presentDrawable:drawable];
    timing.MarkStage(VideoFrameStage::committed, VideoLatencyFrame::Clock::now(),
                     drawable_wait_ms);
    if (timing.stats) {
      auto& stages = timing.stats->MutableRenderStages();
      stages.commits.fetch_add(1, std::memory_order_relaxed);
      if (!visible_at_submit) {
        stages.commit_invisible.fetch_add(1, std::memory_order_relaxed);
      }
    }
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
