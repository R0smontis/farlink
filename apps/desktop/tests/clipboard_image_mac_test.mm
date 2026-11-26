/*
 * macOS clipboard image backend unit tests. The AppKit round-trip runs
 * against a unique named NSPasteboard — the user's general pasteboard is
 * never read or written by this test.
 *
 * Copyright (c) 2026 Farlink contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include <AppKit/AppKit.h>

#include <cstring>
#include <iostream>
#include <string>

#include "clipboard_image_wire.h"
#include "platform/macos/clipboard_image_mac.h"

namespace {

bool Expect(bool condition, const char* message) {
  if (condition) return true;
  std::cerr << "FAIL: " << message << '\n';
  return false;
}

// Renders a small bitmap entirely in memory and returns it as PNG. Uses the
// same NSBitmapImageRep conversion path the backend relies on, so the test
// detects behavioral drift in either direction.
std::string RenderPng(int width, int height, unsigned char seed) {
  NSBitmapImageRep* rep = [[NSBitmapImageRep alloc]
      initWithBitmapDataPlanes:nullptr
                    pixelsWide:width
                    pixelsHigh:height
                 bitsPerSample:8
               samplesPerPixel:4
                      hasAlpha:YES
                      isPlanar:NO
                colorSpaceName:NSDeviceRGBColorSpace
                   bytesPerRow:width * 4
                  bitsPerPixel:32];
  unsigned char* pixels = [rep bitmapData];
  for (int y = 0; y < height; ++y)
    for (int x = 0; x < width; ++x) {
      unsigned char* pixel = pixels + (y * width + x) * 4;
      pixel[0] = static_cast<unsigned char>(seed + x);
      pixel[1] = static_cast<unsigned char>(seed + y);
      pixel[2] = static_cast<unsigned char>(seed ^ 0x5a);
      pixel[3] = 0xff;
    }
  NSData* png =
      [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
  return std::string(static_cast<const char*>(png.bytes), png.length);
}

std::string RenderTiff(int width, int height, unsigned char seed) {
  NSBitmapImageRep* rep = [[NSBitmapImageRep alloc]
      initWithBitmapDataPlanes:nullptr
                    pixelsWide:width
                    pixelsHigh:height
                 bitsPerSample:8
               samplesPerPixel:4
                      hasAlpha:YES
                      isPlanar:NO
                colorSpaceName:NSDeviceRGBColorSpace
                   bytesPerRow:width * 4
                  bitsPerPixel:32];
  unsigned char* pixels = [rep bitmapData];
  for (int y = 0; y < height; ++y)
    for (int x = 0; x < width; ++x) {
      unsigned char* pixel = pixels + (y * width + x) * 4;
      pixel[0] = static_cast<unsigned char>(seed + x);
      pixel[1] = static_cast<unsigned char>(seed + y);
      pixel[2] = static_cast<unsigned char>(seed ^ 0x5a);
      pixel[3] = 0xff;
    }
  NSData* tiff = [rep TIFFRepresentation];
  std::string result(static_cast<const char*>(tiff.bytes), tiff.length);
  return result;
}

}  // namespace

int main() {
  using namespace crossdesk;
  bool ok = true;

  @autoreleasepool {
    // ---- PNG signature validation ----------------------------------------
    const std::string png = RenderPng(8, 8, 3);
    ok &= Expect(!png.empty(), "test setup: PNG rendering failed");
    ok &= Expect(clipboard_image::ValidPngSignature(png.data(), png.size()),
                 "rendered PNG fails the backend's own signature check");
    std::string not_png = png;
    not_png[0] = 'X';
    ok &= Expect(
        !clipboard_image::ValidPngSignature(not_png.data(), not_png.size()),
        "corrupted signature accepted");
    ok &= Expect(!clipboard_image::ValidPngSignature(png.data(), 4),
                 "truncated signature accepted");

    // ---- TIFF -> PNG conversion helper -------------------------------------
    const std::string tiff = RenderTiff(8, 8, 9);
    ok &= Expect(!tiff.empty(), "test setup: TIFF rendering failed");
    const auto converted = macos_clipboard::TiffToPng(tiff);
    ok &= Expect(converted.has_value(), "TIFF to PNG conversion failed");
    ok &= Expect(converted &&
                     clipboard_image::ValidPngSignature(converted->data(),
                                                        converted->size()),
                 "converted TIFF is not a valid PNG");
    // Pixel dimensions survive the conversion.
    if (converted) {
      NSData* data = [NSData dataWithBytes:converted->data()
                                    length:converted->size()];
      NSBitmapImageRep* rep = [NSBitmapImageRep imageRepWithData:data];
      ok &= Expect(rep != nil && rep.pixelsWide == 8 && rep.pixelsHigh == 8,
                   "conversion lost image dimensions");
    }
    const auto garbage = macos_clipboard::TiffToPng("not an image at all");
    ok &= Expect(!garbage.has_value(), "garbage bytes converted as TIFF");

    // ---- Unique named pasteboard round-trip --------------------------------
    NSPasteboard* board = [NSPasteboard pasteboardWithUniqueName];
    ok &= Expect(board != nil, "could not create unique pasteboard");

    // Write a PNG, read it back byte-for-byte (zero re-encode fast path).
    ok &= Expect(macos_clipboard::WriteImagePng(board, png),
                 "pasteboard PNG write failed");
    ok &= Expect(macos_clipboard::ImageIsPrimaryPayload(board),
                 "written PNG not detected as primary payload");
    const auto roundtrip = macos_clipboard::ReadImagePng(board);
    ok &= Expect(roundtrip.has_value(), "pasteboard read-back failed");
    ok &= Expect(roundtrip && *roundtrip == png,
                 "PNG bytes did not round-trip byte-for-byte");
    // The TIFF companion representation exists for legacy consumers.
    NSArray* types = [board types];
    ok &= Expect([types containsObject:NSPasteboardTypeTIFF],
                 "TIFF companion representation missing");

    // Invalid payloads never reach the pasteboard.
    ok &= Expect(!macos_clipboard::WriteImagePng(board, "definitely not png"),
                 "invalid PNG signature written");
    const std::string oversized(clipboard_image::kMaxImageBytes + 1, '\x89');
    ok &= Expect(!macos_clipboard::WriteImagePng(board, oversized),
                 "oversized PNG written");

    // ---- Precedence rules ---------------------------------------------------
    // Image without any text: image wins (screenshot case).
    [board clearContents];
    NSData* tiff_data = [NSData dataWithBytes:tiff.data() length:tiff.size()];
    [board setData:tiff_data forType:NSPasteboardTypeTIFF];
    ok &= Expect(macos_clipboard::ImageIsPrimaryPayload(board),
                 "TIFF-only pasteboard not primary image");
    const auto tiff_read = macos_clipboard::ReadImagePng(board);
    ok &= Expect(tiff_read.has_value() &&
                     clipboard_image::ValidPngSignature(tiff_read->data(),
                                                        tiff_read->size()),
                 "TIFF pasteboard payload not converted to valid PNG");

    // Image declared before a trailing string (browser copy): image wins.
    [board clearContents];
    NSPasteboardItem* item = [[NSPasteboardItem alloc] init];
    [item setData:tiff_data forType:NSPasteboardTypeTIFF];
    [item setString:@"https://example.com/image.png"
            forType:NSPasteboardTypeString];
    [board writeObjects:@[ item ]];
    ok &= Expect(macos_clipboard::ImageIsPrimaryPayload(board),
                 "image-first browser copy not detected as image");
    const auto browser_read = macos_clipboard::ReadImagePng(board);
    ok &= Expect(browser_read.has_value(),
                 "browser-copy image read-back failed");

    // String declared before the image: the text representation is primary,
    // so the image must NOT sync (URL copy, labeled file copies).
    [board clearContents];
    NSPasteboardItem* text_first = [[NSPasteboardItem alloc] init];
    [text_first setString:@"plain text wins" forType:NSPasteboardTypeString];
    [text_first setData:tiff_data forType:NSPasteboardTypeTIFF];
    [board writeObjects:@[ text_first ]];
    ok &= Expect(!macos_clipboard::ImageIsPrimaryPayload(board),
                 "text-first pasteboard misdetected as image");

    // Plain text alone: never an image.
    [board clearContents];
    [board setString:@"hello" forType:NSPasteboardTypeString];
    ok &= Expect(!macos_clipboard::ImageIsPrimaryPayload(board),
                 "text-only pasteboard detected as image");
    ok &= Expect(!macos_clipboard::ReadImagePng(board).has_value(),
                 "text-only pasteboard produced image bytes");

    // ---- changeCount pairs with payload replacement ------------------------
    // The loop-suppression contract relies on clearContents+write bumping the
    // change count exactly once per apply; verify the primitive the guard
    // depends on.
    [board clearContents];
    const NSInteger before = [board changeCount];
    macos_clipboard::WriteImagePng(board, png);
    const NSInteger after = [board changeCount];
    ok &= Expect(after > before,
                 "pasteboard write did not bump changeCount");

    [board releaseGlobally];
  }

  if (ok) {
    std::cout << "clipboard_image_mac_test: all tests passed\n";
    return 0;
  }
  return 1;
}
