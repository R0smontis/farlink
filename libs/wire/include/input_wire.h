/*
 * @Author: DI JUNKUN
 * @Date: 2026-11-27
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _INPUT_WIRE_H_
#define _INPUT_WIRE_H_
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <string>
#include <type_traits>

#include <remote_action.h>

// Zero-allocation binary wire format for hot-path input. Every message is a
// fixed little-endian packet with no heap traffic on either side: encoders
// write into a caller-owned std::array and decoders read in place. Peers
// negotiate the format through the supports_binary_input HostInfo capability;
// legacy peers keep receiving the exact JSON RemoteAction encoding.
//
// The first byte is 'F' (0x46), which can never begin a JSON object ('{'),
// so a receiver can route a frame before capability negotiation finishes.
//
// Packet layout (fixed 32-byte header; KeyState appends a key list):
//   [0..4)   magic "FIN1"
//   [4]      version (1)
//   [5]      kind (InputWire::Kind)
//   [6..8)   flags / button / display_index (uint16)
//   [8..12)  seq (uint32, per-session monotonic)
//   [12..20) send_ts_us (uint64, sender steady_clock microseconds)
//   [20..32) 12-byte payload, kind-specific
//
// The relative-motion payload carries cumulative sender-side counters, so a
// lost or reordered unreliable packet is fully recovered by the next one:
// the receiver injects (cumulative - last_applied) and drops any sequence
// number it has already seen. First-contact and epoch-reset deltas are
// clamped to a sane bound so a hostile or corrupt peer cannot teleport the
// cursor across the desktop in one event.

namespace crossdesk {
namespace input_wire {

inline constexpr std::array<char, 4> kMagic{'F', 'I', 'N', '1'};
inline constexpr uint8_t kVersion = 1;
inline constexpr std::size_t kHeaderBytes = 32;
inline constexpr std::size_t kKeyStateEntryBytes = 13;  // u64 value + u32 scan + u8 ext
inline constexpr std::size_t kMaxKeyStateBytes =
    kHeaderBytes + kMaxKeyboardStateKeys * kKeyStateEntryBytes;
// Largest single binary frame; the wire cap is a hard memory bound.
inline constexpr std::size_t kMaxPacketBytes = kMaxKeyStateBytes;
// Bounds applied to a first-contact or epoch-reset cumulative delta. Normal
// game-mode motion is far below this per event, so the clamp only engages on
// state loss and never truncates a legitimate stream.
inline constexpr int32_t kCumulativeResetClamp = 4096;

enum class Kind : uint8_t {
  invalid = 0,
  move_abs = 1,
  move_rel_cumulative = 2,
  mouse_button = 3,
  scroll_pixel = 4,
  key = 5,
  key_state = 6,
};

// Returns true when a received frame can be a binary input packet. Callers
// must still validate size and fields through Decode; this is only the
// routing discriminator against the legacy JSON wire.
inline bool IsBinaryFrame(const char* data, std::size_t size) {
  return size >= kHeaderBytes && data != nullptr &&
         data[0] == kMagic[0] && data[1] == kMagic[1] && data[2] == kMagic[2] &&
         data[3] == kMagic[3];
}

inline bool IsBinaryFrame(const std::string& data) {
  return IsBinaryFrame(data.data(), data.size());
}

namespace detail {

template <typename T, bool IsEnum>
struct WireUnsignedImpl;

template <typename T>
struct WireUnsignedImpl<T, false> {
  using type = std::make_unsigned_t<T>;
};

template <typename T>
struct WireUnsignedImpl<T, true> {
  using type = std::make_unsigned_t<std::underlying_type_t<T>>;
};

template <typename T>
using WireUnsigned = typename WireUnsignedImpl<T, std::is_enum<T>::value>::type;

template <typename T>
inline void StoreLE(char* out, T value) {
  static_assert(std::is_integral<T>::value || std::is_enum<T>::value,
                "StoreLE requires an integral type");
  using U = WireUnsigned<T>;
  U bits = static_cast<U>(value);
  for (std::size_t i = 0; i < sizeof(T); ++i) {
    out[i] = static_cast<char>((bits >> (8 * i)) & 0xFF);
  }
}

template <typename T>
inline T LoadLE(const char* in) {
  static_assert(std::is_integral<T>::value || std::is_enum<T>::value,
                "LoadLE requires an integral type");
  using U = WireUnsigned<T>;
  U bits = 0;
  for (std::size_t i = 0; i < sizeof(T); ++i) {
    bits |= static_cast<U>(static_cast<uint8_t>(in[i])) << (8 * i);
  }
  return static_cast<T>(bits);
}

inline void StoreFloatLE(char* out, float value) {
  uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  StoreLE<uint32_t>(out, bits);
}

inline float LoadFloatLE(const char* in) {
  const uint32_t bits = LoadLE<uint32_t>(in);
  float value = 0.0f;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

// Writes the fixed header shared by every kind. Returns the payload cursor.
inline char* WriteHeader(char* out, Kind kind, uint16_t flags, uint32_t seq,
                         uint64_t send_ts_us) {
  out[0] = kMagic[0];
  out[1] = kMagic[1];
  out[2] = kMagic[2];
  out[3] = kMagic[3];
  out[4] = static_cast<char>(kVersion);
  out[5] = static_cast<char>(kind);
  StoreLE<uint16_t>(out + 6, flags);
  StoreLE<uint32_t>(out + 8, seq);
  StoreLE<uint64_t>(out + 12, send_ts_us);
  return out + 20;
}

struct Header {
  Kind kind = Kind::invalid;
  uint16_t flags = 0;
  uint32_t seq = 0;
  uint64_t send_ts_us = 0;
};

// Validates magic/version/length and reads the fixed header. |size| must
// already cover the payload for |header.kind|; callers check payload-bearing
// lengths per kind before reading the body.
inline bool ReadHeader(const char* data, std::size_t size, Header& header) {
  if (!data || size < kHeaderBytes) return false;
  if (data[0] != kMagic[0] || data[1] != kMagic[1] || data[2] != kMagic[2] ||
      data[3] != kMagic[3])
    return false;
  if (static_cast<uint8_t>(data[4]) != kVersion) return false;
  const uint8_t kind = static_cast<uint8_t>(data[5]);
  if (kind < static_cast<uint8_t>(Kind::move_abs) ||
      kind > static_cast<uint8_t>(Kind::key_state))
    return false;
  header.kind = static_cast<Kind>(kind);
  header.flags = LoadLE<uint16_t>(data + 6);
  header.seq = LoadLE<uint32_t>(data + 8);
  header.send_ts_us = LoadLE<uint64_t>(data + 12);
  return true;
}

}  // namespace detail

// ---------------------------------------------------------------------------
// Stateless packet codecs. Each Encode writes into a caller-provided buffer
// and returns the byte count (or 0 on invalid input); Decode performs the
// inverse with full validation. Neither allocates.
// ---------------------------------------------------------------------------

// MoveAbs payload: float x, float y, uint32 reserved.
inline std::size_t EncodeMoveAbs(char* out, float x, float y, uint32_t seq,
                                 uint64_t send_ts_us) {
  if (!out || !std::isfinite(x) || !std::isfinite(y)) return 0;
  char* payload = detail::WriteHeader(out, Kind::move_abs, 0, seq, send_ts_us);
  detail::StoreFloatLE(payload, x);
  detail::StoreFloatLE(payload + 4, y);
  detail::StoreLE<uint32_t>(payload + 8, 0);
  return kHeaderBytes;
}

// MoveRelCumulative payload: int32 cum_dx, int32 cum_dy, uint32 epoch.
// Counters are resolution-independent wire units scaled by 2^16 per unit so
// sub-pixel fractions survive exactly; see CumulativeScale.
inline constexpr double kCumulativeScale = 65536.0;

inline std::size_t EncodeMoveRelCumulative(char* out, int32_t cum_dx,
                                           int32_t cum_dy, uint32_t epoch,
                                           uint32_t seq,
                                           uint64_t send_ts_us) {
  if (!out) return 0;
  char* payload =
      detail::WriteHeader(out, Kind::move_rel_cumulative, 0, seq, send_ts_us);
  detail::StoreLE<int32_t>(payload, cum_dx);
  detail::StoreLE<int32_t>(payload + 4, cum_dy);
  detail::StoreLE<uint32_t>(payload + 8, epoch);
  return kHeaderBytes;
}

// MouseButton payload: float x, float y, uint8 mouse_flag, uint8 display_idx,
// uint16 reserved — in absolute mode. In relative (game) mode the absolute
// coordinates are meaningless because the receiver anchors clicks at the
// tracked position, so the x/y slots instead carry the sender's cumulative
// motion counters (int32 fixed-point cum_dx/cum_dy) and the header flags
// field gains kButtonFlagCumulative plus the epoch's low 8 bits. This is the
// click batch boundary (moonlight-common-c InputStream.c:941): the receiver
// applies any missing relative motion BEFORE injecting the click, so a click
// can never land behind a preceding flick whose unreliable motion packet was
// lost — the reliable click is self-synchronizing.
inline constexpr uint16_t kButtonFlagCumulative = 1u;
inline constexpr uint16_t kButtonEpochShift = 1u;  // epoch low bits at [1..9)

inline std::size_t EncodeMouseButton(char* out, float x, float y,
                                     MouseFlag mouse_flag, uint8_t display_idx,
                                     uint32_t seq, uint64_t send_ts_us) {
  if (!out || !std::isfinite(x) || !std::isfinite(y)) return 0;
  char* payload =
      detail::WriteHeader(out, Kind::mouse_button, 0, seq, send_ts_us);
  detail::StoreFloatLE(payload, x);
  detail::StoreFloatLE(payload + 4, y);
  payload[8] = static_cast<char>(mouse_flag);
  payload[9] = static_cast<char>(display_idx);
  detail::StoreLE<uint16_t>(payload + 10, 0);
  return kHeaderBytes;
}

// Relative-mode variant embedding the cumulative counters. |cum_dx|/|cum_dy|
// are the fixed-point wire counters exactly as EncodeMoveRelCumulative
// carries them; |epoch| is truncated to its low 8 bits, which is enough to
// detect a sender restart because epochs only ever increment.
inline std::size_t EncodeMouseButtonCumulative(
    char* out, int32_t cum_dx, int32_t cum_dy, uint32_t epoch,
    MouseFlag mouse_flag, uint8_t display_idx, uint32_t seq,
    uint64_t send_ts_us) {
  if (!out) return 0;
  const uint16_t flags = static_cast<uint16_t>(
      kButtonFlagCumulative |
      ((epoch & 0xFFu) << kButtonEpochShift));
  char* payload =
      detail::WriteHeader(out, Kind::mouse_button, flags, seq, send_ts_us);
  detail::StoreLE<int32_t>(payload, cum_dx);
  detail::StoreLE<int32_t>(payload + 4, cum_dy);
  payload[8] = static_cast<char>(mouse_flag);
  payload[9] = static_cast<char>(display_idx);
  detail::StoreLE<uint16_t>(payload + 10, 0);
  return kHeaderBytes;
}

// ScrollPixel payload: float dx_px, float dy_px, int16 legacy_steps_x,
// int16 legacy_steps_y. The fractional pixel deltas carry the primary smooth
// scrolling stream; the legacy step counts let hosts without pixel injection
// fall back to whole-notch events without losing small scrolls.
inline std::size_t EncodeScrollPixel(char* out, float dx_px, float dy_px,
                                     int16_t legacy_steps_x,
                                     int16_t legacy_steps_y, uint32_t seq,
                                     uint64_t send_ts_us) {
  if (!out || !std::isfinite(dx_px) || !std::isfinite(dy_px)) return 0;
  char* payload =
      detail::WriteHeader(out, Kind::scroll_pixel, 0, seq, send_ts_us);
  detail::StoreFloatLE(payload, dx_px);
  detail::StoreFloatLE(payload + 4, dy_px);
  detail::StoreLE<int16_t>(payload + 8, legacy_steps_x);
  detail::StoreLE<int16_t>(payload + 10, legacy_steps_y);
  return kHeaderBytes;
}

// Key payload: uint64 key_value, uint32 scan_code packed with
// extended/flag bits. scan_code stays a full uint32 so no platform key
// metadata is lost.
inline std::size_t EncodeKey(char* out, uint64_t key_value, uint32_t scan_code,
                             bool extended, KeyFlag key_flag, uint32_t seq,
                             uint64_t send_ts_us) {
  if (!out) return 0;
  char* payload = detail::WriteHeader(out, Kind::key, 0, seq, send_ts_us);
  detail::StoreLE<uint64_t>(payload, key_value);
  detail::StoreLE<uint32_t>(payload + 8, scan_code & 0x7FFFFFFFu);
  // extended and the key flag do not fit the padding; the header's flag
  // field carries them so the payload layout stays fixed-width.
  detail::StoreLE<uint16_t>(out + 6,
                            static_cast<uint16_t>((extended ? 1u : 0u) |
                                                  (key_flag == KeyFlag::key_up ? 2u : 0u)));
  return kHeaderBytes;
}

// KeyState payload: uint32 keyboard-state seq, uint8 pressed_count, then
// pressed_count entries of (uint64 key_value, uint32 scan_code, uint8
// extended). Returns 0 when the list overflows the wire maximum.
inline std::size_t EncodeKeyState(char* out, const KeyboardState& state,
                                  uint32_t seq, uint64_t send_ts_us) {
  if (!out) return 0;
  const std::size_t pressed_count =
      std::min(state.pressed_count, kMaxKeyboardStateKeys);
  char* payload = detail::WriteHeader(out, Kind::key_state, 0, seq, send_ts_us);
  detail::StoreLE<uint32_t>(payload, state.seq);
  payload[4] = static_cast<char>(pressed_count);
  payload[5] = 0;
  detail::StoreLE<uint16_t>(payload + 6, 0);
  char* entry = payload + 8;  // entries start inside the fixed 12-byte window
  for (std::size_t i = 0; i < pressed_count; ++i) {
    detail::StoreLE<uint64_t>(entry, state.pressed_keys[i].key_value);
    detail::StoreLE<uint32_t>(entry + 8, state.pressed_keys[i].scan_code);
    entry[12] = state.pressed_keys[i].extended ? 1 : 0;
    entry += kKeyStateEntryBytes;
  }
  return kHeaderBytes + pressed_count * kKeyStateEntryBytes;
}

// Decoded binary input. The union view mirrors the wire payload so receivers
// can inject without rebuilding a full RemoteAction, while |action| carries
// the canonical RemoteAction translation for the shared input pipeline.
struct Decoded {
  Kind kind = Kind::invalid;
  uint32_t seq = 0;
  uint64_t send_ts_us = 0;
  RemoteAction action{};
  // move_rel_cumulative raw counters; receivers need the cumulative values
  // (not the RemoteAction's per-event dx/dy) to compute loss-immune deltas.
  int32_t cum_dx = 0;
  int32_t cum_dy = 0;
  uint32_t epoch = 0;
  // scroll_pixel primary deltas in physical pixels.
  float scroll_dx_px = 0.0f;
  float scroll_dy_px = 0.0f;
  // True when a mouse_button frame embeds the sender's cumulative motion
  // counters (relative/game mode). The receiver must flush any missing
  // motion before injecting the click — the click batch boundary.
  bool button_has_cumulative = false;
};

// Decodes one binary frame into |output|. Returns false on any malformed,
// truncated, out-of-range or non-finite content; |output| is untouched on
// failure. Never allocates.
inline bool Decode(const char* data, std::size_t size, Decoded& output) {
  detail::Header header;
  if (!detail::ReadHeader(data, size, header)) return false;
  const char* payload = data + 20;

  Decoded result{};
  result.kind = header.kind;
  result.seq = header.seq;
  result.send_ts_us = header.send_ts_us;

  switch (header.kind) {
    case Kind::move_abs: {
      result.action.type = ControlType::mouse;
      result.action.m.flag = MouseFlag::move;
      result.action.m.x = detail::LoadFloatLE(payload);
      result.action.m.y = detail::LoadFloatLE(payload + 4);
      result.action.m.s = 0;
      result.action.m.dx = 0;
      result.action.m.dy = 0;
      if (!std::isfinite(result.action.m.x) || !std::isfinite(result.action.m.y))
        return false;
      break;
    }
    case Kind::move_rel_cumulative: {
      result.cum_dx = detail::LoadLE<int32_t>(payload);
      result.cum_dy = detail::LoadLE<int32_t>(payload + 4);
      result.epoch = detail::LoadLE<uint32_t>(payload + 8);
      result.action.type = ControlType::mouse;
      result.action.m.flag = MouseFlag::move_relative;
      result.action.m.x = 0;
      result.action.m.y = 0;
      result.action.m.s = 0;
      // dx/dy are filled in by the receiver's cumulative tracker, not here:
      // the wire carries positions-in-time, and only the sequence-aware
      // decoder knows how much motion this frame contributes.
      result.action.m.dx = 0;
      result.action.m.dy = 0;
      break;
    }
    case Kind::mouse_button: {
      const int flag = static_cast<uint8_t>(payload[8]);
      // Only real button transitions are valid here; moves and wheels have
      // their own kinds and must never be smuggled through this one.
      if (flag < static_cast<int>(MouseFlag::left_down) ||
          flag > static_cast<int>(MouseFlag::middle_up))
        return false;
      result.action.type = ControlType::mouse;
      result.action.m.flag = static_cast<MouseFlag>(flag);
      result.action.m.s = 0;
      if ((header.flags & kButtonFlagCumulative) != 0) {
        // Relative-mode click: the x/y slots carry the sender's cumulative
        // motion counters so the receiver can close the batch boundary
        // before injecting the button. The action's own x/y stay zero —
        // the host anchors relative-mode clicks at the tracked position.
        result.cum_dx = detail::LoadLE<int32_t>(payload);
        result.cum_dy = detail::LoadLE<int32_t>(payload + 4);
        result.epoch = (header.flags >> kButtonEpochShift) & 0xFFu;
        result.button_has_cumulative = true;
        result.action.m.x = 0;
        result.action.m.y = 0;
      } else {
        result.action.m.x = detail::LoadFloatLE(payload);
        result.action.m.y = detail::LoadFloatLE(payload + 4);
        if (!std::isfinite(result.action.m.x) ||
            !std::isfinite(result.action.m.y))
          return false;
      }
      result.action.m.dx = 0;
      result.action.m.dy = 0;
      break;
    }
    case Kind::scroll_pixel: {
      result.scroll_dx_px = detail::LoadFloatLE(payload);
      result.scroll_dy_px = detail::LoadFloatLE(payload + 4);
      const int16_t legacy_x = detail::LoadLE<int16_t>(payload + 8);
      const int16_t legacy_y = detail::LoadLE<int16_t>(payload + 10);
      if (!std::isfinite(result.scroll_dx_px) ||
          !std::isfinite(result.scroll_dy_px))
        return false;
      result.action.type = ControlType::mouse;
      // Pick the dominant axis for the legacy fallback so hosts without
      // pixel injection behave exactly as before; the pixel-precise path
      // consumes scroll_dx_px/scroll_dy_px directly.
      const bool vertical =
          std::abs(result.scroll_dy_px) >= std::abs(result.scroll_dx_px);
      result.action.m.flag = vertical ? MouseFlag::wheel_vertical
                                      : MouseFlag::wheel_horizontal;
      result.action.m.x = 0;
      result.action.m.y = 0;
      result.action.m.s = vertical ? static_cast<int>(legacy_y)
                                   : static_cast<int>(legacy_x);
      result.action.m.dx = result.scroll_dx_px;
      result.action.m.dy = result.scroll_dy_px;
      break;
    }
    case Kind::key: {
      result.action.type = ControlType::keyboard;
      result.action.k.key_value =
          static_cast<std::size_t>(detail::LoadLE<uint64_t>(payload));
      result.action.k.scan_code = detail::LoadLE<uint32_t>(payload + 8);
      result.action.k.extended = (header.flags & 1u) != 0;
      result.action.k.flag =
          (header.flags & 2u) != 0 ? KeyFlag::key_up : KeyFlag::key_down;
      break;
    }
    case Kind::key_state: {
      const uint8_t pressed_count = static_cast<uint8_t>(payload[4]);
      if (pressed_count > kMaxKeyboardStateKeys) return false;
      const std::size_t expected =
          kHeaderBytes + pressed_count * kKeyStateEntryBytes;
      if (size != expected) return false;
      result.action.type = ControlType::keyboard_state;
      result.action.ks.seq = detail::LoadLE<uint32_t>(payload);
      result.action.ks.pressed_count = pressed_count;
      const char* entry = payload + 8;
      for (std::size_t i = 0; i < pressed_count; ++i) {
        result.action.ks.pressed_keys[i].key_value =
            static_cast<std::size_t>(detail::LoadLE<uint64_t>(entry));
        result.action.ks.pressed_keys[i].scan_code =
            detail::LoadLE<uint32_t>(entry + 8);
        result.action.ks.pressed_keys[i].extended = entry[12] != 0;
        entry += kKeyStateEntryBytes;
      }
      break;
    }
    default:
      return false;
  }

  output = result;
  return true;
}

inline bool Decode(const std::string& data, Decoded& output) {
  return Decode(data.data(), data.size(), output);
}

// ---------------------------------------------------------------------------
// Sender-side stateful encoder. Owns the per-session sequence counter and the
// cumulative relative-motion counters, so individual moves are added into a
// running total rather than sent as per-packet displacements. One instance
// per (session, direction); reconnection constructs a fresh instance, which
// restarts the epoch and lets the receiver resynchronize without ambiguity.
// ---------------------------------------------------------------------------
class Encoder {
 public:
  Encoder() = default;

  uint32_t next_seq() { return ++seq_; }

  template <typename Clock = std::chrono::steady_clock>
  static uint64_t NowMicroseconds() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            Clock::now().time_since_epoch())
            .count());
  }

  // Adds a relative motion increment to the cumulative counters. Fractional
  // input is preserved in double so repeated sub-unit device deltas are not
  // quantized away before encoding.
  void AddRelative(double dx, double dy) {
    cum_dx_exact_ += dx;
    cum_dy_exact_ += dy;
  }

  void ResetRelative() {
    cum_dx_exact_ = 0;
    cum_dy_exact_ = 0;
    ++epoch_;
  }

  // Encodes the current cumulative relative state. The receiver computes the
  // actual per-event delta by subtracting the last counters it applied, so
  // losing this exact packet costs nothing.
  std::size_t EncodeRelative(char* out, uint64_t send_ts_us) {
    const int32_t cum_dx =
        static_cast<int32_t>(std::lround(cum_dx_exact_ * kCumulativeScale));
    const int32_t cum_dy =
        static_cast<int32_t>(std::lround(cum_dy_exact_ * kCumulativeScale));
    return EncodeMoveRelCumulative(out, cum_dx, cum_dy, epoch_, ++seq_,
                                   send_ts_us);
  }

  std::size_t EncodeMoveAbs(char* out, float x, float y, uint64_t send_ts_us) {
    return input_wire::EncodeMoveAbs(out, x, y, ++seq_, send_ts_us);
  }

  std::size_t EncodeMouseButton(char* out, float x, float y,
                                MouseFlag mouse_flag, uint8_t display_idx,
                                uint64_t send_ts_us) {
    return input_wire::EncodeMouseButton(out, x, y, mouse_flag, display_idx,
                                         ++seq_, send_ts_us);
  }

  std::size_t EncodeScrollPixel(char* out, float dx_px, float dy_px,
                                int16_t legacy_steps_x, int16_t legacy_steps_y,
                                uint64_t send_ts_us) {
    return input_wire::EncodeScrollPixel(out, dx_px, dy_px, legacy_steps_x,
                                         legacy_steps_y, ++seq_, send_ts_us);
  }

  std::size_t EncodeKey(char* out, uint64_t key_value, uint32_t scan_code,
                        bool extended, KeyFlag key_flag, uint64_t send_ts_us) {
    return input_wire::EncodeKey(out, key_value, scan_code, extended, key_flag,
                                 ++seq_, send_ts_us);
  }

  std::size_t EncodeKeyState(char* out, const KeyboardState& state,
                             uint64_t send_ts_us) {
    return input_wire::EncodeKeyState(out, state, ++seq_, send_ts_us);
  }

  double cum_dx_exact() const { return cum_dx_exact_; }
  double cum_dy_exact() const { return cum_dy_exact_; }
  uint32_t epoch() const { return epoch_; }
  // Fixed-point wire counters exactly as the next move_rel_cumulative frame
  // would carry them. Button frames embed these so the receiver can close
  // the click batch boundary without waiting for the unreliable stream.
  int32_t cum_dx_wire() const {
    return static_cast<int32_t>(std::lround(cum_dx_exact_ * kCumulativeScale));
  }
  int32_t cum_dy_wire() const {
    return static_cast<int32_t>(std::lround(cum_dy_exact_ * kCumulativeScale));
  }

  // Relative-mode click: embeds the current cumulative counters, making the
  // reliable click self-synchronizing against lost unreliable motion.
  std::size_t EncodeMouseButtonCumulative(char* out, MouseFlag mouse_flag,
                                          uint8_t display_idx,
                                          uint64_t send_ts_us) {
    return input_wire::EncodeMouseButtonCumulative(
        out, cum_dx_wire(), cum_dy_wire(), epoch_, mouse_flag, display_idx,
        ++seq_, send_ts_us);
  }

 private:
  uint32_t seq_ = 0;
  uint32_t epoch_ = 0;
  double cum_dx_exact_ = 0;
  double cum_dy_exact_ = 0;
};

// ---------------------------------------------------------------------------
// Receiver-side stateful decoder. Tracks per-kind sequence numbers and the
// last applied cumulative counters, so reordered packets never move the
// cursor backwards and dropped packets are recovered exactly by the next
// cumulative update.
// ---------------------------------------------------------------------------
class Decoder {
 public:
  Decoder() = default;

  // Result of feeding one move_rel_cumulative frame. |dx|/|dy| are the exact
  // motion to inject, already scaled back into wire units (divide by
  // kCumulativeScale for the fractional unit the sender accumulated).
  struct RelativeDelta {
    bool accepted = false;
    double dx = 0;
    double dy = 0;
  };

  // Sequence gate for absolute moves: returns true when this frame is newer
  // than every move_abs applied so far. The first frame is always accepted.
  bool AcceptMoveAbs(uint32_t seq) {
    if (!seen_move_abs_ ||
        static_cast<int32_t>(seq - last_move_abs_seq_) > 0) {
      last_move_abs_seq_ = seq;
      seen_move_abs_ = true;
      return true;
    }
    return false;
  }

  // Consumes one cumulative relative frame. Frames at or behind the last
  // applied sequence are dropped as duplicates/reorders. An epoch change
  // resynchronizes the counters without injecting the clamped jump.
  RelativeDelta ConsumeRelative(uint32_t seq, int32_t cum_dx, int32_t cum_dy,
                                uint32_t epoch) {
    RelativeDelta result;
    if (seen_rel_ && static_cast<int32_t>(seq - last_rel_seq_) <= 0) {
      return result;  // stale or duplicate
    }
    last_rel_seq_ = seq;
    if (!seen_rel_ || epoch != last_epoch_) {
      // First contact or a sender restart: adopt the counters as the new
      // baseline. Injecting the full cumulative displacement here would
      // re-apply motion from before the epoch boundary, so only the clamped
      // remainder counts. In practice the sender zeroes its counters on an
      // epoch reset, so the first frame of the new epoch carries exactly the
      // motion since that reset.
      last_cum_dx_ = cum_dx;
      last_cum_dy_ = cum_dy;
      last_epoch_ = epoch;
      seen_rel_ = true;
      result.accepted = true;
      const int32_t clamped_dx =
          std::max(-kCumulativeResetClamp, std::min(kCumulativeResetClamp, cum_dx));
      const int32_t clamped_dy =
          std::max(-kCumulativeResetClamp, std::min(kCumulativeResetClamp, cum_dy));
      result.dx = static_cast<double>(clamped_dx) / kCumulativeScale;
      result.dy = static_cast<double>(clamped_dy) / kCumulativeScale;
      return result;
    }
    const int32_t delta_dx = cum_dx - last_cum_dx_;
    const int32_t delta_dy = cum_dy - last_cum_dy_;
    last_cum_dx_ = cum_dx;
    last_cum_dy_ = cum_dy;
    result.accepted = true;
    result.dx = static_cast<double>(delta_dx) / kCumulativeScale;
    result.dy = static_cast<double>(delta_dy) / kCumulativeScale;
    return result;
  }

  // Click batch boundary flush (moonlight-common-c InputStream.c:941-947).
  // A relative-mode button frame embeds the sender's cumulative counters at
  // click time. Feeding them here applies exactly the motion still missing
  // from the unreliable stream — no more, no less — and advances the
  // relative sequence gate past the click, so a late-arriving pre-click
  // motion packet is correctly dropped instead of double-applying. Returns
  // the delta to inject BEFORE the button event; the caller injects the
  // returned motion first, then the click, atomically from the host's point
  // of view.
  RelativeDelta FlushRelativeForButton(uint32_t seq, int32_t cum_dx,
                                       int32_t cum_dy, uint32_t epoch) {
    // The button shares the sender's single sequence space, so the same
    // gate arithmetic applies; stale buttons simply carry no new motion.
    return ConsumeRelative(seq, cum_dx, cum_dy, epoch);
  }

 private:
  bool seen_move_abs_ = false;
  bool seen_rel_ = false;
  uint32_t last_move_abs_seq_ = 0;
  uint32_t last_rel_seq_ = 0;
  uint32_t last_epoch_ = 0;
  int32_t last_cum_dx_ = 0;
  int32_t last_cum_dy_ = 0;
};

}  // namespace input_wire
}  // namespace crossdesk

#endif
