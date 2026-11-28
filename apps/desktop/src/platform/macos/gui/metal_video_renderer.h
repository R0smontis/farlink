#ifndef CROSSDESK_GUI_PLATFORM_METAL_VIDEO_RENDERER_H_
#define CROSSDESK_GUI_PLATFORM_METAL_VIDEO_RENDERER_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "platform/video_renderer.h"

namespace crossdesk {

// macOS video compositor used by CrossDesk only. VideoToolbox CVPixelBuffers
// are sampled directly through CVMetalTextureCache; packed CPU NV12 remains a
// fallback for software decoding and cached snapshots. AppKit attachment is a
// main-thread operation and frame submission is safe from MiniRTC's decode
// callback thread. Presentation runs on a dedicated user-interactive render
// queue so a busy WindowServer drawable pool never blocks the Slint UI or
// input delivery on the main thread; set FARLINK_METAL_SYNC_PRESENT=1 to fall
// back to legacy main-thread presentation for debugging.
class MacMetalVideoRenderer final : public VideoRenderer {
 public:
  MacMetalVideoRenderer();
  ~MacMetalVideoRenderer() override;

  MacMetalVideoRenderer(const MacMetalVideoRenderer&) = delete;
  MacMetalVideoRenderer& operator=(const MacMetalVideoRenderer&) = delete;

  bool IsReady() const override;
  bool IsActive() const override;

  // The selected stream is the only stream uploaded. Frames for background
  // tabs are intentionally dropped to avoid wasting memory bandwidth.
  bool SetSelectedStream(std::string remote_id) override;
  void DiscardStream(std::string_view remote_id) override;

  // Copies one tightly packed NV12 frame into a free shared Metal slot.
  SubmitResult SubmitNv12(std::string_view remote_id, const uint8_t* data,
                          size_t size, int width, int height,
                          VideoLatencyFrame timing = {}) override;

  // Retains an IOSurface-backed VideoToolbox CVPixelBuffer until the Metal
  // command buffer using it has completed.
  SubmitResult SubmitNativeFrame(std::string_view remote_id,
                                 const MiniRtcNativeVideoFrame& frame,
                                 VideoLatencyFrame timing = {}) override;

  // Seeds a newly selected stream from its retained CPU snapshot without
  // replacing a newer decoded frame that is already queued or rendering.
  SubmitResult SubmitCachedNv12(std::string_view remote_id, const uint8_t* data,
                                size_t size, int width, int height,
                                VideoLatencyFrame timing = {}) override;

  // Attaches a native CAMetalLayer-backed sibling below Slint's NSView and
  // restores the containing AppKit window's ordinary opaque titlebar.
  // |slint_view| is an NSView* kept opaque so this header remains valid C++.
  bool Attach(void* slint_view, std::function<void()> drawable_ready = {});
  void Detach();
  bool IsAttached() const;
  void SetTitlebarControlCenterCallback(std::function<void(float anchor_x)> callback);
  void SetTitlebarControlCenterActive(bool active);

  // Cheap main-thread geometry check used by the UI frame timer. New decoded
  // frames are tracked by GuiRuntime's dirty flag; this covers native resize,
  // scale, stream selection, and retry work without a full session sync.
  bool NeedsSurfaceRedraw() const;

  // Publishes the AppKit geometry for |remote_id| and, outside fullscreen
  // transitions, dispatches presentation onto the renderer's dedicated
  // user-interactive queue instead of drawing inline. The returned outcome
  // is harvested from the newest frame the queue actually committed, so UI
  // bookkeeping stays exact even though presentation no longer runs on the
  // calling thread. top_inset_points reserves Slint's tab strip above the
  // native video surface.
  RenderOutcome RenderLatest(std::string_view remote_id,
                             double top_inset_points);

  // Used only when a session closes, so thumbnail generation does not require
  // a second per-frame CPU copy during normal playback.
  bool CopyLatestNv12(std::string_view remote_id,
                      std::vector<unsigned char>* output, int* width,
                      int* height) const override;

 private:
  RenderOutcome DrawFrame(std::string_view remote_id, double raster_width,
                          double raster_height, bool allow_cached_frame);
  SubmitResult SubmitNv12Internal(std::string_view remote_id,
                                  const uint8_t* data, size_t size, int width,
                                  int height, bool replace_pending,
                                  VideoLatencyFrame timing);

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace crossdesk

#endif  // CROSSDESK_GUI_PLATFORM_METAL_VIDEO_RENDERER_H_
