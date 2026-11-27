/*
 * Port-forward runtime glue: transport sends, admission gates, and the
 * per-connection security contract for loopback TCP tunneling.
 *
 * Copyright (c) 2026 Farlink contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include <port_forward_wire.h>
#include <stream_names.h>

#include <shared_mutex>

#include "app/launch_options.h"
#include "rd_log.h"
#include "runtime/gui_runtime.h"
#include "runtime/peer_event_handler.h"
#include "runtime/peer_transport.h"

namespace crossdesk {

int GuiRuntime::SendPortForwardData(const std::string& remote_id,
                                    const char* data, std::size_t size) {
  if (!data || !size || size > port_forward::kHeaderBytes +
                                   port_forward::kMaxPayloadBytes)
    return -1;
  // Outgoing client session: send on that session's own peer. Incoming
  // controller: use the controller routing path, which resolves LAN direct
  // peers and the shared signal peer identically to file transfer.
  if (auto props = FindRemoteSession(remote_id);
      props && props->peer_ && !props->closing_) {
    // Outgoing sessions own their peer (the direct peer for LAN sessions), so
    // address the frame to the exact wire identity like device trust does.
    // The wire id is extracted once: the pointer must outlive the call.
    if (props->peer_events_) {
      const auto wire = props->peer_events_->WireId(remote_id);
      if (!wire.empty())
        return PeerSendReliableDataFrameToPeer(
            props->peer_events_->IsDirect() ? props->peer_events_->DirectPeer()
                                            : props->peer_,
            data, size, kPortForwardStream, wire.data(), wire.size());
    }
    return MiniRtcSendReliableDataFrame(props->peer_, data, size,
                                        kPortForwardStream);
  }
  return SendControllerData(remote_id, data, size, kPortForwardStream);
}

bool GuiRuntime::PortForwardSessionSecure(const std::string& remote_id) {
  // The tunnel carries plaintext application bytes inside whatever the
  // transport negotiated. Forwarding over a connected-but-unencrypted
  // session would silently strip the encryption users expect from the
  // remote session itself, so admission requires the negotiated SRTP state
  // reported by the transport — never the configuration flag, which only
  // expresses intent. srtp_active is published by the transport only after
  // the DTLS handshake completed and SRTP sessions are installed.
  if (auto props = FindRemoteSession(remote_id); props) {
    return props->connection_status_.load() ==
               MiniRtcConnectionStatus::MiniRtcConnectionConnected &&
           props->net_traffic_stats_.Load().srtp_active;
  }
  std::shared_lock lock(connection_status_mutex_);
  const auto found = controller_srtp_active_.find(remote_id);
  return found != controller_srtp_active_.end() && found->second;
}

bool GuiRuntime::PortForwardClientActive(const std::string& remote_id) {
  // A controller-side tunnel endpoint is admitted only while every gate
  // holds: the session is live, the peer advertised the capability, the
  // user granted interactive control (view-only sessions never forward),
  // and the negotiated transport is encrypted.
  auto props = FindRemoteSession(remote_id);
  if (!props || props->closing_) return false;
  if (props->connection_status_.load() !=
      MiniRtcConnectionStatus::MiniRtcConnectionConnected)
    return false;
  if (!props->control_mouse_) return false;
  if (!port_forward_.PeerSupports(remote_id)) return false;
  return PortForwardSessionSecure(remote_id);
}

std::set<std::uint16_t> GuiRuntime::HostPortForwardAllowlist(
    const std::string& remote_id) {
  // Consent resolution uses only host-local facts — never anything the peer
  // claims about itself:
  //
  // 1. Persistent consent (INI) exists ONLY for a cryptographically
  //    authenticated identity: the trusted-device public key whose possession
  //    this exact connection proved by signature over a fresh nonce, and
  //    which is pinned in the local trust store. The key in the INI is
  //    "trusted/<public_key>"; nothing else is ever written or read. LAN
  //    device ids, signal user ids and hostnames are all peer-claimed or
  //    ephemeral and therefore can never name persistent consent.
  //
  // 2. Session consent is explicit local operator intent for this process
  //    lifetime (--port-forward-allow). It is never persisted, so it cannot
  //    outlive the context in which it was granted, and each connection still
  //    faces the full admission chain (authorization + negotiated SRTP)
  //    before this list is even consulted.
  //
  // Both are unioned; the default for every connection is the empty set —
  // deny all, visibly.
  std::set<std::uint16_t> ports = GetLaunchOptions().port_forward_allow;
  if (const auto key = VerifiedPortForwardConsentKey(remote_id);
      !key.empty() && config_center_) {
    const auto persistent = config_center_->GetPortForwardAllowlist(key);
    ports.insert(persistent.begin(), persistent.end());
  }
  return ports;
}

std::string GuiRuntime::VerifiedPortForwardConsentKey(
    const std::string& remote_id) {
  // The only cross-connection identity this host has independently
  // authenticated: a controller that completed the trusted-device proof on
  // THIS connection with a key pinned in the local trust store. Everything
  // else a peer says — its LAN device_id, its signal user id, its hostname —
  // is a claim this host never verified, and the LAN protocol carries no
  // device identity at all (the host itself generates the controller's
  // session id). Consent keyed to any of those would be consent keyed to a
  // stranger's word.
  std::lock_guard lock(controller_trust_mutex_);
  const auto it = controller_trust_.find(remote_id);
  if (it == controller_trust_.end() ||
      !it->second.IsTrusted(trusted_devices_.get()))
    return {};
  return "trusted/" + it->second.verified->public_key;
}

void GuiRuntime::NotePortForwardPeerSupport(const std::string& remote_id,
                                            bool supported) {
  port_forward_.NotePeerSupport(remote_id, supported);
}

void GuiRuntime::HandlePortForwardFrame(const char* data, std::size_t size,
                                        PeerEventHandler* source,
                                        const std::string& remote_id) {
  port_forward_.OnFrame(data, size, source, remote_id);
}

void GuiRuntime::PollPortForward() {
  port_forward_.Poll();
}

void GuiRuntime::ResetPortForwardSession(const std::string& remote_id) {
  port_forward_.DetachClient(remote_id);
  port_forward_.DetachHost(remote_id);
  port_forward_.ForgetPeerSupport(remote_id);
  std::unique_lock lock(connection_status_mutex_);
  controller_srtp_active_.erase(remote_id);
}

std::vector<std::string> GuiRuntime::PortForwardAttachableSessions() {
  std::vector<std::string> ids;
  std::shared_lock lock(remote_sessions_mutex_);
  for (const auto& [id, props] : remote_sessions_) {
    if (props && !props->closing_ &&
        props->connection_status_.load() ==
            MiniRtcConnectionStatus::MiniRtcConnectionConnected)
      ids.push_back(id);
  }
  return ids;
}

std::vector<std::string> GuiRuntime::PortForwardReverseCandidates() {
  // Incoming control sessions eligible for reverse-rule evaluation. The
  // controller's own qualification check (verified proof or explicit live
  // approval) decides what actually binds — this list is only the
  // enumeration of live authorized-capable incoming connections, used so
  // reverse listeners exist without waiting for peer traffic.
  std::vector<std::string> ids;
  std::shared_lock lock(connection_status_mutex_);
  for (const auto& [id, status] : connection_status_) {
    if (status == MiniRtcConnectionStatus::MiniRtcConnectionConnected)
      ids.push_back(id);
  }
  return ids;
}

bool GuiRuntime::PortForwardConnectionInitiator(const std::string& remote_id) {
  // Outgoing sessions live in remote_sessions_; incoming controllers do not.
  return FindRemoteSession(remote_id) != nullptr;
}

std::string GuiRuntime::PortForwardDeviceKey(const std::string& remote_id) {
  // This key names controller-side forwarding RULES only — the operator's own
  // outbound intent ("when I connect to that host, forward these ports"). It
  // is never used for host-side consent: what this machine allows others to
  // reach is resolved exclusively through VerifiedPortForwardConsentKey and
  // explicit session grants. The LAN device id in the preference key is
  // host-claimed, but it arrives over the password-authenticated channel —
  // the shared-secret password IS the LAN authentication baseline, so a
  // peer able to assert the id is already authenticated to the same standard
  // every other LAN feature (input, clipboard, file transfer) relies on.
  // Signal sessions key by server+id the operator explicitly selected.
  if (auto props = FindRemoteSession(remote_id); props)
    return props->device_preference_key_;
  return {};
}

std::map<std::uint16_t, std::uint16_t> GuiRuntime::ConfiguredPortForwardRules(
    const std::string& device_key) {
  return config_center_ ? config_center_->GetPortForwardRules(device_key)
                        : std::map<std::uint16_t, std::uint16_t>{};
}

bool GuiRuntime::AddPortForwardRule(const std::string& remote_id,
                                    std::uint16_t local_port,
                                    std::uint16_t remote_port, bool persist,
                                    std::string* error) {
  if (!remote_port) {
    if (error) *error = "remote port must be nonzero";
    return false;
  }
  // Admission is the actual permission check, not session existence: the
  // session must be live, interactive, capable and encrypted right now. A
  // connected-but-unencrypted or capability-less session rejecting the rule
  // here is what keeps the error visible at the operator's action instead of
  // surfacing later as a silent inactive listener.
  if (!PortForwardClientActive(remote_id)) {
    if (error)
      *error = "session is not admitted for port forwarding (requires an "
               "interactive encrypted session with a capable peer)";
    return false;
  }
  if (!port_forward_.AddClientRule(remote_id, local_port, remote_port, error))
    return false;
  if (persist && config_center_) {
    const auto key = PortForwardDeviceKey(remote_id);
    auto rules = config_center_->GetPortForwardRules(key);
    // Persist the operator-visible local port. Ephemeral CLI rules (local 0)
    // bind a system-chosen port that means nothing across restarts, so only
    // explicitly numbered rules persist.
    if (local_port) {
      rules[local_port] = remote_port;
      if (config_center_->SetPortForwardRules(key, rules) != 0) {
        LOG_WARN("Port forward: rule active but could not be persisted");
      }
    }
  }
  return true;
}

bool GuiRuntime::RemovePortForwardRule(const std::string& remote_id,
                                       std::uint16_t local_port, bool persist) {
  const bool removed = port_forward_.RemoveClientRule(remote_id, local_port);
  if (persist && config_center_) {
    const auto key = PortForwardDeviceKey(remote_id);
    auto rules = config_center_->GetPortForwardRules(key);
    if (rules.erase(local_port))
      config_center_->SetPortForwardRules(key, rules);
  }
  return removed;
}

bool GuiRuntime::SetHostPortForwardConsent(
    const std::string& device_key, const std::set<std::uint16_t>& ports,
    std::string* error) {
  // Persistent consent is accepted ONLY for a cryptographically verified
  // trusted-device key. The caller cannot invent one: the key must carry the
  // "trusted/" prefix and the public key behind it must be pinned in the
  // local trust store right now — writing consent for a key this host has
  // never verified would be persisting approval for a stranger's claim.
  // Session-scoped consent for unverified controllers is the operator's CLI
  // grant (--port-forward-allow), never this persistent path.
  for (const std::uint16_t port : ports) {
    if (!port) {
      if (error) *error = "port zero is never a valid consent entry";
      return false;
    }
  }
  constexpr std::string_view prefix = "trusted/";
  if (device_key.rfind(std::string(prefix), 0) != 0) {
    if (error)
      *error = "persistent consent requires a verified trusted-device key "
               "(trusted/<public_key>); unverified controllers use the "
               "session-scoped --port-forward-allow grant";
    return false;
  }
  const auto public_key = device_key.substr(prefix.size());
  if (!trusted_devices_ || !trusted_devices_->Contains(public_key)) {
    if (error)
      *error = "no trusted device with this public key is pinned locally";
    return false;
  }
  if (!config_center_ ||
      config_center_->SetPortForwardAllowlist(device_key, ports) != 0) {
    if (error) *error = "could not persist consent";
    return false;
  }
  // Apply the replacement to every live connection whose verified identity
  // is this key — and to no other connection.
  port_forward_.RefreshHostConsentForKey(device_key);
  return true;
}

bool GuiRuntime::ApprovePortForwardReverseBinding(const std::string& remote_id,
                                                  std::string* error) {
  // Explicit one-shot approval for reverse forwarding on this exact live
  // connection. This is the operator workflow for controllers WITHOUT a
  // verified trusted-device identity: approve the specific connection you
  // can see in front of you, for its lifetime only. The grant is memory-only
  // — it dies with the connection, is never written to disk, and a reconnect
  // of the same device is a new unapproved instance.
  const bool live = [this, &remote_id] {
    std::shared_lock lock(connection_status_mutex_);
    const auto found = connection_status_.find(remote_id);
    return found != connection_status_.end() &&
           found->second == MiniRtcConnectionStatus::MiniRtcConnectionConnected;
  }();
  if (!live) {
    if (error)
      *error = "no live incoming control connection with this id — approval "
               "binds only to a connection you can verify is the one you mean";
    return false;
  }
  port_forward_.ApproveReverseBinding(remote_id);
  return true;
}

PortForwardController::SessionSnapshot GuiRuntime::PortForwardSnapshot(
    const std::string& remote_id) const {
  return port_forward_.Snapshot(remote_id);
}

void GuiRuntime::NoteControllerSrtp(const std::string& remote_id,
                                    bool srtp_active) {
  const bool was_active = [this, &remote_id] {
    std::shared_lock lock(connection_status_mutex_);
    const auto found = controller_srtp_active_.find(remote_id);
    return found != controller_srtp_active_.end() && found->second;
  }();
  {
    std::unique_lock lock(connection_status_mutex_);
    if (srtp_active) {
      controller_srtp_active_[remote_id] = true;
    } else {
      controller_srtp_active_.erase(remote_id);
    }
  }
  // Losing encryption mid-session revokes admission immediately: the host
  // endpoint is torn down rather than continuing to proxy bytes over a
  // channel whose protection can no longer be verified.
  if (was_active && !srtp_active) port_forward_.DetachHost(remote_id);
}

}  // namespace crossdesk
