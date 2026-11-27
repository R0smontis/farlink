/*
 * @Author: Farlink contributors
 * @Date: 2026-11-26
 * Copyright (c) 2026 by Farlink contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef _LAUNCH_OPTIONS_H_
#define _LAUNCH_OPTIONS_H_

#include <string>
#include <set>
#include <vector>

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
  struct PortForwardRule {
    // Loopback listen port on this controller; zero selects an ephemeral port.
    unsigned local_port = 0;
    // Loopback destination port on the remote host. Never zero: a rule that
    // does not name its target would forward anywhere, which the security
    // model forbids.
    unsigned remote_port = 0;
  };
  // --port-forward <local_port>:<remote_port>, repeatable. Rules apply to the
  // first outgoing connection of this process (the --lan-connect target when
  // present, otherwise the first UI-initiated connection). They are operator
  // intent for one session only — never persisted, never broadened.
  std::vector<PortForwardRule> port_forward_rules;
  // --port-forward-reverse <local_port>:<remote_port>, repeatable: accepting-
  // side forwarding rules. Where --port-forward means "when I connect OUT to
  // that host, expose my loopback service to it", a reverse rule means "when
  // an approved controller connects IN to me, forward my loopback service to
  // it through its own host endpoint". Reverse rules NEVER inherit from
  // outgoing rules and never activate merely because a connection exists:
  // each incoming connection must independently qualify — either the
  // controller completed the trusted-device proof with a pinned key, or the
  // operator explicitly approved that exact live connection through the
  // runtime API. The tunnel terminates on the controller's own machine,
  // where the controller's independent host consent decides whether the
  // destination accepts the open — consent at both ends, neither side's
  // approval substituting for the other's.
  std::vector<PortForwardRule> port_forward_reverse_rules;
  // --port-forward-allow <port>[,<port>...]: explicit local host consent for
  // incoming controllers to reach these loopback destination ports through
  // this machine. This grant is process-lifetime operator intent — it is
  // never persisted, so consent cannot outlive the session context in which
  // the operator granted it, and every individual connection still faces the
  // full admission chain (authorized control session + negotiated SRTP).
  // Persistent consent across restarts exists only for cryptographically
  // verified trusted devices (see port_forward_runtime.cpp).
  std::set<std::uint16_t> port_forward_allow;
  // --no-local-capture: the instance never initializes local screen/audio
  // capture. Controller-only deployments use this to avoid touching
  // ScreenCaptureKit at all — on macOS a second
  // same-machine instance's SCShareableContent enumeration disrupts a
  // capturing instance's stream startup through replayd, so two instances on
  // one machine must not both initialize capture. Read once at startup.
  bool no_local_capture = false;
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
