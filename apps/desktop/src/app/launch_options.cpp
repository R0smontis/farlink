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
      const std::string spec = argv[++i];
      const auto colon = spec.find(':');
      unsigned local = 0, remote = 0;
      const auto parse = [](const char* begin, const char* end, unsigned& out) {
        if (begin == end) return false;
        const auto result = std::from_chars(begin, end, out);
        return result.ec == std::errc{} && result.ptr == end && out <= 65535;
      };
      if (colon == std::string::npos || colon == 0 ||
          !parse(spec.data(), spec.data() + colon, local) ||
          !parse(spec.data() + colon + 1, spec.data() + spec.size(), remote) ||
          remote == 0) {
        valid = false;
        continue;
      }
      g_launch_options.port_forward_rules.push_back({local, remote});
    } else if (std::strcmp(argv[i], "--no-local-capture") == 0) {
      g_launch_options.no_local_capture = true;
    }
  }
  return valid;
}

}  // namespace crossdesk
