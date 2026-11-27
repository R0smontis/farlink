/*
 * Port-forward session wiring: capability gating, consent enforcement,
 * encrypted-channel admission, and per-connection tunnel lifetime.
 *
 * Copyright (c) 2026 Farlink contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef CROSSDESK_GUI_PORT_FORWARD_CONTROLLER_H_
#define CROSSDESK_GUI_PORT_FORWARD_CONTROLLER_H_

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "port_forward_tunnel.h"

namespace crossdesk {

class GuiRuntime;
class PeerEventHandler;

// Owns every tunnel endpoint for the process and enforces the security
// contract before any byte moves:
//   - the peer must advertise supports_port_forward;
//   - the connection must be an authorized, interactive control session —
//     view-only sessions never forward;
//   - the negotiated transport must report SRTP active (a connected but
//     unencrypted session is not a tunnel);
//   - host-side opens are limited to the operator-approved per-device
//     destination port allowlist; default is deny-all.
// Controller-side rules come from explicit user configuration (CLI or
// per-device INI); nothing is ever inferred or broadened.
class PortForwardController {
 public:
  explicit PortForwardController(GuiRuntime& owner);
  ~PortForwardController();

  // Transport callback entry: one frame arrived on the port_forward stream.
  // |props| is the outgoing session when this instance is the controller, or
  // null when an incoming controller sent the frame to us as host.
  void OnFrame(const char* data, std::size_t size, PeerEventHandler* source,
               const std::string& remote_id);

  // Called from the UI tick; pumps every live tunnel endpoint.
  void Poll();

  // Outgoing session lifecycle. Attach lazily creates the client endpoint for
  // a connected, capable, interactive session and applies its configured
  // rules; Detach destroys every listener, stream and buffered byte.
  void DetachClient(const std::string& remote_id);
  // Incoming controller lifecycle.
  void DetachHost(const std::string& remote_id);
  void DetachAll();

  // Live per-device consent. Host endpoints apply the replacement allowlist
  // immediately; ports absent from the list deny new opens. Existing streams
  // to newly-denied ports are closed — consent revocation is effective at
  // once, not only for future connections.
  void SetHostAllowedPorts(const std::string& device_key,
                           const std::set<std::uint16_t>& ports);

  // UI/INI/CLI operator workflow surface. Rules persist per device through
  // the runtime's config path; the controller only consumes them.
  bool AddClientRule(const std::string& remote_id, std::uint16_t local_port,
                     std::uint16_t remote_port, std::string* error);
  bool RemoveClientRule(const std::string& remote_id, std::uint16_t local_port);

  struct SessionSnapshot {
    bool active = false;
    bool host_side = false;
    std::string denial_reason;
    PortForwardSnapshot engine;
  };
  SessionSnapshot Snapshot(const std::string& remote_id) const;

  // Capability advertisement per connection, learned from host_infomation.
  void NotePeerSupport(const std::string& remote_id, bool supported);
  void ForgetPeerSupport(const std::string& remote_id);
  bool PeerSupports(const std::string& remote_id) const;

 private:
  struct HostEndpoint {
    std::unique_ptr<PortForwardHost> engine;
    std::set<std::uint16_t> allowed;
  };
  struct ClientEndpoint {
    std::unique_ptr<PortForwardClient> engine;
    std::map<std::uint16_t, std::uint16_t> rules;  // local -> remote
  };

  // Null when admission fails; the reason is recorded for the snapshot.
  PortForwardClient* EnsureClient(const std::string& remote_id);
  PortForwardHost* EnsureHost(const std::string& remote_id);
  void ApplyConfiguredRules(const std::string& remote_id,
                            ClientEndpoint& endpoint);
  bool ClientAdmitted(const std::string& remote_id);
  bool HostAdmitted(const std::string& remote_id);

  GuiRuntime& owner_;
  // Recursive: runtime admission queries (e.g. PortForwardClientActive ->
  // PeerSupports) re-enter this controller while its own dispatch already
  // holds the lock. Lock ordering with runtime mutexes is strictly
  // port_forward -> runtime, so recursion cannot deadlock across threads.
  mutable std::recursive_mutex mutex_;
  std::map<std::string, ClientEndpoint> clients_;
  std::map<std::string, HostEndpoint> hosts_;
  std::map<std::string, bool> peer_support_;
  std::map<std::string, std::string> denials_;
  bool cli_rules_consumed_ = false;
};

}  // namespace crossdesk

#endif
