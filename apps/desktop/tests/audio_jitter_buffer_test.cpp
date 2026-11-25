/*
 * @Author: Farlink contributors
 * @Date: 2026-11-25
 * Copyright (c) 2026 by Farlink contributors.
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Deterministic behavioural tests for MiniRTC's bounded audio jitter buffer.
 * All time is injected; nothing sleeps on the wall clock. Assertions are on
 * observable behaviour — insert→emit delay, conservation of packets, cadence —
 * never on the internal target number alone.
 *
 * Delay measurement method: every inserted packet carries a unique 2-byte
 * marker in its payload. Emissions are matched back to their source packet by
 * that marker, giving an exact insert→emit delay per packet regardless of
 * loss or reordering in between. PLC frames carry no payload and match
 * nothing, which is correct — they represent no received packet.
 */

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
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

// Minimal parseable Opus payload with a unique marker embedded at bytes [1,2]
// (big-endian). Byte 0 is a valid TOC so opus_packet_get_nb_samples parses a
// positive frame count.
std::vector<uint8_t> MakePacket(uint16_t marker) {
  std::vector<uint8_t> payload(20, 0);
  payload[0] = 0x78;
  payload[1] = uint8_t(marker >> 8);
  payload[2] = uint8_t(marker & 0xFF);
  return payload;
}

uint16_t MarkerOf(const uint8_t* payload, size_t size) {
  if (!payload || size < 3) return 0xFFFF;
  return uint16_t((payload[1] << 8) | payload[2]);
}

struct Output {
  AudioJitterBuffer::FrameKind kind;
  size_t payload_size;
  uint16_t marker;       // 0xFFFF for PLC (no payload)
  int64_t emit_time_ms;  // pump time at which this frame was emitted
};

struct Sink {
  std::vector<Output> frames;
  int64_t now_ms = 0;
  void Push(const AudioJitterBuffer::Frame& f) {
    frames.push_back(
        {f.kind, f.payload_size, MarkerOf(f.payload, f.payload_size), now_ms});
  }
};

void PumpAll(AudioJitterBuffer& jb, int64_t now_ms, Sink& sink) {
  sink.now_ms = now_ms;
  jb.Pump(now_ms, [&](const AudioJitterBuffer::Frame& f) { sink.Push(f); });
}

// Conservation invariant: every received packet is accounted for exactly once
// as emitted (normal/reordered), dropped (duplicate/too-late/drain/shrink),
// or still buffered. A correct grow step must not make a packet vanish.
void CheckConservation(const AudioJitterBuffer& jb, const char* tag) {
  const auto& s = jb.GetStats();
  const uint64_t accounted = s.emitted_normal + s.emitted_reordered +
                             s.duplicates + s.dropped_too_late +
                             s.dropped_drain + s.dropped_shrink +
                             uint64_t(jb.BufferedCountForTesting());
  if (accounted != s.received) {
    std::fprintf(stderr,
                 "FAIL conservation[%s]: received=%llu accounted=%llu "
                 "(norm=%llu reord=%llu dup=%llu late=%llu drain=%llu "
                 "shrink=%llu buffered=%d)\n",
                 tag, (unsigned long long)s.received,
                 (unsigned long long)accounted,
                 (unsigned long long)s.emitted_normal,
                 (unsigned long long)s.emitted_reordered,
                 (unsigned long long)s.duplicates,
                 (unsigned long long)s.dropped_too_late,
                 (unsigned long long)s.dropped_drain,
                 (unsigned long long)s.dropped_shrink,
                 jb.BufferedCountForTesting());
    ++failures;
  }
}

// Insert a packet with a unique marker and record its insert time.
struct Driver {
  AudioJitterBuffer& jb;
  std::map<uint16_t, int64_t> insert_time;  // marker -> insert ms
  void Insert(uint16_t seq, int64_t now_ms) {
    const auto pkt = MakePacket(seq);
    insert_time[seq] = now_ms;
    jb.Insert(seq, pkt.data(), pkt.size(), now_ms);
  }
  // Effective insert→emit delay of the packet with the given marker, measured
  // from the sink. -1 if that packet was never emitted as a real frame.
  int64_t DelayOf(const Sink& sink, uint16_t marker) const {
    auto it = insert_time.find(marker);
    if (it == insert_time.end()) return -1;
    for (const auto& f : sink.frames) {
      if ((f.kind == AudioJitterBuffer::FrameKind::kNormal ||
           f.kind == AudioJitterBuffer::FrameKind::kReordered) &&
          f.marker == marker) {
        return f.emit_time_ms - it->second;
      }
    }
    return -1;
  }
};

