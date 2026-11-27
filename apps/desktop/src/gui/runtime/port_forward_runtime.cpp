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
  // Consent is keyed by the same device identity the operator workflow uses,
  // so approval survives reconnects but never leaks across devices. Unknown
  // controllers get the empty set: every open is denied visibly.
  if (!config_center_) return {};
  return config_center_->GetPortForwardAllowlist(PortForwardDeviceKey(remote_id));
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

bool GuiRuntime::PortForwardConnectionInitiator(const std::string& remote_id) {
  // Outgoing sessions live in remote_sessions_; incoming controllers do not.
  return FindRemoteSession(remote_id) != nullptr;
}

std::string GuiRuntime::PortForwardDeviceKey(const std::string& remote_id) {
  if (auto props = FindRemoteSession(remote_id); props)
    return props->device_preference_key_;
  // Incoming controllers key by their connection identity; trusted-device
  // keying applies where a verified device exists, falling back to the
  // session id. Either way consent never leaks across devices because the
  // key is always scoped to how the operator approved the device.
  return remote_id;
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
  if (!PortForwardClientActive(remote_id) &&
      !FindRemoteSession(remote_id)) {
    if (error) *error = "no outgoing session for port forwarding";
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
  for (const std::uint16_t port : ports) {
    if (!port) {
      if (error) *error = "port zero is never a valid consent entry";
      return false;
    }
  }
  if (!config_center_ ||
      config_center_->SetPortForwardAllowlist(device_key, ports) != 0) {
    if (error) *error = "could not persist consent";
    return false;
  }
  port_forward_.SetHostAllowedPorts(device_key, ports);
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
