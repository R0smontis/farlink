/*
 * @Author: Farlink contributors
 * @Date: 2026-11-26
 * Copyright (c) 2026 by Farlink contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef _LAUNCH_OPTIONS_H_
#define _LAUNCH_OPTIONS_H_

#include <string>

namespace crossdesk {

// Non-interactive launch options collected from the process arguments in
// main(). They are process-lifetime constants: written once before the
// single-instance leases are acquired and read by the GUI on its UI thread
// during startup. This is intentionally not a wire-protocol or configuration
// surface.
struct LaunchOptions {
  // --lan-connect <ip:port>: connect to a LAN host without clicking through
  // the UI. The access password is never taken from the command line; it is
  // read from the FARLINK_LAN_PASSWORD environment variable. Without a
  // password the ordinary UI dialog opens with the endpoint prefilled.
  bool lan_connect = false;
  std::string lan_endpoint;
  // --view-only: the first LAN connection of this process starts with mouse
  // and keyboard control disabled, as if the user had toggled control off in
  // the stream toolbar before any input was captured.
  bool view_only = false;
};

// Storage backing the process-wide launch options. main() must call
// MutableLaunchOptions() exactly once before acquisition; every other reader
// uses GetLaunchOptions().
const LaunchOptions& GetLaunchOptions();
LaunchOptions& MutableLaunchOptions();

// Parse argv into the launch options. Unknown arguments are ignored, matching
// the historical behaviour of the desktop entry point. Returns false when a
// recognised option is malformed (currently: --lan-connect without a value);
// the caller decides how to report it.
bool ParseLaunchOptions(int argc, char* argv[]);

}  // namespace crossdesk

#endif
