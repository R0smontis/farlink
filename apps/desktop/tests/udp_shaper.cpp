/*
 * Copyright (c) 2026 Farlink contributors
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "udp_shaper.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <random>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace crossdesk {
namespace {

using Clock = std::chrono::steady_clock;
using Us = std::chrono::microseconds;

constexpr int64_t UsPerSecond = 1000000;

int64_t NowUs() {
  return std::chrono::duration_cast<Us>(Clock::now().time_since_epoch())
      .count();
}

// One packet waiting out its shaped release instant.
struct QueuedPacket {
  int64_t release_us = 0;
  std::vector<uint8_t> bytes;
};

// Per-direction shaping state. The virtual-clock release schedule gives a
// token-bucket leak with byte-exact serialization: each packet's earliest
// release is the previous packet's release plus its wire time at the shaped
// rate, minus the burst allowance the bucket has regained since.
struct Direction {
  sockaddr_in dest{};
  bool dest_known = false;
  std::deque<QueuedPacket> queue;
  uint64_t queue_bytes = 0;
  // Virtual time the next byte may leave the shaper, in microseconds.
  int64_t next_release_us = 0;
};

bool SameEndpoint(const sockaddr_in& a, const sockaddr_in& b) {
  return a.sin_family == b.sin_family && a.sin_port == b.sin_port &&
         a.sin_addr.s_addr == b.sin_addr.s_addr;
}

std::string EndpointString(const sockaddr_in& a) {
  char ip[INET_ADDRSTRLEN] = {};
  inet_ntop(AF_INET, &a.sin_addr, ip, sizeof(ip));
  return std::string(ip) + ":" + std::to_string(ntohs(a.sin_port));
}

}  // namespace

bool UdpShaper::Start(const std::string& bind_ip, uint16_t bind_port,
                      const UdpShaperConfig& config) {
  if (running_.load()) return false;
  config_ = config;
  socket_fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (socket_fd_ < 0) {
    std::fprintf(stderr, "udp_shaper: socket() failed: %s\n",
                 std::strerror(errno));
    return false;
  }
  int reuse = 1;
  setsockopt(socket_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  // Receive buffer must absorb media bursts at LAN rate while the shaped
  // drain paces them out; the default 256 KB is a few frames at 60 fps.
  int rcvbuf = 4 * 1024 * 1024;
  setsockopt(socket_fd_, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

  sockaddr_in bind_addr{};
  bind_addr.sin_family = AF_INET;
  bind_addr.sin_port = htons(bind_port);
  if (inet_pton(AF_INET, bind_ip.c_str(), &bind_addr.sin_addr) != 1) {
    std::fprintf(stderr, "udp_shaper: bad bind ip %s\n", bind_ip.c_str());
    ::close(socket_fd_);
    socket_fd_ = -1;
    return false;
  }
  if (::bind(socket_fd_, reinterpret_cast<sockaddr*>(&bind_addr),
             sizeof(bind_addr)) != 0) {
    std::fprintf(stderr, "udp_shaper: bind %s:%u failed: %s\n",
                 bind_ip.c_str(), bind_port, std::strerror(errno));
    ::close(socket_fd_);
    socket_fd_ = -1;
    return false;
  }
  int flags = fcntl(socket_fd_, F_GETFL, 0);
  fcntl(socket_fd_, F_SETFL, flags | O_NONBLOCK);

  bound_port_ = bind_port;
  stop_requested_.store(false);
  thread_ = std::thread([this] { Run(); });
  running_.store(true);
  std::fprintf(stderr,
               "udp_shaper: relaying on %s:%u rate=%llu kbps burst=%uB "
               "queue=%uB delay=%ums jitter=%ums loss=%.3f%% seed=%llu\n",
               bind_ip.c_str(), bind_port,
               static_cast<unsigned long long>(config_.rate_bps / 1000),
               config_.burst_bytes, config_.queue_bytes, config_.delay_ms,
               config_.jitter_ms, config_.loss_pct,
               static_cast<unsigned long long>(config_.seed));
  return true;
}

void UdpShaper::Stop() {
  if (!running_.load() && !thread_.joinable()) return;
  stop_requested_.store(true);
  if (thread_.joinable()) thread_.join();
  if (socket_fd_ >= 0) {
    ::close(socket_fd_);
    socket_fd_ = -1;
  }
  running_.store(false);
}

UdpShaperStats UdpShaper::Snapshot() const {
  UdpShaperStats stats;
  for (int i = 0; i < 2; ++i) {
    stats.forwarded_packets[i] = forwarded_packets_[i].load();
    stats.forwarded_bytes[i] = forwarded_bytes_[i].load();
    stats.dropped_packets[i] = dropped_packets_[i].load();
    stats.dropped_bytes[i] = dropped_bytes_[i].load();
    stats.max_queue_bytes[i] = max_queue_bytes_[i].load();
    stats.loss_dropped_packets[i] = loss_dropped_packets_[i].load();
  }
  stats.pre_learn_drops = pre_learn_drops_.load();
  return stats;
}

void UdpShaper::Run() {
  Direction directions[2];
  sockaddr_in endpoints[2]{};
  int endpoint_count = 0;
  for (auto& direction : directions) direction.next_release_us = NowUs();

  const double rate_bytes_per_us =
      config_.rate_bps > 0 ? config_.rate_bps / 8.0 / UsPerSecond : 0;
  // Wire time of one byte at the shaped rate, in microseconds.
  const double us_per_byte =
      rate_bytes_per_us > 0 ? 1.0 / rate_bytes_per_us : 0;
  const int64_t burst_us = static_cast<int64_t>(
      rate_bytes_per_us > 0 ? config_.burst_bytes * us_per_byte : 0);
  const int64_t delay_us = static_cast<int64_t>(config_.delay_ms) * 1000;
  const int64_t jitter_us = static_cast<int64_t>(config_.jitter_ms) * 1000;
  // Deterministic per-direction PRNG streams. std::mt19937_64 is portable and
  // produces an identical sequence for a given seed on every platform, which
  // is what makes the jitter/loss pattern reproducible across A/B runs. The
  // seed is mixed with the direction index so the two directions do not draw
  // correlated offsets; a zero seed falls back to the process clock (explicitly
  // non-reproducible). One engine drives both the jitter offset and the loss
  // decision for each packet in order, so the per-ordinal sequence is fixed
  // regardless of wall-clock pacing.
  std::mt19937_64 prng[2];
  const bool reproducible = config_.seed != 0;
  for (int d = 0; d < 2; ++d) {
    const uint64_t base =
        reproducible ? config_.seed
                     : static_cast<uint64_t>(
                           std::chrono::steady_clock::now()
                               .time_since_epoch()
                               .count());
    prng[d].seed(base + static_cast<uint64_t>(d) * 0x9e3779b97f4a7c15ULL);
  }
  // Precomputed distributions. Jitter is uniform in [-jitter_us, +jitter_us];
  // loss is uniform in [0,100) percent compared against loss_pct. Both are
  // stateless besides the engine, so packet ordinal alone determines draws.
  std::uniform_int_distribution<int64_t> jitter_dist(-jitter_us, jitter_us);
  std::uniform_real_distribution<double> loss_dist(0.0, 100.0);

  uint8_t buffer[65536];
  bool learned_logged = false;

  while (!stop_requested_.load()) {
    // Drain every packet whose shaped release instant has passed.
    const int64_t now = NowUs();
    int64_t next_release = INT64_MAX;
    for (int d = 0; d < 2; ++d) {
      auto& direction = directions[d];
      while (!direction.queue.empty() &&
             direction.queue.front().release_us <= now) {
        auto packet = std::move(direction.queue.front());
        direction.queue.pop_front();
        direction.queue_bytes -= packet.bytes.size();
        const ssize_t sent = ::sendto(
            socket_fd_, packet.bytes.data(), packet.bytes.size(), 0,
            reinterpret_cast<sockaddr*>(&direction.dest),
            sizeof(direction.dest));
        if (sent == static_cast<ssize_t>(packet.bytes.size())) {
          forwarded_packets_[d].fetch_add(1);
          forwarded_bytes_[d].fetch_add(packet.bytes.size());
        }
        // A short send error (EAGAIN on a full kernel buffer) drops the
        // packet — acceptable for a test shaper and indistinguishable from
        // link loss by the receiver, which is the point of the exercise.
      }
      if (!direction.queue.empty())
        next_release = std::min(next_release, direction.queue.front().release_us);
    }

    // Wait for the next inbound datagram or the next release deadline,
    // whichever comes first. 250 us granularity keeps pacing error well
    // below one packet's wire time at multi-Mbps rates.
    int timeout_us = 250;
    if (next_release != INT64_MAX) {
      const int64_t until_release = next_release - NowUs();
      timeout_us = static_cast<int>(std::max<int64_t>(
          0, std::min<int64_t>(timeout_us, until_release)));
    }
    pollfd pfd{};
    pfd.fd = socket_fd_;
    pfd.events = POLLIN;
    const int ready = ::poll(&pfd, 1, (timeout_us + 999) / 1000);
    if (ready < 0 && errno != EINTR) break;

    if (!(pfd.revents & POLLIN)) continue;

    // Empty the socket; the LAN path can deliver back-to-back frames far
    // faster than the shaped drain, and the queue — not the kernel buffer —
    // is where backpressure must be measured.
    for (;;) {
      sockaddr_in source{};
      socklen_t source_len = sizeof(source);
      const ssize_t size =
          ::recvfrom(socket_fd_, buffer, sizeof(buffer), 0,
                     reinterpret_cast<sockaddr*>(&source), &source_len);
      if (size <= 0) break;

      // Endpoint learning: the first two distinct source sockets are the
      // harness endpoints. A packet from a known endpoint travels toward
      // the other one; a packet from anywhere else is not part of the
      // session and is dropped.
      int from = -1;
      for (int i = 0; i < endpoint_count; ++i) {
        if (SameEndpoint(source, endpoints[i])) {
          from = i;
          break;
        }
      }
      if (from < 0) {
        if (endpoint_count < 2) {
          from = endpoint_count;
          endpoints[endpoint_count++] = source;
          endpoints_learned_.store(endpoint_count);
          std::fprintf(stderr, "udp_shaper: endpoint %d = %s\n", from,
                       EndpointString(source).c_str());
          if (endpoint_count == 2 && !learned_logged) {
            learned_logged = true;
            std::fprintf(stderr, "udp_shaper: both endpoints learned\n");
          }
        } else {
          pre_learn_drops_.fetch_add(1);
          continue;
        }
      }
      const int to = 1 - from;
      auto& direction = directions[to];
      if (!direction.dest_known) {
        if (endpoint_count < 2) {
          // The peer has not sent anything yet, so its socket is unknown.
          // Dropping is correct here: these are ICE connectivity checks,
          // which retransmit until the reverse path appears.
          pre_learn_drops_.fetch_add(1);
          continue;
        }
        direction.dest = endpoints[to];
        direction.dest_known = true;
      }

      // Random loss is decided before drop-tail: configured loss models the
      // link itself, so it applies to every packet independent of how full the
      // shaping queue happens to be. The draw consumes one PRNG value in packet
      // order, keeping the per-ordinal loss pattern reproducible for a seed.
      // A lost packet never enters the queue and never consumes token-bucket
      // wire time, exactly like a packet the physical link dropped.
      if (config_.loss_pct > 0 && loss_dist(prng[to]) < config_.loss_pct) {
        loss_dropped_packets_[to].fetch_add(1);
        continue;
      }

      // Drop-tail on the byte-bounded queue comes first: a dropped packet
      // never enters the service pipeline and therefore must not consume
      // token-bucket wire time. Charging dropped bytes to the virtual clock
      // would throttle the drain below the configured rate under sustained
      // overload — exactly when the bottleneck is supposed to saturate it.
      if (direction.queue_bytes + static_cast<uint64_t>(size) >
          config_.queue_bytes) {
        dropped_packets_[to].fetch_add(1);
        dropped_bytes_[to].fetch_add(size);
        continue;
      }

      // Token-bucket serialization with burst allowance. The earliest
      // departure time is the virtual clock after absorbing this packet's
      // wire time; the burst budget lets the virtual clock lag real time by
      // up to burst_us before serialization starts.
      const int64_t arrival = NowUs();
      int64_t release = std::max(direction.next_release_us,
                                 arrival - burst_us) +
                        delay_us;
      const int64_t wire_us =
          static_cast<int64_t>(size * us_per_byte);
      direction.next_release_us =
          std::max(direction.next_release_us, arrival - burst_us) + wire_us;
      // Without a rate limit packets still traverse the relay so shaped
      // and unshaped runs share the exact same forwarding path.
      if (config_.rate_bps == 0) release = arrival + delay_us;

      // Deterministic jitter: a uniform offset in [-jitter_us,+jitter_us] is
      // added to the release instant. Because the queue drains in release-time
      // order, positive jitter genuinely reorders packets inside the queue,
      // reproducing the interleaving a jittered WAN link produces. The draw
      // consumes one PRNG value in packet order after the loss draw, so the
      // per-ordinal jitter sequence is fixed for a seed. Clamped at the
      // arrival instant so jitter never makes a packet depart before it
      // arrived (a physical impossibility even on a zero-delay link).
      if (jitter_us > 0) {
        release = std::max(release + jitter_dist(prng[to]), arrival);
      }

      QueuedPacket packet;
      packet.release_us = release;
      packet.bytes.assign(buffer, buffer + size);
      direction.queue_bytes += packet.bytes.size();
      // Insert in release-time order, not FIFO: with jitter a later-arriving
      // packet can carry an earlier release instant than one already queued,
      // and the drain loop only ever looks at the front. The typical case is
      // still an append (non-negative net reordering), so the scan is almost
      // always zero iterations; the queue is byte-bounded, so even a fully
      // reordered insertion is cheap. Ties keep insertion order (stable).
      auto insert_at = direction.queue.end();
      while (insert_at != direction.queue.begin() &&
             std::prev(insert_at)->release_us > packet.release_us) {
        --insert_at;
      }
      direction.queue.insert(insert_at, std::move(packet));
      uint64_t current = max_queue_bytes_[to].load();
      while (direction.queue_bytes > current &&
             !max_queue_bytes_[to].compare_exchange_weak(
                 current, direction.queue_bytes)) {
      }
    }
  }
  running_.store(false);
}

}  // namespace crossdesk
