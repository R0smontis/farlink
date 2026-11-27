// macOS Wave-6C verification on real Apple Silicon hardware:
//
//   part 1  Full-range BT.709 (420f + ITU_R_709_2) versus limited-range
//           BT.601 (420v + ITU_R_601_4) color and gradation fidelity through
//           the exact production Metal shader. A 256-step dark-UI grayscale
//           ramp plus saturated sRGB primaries are encoded the way each
//           capture mode would deliver them, rendered on the real GPU, and
//           read back to count distinct/monotonic output levels and measure
//           per-channel error.
//   part 2  CAMetalDisplayLink lifecycle: the production driver class
//           compiled into this binary from metal_video_renderer.mm runs on
//           its dedicated QOS_CLASS_USER_INTERACTIVE runloop thread, fires
//           with a pre-acquired non-nil drawable and valid
//           targetPresentationTimestamp, auto-pauses after two idle ticks,
//           wakes on unpause(), and cleanly invalidates + joins across 20
//           rapid attach/detach stress cycles with no leaked threads.
//
// The production renderer translation unit is compiled into this test
// binary directly so the driver under test is the exact code the app runs;
// its five external project symbols are stubbed below (the test never
// attaches a real Slint window, so the window-chrome helpers are never
// exercised).
//
// Not part of the default build. Run on an Apple Silicon macOS host:
//   xmake build macos_color_and_displaylink_test && \
//   xmake run macos_color_and_displaylink_test

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <mach/mach.h>

#import "platform/macos/gui/metal_cas_shader_source.h"

// ---------------------------------------------------------------------------
// Stubs for the five project symbols metal_video_renderer.mm references.
// ---------------------------------------------------------------------------

#include <mach/mach.h>

#include "spdlog/spdlog.h"

namespace crossdesk {

std::shared_ptr<spdlog::logger> get_logger() {
  static std::shared_ptr<spdlog::logger> logger = [] {
    auto instance = std::make_shared<spdlog::logger>("w6c_test");
    instance->set_level(spdlog::level::off);
    return instance;
  }();
  return logger;
}

// The test never attaches a window: the stream-window chrome helpers are
// unreachable, but the linker still needs them.
bool IsStreamWindowFullscreen(void*) { return false; }
bool IsStreamWindowFullscreenTransitioning(void*) { return false; }
float StreamWindowVideoTransitionInset(void*, float, float,
                                       bool* transitioning) {
  if (transitioning) *transitioning = false;
  return 0.0f;
}

class VideoLatencyStats;
void LogVideoFirstFramePresented(const VideoLatencyStats&,
                                 std::chrono::steady_clock::time_point) {}

}  // namespace crossdesk

// ---------------------------------------------------------------------------
// Shared test scaffolding.
// ---------------------------------------------------------------------------

namespace {

int g_failures = 0;

void Check(bool condition, const char* what) {
  if (condition) {
    std::printf("  PASS %s\n", what);
  } else {
    std::printf("  FAIL %s\n", what);
    ++g_failures;
  }
}

void Info(const char* what) { std::printf("  info %s\n", what); }

struct Gpu {
  id<MTLDevice> device = nil;
  id<MTLCommandQueue> queue = nil;
  id<MTLLibrary> library = nil;
  id<MTLRenderPipelineState> pipeline = nil;
};

bool SetupGpu(Gpu& gpu) {
  gpu.device = MTLCreateSystemDefaultDevice();
  if (!gpu.device) {
    std::printf("  FAIL no Metal device\n");
    return false;
  }
  gpu.queue = [gpu.device newCommandQueue];
  NSError* error = nil;
  gpu.library = [gpu.device newLibraryWithSource:ProductionNv12ShaderSource()
                                         options:nil
                                           error:&error];
  if (!gpu.library) {
    std::printf("  FAIL shader compile: %s\n",
                error.localizedDescription.UTF8String);
    return false;
  }
  id<MTLFunction> vertex = [gpu.library newFunctionWithName:@"nv12_vertex"];
  id<MTLFunction> fragment = [gpu.library newFunctionWithName:@"nv12_fragment"];
  MTLRenderPipelineDescriptor* descriptor =
      [[MTLRenderPipelineDescriptor alloc] init];
  descriptor.vertexFunction = vertex;
  descriptor.fragmentFunction = fragment;
  descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
  gpu.pipeline = [gpu.device newRenderPipelineStateWithDescriptor:descriptor
                                                          error:&error];
  if (!gpu.pipeline) {
    std::printf("  FAIL pipeline: %s\n", error.localizedDescription.UTF8String);
    return false;
  }
  return true;
}

struct ShaderColor {
  uint32_t full_range;
  uint32_t bt709;
  float uv_center;
  float sharpness;
};

id<MTLTexture> MakePlaneTexture(Gpu& gpu, const uint8_t* data, int width,
                                int height, int bytes_per_row,
                                MTLPixelFormat format) {
  MTLTextureDescriptor* descriptor =
      [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                                         width:width
                                                        height:height
                                                     mipmapped:NO];
  descriptor.storageMode = MTLStorageModeShared;
  descriptor.usage = MTLTextureUsageShaderRead;
  id<MTLTexture> texture = [gpu.device newTextureWithDescriptor:descriptor];
  [texture replaceRegion:MTLRegionMake2D(0, 0, width, height)
             mipmapLevel:0
               withBytes:data
             bytesPerRow:bytes_per_row];
  return texture;
}

std::vector<uint8_t> RenderOffscreen(
    Gpu& gpu, id<MTLTexture> y_texture, id<MTLTexture> uv_texture,
    ShaderColor color, int out_w, int out_h) {
  MTLTextureDescriptor* target_descriptor =
      [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                         width:out_w
                                                        height:out_h
                                                     mipmapped:NO];
  target_descriptor.storageMode = MTLStorageModeShared;
  target_descriptor.usage =
      MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
  id<MTLTexture> target = [gpu.device newTextureWithDescriptor:target_descriptor];

  MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
  pass.colorAttachments[0].texture = target;
  pass.colorAttachments[0].loadAction = MTLLoadActionClear;
  pass.colorAttachments[0].storeAction = MTLStoreActionStore;
  pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);

