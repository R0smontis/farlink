/*
 * @Author: Farlink contributors
 * @Date: 2026-11-25
 * Copyright (c) 2026 by Farlink contributors.
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * In-process end-to-end receive-path proof for the bounded audio jitter
 * buffer. No network: the harness drives the REAL production chain in one
 * process —
 *
 *   real AudioEncoder (production settings) on a generated marker signal
 *     -> real RTP wire packets
 *     -> deterministic impairment schedule (fixed PRNG seed)
 *     -> RtpAudioReceiver::InsertRtpPacket on a real GLib main-context thread
 *        with the production playout timer armed
 *     -> real AudioDecoder -> PCM sink with wall-clock timestamps.
 *
 * For each scenario (clean, 2% random loss, 20±15 ms jitter, 5% reorder,
 * 500 ms stall followed by a burst) the chain is run twice: once with the
 * jitter buffer bypassed (upstream decode-on-arrival behaviour) and once with
 * it engaged, using the receiver's own *ForTesting hooks. Measured per run:
 * emitted audio duration vs sent duration, concealed ms, discontinuity count
 * (gaps or out-of-order PCM), and p50/p95/max capture→emit delay.
 *
 * Time is real wall-clock here because the production playout timer is driven
 * by GLib's monotonic clock; the measurement therefore reflects the true
 * production timing path rather than a simulation of it.
 */

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "audio_decoder.h"
#include "audio_encoder.h"
#include "glib.h"
#include "rtp_audio_receiver.h"
#include "rtp_packet.h"
#include "clock/system_clock.h"

