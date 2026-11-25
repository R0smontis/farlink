// IOSurface-backed native-frame source for the frame-rate harness. See the
// header for why this exists: it reproduces the zero-copy production capture
// path so the harness can measure the real encoder ceiling instead of the CPU
// memcpy artifact. The retain/release/copy_to_nv12 callbacks mirror
// screen_capturer_sck_impl.mm exactly so the library cannot tell the
// difference.

#import <CoreVideo/CoreVideo.h>
#import <IOSurface/IOSurface.h>

#include <cstring>

#include "iosurface_frame_source.h"

namespace crossdesk {
namespace {

constexpr size_t kMaxSlots = 16;

void RetainPoolPixelBuffer(void* owner) {
  if (owner) {
    CVPixelBufferRetain(static_cast<CVPixelBufferRef>(owner));
  }
}

void ReleasePoolPixelBuffer(void* owner) {
  if (owner) {
    CVPixelBufferRelease(static_cast<CVPixelBufferRef>(owner));
  }
}

// Copy the IOSurface-backed planes into packed CPU NV12. Identical logic to the
// production capture copy: row-by-row because the IOSurface stride is padded.
int CopyPoolPixelBufferToNv12(void* owner, uint8_t* destination,
                              size_t destination_size) {
  auto pixel_buffer = static_cast<CVPixelBufferRef>(owner);
  if (!pixel_buffer || !destination ||
      !CVPixelBufferIsPlanar(pixel_buffer) ||
      CVPixelBufferGetPlaneCount(pixel_buffer) < 2) {
    return -1;
  }
  const OSType pixel_format = CVPixelBufferGetPixelFormatType(pixel_buffer);
  if (pixel_format != kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange &&
      pixel_format != kCVPixelFormatType_420YpCbCr8BiPlanarFullRange) {
    return -1;
  }
  const size_t width = CVPixelBufferGetWidth(pixel_buffer);
  const size_t height = CVPixelBufferGetHeight(pixel_buffer);
  const size_t required_size = width * height * 3U / 2U;
  if (destination_size < required_size ||
      CVPixelBufferLockBaseAddress(pixel_buffer, kCVPixelBufferLock_ReadOnly) !=
          kCVReturnSuccess) {
    return -1;
  }
  const auto* y_plane = static_cast<const uint8_t*>(
      CVPixelBufferGetBaseAddressOfPlane(pixel_buffer, 0));
  const auto* uv_plane = static_cast<const uint8_t*>(
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
    uint8_t* uv_destination = destination + width * height;
    for (size_t row = 0; row < height / 2U; ++row) {
      std::memcpy(uv_destination + row * width, uv_plane + row * uv_stride,
                  width);
    }
  }
  CVPixelBufferUnlockBaseAddress(pixel_buffer, kCVPixelBufferLock_ReadOnly);
  return result;
}

// Stamp the sequence barcode into the luma plane, matching the harness CPU
// path bit-for-bit so latency pairing works identically. Only the top rows are
// touched; the rest of the buffer keeps the baked noise pattern.
void StampSequenceIntoBuffer(CVPixelBufferRef pixel_buffer, int width,
                             uint32_t sequence) {
  constexpr int kSequenceBits = 32;
  const int cell = width / 48 < 8 ? 8 : width / 48;
  if ((kSequenceBits + 2) * cell > width) return;
  if (CVPixelBufferLockBaseAddress(pixel_buffer, 0) != kCVReturnSuccess) return;
  auto* y_plane =
      static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pixel_buffer, 0));
  const size_t y_stride = CVPixelBufferGetBytesPerRowOfPlane(pixel_buffer, 0);
  if (y_plane) {
    for (int bit = 0; bit < kSequenceBits; ++bit) {
      const bool one = (sequence >> bit) & 1u;
      const uint8_t value = one ? 235 : 16;
      const int x0 = (bit + 1) * cell;
      for (int row = 0; row < cell; ++row) {
        std::memset(y_plane + static_cast<size_t>(row) * y_stride + x0, value,
                    cell);
      }
    }
  }
  CVPixelBufferUnlockBaseAddress(pixel_buffer, 0);
}

}  // namespace

