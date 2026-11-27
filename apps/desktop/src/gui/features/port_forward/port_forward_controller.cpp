/*
 * Port-forward session wiring: capability gating, consent enforcement,
 * encrypted-channel admission, and per-connection tunnel lifetime.
 *
 * Copyright (c) 2026 Farlink contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "features/port_forward/port_forward_controller.h"

#include <port_forward_wire.h>
#include <stream_names.h>

#include "app/launch_options.h"
#include "rd_log.h"
#include "runtime/gui_runtime.h"
#include "runtime/peer_event_handler.h"

namespace crossdesk {

PortForwardController::PortForwardController(GuiRuntime& owner) : owner_(owner) {}
PortForwardController::~PortForwardController() { DetachAll(); }

void PortForwardController::NotePeerSupport(const std::string& remote_id,
                                            bool supported) {
  std::lock_guard lock(mutex_);
  peer_support_[remote_id] = supported;
}

void PortForwardController::ForgetPeerSupport(const std::string& remote_id) {
  std::lock_guard lock(mutex_);
  peer_support_.erase(remote_id);
}

bool PortForwardController::PeerSupports(const std::string& remote_id) const {
  std::lock_guard lock(mutex_);
  const auto found = peer_support_.find(remote_id);
  return found != peer_support_.end() && found->second;
}

void PortForwardController::RefreshHostConsentForKey(
    const std::string& consent_key) {
  // A persistent-consent edit reaches exactly the live connections whose
  // verified identity matches the edited key. Re-resolution is host-local:
  // each connection's own verified proof decides whether the key applies.
  std::lock_guard lock(mutex_);
  for (auto& [id, endpoint] : hosts_) {
    if (owner_.VerifiedPortForwardConsentKey(id) != consent_key) continue;
    endpoint.allowed = owner_.HostPortForwardAllowlist(id);
    if (endpoint.engine) endpoint.engine->SetAllowedRemotePorts(endpoint.allowed);
  }
}

void PortForwardController::RefreshHostConsent(const std::string& remote_id) {
  // Re-resolve consent for one live connection from host-local facts. Called
  // when the trust proof completes (a verified identity may add persistent
  // consent) or when the operator changes the persistent list. The engine
  // applies the replacement at once: ports absent from the new list deny new
  // opens and close affected live streams — revocation is immediate, not
  // deferred to the next connection.
  std::lock_guard lock(mutex_);
  const auto it = hosts_.find(remote_id);
  if (it == hosts_.end()) return;
  it->second.allowed = owner_.HostPortForwardAllowlist(remote_id);
  if (it->second.engine)
    it->second.engine->SetAllowedRemotePorts(it->second.allowed);
}

PortForwardClient* PortForwardController::EnsureClient(
    const std::string& remote_id) {
  auto found = clients_.find(remote_id);
  if (found != clients_.end()) return found->second.engine.get();

  // Admission happens once at attach time and on every frame through the
  // runtime's session lookup; a session that loses any requirement is
  // detached by the disconnect/reset paths.
  ClientEndpoint endpoint;
  // Parity comes from a fact both peers share: who initiated the connection.
  // Outgoing sessions (we connect out) allocate odd ids; the accepting side
  // of the same connection derives even. Reverse-direction tunnels on the
  // same connection therefore can never collide stream ownership.
  const int parity = owner_.PortForwardConnectionInitiator(remote_id) ? 1 : 0;
  endpoint.engine = std::make_unique<PortForwardClient>(
      [this, remote_id](const char* data, std::size_t size) {
        // MiniRTC sends are thread-safe and never deliver inline on this
        // thread, so invoking the transport while the controller mutex is
        // held cannot re-enter the engine.
        return owner_.SendPortForwardData(remote_id, data, size) == 0;
      },
      parity);
  auto [it, inserted] = clients_.emplace(remote_id, std::move(endpoint));
  if (inserted) ApplyConfiguredRules(remote_id, it->second);
  // Approval or verification can arrive after the endpoint exists (the
  // endpoint is created by incoming handshake traffic before the trust proof
  // completes). Binding is monotonic and one-shot: once qualified, the rules
  // attach exactly once.
  if (!it->second.reverse_rules_applied &&
      !owner_.PortForwardConnectionInitiator(remote_id) &&
      ReverseBindingApproved(remote_id))
    ApplyReverseRules(remote_id, it->second);
  return it->second.engine.get();
}

void PortForwardController::ApplyConfiguredRules(const std::string& remote_id,
                                                 ClientEndpoint& endpoint) {
  // Rule binding is direction-split with no inference between the two:
  //
  //   OUTGOING session (this machine connected out): CLI --port-forward rules
  //   (once, first outgoing connection) plus the per-device INI rules for the
  //   target the operator selected.
  //
  //   INCOMING control session (a controller connected to us): ONLY the
  //   explicit --port-forward-reverse set, and only when this exact
  //   connection independently qualifies through ReverseBindingApproved.
  //   Outgoing rules never apply here — "when I connect to that host" is a
  //   fact about a different connection entirely, and the first arbitrary
  //   incoming device must never inherit intent written for another peer.
  if (!owner_.PortForwardConnectionInitiator(remote_id)) {
    if (ReverseBindingApproved(remote_id)) ApplyReverseRules(remote_id, endpoint);
    return;
  }
  std::string error;
  const auto& launch = GetLaunchOptions();
  if (!launch.port_forward_rules.empty() && !cli_rules_consumed_) {
    cli_rules_consumed_ = true;
    for (const auto& rule : launch.port_forward_rules) {
      const std::uint16_t bound = endpoint.engine->AddRule(
          static_cast<std::uint16_t>(rule.local_port),
          static_cast<std::uint16_t>(rule.remote_port), &error);
      if (bound) {
        endpoint.rules[bound] = static_cast<std::uint16_t>(rule.remote_port);
        LOG_INFO("Port forward: CLI rule 127.0.0.1:{} -> 127.0.0.1:{} active",
                 bound, rule.remote_port);
      } else {
        LOG_WARN("Port forward: CLI rule {}:{} rejected: {}", rule.local_port,
                 rule.remote_port, error);
      }
    }
  }
  const auto device_key = owner_.PortForwardDeviceKey(remote_id);
  if (device_key.empty()) return;
  for (const auto& [local, remote] : owner_.ConfiguredPortForwardRules(device_key)) {
    if (endpoint.rules.count(local)) continue;  // CLI rule already owns it.
    std::string rule_error;
    const std::uint16_t bound =
        endpoint.engine->AddRule(local, remote, &rule_error);
    if (bound) {
      endpoint.rules[bound] = remote;
      LOG_INFO("Port forward: configured rule 127.0.0.1:{} -> 127.0.0.1:{} active",
               bound, remote);
    } else {
      LOG_WARN("Port forward: configured rule {}:{} rejected: {}", local,
               remote, rule_error);
    }
  }
}

void PortForwardController::ApplyReverseRules(const std::string& remote_id,
                                              ClientEndpoint& endpoint) {
  if (endpoint.reverse_rules_applied) return;
  endpoint.reverse_rules_applied = true;
  const auto& launch = GetLaunchOptions();
  std::string error;
  for (const auto& rule : launch.port_forward_reverse_rules) {
    const std::uint16_t bound = endpoint.engine->AddRule(
        static_cast<std::uint16_t>(rule.local_port),
        static_cast<std::uint16_t>(rule.remote_port), &error);
    if (bound) {
      endpoint.rules[bound] = static_cast<std::uint16_t>(rule.remote_port);
      LOG_INFO("Port forward: reverse rule 127.0.0.1:{} -> controller 127.0.0.1:{} active",
               bound, rule.remote_port);
    } else {
      LOG_WARN("Port forward: reverse rule {}:{} rejected: {}", rule.local_port,
               rule.remote_port, error);
    }
  }
}

PortForwardHost* PortForwardController::EnsureHost(const std::string& remote_id) {
  auto found = hosts_.find(remote_id);
  if (found != hosts_.end()) return found->second.engine.get();
  HostEndpoint endpoint;
  endpoint.allowed = owner_.HostPortForwardAllowlist(remote_id);
  endpoint.engine = std::make_unique<PortForwardHost>(
      [this, remote_id](const char* data, std::size_t size) {
        return owner_.SendPortForwardData(remote_id, data, size) == 0;
      });
  endpoint.engine->SetAllowedRemotePorts(endpoint.allowed);
  auto [it, inserted] = hosts_.emplace(remote_id, std::move(endpoint));
  return it->second.engine.get();
}

void PortForwardController::OnFrame(const char* data, std::size_t size,
                                    PeerEventHandler* source,
                                    const std::string& remote_id) {
  std::lock_guard lock(mutex_);
  // Routing is direction-symmetric: both endpoints may exist on one
  // connection. kOpen always belongs to the host endpoint (the side that
  // connects to the destination); handshake replies belong to the client;
  // stream traffic belongs to whichever endpoint owns the stream id — the
  // side that opened the stream knows it, the side that accepted it knows
  // it, and no third state exists. Admission is enforced per role:
  //   - client role: interactive, capable, encrypted outgoing OR incoming
  //     session — forwarding rules are local operator intent either way;
  //   - host role: authorized control session, negotiated SRTP, and the
  //     per-device destination-port consent inside the engine.
  port_forward::Frame frame;
  if (!port_forward::DecodeFrame(data, size, frame)) return;

  const bool client_admitted = ClientAdmitted(remote_id);
  const bool host_admitted = HostAdmitted(remote_id);

  if (frame.kind == port_forward::Kind::kOpen) {
    if (!host_admitted) {
      // Deny visibly without ever instantiating the endpoint: an unauthorized
      // or unencrypted session must not reach the consent check, because
      // consent is an additional restriction inside admission — never a
      // substitute for it. The refusal frame makes the denial visible to the
      // operator on the opening side instead of a silent timeout.
      denials_[remote_id] = "unauthorized or unencrypted session";
      std::string reason = "port forwarding requires an authorized encrypted control session";
      char frame_buffer[port_forward::kHeaderBytes + 128];
      const port_forward::Frame denial{
          port_forward::Kind::kOpenErr, port_forward::kErrorDenied,
          frame.stream_id, reason};
      if (const std::size_t size_out = port_forward::EncodeFrame(
              denial, frame_buffer, sizeof(frame_buffer)))
        owner_.SendPortForwardData(remote_id, frame_buffer, size_out);
      return;
    }
    if (auto* host = EnsureHost(remote_id)) host->OnFrame(data, size);
    return;
  }
  if (frame.kind == port_forward::Kind::kOpenOk ||
      frame.kind == port_forward::Kind::kOpenErr) {
    if (client_admitted)
      if (auto* client = EnsureClient(remote_id)) client->OnFrame(data, size);
    return;
  }
  // Stream traffic: deliver to the endpoint that owns the stream — but only
  // while the owning role is still admitted. Admission is re-evaluated on
  // every frame, not cached at attach time: a session that loses
  // authorization or encryption with data in flight must stop moving bytes
  // on THIS frame, not at the next poll tick. An endpoint whose admission is
  // gone is detached outright, because a half-fed engine holding buffered
  // plaintext is exactly the residue revocation exists to eliminate.
  if (!client_admitted) clients_.erase(remote_id);
  if (!host_admitted) hosts_.erase(remote_id);
  bool owned = false;
  if (client_admitted) {
    if (auto client = clients_.find(remote_id); client != clients_.end()) {
      client->second.engine->OnFrame(data, size);
      owned = true;
    }
  }
  if (host_admitted) {
    if (auto host = hosts_.find(remote_id); host != hosts_.end()) {
      host->second.engine->OnFrame(data, size);
      owned = true;
    }
  }
  if (!owned && !client_admitted && !host_admitted)
    denials_[remote_id] = "unauthorized or unencrypted session";
}

bool PortForwardController::ClientAdmitted(const std::string& remote_id) {
  // Session-level admission for a client endpoint. What the endpoint may
  // then DO is decided separately by the rule-binding qualification in
  // ReverseBindingApproved / ApplyConfiguredRules — admission here only
  // establishes that this connection is allowed to carry tunnel traffic at
  // all:
  //   - outgoing sessions: the session is live, interactive, capable and
  //     encrypted (PortForwardClientActive);
  //   - incoming control sessions: the controller passed the host-local
  //     authorization chain, the transport negotiated SRTP, and the peer
  //     advertised the capability.
  if (owner_.PortForwardClientActive(remote_id)) return true;
  const auto support = peer_support_.find(remote_id);
  return owner_.IsAuthorizedController(remote_id) &&
         owner_.PortForwardSessionSecure(remote_id) &&
         support != peer_support_.end() && support->second;
}

bool PortForwardController::ReverseBindingApproved(const std::string& remote_id) {
  // Reverse rules expose THIS machine's loopback services to whoever is
  // controlling us — strictly more dangerous than outbound rules, so the
  // binding bar is strictly higher than "a connection exists". Exactly two
  // qualifications exist, and neither can be inferred:
  //
  //   1. The controller completed the trusted-device proof on THIS
  //      connection with a key pinned in the local trust store — a
  //      cryptographically authenticated identity the operator already
  //      selected for trust. The verified identity is the selection.
  //
  //   2. The operator explicitly approved this exact live connection through
  //      ApproveReverseBinding — a one-shot, connection-instance-scoped grant
  //      that dies with the connection and is never broadened, persisted or
  //      inherited by any other connection, including a reconnect of the
  //      same device.
  //
  // Outgoing rules, mere authorization, device names and reconnect history
  // all fail this test by design.
  if (!owner_.VerifiedPortForwardConsentKey(remote_id).empty()) return true;
  const auto approved = reverse_approved_.find(remote_id);
  return approved != reverse_approved_.end() && approved->second;
}

void PortForwardController::ApplyVerifiedReverseBinding(
    const std::string& remote_id) {
  std::lock_guard lock(mutex_);
  if (!ReverseBindingApproved(remote_id)) return;
  if (!ClientAdmitted(remote_id)) return;
  // The verified identity itself is the approval — no separate operator
  // action needed for a controller the operator already pinned. Create the
  // endpoint eagerly so reverse listeners exist without waiting for traffic.
  EnsureClient(remote_id);
}

void PortForwardController::ApproveReverseBinding(const std::string& remote_id) {
  std::lock_guard lock(mutex_);
  reverse_approved_[remote_id] = true;
  // Approval takes effect at once when the connection is admitted: bind the
  // reverse rules into the endpoint, creating it if necessary so listeners
  // exist without waiting for peer traffic. An unadmitted connection keeps
  // the approval for its lifetime but binds nothing — the grant can never
  // leak into a session that lost authorization or encryption.
  if (!ClientAdmitted(remote_id)) return;
  EnsureClient(remote_id);
}

bool PortForwardController::HostAdmitted(const std::string& remote_id) {
  // The host endpoint serves opens the PEER initiates toward this machine's
  // loopback. Admission is direction-symmetric but keyed to host-local facts
  // in both session shapes:
  //   - incoming control session: the controller passed the full
  //     password/trust admission chain (IsAuthorizedController);
  //   - outgoing session: we ourselves established the session interactively,
  //     so its connected presence in the session table IS the authorization
  //     fact — the same fact the rest of the app uses to send input and
  //     clipboard. PortForwardClientActive already requires connected +
  //     encrypted + capability; requiring it here keeps the reverse
  //     direction available exactly "where approved" — an interactive,
  //     encrypted, capable session — while consent (resolved host-locally)
  //     remains the independent gate on what the peer may reach.
  return (owner_.IsAuthorizedController(remote_id) ||
          owner_.PortForwardClientActive(remote_id)) &&
         owner_.PortForwardSessionSecure(remote_id);
}

void PortForwardController::Poll() {
  std::lock_guard lock(mutex_);
  // Eagerly bind configured listeners as soon as the session admits them.
  // Waiting for the first peer frame would leave operator-configured rules
  // silently inactive on otherwise idle connections — the listener existing
  // IS the feature. This covers outgoing sessions (CLI/INI rules) and
  // incoming sessions alike: the incoming case binds nothing unless the
  // connection independently qualifies through ReverseBindingApproved, so
  // enumeration here cannot broaden anything — qualification is re-checked
  // inside the binding path itself.
  for (const auto& remote_id : owner_.PortForwardAttachableSessions()) {
    if (clients_.find(remote_id) == clients_.end() &&
        ClientAdmitted(remote_id))
      EnsureClient(remote_id);
  }
  for (const auto& remote_id : owner_.PortForwardReverseCandidates()) {
    if (clients_.find(remote_id) == clients_.end() &&
        ClientAdmitted(remote_id) && ReverseBindingApproved(remote_id))
      EnsureClient(remote_id);
  }
  for (auto it = clients_.begin(); it != clients_.end();) {
    if (!ClientAdmitted(it->first)) {
      it = clients_.erase(it);
      continue;
    }
    it->second.engine->Poll();
    ++it;
  }
  for (auto it = hosts_.begin(); it != hosts_.end();) {
    if (!HostAdmitted(it->first)) {
      it = hosts_.erase(it);
      continue;
    }
    it->second.engine->Poll();
    ++it;
  }
}

void PortForwardController::DetachClient(const std::string& remote_id) {
  std::lock_guard lock(mutex_);
  clients_.erase(remote_id);
  denials_.erase(remote_id);
  // The one-shot approval is scoped to the connection INSTANCE: it dies here
  // with the connection. A reconnect — even of the same physical device — is
  // a new instance requiring its own approval or verified proof.
  reverse_approved_.erase(remote_id);
}

void PortForwardController::DetachHost(const std::string& remote_id) {
  std::lock_guard lock(mutex_);
  hosts_.erase(remote_id);
  denials_.erase(remote_id);
}

void PortForwardController::DetachAll() {
  std::lock_guard lock(mutex_);
  clients_.clear();
  hosts_.clear();
  peer_support_.clear();
  denials_.clear();
}

bool PortForwardController::AddClientRule(const std::string& remote_id,
                                          std::uint16_t local_port,
                                          std::uint16_t remote_port,
                                          std::string* error) {
  if (!remote_port) {
    if (error) *error = "remote port must be nonzero";
    return false;
  }
  std::lock_guard lock(mutex_);
  auto* client = EnsureClient(remote_id);
  if (!client) {
    if (error) *error = "session is not admitted for port forwarding";
    return false;
  }
  const std::uint16_t bound = client->AddRule(local_port, remote_port, error);
  if (!bound) return false;
  clients_[remote_id].rules[bound] = remote_port;
  return true;
}

bool PortForwardController::RemoveClientRule(const std::string& remote_id,
                                             std::uint16_t local_port) {
  std::lock_guard lock(mutex_);
  auto found = clients_.find(remote_id);
  if (found == clients_.end()) return false;
  found->second.rules.erase(local_port);
  return found->second.engine->RemoveRule(local_port);
}

PortForwardController::SessionSnapshot PortForwardController::Snapshot(
    const std::string& remote_id) const {
  std::lock_guard lock(mutex_);
  SessionSnapshot snapshot;
  if (const auto client = clients_.find(remote_id); client != clients_.end()) {
    snapshot.active = true;
    snapshot.host_side = false;
    snapshot.engine = client->second.engine->Snapshot();
  } else if (const auto host = hosts_.find(remote_id); host != hosts_.end()) {
    snapshot.active = true;
    snapshot.host_side = true;
    snapshot.engine = host->second.engine->Snapshot();
  }
  if (const auto denial = denials_.find(remote_id); denial != denials_.end())
    snapshot.denial_reason = denial->second;
  return snapshot;
}

}  // namespace crossdesk
