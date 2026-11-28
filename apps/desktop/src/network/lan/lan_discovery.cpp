#include "lan_discovery.h"

#include <algorithm>
#include <cstring>
#include <map>

#include "rd_log.h"

#ifdef _WIN32
// Winsock must precede windows.h; the networking headers need its types.
// clang-format off
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
// clang-format on
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace crossdesk::lan {
namespace {

constexpr size_t kMaxDatagramsPerPoll = 64;
constexpr size_t kMaxPeers = 128;
#ifdef _WIN32
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
int CloseSocket(SocketHandle fd) { return closesocket(fd); }
bool WouldBlock() { return WSAGetLastError() == WSAEWOULDBLOCK; }
int SocketError() { return WSAGetLastError(); }
#else
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;
int CloseSocket(SocketHandle fd) { return close(fd); }
bool WouldBlock() { return errno == EAGAIN || errno == EWOULDBLOCK; }
int SocketError() { return errno; }
#endif

void Store16(uint8_t* at, uint16_t value) {
  at[0] = static_cast<uint8_t>(value & 0xff);
  at[1] = static_cast<uint8_t>(value >> 8);
}
uint16_t Load16(const uint8_t* at) {
  return static_cast<uint16_t>(at[0] | (at[1] << 8));
}

// Whole-character UTF-8 truncation: never split a multi-byte sequence.
std::string TruncateUtf8(const std::string& text, size_t limit) {
  if (text.size() <= limit) return text;
  size_t end = limit;
  while (end > 0 && (static_cast<uint8_t>(text[end]) & 0xc0) == 0x80) --end;
  // text[end] is a leading byte only when the sequence fits entirely.
  const uint8_t lead = static_cast<uint8_t>(text[end]);
  size_t width = 1;
  if ((lead & 0x80) == 0)
    width = 1;
  else if ((lead & 0xe0) == 0xc0)
    width = 2;
  else if ((lead & 0xf0) == 0xe0)
    width = 3;
  else if ((lead & 0xf8) == 0xf0)
    width = 4;
  if (end + width > limit) return text.substr(0, end);
  return text.substr(0, limit);
}

// Field must contain a NUL terminator within its fixed capacity; the content
// is the byte range before the first NUL.
bool PaddedField(const uint8_t* at, size_t capacity, size_t& length) {
  const void* nul = memchr(at, 0, capacity);
  if (!nul) return false;
  length = static_cast<const uint8_t*>(nul) - at;
  return true;
}

#ifdef _WIN32
struct WinsockSession {
  WinsockSession() {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) == 0) owned = true;
  }
  ~WinsockSession() {
    if (owned) WSACleanup();
  }
  bool owned = false;
};
WinsockSession& Winsock() {
  static WinsockSession session;
  return session;
}
#endif

// IPv4 unicast address plus the subnet broadcast address of every up,
// non-loopback interface. Loopback is included so tests can exchange beacons
// over 127.0.0.1 when no LAN interface exists (e.g. CI sandboxes); real
// loopback traffic stays invisible to other devices.
std::vector<std::pair<in_addr, in_addr>> BroadcastTargets(
    bool include_loopback) {
  std::vector<std::pair<in_addr, in_addr>> result;
#ifdef _WIN32
  ULONG bytes = 16384;
  std::vector<unsigned char> buffer(bytes);
  auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
  ULONG code = GetAdaptersAddresses(
      AF_INET, GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr,
      adapters, &bytes);
  if (code == ERROR_BUFFER_OVERFLOW) {
    buffer.resize(bytes);
    adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    code = GetAdaptersAddresses(AF_INET,
                                GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                nullptr, adapters, &bytes);
  }
  if (code != NO_ERROR) return result;
  for (auto* a = adapters; a; a = a->Next) {
    if (a->OperStatus != IfOperStatusUp) continue;
    const bool loopback = a->IfType == IF_TYPE_SOFTWARE_LOOPBACK;
    if (loopback && !include_loopback) continue;
    for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
      if (!u->Address.lpSockaddr ||
          u->Address.lpSockaddr->sa_family != AF_INET)
        continue;
      const auto* sin =
          reinterpret_cast<const sockaddr_in*>(u->Address.lpSockaddr);
      in_addr local = sin->sin_addr;
      in_addr broadcast;
      broadcast.s_addr = htonl(0xffffffff);
      // Compute the subnet broadcast from the prefix length where available.
      ULONG mask = 0;
      if (ConvertLengthToIpv4Mask(u->OnLinkPrefixLength, &mask) == NO_ERROR &&
          mask != 0) {
        broadcast.s_addr = local.s_addr | ~mask;
      }
      result.push_back({local, broadcast});
    }
  }
