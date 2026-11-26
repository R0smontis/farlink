#include <array>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>

#include "network/lan/lan_discovery.h"

namespace {

using crossdesk::lan::Beacon;
using crossdesk::lan::Discovery;
using crossdesk::lan::kBeaconSize;

int failures = 0;
bool Expect(bool condition, const char* message) {
  if (condition) return true;
  std::cerr << "FAIL: " << message << '\n';
  ++failures;
  return false;
}

Beacon SampleBeacon() {
  Beacon beacon;
  beacon.flags = crossdesk::lan::kBeaconFlagListenAll |
                 crossdesk::lan::kBeaconFlagPasswordSet;
  beacon.tcp_port = 52888;
  beacon.device_id = "0123456789abcde";  // 15 chars: exact wire capacity
  beacon.hostname = "studio-pc";
  beacon.ipv4 = {192, 168, 1, 20};
  return beacon;
}

void PackUnpackRoundTrip() {
  std::array<uint8_t, kBeaconSize> wire;
  const auto beacon = SampleBeacon();
  Expect(crossdesk::lan::PackBeacon(beacon, wire), "pack valid beacon");
  Expect(wire.size() == 64, "wire size is 64 bytes");
  Expect(memcmp(wire.data(), "FLN1", 4) == 0, "magic on wire");
  Expect(wire[4] == 1, "version byte is 1");
  Expect(wire[6] == 52888 % 256 && wire[7] == 52888 / 256,
         "tcp_port little-endian");
  Expect(wire[8 + 15] == 0, "device id padded to 16 bytes");
  Expect(wire[56] == 192 && wire[59] == 20, "ipv4 in network order");
  Expect(wire[60] == 0 && wire[63] == 0, "reserved bytes zero");

  Beacon decoded;
  Expect(crossdesk::lan::UnpackBeacon(wire.data(), wire.size(), decoded),
         "unpack valid beacon");
  Expect(decoded.tcp_port == 52888, "round-trip tcp_port");
  Expect(decoded.device_id == beacon.device_id, "round-trip device_id");
  Expect(decoded.hostname == beacon.hostname, "round-trip hostname");
  Expect(decoded.ipv4 == beacon.ipv4, "round-trip ipv4");
  Expect(decoded.flags == beacon.flags, "round-trip flags");
}

void FullLengthDeviceIdRoundTrip() {
  // A 16-character hex prefix fills the device_id field exactly: no NUL
  // terminator is required and the full prefix must survive the round trip.
  auto beacon = SampleBeacon();
  beacon.device_id = "0123456789abcdef";  // 16 chars: entire field, no NUL
  std::array<uint8_t, kBeaconSize> wire;
  Expect(crossdesk::lan::PackBeacon(beacon, wire), "pack 16-char device id");
  Beacon decoded;
  Expect(crossdesk::lan::UnpackBeacon(wire.data(), wire.size(), decoded),
         "unpack 16-char device id");
  Expect(decoded.device_id == "0123456789abcdef",
         "full 16-char device id round-trips without truncation");
  // A 32-char identity id is truncated to its 16-char prefix on the wire.
  beacon.device_id = "0123456789abcdef0123456789abcdef";
  crossdesk::lan::PackBeacon(beacon, wire);
  Expect(crossdesk::lan::UnpackBeacon(wire.data(), wire.size(), decoded) &&
             decoded.device_id == "0123456789abcdef",
         "long identity id truncated to 16-char prefix");
}

void DeviceIdPaddingValidation() {
  auto beacon = SampleBeacon();
  std::array<uint8_t, kBeaconSize> wire;
  crossdesk::lan::PackBeacon(beacon, wire);
  Beacon decoded;
  // Non-zero garbage after the first NUL inside the padding is rejected:
  // accepting it would let a corrupted length resurface stale bytes.
  auto copy = wire;
  copy[8 + 4] = 0;      // truncate the visible id at 4 characters
  copy[8 + 10] = 'x';   // garbage inside what must be zero padding
  Expect(!crossdesk::lan::UnpackBeacon(copy.data(), kBeaconSize, decoded),
         "non-zero device id padding rejected");
  copy = wire;
  copy[8] = 0;  // empty visible prefix
  Expect(!crossdesk::lan::UnpackBeacon(copy.data(), kBeaconSize, decoded),
         "empty device id rejected");
}

void RejectMalformed() {
  std::array<uint8_t, kBeaconSize> wire;
  const auto beacon = SampleBeacon();
  crossdesk::lan::PackBeacon(beacon, wire);
  Beacon decoded;

  // Truncated input.
  for (size_t size : {size_t(0), size_t(4), size_t(32), size_t(63)})
    Expect(!crossdesk::lan::UnpackBeacon(wire.data(), size, decoded),
           "truncated datagram rejected");

  // Oversized datagram: the fixed wire layout tolerates no extra bytes.
  {
    std::array<uint8_t, kBeaconSize + 1> overlong{};
    memcpy(overlong.data(), wire.data(), wire.size());
    Expect(!crossdesk::lan::UnpackBeacon(overlong.data(), overlong.size(),
                                         decoded),
           "oversized datagram rejected");
  }

  auto with_mutation = [&](size_t offset, uint8_t value) {
    auto copy = wire;
    copy[offset] = value;
    return copy;
  };

  Expect(!crossdesk::lan::UnpackBeacon(with_mutation(0, 'X').data(),
                                       kBeaconSize, decoded),
         "bad magic rejected");
  Expect(!crossdesk::lan::UnpackBeacon(with_mutation(4, 2).data(), kBeaconSize,
                                       decoded),
         "bad version rejected");
  {
    // Mutating the low port byte changes the decoded port but stays valid.
    auto copy = wire;
    copy[6] = 0x39;
    copy[7] = 0x30;  // port 12345 little-endian
    Beacon out;
    Expect(crossdesk::lan::UnpackBeacon(copy.data(), kBeaconSize, out) &&
               out.tcp_port == 12345,
           "little-endian port decodes exactly");
  }
  {
    auto copy = wire;
    copy[6] = 0;
    copy[7] = 0;
    Expect(!crossdesk::lan::UnpackBeacon(copy.data(), kBeaconSize, decoded),
           "zero port rejected");
  }
  {
    auto copy = wire;
    copy[60] = 1;
    Expect(!crossdesk::lan::UnpackBeacon(copy.data(), kBeaconSize, decoded),
           "non-zero reserved byte rejected");
  }
  {
    // A hostname with no NUL in its 32-byte field is rejected rather than
    // read out of bounds.
    auto copy = wire;
    memset(copy.data() + 24, 'x', 32);
    Expect(!crossdesk::lan::UnpackBeacon(copy.data(), kBeaconSize, decoded),
           "unterminated hostname rejected");
  }
  {
    // Empty device id is rejected.
    auto copy = wire;
    memset(copy.data() + 8, 0, 16);
    Expect(!crossdesk::lan::UnpackBeacon(copy.data(), kBeaconSize, decoded),
           "empty device id rejected");
  }
  {
    // NUL inside the hostname truncates at the first terminator; padding
    // after it must not resurface.
    Beacon host_nul = SampleBeacon();
    host_nul.hostname = "abc";
    std::array<uint8_t, kBeaconSize> packed;
    crossdesk::lan::PackBeacon(host_nul, packed);
    packed[24 + 3] = 0;
    packed[24 + 4] = 'z';  // content after the first NUL must be ignored
    Beacon out;
    Expect(crossdesk::lan::UnpackBeacon(packed.data(), kBeaconSize, out),
           "NUL-padded hostname accepted");
    Expect(out.hostname == "abc", "hostname stops at first NUL");
  }
  // Port range: maximum valid port round-trips.
  {
    Beacon max_port = SampleBeacon();
    max_port.tcp_port = 65535;
    std::array<uint8_t, kBeaconSize> packed;
    Expect(crossdesk::lan::PackBeacon(max_port, packed), "pack port 65535");
    Beacon out;
    Expect(crossdesk::lan::UnpackBeacon(packed.data(), kBeaconSize, out) &&
               out.tcp_port == 65535,
           "port 65535 round-trips");
  }
  // Pack rejects invalid input.
  {
    std::array<uint8_t, kBeaconSize> packed;
    Beacon invalid = SampleBeacon();
    invalid.tcp_port = 0;
    Expect(!crossdesk::lan::PackBeacon(invalid, packed),
           "pack rejects zero port");
    invalid = SampleBeacon();
    invalid.device_id.clear();
    Expect(!crossdesk::lan::PackBeacon(invalid, packed),
           "pack rejects empty device id");
  }
}

void HostnameTruncation() {
  // 31 ASCII bytes fit exactly; a 32-byte hostname is truncated to 31.
  Beacon beacon = SampleBeacon();
  beacon.hostname = std::string(40, 'h');
  std::array<uint8_t, kBeaconSize> wire;
  crossdesk::lan::PackBeacon(beacon, wire);
  Beacon out;
  Expect(crossdesk::lan::UnpackBeacon(wire.data(), kBeaconSize, out),
         "long hostname unpacks");
  Expect(out.hostname.size() == 31, "hostname truncated to 31 bytes");

  // Multi-byte UTF-8 is never split: 10 CJK characters (30 bytes) plus one
  // ASCII byte fit exactly, but 11 CJK characters (33 bytes) must truncate
  // to 30 bytes rather than split the 11th character.
  const std::string cjk = "\xe4\xb8\xad";  // U+4E2D, 3 bytes
  beacon.hostname.clear();
  for (int i = 0; i < 11; ++i) beacon.hostname += cjk;
  crossdesk::lan::PackBeacon(beacon, wire);
  Expect(crossdesk::lan::UnpackBeacon(wire.data(), kBeaconSize, out),
         "CJK hostname unpacks");
  Expect(out.hostname.size() == 30, "CJK hostname truncated at char boundary");
}

Discovery::Config TestConfig(const std::string& device_id) {
  Discovery::Config config;
  config.udp_port = 0;  // ephemeral
  config.device_id = device_id;
  config.hostname = "host-" + device_id;
  config.password_set = true;
  config.broadcast_interval = std::chrono::milliseconds(80);
  config.peer_ttl = std::chrono::milliseconds(1500);
  return config;
}

// Deterministic in-process peer table verification: both instances bind
// ephemeral ports; each is reconfigured to broadcast at the other's port.
void TwoInstanceExchange() {
  auto config_a = TestConfig("aaaa1111");
  auto config_b = TestConfig("bbbb2222");
  Discovery a(config_a), b(config_b);
  Expect(a.Start(), "instance A starts");
  Expect(b.Start(), "instance B starts");

  // Broadcast at the peer's ephemeral port by rewriting the shared port knob:
  // this exercises the real UDP loopback path with production packing.
  struct PortSwap {
    // Not needed with mutable config; instances are reconfigured in place.
  };
  // Swap target ports: A broadcasts to B's port and vice versa.
  const uint16_t port_a = a.bound_port(), port_b = b.bound_port();
  Expect(port_a != 0 && port_b != 0, "ephemeral ports assigned");

  // Point each instance at the other using a private broadcast override.
  // The production config port doubles as the destination port; binding was
  // ephemeral because the OS received port 0 at bind time only when the
  // socket was created with it. Recreate both instances with the correct
  // fixed destination through the loopback broadcast address instead.
  a.Stop();
  b.Stop();

  // Simplest correct setup: both instances share one fixed high port chosen
  // by probing. Loopback broadcast delivery reaches every socket bound to
  // the same port thanks to SO_REUSEPORT/SO_REUSEADDR.
  Discovery::Config shared_a = TestConfig("aaaa1111");
  Discovery::Config shared_b = TestConfig("bbbb2222");
  shared_a.hostname = "alpha-host";
  shared_b.hostname = "beta-host";

  // Find a free UDP port by binding an ephemeral instance first.
  uint16_t shared_port = 0;
  {
    Discovery probe(TestConfig("probe"));
    probe.Start();
    shared_port = probe.bound_port();
  }
  shared_a.udp_port = shared_port;
  shared_b.udp_port = shared_port;

  Discovery sa(shared_a), sb(shared_b);
  Expect(sa.Start(), "shared instance A starts");
  Expect(sb.Start(), "shared instance B starts");

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
  const auto start = std::chrono::steady_clock::now();
  bool discovered = false, discovered_reverse = false;
  std::chrono::milliseconds latency{0};
  while (std::chrono::steady_clock::now() < deadline) {
    sa.Tick(52888);  // A advertises its listener
    sb.Tick(52900);  // B advertises on a different TCP port
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    sa.Poll();
    sb.Poll();
    for (const auto& peer : sb.Peers())
      if (peer.device_id == "aaaa1111" && !discovered) {
        discovered = true;
        latency = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);
        Expect(peer.hostname == "alpha-host", "peer hostname exact");
        Expect(peer.port == 52888, "peer tcp port exact");
        Expect(!peer.address.empty(), "peer address present");
      }
    for (const auto& peer : sa.Peers())
      if (peer.device_id == "bbbb2222" && !discovered_reverse) {
        discovered_reverse = true;
        Expect(peer.hostname == "beta-host", "reverse hostname exact");
        Expect(peer.port == 52900, "reverse tcp port exact");
      }
    if (discovered && discovered_reverse) break;
  }
  Expect(discovered, "instance B discovers A within deadline");
  Expect(discovered && latency < std::chrono::milliseconds(500),
         "discovery latency below 500 ms");
  Expect(discovered_reverse, "instance A discovers B within deadline");