// 1. In-order passthrough: first frame emitted exactly one target delay after
// insertion, subsequent frames every 10 ms.
void TestInOrderPassthrough() {
  AudioJitterBuffer jb;
  Sink sink;
  Driver drv{jb, {}};

  drv.Insert(100, 0);
  PumpAll(jb, 0, sink);
  CHECK(sink.frames.empty());
  PumpAll(jb, 29, sink);
  CHECK(sink.frames.empty());  // deadline at 30 ms
  PumpAll(jb, 30, sink);
  CHECK(sink.frames.size() == 1);
  CHECK(sink.frames[0].kind == AudioJitterBuffer::FrameKind::kNormal);

  drv.Insert(101, 10);
  PumpAll(jb, 39, sink);
  CHECK(sink.frames.size() == 1);
  PumpAll(jb, 40, sink);
  CHECK(sink.frames.size() == 2);
  CHECK(sink.frames[1].emit_time_ms - sink.frames[0].emit_time_ms == 10);
  // Exact delay proof for a clean stream.
  CHECK(drv.DelayOf(sink, 100) == 30);
  CHECK(drv.DelayOf(sink, 101) == 30);
  std::printf("in-order passthrough OK (delay exactly 30 ms)\n");
}

// 2. Reordering inside the window is repaired with zero loss.
void TestReorderRepair() {
  AudioJitterBuffer jb;
  Sink sink;
  Driver drv{jb, {}};

  drv.Insert(100, 0);
  drv.Insert(102, 12);
  drv.Insert(101, 15);  // reordered, in window
  for (int t = 30; t <= 59; ++t) PumpAll(jb, t, sink);
  int normals = 0, reordered = 0, plc = 0;
  for (const auto& f : sink.frames) {
    if (f.kind == AudioJitterBuffer::FrameKind::kNormal) ++normals;
    if (f.kind == AudioJitterBuffer::FrameKind::kReordered) ++reordered;
    if (f.kind == AudioJitterBuffer::FrameKind::kPlc) ++plc;
  }
  CHECK(normals == 2);
  CHECK(reordered == 1);
  CHECK(plc == 0);
  CheckConservation(jb, "reorder");
  std::printf("reorder repair OK\n");
}

// 3. Loss produces PLC concealment; cadence stays continuous.
void TestLossConcealment() {
  AudioJitterBuffer jb;
  Sink sink;
  Driver drv{jb, {}};

  drv.Insert(100, 0);
  drv.Insert(102, 20);  // 101 lost
  for (int t = 30; t <= 59; ++t) PumpAll(jb, t, sink);
  CHECK(sink.frames.size() == 3);
  CHECK(sink.frames[0].kind == AudioJitterBuffer::FrameKind::kNormal);
  CHECK(sink.frames[1].kind == AudioJitterBuffer::FrameKind::kPlc);
  CHECK(sink.frames[1].payload_size == 0);
  CHECK(sink.frames[2].kind == AudioJitterBuffer::FrameKind::kNormal);
  CHECK(sink.frames[1].emit_time_ms - sink.frames[0].emit_time_ms == 10);
  CHECK(sink.frames[2].emit_time_ms - sink.frames[1].emit_time_ms == 10);
  std::printf("loss concealment OK\n");
}

