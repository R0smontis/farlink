/* Copyright (c) 2026 by DI JUNKUN, All Rights Reserved. */
#ifndef _PRIVACY_WALLPAPER_MAC_H_
#define _PRIVACY_WALLPAPER_MAC_H_

#import <AppKit/AppKit.h>

namespace crossdesk {

// The user's desktop picture for the given screen, loaded through the public
// NSWorkspace API and cached per display. Returns nil when the wallpaper URL
// is unreadable, undecodable or blocked, so the caller can fall back to the
// gradient. Safe to call on the main thread only (NSWorkspace/AppKit).
NSImage* MacPrivacyWallpaperImage(NSScreen* screen);

// Aspect-fill center-cropped drawing matching the macOS desktop's "Fill
// Screen" scaling, cropped with ComputeAspectFillRect in CGImage pixel space.
// Returns false when the image carries no raster content.
bool DrawMacPrivacyAspectFillImage(NSImage* image, NSRect bounds);

// Calm dark-slate fallback in the spirit of the default macOS gradient,
// drawn whenever the real wallpaper is unavailable.
void DrawMacPrivacyFallbackWallpaper(NSRect bounds);

}  // namespace crossdesk

#endif  // _PRIVACY_WALLPAPER_MAC_H_