  // Own beacons never surface as peers.
  for (const auto& peer : sa.Peers())
    Expect(peer.device_id != "aaaa1111", "A never lists itself");
  for (const auto& peer : sb.Peers())
    Expect(peer.device_id != "bbbb2222", "B never lists itself");

  // Privacy: when the listener goes away (tcp_port == 0) the instance stops
  // advertising; the peer expires after TTL.
  const auto wait_start = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() - wait_start <
         std::chrono::milliseconds(2500)) {
    sa.Tick(0);      // A's listener is off: no more beacons
    sb.Tick(52900);  // B keeps listening
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    sa.Poll();
    sb.Poll();
  }
  bool a_still_visible = false;
  for (const auto& peer : sb.Peers())
    if (peer.device_id == "aaaa1111") a_still_visible = true;
  Expect(!a_still_visible, "silent peer expires after TTL");
  bool b_still_visible = false;
  for (const auto& peer : sa.Peers())
    if (peer.device_id == "bbbb2222") b_still_visible = true;
  Expect(b_still_visible, "broadcasting peer remains visible");

  // Re-appearance: A's listener returns, B must see it again promptly.
  bool reappeared = false;
  const auto restart = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() - restart <
         std::chrono::milliseconds(1500)) {
    sa.Tick(52888);
    sb.Tick(52900);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    sa.Poll();
    sb.Poll();
    for (const auto& peer : sb.Peers())
      if (peer.device_id == "aaaa1111") reappeared = true;
    if (reappeared) break;
  }
  Expect(reappeared, "peer reappears when its listener returns");

  // Endpoint update: A rebinds its TCP listener to a different port; the
  // peer entry must update in place rather than duplicate.
  bool updated = false;
  const auto rebind = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() - rebind <
         std::chrono::milliseconds(1500)) {
    sa.Tick(53001);
    sb.Tick(52900);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    sa.Poll();
    sb.Poll();
    for (const auto& peer : sb.Peers())
      if (peer.device_id == "aaaa1111" && peer.port == 53001) updated = true;
    if (updated) break;
  }
  Expect(updated, "peer endpoint updates in place");
  size_t count_a = 0;
  for (const auto& peer : sb.Peers())
    if (peer.device_id == "aaaa1111") ++count_a;
  Expect(count_a == 1, "endpoint update never duplicates the peer");
}

