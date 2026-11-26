/*
 * Copyright (c) 2026 Farlink contributors
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */

// Test-only user-space UDP bottleneck forwarder. Sits between the two
// frame_rate_harness endpoints by owning the address their rewritten ICE
// candidates point at, learns each endpoint's real socket from the first
// packet it emits, and relays both directions through a token-bucket shaper
// with a bounded drop-tail queue and optional fixed added delay. No part of
// the media payload is inspected: STUN/DTLS/SRTP all traverse the relay
// untouched, exactly as a WAN bottleneck link would carry them.

#ifndef CROSSDESK_TEST_UDP_SHAPER_H
#define CROSSDESK_TEST_UDP_SHAPER_H

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

namespace crossdesk {

struct UdpShaperConfig {
  // Bottleneck throughput per direction in bits per second. 0 disables
  // rate limiting (packets still traverse the relay and its counters).
  uint64_t rate_bps = 0;
  // Token bucket burst allowance in bytes: how far ahead of the sustained
  // rate a burst may drain before serialization kicks in.
  uint32_t burst_bytes = 1500;
  // Drop-tail bound of the per-direction shaping queue in bytes.
  uint32_t queue_bytes = 65536;
  // Fixed additional one-way delay applied to every forwarded packet.
  uint32_t delay_ms = 0;
  // Uniform jitter magnitude in milliseconds. Each forwarded packet draws a
  // deterministic offset in [-jitter_ms, +jitter_ms] added to its shaped
  // release instant, so packets reorder inside the queue exactly as a WAN
  // bottleneck reorders them. 0 disables jitter. The draw sequence is fixed
  // for a given seed and packet ordinal, independent of wall-clock timing.
  uint32_t jitter_ms = 0;
  // Uniform random packet loss probability in percent (0..100, fractional
  // allowed — e.g. 1.5 means 1.5%). Lossy packets are dropped before entering
  // the shaping queue, so like the drop-tail path they never consume
  // token-bucket wire time. The drop decision uses the same deterministic
  // per-direction PRNG stream as jitter, so a fixed seed reproduces the exact
  // loss pattern for a given packet ordinal. 0 disables random loss.
  double loss_pct = 0;
  // Seed for the deterministic per-direction PRNG streams (std::mt19937_64).
  // Direction d uses seed + d so the two directions do not correlate. 0 means
  // seed from the process clock (non-reproducible); any nonzero value makes
  // the jitter/loss sequence reproducible across runs and across the A/B
  // comparison, which is the entire point of the exercise.
  uint64_t seed = 0;
};

struct UdpShaperStats {
  uint64_t forwarded_packets[2] = {};
  uint64_t forwarded_bytes[2] = {};
  uint64_t dropped_packets[2] = {};
  uint64_t dropped_bytes[2] = {};
  uint64_t max_queue_bytes[2] = {};
  uint64_t pre_learn_drops = 0;
  // Packets dropped by the deterministic random-loss model, counted separately
  // from the drop-tail queue overflow above so a run can tell configured loss
  // apart from genuine bottleneck congestion. Only nonzero when loss_pct > 0.
  uint64_t loss_dropped_packets[2] = {};
};

// Direction indices are stable for the process lifetime: 0 is the first
// endpoint learned, 1 the second. The harness only consumes the aggregate,
// so the assignment order does not matter.
class UdpShaper {
 public:
  UdpShaper() = default;
  UdpShaper(const UdpShaper&) = delete;
  UdpShaper& operator=(const UdpShaper&) = delete;
  ~UdpShaper() { Stop(); }

  // Binds the relay socket on bind_ip:bind_port and launches the forwarding
  // thread. Returns false on any socket error; error detail goes to stderr.
  bool Start(const std::string& bind_ip, uint16_t bind_port,
             const UdpShaperConfig& config);
  // Stops the thread and closes the socket. Safe to call when not started.
  void Stop();
  bool Running() const { return running_.load(); }
  uint16_t BoundPort() const { return bound_port_; }

  UdpShaperStats Snapshot() const;
  // Per-direction instantaneous counters behind Snapshot(); the harness
  // samples these once per second for its trajectory lines.
  uint64_t ForwardedBytes(int direction) const {
    return forwarded_bytes_[direction].load();
  }
  // Both endpoint addresses learned so far; count is 0..2. Purely
  // diagnostic, surfaces in the harness stderr log once learning completes.
  int EndpointsLearned() const { return endpoints_learned_.load(); }

 private:
  void Run();

  std::atomic<bool> running_{false};
  std::atomic<bool> stop_requested_{false};
  std::thread thread_;
  int socket_fd_ = -1;
  uint16_t bound_port_ = 0;
  UdpShaperConfig config_;

  // Endpoint learning state, owned by the forwarding thread except for the
  // learned-count mirror used by diagnostics.
  std::atomic<int> endpoints_learned_{0};

  // Statistics, written by the forwarding thread, read by the harness.
  std::atomic<uint64_t> forwarded_packets_[2] = {};
  std::atomic<uint64_t> forwarded_bytes_[2] = {};
  std::atomic<uint64_t> dropped_packets_[2] = {};
  std::atomic<uint64_t> dropped_bytes_[2] = {};
  std::atomic<uint64_t> max_queue_bytes_[2] = {};
  std::atomic<uint64_t> loss_dropped_packets_[2] = {};
  std::atomic<uint64_t> pre_learn_drops_{0};
};

}  // namespace crossdesk

#endif