#else
  ifaddrs* list = nullptr;
  if (getifaddrs(&list) != 0) return result;
  for (auto* a = list; a; a = a->ifa_next) {
    if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET ||
        !(a->ifa_flags & IFF_UP))
      continue;
    const bool loopback = (a->ifa_flags & IFF_LOOPBACK) != 0;
    if (loopback && !include_loopback) continue;
    const auto* sin = reinterpret_cast<const sockaddr_in*>(a->ifa_addr);
    in_addr local = sin->sin_addr;
    in_addr broadcast;
    broadcast.s_addr = htonl(0xffffffff);
    if ((a->ifa_flags & IFF_BROADCAST) && a->ifa_broadaddr &&
        a->ifa_broadaddr->sa_family == AF_INET) {
      broadcast =
          reinterpret_cast<const sockaddr_in*>(a->ifa_broadaddr)->sin_addr;
    } else if (a->ifa_netmask && a->ifa_netmask->sa_family == AF_INET) {
      const auto* mask = reinterpret_cast<const sockaddr_in*>(a->ifa_netmask);
      broadcast.s_addr = local.s_addr | ~mask->sin_addr.s_addr;
    }
    result.push_back({local, broadcast});
  }
  freeifaddrs(list);
#endif
  return result;
}

}  // namespace

bool PackBeacon(const Beacon& beacon, std::array<uint8_t, kBeaconSize>& out) {
  if (!beacon.tcp_port || beacon.device_id.empty()) return false;
  out.fill(0);
  memcpy(out.data(), kBeaconMagic, 4);
  out[4] = kBeaconVersion;
  out[5] = beacon.flags;
  Store16(out.data() + 6, beacon.tcp_port);
  const size_t id_length = std::min<size_t>(beacon.device_id.size(), 16);
  memcpy(out.data() + 8, beacon.device_id.data(), id_length);
  const std::string host = TruncateUtf8(beacon.hostname, 31);
  memcpy(out.data() + 24, host.data(), host.size());
  memcpy(out.data() + 56, beacon.ipv4.data(), 4);
  // bytes 60..63: reserved, already zeroed
  return true;
}

bool UnpackBeacon(const uint8_t* data, size_t size, Beacon& out) {
  if (!data || size != kBeaconSize) return false;
  if (memcmp(data, kBeaconMagic, 4) != 0) return false;
  if (data[4] != kBeaconVersion) return false;
  const uint16_t port = Load16(data + 6);
  if (!port) return false;
  // Reserved bytes must be zero so a future version can detect extensions.
  if (data[60] || data[61] || data[62] || data[63]) return false;
  // device_id is a 16-byte ASCII hex prefix: a full 16-character prefix needs
  // no terminator, while shorter ids are NUL-padded and the padding must stay
  // zero so a corrupted length cannot resurface stale bytes.
  size_t id_length = 16, host_length = 0;
  if (const void* nul = memchr(data + 8, 0, 16)) {
    id_length = static_cast<const uint8_t*>(nul) - (data + 8);
    for (size_t i = id_length; i < 16; ++i)
      if (data[8 + i] != 0) return false;
  }
  if (!id_length) return false;
  if (!PaddedField(data + 24, 32, host_length)) return false;
  Beacon beacon;
  beacon.flags = data[5];
  beacon.tcp_port = port;
  beacon.device_id.assign(reinterpret_cast<const char*>(data + 8), id_length);
  beacon.hostname.assign(reinterpret_cast<const char*>(data + 24),
                         host_length);
  memcpy(beacon.ipv4.data(), data + 56, 4);
  out = std::move(beacon);
  return true;
}

struct Discovery::Impl {
  explicit Impl(Config cfg, ClockFn clock_fn)
      : config(std::move(cfg)), clock(std::move(clock_fn)) {}
  Config config;
  ClockFn clock;
  SocketHandle socket = kInvalidSocket;
  bool sockets_ready = false;
  Clock::time_point next_broadcast{};
  Clock::time_point next_targets_refresh{};
  bool broadcasting = false;
  unsigned advertised_port = 0;
  in_addr primary_ipv4{};
  std::vector<std::pair<in_addr, in_addr>> targets;
  std::map<std::string, DiscoveredPeer> peers;

