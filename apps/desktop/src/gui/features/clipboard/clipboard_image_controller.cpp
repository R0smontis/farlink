/*
 * Clipboard image (PNG) synchronization — controller side of the chunked
 * binary wire protocol in libs/wire/include/clipboard_image_wire.h.
 *
 * Copyright (c) 2026 Farlink contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "clipboard_controller.h"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_error.h>

#include <shared_mutex>
#include <utility>

#include "clipboard.h"
#include "rd_log.h"
#include "runtime/gui_runtime.h"
#include "runtime/remote_session.h"

namespace crossdesk {

namespace {

// One MiniRTC reliable message on the clipboard stream. Sized like the web
// clipboard protocol: comfortably inside the transport's fragmentation
// window, small enough to interleave with text traffic.
constexpr size_t kImageMessageLimit = 48 * 1024;

}  // namespace

bool ClipboardController::ImageSessionActive(
    const std::string& remote_id, const std::shared_ptr<ImageSession>& session) {
  if (!session || !events_enabled_.load(std::memory_order_acquire) ||
      owner_.unattended_host_)
    return false;
  const auto events = session->events.lock();
  if (!events || !events->DataAllowed()) return false;
  if (session->controlling) {
    const auto viewer = session->viewer.lock();
    if (!viewer || viewer->closing_ ||
        owner_.FindRemoteSession(remote_id) != viewer ||
        viewer->peer_events_ != events ||
        !viewer->clipboard_image_supported_.load(std::memory_order_acquire))
      return false;
  } else if (!owner_.IsAuthorizedController(remote_id) ||
             owner_.ControllerEvents(remote_id) != events) {
    return false;
  }
  std::lock_guard lock(image_sessions_mutex_);
  const auto found = image_sessions_.find(remote_id);
  return found != image_sessions_.end() && found->second == session;
}

void ClipboardController::SetImageSupport(const std::string& remote_id,
                                          PeerEventHandler* source,
                                          bool supported) {
#ifndef __OHOS__
  if (!events_enabled_.load(std::memory_order_acquire) ||
      owner_.unattended_host_)
    return;
  const auto viewer = owner_.FindRemoteSession(remote_id);
  const auto events =
      viewer ? viewer->peer_events_ : owner_.ControllerEvents(remote_id);
  if (!events || events.get() != source || !events->DataAllowed()) return;
  std::lock_guard lock(image_sessions_mutex_);
  if (!events_enabled_.load(std::memory_order_relaxed)) return;
  auto& slot = image_sessions_[remote_id];
  if (supported && slot && slot->events.lock() == events) return;
  if (!supported) {
    image_sessions_.erase(remote_id);
    return;
  }
  auto session = std::make_shared<ImageSession>();
  session->events = events;
  session->viewer = viewer;
  session->controlling = bool(viewer);
  slot = std::move(session);
#endif
}

void ClipboardController::ForgetImage(const std::string& remote_id) {
  std::lock_guard lock(image_sessions_mutex_);
  image_sessions_.erase(remote_id);
}

void ClipboardController::ReceiveImageFrame(const char* data, size_t size,
                                            const std::string& remote_id,
                                            PeerEventHandler* source) {
#ifndef __OHOS__
  if (!events_enabled_.load(std::memory_order_acquire) ||
      owner_.unattended_host_ || !data)
    return;
  std::shared_ptr<ImageSession> session;
  {
    std::lock_guard lock(image_sessions_mutex_);
    const auto found = image_sessions_.find(remote_id);
    if (found != image_sessions_.end()) session = found->second;
  }
  if (!ImageSessionActive(remote_id, session) ||
      session->events.lock().get() != source)
    return;
  // Privacy gate: a viewer (controlling) session with clipboard disallowed —
  // view-only launch or control toggled off — must not let a remote host
  // assemble an image payload inside this process. The session itself is
  // preserved in image_sessions_ so regaining clipboard access does not
  // require a fresh capability negotiation; only the inbound frames drop.
  if (session->controlling) {
    const auto viewer = session->viewer.lock();
    if (!viewer || !viewer->clipboard_allowed_.load()) return;
  }
  const auto result =
      session->assembler.Receive(std::string_view(data, size));
  if (!result.transfer_id || result.png.empty()) return;

  // Only the newest clipboard value matters. A completed image supersedes any
  // earlier pending image; the SDL thread applies it exactly like text.
  {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    if (!events_enabled_.load(std::memory_order_relaxed)) return;
    pending_remote_image_ = std::move(result.png);
    pending_image_remote_id_ = remote_id;
    pending_image_session_ = session->viewer;
    // Snapshot the input generation alongside the session so ApplyPendingImage
    // can reject stale payloads exactly like ApplyPendingWrite does for text.
    const auto viewer = session->viewer.lock();
    pending_image_generation_ = viewer ? viewer->input_generation_.load() : 0;
  }
  SDL_Event event{};
  event.type = event_type_;
  if (event_type_ != 0 && !SDL_PushEvent(&event)) {
    LOG_WARN("Failed to wake SDL loop for remote clipboard image: {}",
             SDL_GetError());
  }
#endif
}

void ClipboardController::ApplyPendingImage() {
#ifndef __OHOS__
  std::optional<std::string> png;
  std::string source_id;
  std::weak_ptr<gui_detail::RemoteSession> source_session;
  uint64_t generation = 0;
  {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    png.swap(pending_remote_image_);
    source_id.swap(pending_image_remote_id_);
    source_session.swap(pending_image_session_);
    generation = pending_image_generation_;
    pending_image_generation_ = 0;
  }
  if (!png || png->empty()) return;
  if (!events_enabled_.load(std::memory_order_acquire)) return;

  // Re-validate on the applying thread, mirroring ApplyPendingWrite exactly:
  // a viewer (controlling) payload must still match its session, have
  // clipboard_allowed_ and an unchanged input generation; a host-side write
  // from a controller must still come from an authorized connected controller.
  // Focus loss or session teardown between packet arrival and this deferred
  // write must not let a stale image reach the OS clipboard.
  if (!source_id.empty()) {
    if (auto props = owner_.FindRemoteSession(source_id)) {
      if (props != source_session.lock() ||
          !props->clipboard_allowed_.load() ||
          props->input_generation_.load() != generation) {
        return;
      }
    } else if (!owner_.IsAuthorizedController(source_id)) {
      return;
    }
  }

  // Validate once more on the applying thread: the wire assembler already
  // checks the signature, but the OS write below is the last gate before the
  // payload reaches local applications.
  if (!clipboard_image::ValidPngSignature(png->data(), png->size())) {
    LOG_WARN("Dropping remote clipboard image with invalid PNG signature");
    return;
  }

  ClearFileOffer();
  if (!Clipboard::SetImagePng(*png)) {
    LOG_ERROR("Failed to write remote clipboard image ({} bytes)", png->size());
    return;
  }

  // Echo suppression: record the payload hash both locally (this controller's
  // dedupe) and globally (the platform clipboard monitor), paired with the
  // post-write change count, before any clipboard-changed event can fire.
  const uint64_t hash = clipboard_image::HashPng(*png);
  Clipboard::NoteRemoteImageApplied(hash);
  last_image_hash_ = hash;
  ++clipboard_revision_;
  last_text_.clear();
  last_files_.clear();
  local_files_ = false;
  deferred_local_session_.reset();
  deferred_file_session_.reset();
  LOG_INFO("Applied remote clipboard image: {} bytes from [{}]", png->size(),
           source_id);
#endif
}

int ClipboardController::SendImageToController(const std::string& remote_id,
                                               const std::string& png) {
  std::shared_ptr<ImageSession> session;
  {
    std::lock_guard lock(image_sessions_mutex_);
    const auto found = image_sessions_.find(remote_id);
    if (found != image_sessions_.end()) session = found->second;
  }
  if (!ImageSessionActive(remote_id, session) || session->controlling)
    return -1;
  const std::weak_ptr<ImageSession> weak = session;
  const auto transfer_id = next_image_transfer_id_++;
  if (!next_image_transfer_id_) next_image_transfer_id_ = 1;
  return clipboard_image::SendImage(
             png, transfer_id, kImageMessageLimit,
             [this, &remote_id, weak](const std::string& frame) {
               const auto session = weak.lock();
               if (!ImageSessionActive(remote_id, session)) return false;
               const auto events = session->events.lock();
               if (!events) return false;
               auto callback = events->EnterCallback();
               if (!callback || !events->DataAllowed()) return false;
               return owner_.SendControllerData(
                          remote_id, frame.data(), frame.size(),
                          owner_.clipboard_label_.c_str()) == 0;
             })
             ? 0
             : -1;
}

int ClipboardController::SendImageToPeers(const std::string& png) {
  int result = 0;
  {
    std::shared_lock lock(owner_.remote_sessions_mutex_);
    for (const auto& [remote_id, props] : owner_.remote_sessions_) {
      if (!props || props->closing_ || !props->peer_ ||
          !props->connection_established_ ||
          !props->clipboard_allowed_.load() ||
          !props->clipboard_image_supported_.load(std::memory_order_acquire))
        continue;
      const auto transfer_id = next_image_transfer_id_++;
      if (!next_image_transfer_id_) next_image_transfer_id_ = 1;
      auto* peer = props->peer_;
      const std::string label = props->clipboard_label_;
      const bool sent = clipboard_image::SendImage(
          png, transfer_id, kImageMessageLimit,
          [peer, &label](const std::string& frame) {
            return MiniRtcSendReliableDataFrame(peer, frame.data(), frame.size(),
                                                label.c_str()) == 0;
          });
      if (!sent) {
        LOG_WARN("Failed to send clipboard image to [{}]", remote_id);
        result = -1;
      }
    }
  }
  for (const auto& id : owner_.ConnectedControllers()) {
    if (SendImageToController(id, png) != 0) result = -1;
  }
  return result;
}

}  // namespace crossdesk