void InjectedClockTtl() {
  // Deterministic TTL expiry with an injected clock: no wall-clock sleeps.
  auto now = std::chrono::steady_clock::now();
  Discovery::Config config = TestConfig("self0000");
  Discovery discovery(config, [&now] { return now; });
  Expect(discovery.Start(), "clocked instance starts");

  // Inject a foreign beacon directly into the socket is complex; instead use
  // a live second instance with the real clock, then freeze/advance the
  // injected clock to verify TTL logic deterministically.
  const uint16_t port = discovery.bound_port();
  Discovery::Config sender_config = TestConfig("peer9999");
  sender_config.udp_port = port;
  Discovery sender(sender_config);
  Expect(sender.Start(), "sender starts");
  sender.Tick(52888);

  // Deliver the beacon.
  for (int i = 0; i < 40; ++i) {
    sender.Tick(52888);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    discovery.Poll();
    if (!discovery.Peers().empty()) break;
  }
  Expect(!discovery.Peers().empty(), "peer recorded under injected clock");
  Expect(discovery.Peers().front().device_id == "peer9999",
         "injected clock peer id matches");

  // Advance the injected clock beyond TTL without new beacons: expires.
  now += std::chrono::milliseconds(1501);
  discovery.Poll();
  Expect(discovery.Peers().empty(), "peer expires at TTL with injected clock");

  // Re-deliver, then advance within TTL: retained.
  const auto restart = std::chrono::steady_clock::now();
  while (discovery.Peers().empty() &&
         std::chrono::steady_clock::now() - restart <
             std::chrono::milliseconds(1000)) {
    sender.Tick(52888);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    discovery.Poll();
  }
  now += std::chrono::milliseconds(1499);
  discovery.Poll();
  Expect(!discovery.Peers().empty(), "peer retained inside TTL");
}

