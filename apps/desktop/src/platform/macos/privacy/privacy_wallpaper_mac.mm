/* Copyright (c) 2026 by DI JUNKUN, All Rights Reserved. */

#include "privacy_wallpaper_mac.h"

#include <mutex>

#include "privacy_cover_style.h"

namespace crossdesk {
namespace {

// One decoded desktop picture per display, invalidated automatically when the
// wallpaper URL changes (the key is the display id, the URL is remembered
// alongside). Bounded by the number of attached screens, so no eviction
// policy is needed beyond replacement.
std::mutex cache_mutex;

NSMutableDictionary<NSNumber*, NSDictionary*>* Cache() {
  static NSMutableDictionary<NSNumber*, NSDictionary*>* store = nil;
  static dispatch_once_t once;
  dispatch_once(&once, ^{ store = [NSMutableDictionary dictionary]; });
  return store;
}

NSImage* LoadWallpaperImage(NSURL* url) {
  if (!url) return nil;
  // filePromise/remote URLs never appear here, but a non-file URL cannot be
  // read synchronously anyway.
  if (!url.isFileURL) return nil;
  NSImage* image = [[NSImage alloc] initWithContentsOfURL:url];
  if (!image.valid) return nil;
  // Dynamic .heic wallpapers decode to a single still through NSImage. When
  // even that fails (sandbox, corrupted container) the reps are empty and
  // the caller falls back to the gradient.
  for (NSImageRep* rep in image.representations) {
    if (rep.pixelsWide > 0 && rep.pixelsHigh > 0) return image;
  }
  return nil;
}

}  // namespace

NSImage* MacPrivacyWallpaperImage(NSScreen* screen) {
  if (!screen) return nil;
  NSURL* url = [[NSWorkspace sharedWorkspace] desktopImageURLForScreen:screen];
  if (!url) return nil;
  NSNumber* key = screen.deviceDescription[@"NSScreenNumber"];
  if (!key) return nil;
  NSString* url_key = url.absoluteString;
  std::lock_guard lock(cache_mutex);
  NSMutableDictionary* store = Cache();
  NSDictionary* entry = store[key];
  if (entry && [entry[@"url"] isEqualToString:url_key]) {
    id image = entry[@"image"];
    return [image isKindOfClass:NSImage.class] ? (NSImage*)image : nil;
  }
  NSImage* image = LoadWallpaperImage(url);
  if (!image) {
    // Negative caching matters as much as positive caching: an unreadable
    // wallpaper must not be re-decoded on every frame of the cover redraw.
    store[key] = @{@"url": url_key, @"image": [NSNull null]};
    return nil;
  }
  store[key] = @{@"url": url_key, @"image": image};
  return image;
}

bool DrawMacPrivacyAspectFillImage(NSImage* image, NSRect bounds) {
  if (!image || bounds.size.width <= 0 || bounds.size.height <= 0) return false;
  CGImageRef cgimage = [image CGImageForProposedRect:nullptr context:nil hints:nil];
  if (!cgimage) return false;
  const double src_w = CGImageGetWidth(cgimage);
  const double src_h = CGImageGetHeight(cgimage);
  const AspectFillRect crop =
      ComputeAspectFillRect(src_w, src_h, bounds.size.width, bounds.size.height);
  if (crop.w <= 0 || crop.h <= 0) return false;
  CGImageRef cropped = CGImageCreateWithImageInRect(
      cgimage, CGRectMake(crop.x, crop.y, crop.w, crop.h));
  if (!cropped) return false;
  CGContextRef context = NSGraphicsContext.currentContext.CGContext;
  if (!context) { CGImageRelease(cropped); return false; }
  // Quartz places the image's top row at the rect's maximum user-space y.
  // That is correct in a bottom-up context but mirrors the image wherever
  // the y axis points down. NSGraphicsContext.isFlipped only reflects how
  // the context was created, so detect the axis direction from the CTM
  // itself: rectilinear AppKit transforms always have b == c == 0.
  const CGAffineTransform ctm = CGContextGetCTM(context);
  const bool y_down = ctm.b == 0 && ctm.c == 0 && ctm.d < 0;
  CGRect target = bounds;
  CGContextSaveGState(context);
  CGContextSetInterpolationQuality(context, kCGInterpolationHigh);
  if (y_down) {
    CGContextTranslateCTM(context, bounds.origin.x,
                          bounds.origin.y + bounds.size.height);
    CGContextScaleCTM(context, 1, -1);
    target = CGRectMake(0, 0, bounds.size.width, bounds.size.height);
  }
  CGContextDrawImage(context, target, cropped);
  CGContextRestoreGState(context);
  CGImageRelease(cropped);
  return true;
}

void DrawMacPrivacyFallbackWallpaper(NSRect bounds) {
  // A muted slate gradient echoing the default macOS desktop tones: calm,
  // dark and free of any branding.
  NSColor* top = [NSColor colorWithSRGBRed:38.0 / 255 green:44.0 / 255
                                      blue:56.0 / 255 alpha:1];
  NSColor* bottom = [NSColor colorWithSRGBRed:16.0 / 255 green:20.0 / 255
                                         blue:28.0 / 255 alpha:1];
  NSGradient* gradient = [[NSGradient alloc] initWithStartingColor:bottom
                                                       endingColor:top];
  [gradient drawInRect:bounds angle:90];
}

}  // namespace crossdesk
