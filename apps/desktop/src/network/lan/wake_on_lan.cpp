/*
 * @Author: DI JUNKUN
 * @Date: 2026-11-27
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#include "wake_on_lan.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <vector>

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
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
    defined(__NetBSD__)
#include <net/if_dl.h>
#elif defined(__linux__)
#include <netpacket/packet.h>
#endif
#endif

namespace crossdesk::lan {
namespace {

constexpr uint8_t kBroadcastMac[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
constexpr uint8_t kZeroMac[6] = {};

bool ReservedMac(const MacAddress& mac) {
  return !std::memcmp(mac.data(), kBroadcastMac, 6) ||
         !std::memcmp(mac.data(), kZeroMac, 6);
}

int HexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool ParseSeparated(std::string_view text, char separator, MacAddress& out) {
  size_t position = 0;
  for (size_t index = 0; index < 6; ++index) {
    if (position + 2 > text.size()) return false;
    const int high = HexDigit(text[position]);
    const int low = HexDigit(text[position + 1]);
    if (high < 0 || low < 0) return false;
    out[index] = static_cast<uint8_t>((high << 4) | low);
    position += 2;
    if (index != 5) {
      if (position >= text.size() || text[position] != separator) return false;
      ++position;
    }
  }
  return position == text.size();
}

bool ParseDotted(std::string_view text, MacAddress& out) {
  size_t position = 0;
  for (size_t group = 0; group < 3; ++group) {
    if (position + 4 > text.size()) return false;
    for (size_t digit = 0; digit < 4; ++digit) {
      const int value = HexDigit(text[position + digit]);
      if (value < 0) return false;
      const size_t byte_index = group * 2 + digit / 2;
      if (digit % 2 == 0)
        out[byte_index] = static_cast<uint8_t>(value << 4);
      else
        out[byte_index] |= static_cast<uint8_t>(value);
    }
    position += 4;
    if (group != 2) {
      if (position >= text.size() || text[position] != '.') return false;
      ++position;
    }
  }
  return position == text.size();
}

bool ParsePlain(std::string_view text, MacAddress& out) {
  if (text.size() != 12) return false;
  for (size_t index = 0; index < 6; ++index) {
    const int high = HexDigit(text[index * 2]);
    const int low = HexDigit(text[index * 2 + 1]);
    if (high < 0 || low < 0) return false;
    out[index] = static_cast<uint8_t>((high << 4) | low);
  }
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

using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
int CloseSocket(SocketHandle fd) { return closesocket(fd); }
int SocketError() { return WSAGetLastError(); }
#else
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;
int CloseSocket(SocketHandle fd) { return close(fd); }
int SocketError() { return errno; }
#endif

std::string SocketErrorMessage(const char* operation) {
  const int code = SocketError();
  char buffer[128];
#ifdef _WIN32
  std::snprintf(buffer, sizeof(buffer), "%s failed (%d)", operation, code);
#else
  std::snprintf(buffer, sizeof(buffer), "%s failed (%d: %s)", operation, code,
                std::strerror(code));
#endif
  return buffer;
}

bool SendToEndpoint(SocketHandle socket_fd, const uint8_t* packet, size_t size,
                    const in_addr& address, uint16_t port, std::string* error) {
  sockaddr_in endpoint{};
  endpoint.sin_family = AF_INET;
  endpoint.sin_addr = address;
  endpoint.sin_port = htons(port);
  const int sent = sendto(socket_fd, reinterpret_cast<const char*>(packet),
#ifdef _WIN32
                          static_cast<int>(size),
#else
                          size,
#endif
                          0, reinterpret_cast<const sockaddr*>(&endpoint),
                          sizeof(endpoint));
  if (sent != static_cast<int>(size)) {
    if (error) *error = SocketErrorMessage("Wake send");
    return false;
  }
  return true;
}

std::vector<in_addr> InterfaceBroadcastAddresses() {
  std::vector<in_addr> result;
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
  for (auto* adapter = adapters; adapter; adapter = adapter->Next) {
    if (adapter->OperStatus != IfOperStatusUp ||
        adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
      continue;
    for (auto* unicast = adapter->FirstUnicastAddress; unicast;
         unicast = unicast->Next) {
      if (!unicast->Address.lpSockaddr ||
          unicast->Address.lpSockaddr->sa_family != AF_INET)
        continue;
      const auto* sin =
          reinterpret_cast<const sockaddr_in*>(unicast->Address.lpSockaddr);
      in_addr broadcast;
      broadcast.s_addr = htonl(0xffffffff);
      ULONG mask = 0;
      if (ConvertLengthToIpv4Mask(unicast->OnLinkPrefixLength, &mask) ==
              NO_ERROR &&
          mask != 0) {
        broadcast.s_addr = sin->sin_addr.s_addr | ~mask;
      }
      if (std::none_of(result.begin(), result.end(), [&](const in_addr& value) {
            return value.s_addr == broadcast.s_addr;
          }))
        result.push_back(broadcast);
    }
  }
#else
  ifaddrs* list = nullptr;
  if (getifaddrs(&list) != 0) return result;
  for (auto* adapter = list; adapter; adapter = adapter->ifa_next) {
    if (!adapter->ifa_addr || adapter->ifa_addr->sa_family != AF_INET ||
        !(adapter->ifa_flags & IFF_UP) || (adapter->ifa_flags & IFF_LOOPBACK))
      continue;
    const auto* sin = reinterpret_cast<const sockaddr_in*>(adapter->ifa_addr);
    in_addr broadcast;
    broadcast.s_addr = htonl(0xffffffff);
    if ((adapter->ifa_flags & IFF_BROADCAST) && adapter->ifa_broadaddr &&
        adapter->ifa_broadaddr->sa_family == AF_INET) {
      broadcast =
          reinterpret_cast<const sockaddr_in*>(adapter->ifa_broadaddr)->sin_addr;
    } else if (adapter->ifa_netmask &&
               adapter->ifa_netmask->sa_family == AF_INET) {
      const auto* mask =
          reinterpret_cast<const sockaddr_in*>(adapter->ifa_netmask);
      broadcast.s_addr = sin->sin_addr.s_addr | ~mask->sin_addr.s_addr;
    }
    if (std::none_of(result.begin(), result.end(), [&](const in_addr& value) {
          return value.s_addr == broadcast.s_addr;
        }))
      result.push_back(broadcast);
  }
  freeifaddrs(list);
#endif
  return result;
}

#ifdef _WIN32
std::optional<MacAddress> PrimaryMacWindows() {
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
  if (code != NO_ERROR) return std::nullopt;
  for (auto* adapter = adapters; adapter; adapter = adapter->Next) {
    if (adapter->OperStatus != IfOperStatusUp ||
        adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK ||
        adapter->PhysicalAddressLength != 6)
      continue;
    bool has_ipv4 = false;
    for (auto* unicast = adapter->FirstUnicastAddress; unicast;
         unicast = unicast->Next) {
      if (unicast->Address.lpSockaddr &&
          unicast->Address.lpSockaddr->sa_family == AF_INET) {
        has_ipv4 = true;
        break;
      }
    }
    if (!has_ipv4) continue;
    MacAddress mac{};
    std::memcpy(mac.data(), adapter->PhysicalAddress, 6);
    if (!ReservedMac(mac)) return mac;
  }
  return std::nullopt;
}
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
    defined(__NetBSD__)
std::optional<MacAddress> PrimaryMacIfaddrs() {
  ifaddrs* list = nullptr;
  if (getifaddrs(&list) != 0) return std::nullopt;
  std::optional<MacAddress> result;
  for (auto* adapter = list; adapter && !result; adapter = adapter->ifa_next) {
    if (!adapter->ifa_name || !adapter->ifa_addr) continue;
    const std::string_view name(adapter->ifa_name);
    if (name.rfind("lo", 0) == 0 || name.rfind("utun", 0) == 0 ||
        name.rfind("awdl", 0) == 0)
      continue;
    for (auto* ipv4 = list; ipv4; ipv4 = ipv4->ifa_next) {
      if (!ipv4->ifa_name || name != ipv4->ifa_name || !ipv4->ifa_addr ||
          ipv4->ifa_addr->sa_family != AF_INET ||
          !(ipv4->ifa_flags & IFF_UP) || (ipv4->ifa_flags & IFF_LOOPBACK))
        continue;
      if (adapter->ifa_addr->sa_family != AF_LINK) continue;
      const auto* link =
          reinterpret_cast<const sockaddr_dl*>(adapter->ifa_addr);
      if (link->sdl_alen != 6) continue;
      MacAddress mac{};
      std::memcpy(mac.data(), LLADDR(link), 6);
      if (!ReservedMac(mac)) result = mac;
      break;
    }
  }
  freeifaddrs(list);
  return result;
}
#elif defined(__linux__)
std::optional<MacAddress> PrimaryMacIfaddrs() {
  ifaddrs* list = nullptr;
  if (getifaddrs(&list) != 0) return std::nullopt;
  std::optional<MacAddress> result;
  for (auto* adapter = list; adapter && !result; adapter = adapter->ifa_next) {
    if (!adapter->ifa_name || !adapter->ifa_addr ||
        adapter->ifa_addr->sa_family != AF_PACKET)
      continue;
    const std::string_view name(adapter->ifa_name);
    if (name.rfind("lo", 0) == 0) continue;
    for (auto* ipv4 = list; ipv4; ipv4 = ipv4->ifa_next) {
      if (!ipv4->ifa_name || name != ipv4->ifa_name || !ipv4->ifa_addr ||
          ipv4->ifa_addr->sa_family != AF_INET ||
          !(ipv4->ifa_flags & IFF_UP) || (ipv4->ifa_flags & IFF_LOOPBACK))
        continue;
      const auto* link =
          reinterpret_cast<const sockaddr_ll*>(adapter->ifa_addr);
      if (link->sll_halen != 6) continue;
      MacAddress mac{};
      std::memcpy(mac.data(), link->sll_addr, 6);
      if (!ReservedMac(mac)) result = mac;
      break;
    }
  }
  freeifaddrs(list);
  return result;
}
#endif

}  // namespace

std::optional<MacAddress> ParseMacAddress(std::string_view text) {
  MacAddress mac{};
  bool valid = false;
  if (text.size() == 17 && text[2] == ':')
    valid = ParseSeparated(text, ':', mac);
  else if (text.size() == 17 && text[2] == '-')
    valid = ParseSeparated(text, '-', mac);
  else if (text.size() == 14)
    valid = ParseDotted(text, mac);
  else if (text.size() == 12)
    valid = ParsePlain(text, mac);
  if (!valid || ReservedMac(mac)) return std::nullopt;
  return mac;
}

std::string FormatMacAddress(const MacAddress& mac) {
  char text[18];
  std::snprintf(text, sizeof(text), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0],
                mac[1], mac[2], mac[3], mac[4], mac[5]);
  return text;
}

std::array<uint8_t, 102> BuildMagicPacket(const MacAddress& mac) {
  std::array<uint8_t, 102> packet{};
  std::memset(packet.data(), 0xff, 6);
  for (size_t repetition = 0; repetition < 16; ++repetition)
    std::memcpy(packet.data() + 6 + repetition * 6, mac.data(), 6);
  return packet;
}

bool SendMagicPacket(const MacAddress& mac, std::string_view broadcast_ip,
                     uint16_t port, std::string* error) {
  if (error) error->clear();
  if (ReservedMac(mac)) {
    if (error) *error = "Invalid wake MAC address";
    return false;
  }
#ifdef _WIN32
  if (!Winsock().owned) {
    if (error) *error = "Winsock initialization failed";
    return false;
  }
#endif
  const auto packet = BuildMagicPacket(mac);
  SocketHandle socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (socket_fd == kInvalidSocket) {
    if (error) *error = SocketErrorMessage("Wake socket");
    return false;
  }
  const int enable = 1;
  if (setsockopt(socket_fd, SOL_SOCKET, SO_BROADCAST,
                 reinterpret_cast<const char*>(&enable), sizeof(enable)) != 0) {
    if (error) *error = SocketErrorMessage("Wake broadcast setup");
    CloseSocket(socket_fd);
    return false;
  }

  bool sent = false;
  std::string first_error;
  const auto send = [&](const in_addr& address) {
    std::string send_error;
    if (SendToEndpoint(socket_fd, packet.data(), packet.size(), address, port,
                       &send_error)) {
      sent = true;
    } else if (first_error.empty()) {
      first_error = std::move(send_error);
    }
  };

  if (!broadcast_ip.empty()) {
    in_addr endpoint{};
    const std::string text(broadcast_ip);
    if (inet_pton(AF_INET, text.c_str(), &endpoint) != 1) {
      if (error) *error = "Invalid wake broadcast address";
      CloseSocket(socket_fd);
      return false;
    }
    send(endpoint);
  } else {
    in_addr limited{};
    limited.s_addr = htonl(INADDR_BROADCAST);
    send(limited);
    for (const auto& address : InterfaceBroadcastAddresses()) {
      if (address.s_addr == limited.s_addr) continue;
      send(address);
    }
  }
  CloseSocket(socket_fd);
  if (!sent && error) *error = first_error.empty() ? "Wake send failed" : first_error;
  return sent;
}

std::optional<MacAddress> GetPrimaryLocalMacAddress() {
#ifdef _WIN32
  return PrimaryMacWindows();
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
    defined(__NetBSD__) || defined(__linux__)
  return PrimaryMacIfaddrs();
#else
  return std::nullopt;
#endif
}

}  // namespace crossdesk::lan