  id<MTLCommandBuffer> command = [gpu.queue commandBuffer];
  id<MTLRenderCommandEncoder> encoder =
      [command renderCommandEncoderWithDescriptor:pass];
  [encoder setRenderPipelineState:gpu.pipeline];
  [encoder setFragmentBytes:&color length:sizeof(color) atIndex:0];
  [encoder setFragmentTexture:y_texture atIndex:0];
  [encoder setFragmentTexture:uv_texture atIndex:1];
  [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
  [encoder endEncoding];
  [command commit];
  [command waitUntilCompleted];
  std::vector<uint8_t> pixels(static_cast<size_t>(out_w) * out_h * 4);
  [target getBytes:pixels.data()
       bytesPerRow:static_cast<size_t>(out_w) * 4
        fromRegion:MTLRegionMake2D(0, 0, out_w, out_h)
       mipmapLevel:0];
  return pixels;
}

// ---------------------------------------------------------------------------
// Part 1: color and gradation.
// ---------------------------------------------------------------------------

// Encodes one horizontal scanline ramp whose columns sweep luma codes 0..255.
// |video_range| reproduces what each capture mode delivers: full-range 420f
// preserves the code verbatim; limited-range 420v crushes it into the
// 16..235 studio swing before encode. Chroma is neutral throughout.
void MakeRampNv12(int width, int height, bool video_range,
                  std::vector<uint8_t>& y_plane,
                  std::vector<uint8_t>& uv_plane) {
  y_plane.assign(static_cast<size_t>(width) * height, 0);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const int code = x * 255 / (width - 1);
      uint8_t encoded;
      if (video_range) {
        encoded = static_cast<uint8_t>(16 + (code * 219 + 127) / 255);
      } else {
        encoded = static_cast<uint8_t>(code);
      }
      y_plane[static_cast<size_t>(y) * width + x] = encoded;
    }
  }
  uv_plane.assign(static_cast<size_t>(width) * height / 2, 128);
}

// Encodes a constant-color patch of BT.709 full-range YCbCr using the exact
// inverse of the production shader's reconstruction matrix:
//   y  = 0.2126 r + 0.7152 g + 0.0722 b        (all in 0..1)
//   cb = (b - y) / 1.8556,  cr = (r - y) / 1.5748
// Chroma is written into every subsampled texel, so the constant patch is
// immune to 4:2:0 cositing.
void MakeSolid709FullRangeNv12(int width, int height, float r, float g,
                               float b, std::vector<uint8_t>& y_plane,
                               std::vector<uint8_t>& uv_plane) {
  const float y = 0.2126f * r + 0.7152f * g + 0.0722f * b;
  const float cb = (b - y) / 1.8556f;
  const float cr = (r - y) / 1.5748f;
  const auto clamp8 = [](float v) {
    return static_cast<uint8_t>(std::clamp(v, 0.0f, 255.0f) + 0.5f);
  };
  y_plane.assign(static_cast<size_t>(width) * height, clamp8(y * 255.0f));
  uv_plane.assign(static_cast<size_t>(width) * height / 2, 0);
  for (size_t i = 0; i < uv_plane.size(); i += 2) {
    uv_plane[i] = clamp8(128.0f + cb * 255.0f);
    uv_plane[i + 1] = clamp8(128.0f + cr * 255.0f);
  }
}

