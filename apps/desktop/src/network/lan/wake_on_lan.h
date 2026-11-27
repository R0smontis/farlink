/*
 * @Author: DI JUNKUN
 * @Date: 2026-11-27
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _WAKE_ON_LAN_H_
#define _WAKE_ON_LAN_H_

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace crossdesk::lan {

using MacAddress = std::array<uint8_t, 6>;

// Accepts AA:BB:CC:DD:EE:FF, AA-BB-CC-DD-EE-FF, aabb.ccdd.eeff and
// AABBCCDDEEFF. Broadcast and all-zero station addresses are not wake targets.
std::optional<MacAddress> ParseMacAddress(std::string_view text);
std::string FormatMacAddress(const MacAddress& mac);

// Standard Ethernet wake frame payload: six FF bytes followed by sixteen
// repetitions of the station address.
std::array<uint8_t, 102> BuildMagicPacket(const MacAddress& mac);

// UDP-broadcasts the magic packet. An explicit broadcast address is sent only
// there; the empty default reaches 255.255.255.255 plus every active IPv4
// interface broadcast address. Returns true when at least one send succeeds.
bool SendMagicPacket(const MacAddress& mac, std::string_view broadcast_ip = {},
                     uint16_t port = 9, std::string* error = nullptr);

// Primary station address used when this device advertises itself for wake.
std::optional<MacAddress> GetPrimaryLocalMacAddress();

}  // namespace crossdesk::lan

#endif
