#pragma once

#include <CoreVideo/CVPixelBuffer.h>

namespace crossdesk {

constexpr bool IsMacNv12(OSType format) {
  return format == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange ||
         format == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange;
}

constexpr bool IsMacYuv444(OSType format) {
  return format == kCVPixelFormatType_444YpCbCr10BiPlanarFullRange ||
         format == kCVPixelFormatType_444YpCbCr8BiPlanarVideoRange;
}

}  // namespace crossdesk
