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

  // Re-resolve one connection's host-side consent from host-local facts
  // (verified trusted-device persistent consent unioned with the operator's
  // session-scoped CLI grant). Called when the trust proof completes or the
  // operator edits the persistent list; revocation applies to live streams.
  void RefreshHostConsent(const std::string& remote_id);
  // Apply a persistent-consent edit to exactly the live connections whose
  // cryptographically verified trusted-device identity is this key.
  void RefreshHostConsentForKey(const std::string& consent_key);

  // One-shot live approval: bind the operator's --port-forward-reverse rules
  // to THIS exact connection instance. The grant dies with the connection —
  // never persisted, never inherited by reconnects or any other device.
  void ApproveReverseBinding(const std::string& remote_id);
  // Called when a controller's trusted-device proof completes: the verified
  // pinned identity is itself the operator's selection, so reverse rules
  // bind without further action. No effect for unverified connections.
  void ApplyVerifiedReverseBinding(const std::string& remote_id);

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
    // Reverse rules bind at most once per endpoint, at approval time — never
    // re-evaluated into existence for a connection that never qualified.
    bool reverse_rules_applied = false;
  };

  // Null when admission fails; the reason is recorded for the snapshot.
  PortForwardClient* EnsureClient(const std::string& remote_id);
  PortForwardHost* EnsureHost(const std::string& remote_id);
  void ApplyConfiguredRules(const std::string& remote_id,
                            ClientEndpoint& endpoint);
  void ApplyReverseRules(const std::string& remote_id,
                         ClientEndpoint& endpoint);
  bool ClientAdmitted(const std::string& remote_id);
  bool HostAdmitted(const std::string& remote_id);
  bool ReverseBindingApproved(const std::string& remote_id);

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
  // One-shot live reverse-binding approvals, keyed by connection instance.
  // Entries die with their connection in DetachClient — they are memory-only
  // operator intent, never persisted and never transferable.
  std::map<std::string, bool> reverse_approved_;
  bool cli_rules_consumed_ = false;
};

}  // namespace crossdesk

#endif