int RunColorTests(Gpu& gpu) {
  std::printf("part 1: full-range BT.709 vs limited-range BT.601\n");

  constexpr int kRampW = 512;   // two output pixels per luma code
  constexpr int kRampH = 64;

  // --- 420f + BT.709: every one of the 256 luma codes survives. -----------
  std::vector<uint8_t> y_full, uv_full;
  MakeRampNv12(kRampW, kRampH, false, y_full, uv_full);
  id<MTLTexture> y_tex_full = MakePlaneTexture(
      gpu, y_full.data(), kRampW, kRampH, kRampW, MTLPixelFormatR8Unorm);
  id<MTLTexture> uv_tex_full = MakePlaneTexture(
      gpu, uv_full.data(), kRampW / 2, kRampH / 2, kRampW, MTLPixelFormatRG8Unorm);
  ShaderColor color_full{1, 1, 128.0f / 255.0f, 0.0f};
  const auto out_full =
      RenderOffscreen(gpu, y_tex_full, uv_tex_full, color_full, kRampW, kRampH);

  // Sample the middle row's green channel (achromatic: r=g=b), folding the
  // two pixel columns of each code into one measurement.
  std::vector<int> measured_full(256, -1);
  for (int x = 0; x < kRampW; ++x) {
    const int code = x * 255 / (kRampW - 1);
    const int g = out_full[(static_cast<size_t>(kRampH / 2) * kRampW + x) * 4 + 1];
    measured_full[code] = g;
  }
  int distinct_full = 0;
  int monotonic_full = 1;
  int max_err_full = 0;
  int crushed_dark_full = 0;   // codes 0..31 sharing one output value
  int crushed_high_full = 0;   // codes 224..255 sharing one output value
  {
    int prev = -1;
    for (int i = 0; i < 256; ++i) {
      if (measured_full[i] != prev) {
        ++distinct_full;
        if (prev >= 0 && measured_full[i] < prev) monotonic_full = 0;
        prev = measured_full[i];
      }
      max_err_full = std::max(max_err_full, std::abs(measured_full[i] - i));
    }
    for (int i = 1; i < 32; ++i)
      if (measured_full[i] == measured_full[0]) ++crushed_dark_full;
    for (int i = 225; i < 256; ++i)
      if (measured_full[i] == measured_full[224]) ++crushed_high_full;
  }

  // --- 420v + BT.601: the studio swing quantizes the ramp. ----------------
  std::vector<uint8_t> y_lim, uv_lim;
  MakeRampNv12(kRampW, kRampH, true, y_lim, uv_lim);
  id<MTLTexture> y_tex_lim = MakePlaneTexture(
      gpu, y_lim.data(), kRampW, kRampH, kRampW, MTLPixelFormatR8Unorm);
  id<MTLTexture> uv_tex_lim = MakePlaneTexture(
      gpu, uv_lim.data(), kRampW / 2, kRampH / 2, kRampW, MTLPixelFormatRG8Unorm);
  ShaderColor color_lim{0, 0, 128.0f / 255.0f, 0.0f};
  const auto out_lim =
      RenderOffscreen(gpu, y_tex_lim, uv_tex_lim, color_lim, kRampW, kRampH);

  std::vector<int> measured_lim(256, -1);
  for (int x = 0; x < kRampW; ++x) {
    const int code = x * 255 / (kRampW - 1);
    const int g = out_lim[(static_cast<size_t>(kRampH / 2) * kRampW + x) * 4 + 1];
    measured_lim[code] = g;
  }
  int distinct_lim = 0;
  int crushed_dark_lim = 0;
  int crushed_high_lim = 0;
  {
    int prev = -1;
    for (int i = 0; i < 256; ++i) {
      if (measured_lim[i] != prev) {
        ++distinct_lim;
        prev = measured_lim[i];
      }
    }
    // "Crushed" in limited range means adjacent input codes quantize onto
    // the same 16..235 code and therefore the same output level.
    for (int i = 1; i < 32; ++i)
      if (measured_lim[i] == measured_lim[i - 1]) ++crushed_dark_lim;
    for (int i = 225; i < 256; ++i)
      if (measured_lim[i] == measured_lim[i - 1]) ++crushed_high_lim;
  }

  std::printf("  info 420f+BT.709: distinct=%d monotonic=%d max_err=%d LSB, "
              "dark crush duplicates=%d/31, high duplicates=%d/31\n",
              distinct_full, monotonic_full, max_err_full,
              crushed_dark_full, crushed_high_full);
  std::printf("  info 420v+BT.601: distinct=%d, dark duplicates=%d/31, "
              "high duplicates=%d/31\n",
              distinct_lim, crushed_dark_lim, crushed_high_lim);

  Check(distinct_full == 256,
        "420f+BT.709 reproduces all 256 distinct ramp levels");
  Check(monotonic_full == 1, "420f+BT.709 ramp output is monotonic");
  Check(max_err_full <= 1, "420f+BT.709 max roundtrip error <= 1 LSB");
  Check(crushed_dark_full == 0,
        "420f+BT.709 crushes zero dark-UI codes in 0..31");
  Check(distinct_lim <= 221,
        "420v+BT.601 is limited to ~220 quantized steps");
  Check(distinct_lim < distinct_full,
        "420v loses gradation versus 420f");
  Check(crushed_dark_lim > 0 || crushed_high_lim > 0,
        "420v visibly quantizes the dark/high ramp tails");

  // --- Saturated sRGB primaries through BT.709 full-range. ----------------
  constexpr int kPatchW = 64;
  constexpr int kPatchH = 64;
  struct Primary {
    const char* name;
    float r, g, b;
    int expect_r, expect_g, expect_b;
  };
  const Primary primaries[] = {
      {"#FF0000", 1.0f, 0.0f, 0.0f, 255, 0, 0},
      {"#00FF00", 0.0f, 1.0f, 0.0f, 0, 255, 0},
      {"#0000FF", 0.0f, 0.0f, 1.0f, 0, 0, 255},
  };
  int worst_channel_error = 0;
  for (const auto& primary : primaries) {
    std::vector<uint8_t> y_p, uv_p;
    MakeSolid709FullRangeNv12(kPatchW, kPatchH, primary.r, primary.g,
                              primary.b, y_p, uv_p);
    id<MTLTexture> y_tex = MakePlaneTexture(
        gpu, y_p.data(), kPatchW, kPatchH, kPatchW, MTLPixelFormatR8Unorm);
    id<MTLTexture> uv_tex = MakePlaneTexture(
        gpu, uv_p.data(), kPatchW / 2, kPatchH / 2, kPatchW,
        MTLPixelFormatRG8Unorm);
    const auto out =
        RenderOffscreen(gpu, y_tex, uv_tex, color_full, kPatchW, kPatchH);
    const size_t center =
        (static_cast<size_t>(kPatchH / 2) * kPatchW + kPatchW / 2) * 4;
    const int b_out = out[center + 0];
    const int g_out = out[center + 1];
    const int r_out = out[center + 2];
    const int err = std::max({std::abs(r_out - primary.expect_r),
                              std::abs(g_out - primary.expect_g),
                              std::abs(b_out - primary.expect_b)});
    worst_channel_error = std::max(worst_channel_error, err);
    std::printf("  info %s -> rgb(%d,%d,%d), max channel error %d LSB\n",
                primary.name, r_out, g_out, b_out, err);
  }
  Check(worst_channel_error <= 1,
        "sRGB primaries reconstruct through 420f+BT.709 within 1 LSB");
  return 0;
}

