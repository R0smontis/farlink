/*
 * @Author: DI JUNKUN
 * @Date: 2025-12-28
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _CLIPBOARD_H_
#define _CLIPBOARD_H_

#include "clipboard_file_promise.h"

#include <functional>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace crossdesk {

class Clipboard {
 public:
  using OnClipboardChanged = std::function<int(const char* data, size_t size)>;

  Clipboard() = default;
  ~Clipboard() = default;

  static std::string GetText();

  static bool SetText(const std::string& text);

  static bool HasText();

  // Compressed PNG image data on the OS clipboard. GetImagePng returns the
  // image exactly as stored (PNG) or re-encoded from the platform-native
  // bitmap type (TIFF on macOS, DIB on Windows); it never invents pixels.
  // Precedence rule for clipboard sync: file URLs win over images, images
  // win over text only when the image is the primary payload — see the
  // platform implementations for the exact type-ordering test.
  static std::optional<std::string> GetImagePng();
  // Writes compressed PNG bytes as the clipboard's primary image payload.
  // Implementations must validate the PNG signature and reject inputs above
  // clipboard_image::kMaxImageBytes.
  static bool SetImagePng(const std::string& png);
  // True when an image type is the clipboard's primary payload (the same
  // precedence GetImagePng applies — image present and not shadowed by
  // file URLs or a leading plain-text representation).
  static bool HasImage();

  // Echo suppression for clipboard sync: after applying a remote image the
  // receiver records the payload hash and the resulting change count, so the
  // clipboard monitor never sends the identical image back to the sender.
  static void NoteRemoteImageApplied(uint64_t png_hash);
  // True when the current clipboard image is exactly the most recently
  // applied remote payload (hash equality plus an unchanged change count).
  static bool IsEchoOfRemoteImage(uint64_t png_hash);


  // Main/UI thread only. File paths are UTF-8; a nonempty list takes precedence
  // over a file manager's accompanying text representation. No move/delete.
  static std::optional<std::vector<std::string>> GetFiles();
  static bool SetFiles(const std::vector<std::string>& paths);
  static uint64_t ChangeCount();
  // Publish metadata only. File contents are requested by an actual OS paste.
  // Keep enumerators distinct from X11 macros, including None and Success.
  enum class FileOfferError { NoError, UnavailableRuntime, MountFailed, PublishFailed };
  static bool SetFileOffer(std::shared_ptr<ClipboardFilePromise> offer,
                           FileOfferError* error = nullptr);
  static bool OwnsFileOffer(const std::shared_ptr<ClipboardFilePromise>& offer);
  static void ClearFileOffer();

  static void StartMonitoring(int check_interval_ms = 100,
                              OnClipboardChanged on_changed = nullptr);

  static void StopMonitoring();

  static bool IsMonitoring();
};

}  // namespace crossdesk

#endif