IoSurfaceFrameSource* IoSurfaceFrameSource::Create(
    int width, int height, int count, const uint8_t* noise_template) {
  if (width <= 0 || height <= 0 || (width % 2) != 0 || (height % 2) != 0 ||
      count <= 0 || static_cast<size_t>(count) > kMaxSlots || !noise_template) {
    return nullptr;
  }
  auto* source = new IoSurfaceFrameSource();
  source->width_ = width;
  source->height_ = height;
  source->count_ = count;
  source->frame_bytes_ = static_cast<size_t>(width) * height * 3U / 2U;

  // IOSurface backing makes the buffer represent production exactly: an empty
  // IOSurfaceProperties dictionary forces the allocation onto an IOSurface.
  NSDictionary* pixel_attributes =
      @{(id)kCVPixelBufferIOSurfacePropertiesKey : @{}};
  const size_t y_size = static_cast<size_t>(width) * height;
  for (int i = 0; i < count; ++i) {
    CVPixelBufferRef buffer = nullptr;
    const CVReturn status = CVPixelBufferCreate(
        kCFAllocatorDefault, width, height,
        kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
        (__bridge CFDictionaryRef)pixel_attributes, &buffer);
    if (status != kCVReturnSuccess || !buffer) {
      source->Destroy();
      return nullptr;
    }
    // Seed the noise template once. Encode cost then matches CPU mode; only the
    // barcode is rewritten per frame.
    if (CVPixelBufferLockBaseAddress(buffer, 0) == kCVReturnSuccess) {
      auto* y_plane = static_cast<uint8_t*>(
          CVPixelBufferGetBaseAddressOfPlane(buffer, 0));
      auto* uv_plane = static_cast<uint8_t*>(
          CVPixelBufferGetBaseAddressOfPlane(buffer, 1));
      const size_t y_stride = CVPixelBufferGetBytesPerRowOfPlane(buffer, 0);
      const size_t uv_stride = CVPixelBufferGetBytesPerRowOfPlane(buffer, 1);
      for (int row = 0; row < height; ++row) {
        std::memcpy(y_plane + static_cast<size_t>(row) * y_stride,
                    noise_template + static_cast<size_t>(row) * width, width);
      }
      const uint8_t* uv_template = noise_template + y_size;
      for (int row = 0; row < height / 2; ++row) {
        std::memcpy(uv_plane + static_cast<size_t>(row) * uv_stride,
                    uv_template + static_cast<size_t>(row) * width, width);
      }
      CVPixelBufferUnlockBaseAddress(buffer, 0);
    }
    source->buffers_[i] = buffer;

    // Static descriptor fields never change for a slot; width/height/owner are
    // refreshed per frame in FillFrame.
    MiniRtcNativeVideoFrame& native = source->descriptors_[i];
    native.struct_size = sizeof(native);
    native.type = MiniRtcNativeVideoFrameCVPixelBuffer;
    native.retain = &RetainPoolPixelBuffer;
    native.release = &ReleasePoolPixelBuffer;
    native.copy_to_nv12 = &CopyPoolPixelBufferToNv12;
  }
  return source;
}

void IoSurfaceFrameSource::FillFrame(int slot, uint32_t sequence,
                                     uint64_t captured_timestamp,
                                     MiniRtcVideoFrame* frame) {
  if (!frame || slot < 0 || slot >= count_ || !buffers_[slot]) return;
  auto pixel_buffer = static_cast<CVPixelBufferRef>(buffers_[slot]);
  StampSequenceIntoBuffer(pixel_buffer, width_, sequence);

  // Refresh the persistent per-slot descriptor for this frame. The library
  // copies the descriptor and retains the owner synchronously inside the send
  // call, so the descriptor only has to remain valid until the send returns —
  // and as a pool member it outlives every send.
  MiniRtcNativeVideoFrame& native = descriptors_[slot];
  native.width = static_cast<uint32_t>(width_);
  native.height = static_cast<uint32_t>(height_);
  native.owner = pixel_buffer;
  native.payload.cv_pixel_buffer = pixel_buffer;

  frame->data = nullptr;
  frame->size = frame_bytes_;
  frame->width = static_cast<uint32_t>(width_);
  frame->height = static_cast<uint32_t>(height_);
  frame->captured_timestamp = captured_timestamp;
  frame->native_frame = &native;
}

void IoSurfaceFrameSource::Destroy() {
  for (int i = 0; i < count_; ++i) {
    if (buffers_[i]) {
      CVPixelBufferRelease(static_cast<CVPixelBufferRef>(buffers_[i]));
      buffers_[i] = nullptr;
    }
  }
  delete this;
}

}  // namespace crossdesk
