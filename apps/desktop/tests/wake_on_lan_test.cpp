#include <array>
#include <cstring>
#include <iostream>
#include <string>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "network/lan/wake_on_lan.h"

namespace {

using crossdesk::lan::MacAddress;

int failures = 0;
bool Expect(bool condition, const char* message) {
  if (condition) return true;
  std::cerr << "FAIL: " << message << '\n';
  ++failures;
  return false;
}

// Inert documentation-range fixture (RFC 7042 / documentation prefix style):
// locally administered, unicast, never a real station on any LAN here. The
// loopback injection test below is the only send performed in this process.
constexpr MacAddress kFixture{0x02, 0x00, 0x5e, 0x10, 0x20, 0x30};

void ParseAcceptsAllFormats() {
  const auto colon = crossdesk::lan::ParseMacAddress("02:00:5E:10:20:30");
  Expect(colon && *colon == kFixture, "colon format parses");
  const auto dash = crossdesk::lan::ParseMacAddress("02-00-5e-10-20-30");
  Expect(dash && *dash == kFixture, "dash format parses case-insensitively");
  const auto dotted = crossdesk::lan::ParseMacAddress("0200.5e10.2030");
  Expect(dotted && *dotted == kFixture, "dotted triplet format parses");
  const auto plain = crossdesk::lan::ParseMacAddress("02005E102030");
  Expect(plain && *plain == kFixture, "plain hex format parses");
}

void ParseRejectsInvalid() {
  Expect(!crossdesk::lan::ParseMacAddress("FF:FF:FF:FF:FF:FF"),
         "broadcast address rejected");
  Expect(!crossdesk::lan::ParseMacAddress("ff-ff-ff-ff-ff-ff"),
         "lowercase broadcast rejected");
  Expect(!crossdesk::lan::ParseMacAddress("00:00:00:00:00:00"),
         "all-zero address rejected");
  Expect(!crossdesk::lan::ParseMacAddress("ffffffffffff"),
         "plain broadcast rejected");
  Expect(!crossdesk::lan::ParseMacAddress("000000000000"),
         "plain all-zero rejected");
  Expect(!crossdesk::lan::ParseMacAddress(""), "empty rejected");
  Expect(!crossdesk::lan::ParseMacAddress("02:00:5E:10:20"),
         "short colon form rejected");
  Expect(!crossdesk::lan::ParseMacAddress("02:00:5E:10:20:30:40"),
         "overlong colon form rejected");
  Expect(!crossdesk::lan::ParseMacAddress("02:00:5E:10:20:3G"),
         "non-hex digit rejected");
  Expect(!crossdesk::lan::ParseMacAddress("0200.5e10.203"),
         "short dotted form rejected");
  Expect(!crossdesk::lan::ParseMacAddress("02005e10203"),
         "11-char plain form rejected");
  Expect(!crossdesk::lan::ParseMacAddress("02/00/5E/10/20/30"),
         "wrong separator rejected");
  Expect(!crossdesk::lan::ParseMacAddress(" 02:00:5E:10:20:30"),
         "leading whitespace rejected");
}

void FormatIsCanonical() {
  Expect(crossdesk::lan::FormatMacAddress(kFixture) == "02:00:5E:10:20:30",
         "canonical uppercase colon formatting");
  const auto roundtrip = crossdesk::lan::ParseMacAddress(
      crossdesk::lan::FormatMacAddress(kFixture));
  Expect(roundtrip && *roundtrip == kFixture, "format/parse round trip");
}

void MagicPacketLayout() {
  const auto packet = crossdesk::lan::BuildMagicPacket(kFixture);
  Expect(packet.size() == 102, "packet is exactly 102 bytes");
  bool sync = true;
  for (size_t i = 0; i < 6; ++i) sync = sync && packet[i] == 0xff;
  Expect(sync, "6-byte FF synchronization stream");
  bool repetitions = true;
  for (size_t rep = 0; rep < 16; ++rep)
    repetitions =
        repetitions &&
        std::memcmp(packet.data() + 6 + rep * 6, kFixture.data(), 6) == 0;
  Expect(repetitions, "16 exact MAC repetitions");
}

void PrimaryMacIsReal() {
  const auto mac = crossdesk::lan::GetPrimaryLocalMacAddress();
  Expect(mac.has_value(), "primary local MAC detected on this machine");
  if (!mac) return;
  bool nonzero = false;
  for (const auto byte : *mac) nonzero = nonzero || byte != 0;
  Expect(nonzero, "primary MAC is not all-zero");
  Expect(!((*mac)[0] & 0x01), "primary MAC is unicast (not multicast)");
  Expect(crossdesk::lan::ParseMacAddress(
             crossdesk::lan::FormatMacAddress(*mac))
             .has_value(),
         "primary MAC survives canonical formatting");
}

// The ONLY network send in this suite: injected loopback endpoint, inert
// fixture MAC. No broadcast address is ever used here, so nothing reaches a
// physical interface or another machine.
void LoopbackUdpRoundTrip() {
  const int receiver = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  Expect(receiver >= 0, "receiver socket created");
  if (receiver < 0) return;
  sockaddr_in bind_address{};
  bind_address.sin_family = AF_INET;
  bind_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  bind_address.sin_port = 0;
  Expect(bind(receiver, reinterpret_cast<const sockaddr*>(&bind_address),
              sizeof(bind_address)) == 0,
         "receiver bound to 127.0.0.1:0");
  sockaddr_in bound{};
  socklen_t length = sizeof(bound);
  Expect(getsockname(receiver, reinterpret_cast<sockaddr*>(&bound),
                     &length) == 0,
         "ephemeral port discovered");
  const uint16_t port = ntohs(bound.sin_port);
  Expect(port != 0, "bound port is nonzero");

  std::string error;
  Expect(crossdesk::lan::SendMagicPacket(kFixture, "127.0.0.1", port, &error),
         "injected loopback send succeeds");
  if (!error.empty())
    std::cerr << "send error detail: " << error << '\n';

  timeval timeout{};
  timeout.tv_sec = 2;
  setsockopt(receiver, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  std::array<uint8_t, 256> buffer{};
  const ssize_t received =
      recvfrom(receiver, buffer.data(), buffer.size(), 0, nullptr, nullptr);
  Expect(received == 102, "exactly 102 bytes received");
  if (received == 102) {
    const auto expected = crossdesk::lan::BuildMagicPacket(kFixture);
    Expect(std::memcmp(buffer.data(), expected.data(), 102) == 0,
           "received bytes match magic packet byte-for-byte");
  }
  close(receiver);
}

void InvalidInputsFailClosed() {
  std::string error;
  const MacAddress broadcast{0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
  Expect(!crossdesk::lan::SendMagicPacket(broadcast, "127.0.0.1", 9, &error),
         "broadcast target refused without touching network");
  const MacAddress zero{};
  Expect(!crossdesk::lan::SendMagicPacket(zero, "127.0.0.1", 9, &error),
         "all-zero target refused without touching network");
  Expect(!crossdesk::lan::SendMagicPacket(kFixture, "not-an-ip", 9, &error),
         "malformed broadcast endpoint rejected");
}

}  // namespace

int main() {
  ParseAcceptsAllFormats();
  ParseRejectsInvalid();
  FormatIsCanonical();
  MagicPacketLayout();
  PrimaryMacIsReal();
  LoopbackUdpRoundTrip();
  InvalidInputsFailClosed();
  if (failures) {
    std::cerr << failures << " failure(s)\n";
    return 1;
  }
  std::cout << "wake_on_lan_test: all assertions passed\n";
  return 0;
}
