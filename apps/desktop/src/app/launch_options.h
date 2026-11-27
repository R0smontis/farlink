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
  // --no-local-capture: the instance never initializes local screen/audio
  // capture. Controller-only deployments use this to avoid touching
  // ScreenCaptureKit at all — on macOS a second
  // same-machine instance's SCShareableContent enumeration disrupts a
  // capturing instance's stream startup through replayd, so two instances on
  // one machine must not both initialize capture. Read once at startup.
  bool no_local_capture = false;
  // --wol <mac> [--wol-broadcast <ip>]: send a Wake-on-LAN magic packet and
  // exit before any instance acquisition or GUI startup, so scripted power
  // management never disturbs a running session.
  bool wol = false;
  std::string wol_mac;
  std::string wol_broadcast;
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
