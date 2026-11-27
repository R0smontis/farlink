// Unit + microbenchmark tests for the zero-allocation binary input wire.
// Covers round-trip fidelity of every kind, out-of-order absolute moves,
// cumulative relative motion under simulated packet loss, malformed-frame
// rejection, capability round-tripping, legacy coexistence, and a timed
// comparison against the JSON RemoteAction encoding.

#include <input_wire.h>

#include <nlohmann/json.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <random>
#include <string>
#include <vector>

namespace {

int failures = 0;

void Check(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

using crossdesk::input_wire::Decode;
using crossdesk::input_wire::Decoded;
using crossdesk::input_wire::Encoder;
using crossdesk::input_wire::kHeaderBytes;
using crossdesk::input_wire::Kind;

// ---------------------------------------------------------------------------
// Round-trip tests
// ---------------------------------------------------------------------------

void TestMoveAbsRoundTrip() {
  Encoder encoder;
  std::array<char, 64> buffer{};
  const std::size_t size = encoder.EncodeMoveAbs(buffer.data(), 0.25f, 0.75f, 1000);
  Check(size == kHeaderBytes, "move_abs encode size");
  Check(buffer[0] == 'F' && buffer[1] == 'I' && buffer[2] == 'N' && buffer[3] == '1',
        "move_abs magic");

  Decoded decoded{};
  Check(Decode(buffer.data(), size, decoded), "move_abs decode");
  Check(decoded.kind == Kind::move_abs, "move_abs kind");
  Check(decoded.seq == 1, "move_abs seq increments");
  Check(decoded.send_ts_us == 1000, "move_abs timestamp");
  Check(decoded.action.type == crossdesk::ControlType::mouse, "move_abs action type");
  Check(decoded.action.m.flag == crossdesk::MouseFlag::move, "move_abs flag");
  Check(decoded.action.m.x == 0.25f && decoded.action.m.y == 0.75f,
        "move_abs coordinates");
}

void TestMoveRelCumulativeRoundTrip() {
  Encoder encoder;
  std::array<char, 64> buffer{};
  encoder.AddRelative(0.5, -0.25);
  encoder.AddRelative(0.25, 0.25);  // cumulative (0.75, 0.0) in wire units
  const std::size_t size = encoder.EncodeRelative(buffer.data(), 2000);
  Check(size == kHeaderBytes, "move_rel encode size");

  Decoded decoded{};
  Check(Decode(buffer.data(), size, decoded), "move_rel decode");
  Check(decoded.kind == Kind::move_rel_cumulative, "move_rel kind");
  Check(decoded.cum_dx == static_cast<int32_t>(0.75 * crossdesk::input_wire::kCumulativeScale),
        "move_rel cum_dx scaled");
  Check(decoded.cum_dy == 0, "move_rel cum_dy exact zero");
}

void TestMouseButtonRoundTrip() {
  Encoder encoder;
  std::array<char, 64> buffer{};
  const std::size_t size = encoder.EncodeMouseButton(
      buffer.data(), 0.5f, 0.5f, crossdesk::MouseFlag::left_down, 2, 3000);
  Check(size == kHeaderBytes, "mouse_button encode size");

  Decoded decoded{};
  Check(Decode(buffer.data(), size, decoded), "mouse_button decode");
  Check(decoded.kind == Kind::mouse_button, "mouse_button kind");
  Check(decoded.action.m.flag == crossdesk::MouseFlag::left_down,
        "mouse_button flag");
  Check(decoded.action.m.x == 0.5f && decoded.action.m.y == 0.5f,
        "mouse_button coordinates");
}

void TestScrollPixelRoundTrip() {
  Encoder encoder;
  std::array<char, 64> buffer{};
  const std::size_t size =
      encoder.EncodeScrollPixel(buffer.data(), 1.5f, -42.25f, 0, -1, 4000);
  Check(size == kHeaderBytes, "scroll_pixel encode size");

  Decoded decoded{};
  Check(Decode(buffer.data(), size, decoded), "scroll_pixel decode");
  Check(decoded.kind == Kind::scroll_pixel, "scroll_pixel kind");
  Check(decoded.scroll_dx_px == 1.5f && decoded.scroll_dy_px == -42.25f,
        "scroll_pixel pixel deltas");
  // Dominant axis vertical: flag + legacy step should reflect dy.
  Check(decoded.action.m.flag == crossdesk::MouseFlag::wheel_vertical,
        "scroll_pixel dominant vertical");
  Check(decoded.action.m.s == -1, "scroll_pixel legacy steps");
}

void TestKeyRoundTrip() {
  Encoder encoder;
  std::array<char, 64> buffer{};
  const std::size_t size =
      encoder.EncodeKey(buffer.data(), 0x1F2E3D4C, 0xE01D, true,
                        crossdesk::KeyFlag::key_up, 5000);
  Check(size == kHeaderBytes, "key encode size");

  Decoded decoded{};
  Check(Decode(buffer.data(), size, decoded), "key decode");
  Check(decoded.kind == Kind::key, "key kind");
  Check(decoded.action.type == crossdesk::ControlType::keyboard, "key type");
  Check(decoded.action.k.key_value == 0x1F2E3D4C, "key value preserved");
  Check(decoded.action.k.scan_code == 0xE01D, "key scan_code preserved");
  Check(decoded.action.k.extended, "key extended preserved");
  Check(decoded.action.k.flag == crossdesk::KeyFlag::key_up, "key flag up");

  // key_down / not extended variant
  const std::size_t size2 =
      encoder.EncodeKey(buffer.data(), 65, 30, false,
                        crossdesk::KeyFlag::key_down, 5001);
  Check(Decode(buffer.data(), size2, decoded), "key decode down");
  Check(!decoded.action.k.extended, "key extended false");
  Check(decoded.action.k.flag == crossdesk::KeyFlag::key_down, "key flag down");
}

void TestKeyStateRoundTrip() {
  Encoder encoder;
  std::array<char, crossdesk::input_wire::kMaxPacketBytes> buffer{};
  crossdesk::KeyboardState state{};
  state.seq = 77;
  state.pressed_count = 3;
  state.pressed_keys[0] = {65, 30, false};
  state.pressed_keys[1] = {0xA3, 29, true};
  state.pressed_keys[2] = {0x5B, 0xE05B, true};
  const std::size_t size = encoder.EncodeKeyState(buffer.data(), state, 6000);
  const std::size_t expected =
      kHeaderBytes + 3 * crossdesk::input_wire::kKeyStateEntryBytes;
  Check(size == expected, "key_state encode size");

  Decoded decoded{};
  Check(Decode(buffer.data(), size, decoded), "key_state decode");
  Check(decoded.kind == Kind::key_state, "key_state kind");
  Check(decoded.action.type == crossdesk::ControlType::keyboard_state,
        "key_state action type");
  Check(decoded.action.ks.seq == 77, "key_state seq");
  Check(decoded.action.ks.pressed_count == 3, "key_state count");
  Check(decoded.action.ks.pressed_keys[1].key_value == 0xA3 &&
            decoded.action.ks.pressed_keys[1].scan_code == 29 &&
            decoded.action.ks.pressed_keys[1].extended,
        "key_state entry 1 preserved");

  // Empty pressed list round-trips.
  state.pressed_count = 0;
  const std::size_t size0 = encoder.EncodeKeyState(buffer.data(), state, 6001);
  Check(size0 == kHeaderBytes, "key_state empty size");
  Check(Decode(buffer.data(), size0, decoded), "key_state empty decode");
  Check(decoded.action.ks.pressed_count == 0, "key_state empty count");
}

// ---------------------------------------------------------------------------
// Sequencing / loss recovery
// ---------------------------------------------------------------------------

void TestMoveAbsOutOfOrder() {
  crossdesk::input_wire::Decoder decoder;
  // seq 10 arrives first, then stale 9 and duplicate 10, then fresh 11.
  Check(decoder.AcceptMoveAbs(10), "first move_abs accepted");
  Check(!decoder.AcceptMoveAbs(9), "stale move_abs dropped");
  Check(!decoder.AcceptMoveAbs(10), "duplicate move_abs dropped");
  Check(decoder.AcceptMoveAbs(11), "newer move_abs accepted");
  // Wraparound: sequence numbers close to UINT32_MAX must compare correctly.
  crossdesk::input_wire::Decoder wrap;
  Check(wrap.AcceptMoveAbs(0xFFFFFFFEu), "wrap baseline");
  Check(wrap.AcceptMoveAbs(1u), "wrap crossing accepted");
  Check(!wrap.AcceptMoveAbs(0xFFFFFFFFu), "wrap stale dropped");
}

void TestMoveRelLossRecovery() {
  // Simulate 1,000 relative moves with 30% random packet loss. The cumulative
  // wire must deliver the exact total displacement despite the drops.
  std::mt19937 rng(1234);
  std::uniform_real_distribution<double> move(-0.01, 0.01);
  std::uniform_real_distribution<double> drop(0.0, 1.0);

  Encoder encoder;
  crossdesk::input_wire::Decoder decoder;
  std::array<char, 64> buffer{};

  double truth_dx = 0, truth_dy = 0;
  double applied_dx = 0, applied_dy = 0;
  int sent = 0, dropped = 0;

  for (int i = 0; i < 1000; ++i) {
    const double dx = move(rng);
    const double dy = move(rng);
    truth_dx += dx;
    truth_dy += dy;
    encoder.AddRelative(dx, dy);
    const std::size_t size = encoder.EncodeRelative(buffer.data(), 1000 + i);
    ++sent;
    if (drop(rng) < 0.30) {
      ++dropped;  // packet lost in transit
      continue;
    }
    Decoded decoded{};
    if (!Decode(buffer.data(), size, decoded)) {
      Check(false, "move_rel decode failed mid-stream");
      break;
    }
    const auto delta = decoder.ConsumeRelative(decoded.seq, decoded.cum_dx,
                                               decoded.cum_dy, decoded.epoch);
    if (delta.accepted) {
      applied_dx += delta.dx;
      applied_dy += delta.dy;
    }
  }
  Check(dropped > 150, "loss simulation actually dropped packets");
  // The encoder quantizes to 1/65536 wire units, so exactness is bounded by
  // half a unit per axis total (the running total is only rounded once per
  // encode, and drops/reorders never accumulate further error).
  const double tolerance = 2.0 / crossdesk::input_wire::kCumulativeScale;
  Check(std::abs(applied_dx - truth_dx) < tolerance,
        "loss-recovered dx matches ground truth");
  Check(std::abs(applied_dy - truth_dy) < tolerance,
        "loss-recovered dy matches ground truth");
  // Integer exactness in wire units: the injected total must equal the
  // encoder's final cumulative counters with zero drift, because every
  // injected delta is a difference of cumulative snapshots.
  std::array<char, 64> final{};
  const std::size_t final_size = encoder.EncodeRelative(final.data(), 99999);
  Decoded final_decoded{};
  Check(Decode(final.data(), final_size, final_decoded), "final cumulative decodes");
  const int64_t applied_units_dx =
      static_cast<int64_t>(std::llround(applied_dx * crossdesk::input_wire::kCumulativeScale));
  const int64_t applied_units_dy =
      static_cast<int64_t>(std::llround(applied_dy * crossdesk::input_wire::kCumulativeScale));
  Check(applied_units_dx == final_decoded.cum_dx,
        "wire-unit dx recovery is integer exact");
  Check(applied_units_dy == final_decoded.cum_dy,
        "wire-unit dy recovery is integer exact");
  std::cout << "  loss recovery: sent=" << sent << " dropped=" << dropped
            << " truth_dx=" << truth_dx << " applied_dx=" << applied_dx
            << " err_dx=" << std::abs(applied_dx - truth_dx)
            << " wire_units dx=" << applied_units_dx << "/" << final_decoded.cum_dx
            << " dy=" << applied_units_dy << "/" << final_decoded.cum_dy << '\n';
}

void TestMoveRelOutOfOrderRecovery() {
  Encoder encoder;
  crossdesk::input_wire::Decoder decoder;
  std::array<char, 64> buffer{};

  // Generate three cumulative states with realistic sub-clamp increments:
  // A(0.01,0.01) seq1, B(0.03,0.01) seq2, C(0.03,0.04) seq3.
  encoder.AddRelative(0.01, 0.01);
  std::array<char, 64> a{};
  encoder.EncodeRelative(a.data(), 1);
  encoder.AddRelative(0.02, 0);
  std::array<char, 64> b{};
  encoder.EncodeRelative(b.data(), 2);
  encoder.AddRelative(0, 0.03);
  std::array<char, 64> c{};
  const std::size_t csize = encoder.EncodeRelative(c.data(), 3);

  // Deliver C first, then B and A out of order; the stale ones must drop.
  Decoded decoded{};
  Check(Decode(c.data(), csize, decoded), "decode C");
  auto delta = decoder.ConsumeRelative(decoded.seq, decoded.cum_dx,
                                       decoded.cum_dy, decoded.epoch);
  Check(delta.accepted, "first cumulative accepted");
  // First contact injects the cumulative position (under the reset clamp).
  Check(std::abs(delta.dx - 0.03) < 2e-5 && std::abs(delta.dy - 0.04) < 2e-5,
        "first cumulative injects clamped position");

  Check(Decode(b.data(), kHeaderBytes, decoded), "decode B");
  delta = decoder.ConsumeRelative(decoded.seq, decoded.cum_dx, decoded.cum_dy,
                                  decoded.epoch);
  Check(!delta.accepted, "stale cumulative dropped");
  Check(Decode(a.data(), kHeaderBytes, decoded), "decode A");
  delta = decoder.ConsumeRelative(decoded.seq, decoded.cum_dx, decoded.cum_dy,
                                  decoded.epoch);
  Check(!delta.accepted, "oldest cumulative dropped");

  // Now fresh motion past C arrives: only the increment beyond C counts.
  encoder.AddRelative(0.5, 0.5);
  const std::size_t dsize = encoder.EncodeRelative(buffer.data(), 4);
  Check(Decode(buffer.data(), dsize, decoded), "decode D");
  delta = decoder.ConsumeRelative(decoded.seq, decoded.cum_dx, decoded.cum_dy,
                                  decoded.epoch);
  Check(delta.accepted, "fresh cumulative accepted");
  Check(std::abs(delta.dx - 0.5) < 2e-5 && std::abs(delta.dy - 0.5) < 2e-5,
        "fresh cumulative injects only the new increment");
}

void TestEpochReset() {
  Encoder encoder;
  crossdesk::input_wire::Decoder decoder;
  std::array<char, 64> buffer{};

  encoder.AddRelative(0.5, 0.5);  // pre-reset motion
  encoder.EncodeRelative(buffer.data(), 1);
  Decoded decoded{};
  Decode(buffer.data(), kHeaderBytes, decoded);
  auto delta = decoder.ConsumeRelative(decoded.seq, decoded.cum_dx,
                                       decoded.cum_dy, decoded.epoch);
  Check(delta.accepted, "pre-reset accepted");

  // Session reconnects: fresh encoder restarts at zero with a bumped epoch.
  encoder.ResetRelative();
  encoder.AddRelative(0.02, 0.02);
  const std::size_t size = encoder.EncodeRelative(buffer.data(), 2);
  Decode(buffer.data(), size, decoded);
  delta = decoder.ConsumeRelative(decoded.seq, decoded.cum_dx, decoded.cum_dy,
                                  decoded.epoch);
  Check(delta.accepted, "post-reset accepted");
  // Must inject only the post-reset motion, never the pre-reset 0.5/0.5.
  Check(std::abs(delta.dx - 0.02) < 2e-5 && std::abs(delta.dy - 0.02) < 2e-5,
        "epoch reset never re-applies old motion");
}

void TestFirstContactClamp() {
  // A corrupt or hostile first frame must not teleport the cursor: the
  // first-contact injection is clamped to kCumulativeResetClamp wire units.
  Encoder encoder;
  crossdesk::input_wire::Decoder decoder;
  std::array<char, 64> buffer{};

  encoder.AddRelative(1000, -1000);  // far beyond the clamp
  const std::size_t size = encoder.EncodeRelative(buffer.data(), 1);
  Decoded decoded{};
  Decode(buffer.data(), size, decoded);
  const auto delta = decoder.ConsumeRelative(decoded.seq, decoded.cum_dx,
                                             decoded.cum_dy, decoded.epoch);
  Check(delta.accepted, "clamped first contact accepted");
  const double clamp_units =
      static_cast<double>(crossdesk::input_wire::kCumulativeResetClamp) /
      crossdesk::input_wire::kCumulativeScale;
  Check(std::abs(delta.dx - clamp_units) < 1e-9,
        "first contact dx clamped positive");
  Check(std::abs(delta.dy + clamp_units) < 1e-9,
        "first contact dy clamped negative");
}

void TestClickBatchBoundary() {
  // Moonlight-style batch boundary: relative motion rides the unreliable
  // stream while clicks ride the reliable one. When the motion packet just
  // before a click is lost, the click's embedded cumulative counters must
  // flush exactly the missing displacement — never less (click lands behind
  // the flick), never more (double-applied motion).
  Encoder encoder;
  crossdesk::input_wire::Decoder decoder;
  std::array<char, 64> buffer{};

  double applied_dx = 0, applied_dy = 0;
  double click_position_dx = -1, click_position_dy = -1;

  // Motion 1: +0.02, delivered.
  encoder.AddRelative(0.02, 0.01);
  std::size_t size = encoder.EncodeRelative(buffer.data(), 1);
  Decoded decoded{};
  Check(Decode(buffer.data(), size, decoded), "motion 1 decodes");
  auto delta = decoder.ConsumeRelative(decoded.seq, decoded.cum_dx,
                                       decoded.cum_dy, decoded.epoch);
  Check(delta.accepted, "motion 1 accepted");
  applied_dx += delta.dx;
  applied_dy += delta.dy;

  // Motion 2: +0.03 more, LOST in transit (never reaches the decoder).
  encoder.AddRelative(0.03, 0.02);
  encoder.EncodeRelative(buffer.data(), 2);  // dropped

  // The click encodes cumulative counters at click time (0.05, 0.03 total).
  size = encoder.EncodeMouseButtonCumulative(
      buffer.data(), crossdesk::MouseFlag::left_down, 0, 3);
  Check(size == kHeaderBytes, "cumulative click encode size");
  Check(Decode(buffer.data(), size, decoded), "cumulative click decodes");
  Check(decoded.kind == Kind::mouse_button, "click kind");
  Check(decoded.button_has_cumulative, "click carries cumulative");
  Check(decoded.action.m.flag == crossdesk::MouseFlag::left_down,
        "click flag preserved");

  // The receiver flushes before injecting the click.
  const auto flush = decoder.FlushRelativeForButton(
      decoded.seq, decoded.cum_dx, decoded.cum_dy, decoded.epoch);
  Check(flush.accepted, "click flush accepted");
  // Exactly the lost 0.03/0.02 is recovered — no more, no less.
  Check(std::abs(flush.dx - 0.03) < 2e-5 && std::abs(flush.dy - 0.02) < 2e-5,
        "flush recovers exactly the lost motion");
  applied_dx += flush.dx;
  applied_dy += flush.dy;
  click_position_dx = applied_dx;
  click_position_dy = applied_dy;

  // Ground truth total motion = 0.05/0.03; the click must land exactly there.
  Check(std::abs(click_position_dx - 0.05) < 2e-5 &&
            std::abs(click_position_dy - 0.03) < 2e-5,
        "click lands at the exact end of the flick");

  // The lost motion packet arriving late (after the click) must be dropped:
  // its displacement was already applied by the flush. Re-feed its counters.
  const auto late = decoder.ConsumeRelative(2, decoded.cum_dx, decoded.cum_dy,
                                            decoded.epoch);
  Check(!late.accepted, "late pre-click motion dropped after flush");

  // Post-click motion still applies normally.
  encoder.AddRelative(0.01, 0);
  size = encoder.EncodeRelative(buffer.data(), 4);
  Check(Decode(buffer.data(), size, decoded), "post-click motion decodes");
  delta = decoder.ConsumeRelative(decoded.seq, decoded.cum_dx, decoded.cum_dy,
                                  decoded.epoch);
  Check(delta.accepted, "post-click motion accepted");
  Check(std::abs(delta.dx - 0.01) < 2e-5 && std::abs(delta.dy) < 2e-5,
        "post-click motion is exactly the new increment");

  // Absolute-mode click round-trips its own coordinates (no cumulative).
  size = encoder.EncodeMouseButton(buffer.data(), 0.25f, 0.75f,
                                   crossdesk::MouseFlag::left_down, 0, 5);
  Check(Decode(buffer.data(), size, decoded), "absolute click decodes");
  Check(!decoded.button_has_cumulative, "absolute click has no cumulative");
  Check(decoded.action.m.x == 0.25f && decoded.action.m.y == 0.75f,
        "absolute click positions at its own coordinates");
}

// ---------------------------------------------------------------------------
// Robustness
// ---------------------------------------------------------------------------

void TestMalformedRejection() {
  Encoder encoder;
  std::array<char, 64> buffer{};
  const std::size_t size = encoder.EncodeMoveAbs(buffer.data(), 0.5f, 0.5f, 1);

  Decoded decoded{};
  // Truncated.
  Check(!Decode(buffer.data(), size - 1, decoded), "truncated rejected");
  Check(!Decode(buffer.data(), 4, decoded), "tiny rejected");
  Check(!Decode(nullptr, size, decoded), "null rejected");
  // Wrong magic.
  std::array<char, 64> copy = buffer;
  copy[0] = 'G';
  Check(!Decode(copy.data(), size, decoded), "wrong magic rejected");
  // JSON must never parse as binary.
  const std::string json = "{\"type\":0,\"mouse\":{\"x\":0.5}}";
  Check(!Decode(json.data(), json.size(), decoded), "json rejected as binary");
  Check(!crossdesk::input_wire::IsBinaryFrame(json), "json not binary frame");
  // Binary must never parse as JSON.
  Check(!nlohmann::json::parse(buffer.data(), buffer.data() + size, nullptr,
                               false)
             .is_object(),
        "binary not valid json");
  // Wrong version.
  copy = buffer;
  copy[4] = 2;
  Check(!Decode(copy.data(), size, decoded), "wrong version rejected");
  // Invalid kind.
  copy = buffer;
  copy[5] = 99;
  Check(!Decode(copy.data(), size, decoded), "invalid kind rejected");
  // Non-finite coordinates.
  copy = buffer;
  const float nan = std::numeric_limits<float>::quiet_NaN();
  std::memcpy(copy.data() + 20, &nan, 4);
  Check(!Decode(copy.data(), size, decoded), "nan coordinate rejected");
  // mouse_button smuggling a move flag must fail.
  std::array<char, 64> btn{};
  const std::size_t bsize = encoder.EncodeMouseButton(
      btn.data(), 0.5f, 0.5f, crossdesk::MouseFlag::left_down, 0, 2);
  btn[20 + 8] = static_cast<char>(crossdesk::MouseFlag::move);
  Check(!Decode(btn.data(), bsize, decoded), "button smuggling move rejected");
  // key_state length mismatch.
  std::array<char, crossdesk::input_wire::kMaxPacketBytes> ks{};
  crossdesk::KeyboardState state{};
  state.pressed_count = 2;
  const std::size_t ksize = encoder.EncodeKeyState(ks.data(), state, 3);
  Check(!Decode(ks.data(), ksize - 1, decoded), "key_state truncated rejected");
  // Corrupt count byte beyond the wire maximum.
  ks[20 + 4] = static_cast<char>(crossdesk::kMaxKeyboardStateKeys + 1);
  Check(!Decode(ks.data(), ksize, decoded), "key_state overcount rejected");
}

void TestBinaryFrameDetection() {
  Encoder encoder;
  std::array<char, 64> buffer{};
  encoder.EncodeMoveAbs(buffer.data(), 0.5f, 0.5f, 1);
  Check(crossdesk::input_wire::IsBinaryFrame(buffer.data(), kHeaderBytes),
        "binary frame detected");
  Check(buffer[0] == 'F' && 'F' != '{', "magic distinguishes from json");
  // A JSON payload starting with 'F' but not FIN1 must not route as binary.
  const std::string almost = "FIN2xxxxxxxxxxxxxxxxxxxxxxxxxxxx";
  Check(!crossdesk::input_wire::IsBinaryFrame(almost), "FIN2 not FIN1");
  // Too-short frames never match.
  Check(!crossdesk::input_wire::IsBinaryFrame("FIN", 3), "short not binary");
}

// ---------------------------------------------------------------------------
// Capability negotiation
// ---------------------------------------------------------------------------

void TestCapabilityRoundTrip() {
  crossdesk::RemoteAction action{};
  action.type = crossdesk::ControlType::host_infomation;
  action.i.supports_binary_input = true;
  // Keep the rest of HostInfo minimally valid.
  action.i.host_name[0] = 'h';
  action.i.host_name[1] = '\0';
  action.i.host_name_size = 1;
  action.i.display_num = 0;
  action.i.display_list = nullptr;
  action.i.left = action.i.top = action.i.right = action.i.bottom = nullptr;
  const std::string json = action.to_json();
  Check(json.find("supports_binary_input") != std::string::npos,
        "capability serialized");
  crossdesk::RemoteAction parsed{};
  Check(parsed.from_json(json), "capability host_info parses");
  Check(parsed.i.supports_binary_input, "capability survives round-trip");
  if (parsed.type == crossdesk::ControlType::host_infomation)
    crossdesk::FreeRemoteAction(parsed);

  // Absent in legacy JSON: defaults false.
  const std::string legacy =
      "{\"type\":3,\"host_info\":{\"host_name\":\"x\",\"display_num\":0,"
      "\"displays\":[]}}";
  crossdesk::RemoteAction legacy_parsed{};
  Check(legacy_parsed.from_json(legacy), "legacy host_info parses");
  Check(!legacy_parsed.i.supports_binary_input,
        "legacy defaults binary_input false");
  if (legacy_parsed.type == crossdesk::ControlType::host_infomation)
    crossdesk::FreeRemoteAction(legacy_parsed);
}

// ---------------------------------------------------------------------------
// Microbenchmark: JSON vs binary encode+decode
// ---------------------------------------------------------------------------

void Benchmark() {
  constexpr int kIterations = 100000;

  // JSON path: serialize + parse a representative mouse move.
  crossdesk::RemoteAction json_action{};
  json_action.type = crossdesk::ControlType::mouse;
  json_action.m.x = 0.5f;
  json_action.m.y = 0.5f;
  json_action.m.s = 0;
  json_action.m.flag = crossdesk::MouseFlag::move;

  const auto json_start = std::chrono::steady_clock::now();
  std::size_t json_bytes = 0;
  for (int i = 0; i < kIterations; ++i) {
    const std::string wire = json_action.to_json();
    json_bytes += wire.size();
    crossdesk::RemoteAction parsed{};
    if (!parsed.from_json(wire)) {
      std::cerr << "json parse failed in benchmark\n";
      ++failures;
      break;
    }
  }
  const auto json_end = std::chrono::steady_clock::now();
  const double json_ns =
      std::chrono::duration<double, std::nano>(json_end - json_start).count() /
      kIterations;

  // Binary path: encode + decode the equivalent move.
  Encoder encoder;
  crossdesk::input_wire::Decoder decoder;
  std::array<char, 64> buffer{};
  const auto bin_start = std::chrono::steady_clock::now();
  std::size_t bin_bytes = 0;
  for (int i = 0; i < kIterations; ++i) {
    const std::size_t size =
        encoder.EncodeMoveAbs(buffer.data(), 0.5f, 0.5f, 12345);
    bin_bytes += size;
    Decoded decoded{};
    if (!Decode(buffer.data(), size, decoded)) {
      std::cerr << "binary decode failed in benchmark\n";
      ++failures;
      break;
    }
    if (!decoder.AcceptMoveAbs(decoded.seq)) {
      // Not an error; just consume the gate.
    }
  }
  const auto bin_end = std::chrono::steady_clock::now();
  const double bin_ns =
      std::chrono::duration<double, std::nano>(bin_end - bin_start).count() /
      kIterations;

  std::cout << "  benchmark (" << kIterations << " iterations):\n"
            << "    JSON encode+decode:   " << json_ns << " ns/op, "
            << (json_bytes / kIterations) << " bytes/frame\n"
            << "    binary encode+decode: " << bin_ns << " ns/op, "
            << (bin_bytes / kIterations) << " bytes/frame\n"
            << "    speedup: " << (json_ns / bin_ns) << "x\n";
  Check(bin_ns < json_ns, "binary faster than json");
  Check(bin_bytes / kIterations == kHeaderBytes, "binary fixed frame size");
}

}  // namespace

int main() {
  TestMoveAbsRoundTrip();
  TestMoveRelCumulativeRoundTrip();
  TestMouseButtonRoundTrip();
  TestScrollPixelRoundTrip();
  TestKeyRoundTrip();
  TestKeyStateRoundTrip();
  TestMoveAbsOutOfOrder();
  TestMoveRelLossRecovery();
  TestMoveRelOutOfOrderRecovery();
  TestEpochReset();
  TestFirstContactClamp();
  TestClickBatchBoundary();
  TestMalformedRejection();
  TestBinaryFrameDetection();
  TestCapabilityRoundTrip();
  Benchmark();

  if (failures == 0) {
    std::cout << "input_wire_test: all tests passed\n";
    return 0;
  }
  std::cerr << "input_wire_test: " << failures << " failures\n";
  return 1;
}