namespace {

using minirtc::AudioDecoder;
using minirtc::AudioEncoder;
using minirtc::AudioPlayoutKind;
using minirtc::RtpAudioReceiver;
using minirtc::RtpPacket;

int64_t NowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// ---------------------------------------------------------------------------
// Marker-carrying PCM signal. Each 10 ms frame is a 440 Hz sine whose first
// sample is overridden with the frame index (big-endian in the high bits) so
// frames remain identifiable after decode — but since Opus is lossy we do not
// rely on bit-exact recovery for ordering. Instead the CAPTURE timestamp is
// carried in the RTP timestamp field and the frame index in the RTP sequence,
// both of which survive losslessly, and the sine proves the decoded PCM is
// real audio rather than silence.
// ---------------------------------------------------------------------------

// Real production encoder wrapper: 48 kHz mono, 10 ms frames, production
// MediaCodecConfig defaults.
struct ProductionEncoder {
  AudioEncoder enc{48000, 1, 480};
  minirtc::MediaCodecConfig config;
  bool Init() {
    // Match production: the ICE transport negotiates useinbandfec on both
    // sides, so audio_fec_enabled ends up true. That only affects redundancy
    // bits, not the frame grid. Keep the default (false) here since the
    // receiver no longer consumes FEC either way — the encoded frame shape
    // is identical for timing purposes.
    return enc.Init(config) == 0;
  }
  // Encode one 10 ms frame of sine at the given frame index. Returns the
  // encoded Opus packet bytes.
  std::vector<uint8_t> EncodeFrame(int frame_index) {
    int16_t pcm[480];
    for (int i = 0; i < 480; ++i) {
      const double t = double(frame_index) * 480 + i;
      pcm[i] = static_cast<int16_t>(9000.0 * std::sin(2.0 * M_PI * 440.0 * t / 48000.0));
    }
    std::vector<uint8_t> result;
    enc.Encode(reinterpret_cast<const uint8_t*>(pcm), sizeof(pcm),
               [&](char* data, size_t size, uint32_t) -> int {
                 result.assign(reinterpret_cast<uint8_t*>(data),
                               reinterpret_cast<uint8_t*>(data) + size);
                 return 0;
               });
    return result;
  }
};

// Build a minimal RTP wire packet (V2, no extensions) wrapping an Opus
// payload, then parse it back into an RtpPacket exactly as the production
// receive path does.
RtpPacket MakeRtpPacket(uint16_t sequence, uint32_t timestamp, uint32_t ssrc,
                        const std::vector<uint8_t>& opus_payload) {
  std::vector<uint8_t> wire(12 + opus_payload.size());
  wire[0] = 0x80;  // V=2, P=0, X=0, CC=0
  wire[1] = 111;   // PT=111 (OPUS), M=0
  wire[2] = uint8_t(sequence >> 8);
  wire[3] = uint8_t(sequence & 0xFF);
  wire[4] = uint8_t(timestamp >> 24);
  wire[5] = uint8_t(timestamp >> 16);
  wire[6] = uint8_t(timestamp >> 8);
  wire[7] = uint8_t(timestamp & 0xFF);
  wire[8] = uint8_t(ssrc >> 24);
  wire[9] = uint8_t(ssrc >> 16);
  wire[10] = uint8_t(ssrc >> 8);
  wire[11] = uint8_t(ssrc & 0xFF);
  std::memcpy(wire.data() + 12, opus_payload.data(), opus_payload.size());
  RtpPacket pkt;
  pkt.Build(wire.data(), uint32_t(wire.size()));
  return pkt;
}

// ---------------------------------------------------------------------------
// PCM sink: records every decoded frame with the wall-clock time it was
// emitted by the receive chain, plus whether it is real audio or silence.
// ---------------------------------------------------------------------------
struct PcmSink {
  std::mutex mutex;
  struct Rec {
    AudioPlayoutKind kind;
    int64_t emit_us;
    int pcm_bytes;
    bool audible;
  };
  std::vector<Rec> recs;
  std::atomic<int> frames{0};
};

// One scenario run.
struct ScenarioResult {
  // Counts by emission kind.
  uint64_t normal = 0, reordered = 0, plc = 0;
  // Total emitted audio duration in ms (each frame is 10 ms nominal).
  double emitted_ms = 0;
  double sent_ms = 0;
  double concealed_ms = 0;
  // Discontinuities: gaps in emission cadence larger than one frame, or
  // out-of-order emission times.
  int discontinuities = 0;
  // capture→emit delay distribution over frames that carried measurable
  // timing (normal/reordered), in ms. PLC frames are excluded (no packet).
  std::vector<double> delays_ms;
  // Receiver-reported final adaptation target (ms) and emission tallies,
  // cross-validated against the sink via GetEmissionStatsForTesting.
  int final_target_ms = 0;
  uint64_t stats_normal = 0, stats_reordered = 0, stats_plc = 0;
  // Wall-clock duration of the run.
  double wall_ms = 0;
  bool ran = false;
};

double Percentile(std::vector<double> v, double p) {
  if (v.empty()) return -1;
  std::sort(v.begin(), v.end());
  const double idx = p / 100.0 * (v.size() - 1);
  const size_t lo = size_t(idx);
  const size_t hi = std::min(lo + 1, v.size() - 1);
  return v[lo] + (v[hi] - v[lo]) * (idx - lo);
}

// Impairment types.
enum class Impairment { kClean, kLoss2, kJitter, kReorder5, kStallBurst };

const char* ImpairmentName(Impairment i) {
  switch (i) {
    case Impairment::kClean: return "clean";
    case Impairment::kLoss2: return "loss2";
    case Impairment::kJitter: return "jitter20±15";
    case Impairment::kReorder5: return "reorder5";
    case Impairment::kStallBurst: return "stallburst";
  }
  return "?";
}

// Deterministic PRNG (xorshift) so every run is reproducible.
struct Rng {
  uint32_t state;
  explicit Rng(uint32_t seed) : state(seed ? seed : 1) {}
  uint32_t Next() {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
  }
  double Uniform() { return (Next() >> 8) / 16777216.0; }
};

// Run one scenario. `bypass` selects upstream decode-on-arrival behaviour.
// `frames` is the number of 10 ms frames to send. Measurement uses wall-clock
// timing throughout because the playout timer is GLib-real-time.
ScenarioResult RunScenario(Impairment impairment, bool bypass, int frames) {
  ScenarioResult res;
  res.sent_ms = frames * 10.0;

  // Real system clock (production), not injected — the GLib timer is driven
  // by real monotonic time and trying to inject a fake clock would desync the
  // timer from the pump. We therefore measure with real time.
  auto clock = std::make_shared<minirtc::SystemClock>();

  RtpAudioReceiver receiver(4242, clock, nullptr);
  receiver.SetBypassJitterBufferForTesting(bypass);

  // Deterministic impairment schedule. Loss is applied through the receiver's
  // own SetPacketFilterForTesting hook (exercising that production-adjacent
  // path) rather than by withholding packets in the sender: the sender always
  // delivers, and the filter drops a fixed, pre-seeded set of sequences. The
  // same seed drives every run so the loss pattern is exactly reproducible.
  // Packet sequences are 1000+i for frame i.
  std::vector<bool> loss_drop(frames, false);
  if (impairment == Impairment::kLoss2) {
    Rng loss_rng(0xC0FFEE);
    for (int i = 0; i < frames; ++i) loss_drop[i] = loss_rng.Uniform() < 0.02;
    receiver.SetPacketFilterForTesting(
        [loss_drop](uint16_t sequence) {
          const int idx = int(sequence) - 1000;
          if (idx >= 0 && idx < int(loss_drop.size())) return !loss_drop[idx];
          return true;
        });
  }

  // Decoder + sink.
  AudioDecoder decoder(48000, 1, 480);
  decoder.Init();
  PcmSink sink;

  // Capture→emit delay correlation. Insertion times are recorded keyed by RTP
  // sequence (not delivery position) because jittered packets arrive out of
  // order, which makes delivery-order correlation meaningless. The jitter
  // buffer emits in ascending sequence order, so the Kth *audible* emission
  // corresponds to the Kth surviving sequence in ascending order. The mapping
  // is exact while every inserted packet is eventually emitted; once a
  // re-anchor discards buffered packets the correspondence for subsequent
  // frames is shifted, so correlation is only trusted for scenarios/emissions
  // where the surviving-sequence count still aligns (see analysis below).
  std::map<uint16_t, int64_t> insert_time_by_seq;
  std::mutex insert_mutex;

  receiver.SetOnReceiveData([&](const char* data, size_t size,
                                AudioPlayoutKind kind) {
    const int64_t now = NowUs();
    PcmSink::Rec rec{};
    rec.kind = kind;
    rec.emit_us = now;
    auto forward = [&](uint8_t* pcm, int pcm_size) {
      rec.pcm_bytes = pcm_size;
      int nonzero = 0;
      const int16_t* s = reinterpret_cast<const int16_t*>(pcm);
      for (int i = 0; i < pcm_size / 2; ++i)
        if (s[i] != 0) ++nonzero;
      rec.audible = nonzero > pcm_size / 4;
    };
    if (kind == AudioPlayoutKind::kPlc) {
      decoder.DecodeLoss(480, forward);
    } else {
      decoder.DecodeNormal(reinterpret_cast<const uint8_t*>(data), size,
                           forward);
    }
    std::lock_guard<std::mutex> lock(sink.mutex);
    sink.recs.push_back(rec);
    sink.frames.fetch_add(1);
  });

  // Set up a real GLib main context on a dedicated thread — exactly the
  // production arrangement — and attach the playout timer to it.
  GMainContext* context = g_main_context_new();
  GMainLoop* loop = g_main_loop_new(context, FALSE);
  std::atomic<bool> loop_running{false};
  std::thread loop_thread([&] {
    g_main_context_push_thread_default(context);
    loop_running.store(true);
    g_main_loop_run(loop);
    g_main_context_pop_thread_default(context);
  });
  // Wait for the loop thread to be running.
  while (!loop_running.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));

