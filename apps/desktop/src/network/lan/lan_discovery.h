/*
 * @Author: DI JUNKUN
 * @Date: 2026-11-27
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _LAN_DISCOVERY_H_
#define _LAN_DISCOVERY_H_

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace crossdesk::lan {

// Fixed 64-byte little-endian wire beacon broadcast over UDP. The layout is
// packed by hand in PackBeacon; no padding or host-order field ever reaches
// the wire, so mixed-endian and mixed-bitness peers decode identically.
//
//   0  magic      4 bytes  "FLN1"
//   4  version    1 byte   currently 1
//   5  flags      1 byte   bit0 = listen_all, bit1 = password_set
//   6  tcp_port   2 bytes  little-endian listener port
//   8  device_id 16 bytes  ASCII hex prefix of Identity::id(), NUL-padded
//  24  hostname  32 bytes  UTF-8, NUL-padded, truncated to whole characters
//  56  ipv4       4 bytes  sender primary IPv4, network byte order
//  60  reserved   4 bytes  zero
inline constexpr size_t kBeaconSize = 64;
inline constexpr char kBeaconMagic[5] = "FLN1";
inline constexpr uint8_t kBeaconVersion = 1;
inline constexpr uint8_t kBeaconFlagListenAll = 0x01;
inline constexpr uint8_t kBeaconFlagPasswordSet = 0x02;
inline constexpr uint16_t kDiscoveryPort = 52889;

struct Beacon {
  uint8_t flags = 0;
  uint16_t tcp_port = 0;
  std::string device_id;  // up to 16 ASCII hex characters; shorter ids are NUL-padded
  std::string hostname;   // UTF-8, at most 31 bytes on the wire
  std::array<uint8_t, 4> ipv4{};
};

// Serializes a validated beacon into exactly kBeaconSize bytes. Returns false
// when the port is zero or the device id is empty; overlong hostnames are
// truncated at a UTF-8 character boundary instead of failing.
bool PackBeacon(const Beacon& beacon, std::array<uint8_t, kBeaconSize>& out);
// Validates magic, version, port range, reserved bytes, NUL padding and a
// non-empty device id. Returns false for any malformed datagram, including
// truncated input and embedded NULs in the hostname/device fields.
bool UnpackBeacon(const uint8_t* data, size_t size, Beacon& out);

struct DiscoveredPeer {
  std::string device_id;
  std::string hostname;
  std::string address;  // IPv4 dotted-quad the beacon was received from
  unsigned port = 0;    // TCP listener port advertised by the peer
  std::chrono::steady_clock::time_point last_seen;
};

// UDP broadcast auto-discovery for LAN peers. Advertises this device's LAN
// listener with a fixed-size beacon and maintains a bounded peer table with
// TTL expiry. Discovery only announces presence; authentication still goes
// through the unchanged PAKE password exchange. All methods run on the owner
// thread (the GUI tick); there is no internal networking thread.
class Discovery {
 public:
  using Clock = std::chrono::steady_clock;
  using ClockFn = std::function<Clock::time_point()>;

  struct Config {
    // Shared discovery port. Zero binds an ephemeral receive port; unit tests
    // use that to run several instances in one process, then point instances
    // at each other through a shared fixed port.
    uint16_t udp_port = kDiscoveryPort;
    std::chrono::milliseconds broadcast_interval{2000};
    std::chrono::milliseconds peer_ttl{15000};
    bool listen_all = true;
    bool password_set = false;
    std::string device_id;  // own id; beacons carrying it are ignored
    std::string hostname;
  };

  // clock is injectable for deterministic TTL and scheduling tests.
  explicit Discovery(Config config, ClockFn clock = Clock::now);
  ~Discovery();
  Discovery(const Discovery&) = delete;
  Discovery& operator=(const Discovery&) = delete;

  // Opens the non-blocking broadcast socket. Returns false without throwing
  // when the platform rejects the socket; discovery then stays inert.
  bool Start();
  void Stop();
  bool active() const;

  // Presence metadata is mutable: password rotation and listener-mode changes
  // must reach the next beacon without rebinding the socket.
  void SetPresence(std::string hostname, bool listen_all, bool password_set);

  // Broadcasts immediately when the listener becomes available and then on
  // the configured interval, to 255.255.255.255 plus every IPv4 interface
  // subnet broadcast address. While the LAN listener is off (tcp_port == 0)
  // nothing is advertised; receiving continues so peers that restart reappear
  // quickly.
  void Tick(unsigned tcp_port);
  // One non-blocking drain of pending datagrams.
  void Poll();
  std::vector<DiscoveredPeer> Peers() const;
  // Test/introspection hook: the UDP port the receive socket is bound to.
  uint16_t bound_port() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace crossdesk::lan

#endif
