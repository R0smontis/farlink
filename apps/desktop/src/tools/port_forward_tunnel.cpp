/*
 * Transport-agnostic encrypted TCP port forwarding engine.
 *
 * Copyright (c) 2026 Farlink contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "port_forward_tunnel.h"

#include <port_forward_wire.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <deque>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace crossdesk {
namespace {

namespace wire = port_forward;

#ifdef _WIN32
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;

int CloseSocket(SocketHandle socket) { return closesocket(socket); }
int LastSocketError() { return WSAGetLastError(); }
bool WouldBlock(int error) { return error == WSAEWOULDBLOCK; }
bool ConnectInProgress(int error) {
  return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS || error == WSAEALREADY;
}
bool AddressInUse(int error) { return error == WSAEADDRINUSE; }
bool SetNonBlocking(SocketHandle socket) {
  u_long enabled = 1;
  return ioctlsocket(socket, FIONBIO, &enabled) == 0;
}
bool SetNoSigpipe(SocketHandle) { return true; }
int SendSocket(SocketHandle socket, const char* data, std::size_t size) {
  return send(socket, data, static_cast<int>(size), 0);
}
using ReceiveResult = int;
ReceiveResult ReceiveSocket(SocketHandle socket, char* data, std::size_t size) {
  return recv(socket, data, static_cast<int>(size), 0);
}
#else
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;

int CloseSocket(SocketHandle socket) { return close(socket); }
int LastSocketError() { return errno; }
bool WouldBlock(int error) { return error == EAGAIN || error == EWOULDBLOCK; }
bool ConnectInProgress(int error) { return error == EINPROGRESS; }
bool AddressInUse(int error) { return error == EADDRINUSE; }
bool SetNonBlocking(SocketHandle socket) {
  const int flags = fcntl(socket, F_GETFL, 0);
  return flags >= 0 && fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
}
bool SetNoSigpipe(SocketHandle socket) {
#ifdef SO_NOSIGPIPE
  int enabled = 1;
  return setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) == 0;
#else
  (void)socket;
  return true;
#endif
}
int SendSocket(SocketHandle socket, const char* data, std::size_t size) {
#ifdef MSG_NOSIGNAL
  return static_cast<int>(send(socket, data, size, MSG_NOSIGNAL));
#else
  return static_cast<int>(send(socket, data, size, 0));
#endif
}
using ReceiveResult = ssize_t;
ReceiveResult ReceiveSocket(SocketHandle socket, char* data, std::size_t size) {
  return recv(socket, data, size, 0);
}
#endif

void CloseIfValid(SocketHandle& socket) {
  if (socket != kInvalidSocket) {
    CloseSocket(socket);
    socket = kInvalidSocket;
  }
}

bool ConfigureStreamSocket(SocketHandle socket) {
  int enabled = 1;
  setsockopt(socket, IPPROTO_TCP, TCP_NODELAY,
             reinterpret_cast<const char*>(&enabled), sizeof(enabled));
  return SetNonBlocking(socket) && SetNoSigpipe(socket);
}

bool EncodeAndSend(const SendFrameFn& send, wire::Kind kind, std::uint16_t flags,
                   std::uint32_t stream_id, std::string_view payload = {}) {
  if (!send) return false;
  char storage[wire::kHeaderBytes + wire::kMaxPayloadBytes];
  const wire::Frame frame{kind, flags, stream_id, payload};
  const std::size_t size = wire::EncodeFrame(frame, storage, sizeof(storage));
  return size != 0 && send(storage, size);
}

// Bounded byte queue for socket-bound payload. The cap is one full credit
// window plus a final chunk: a correct peer can never have more than that in
// flight, so reaching the cap means the peer violated the window contract —
// or the socket consumer is fully stalled, which is exactly the backpressure
// state the window exists to create. Push beyond the cap fails and the
// caller tears the stream down rather than growing memory without limit.
constexpr std::size_t kEgressCapBytes =
    wire::kStreamWindowBytes + wire::kMaxPayloadBytes;

class BoundedQueue {
 public:
  bool Push(const char* data, std::size_t size) {
    if (!size) return true;
    if (!data || bytes_ + size > kEgressCapBytes) return false;
    segments_.emplace_back(data, size);
    bytes_ += size;
    return true;
  }
  bool Push(std::string_view data) { return Push(data.data(), data.size()); }
  std::size_t bytes() const { return bytes_; }
  bool empty() const { return bytes_ == 0; }

  // Copies up to |maximum| bytes into |out| without removing them; returns
  // the contiguous prefix byte count available (which may exceed the first
  // segment because bytes are copied across segment boundaries).
  std::size_t Peek(char* out, std::size_t maximum) const {
    std::size_t copied = 0;
    for (const auto& segment : segments_) {
      if (copied >= maximum) break;
      const std::size_t count = std::min(maximum - copied, segment.size());
      std::memcpy(out + copied, segment.data(), count);
      copied += count;
    }
    return copied;
  }

  void Pop(std::size_t count) {
    count = std::min(count, bytes_);
    while (count && !segments_.empty()) {
      auto& first = segments_.front();
      if (count >= first.size()) {
        count -= first.size();
        bytes_ -= first.size();
        segments_.pop_front();
      } else {
        first.erase(0, count);
        bytes_ -= count;
        count = 0;
      }
    }
  }

 private:
  std::deque<std::string> segments_;
  std::size_t bytes_ = 0;
};

// Direction-agnostic half of one proxied TCP connection. Terminology:
//   socket -> wire  = "ingress" (local bytes enter the tunnel)
//   wire  -> socket = "egress"  (remote bytes leave the tunnel)
struct StreamPump {
  std::uint32_t id = 0;
  std::uint16_t local_port = 0;    // client: listener port
  std::uint16_t remote_port = 0;   // destination port on the host
  SocketHandle socket = kInvalidSocket;
  bool established = false;        // open handshake completed
  bool connecting = false;         // host: nonblocking connect in flight
  bool ingress_eof_signaled = false;   // socket read hit EOF, kEof sent
  bool egress_eof = false;             // peer sent kEof: no more wire data
  bool egress_write_shutdown = false;  // shutdown(SHUT_WR) done after drain
  bool close_sent = false;
  bool dead = false;               // fatal socket error; close at next sweep
  std::chrono::steady_clock::time_point connect_started{};
  BoundedQueue egress;             // wire bytes waiting for the socket
  std::size_t ingress_in_flight = 0;   // unacknowledged kData bytes at transport
  std::uint64_t ingress_bytes = 0;
  std::uint64_t egress_bytes = 0;
};

// Shared pump logic for both tunnel endpoints. The derived endpoint supplies
// the open handshake; everything after establishment is symmetric:
//   - ingress: read socket while the peer's credit window permits, chunked
//     to kPortForwardIoChunkBytes, decrementing the window per kData frame;
//   - egress: write queued wire bytes to the socket, returning kWindow
//     credit for every byte consumed;
//   - half-close: socket EOF -> kEof; peer kEof -> drain then SHUT_WR.
class PumpCore {
 public:
  PumpCore() = default;

  bool SendFrame(wire::Kind kind, std::uint16_t flags, std::uint32_t stream_id,
                 std::string_view payload = {}) {
    return EncodeAndSend(send_, kind, flags, stream_id, payload);
  }

  void SendClose(StreamPump& stream) {
    if (!stream.close_sent) {
      SendFrame(wire::Kind::kClose, 0, stream.id);
      stream.close_sent = true;
    }
  }

  void SendWindowCredit(StreamPump& stream, std::uint32_t bytes) {
    if (!bytes) return;
    char payload[4];
    wire::WriteCredit(payload, bytes);
    SendFrame(wire::Kind::kWindow, 0, stream.id, std::string_view(payload, 4));
  }

  // Ingress: pull socket bytes while credit remains. A full read chunk per
  // frame keeps wire overhead at 16 bytes per 16 KiB.
  void PumpIngress(StreamPump& stream, std::uint64_t& bytes_in_counter) {
    if (stream.ingress_eof_signaled || stream.socket == kInvalidSocket ||
        stream.connecting || stream.dead)
      return;
    while (stream.ingress_in_flight < wire::kStreamWindowBytes) {
      const std::size_t credit =
          wire::kStreamWindowBytes - stream.ingress_in_flight;
      const std::size_t want = std::min<std::size_t>(kPortForwardIoChunkBytes, credit);
      char buffer[kPortForwardIoChunkBytes];
      const ReceiveResult received =
          ReceiveSocket(stream.socket, buffer, std::min(want, sizeof(buffer)));
      if (received > 0) {
        const auto count = static_cast<std::size_t>(received);
        if (!SendFrame(wire::Kind::kData, 0, stream.id,
                       std::string_view(buffer, count))) {
          // recv already consumed these bytes, so a transport refusal cannot
          // be replayed: continuing would silently corrupt the byte stream.
          // Tear the stream down instead of forwarding with a gap.
          SetError("transport rejected forwarding data frame");
          stream.dead = true;
          return;
        }
        stream.ingress_in_flight += count;
        stream.ingress_bytes += count;
        bytes_in_counter += count;
        if (count < sizeof(buffer)) return;  // drained for now
        continue;
      }
      if (received == 0) {
        stream.ingress_eof_signaled = true;
        SendFrame(wire::Kind::kEof, 0, stream.id);
        return;
      }
      const int error = LastSocketError();
      if (!WouldBlock(error)) {
        stream.ingress_eof_signaled = true;
        stream.dead = true;
      }
      return;
    }
  }

  // Egress: flush queued wire bytes to the socket, returning credit for every
  // consumed byte so the peer's send window reopens.
  void PumpEgress(StreamPump& stream, std::uint64_t& bytes_out_counter) {
    if (stream.socket == kInvalidSocket || stream.connecting || stream.dead)
      return;
    while (!stream.egress.empty()) {
      char buffer[kPortForwardIoChunkBytes];
      const std::size_t available = stream.egress.Peek(buffer, sizeof(buffer));
      if (!available) break;
      const int written = SendSocket(stream.socket, buffer, available);
      if (written > 0) {
        const auto count = static_cast<std::size_t>(written);
        stream.egress.Pop(count);
        stream.egress_bytes += count;
        bytes_out_counter += count;
        SendWindowCredit(stream, static_cast<std::uint32_t>(count));
        if (count < available) break;
        continue;
      }
      if (written == 0) break;
      const int error = LastSocketError();
      if (!WouldBlock(error)) {
        stream.dead = true;
      }
      break;
    }
    if (stream.egress_eof && stream.egress.empty() &&
        !stream.egress_write_shutdown && stream.socket != kInvalidSocket) {
      shutdown(stream.socket, SHUT_WR);
      stream.egress_write_shutdown = true;
    }
  }

  // A stream fully terminates when both directions are done: ingress EOF has
  // been signaled, the peer has EOFed egress, and everything is drained. Any
  // fatal socket error terminates immediately with kClose.
  bool Finished(const StreamPump& stream) {
    if (stream.dead) return true;
    return stream.ingress_eof_signaled && stream.egress_eof &&
           stream.egress.empty();
  }

  void ApplyDataFrame(StreamPump& stream, std::string_view payload) {
    if (!stream.egress.Push(payload)) {
      SetError("forwarded stream receive buffer exhausted");
      stream.dead = true;
    }
  }

  void ApplyWindowFrame(StreamPump& stream, std::string_view payload) {
    const std::uint32_t credit = wire::ReadCredit(payload);
    // Credits can legitimately exceed the in-flight estimate when frames
    // cross with close propagation; clamp instead of overflowing.
    if (credit >= stream.ingress_in_flight) {
      stream.ingress_in_flight = 0;
    } else {
      stream.ingress_in_flight -= credit;
    }
  }

  void SetError(const std::string& value) { last_error = value; }

  SendFrameFn send_;
  std::string last_error;
};

}  // namespace

struct PortForwardClient::Impl : PumpCore {
  struct RuleState {
    std::uint16_t local_port = 0;
    std::uint16_t remote_port = 0;
    SocketHandle listener = kInvalidSocket;
  };

  explicit Impl(SendFrameFn send_fn) { send_ = std::move(send_fn); }
  ~Impl() { StopAll(); }

  void StopAll() {
    stopping = true;
    for (auto& [port, rule] : rules) CloseIfValid(rule.listener);
    rules.clear();
    for (auto& [id, stream] : streams) {
      SendClose(stream);
      CloseIfValid(stream.socket);
    }
    streams.clear();
  }

  std::uint16_t AddRule(std::uint16_t local_port, std::uint16_t remote_port,
                        std::string* error) {
    const auto fail = [&](const std::string& message) {
      if (error) *error = message;
      SetError(message);
      return std::uint16_t{0};
    };
    if (!remote_port) return fail("remote port must be nonzero");
    if (rules.size() >= kPortForwardMaxRules &&
        (local_port == 0 || rules.find(local_port) == rules.end()))
      return fail("too many forwarding rules");
    if (local_port && rules.find(local_port) != rules.end())
      return fail("local port already has a forwarding rule");

    SocketHandle listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == kInvalidSocket) return fail("cannot create loopback listener socket");
    int enabled = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&enabled), sizeof(enabled));
    if (!SetNonBlocking(listener)) {
      CloseSocket(listener);
      return fail("cannot make listener nonblocking");
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(local_port);
    if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
      const int socket_error = LastSocketError();
      CloseSocket(listener);
      return fail(AddressInUse(socket_error) ? "local port is already in use"
                                             : "cannot bind loopback listener");
    }
    if (listen(listener, 16) != 0) {
      CloseSocket(listener);
      return fail("cannot listen on loopback socket");
    }
    sockaddr_in bound{};
#ifdef _WIN32
    int bound_size = sizeof(bound);
#else
    socklen_t bound_size = sizeof(bound);
#endif
    if (getsockname(listener, reinterpret_cast<sockaddr*>(&bound), &bound_size) != 0) {
      CloseSocket(listener);
      return fail("cannot query bound listener port");
    }
    const std::uint16_t bound_port = ntohs(bound.sin_port);
    if (!bound_port) {
      CloseSocket(listener);
      return fail("loopback listener bound invalid port");
    }
    if (rules.find(bound_port) != rules.end()) {
      CloseSocket(listener);
      return fail("local port already has a forwarding rule");
    }
    RuleState rule;
    rule.local_port = bound_port;
    rule.remote_port = remote_port;
    rule.listener = listener;
    rules.emplace(bound_port, rule);
    return bound_port;
  }

  bool RemoveRule(std::uint16_t local_port) {
    const auto rule = rules.find(local_port);
    if (rule == rules.end()) return false;
    CloseIfValid(rule->second.listener);
    rules.erase(rule);
    for (auto it = streams.begin(); it != streams.end();) {
      if (it->second.local_port == local_port) {
        SendClose(it->second);
        CloseIfValid(it->second.socket);
        it = streams.erase(it);
      } else {
        ++it;
      }
    }
    return true;
  }

  void AcceptRule(RuleState& rule) {
    for (;;) {
      sockaddr_in peer{};
#ifdef _WIN32
      int peer_size = sizeof(peer);
#else
      socklen_t peer_size = sizeof(peer);
#endif
      SocketHandle accepted =
          accept(rule.listener, reinterpret_cast<sockaddr*>(&peer), &peer_size);
      if (accepted == kInvalidSocket) return;
      if (ntohl(peer.sin_addr.s_addr) != INADDR_LOOPBACK) {
        CloseSocket(accepted);
        SetError("rejected non-loopback local peer");
        continue;
      }
      if (streams.size() >= kPortForwardMaxStreams) {
        CloseSocket(accepted);
        SetError("maximum active forwarded streams reached");
        continue;
      }
      if (!ConfigureStreamSocket(accepted)) {
        CloseSocket(accepted);
        continue;
      }
      const std::uint32_t id = AllocateId();
      if (!id) {
        CloseSocket(accepted);
        SetError("no forwarding stream identifiers available");
        continue;
      }
      StreamPump stream;
      stream.id = id;
      stream.local_port = rule.local_port;
      stream.remote_port = rule.remote_port;
      stream.socket = accepted;
      if (!SendFrame(wire::Kind::kOpen, rule.remote_port, id)) {
        CloseSocket(accepted);
        SetError("transport rejected forwarding open frame");
        continue;
      }
      streams.emplace(id, std::move(stream));
    }
  }

  std::uint32_t AllocateId() {
    for (std::uint32_t attempts = 0; attempts < 0x7fffff00u; ++attempts) {
      const std::uint32_t candidate = next_stream_id;
      next_stream_id += 2;  // stay inside this endpoint's parity class
      if (next_stream_id == 0 || next_stream_id == 1)
        next_stream_id = first_stream_id;
      if (candidate && streams.find(candidate) == streams.end()) return candidate;
    }
    return 0;
  }

  void OnFrame(const char* data, std::size_t size) {
    wire::Frame frame;
    if (!wire::DecodeFrame(data, size, frame)) {
      SetError("dropped malformed forwarding frame");
      return;
    }
    auto found = streams.find(frame.stream_id);
    if (found == streams.end()) return;
    StreamPump& stream = found->second;
    switch (frame.kind) {
      case wire::Kind::kOpen:
        SetError("dropped host-only forwarding frame");
        return;
      case wire::Kind::kOpenOk:
        stream.established = true;
        return;
      case wire::Kind::kOpenErr:
        SetError(frame.payload.empty()
                     ? "remote forwarding open denied"
                     : "remote forwarding open denied: " + std::string(frame.payload));
        CloseIfValid(stream.socket);
        streams.erase(found);
        return;
      case wire::Kind::kData:
        ApplyDataFrame(stream, frame.payload);
        return;
      case wire::Kind::kWindow:
        ApplyWindowFrame(stream, frame.payload);
        return;
      case wire::Kind::kEof:
        stream.egress_eof = true;
        return;
      case wire::Kind::kClose:
        CloseIfValid(stream.socket);
        streams.erase(found);
        return;
    }
  }

  void Poll() {
    if (stopping) return;
    for (auto& [port, rule] : rules) AcceptRule(rule);
    // Snapshot ids because frame sends during pumping can synchronously
    // deliver peer frames through the in-memory test transport, which must
    // never invalidate the iteration.
    std::vector<std::uint32_t> ids;
    ids.reserve(streams.size());
    for (const auto& [id, stream] : streams) ids.push_back(id);
    for (const std::uint32_t id : ids) {
      auto found = streams.find(id);
      if (found == streams.end()) continue;
      PumpEgress(found->second, bytes_out);
    }
    for (const std::uint32_t id : ids) {
      auto found = streams.find(id);
      if (found == streams.end()) continue;
      // Ingress only after establishment: pre-open socket bytes would race
      // the host's connect and violate ordering guarantees on the first
      // byte the remote service sees.
      if (found->second.established) PumpIngress(found->second, bytes_in);
    }
    for (auto it = streams.begin(); it != streams.end();) {
      if (Finished(it->second)) {
        SendClose(it->second);
        CloseIfValid(it->second.socket);
        it = streams.erase(it);
      } else {
        ++it;
      }
    }
  }

  PortForwardSnapshot Snapshot() const {
    PortForwardSnapshot snapshot;
    snapshot.rules.reserve(rules.size());
    for (const auto& [port, rule] : rules)
      snapshot.rules.push_back({rule.local_port, rule.remote_port});
    snapshot.open_streams = streams.size();
    snapshot.bytes_in = bytes_in;
    snapshot.bytes_out = bytes_out;
    snapshot.last_error = last_error;
    return snapshot;
  }

  std::map<std::uint16_t, RuleState> rules;
  std::map<std::uint32_t, StreamPump> streams;
  std::uint32_t first_stream_id = 1;
  std::uint32_t next_stream_id = 1;
  std::uint64_t bytes_in = 0;
  std::uint64_t bytes_out = 0;
  bool stopping = false;
};

struct PortForwardHost::Impl : PumpCore {
  explicit Impl(SendFrameFn send_fn) { send_ = std::move(send_fn); }
  ~Impl() { Reset(); }

  void Reset() {
    for (auto& [id, stream] : streams) CloseIfValid(stream.socket);
    streams.clear();
  }

  void SendOpenError(std::uint32_t id, std::uint16_t code, std::string_view reason) {
    SendFrame(wire::Kind::kOpenErr, code, id, reason);
    SetError(std::string(reason));
  }

  void OpenStream(std::uint32_t id, std::uint16_t remote_port) {
    if (streams.find(id) != streams.end()) {
      SendOpenError(id, wire::kErrorProtocol, "duplicate forwarding stream id");
      return;
    }
    if (!remote_port) {
      SendOpenError(id, wire::kErrorProtocol, "destination port is invalid");
      return;
    }
    if (allowed_ports.find(remote_port) == allowed_ports.end()) {
      SendOpenError(id, wire::kErrorDenied,
                    "destination port is not approved on the host");
      return;
    }
    if (streams.size() >= kPortForwardMaxStreams) {
      SendOpenError(id, wire::kErrorCapacity,
                    "maximum active forwarded streams reached");
      return;
    }
    SocketHandle socket_handle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket_handle == kInvalidSocket || !ConfigureStreamSocket(socket_handle)) {
      if (socket_handle != kInvalidSocket) CloseSocket(socket_handle);
      SendOpenError(id, wire::kErrorRefused, "cannot create loopback destination socket");
      return;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(remote_port);
    const int result =
        connect(socket_handle, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    StreamPump stream;
    stream.id = id;
    stream.remote_port = remote_port;
    stream.socket = socket_handle;
    stream.connect_started = std::chrono::steady_clock::now();
    if (result == 0) {
      stream.connecting = false;
    } else {
      const int error = LastSocketError();
      if (!ConnectInProgress(error)) {
        CloseSocket(socket_handle);
        SendOpenError(id, wire::kErrorRefused, "destination connection refused");
        return;
      }
      stream.connecting = true;
    }
    if (!stream.connecting) {
      if (!SendFrame(wire::Kind::kOpenOk, 0, id)) {
        CloseSocket(socket_handle);
        SetError("transport rejected forwarding open acknowledgement");
        return;
      }
      stream.established = true;
    }
    streams.emplace(id, std::move(stream));
  }

  void FinishConnect(StreamPump& stream) {
    // A nonblocking connect is complete only when the error state is known:
    // SO_ERROR reports 0 both for a successful connect AND while the attempt
    // is still in flight, so it cannot distinguish the two. getpeername
    // succeeds exactly when the connection is established; ENOTCONN means
    // still in flight. Anything in SO_ERROR after that is a real failure.
    int socket_error = 0;
#ifdef _WIN32
    int length = sizeof(socket_error);
#else
    socklen_t length = sizeof(socket_error);
#endif
    if (getsockopt(stream.socket, SOL_SOCKET, SO_ERROR,
                   reinterpret_cast<char*>(&socket_error), &length) != 0) {
      socket_error = LastSocketError();
    }
    if (socket_error != 0) {
      const std::uint32_t id = stream.id;
      CloseIfValid(stream.socket);
      streams.erase(id);
      SendOpenError(id, wire::kErrorRefused, "destination connection refused");
      return;
    }
    sockaddr_in peer{};
#ifdef _WIN32
    int peer_size = sizeof(peer);
#else
    socklen_t peer_size = sizeof(peer);
#endif
    if (getpeername(stream.socket, reinterpret_cast<sockaddr*>(&peer),
                    &peer_size) != 0) {
      // Not yet connected: the timeout sweep in Poll bounds the wait.
      return;
    }
    stream.connecting = false;
    stream.established = true;
    if (!SendFrame(wire::Kind::kOpenOk, 0, stream.id)) {
      SetError("transport rejected forwarding open acknowledgement");
      stream.dead = true;
    }
  }

  void OnFrame(const char* data, std::size_t size) {
    wire::Frame frame;
    if (!wire::DecodeFrame(data, size, frame)) {
      SetError("dropped malformed forwarding frame");
      return;
    }
    if (frame.kind == wire::Kind::kOpen) {
      OpenStream(frame.stream_id, frame.flags);
      return;
    }
    auto found = streams.find(frame.stream_id);
    if (found == streams.end()) return;
    StreamPump& stream = found->second;
    switch (frame.kind) {
      case wire::Kind::kOpen:
        return;
      case wire::Kind::kOpenOk:
      case wire::Kind::kOpenErr:
        SetError("dropped controller-only forwarding frame");
        return;
      case wire::Kind::kData:
        ApplyDataFrame(stream, frame.payload);
        return;
      case wire::Kind::kWindow:
        ApplyWindowFrame(stream, frame.payload);
        return;
      case wire::Kind::kEof:
        stream.egress_eof = true;
        return;
      case wire::Kind::kClose:
        CloseIfValid(stream.socket);
        streams.erase(found);
        return;
    }
  }

  void Poll() {
    std::vector<std::uint32_t> ids;
    ids.reserve(streams.size());
    for (const auto& [id, stream] : streams) ids.push_back(id);
    const auto now = std::chrono::steady_clock::now();
    for (const std::uint32_t id : ids) {
      auto found = streams.find(id);
      if (found == streams.end()) continue;
      StreamPump& stream = found->second;
      if (stream.connecting &&
          now - stream.connect_started > kPortForwardConnectTimeout) {
        const std::uint32_t timeout_id = stream.id;
        CloseIfValid(stream.socket);
        streams.erase(found);
        SendOpenError(timeout_id, wire::kErrorTimeout,
                      "destination connection timed out");
        continue;
      }
      if (stream.connecting) FinishConnect(stream);
    }
    ids.clear();
    for (const auto& [id, stream] : streams) ids.push_back(id);
    for (const std::uint32_t id : ids) {
      auto found = streams.find(id);
      if (found == streams.end()) continue;
      PumpEgress(found->second, bytes_out);
    }
    for (const std::uint32_t id : ids) {
      auto found = streams.find(id);
      if (found == streams.end()) continue;
      if (found->second.established) PumpIngress(found->second, bytes_in);
    }
    for (auto it = streams.begin(); it != streams.end();) {
      if (Finished(it->second)) {
        SendClose(it->second);
        CloseIfValid(it->second.socket);
        it = streams.erase(it);
      } else {
        ++it;
      }
    }
  }

  PortForwardSnapshot Snapshot() const {
    PortForwardSnapshot snapshot;
    snapshot.rules.reserve(allowed_ports.size());
    for (const std::uint16_t port : allowed_ports)
      snapshot.rules.push_back({0, port});
    snapshot.open_streams = streams.size();
    snapshot.bytes_in = bytes_in;
    snapshot.bytes_out = bytes_out;
    snapshot.last_error = last_error;
    return snapshot;
  }

  std::set<std::uint16_t> allowed_ports;
  std::map<std::uint32_t, StreamPump> streams;
  std::uint64_t bytes_in = 0;
  std::uint64_t bytes_out = 0;
};

PortForwardClient::PortForwardClient(SendFrameFn send, int id_parity)
    : impl_(std::make_unique<Impl>(std::move(send))) {
  impl_->first_stream_id = id_parity ? 1u : 2u;
  impl_->next_stream_id = impl_->first_stream_id;
}
PortForwardClient::~PortForwardClient() = default;

std::uint16_t PortForwardClient::AddRule(std::uint16_t local_port,
                                         std::uint16_t remote_port,
                                         std::string* error) {
  return impl_->AddRule(local_port, remote_port, error);
}

bool PortForwardClient::RemoveRule(std::uint16_t local_port) {
  return impl_->RemoveRule(local_port);
}

void PortForwardClient::StopAll() { impl_->StopAll(); }
void PortForwardClient::Poll() { impl_->Poll(); }
void PortForwardClient::OnFrame(const char* data, std::size_t size) {
  impl_->OnFrame(data, size);
}
PortForwardSnapshot PortForwardClient::Snapshot() const { return impl_->Snapshot(); }

PortForwardHost::PortForwardHost(SendFrameFn send)
    : impl_(std::make_unique<Impl>(std::move(send))) {}
PortForwardHost::~PortForwardHost() = default;

void PortForwardHost::SetAllowedRemotePorts(std::set<std::uint16_t> ports) {
  ports.erase(0);
  impl_->allowed_ports = std::move(ports);
  // Revocation is effective immediately: streams whose destination port is no
  // longer consented close now rather than living out the session on stale
  // approval. The peer learns through kClose like any other teardown.
  for (auto it = impl_->streams.begin(); it != impl_->streams.end();) {
    if (impl_->allowed_ports.find(it->second.remote_port) ==
        impl_->allowed_ports.end()) {
      impl_->SendClose(it->second);
      CloseIfValid(it->second.socket);
      it = impl_->streams.erase(it);
    } else {
      ++it;
    }
  }
}

bool PortForwardHost::IsRemotePortAllowed(std::uint16_t port) const {
  return port != 0 && impl_->allowed_ports.find(port) != impl_->allowed_ports.end();
}

void PortForwardHost::Reset() { impl_->Reset(); }
void PortForwardHost::Poll() { impl_->Poll(); }
void PortForwardHost::OnFrame(const char* data, std::size_t size) {
  impl_->OnFrame(data, size);
}
PortForwardSnapshot PortForwardHost::Snapshot() const { return impl_->Snapshot(); }

}  // namespace crossdesk