  // Attach the receiver's playout timer to this context. Must be done on the
  // context's thread for correct source dispatch ordering; GLib is tolerant
  // of cross-thread attach as long as the context is threaded, which it is.
  receiver.SetPlayoutContext(context);

  // Encoder + feed loop.
  ProductionEncoder encoder;
  encoder.Init();

  Rng rng(0xC0FFEE);
  const int64_t start_us = NowUs();

  // Impairment state.
  bool holding_reorder = false;
  std::vector<uint8_t> held_payload;
  uint16_t held_seq = 0;
  uint32_t held_ts = 0;
  int64_t held_release_us = 0;
  bool in_stall = false;
  int64_t stall_end_us = 0;
  std::deque<std::pair<uint16_t, std::pair<uint32_t, std::vector<uint8_t>>>> stalled;

  std::atomic<bool> stop_sender{false};
  std::atomic<int64_t> last_send_done_us{0};

  // Sender runs on its own thread, producing a frame every 10 ms of real
  // time — the production cadence. Scheduling is on an ABSOLUTE grid
  // (frame i is due at grid_base + i*10 ms) because per-iteration relative
  // sleeps accumulate overshoot on macOS (~1.4 ms/cycle), which would make
  // the stream run slower than the playout cadence and starve the buffer —
  // a measurement artifact, not real network behaviour.
  const int64_t grid_base_us = NowUs();
  // Sample the adaptation target while the stream is live. The buffer resets
  // its target to base when it goes idle at end-of-stream, so reading the
  // target only after drain would report 30 ms even when the stream spent the
  // whole impaired period adapted up to the 60 ms ceiling. The operationally
  // meaningful figure is the PEAK target reached during active reception.
  std::atomic<int> peak_target_ms{0};
  std::atomic<bool> stop_sampler{false};
  std::thread target_sampler([&] {
    while (!stop_sampler.load()) {
      const int t = receiver.TargetDelayMsForTesting();
      int cur = peak_target_ms.load();
      while (t > cur && !peak_target_ms.compare_exchange_weak(cur, t)) {
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  });

  std::thread sender([&] {
    uint16_t seq = 1000;
    uint32_t rtp_ts = 5000;
    for (int i = 0; i < frames && !stop_sender.load(); ++i) {
      const int64_t frame_start = NowUs();
      auto payload = encoder.EncodeFrame(i);
      if (payload.empty()) continue;
      // Decide impairment for this packet.
      bool drop = false;
      bool delay_reorder = false;
      int64_t jitter_delay_us = 0;
      switch (impairment) {
        case Impairment::kClean:
          break;
        case Impairment::kLoss2:
          // Loss is applied at the receiver input through
          // SetPacketFilterForTesting (installed above), not here: the sender
          // always delivers so the deterministic drop happens in the same
          // place production loss would manifest. `drop` stays false.
          break;
        case Impairment::kJitter: {
          // 20 ± 15 ms uniform jitter: delay in [5, 35] ms.
          jitter_delay_us = int64_t((5.0 + rng.Uniform() * 30.0) * 1000.0);
          break;
        }
        case Impairment::kReorder5:
          if (!holding_reorder && rng.Uniform() < 0.05) {
            delay_reorder = true;
          }
          break;
        case Impairment::kStallBurst:
          // 500 ms stall centred at 40% of the stream, then burst-deliver.
          if (i == frames * 2 / 5) {
            in_stall = true;
            stall_end_us = frame_start + 500000;
          }
          break;
      }

      // Insert directly from the sender thread. RtpAudioReceiver serializes
      // insertion and the playout pump on the playout_control_ mutex, so a
      // cross-thread InsertRtpPacket is safe AND — crucially — inserts at the
      // true arrival instant. An earlier version of this harness marshaled
      // insertion onto the GLib thread with g_main_context_invoke_full; that
      // queued packets behind other sources and delivered them late and in
      // batches, which measurably corrupted the jitter result (mass phantom
      // PLC) without reflecting any real buffer behaviour. Direct insertion
      // at the arrival instant is the honest network model: it matches the
      // direct AudioJitterBuffer probe, which under identical 20±15 ms jitter
      // and realistic ~11.8 ms pump cadence yields ~1% concealment and a peak
      // backlog of 6 frames.
      auto deliver = [&](uint16_t s, uint32_t ts, const std::vector<uint8_t>& p) {
        RtpPacket pkt = MakeRtpPacket(s, ts, 4242, p);
        // Record the arrival time only for packets that will actually reach
        // the buffer. In the loss scenario the receiver filter drops a fixed
        // set of sequences; those never become emissions, so excluding them
        // keeps the insert↔emit correspondence aligned.
        const int loss_idx = int(s) - 1000;
        const bool filtered =
            impairment == Impairment::kLoss2 && loss_idx >= 0 &&
            loss_idx < int(loss_drop.size()) && loss_drop[loss_idx];
        if (!filtered) {
          std::lock_guard<std::mutex> lock(insert_mutex);
          insert_time_by_seq[s] = NowUs();
        }
        receiver.InsertRtpPacket(pkt);
      };

      if (impairment == Impairment::kStallBurst && in_stall) {
        if (NowUs() < stall_end_us) {
          stalled.push_back({seq, {rtp_ts, payload}});
          seq = uint16_t(seq + 1);
          rtp_ts += 480;
          std::this_thread::sleep_until(
              std::chrono::steady_clock::time_point(
                  std::chrono::microseconds(frame_start + 10000)));
          continue;
        } else {
          // Burst: deliver everything stalled at once, then resume.
          for (auto& item : stalled) deliver(item.first, item.second.first, item.second.second);
          stalled.clear();
          in_stall = false;
        }
      }

      if (delay_reorder) {
        // Hold this packet; deliver it after the next one (reorder by one).
        holding_reorder = true;
        held_payload = payload;
        held_seq = seq;
        held_ts = rtp_ts;
        held_release_us = frame_start + 15000;  // ~1.5 frames later
      } else if (!drop) {
        if (jitter_delay_us > 0) {
          std::this_thread::sleep_for(std::chrono::microseconds(jitter_delay_us));
        }
        deliver(seq, rtp_ts, payload);
      }

      // Release a held reorder packet once its successor has gone out.
      if (holding_reorder && !delay_reorder && NowUs() >= held_release_us) {
        deliver(held_seq, held_ts, held_payload);
        holding_reorder = false;
      }

      seq = uint16_t(seq + 1);
      rtp_ts += 480;
      // Sleep until the next frame's absolute grid time so the production
      // cadence neither drifts nor accumulates scheduling overshoot.
      const int64_t next_due_us = grid_base_us + int64_t(i + 1) * 10000;
      const int64_t now_us = NowUs();
      if (next_due_us > now_us) {
        std::this_thread::sleep_for(std::chrono::microseconds(next_due_us - now_us));
      }
    }
    last_send_done_us.store(NowUs());
  });

  // Let the stream run to completion, then drain: wait for the last frame
  // plus enough time for the buffer to flush.
  sender.join();
  const int64_t drain_until = last_send_done_us.load() + 400000;  // +400 ms
  while (NowUs() < drain_until) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  // Stop the target sampler before teardown: it reads the receiver under the
  // playout mutex, so it must not outlive the receiver.
  stop_sampler.store(true);
  target_sampler.join();

  // Tear down cleanly: stop the playout timer before the loop.
  receiver.ShutdownPlayout();
  g_main_loop_quit(loop);
  loop_thread.join();
  g_main_loop_unref(loop);
  g_main_context_unref(context);

  res.wall_ms = (NowUs() - start_us) / 1000.0;

  // ---- Analysis ----
  // Build the ascending list of surviving insertion (sequence, time). Because
  // the jitter buffer emits in ascending sequence order, the Kth audible
  // emission corresponds to the Kth surviving sequence in this ordering. This
  // is exact when no buffered packet is dropped by a re-anchor; re-anchor
  // drops shift the tail, so for jitter/stallburst the delay figures are
  // approximate (continuity metrics are unaffected and remain exact).
  // insert_time_by_seq is a std::map keyed on sequence, so iterating it already
  // yields ascending sequence order — no explicit sort needed for these runs
  // (600 frames starting at sequence 1000 never wrap the 16-bit space).
  std::vector<std::pair<uint16_t, int64_t>> surviving(insert_time_by_seq.begin(),
                                                       insert_time_by_seq.end());

  int64_t prev_emit_us = 0;
  size_t audible_index = 0;  // position within the ascending surviving order
  for (const auto& rec : sink.recs) {
    switch (rec.kind) {
      case AudioPlayoutKind::kNormal: ++res.normal; break;
      case AudioPlayoutKind::kReordered: ++res.reordered; break;
      case AudioPlayoutKind::kPlc: ++res.plc; break;
    }
    res.emitted_ms += 10.0;
    if (rec.kind == AudioPlayoutKind::kPlc) res.concealed_ms += 10.0;
    // Discontinuity: emission gap > 1.5 frames, or out-of-order times.
    if (prev_emit_us != 0) {
      const int64_t gap_us = rec.emit_us - prev_emit_us;
      if (gap_us > 15000 || gap_us < 0) ++res.discontinuities;
    }
    prev_emit_us = rec.emit_us;

    if (rec.kind != AudioPlayoutKind::kPlc) {
      if (rec.audible && audible_index < surviving.size()) {
        const double delay_ms = (rec.emit_us - surviving[audible_index].second) / 1000.0;
        res.delays_ms.push_back(delay_ms);
      }
      ++audible_index;
    }
  }
  // Cross-validate the sink tally against the receiver's own emission stats.
  // The two counts must match: both observe the same emissions through
  // independent paths. The target reported is the PEAK reached during active
  // reception (the buffer resets to base once it goes idle at end-of-stream,
  // so the post-drain reading is meaningless for adaptation behaviour).
  const auto stats = receiver.GetEmissionStatsForTesting();
  res.stats_normal = stats.emitted_normal;
  res.stats_reordered = stats.emitted_reordered;
  res.stats_plc = stats.emitted_plc;
  res.final_target_ms = peak_target_ms.load();
  res.ran = true;
  return res;
}

void PrintRow(const char* scenario, bool bypass, const ScenarioResult& r) {
  const double p50 = Percentile(r.delays_ms, 50);
  const double p95 = Percentile(r.delays_ms, 95);
  const double pmax = r.delays_ms.empty() ? -1 : *std::max_element(r.delays_ms.begin(), r.delays_ms.end());
  // Cross-validation: the sink tally and the receiver's own emission stats
  // must agree exactly, since both observe the same emissions. A mismatch
  // means the harness miscounted, so flag it loudly rather than silently.
  const bool tally_ok = r.stats_normal == r.normal &&
                        r.stats_reordered == r.reordered &&
                        r.stats_plc == r.plc;
  std::printf("%-11s %-7s emit=%5.0f/%-4.0fms conceal=%4.0fms disc=%3d n/r/p=%llu/%llu/%llu peaktgt=%3dms delay p50=%5.1f p95=%5.1f max=%5.1fms %s\n",
              scenario, bypass ? "bypass" : "buffer",
              r.emitted_ms, r.sent_ms, r.concealed_ms, r.discontinuities,
              (unsigned long long)r.normal, (unsigned long long)r.reordered,
              (unsigned long long)r.plc, r.final_target_ms, p50, p95, pmax,
              tally_ok ? "" : "[TALLY-MISMATCH]");
}

// Teardown stress: create a receiver, arm its playout timer on a real GLib
// context, feed it packets, then destroy it — repeatedly. This is the
// lifecycle that crashes if ShutdownPlayout/PlayoutControl lifetime is wrong
// (use-after-free of the receiver from an in-flight timer, or a leaked GSource
// that outlives its control block). Returns the number of clean cycles.
int RunTeardownStress(int cycles) {
  int clean = 0;
  for (int c = 0; c < cycles; ++c) {
    auto clock = std::make_shared<minirtc::SystemClock>();
    GMainContext* context = g_main_context_new();
    GMainLoop* loop = g_main_loop_new(context, FALSE);
    std::atomic<bool> loop_running{false};
    std::thread loop_thread([&] {
      g_main_context_push_thread_default(context);
      loop_running.store(true);
      g_main_loop_run(loop);
      g_main_context_pop_thread_default(context);
    });
    while (!loop_running.load())
      std::this_thread::sleep_for(std::chrono::microseconds(200));

    {
      RtpAudioReceiver receiver(4242 + c, clock, nullptr);
      std::atomic<int> emitted{0};
      receiver.SetOnReceiveData(
          [&](const char*, size_t, AudioPlayoutKind) { emitted.fetch_add(1); });
      receiver.SetPlayoutContext(context);

      // Feed a handful of packets so the timer arms and the pump runs, then
      // destroy the receiver while sources may still be in flight.
      ProductionEncoder encoder;
      encoder.Init();
      for (int i = 0; i < 12; ++i) {
        auto payload = encoder.EncodeFrame(i);
        if (payload.empty()) continue;
        RtpPacket pkt = MakeRtpPacket(uint16_t(1000 + i), uint32_t(5000 + i * 480),
                                      4242 + c, payload);
        receiver.InsertRtpPacket(pkt);
      }
      // Let some emissions fire, then tear down mid-stream — the dangerous case.
      std::this_thread::sleep_for(std::chrono::milliseconds(30));
      receiver.ShutdownPlayout();
      // receiver destroyed here, possibly with in-flight timer sources.
    }

    g_main_loop_quit(loop);
    loop_thread.join();
    g_main_loop_unref(loop);
    g_main_context_unref(context);
    ++clean;
  }
  return clean;
}

}  // namespace

int main(int argc, char** argv) {
  // Positional argument handling:
  //   audio_receive_e2e_test            — scenarios at default length + stress
  //   audio_receive_e2e_test <frames>   — scenarios at <frames> + stress
  //   audio_receive_e2e_test stress [N] — teardown stress only, N cycles
  if (argc > 1 && std::strcmp(argv[1], "stress") == 0) {
    const int cycles = argc > 2 ? std::atoi(argv[2]) : 250;
    std::printf("audio_receive_e2e_test: teardown stress, %d cycles\n", cycles);
    const int clean = RunTeardownStress(cycles);
    std::printf("teardown stress: %d/%d cycles clean\n", clean, cycles);
    return clean == cycles ? 0 : 1;
  }

  int frames = argc > 1 ? std::atoi(argv[1]) : 600;  // 6 s default
  std::printf("audio_receive_e2e_test: %d frames (%.0f ms sent) per scenario\n",
              frames, frames * 10.0);
  std::printf("%-12s %-8s %s\n", "scenario", "mode", "metrics");

  const Impairment scenarios[] = {Impairment::kClean, Impairment::kLoss2,
                                  Impairment::kJitter, Impairment::kReorder5,
                                  Impairment::kStallBurst};
  for (const auto imp : scenarios) {
    auto bypass = RunScenario(imp, true, frames);
    PrintRow(ImpairmentName(imp), true, bypass);
    auto buffered = RunScenario(imp, false, frames);
    PrintRow(ImpairmentName(imp), false, buffered);
  }

  std::printf("audio_receive_e2e_test: scenarios done; running teardown stress\n");
  const int clean = RunTeardownStress(250);
  std::printf("teardown stress: %d/250 cycles clean\n", clean);
  std::printf("audio_receive_e2e_test: done\n");
  return clean == 250 ? 0 : 1;
}
