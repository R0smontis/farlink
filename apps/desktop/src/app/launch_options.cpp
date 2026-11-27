/*
 * @Author: Farlink contributors
 * @Date: 2026-11-26
 * Copyright (c) 2026 by Farlink contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "launch_options.h"

#include <cstdlib>
#include <charconv>
#include <cstring>

namespace crossdesk {

namespace {
LaunchOptions g_launch_options;

// Parses "<local>:<remote>" into a rule. Local zero selects an ephemeral
// port; remote zero is rejected — a rule that does not name its destination
// would forward anywhere, which the security model forbids.
bool ParsePortForwardRule(const std::string& spec,
                          LaunchOptions::PortForwardRule* out) {
  const auto colon = spec.find(':');
  unsigned local = 0, remote = 0;
  const auto parse = [](const char* begin, const char* end, unsigned& value) {
    if (begin == end) return false;
    const auto result = std::from_chars(begin, end, value);
    return result.ec == std::errc{} && result.ptr == end && value <= 65535;
  };
  if (colon == std::string::npos || colon == 0 ||
      !parse(spec.data(), spec.data() + colon, local) ||
      !parse(spec.data() + colon + 1, spec.data() + spec.size(), remote) ||
      remote == 0)
    return false;
  out->local_port = local;
  out->remote_port = remote;
  return true;
}
}

const LaunchOptions& GetLaunchOptions() { return g_launch_options; }

LaunchOptions& MutableLaunchOptions() { return g_launch_options; }

bool ParseLaunchOptions(int argc, char* argv[]) {
  bool valid = true;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--lan-connect") == 0) {
      if (i + 1 < argc && argv[i + 1][0] != '-') {
        g_launch_options.lan_connect = true;
        g_launch_options.lan_endpoint = argv[++i];
      } else {
        valid = false;
      }
    } else if (std::strcmp(argv[i], "--view-only") == 0) {
      g_launch_options.view_only = true;
    } else if (std::strcmp(argv[i], "--port-forward") == 0) {
      if (i + 1 >= argc || argv[i + 1][0] == '-') {
        valid = false;
        continue;
      }
      LaunchOptions::PortForwardRule rule{};
      if (!ParsePortForwardRule(argv[++i], &rule)) {
        valid = false;
        continue;
      }
      g_launch_options.port_forward_rules.push_back(rule);
    } else if (std::strcmp(argv[i], "--port-forward-reverse") == 0) {
      if (i + 1 >= argc || argv[i + 1][0] == '-') {
        valid = false;
        continue;
      }
      LaunchOptions::PortForwardRule rule{};
      if (!ParsePortForwardRule(argv[++i], &rule)) {
        valid = false;
        continue;
      }
      g_launch_options.port_forward_reverse_rules.push_back(rule);
    } else if (std::strcmp(argv[i], "--port-forward-allow") == 0) {
      if (i + 1 >= argc || argv[i + 1][0] == '-') {
        valid = false;
        continue;
      }
      const std::string spec = argv[++i];
      // Comma-separated destination ports; every entry must be a valid
      // nonzero port or the whole option is rejected — a partially parsed
      // consent list would silently narrow what the operator wrote.
      bool spec_valid = !spec.empty();
      std::set<std::uint16_t> ports;
      std::size_t begin = 0;
      while (spec_valid && begin <= spec.size()) {
        const auto comma = spec.find(',', begin);
        const auto end = comma == std::string::npos ? spec.size() : comma;
        unsigned port = 0;
        const auto result = std::from_chars(spec.data() + begin, spec.data() + end, port);
        if (end == begin || result.ec != std::errc{} ||
            result.ptr != spec.data() + end || port == 0 || port > 65535) {
          spec_valid = false;
          break;
        }
        ports.insert(static_cast<std::uint16_t>(port));
        if (comma == std::string::npos) break;
        begin = comma + 1;
      }
      if (!spec_valid) {
        valid = false;
        continue;
      }
      g_launch_options.port_forward_allow.insert(ports.begin(), ports.end());
    } else if (std::strcmp(argv[i], "--no-local-capture") == 0) {
      g_launch_options.no_local_capture = true;
    }
  }
  return valid;
}

}  // namespace crossdesk