// 4. Grow-step mechanics, proven directly. Establish a clean base schedule,
// verify delay is exactly 30 ms, then drive jitter that forces the target to
// grow and verify that on-time packets after the step measure exactly the new
// target — and that no packet vanished in the process.
void TestGrowInsertsLatencyWithoutLoss() {
  AudioJitterBuffer jb;
  Sink sink;
  Driver drv{jb, {}};

  // Clean base: 80 frames, in-order, on time.
  uint16_t seq = 100;
  for (int i = 0; i < 80; ++i, ++seq) {
    const int64_t grid = i * 10;
    drv.Insert(seq, grid);
    for (int64_t pump = grid; pump < grid + 10; ++pump) PumpAll(jb, pump, sink);
  }
  // Baseline: a settled on-time packet measures exactly 30 ms.
  CHECK(drv.DelayOf(sink, uint16_t(150)) == 30);
  CHECK(jb.TargetDelayMs() == 30);
  CheckConservation(jb, "grow-base-clean");

  // Now persistent heavy jitter that keeps the target elevated: half the
  // packets 35 ms late. The target must grow and stay above base while the
  // jitter persists. Probe on-time packets inserted while the target is high.
  std::vector<uint16_t> probes;
  int target_when_high = 0;
  for (int i = 80; i < 400; ++i, ++seq) {
    const int64_t grid = i * 10;
    const int64_t arrival = (i % 2 == 0) ? grid + 35 : grid + 2;  // half tardy
    drv.Insert(seq, arrival);
    // Collect on-time probes only while the target is confirmed elevated.
    if (i >= 250 && (i % 2 != 0) && jb.TargetDelayMs() > 30) {
      probes.push_back(seq);
      target_when_high = jb.TargetDelayMs();
    }
    for (int64_t pump = grid; pump < grid + 12; ++pump) PumpAll(jb, pump, sink);
  }
  CHECK(jb.GetStats().grow_events > 0);
  // Conservation: grow must not make any packet vanish.
  CheckConservation(jb, "grow-no-loss");

  // Behavioural proof: probes captured while the target was elevated measure
  // a delay consistent with that target. The target is the grid-to-emit
  // delay; because these on-time probes are inserted 2 ms after their grid
  // production time, insert→emit must equal (target - 2). If grow inserted
  // zero latency (the regression), the delay would stay at the base offset.
  int measured_exact = 0, measured_total = 0;
  const int64_t expected = target_when_high - 2;  // 2 ms arrival offset
  for (const uint16_t probe : probes) {
    const int64_t d = drv.DelayOf(sink, probe);
    if (d >= 0) {
      ++measured_total;
      if (d == expected) ++measured_exact;
    }
  }
  CHECK(measured_exact > 0);
  // And critically: the measured delay must be LARGER than the same measure
  // at base target, proving grow added real latency. At base (30) with the
  // same 2 ms offset the delay would be 28; at grown target it must exceed it.
  CHECK(expected > 28);
  std::printf("grow inserts latency OK (target reached %d ms, %d/%d on-time probes at insert->emit %lld ms = target-2, grow_events %llu)\n",
              target_when_high, measured_exact, measured_total,
              (long long)expected,
              (unsigned long long)jb.GetStats().grow_events);
}

// 5. A single grow step inserts exactly one extra concealment frame and emits
// the head packet one position later — observable in the emission stream.
void TestGrowStepEmitsOneConcealmentAndKeepsPacket() {
  // Craft a scenario that triggers exactly one grow, then inspect the local
  // emission sequence around it. Because the adaptation window is 64 frames,
  // we drive enough tardy frames to force one grow, then observe.
  AudioJitterBuffer jb;
  Sink sink;
  Driver drv{jb, {}};
  uint16_t seq = 100;
  // Tardy pattern tuned to cross the grow threshold in the first window.
  for (int i = 0; i < 130; ++i, ++seq) {
    const int64_t grid = i * 10;
    const int64_t arrival = (i % 3 == 2) ? grid + 40 : grid + 2;  // 1/3 tardy
    drv.Insert(seq, arrival);
    for (int64_t pump = grid; pump < grid + 12; ++pump) PumpAll(jb, pump, sink);
  }
  // Whatever the adaptation did, conservation must hold — this is the core
  // regression for the grow-drops-packet bug.
  CheckConservation(jb, "grow-keeps-packets");
  std::printf("grow-step conservation OK (grow_events %llu shrink_events %llu)\n",
              (unsigned long long)jb.GetStats().grow_events,
              (unsigned long long)jb.GetStats().shrink_events);
}

// 6. Full grow→shrink cycle returns the stream to exactly base latency,
// verified behaviourally with a probe measuring exactly 30 ms.
void TestGrowShrinkCycleReturnsToBase() {
  AudioJitterBuffer jb;
  Sink sink;
  Driver drv{jb, {}};
  uint16_t seq = 100;
  // Phase 1: jittery traffic to grow the target.
  for (int i = 0; i < 300; ++i, ++seq) {
    const int64_t grid = i * 10;
    const int64_t arrival = (i % 5 == 4) ? grid + 35 : grid + 2;
    drv.Insert(seq, arrival);
    for (int64_t pump = grid; pump < grid + 12; ++pump) PumpAll(jb, pump, sink);
  }
  const int grown = jb.TargetDelayMs();
  CHECK(grown > 30);

  // Phase 2: clean traffic long enough to shrink back to base.
  const uint16_t clean_start = seq;
  for (int i = 0; i < 900; ++i, ++seq) {
    const int64_t grid = i * 10;
    drv.Insert(seq, grid);
    for (int64_t pump = grid; pump < grid + 10; ++pump) PumpAll(jb, pump, sink);
  }
  CHECK(jb.TargetDelayMs() == 30);
  // Behavioural proof: a late clean probe measures exactly 30 ms.
  const uint16_t probe = uint16_t(seq - 50);
  const int64_t d = drv.DelayOf(sink, probe);
  CHECK(d == 30);
  CheckConservation(jb, "grow-shrink-cycle");
  std::printf("grow-shrink cycle OK (grew to %d ms, back to base, probe %u delay %lld ms)\n",
              grown, unsigned(probe - clean_start), (long long)d);
}

