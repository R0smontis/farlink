// IOSurface-backed native-frame source for the frame-rate harness.
//
// Production macOS capture hands VideoToolbox a zero-copy CVPixelBuffer
// reference; the harness's CPU NV12 path instead memcpy's every frame into a
// fresh pixel buffer, which inflates the measured capture+copy cost. This
// source reproduces the production path: it pre-allocates a small pool of
// IOSurface-backed CVPixelBuffers, copies the harness noise template into each
// once, and per frame only rewrites the tiny sequence barcode before submitting
// the buffer by reference — exactly what ScreenCaptureKit does. Comparing a run
// in this mode against the CPU mode isolates how much of the harness ceiling is
// the copy artifact versus real encoder/pipeline cost.
//
// macOS-only; the harness falls back to the CPU path elsewhere.

#ifndef CROSSDESK_IOSURFACE_FRAME_SOURCE_H_
#define CROSSDESK_IOSURFACE_FRAME_SOURCE_H_

#include <cstdint>
#include <cstddef>

#include "minirtc.h"

namespace crossdesk {

// Owns a pool of IOSurface-backed CVPixelBuffers. All methods are safe to call
// from the single harness submit thread; the retain/release/copy callbacks
// handed to the library are safe on the library's worker threads.
class IoSurfaceFrameSource {
 public:
  // Creates count buffers of width x height NV12, seeding each with the packed
  // NV12 noise template (width*height*3/2 bytes) so encode cost matches CPU
  // mode. Returns null on failure or when count exceeds the slot capacity.
  static IoSurfaceFrameSource* Create(int width, int height, int count,
                                      const uint8_t* noise_template);

  // Fills `frame` with a native CVPixelBuffer descriptor for slot `slot` after
  // stamping `sequence` into its luma barcode. width/height/captured_timestamp
  // mirror the CPU path. The buffer stays owned by the pool; the library
  // retains it through the descriptor callbacks as needed.
  void FillFrame(int slot, uint32_t sequence, uint64_t captured_timestamp,
                 MiniRtcVideoFrame* frame);

  // Packed NV12 byte count for the configured dimensions.
  size_t FrameBytes() const { return frame_bytes_; }

  void Destroy();

  int width_ = 0;
  int height_ = 0;
  int count_ = 0;
  size_t frame_bytes_ = 0;
  // One retained CVPixelBufferRef per slot, stored as opaque pointers so this
  // header stays pure C++. Owned by the pool until Destroy().
  void* buffers_[16] = {};
  // One persistent descriptor per slot. The library copies the descriptor and
  // retains the owner synchronously inside the send call, so these only need to
  // outlive the pool — which they do as members. Keeping them per-slot rather
  // than a shared static means concurrent submissions of different slots never
  // write the same storage.
  MiniRtcNativeVideoFrame descriptors_[16] = {};
};

}  // namespace crossdesk

#endif  // CROSSDESK_IOSURFACE_FRAME_SOURCE_H_
