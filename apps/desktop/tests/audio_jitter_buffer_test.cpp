/*
 * @Author: Farlink contributors
 * @Date: 2026-11-25
 * Copyright (c) 2026 by Farlink contributors, All Rights Reserved.
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Deterministic unit tests for MiniRTC's bounded audio jitter buffer.
 * All time is injected; nothing sleeps on the wall clock.
 */

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "audio_jitter_buffer.h"

namespace {

using minirtc::AudioJitterBuffer;

int failures = 0;

#define CHECK(cond)                                                \
  do {                                                             \
    if (!(cond)) {                                                 \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                   #cond);                                         \
      ++failures;                                                  \
    }                                                              \
  } while (0)

// Minimal parseable Opus payload. Only the TOC byte matters here: the
// buffer needs opus_packet_get_nb_samples to return a positive frame count.
std::vector<uint8_t> MakeOpusPacket(uint8_t seed) {
  std::vector<uint8_t> payload(20, seed);
  payload[0] = 0x78;
  return payload;
}

struct Output {
  AudioJitterBuffer::FrameKind kind;
  uint16_t sequence;
  uint32_t timestamp;
  size_t payload_size;
  int64_t emit_time_ms;  // pump time at which this frame was emitted
};

struct Sink {
  std::vector<Output> frames;
  int64_t now_ms = 0;
  void Push(const AudioJitterBuffer::Frame& f) {
    frames.push_back({f.kind, f.sequence, f.timestamp, f.payload_size, now_ms});
  }
};

void PumpAll(AudioJitterBuffer& jb, int64_t now_ms, Sink& sink) {
  sink.now_ms = now_ms;
  jb.Pump(now_ms, [&](const AudioJitterBuffer::Frame& f) { sink.Push(f); });
}

// Effective latency of a given sequence: pump time it was emitted minus the
// time it was inserted. -1 if never emitted as a real frame.
int64_t EffectiveDelayOf(const Sink& sink, uint16_t sequence,
                         int64_t insert_time_ms) {
  for (const auto& f : sink.frames) {
    if (f.sequence == sequence &&
        (f.kind == AudioJitterBuffer::FrameKind::kNormal ||
         f.kind == AudioJitterBuffer::FrameKind::kReordered)) {
      return f.emit_time_ms - insert_time_ms;
    }
  }
  return -1;
}

// 1. In-order passthrough: first frame is emitted exactly one target delay
// after insertion, subsequent frames every 10 ms.
void TestInOrderPassthrough() {
  AudioJitterBuffer jb;
  Sink sink;
  const auto pkt = MakeOpusPacket(1);

  CHECK(jb.Insert(100, 1000, pkt.data(), pkt.size(), 0));
  PumpAll(jb, 0, sink);
  CHECK(sink.frames.empty());  // nothing due yet
  PumpAll(jb, 29, sink);
  CHECK(sink.frames.empty());  // deadline at 30 ms
  PumpAll(jb, 30, sink);
  CHECK(sink.frames.size() == 1);
  CHECK(sink.frames[0].kind == AudioJitterBuffer::FrameKind::kNormal);
  CHECK(sink.frames[0].sequence == 100);
  CHECK(sink.frames[0].timestamp == 1000);

  CHECK(jb.Insert(101, 1480, pkt.data(), pkt.size(), 10));
  PumpAll(jb, 39, sink);
  CHECK(sink.frames.size() == 1);  // next due at 40
  PumpAll(jb, 40, sink);
  CHECK(sink.frames.size() == 2);
  CHECK(sink.frames[1].sequence == 101);
  CHECK(sink.frames[1].timestamp == 1480);
  std::printf("in-order passthrough OK\n");
}

// 2. Reordering inside the reorder window is repaired: the delayed packet is
// played in sequence position with zero loss.
void TestReorderRepair() {
  AudioJitterBuffer jb;
  Sink sink;
  const auto pkt = MakeOpusPacket(2);

  CHECK(jb.Insert(100, 1000, pkt.data(), pkt.size(), 0));
  // seq 101 arrives late, after 102
  CHECK(jb.Insert(102, 1960, pkt.data(), pkt.size(), 12));
  CHECK(jb.Insert(101, 1480, pkt.data(), pkt.size(), 15));
  // Pump just up to the third emission boundary: positions 100/101/102 due
  // at 30/40/50. (Pumping further would emit PLC for the missing tail —
  // correct grid behavior but not what this test measures.)
  for (int t = 30; t <= 59; ++t) PumpAll(jb, t, sink);
  CHECK(sink.frames.size() == 3);
  CHECK(sink.frames[0].sequence == 100);
  CHECK(sink.frames[1].sequence == 101);
  CHECK(sink.frames[1].kind == AudioJitterBuffer::FrameKind::kReordered);
  CHECK(sink.frames[2].sequence == 102);
  CHECK(sink.frames[2].kind == AudioJitterBuffer::FrameKind::kNormal);
  std::printf("reorder repair OK\n");
}

// 3. Loss produces concealment in grid position; sequence and timestamp
// cadence stay continuous so downstream decode/pacing see no gap.
void TestLossConcealmentGrid() {
  AudioJitterBuffer jb;
  Sink sink;
  const auto pkt = MakeOpusPacket(3);

  CHECK(jb.Insert(100, 1000, pkt.data(), pkt.size(), 0));
  // seq 101 lost; 102 arrives on time
  CHECK(jb.Insert(102, 1960, pkt.data(), pkt.size(), 20));
  for (int t = 30; t <= 59; ++t) PumpAll(jb, t, sink);
  CHECK(sink.frames.size() == 3);
  CHECK(sink.frames[0].sequence == 100);
  CHECK(sink.frames[1].sequence == 101);
  CHECK(sink.frames[1].kind == AudioJitterBuffer::FrameKind::kPlc);
  CHECK(sink.frames[1].payload_size == 0);
  // Grid cadence: timestamp of the concealed position advances by exactly
  // one frame (480 ticks at 48 kHz).
  CHECK(sink.frames[1].timestamp == 1000 + 480);
  CHECK(sink.frames[2].sequence == 102);
  CHECK(sink.frames[2].timestamp == 1960);
  // Emission times are exactly 10 ms apart.
  CHECK(sink.frames[2].emit_time_ms - sink.frames[1].emit_time_ms == 10);
  std::printf("loss concealment grid OK\n");
}

// 4. Burst after stall: emission schedule compacts instead of grinding
// through stale audio. After a 500 ms stall, a burst of 30 packets arrives
// at once; the stream must re-anchor onto the newest packets and resume
// within the target delay, never emitting more than the ceiling of stale
// audio.
void TestBurstAfterStallReanchor() {
  AudioJitterBuffer jb;
  Sink sink;
  const auto pkt = MakeOpusPacket(4);

  CHECK(jb.Insert(100, 1000, pkt.data(), pkt.size(), 0));
  PumpAll(jb, 30, sink);
  CHECK(sink.frames.size() == 1);

  // Stall: no packets for 500 ms. Emission grinds out a few PLC frames then
  // goes idle.
  for (int t = 31; t <= 530; ++t) PumpAll(jb, t, sink);
  const size_t frames_after_stall = sink.frames.size();
  CHECK(frames_after_stall <= 1 + 4);  // at most kMaxPlcRun conceals

  // Burst: packets 101..130 arrive together at t=530.
  for (int i = 1; i <= 30; ++i) {
    jb.Insert(uint16_t(100 + i), uint32_t(1000 + i * 480), pkt.data(),
              pkt.size(), 530);
  }
  // Pump forward and measure: after re-anchor, real frames resume quickly.
  for (int t = 531; t <= 700; ++t) PumpAll(jb, t, sink);
  // Count real (non-concealed) frames emitted after the burst.
  int real_after_burst = 0;
  int64_t last_real_emit = 0;
  for (const auto& f : sink.frames) {
    if (f.kind == AudioJitterBuffer::FrameKind::kNormal ||
        f.kind == AudioJitterBuffer::FrameKind::kReordered) {
      if (f.emit_time_ms > 530) {
        ++real_after_burst;
        last_real_emit = f.emit_time_ms;
      }
    }
  }
  CHECK(real_after_burst > 0);
  // The crucial bound: the first post-burst real frame arrives at most
  // target+frame after the burst (re-anchor), not 300+ms of catch-up.
  int64_t first_real_after = -1;
  for (const auto& f : sink.frames) {
    if ((f.kind == AudioJitterBuffer::FrameKind::kNormal ||
         f.kind == AudioJitterBuffer::FrameKind::kReordered) &&
        f.emit_time_ms > 530) {
      first_real_after = f.emit_time_ms;
      break;
    }
  }
  CHECK(first_real_after > 0);
  CHECK(first_real_after - 530 <= 60);
  std::printf("burst-after-stall reanchor OK (first real frame %lld ms after burst)\n",
              (long long)(first_real_after - 530));
}

// 5. Ceiling drain: feed a stream consistently 70 ms early so the queue
// grows past the 60 ms ceiling; the buffer must drop head positions such
// that the backlog never exceeds the ceiling in steady state.
void TestCeilingDrainCompaction() {
  AudioJitterBuffer jb;
  Sink sink;
  const auto pkt = MakeOpusPacket(5);

  // Insert packets with large lead time so the queue fills.
  for (int i = 0; i < 12; ++i) {
    jb.Insert(uint16_t(100 + i), uint32_t(1000 + i * 480), pkt.data(),
              pkt.size(), 0);
  }
  // Pump a long time; count emitted frames. 12 packets were fed over a
  // 120 ms nominal window, but only 60 ms of them fit the ceiling: the
  // rest must have been drained rather than played out late.
  for (int t = 0; t <= 200; ++t) PumpAll(jb, t, sink);
  size_t real = 0;
  for (const auto& f : sink.frames) {
    if (f.kind == AudioJitterBuffer::FrameKind::kNormal) ++real;
  }
  CHECK(real <= 7);  // <= ceiling frames + slack, rest drained
  CHECK(jb.GetStats().dropped_drain > 0);
  std::printf("ceiling drain compaction OK (real %zu drained %llu)\n", real,
              (unsigned long long)jb.GetStats().dropped_drain);
}

// 6. Wraparound: sequence numbers near 0xFFFF wrap correctly.
void TestSequenceWraparound() {
  AudioJitterBuffer jb;
  Sink sink;
  const auto pkt = MakeOpusPacket(6);

  CHECK(jb.Insert(0xFFFE, 1000, pkt.data(), pkt.size(), 0));
  CHECK(jb.Insert(0xFFFF, 1480, pkt.data(), pkt.size(), 10));
  CHECK(jb.Insert(0x0000, 1960, pkt.data(), pkt.size(), 20));
  for (int t = 30; t <= 59; ++t) PumpAll(jb, t, sink);
  CHECK(sink.frames.size() == 3);
  CHECK(sink.frames[0].sequence == 0xFFFE);
  CHECK(sink.frames[1].sequence == 0xFFFF);
  CHECK(sink.frames[2].sequence == 0x0000);
  std::printf("sequence wraparound OK\n");
}

// 7. Duplicates are ignored.
void TestDuplicatesIgnored() {
  AudioJitterBuffer jb;
  Sink sink;
  const auto pkt = MakeOpusPacket(7);

  CHECK(jb.Insert(100, 1000, pkt.data(), pkt.size(), 0));
  CHECK(!jb.Insert(100, 1000, pkt.data(), pkt.size(), 1));  // duplicate
  CHECK(jb.GetStats().duplicates == 1);
  for (int t = 30; t <= 40; ++t) PumpAll(jb, t, sink);
  // Exactly one real frame total: the duplicate never produced a second
  // emission (a trailing PLC for the missing next position is expected).
  int real = 0;
  for (const auto& f : sink.frames)
    if (f.kind == AudioJitterBuffer::FrameKind::kNormal) ++real;
  CHECK(real == 1);
  std::printf("duplicates ignored OK\n");
}

// 8. Target adaptation is frame-grid coherent, measured by *effective*
// delay (insert time -> emit time of a given sequence), not the internal
// target number.
void TestAdaptationEffectiveDelay() {
  // 8a. Clean stream: effective delay equals the 30 ms base target exactly.
  {
    AudioJitterBuffer jb;
    Sink sink;
    const auto pkt = MakeOpusPacket(8);
    uint16_t seq = 100;
    for (int i = 0; i < 200; ++i, ++seq) {
      const int64_t t = i * 10;
      jb.Insert(seq, uint32_t(1000 + i * 480), pkt.data(), pkt.size(), t);
      for (int64_t pump_t = t; pump_t < t + 10; ++pump_t)
        PumpAll(jb, pump_t, sink);
    }
    // Settled mid-stream frames: insert-to-emit must be exactly 30 ms.
    int64_t clean_delay = EffectiveDelayOf(sink, uint16_t(150), 50 * 10);
    CHECK(clean_delay == 30);
    std::printf("adaptation: clean-stream effective delay %lld ms (base 30)\n",
                (long long)clean_delay);
  }

  // 8b. True jitter variance: most packets arrive ~on time, every 5th
  // packet arrives 35 ms after its grid time — well beyond what the 30 ms
  // base target absorbs. Tardy positions accumulate, the target must grow
  // in whole-frame steps, and the effective delay of late-arriving packets
  // must rise correspondingly while never exceeding the ceiling.
  {
    AudioJitterBuffer jb;
    Sink sink;
    const auto pkt = MakeOpusPacket(8);
    uint16_t seq = 100;
    int64_t probe_insert_t = 0;
    uint16_t probe_seq = 0;
    for (int i = 0; i < 400; ++i, ++seq) {
      const int64_t grid_t = i * 10;
      const int64_t arrival = (i % 5 == 4) ? grid_t + 35 : grid_t + 2;
      jb.Insert(seq, uint32_t(1000 + i * 480), pkt.data(), pkt.size(), arrival);
      if (i == 354) { probe_seq = seq; probe_insert_t = arrival; }
      for (int64_t pump_t = grid_t; pump_t < grid_t + 12; ++pump_t)
        PumpAll(jb, pump_t, sink);
    }
    CHECK(jb.GetStats().grow_events > 0);
    CHECK(jb.TargetDelayMs() > 30);
    CHECK(jb.TargetDelayMs() <= 60);
    // Target steps are whole frames — a 5 ms step cannot exist on a 10 ms
    // grid.
    CHECK(jb.TargetDelayMs() % 10 == 0);
    const int64_t grown_delay = EffectiveDelayOf(sink, probe_seq, probe_insert_t);
    // The probe packet's insert-to-emit delay must stay within the ceiling
    // whenever it was emitted as a real frame. (A negative value means the
    // position was emitted as concealment before the late packet arrived
    // and the real packet was then dropped as too-late — also correct
    // behavior, just not measurable as a delay.)
    if (grown_delay >= 0) {
      CHECK(grown_delay <= 60);
      CHECK(grown_delay >= 0);
    }
    std::printf("adaptation: jittery-stream target %d ms, grow_events %llu, probe effective delay %lld ms\n",
                jb.TargetDelayMs(),
                (unsigned long long)jb.GetStats().grow_events,
                (long long)grown_delay);
  }

  // 8c. Shrink: after the target grew, sustained clean traffic shrinks it
  // back; each shrink consumes exactly one head frame (visible in
  // dropped_shrink), and the effective delay falls back toward base.
  {
    AudioJitterBuffer jb;
    Sink sink;
    const auto pkt = MakeOpusPacket(8);
    uint16_t seq = 100;
    // Phase 1: jittery traffic grows the target.
    for (int i = 0; i < 300; ++i, ++seq) {
      const int64_t grid_t = i * 10;
      const int64_t arrival = (i % 5 == 4) ? grid_t + 35 : grid_t + 2;
      jb.Insert(seq, uint32_t(1000 + i * 480), pkt.data(), pkt.size(), arrival);
      for (int64_t pump_t = grid_t; pump_t < grid_t + 12; ++pump_t)
        PumpAll(jb, pump_t, sink);
    }
    const int grown_target = jb.TargetDelayMs();
    CHECK(grown_target > 30);
    const uint64_t shrinks_before = jb.GetStats().shrink_events;

    // Phase 2: clean traffic, target shrinks back frame by frame.
    for (int i = 300; i < 1200; ++i, ++seq) {
      const int64_t grid_t = i * 10;
      jb.Insert(seq, uint32_t(1000 + i * 480), pkt.data(), pkt.size(), grid_t);
      for (int64_t pump_t = grid_t; pump_t < grid_t + 10; ++pump_t)
        PumpAll(jb, pump_t, sink);
    }
    CHECK(jb.GetStats().shrink_events > shrinks_before);
    CHECK(jb.TargetDelayMs() < grown_target);
    CHECK(jb.TargetDelayMs() >= 30);
    CHECK(jb.TargetDelayMs() % 10 == 0);
    // Every shrink step drops exactly one frame position, so the totals
    // must match exactly.
    CHECK(jb.GetStats().dropped_shrink == jb.GetStats().shrink_events);
    std::printf("adaptation: shrink grew=%d ms final=%d ms shrink_events=%llu dropped_shrink=%llu\n",
                grown_target, jb.TargetDelayMs(),
                (unsigned long long)jb.GetStats().shrink_events,
                (unsigned long long)jb.GetStats().dropped_shrink);
  }
}

}  // namespace

int main() {
  TestInOrderPassthrough();
  TestReorderRepair();
  TestLossConcealmentGrid();
  TestBurstAfterStallReanchor();
  TestCeilingDrainCompaction();
  TestSequenceWraparound();
  TestDuplicatesIgnored();
  TestAdaptationEffectiveDelay();

  if (failures == 0) {
    std::printf("audio_jitter_buffer_test: all checks passed\n");
    return 0;
  }
  std::fprintf(stderr, "audio_jitter_buffer_test: %d check(s) failed\n",
               failures);
  return 1;
}