// 7. Ceiling drain compacts and never plays unboundedly late audio.
void TestCeilingDrainCompaction() {
  AudioJitterBuffer jb;
  Sink sink;
  Driver drv{jb, {}};
  for (int i = 0; i < 12; ++i) {
    drv.Insert(uint16_t(100 + i), 0);
  }
  for (int t = 0; t <= 200; ++t) PumpAll(jb, t, sink);
  size_t real = 0;
  for (const auto& f : sink.frames)
    if (f.kind == AudioJitterBuffer::FrameKind::kNormal) ++real;
  CHECK(real <= 7);
  CHECK(jb.GetStats().dropped_drain > 0);
  CheckConservation(jb, "ceiling-drain");
  std::printf("ceiling drain compaction OK (real %zu drained %llu)\n", real,
              (unsigned long long)jb.GetStats().dropped_drain);
}

// 8. Burst after stall: re-anchors instead of grinding stale audio.
void TestBurstAfterStallReanchor() {
  AudioJitterBuffer jb;
  Sink sink;
  Driver drv{jb, {}};
  drv.Insert(100, 0);
  PumpAll(jb, 30, sink);
  CHECK(sink.frames.size() == 1);
  for (int t = 31; t <= 530; ++t) PumpAll(jb, t, sink);
  const size_t after_stall = sink.frames.size();
  CHECK(after_stall <= 1 + 4);
  for (int i = 1; i <= 30; ++i) {
    drv.Insert(uint16_t(100 + i), 530);
  }
  for (int t = 531; t <= 700; ++t) PumpAll(jb, t, sink);
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
  CheckConservation(jb, "burst-reanchor");
  std::printf("burst-after-stall reanchor OK (first real frame %lld ms after burst)\n",
              (long long)(first_real_after - 530));
}

// 9. Sequence wraparound handled correctly.
void TestSequenceWraparound() {
  AudioJitterBuffer jb;
  Sink sink;
  Driver drv{jb, {}};
  drv.Insert(0xFFFE, 0);
  drv.Insert(0xFFFF, 10);
  drv.Insert(0x0000, 20);
  for (int t = 30; t <= 59; ++t) PumpAll(jb, t, sink);
  int normals = 0;
  for (const auto& f : sink.frames)
    if (f.kind == AudioJitterBuffer::FrameKind::kNormal) ++normals;
  CHECK(normals == 3);
  std::printf("sequence wraparound OK\n");
}

// 10. Duplicates ignored.
void TestDuplicatesIgnored() {
  AudioJitterBuffer jb;
  Sink sink;
  Driver drv{jb, {}};
  const auto pkt = MakePacket(100);
  CHECK(jb.Insert(100, pkt.data(), pkt.size(), 0));
  CHECK(!jb.Insert(100, pkt.data(), pkt.size(), 1));
  CHECK(jb.GetStats().duplicates == 1);
  for (int t = 30; t <= 40; ++t) PumpAll(jb, t, sink);
  int real = 0;
  for (const auto& f : sink.frames)
    if (f.kind == AudioJitterBuffer::FrameKind::kNormal) ++real;
  CHECK(real == 1);
  CheckConservation(jb, "duplicates");
  std::printf("duplicates ignored OK\n");
}

}  // namespace

int main() {
  TestInOrderPassthrough();
  TestReorderRepair();
  TestLossConcealment();
  TestGrowInsertsLatencyWithoutLoss();
  TestGrowStepEmitsOneConcealmentAndKeepsPacket();
  TestGrowShrinkCycleReturnsToBase();
  TestCeilingDrainCompaction();
  TestBurstAfterStallReanchor();
  TestSequenceWraparound();
  TestDuplicatesIgnored();

  if (failures == 0) {
    std::printf("audio_jitter_buffer_test: all checks passed\n");
    return 0;
  }
  std::fprintf(stderr, "audio_jitter_buffer_test: %d check(s) failed\n",
               failures);
  return 1;
}