  bool Open() {
#ifdef _WIN32
    if (!Winsock().owned) {
      LOG_ERROR("LAN discovery: Winsock initialization failed");
      return false;
    }
#endif
    if (socket != kInvalidSocket) return true;
    socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket == kInvalidSocket) {
      LOG_ERROR("LAN discovery: socket creation failed");
      return false;
    }
    const int one = 1;
    setsockopt(socket, SOL_SOCKET, SO_BROADCAST,
               reinterpret_cast<const char*>(&one), sizeof(one));
    setsockopt(socket, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&one), sizeof(one));
#if defined(SO_REUSEPORT) && !defined(_WIN32)
    // Lets several instances on one host (and the per-user unattended host)
    // share the discovery port. Windows has no SO_REUSEPORT; SO_REUSEADDR
    // there already permits the shared bind for UDP.
    setsockopt(socket, SOL_SOCKET, SO_REUSEPORT,
               reinterpret_cast<const char*>(&one), sizeof(one));
#endif
#ifdef _WIN32
    unsigned long nonblocking = 1;
    ioctlsocket(socket, FIONBIO, &nonblocking);
#else
    fcntl(socket, F_SETFL, fcntl(socket, F_GETFL, 0) | O_NONBLOCK);
#endif
    sockaddr_in bind_addr{};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind_addr.sin_port = htons(config.udp_port);
    if (::bind(socket, reinterpret_cast<sockaddr*>(&bind_addr),
               sizeof(bind_addr)) != 0) {
      // A fixed-port bind conflict is non-fatal but disables discovery.
      LOG_ERROR("LAN discovery: cannot bind UDP port {}", config.udp_port);
      CloseSocket(socket);
      socket = kInvalidSocket;
      return false;
    }
    sockets_ready = true;
    return true;
  }

  void Close() {
    if (socket != kInvalidSocket) {
      CloseSocket(socket);
      socket = kInvalidSocket;
    }
    sockets_ready = false;
  }

  uint16_t BoundPort() const {
    if (socket == kInvalidSocket) return 0;
    sockaddr_in local{};
#ifdef _WIN32
    int length = sizeof(local);
#else
    socklen_t length = sizeof(local);
#endif
    if (getsockname(socket, reinterpret_cast<sockaddr*>(&local), &length) != 0)
      return 0;
    return ntohs(local.sin_port);
  }

  void RefreshTargets(const Clock::time_point& now) {
    if (now < next_targets_refresh && !targets.empty()) return;
    next_targets_refresh = now + std::chrono::seconds(2);
    targets = BroadcastTargets(true);
    primary_ipv4.s_addr = 0;
    for (const auto& [local, _] : targets) {
      const uint32_t host_order = ntohl(local.s_addr);
      // Prefer a non-loopback address as the advertised primary IPv4.
      if ((host_order & 0xff000000u) != 0x7f000000u) {
        primary_ipv4 = local;
        break;
      }
      if (!primary_ipv4.s_addr) primary_ipv4 = local;
    }
  }

  void Broadcast() {
    Beacon beacon;
    beacon.flags = static_cast<uint8_t>(
        (config.listen_all ? kBeaconFlagListenAll : 0) |
        (config.password_set ? kBeaconFlagPasswordSet : 0));
    beacon.tcp_port = static_cast<uint16_t>(advertised_port);
    beacon.device_id = config.device_id;
    beacon.hostname = config.hostname;
    memcpy(beacon.ipv4.data(), &primary_ipv4.s_addr, 4);
    std::array<uint8_t, kBeaconSize> packet;
    if (!PackBeacon(beacon, packet)) return;
    // The limited broadcast reaches interfaces whose broadcast address is
    // misconfigured; per-interface subnet broadcasts reach segmented networks
    // that filter 255.255.255.255.
    std::vector<in_addr> destinations;
    in_addr limited;
    limited.s_addr = htonl(0xffffffff);
    destinations.push_back(limited);
    for (const auto& target_pair : targets) {
      const in_addr& broadcast = target_pair.second;
      if (std::none_of(destinations.begin(), destinations.end(),
                       [&](const in_addr& existing) {
                         return existing.s_addr == broadcast.s_addr;
                       }))
        destinations.push_back(broadcast);
    }
    for (const auto& destination : destinations) {
      sockaddr_in target{};
      target.sin_family = AF_INET;
      target.sin_addr = destination;
      // Instances ordinarily share the fixed discovery port. Tests bind
      // ephemeral ports and rewrite this destination per instance through the
      // same config knob.
      target.sin_port = htons(config.udp_port ? config.udp_port : BoundPort());
      if (sendto(socket, reinterpret_cast<const char*>(packet.data()),
                 packet.size(), 0, reinterpret_cast<sockaddr*>(&target),
                 sizeof(target)) < 0 &&
          !WouldBlock()) {
        LOG_ERROR("LAN discovery: broadcast failed ({})", SocketError());
      }
    }
  }

  void Receive() {
    if (socket == kInvalidSocket) return;
    for (size_t i = 0; i < kMaxDatagramsPerPoll; ++i) {
      uint8_t buffer[256];
      sockaddr_in from{};
#ifdef _WIN32
      int from_length = sizeof(from);
#else
      socklen_t from_length = sizeof(from);
#endif
      const auto received =
          recvfrom(socket, reinterpret_cast<char*>(buffer), sizeof(buffer), 0,
                   reinterpret_cast<sockaddr*>(&from), &from_length);
      if (received < 0) {
        if (!WouldBlock())
          LOG_ERROR("LAN discovery: receive error ({})", SocketError());
        return;
      }
      Beacon beacon;
      if (!UnpackBeacon(buffer, static_cast<size_t>(received), beacon))
        continue;
      if (beacon.device_id == config.device_id) continue;  // own beacon
      char text[INET_ADDRSTRLEN]{};
      if (!inet_ntop(AF_INET, &from.sin_addr, text, sizeof(text))) continue;
      const auto now = clock();
      auto& peer = peers[beacon.device_id];
      if (peer.device_id.empty() && peers.size() > kMaxPeers) {
        peers.erase(beacon.device_id);
        return;  // table full of strangers: do not grow unbounded
      }
      peer.device_id = beacon.device_id;
      peer.hostname = beacon.hostname;
      peer.address = text;
      peer.port = beacon.tcp_port;
      peer.last_seen = now;
    }
  }

  void Expire(const Clock::time_point& now) {
    for (auto it = peers.begin(); it != peers.end();) {
      if (now - it->second.last_seen >= config.peer_ttl)
        it = peers.erase(it);
      else
        ++it;
    }
  }
};

