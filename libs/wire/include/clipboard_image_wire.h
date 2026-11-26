/*
 * Clipboard image (PNG) wire protocol — chunked binary transfer over the
 * reliable desktop clipboard stream.
 *
 * Copyright (c) 2026 Farlink contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef _CLIPBOARD_IMAGE_WIRE_H_
#define _CLIPBOARD_IMAGE_WIRE_H_

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace crossdesk::clipboard_image {

// CrossDesk application protocol, carried as opaque binary payloads on the
// reliable desktop clipboard stream ("clipboard"). The stream already carries
// legacy plain-text clipboard values; image frames are distinguished by the
// magic prefix below, which cannot begin a valid UTF-8 clipboard text because
// legacy receivers reject embedded NUL bytes — and 0x89 is never a valid
// UTF-8 leading byte, so a text paste can never be mistaken for an image
// frame either. Peers that do not advertise supports_clipboard_image in
// host_infomation must never receive these frames.
inline constexpr char kMagic[] = "\x89" "CLIPIMG";
inline constexpr std::size_t kMagicBytes = 8;
inline constexpr uint32_t kVersion = 1;

// Hard safety cap on the compressed PNG payload. Screenshots of a 5K display
// compress to roughly 1-4 MiB; 8 MiB leaves generous headroom while bounding
// memory use and control-channel occupancy.
inline constexpr std::size_t kMaxImageBytes = 8 * 1024 * 1024;
// Payload bytes per frame. Sized with the legacy clipboard text path, which
// already streams 128 KiB values through MiniRTC/KCP fragmentation.
inline constexpr std::size_t kChunkBytes = 32 * 1024;

// Fixed little-endian header:
//   [0,8)   magic kMagic
//   [8,12)  kind (uint32)
//   [12,16) transfer_id (uint32, nonzero)
//   [16,20) total_size (uint32) — total PNG bytes across all chunks
//   [20,24) chunk_index (uint32) — zero-based
//   [24,28) chunk_count (uint32)
//   [28,32) chunk_stride (uint32) — payload bytes in every non-final chunk;
//           identical across all chunks of one transfer, so receivers can
//           place out-of-order fragments without guessing the sender's split.
//   [32,)   PNG payload fragment
inline constexpr std::size_t kHeaderBytes = 32;

inline constexpr uint32_t kChunk = 1;    // one payload fragment
inline constexpr uint32_t kCancel = 2;   // sender aborts transfer_id
inline constexpr uint32_t kApplied = 3;  // receiver wrote the image
inline constexpr uint32_t kRejected = 4; // receiver refused the image

inline uint32_t Read32(const char* data) {
  const auto* p = reinterpret_cast<const unsigned char*>(data);
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
         (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

inline void Write32(char* data, uint32_t value) {
  for (int i = 0; i < 4; ++i) data[i] = char(value >> (8 * i));
}

inline bool IsImageFrame(std::string_view frame) {
  return frame.size() >= kHeaderBytes &&
         frame.substr(0, kMagicBytes) == std::string_view(kMagic, kMagicBytes);
}

// PNG signature validation shared by every platform backend and the receiver.
inline bool ValidPngSignature(const void* data, std::size_t size) {
  static constexpr unsigned char kSignature[8] = {
      0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
  if (!data || size < sizeof(kSignature)) return false;
  const auto* bytes = static_cast<const unsigned char*>(data);
  for (std::size_t i = 0; i < sizeof(kSignature); ++i)
    if (bytes[i] != kSignature[i]) return false;
  return true;
}

inline std::string Frame(uint32_t kind, uint32_t transfer_id,
                         uint32_t total_size, uint32_t chunk_index,
                         uint32_t chunk_count, std::string_view bytes = {},
                         uint32_t chunk_stride = 0) {
  std::string frame(kHeaderBytes, '\0');
  std::copy(kMagic, kMagic + kMagicBytes, frame.data());
  Write32(frame.data() + 8, kind);
  Write32(frame.data() + 12, transfer_id);
  Write32(frame.data() + 16, total_size);
  Write32(frame.data() + 20, chunk_index);
  Write32(frame.data() + 24, chunk_count);
  Write32(frame.data() + 28, chunk_stride);
  frame.append(bytes);
  return frame;
}

inline std::string Control(uint32_t kind, uint32_t transfer_id) {
  return Frame(kind, transfer_id, 0, 0, 0);
}

// Deterministic PNG chunker: splits one compressed image into ordered frames
// of at most kChunkBytes payload each (message_limit permitting). Pure and
// allocation-lean; returns false for empty/oversized images, a zero transfer
// id or a message limit too small for one payload byte.
template <typename Send>
bool SendImage(std::string_view png, uint32_t transfer_id,
               std::size_t message_limit, Send send) {
  if (png.empty() || png.size() > kMaxImageBytes || !transfer_id ||
      message_limit <= kHeaderBytes)
    return false;
  const std::size_t chunk =
      std::min(kChunkBytes, message_limit - kHeaderBytes);
  const uint32_t count =
      uint32_t((png.size() + chunk - 1) / chunk);
  const uint32_t stride = uint32_t(chunk);
  for (uint32_t index = 0; index < count; ++index) {
    const std::size_t offset = std::size_t(index) * chunk;
    if (!send(Frame(kChunk, transfer_id, uint32_t(png.size()), index, count,
                    png.substr(offset, chunk), stride)))
      return false;
  }
  return true;
}

// Reassembles one image transfer per remote endpoint. Chunk frames may arrive
// in any order (MiniRTC reliable streams are ordered, but a cancelled or
// superseded transfer can interleave with the next one); incomplete transfers
// are evicted after the staleness timeout. All validation is deterministic
// and allocation is bounded by kMaxImageBytes per transfer.
class Assembler {
 public:
  struct Result {
    uint32_t transfer_id = 0;
    std::string png;
  };

  // Consumes one frame. Returns a complete image exactly once per transfer;
  // otherwise an empty optional-like Result (transfer_id == 0). Malformed
  // frames drop their transfer without failing the stream.
  Result Receive(std::string_view frame,
                 std::chrono::steady_clock::time_point now =
                     std::chrono::steady_clock::now()) {
    EvictStale(now);
    if (!IsImageFrame(frame) ||
        frame.size() > kHeaderBytes + kChunkBytes) {
      return {};
    }
    const uint32_t kind = Read32(frame.data() + 8);
    const uint32_t id = Read32(frame.data() + 12);
    const uint32_t total = Read32(frame.data() + 16);
    const uint32_t index = Read32(frame.data() + 20);
    const uint32_t count = Read32(frame.data() + 24);
    if (kind == kCancel) {
      if (transfer_.id == id) Reset();
      return {};
    }
    if (kind != kChunk || !id || !total || total > kMaxImageBytes || !count ||
        index >= count) {
      return {};
    }
    const auto bytes = frame.substr(kHeaderBytes);
    // The sender stamps every chunk with the transfer's stride so fragments
    // can be placed in any arrival order. Validation is strict: the stride
    // must tile the payload exactly — full non-final chunks, a non-empty
    // remainder in the final one.
    const uint32_t stride = Read32(frame.data() + 28);
    if (!stride || stride > kChunkBytes ||
        uint64_t(stride) * (count - 1) >= total ||
        uint64_t(stride) * count < total)
      return {};
    const bool last = index + 1 == count;
    const uint64_t end = uint64_t(index) * stride + bytes.size();
    if (bytes.empty() || (!last && bytes.size() != stride) ||
        (last && end != total) || (!last && end > total))
      return {};

    if (!transfer_.id) {
      transfer_.id = id;
      transfer_.total = total;
      transfer_.count = count;
      transfer_.stride = stride;
      transfer_.data.resize(total);
      transfer_.received.assign(count, false);
      transfer_.bytes_received = 0;
      transfer_.updated = now;
    }
    if (transfer_.id != id || transfer_.total != total ||
        transfer_.count != count || transfer_.stride != stride) {
      // A new transfer id supersedes the stale one; a conflicting header for
      // the same id corrupts it either way — reset and ignore the frame.
      if (transfer_.id != id) {
        Reset();
        transfer_.id = id;
        transfer_.total = total;
        transfer_.count = count;
        transfer_.stride = stride;
        transfer_.data.resize(total);
        transfer_.received.assign(count, false);
        transfer_.bytes_received = 0;
        transfer_.updated = now;
      } else {
        Reset();
        return {};
      }
    }
    if (transfer_.received[index]) return {};  // duplicate chunk
    std::copy(bytes.begin(), bytes.end(),
              transfer_.data.begin() + std::size_t(index) * transfer_.stride);
    transfer_.received[index] = true;
    transfer_.bytes_received += bytes.size();
    transfer_.updated = now;
    if (transfer_.bytes_received != transfer_.total) return {};
    for (bool got : transfer_.received)
      if (!got) return {};
    if (!ValidPngSignature(transfer_.data.data(), transfer_.data.size())) {
      Reset();
      return {};
    }
    Result result{id, std::move(transfer_.data)};
    Reset();
    return result;
  }

  // Drops incomplete transfers idle for longer than the timeout. Called on
  // every Receive; hosts with no traffic can also call it from a timer.
  void EvictStale(std::chrono::steady_clock::time_point now,
                  std::chrono::milliseconds timeout =
                      std::chrono::milliseconds(15000)) {
    if (transfer_.id && now - transfer_.updated >= timeout) Reset();
  }

  bool pending() const { return transfer_.id != 0; }

 private:
  void Reset() {
    transfer_.id = 0;
    transfer_.data.clear();
    transfer_.data.shrink_to_fit();
    transfer_.received.clear();
    transfer_.received.shrink_to_fit();
    transfer_.total = transfer_.count = transfer_.stride = 0;
    transfer_.bytes_received = 0;
  }

  struct Transfer {
    uint32_t id = 0;
    uint32_t total = 0;
    uint32_t count = 0;
    uint32_t stride = 0;
    std::size_t bytes_received = 0;
    std::string data;
    std::vector<bool> received;
    std::chrono::steady_clock::time_point updated{};
  } transfer_;
};

// FNV-1a over PNG bytes for echo suppression: the receiver hashes the bytes
// it wrote into the OS clipboard so its own clipboard monitor can skip
// re-sending the identical image back to the sender.
inline uint64_t HashPng(std::string_view png) {
  uint64_t hash = 14695981039346656037ull;
  for (const char byte : png) {
    hash ^= static_cast<unsigned char>(byte);
    hash *= 1099511628211ull;
  }
  return hash;
}

}  // namespace crossdesk::clipboard_image

#endif  // _CLIPBOARD_IMAGE_WIRE_H_