void SortOrder() {
  // Peers sort by last_seen descending. With two live instances the more
  // recently heard peer must come first.
  uint16_t shared_port = 0;
  {
    Discovery probe(TestConfig("probe"));
    probe.Start();
    shared_port = probe.bound_port();
  }
  auto config = TestConfig("self0000");
  config.udp_port = shared_port;
  Discovery observer(config);
  observer.Start();
  auto cfg1 = TestConfig("peer0001");
  cfg1.udp_port = shared_port;
  cfg1.hostname = "zulu";
  auto cfg2 = TestConfig("peer0002");
  cfg2.udp_port = shared_port;
  cfg2.hostname = "alpha";
  Discovery p1(cfg1), p2(cfg2);
  p1.Start();
  p2.Start();
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
  while (std::chrono::steady_clock::now() < deadline) {
    p1.Tick(52888);
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    observer.Poll();
    if (observer.Peers().size() >= 1) break;
  }
  // Now introduce p2 after p1: p2's beacons are fresher.
  const auto deadline2 =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
  while (std::chrono::steady_clock::now() < deadline2) {
    p1.Tick(52888);
    p2.Tick(52900);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    observer.Poll();
    if (observer.Peers().size() >= 2) {
      // give p2's latest beacon one final chance to be the freshest
      p2.Tick(52900);
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      observer.Poll();
      break;
    }
  }
  const auto peers = observer.Peers();
  Expect(peers.size() == 2, "two distinct peers tracked");
  if (peers.size() == 2) {
    Expect(peers[0].last_seen >= peers[1].last_seen,
           "peers sorted by last_seen descending");
    Expect(peers[0].device_id != peers[1].device_id, "distinct device ids");
  }
}

}  // namespace

int main() {
  PackUnpackRoundTrip();
  FullLengthDeviceIdRoundTrip();
  DeviceIdPaddingValidation();
  RejectMalformed();
  HostnameTruncation();
  TwoInstanceExchange();
  InjectedClockTtl();
  SortOrder();
  if (failures) {
    std::cerr << failures << " failure(s)\n";
    return 1;
  }
  std::cout << "lan_discovery_test: all tests passed\n";
  return 0;
}
