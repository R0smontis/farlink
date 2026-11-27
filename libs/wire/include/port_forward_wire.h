/*
 * Port-forward tunnel wire protocol — small binary frames carried on the
 * reliable desktop data stream named by kPortForwardStream.
 *
 * Copyright (c) 2026 Farlink contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef _PORT_FORWARD_WIRE_H_
#define _PORT_FORWARD_WIRE_H_

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace crossdesk::port_forward {

// Fixed little-endian frame layout:
//   [0,4)   magic "FPF1" (0x31465046)
//   [4,6)   kind (uint16)
//   [6,8)   flags/port/error code (uint16; remote_port on kOpen,
//           error code on kOpenErr, zero elsewhere)
//   [8,12)  stream_id (uint32, nonzero)
//   [12,16) payload_len (uint32, <= kMaxPayloadBytes)
//   [16,)   payload bytes
inline constexpr std::uint32_t kMagic = 0x31465046u;
inline constexpr std::size_t kHeaderBytes = 16;
inline constexpr std::size_t kMaxPayloadBytes = 32 * 1024;
inline constexpr std::size_t kMaxReasonBytes = 512;

inline constexpr std::uint16_t kErrorRefused = 1;
inline constexpr std::uint16_t kErrorDenied = 2;
inline constexpr std::uint16_t kErrorCapacity = 3;
inline constexpr std::uint16_t kErrorTimeout = 4;
inline constexpr std::uint16_t kErrorProtocol = 5;

enum class Kind : std::uint16_t {
  kOpen = 1,
  kOpenOk = 2,
  kOpenErr = 3,
  kData = 4,
  kEof = 5,
  kClose = 6,
  // Flow-control credit: payload is a 4-byte little-endian count of payload
  // bytes the receiver has consumed (written to its local socket) since the
  // last credit. Senders must never carry more than kStreamWindowBytes of
  // unacknowledged kData payload per stream; because the data channel's
  // transport enqueue is unconditional, this application-level window is the
  // only bound on in-flight tunneled memory.
  kWindow = 7,
};

// Initial per-stream send window. Matches the bounded per-stream socket
// buffer so a stalled peer can never accumulate more than this many bytes
// inside the reliable transport plus both engines combined.
inline constexpr std::size_t kStreamWindowBytes = 256 * 1024;

struct Frame {
  Kind kind = Kind::kOpen;
  // kOpen carries the destination TCP port; kOpenErr carries a kError* code.
  std::uint16_t flags = 0;
  std::uint32_t stream_id = 0;
  // Borrowed by DecodeFrame; ownership remains with the input buffer.
  std::string_view payload;
};

inline std::uint16_t Read16(const char* data) {
  const auto* p = reinterpret_cast<const unsigned char*>(data);
  return static_cast<std::uint16_t>(std::uint16_t(p[0]) |
                                    (std::uint16_t(p[1]) << 8));
}

inline std::uint32_t Read32(const char* data) {
  const auto* p = reinterpret_cast<const unsigned char*>(data);
  return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) |
         (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}

inline void Write16(char* data, std::uint16_t value) {
  data[0] = static_cast<char>(value & 0xff);
  data[1] = static_cast<char>((value >> 8) & 0xff);
}

inline void Write32(char* data, std::uint32_t value) {
  for (int i = 0; i < 4; ++i) data[i] = static_cast<char>(value >> (8 * i));
}

inline bool ValidUtf8(std::string_view text) {
  for (std::size_t i = 0; i < text.size();) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    if (c < 0x80) {
      if (c == 0) return false;
      ++i;
      continue;
    }
    std::size_t continuation = 0;
    std::uint32_t value = 0;
    std::uint32_t minimum = 0;
    if ((c & 0xe0) == 0xc0) {
      continuation = 1;
      value = c & 0x1f;
      minimum = 0x80;
    } else if ((c & 0xf0) == 0xe0) {
      continuation = 2;
      value = c & 0x0f;
      minimum = 0x800;
    } else if ((c & 0xf8) == 0xf0) {
      continuation = 3;
      value = c & 0x07;
      minimum = 0x10000;
    } else {
      return false;
    }
    if (i + continuation >= text.size()) return false;
    for (std::size_t n = 1; n <= continuation; ++n) {
      const unsigned char next = static_cast<unsigned char>(text[i + n]);
      if ((next & 0xc0) != 0x80) return false;
      value = (value << 6) | (next & 0x3f);
    }
    if (value < minimum || value > 0x10ffff ||
        (value >= 0xd800 && value <= 0xdfff))
      return false;
    i += continuation + 1;
  }
  return true;
}

inline bool ValidFrameFields(const Frame& frame) {
  if (!frame.stream_id || frame.payload.size() > kMaxPayloadBytes) return false;
  switch (frame.kind) {
    case Kind::kOpen:
      return frame.flags != 0 && frame.payload.empty();
    case Kind::kOpenOk:
      return frame.flags == 0 && frame.payload.empty();
    case Kind::kOpenErr:
      return frame.flags >= kErrorRefused && frame.flags <= kErrorProtocol &&
             frame.payload.size() <= kMaxReasonBytes && ValidUtf8(frame.payload);
    case Kind::kData:
      return frame.flags == 0 && !frame.payload.empty();
    case Kind::kEof:
    case Kind::kClose:
      return frame.flags == 0 && frame.payload.empty();
    case Kind::kWindow:
      return frame.flags == 0 && frame.payload.size() == 4;
  }
  return false;
}

inline std::uint32_t ReadCredit(std::string_view payload) {
  return payload.size() == 4 ? Read32(payload.data()) : 0;
}

inline void WriteCredit(char* destination, std::uint32_t bytes) {
  Write32(destination, bytes);
}

// Encodes one frame without allocating. Returns the byte count, or zero when
// the frame is invalid or the destination capacity is insufficient.
inline std::size_t EncodeFrame(const Frame& frame, char* destination,
                               std::size_t capacity) {
  if (!destination || !ValidFrameFields(frame)) return 0;
  const std::size_t total = kHeaderBytes + frame.payload.size();
  if (capacity < total) return 0;
  Write32(destination, kMagic);
  Write16(destination + 4, static_cast<std::uint16_t>(frame.kind));
  Write16(destination + 6, frame.flags);
  Write32(destination + 8, frame.stream_id);
  Write32(destination + 12, static_cast<std::uint32_t>(frame.payload.size()));
  if (!frame.payload.empty())
    std::char_traits<char>::copy(destination + kHeaderBytes, frame.payload.data(),
                                 frame.payload.size());
  return total;
}

// Decodes and validates one frame. The output payload is a borrowed view of
// the input; the caller must not retain it past the input buffer's lifetime.
inline bool DecodeFrame(const char* data, std::size_t size, Frame& frame) {
  if (!data || size < kHeaderBytes || size > kHeaderBytes + kMaxPayloadBytes)
    return false;
  if (Read32(data) != kMagic) return false;
  const std::uint16_t kind = Read16(data + 4);
  if (kind < static_cast<std::uint16_t>(Kind::kOpen) ||
      kind > static_cast<std::uint16_t>(Kind::kWindow))
    return false;
  const std::uint32_t payload_len = Read32(data + 12);
  if (payload_len > kMaxPayloadBytes ||
      size - kHeaderBytes != static_cast<std::size_t>(payload_len))
    return false;
  Frame decoded;
  decoded.kind = static_cast<Kind>(kind);
  decoded.flags = Read16(data + 6);
  decoded.stream_id = Read32(data + 8);
  decoded.payload = std::string_view(data + kHeaderBytes, payload_len);
  if (!ValidFrameFields(decoded)) return false;
  frame = decoded;
  return true;
}

}  // namespace crossdesk::port_forward

#endif
