/*
 * macOS pasteboard image helpers shared by the production clipboard backend
 * (platform/macos/clipboard.mm) and the unit test
 * (apps/desktop/tests/clipboard_image_mac_test.mm). Every function takes an
 * explicit NSPasteboard so tests run against a unique named pasteboard
 * without touching the user's general pasteboard.
 *
 * Copyright (c) 2026 Farlink contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef CROSSDESK_PLATFORM_MACOS_CLIPBOARD_IMAGE_MAC_H_
#define CROSSDESK_PLATFORM_MACOS_CLIPBOARD_IMAGE_MAC_H_

#include <AppKit/AppKit.h>

#include <optional>
#include <string>

#include "clipboard_image_wire.h"

namespace crossdesk::macos_clipboard {

// Precedence rule for macOS image sync: an image is the clipboard's primary
// payload when the pasteboard carries PNG or TIFF data and either
//   1. no plain-text representation exists at all (screenshots, Preview
//      crops), or
//   2. the image type precedes the string type in the pasteboard's declared
//      type order — browsers copying an image often append a URL string, but
//      the owner declares its primary type first.
// File URLs are handled before this test is ever consulted (caller side), so
// a Finder copy never reaches here as a false positive.
inline bool ImageIsPrimaryPayload(NSPasteboard* pasteboard) {
  NSArray* types = [pasteboard types];
  const BOOL has_png = [types containsObject:NSPasteboardTypePNG];
  const BOOL has_tiff = [types containsObject:NSPasteboardTypeTIFF];
  if (!has_png && !has_tiff) return false;
  const NSUInteger string_index = [types indexOfObject:NSPasteboardTypeString];
  if (string_index == NSNotFound) return true;
  NSUInteger image_index = NSNotFound;
  if (has_png) image_index = [types indexOfObject:NSPasteboardTypePNG];
  if (has_tiff) {
    const NSUInteger tiff_index = [types indexOfObject:NSPasteboardTypeTIFF];
    if (image_index == NSNotFound || tiff_index < image_index)
      image_index = tiff_index;
  }
  return image_index != NSNotFound && image_index < string_index;
}

// Reads the pasteboard image as compressed PNG. PNG payloads pass through
// byte-for-byte (zero re-encode); TIFF payloads convert exactly once through
// NSBitmapImageRep. Returns nullopt when no primary image exists, the data
// is unreadable or the compressed size exceeds the wire safety cap.
inline std::optional<std::string> ReadImagePng(NSPasteboard* pasteboard,
                                               std::string* error = nullptr) {
  if (!ImageIsPrimaryPayload(pasteboard)) return std::nullopt;

  NSData* png = [pasteboard dataForType:NSPasteboardTypePNG];
  if (png != nil && png.length > 0) {
    if (png.length > clipboard_image::kMaxImageBytes) {
      if (error) *error = "PNG payload exceeds the 8 MiB wire cap";
      return std::nullopt;
    }
    return std::string(static_cast<const char*>(png.bytes), png.length);
  }

  NSData* tiff = [pasteboard dataForType:NSPasteboardTypeTIFF];
  if (tiff == nil || tiff.length == 0) return std::nullopt;
  NSBitmapImageRep* rep = [NSBitmapImageRep imageRepWithData:tiff];
  if (rep == nil) {
    if (error) *error = "unreadable TIFF image data";
    return std::nullopt;
  }
  NSData* converted =
      [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
  if (converted == nil || converted.length == 0) {
    if (error) *error = "TIFF to PNG conversion failed";
    return std::nullopt;
  }
  if (converted.length > clipboard_image::kMaxImageBytes) {
    if (error) *error = "converted PNG exceeds the 8 MiB wire cap";
    return std::nullopt;
  }
  return std::string(static_cast<const char*>(converted.bytes),
                     converted.length);
}

// Validates and writes compressed PNG bytes as the pasteboard's primary
// image payload. PNG is the canonical type; a TIFF representation is added
// best-effort for older AppKit consumers (Notes, Word). Returns false for
// invalid signatures, oversize payloads or pasteboard write failures.
inline bool WriteImagePng(NSPasteboard* pasteboard, const std::string& png) {
  if (!clipboard_image::ValidPngSignature(png.data(), png.size()) ||
      png.size() > clipboard_image::kMaxImageBytes)
    return false;
  NSData* data = [NSData dataWithBytes:png.data() length:png.size()];
  if (data == nil) return false;
  [pasteboard clearContents];
  if (![pasteboard setData:data forType:NSPasteboardTypePNG]) return false;
  if (NSBitmapImageRep* rep = [NSBitmapImageRep imageRepWithData:data]) {
    if (NSData* tiff = [rep TIFFRepresentation]) {
      [pasteboard setData:tiff forType:NSPasteboardTypeTIFF];
    }
  }
  return true;
}

// Converts TIFF bytes to PNG bytes without touching any pasteboard; exposed
// for the deterministic conversion unit test.
inline std::optional<std::string> TiffToPng(const std::string& tiff) {
  NSData* input = [NSData dataWithBytes:tiff.data() length:tiff.size()];
  NSBitmapImageRep* rep = [NSBitmapImageRep imageRepWithData:input];
  if (rep == nil) return std::nullopt;
  NSData* converted =
      [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
  if (converted == nil || converted.length == 0) return std::nullopt;
  return std::string(static_cast<const char*>(converted.bytes),
                     converted.length);
}

}  // namespace crossdesk::macos_clipboard

#endif  // CROSSDESK_PLATFORM_MACOS_CLIPBOARD_IMAGE_MAC_H_