Discovery::Discovery(Config config, ClockFn clock)
    : impl_(std::make_unique<Impl>(std::move(config), std::move(clock))) {}
Discovery::~Discovery() = default;

bool Discovery::Start() {
  if (!impl_->Open()) return false;
  impl_->next_broadcast = {};  // advertise immediately once a port is known
  return true;
}

void Discovery::Stop() {
  impl_->Close();
  impl_->peers.clear();
  impl_->broadcasting = false;
  impl_->advertised_port = 0;
}

bool Discovery::active() const { return impl_->sockets_ready; }

void Discovery::SetPresence(std::string hostname, bool listen_all,
                            bool password_set) {
  impl_->config.hostname = std::move(hostname);
  impl_->config.listen_all = listen_all;
  impl_->config.password_set = password_set;
}

uint16_t Discovery::bound_port() const { return impl_->BoundPort(); }

void Discovery::Tick(unsigned tcp_port) {
  if (!impl_->sockets_ready) return;
  const auto now = impl_->clock();
  impl_->RefreshTargets(now);
  impl_->Receive();
  impl_->Expire(now);
  const bool should_broadcast = tcp_port != 0;
  if (should_broadcast &&
      (!impl_->broadcasting || impl_->advertised_port != tcp_port)) {
    // Listener state changed: announce immediately instead of waiting out
    // the interval so peers notice a restarted or rebound listener quickly.
    impl_->next_broadcast = now;
  }
  impl_->broadcasting = should_broadcast;
  impl_->advertised_port = tcp_port;
  if (should_broadcast && now >= impl_->next_broadcast) {
    impl_->Broadcast();
    impl_->next_broadcast = now + impl_->config.broadcast_interval;
  }
}

void Discovery::Poll() {
  if (!impl_->sockets_ready) return;
  impl_->Receive();
  impl_->Expire(impl_->clock());
}

std::vector<DiscoveredPeer> Discovery::Peers() const {
  std::vector<DiscoveredPeer> result;
  result.reserve(impl_->peers.size());
  for (const auto& [_, peer] : impl_->peers) result.push_back(peer);
  // Presentation order must not follow freshness: every broadcast refreshes
  // last_seen, so ordering by it makes two equally live peers trade places
  // on every poll and the discovery chips visibly jump. Sort by stable
  // identity instead; last_seen still drives expiry.
  std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
    if (a.hostname != b.hostname) return a.hostname < b.hostname;
    if (a.device_id != b.device_id) return a.device_id < b.device_id;
    if (a.address != b.address) return a.address < b.address;
    return a.port < b.port;
  });
  return result;
}

}  // namespace crossdesk::lan