// ---------------------------------------------------------------------------
// Part 2: CAMetalDisplayLink lifecycle and stress.
// ---------------------------------------------------------------------------

}  // namespace

// The production driver class is defined inside metal_video_renderer.mm,
// which this target compiles in. Re-declare its interface here; the runtime
// resolves the exact production implementation.
API_AVAILABLE(macos(14.0))
@interface CrossDeskMetalDisplayLinkDriver : NSObject <CAMetalDisplayLinkDelegate>
@property(nonatomic, copy) BOOL (^renderTick)(id<CAMetalDrawable> drawable);
- (BOOL)startWithLayer:(CAMetalLayer*)layer;
- (void)stop;
- (void)unpause;
@end

namespace {

mach_port_t CountThreads() {
  thread_act_array_t threads = nullptr;
  mach_msg_type_number_t count = 0;
  if (task_threads(mach_task_self(), &threads, &count) != KERN_SUCCESS) {
    return 0;
  }
  for (mach_msg_type_number_t i = 0; i < count; ++i) {
    mach_port_deallocate(mach_task_self(), threads[i]);
  }
  vm_deallocate(mach_task_self(),
                reinterpret_cast<vm_address_t>(threads),
                count * sizeof(thread_act_t));
  return count;
}

struct TickStats {
  std::atomic<int> ticks{0};
  std::atomic<int> nil_drawables{0};
  std::atomic<int> bad_timestamps{0};
  std::atomic<double> first_target{-1.0};
  std::atomic<double> last_target{0.0};
};

// Pumping the main runloop instead of sleeping keeps WindowServer
// compositing and AppKit window bookkeeping alive in a process that never
// enters NSApplicationMain; the display link's measured cadence is the
// whole point of the test, so its environment has to be honest.
void PumpRunloop(double seconds) {
  const auto deadline = std::chrono::steady_clock::now() +
      std::chrono::duration<double>(seconds);
  while (std::chrono::steady_clock::now() < deadline) {
    @autoreleasepool {
      [[NSRunLoop mainRunLoop]
          runMode:NSDefaultRunLoopMode
          beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
    }
  }
}

int RunDisplayLinkTests() {
  std::printf("part 2: CAMetalDisplayLink lifecycle and stress\n");
  @autoreleasepool {
    // CAMetalDisplayLink phase-locks against the physical display its layer
    // is composited on; a windowless or occluded surface either never fires
    // or degrades to a throttled cadence. The test process owns no running
    // NSApplication event loop by default, so WindowServer would never
    // composite the window at full rate: activate an accessory application
    // and pump the main runloop in every wait window below.
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
    [NSApp finishLaunching];
    NSRect frame = NSMakeRect(200, 400, 128, 128);
    NSWindow* window = [[NSWindow alloc]
        initWithContentRect:frame
                  styleMask:NSWindowStyleMaskTitled
                    backing:NSBackingStoreBuffered
                      defer:NO];
    [window orderFrontRegardless];
    // NSWindow defaults to releasing itself on close; the strong local
    // reference would then double-release it at scope exit, which is the
    // autorelease-pool crash this line prevents.
    window.releasedWhenClosed = NO;
    [window display];
    PumpRunloop(0.2);
    NSView* content = window.contentView;
    content.wantsLayer = YES;
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    CAMetalLayer* layer = [CAMetalLayer layer];
    layer.device = device;
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    layer.framebufferOnly = YES;
    layer.drawableSize = CGSizeMake(96, 96);
    layer.frame = content.bounds;
    [content.layer addSublayer:layer];

    Check(layer.presentsWithTransaction == NO,
          "CAMetalLayer presentsWithTransaction defaults to NO");

    // --- single lifecycle: start, fire, idle-pause, wake, stop ------------
    {
      auto stats = std::make_shared<TickStats>();
      CrossDeskMetalDisplayLinkDriver* driver =
          [[CrossDeskMetalDisplayLinkDriver alloc] init];
      driver.renderTick = ^BOOL(id<CAMetalDrawable> drawable) {
        if (drawable == nil) {
          stats->nil_drawables.fetch_add(1, std::memory_order_relaxed);
          return NO;
        }
        stats->ticks.fetch_add(1, std::memory_order_relaxed);
        // Present the pre-acquired drawable unmodified: committing nothing
        // would recycle it silently, while the pool contract here only
        // needs it returned so the cadence keeps flowing.
        [drawable present];
        return YES;
      };
      if (![driver startWithLayer:layer]) {
        Check(false, "display link driver starts on a window-backed layer");
      } else {
        Check(true, "display link driver starts on a window-backed layer");
        [driver unpause];
        // ~300ms at 60Hz+ should yield several ticks.
        PumpRunloop(0.3);
        const int ticks_active = stats->ticks.load(std::memory_order_relaxed);
        std::printf("  info active-phase ticks in 300ms: %d\n", ticks_active);
        Check(ticks_active >= 3,
              "unpaused display link fires repeatedly with non-nil drawable");
        Check(stats->nil_drawables.load(std::memory_order_relaxed) == 0,
              "every update arrives with a pre-acquired drawable");

        // Idle: renderTick starts returning NO, two consecutive ticks pause.
        driver.renderTick = ^BOOL(id<CAMetalDrawable> drawable) {
          stats->ticks.fetch_add(1, std::memory_order_relaxed);
          if (drawable == nil) {
            stats->nil_drawables.fetch_add(1, std::memory_order_relaxed);
          } else {
            [drawable present];
          }
          return NO;
        };
        PumpRunloop(0.15);
        const int before_idle = stats->ticks.load(std::memory_order_relaxed);
        PumpRunloop(0.15);
        const int after_idle = stats->ticks.load(std::memory_order_relaxed);
        std::printf("  info idle-window extra ticks: %d (expect 0 after "
                    "auto-pause settles)\n",
                    after_idle - before_idle);
        Check(after_idle - before_idle <= 1,
              "display link auto-pauses after two idle ticks (0Hz zero-power)");

        // Wake on demand: unpause must restart the cadence.
        driver.renderTick = ^BOOL(id<CAMetalDrawable> drawable) {
          stats->ticks.fetch_add(1, std::memory_order_relaxed);
          if (drawable != nil) [drawable present];
          return YES;
        };
        [driver unpause];
        PumpRunloop(0.15);
        const int after_wake = stats->ticks.load(std::memory_order_relaxed);
        std::printf("  info post-wake ticks in 150ms: %d\n",
                    after_wake - after_idle);
        Check(after_wake - after_idle >= 2,
              "unpause wakes the paused display link");

        const auto stop_start = std::chrono::steady_clock::now();
        [driver stop];
        const double stop_ms =
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - stop_start).count();
        std::printf("  info driver stop (invalidate+join) took %.2f ms\n",
                    stop_ms);
        Check(stop_ms < 20.0, "invalidate + thread join completes in < 20 ms");
      }
    }

    // --- stress: 20 rapid attach/detach cycles ----------------------------
    {
      const mach_port_t threads_before = CountThreads();
      double worst_stop_ms = 0;
      double total_ms = 0;
      int cycles_ok = 0;
      auto stress_ticks = std::make_shared<std::atomic<int>>(0);
      for (int cycle = 0; cycle < 20; ++cycle) {
        @autoreleasepool {
          CrossDeskMetalDisplayLinkDriver* driver =
              [[CrossDeskMetalDisplayLinkDriver alloc] init];
          driver.renderTick = ^BOOL(id<CAMetalDrawable> drawable) {
            if (drawable != nil) {
              stress_ticks->fetch_add(1, std::memory_order_relaxed);
              [drawable present];
            }
            return YES;
          };
          const auto cycle_start = std::chrono::steady_clock::now();
          if (![driver startWithLayer:layer]) {
            continue;
          }
          [driver unpause];
          // A few milliseconds of cadence, then immediate teardown: the
          // worst case for invalidate-while-callback-in-flight races.
          PumpRunloop(0.012);
          const auto stop_start = std::chrono::steady_clock::now();
          [driver stop];
          const double stop_ms =
              std::chrono::duration<double, std::milli>(
                  std::chrono::steady_clock::now() - stop_start).count();
          worst_stop_ms = std::max(worst_stop_ms, stop_ms);
          total_ms += std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - cycle_start)
                          .count();
          ++cycles_ok;
        }
      }
      // Let any straggler thread bookkeeping drain before counting.
      PumpRunloop(0.1);
      const mach_port_t threads_after = CountThreads();
      std::printf("  info stress: %d/20 cycles ok, worst stop %.2f ms, "
                  "mean cycle %.2f ms, ticks %d, threads %u -> %u\n",
                  cycles_ok, worst_stop_ms, total_ms / 20.0,
                  stress_ticks->load(std::memory_order_relaxed),
                  threads_before, threads_after);
      Check(cycles_ok == 20, "all 20 rapid attach/detach cycles complete");
      Check(worst_stop_ms < 20.0,
            "every invalidate+join in stress completes in < 20 ms");
      Check(stress_ticks->load(std::memory_order_relaxed) > 0,
            "stress cycles deliver real prebound-drawable callbacks");
      Check(threads_after <= threads_before + 1,
            "no leaked display-link threads after 20 cycles");
    }

    [window close];
  }
  return 0;
}

}  // namespace

int main() {
  @autoreleasepool {
    std::printf("macos_color_and_displaylink_test\n");
    Gpu gpu;
    if (!SetupGpu(gpu)) return 1;
    std::printf("  PASS shader compiles with zero errors on %s\n",
                gpu.device.name.UTF8String);
    RunColorTests(gpu);
    RunDisplayLinkTests();
    if (g_failures == 0) {
      std::printf("RESULT: all checks passed\n");
    } else {
      std::printf("RESULT: %d check(s) failed\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
  }
}
