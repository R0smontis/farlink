/*
 * Port-forward wire protocol unit tests: exact binary round trip, malformed
 * input rejection, backward-compatible capability negotiation.
 *
 * Copyright (c) 2026 Farlink contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include <port_forward_wire.h>
#include <remote_action.h>
#include <stream_names.h>

namespace {

int failures = 0;
bool Expect(bool condition, const char* message) {
  if (condition) return true;
  std::cerr << "FAIL: " << message << '\n';
  ++failures;
  return false;
}

using crossdesk::port_forward::DecodeFrame;
using crossdesk::port_forward::EncodeFrame;
using crossdesk::port_forward::Frame;
using crossdesk::port_forward::Kind;

std::string Encode(const Frame& frame) {
  std::string out(crossdesk::port_forward::kHeaderBytes +
                      crossdesk::port_forward::kMaxPayloadBytes,
                  '\0');
  const std::size_t size = EncodeFrame(frame, out.data(), out.size());
  out.resize(size);
  return out;
}

}  // namespace

int main() {
  using namespace crossdesk;
  using namespace crossdesk::port_forward;
  bool ok = true;

  ok &= Expect(std::strlen(kPortForwardStream) == strlen("port_forward"),
               "dedicated reliable stream name is registered");

  // ---- Exact little-endian framing ---------------------------------------
  {
    const Frame open{Kind::kOpen, 2222, 0x01020304, {}};
    const auto encoded = Encode(open);
    ok &= Expect(encoded.size() == kHeaderBytes, "open frame is header only");
    ok &= Expect(std::memcmp(encoded.data(), "FPF1", 4) == 0,
                 "magic appears literally as FPF1");
    ok &= Expect(static_cast<unsigned char>(encoded[4]) == 1 &&
                     encoded[5] == 0,
                 "kind little-endian");
    ok &= Expect(static_cast<unsigned char>(encoded[6]) == 2222 % 256 &&
                     static_cast<unsigned char>(encoded[7]) == 2222 / 256,
                 "remote port little-endian");
    ok &= Expect(static_cast<unsigned char>(encoded[8]) == 4 &&
                     static_cast<unsigned char>(encoded[9]) == 3 &&
                     static_cast<unsigned char>(encoded[10]) == 2 &&
                     static_cast<unsigned char>(encoded[11]) == 1,
                 "stream id little-endian");
    ok &= Expect(encoded[12] == 0 && encoded[15] == 0,
                 "zero payload length little-endian");
  }

  // ---- Round-trip every kind ----------------------------------------------
  for (const Frame sample : {
           Frame{Kind::kOpen, 22, 1, {}},
           Frame{Kind::kOpenOk, 0, 2, {}},
           Frame{Kind::kOpenErr, kErrorDenied, 3, "denied"},
           Frame{Kind::kData, 0, 4, "abc\x00xyz"},
           Frame{Kind::kEof, 0, 5, {}},
           Frame{Kind::kClose, 0, 6, {}},
           Frame{Kind::kWindow, 0, 7, "ABCD"},
       }) {
    const auto encoded = Encode(sample);
    ok &= Expect(encoded.size() == kHeaderBytes + sample.payload.size(),
                 "encoded size is exact");
    Frame decoded;
    ok &= Expect(DecodeFrame(encoded.data(), encoded.size(), decoded),
                 "round-trip frame decodes");
    ok &= Expect(decoded.kind == sample.kind && decoded.flags == sample.flags &&
                     decoded.stream_id == sample.stream_id &&
                     decoded.payload == sample.payload,
                 "round-trip frame fields match");
    ok &= Expect(decoded.payload.data() == encoded.data() + kHeaderBytes ||
                     sample.payload.empty(),
                 "decoded payload borrows the input buffer");
  }

  // ---- Maximum payload boundary -------------------------------------------
  {
    const std::string maximum_payload(kMaxPayloadBytes, 'x');
    Frame data{Kind::kData, 0, 77, maximum_payload};
    auto encoded = Encode(data);
    ok &= Expect(encoded.size() == kHeaderBytes + kMaxPayloadBytes,
                 "32 KiB data frame is accepted");
    Frame decoded;
    ok &= Expect(DecodeFrame(encoded.data(), encoded.size(), decoded) &&
                     decoded.payload.size() == kMaxPayloadBytes,
                 "32 KiB data frame round-trips");
    char small[kHeaderBytes + 8];
    ok &= Expect(EncodeFrame(data, small, sizeof(small)) == 0,
                 "insufficient destination capacity is rejected");
  }

  // ---- Malformed and hostile frames ----------------------------------------
  {
    Frame valid{Kind::kData, 0, 9, "payload"};
    auto encoded = Encode(valid);
    const auto reject = [&](std::string mutated, const char* message) {
      Frame decoded;
      ok &= Expect(!DecodeFrame(mutated.data(), mutated.size(), decoded), message);
    };
    reject(encoded.substr(0, kHeaderBytes - 1), "truncated header rejected");
    reject(std::string(kHeaderBytes + kMaxPayloadBytes + 1, '\0'),
           "oversized total frame rejected");
    auto bad_magic = encoded;
    bad_magic[0] = 'X';
    reject(bad_magic, "bad magic rejected");
    auto bad_kind_zero = encoded;
    bad_kind_zero[4] = 0;
    reject(bad_kind_zero, "kind zero rejected");
    auto bad_kind_high = encoded;
    bad_kind_high[4] = 7;
    reject(bad_kind_high, "unknown kind rejected");
    auto bad_stream = encoded;
    for (int i = 8; i < 12; ++i) bad_stream[i] = 0;
    reject(bad_stream, "zero stream id rejected");
    auto bad_length = encoded;
    bad_length[12] = static_cast<char>(valid.payload.size() + 1);
    reject(bad_length, "payload length mismatch rejected");
    auto bad_flags = encoded;
    bad_flags[6] = 1;
    reject(bad_flags, "nonzero flags on data rejected");
    Frame empty_data{Kind::kData, 0, 9, {}};
    ok &= Expect(EncodeFrame(empty_data, encoded.data(), encoded.size()) == 0,
                 "empty data payload cannot encode");
    Frame open_zero{Kind::kOpen, 0, 9, {}};
    ok &= Expect(EncodeFrame(open_zero, encoded.data(), encoded.size()) == 0,
                 "zero destination port cannot encode");
    Frame open_payload{Kind::kOpen, 22, 9, "unexpected"};
    ok &= Expect(EncodeFrame(open_payload, encoded.data(), encoded.size()) == 0,
                 "open payload cannot encode");
    Frame eof_payload{Kind::kEof, 0, 9, "unexpected"};
    ok &= Expect(EncodeFrame(eof_payload, encoded.data(), encoded.size()) == 0,
                 "eof payload cannot encode");
    Frame window_short{Kind::kWindow, 0, 9, "abc"};
    ok &= Expect(EncodeFrame(window_short, encoded.data(), encoded.size()) == 0,
                 "short window credit payload cannot encode");
    Frame window_flags{Kind::kWindow, 4, 9, "abcd"};
    ok &= Expect(EncodeFrame(window_flags, encoded.data(), encoded.size()) == 0,
                 "window frame with nonzero flags cannot encode");
    // Credit values round-trip through the helpers.
    char credit_payload[4];
    WriteCredit(credit_payload, 262144);
    ok &= Expect(ReadCredit(std::string_view(credit_payload, 4)) == 262144,
                 "window credit round-trips");
    Frame bad_error{Kind::kOpenErr, 99, 9, "bad"};
    ok &= Expect(EncodeFrame(bad_error, encoded.data(), encoded.size()) == 0,
                 "unknown open error code cannot encode");
    const std::string nul_reason("a\0b", 3);
    Frame bad_reason{Kind::kOpenErr, kErrorDenied, 9, nul_reason};
    ok &= Expect(EncodeFrame(bad_reason, encoded.data(), encoded.size()) == 0,
                 "embedded NUL reason cannot encode");
  }

  // ---- Valid UTF-8 reasons --------------------------------------------------
  {
    Frame unicode{Kind::kOpenErr, kErrorDenied, 9, "拒绝: port 22"};
    const auto encoded = Encode(unicode);
    Frame decoded;
    ok &= Expect(!encoded.empty() &&
                     DecodeFrame(encoded.data(), encoded.size(), decoded) &&
                     decoded.payload == unicode.payload,
                 "UTF-8 denial reason round-trips");
    const std::string truncated_reason = std::string("ok\xc3\xa9").substr(0, 3);
    Frame truncated{Kind::kOpenErr, kErrorDenied, 9, truncated_reason};
    char buffer[128];
    ok &= Expect(EncodeFrame(truncated, buffer, sizeof(buffer)) == 0,
                 "truncated UTF-8 reason cannot encode");
  }

  // ---- Capability flag round-trip -------------------------------------------
  {
    auto action = MakeHostInformation("host", {}, false, "1.0.0",
                                      HostPlatform::MacOS);
    action.i.supports_port_forward = true;
    const auto encoded = action.to_json();
    FreeRemoteAction(action);
    RemoteAction decoded{};
    ok &= Expect(decoded.from_json(encoded), "host information parses");
    ok &= Expect(decoded.i.supports_port_forward,
                 "supports_port_forward true round-trips");
    FreeRemoteAction(decoded);

    auto legacy = MakeHostInformation("host", {}, false, "1.0.0",
                                      HostPlatform::MacOS);
    const auto encoded_false = legacy.to_json();
    FreeRemoteAction(legacy);
    RemoteAction decoded_false{};
    ok &= Expect(decoded_false.from_json(encoded_false) &&
                     !decoded_false.i.supports_port_forward,
                 "explicit false remains false");
    FreeRemoteAction(decoded_false);

    // Simulate a legacy peer whose host information predates the
    // capability: the key is absent entirely and must default to false.
    const std::string minimal =
        R"({"type":3,"host_info":{"host_name":"h","display_num":0,"displays":[]}})";
    RemoteAction legacy_decoded{};
    ok &= Expect(legacy_decoded.from_json(minimal) &&
                     !legacy_decoded.i.supports_port_forward,
                 "legacy host information without the key defaults false");
    FreeRemoteAction(legacy_decoded);
  }

  if (ok) {
    std::cout << "port_forward_wire_test: PASS\n";
    return 0;
  }
  std::cerr << "port_forward_wire_test: " << failures << " failure(s)\n";
  return 1;
}
