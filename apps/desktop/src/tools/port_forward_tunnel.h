/*
 * Transport-agnostic encrypted TCP port forwarding engine.
 *
 * Copyright (c) 2026 Farlink contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef _PORT_FORWARD_TUNNEL_H_
#define _PORT_FORWARD_TUNNEL_H_

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace crossdesk {

// Synchronous reliable ordered frame sender. The callee must copy any payload
// it retains; returning false means the transport did not accept this frame.
using SendFrameFn = std::function<bool(const char*, std::size_t)>;

struct PortForwardRule {
  std::uint16_t local_port = 0;
  std::uint16_t remote_port = 0;
};

struct PortForwardSnapshot {
  std::vector<PortForwardRule> rules;
  std::size_t open_streams = 0;
  std::uint64_t bytes_in = 0;
  std::uint64_t bytes_out = 0;
  std::string last_error;
};

inline constexpr std::size_t kPortForwardMaxStreams = 64;
inline constexpr std::size_t kPortForwardMaxRules = 32;
inline constexpr std::size_t kPortForwardStreamBufferBytes = 256 * 1024;
inline constexpr std::size_t kPortForwardIoChunkBytes = 16 * 1024;
inline constexpr std::chrono::milliseconds kPortForwardConnectTimeout{3000};

namespace detail {
class SocketPump;
}

// Controller side. Instances are externally serialized: Poll(), OnFrame(),
// AddRule(), RemoveRule() and StopAll() must not execute concurrently with one
// another. Production wiring serializes Poll with MiniRTC callbacks; tests use
// a synchronous in-memory transport. SendFrameFn may be invoked inline by
// either direction and therefore must not recursively enter this object.
class PortForwardClient {
 public:
  // |id_parity| partitions the stream-id space when both directions of one
  // connection forward concurrently: one side allocates odd ids, the other
  // even. Both endpoints compute parity from the same connection fact (who
  // initiated the session), so colliding ownership is impossible. 0 = even,
  // 1 = odd; single-direction tunnels are unaffected.
  explicit PortForwardClient(SendFrameFn send, int id_parity = 1);
  ~PortForwardClient();

  PortForwardClient(const PortForwardClient&) = delete;
  PortForwardClient& operator=(const PortForwardClient&) = delete;

  // Binds a loopback-only TCP listener. local_port zero selects an ephemeral
  // port and the bound port is returned. A duplicate configured local port or
  // an OS port collision is reported through error and yields zero.
  std::uint16_t AddRule(std::uint16_t local_port, std::uint16_t remote_port,
                        std::string* error = nullptr);
  bool RemoveRule(std::uint16_t local_port);
  // Closes listeners and active sockets, best-effort sending kClose first.
  void StopAll();
  void Poll();
  void OnFrame(const char* data, std::size_t size);
  PortForwardSnapshot Snapshot() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Host side. The same external-serialization contract applies. Consent is a
// process-local explicit destination-port allowlist; absent consent denies
// every open visibly with kErrorDenied.
class PortForwardHost {
 public:
  explicit PortForwardHost(SendFrameFn send);
  ~PortForwardHost();

  PortForwardHost(const PortForwardHost&) = delete;
  PortForwardHost& operator=(const PortForwardHost&) = delete;

  void SetAllowedRemotePorts(std::set<std::uint16_t> ports);
  bool IsRemotePortAllowed(std::uint16_t port) const;
  void Reset();
  void Poll();
  void OnFrame(const char* data, std::size_t size);
  PortForwardSnapshot Snapshot() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace crossdesk

#endif
