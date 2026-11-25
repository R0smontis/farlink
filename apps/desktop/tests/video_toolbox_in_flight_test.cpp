/*
 * Copyright (c) 2026 Farlink contributors
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */

// Behavioural verification of the leak-proof encode in-flight accounting.
//
// This test drives the real minirtc::VideoToolboxEncoder — the asynchronous
// hardware encoder whose output callback fires on a VideoToolbox thread after
// Encode() has returned — and asserts that FramesInFlight() (derived from the
// pending-callback registry: +1 on registration, exactly one consumption per
// registration) can never leak:
//
//   case 1  Encode a burst of real NV12 frames, wait for the pipeline to
//           drain, and observe FramesInFlight() return exactly to zero.
//   case 2  While frames are inside the hardware pipeline, FramesInFlight()
//           must be observed strictly positive — proving the count is driven
//           by real submissions and not pinned at zero. A dedicated sampler
//           thread spins on the counter for the whole submit+drain window so
//           even transient positive values are caught.
//   case 3  Destroying an encoder with frames still pending must not crash
//           and must not deliver any output callback afterwards; the
//           destructor cancels all outstanding registrations (the count is
//           reclaimed inside the object being destroyed).
//   case 4  A mid-stream resolution change forces the encoder to invalidate
//           and recreate its VTCompressionSession, cancelling every pending
//           registration of the old session. After the drain the count must
//           return to zero, and post-change frames — identified by their
//           encoded dimensions — must still produce output callbacks.
//   case 5  A synchronous swallow encoder (Encode() returns success but
//           never invokes the output callback) uses the MediaCodec base-class
//           default FramesInFlight() == 0 forever. This is what makes the
//           transport's admission gate correct for synchronous encoders: a
//           swallowed frame can never wedge the gate because nothing is ever
//           counted as in flight.
//
// Timing robustness: all frames are 1920x1080 or 1280x720. At these sizes
// the hardware encode of one frame (milliseconds) far exceeds the cost of
// one Encode() call (a 3 MB copy plus submission, well under a millisecond),
// so a burst of submissions necessarily overlaps inside the pipeline. Small
// frames (640x480) were tried first and are NOT usable here: hardware
// completes them faster than the submission loop, so the in-flight count
// oscillates back to zero between frames and no overlap is guaranteed.
//
// Exactly-once accounting is verified per frame: every frame carries a
// distinct capture timestamp whose low bits encode its slot index, and the
// output path preserves the presentation timestamp, so each slot's callback
// count must never exceed 1. Hardware-dropped frames legitimately produce no
// user callback (the registration is still consumed exactly once — that is
// what the count tracks), so the total callback count is asserted <= the
// submission count, never ==.
//
// macOS only: the encoder under test is the Apple VideoToolbox backend.
// Prints PASS on success and returns 0; any failure returns 1.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

#include "media_codec.h"
#include "raw_frame.h"
#include "clock/system_clock.h"
#include "video_toolbox_encoder.h"

namespace {

constexpr uint32_t kWidth = 1920;
constexpr uint32_t kHeight = 1080;
constexpr uint32_t kAltWidth = 1280;
constexpr uint32_t kAltHeight = 720;

int g_failures = 0;

void Expect(bool condition, const char* what) {
  if (condition) {
    std::printf("  ok: %s\n", what);
  } else {
    std::printf("  FAIL: %s\n", what);
    ++g_failures;
  }
}

// Packed NV12 frame with a moving gradient so consecutive frames differ.
// `seq` shifts the pattern per frame.
std::vector<uint8_t> MakeNv12Frame(uint32_t width, uint32_t height,
                                   uint32_t seq) {
  std::vector<uint8_t> frame(static_cast<size_t>(width) * height * 3 / 2);
  uint8_t* y = frame.data();
  for (uint32_t row = 0; row < height; ++row) {
    for (uint32_t col = 0; col < width; ++col) {
      y[row * width + col] =
          static_cast<uint8_t>((row + col + seq * 7) & 0xff);
    }
  }
  uint8_t* uv = frame.data() + static_cast<size_t>(width) * height;
  const size_t uv_size = static_cast<size_t>(width) * height / 2;
  for (size_t i = 0; i < uv_size; ++i) {
    uv[i] = static_cast<uint8_t>((128 + seq) & 0xff);
  }
  return frame;
}

minirtc::MediaCodecConfig MakeConfig(uint32_t width, uint32_t height) {
  minirtc::MediaCodecConfig config;
  config.init_width = static_cast<int>(width);
  config.init_height = static_cast<int>(height);
  config.max_frame_rate = 60;
  config.min_frame_rate = 30;
  config.average_bitrate = 2'500'000;
  config.max_bitrate = 8'000'000;
  config.init_bitrate = 2'500'000;
  return config;
}

// Polls `predicate` until it holds or the timeout elapses. Generous timeouts
// and short polling intervals: the VT output callback latency is hardware
// dependent, so nothing here may rely on fixed sleeps.
template <typename Predicate>
bool WaitUntil(Predicate predicate, int timeout_ms) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return predicate();
}

// Spins on FramesInFlight() for the lifetime of the object, recording the
// maximum. Hardware encode windows are milliseconds wide; a spinning sampler
// on another core cannot miss them. Must be stopped (destruction) before the
// encoder it watches goes away.
class InFlightSampler {
 public:
  explicit InFlightSampler(const minirtc::VideoToolboxEncoder* encoder)
      : encoder_(encoder),
        thread_([this] { Run(); }) {}

  ~InFlightSampler() {
    stop_.store(true, std::memory_order_release);
    thread_.join();
  }

  int MaxObserved() const { return max_observed_.load(std::memory_order_acquire); }

 private:
  void Run() {
    while (!stop_.load(std::memory_order_acquire)) {
      const int in_flight = encoder_->FramesInFlight();
      int observed = max_observed_.load(std::memory_order_relaxed);
      while (in_flight > observed &&
             !max_observed_.compare_exchange_weak(observed, in_flight,
                                                  std::memory_order_relaxed)) {
      }
      if (in_flight < 0) {
        negative_observed_.store(true, std::memory_order_relaxed);
      }
      std::this_thread::yield();
    }
  }

  const minirtc::VideoToolboxEncoder* encoder_;
  std::atomic<bool> stop_{false};
  std::atomic<int> max_observed_{0};
  std::thread thread_;

 public:
  // A negative count means a registration was consumed twice — the exact
  // leak-in-reverse this accounting exists to prevent.
  std::atomic<bool> negative_observed_{false};
};

// Counts output callbacks per frame slot. Frame i of a burst carries capture
// timestamp base + i; the encoder copies the presentation timestamp into
// EncodedFrame::CapturedTimestamp(), so each slot counts exactly how often
// its frame's user callback fired. A count above 1 means a registration
// produced two callbacks; a count of 0 means the frame was hardware-dropped
// or cancelled — both legal single consumptions.
struct SlotCounter {
  explicit SlotCounter(int slots) : counts(slots) {
    for (auto& count : counts) count.store(0, std::memory_order_relaxed);
  }

  // Returns a callback closure stamping into `base`-relative slots.
  std::function<int(const minirtc::EncodedFrame&)> Callback(int64_t base) {
    return [this, base](const minirtc::EncodedFrame& frame) -> int {
      total.fetch_add(1, std::memory_order_acq_rel);
      const int64_t slot = frame.CapturedTimestamp() - base;
      if (slot >= 0 && slot < static_cast<int64_t>(counts.size())) {
        counts[static_cast<size_t>(slot)].fetch_add(1,
                                                    std::memory_order_acq_rel);
      } else {
        unattributed.fetch_add(1, std::memory_order_acq_rel);
      }
      if (frame.EncodedWidth() == kAltWidth) {
        alt_width_total.fetch_add(1, std::memory_order_acq_rel);
      } else if (frame.EncodedWidth() == kWidth) {
        full_width_total.fetch_add(1, std::memory_order_acq_rel);
      }
      return 0;
    };
  }

  int MaxSlotCount() const {
    int max_count = 0;
    for (const auto& count : counts) {
      max_count = std::max(max_count, count.load(std::memory_order_acquire));
    }
    return max_count;
  }

  std::vector<std::atomic<int>> counts;
  std::atomic<int> total{0};
  std::atomic<int> unattributed{0};
  // Classification by encoded dimensions, robust even if the presentation
  // timestamp does not round-trip.
  std::atomic<int> full_width_total{0};
  std::atomic<int> alt_width_total{0};
};

// Case 1 + 2: drain-to-zero and non-zero-while-pending on the real encoder.
void TestDrainToZero() {
  std::printf("case 1/2: encode a burst, observe in-flight while pending, "
              "drain to zero\n");
  auto clock = std::make_shared<minirtc::SystemClock>();
  minirtc::VideoToolboxEncoder encoder(clock, minirtc::VideoCodecType::H264);
  if (encoder.Init(MakeConfig(kWidth, kHeight)) != 0) {
    std::printf("  FAIL: encoder Init failed (no VideoToolbox session)\n");
    ++g_failures;
    return;
  }

  constexpr int kFrameCount = 12;
  constexpr int64_t kPtsBase = 1'000'000;
  SlotCounter slots(kFrameCount);
  auto on_encoded = slots.Callback(kPtsBase);

  int submitted = 0;
  {
    InFlightSampler sampler(&encoder);
    for (int i = 0; i < kFrameCount; ++i) {
      std::vector<uint8_t> nv12 = MakeNv12Frame(kWidth, kHeight, i);
      minirtc::RawFrame frame(nv12.data(), nv12.size(), kWidth, kHeight);
      frame.SetCapturedTimestamp(kPtsBase + i);
      if (encoder.Encode(frame, on_encoded) != 0) {
        std::printf("  FAIL: Encode() rejected frame %d\n", i);
        ++g_failures;
        return;
      }
      ++submitted;
    }

    // Case 2: at some point between submission and completion the count must
    // have been positive. At 1920x1080 each frame spends milliseconds inside
    // the hardware pipeline while submission costs a fraction of that, so
    // the pipeline necessarily overlaps during the burst; the sampler spins
    // through the entire window including the drain tail, where the final
    // frame's registration is pending for its whole encode duration.
    const bool drained = WaitUntil(
        [&] { return encoder.FramesInFlight() == 0; }, 10'000);

    Expect(sampler.MaxObserved() > 0,
           "FramesInFlight() observed > 0 while frames pending");
    Expect(!sampler.negative_observed_.load(),
           "FramesInFlight() never went negative (no double consumption)");
    Expect(drained, "in-flight drained within timeout");
    std::printf("  max in-flight observed: %d, callbacks: %d/%d "
                "(unattributed pts: %d)\n",
                sampler.MaxObserved(), slots.total.load(), submitted,
                slots.unattributed.load());
  }

  Expect(encoder.FramesInFlight() == 0,
         "FramesInFlight() == 0 after drain");
  Expect(slots.total.load() >= 1,
         "output callbacks fire for real encoded frames");
  Expect(slots.total.load() <= submitted,
         "output callbacks never exceed submissions");
  Expect(slots.MaxSlotCount() <= 1,
         "no frame's output callback ever fired twice");
}

// Case 3: teardown with frames pending must cancel every registration and
// never deliver a callback into dead state.
void TestTeardownWithPending() {
  std::printf("case 3: destroy encoder while frames still pending\n");
  constexpr int kBurst = 24;
  constexpr int64_t kPtsBase = 2'000'000;
  SlotCounter slots(kBurst);
  int pending_at_exit = -1;
  int submitted = 0;
  {
    auto clock = std::make_shared<minirtc::SystemClock>();
    minirtc::VideoToolboxEncoder encoder(clock,
                                         minirtc::VideoCodecType::H264);
    if (encoder.Init(MakeConfig(kWidth, kHeight)) != 0) {
      std::printf("  FAIL: encoder Init failed\n");
      ++g_failures;
      return;
    }
    auto on_encoded = slots.Callback(kPtsBase);
    for (int i = 0; i < kBurst; ++i) {
      std::vector<uint8_t> nv12 = MakeNv12Frame(kWidth, kHeight, i + 100);
      minirtc::RawFrame frame(nv12.data(), nv12.size(), kWidth, kHeight);
      frame.SetCapturedTimestamp(kPtsBase + i);
      if (encoder.Encode(frame, on_encoded) != 0) {
        std::printf("  FAIL: Encode() rejected frame %d\n", i);
        ++g_failures;
        return;
      }
      ++submitted;
    }
    // The final frame's registration exists until the hardware finishes
    // encoding it — milliseconds at 1920x1080 — while this read runs
    // microseconds after submission, so frames are still pending here on any
    // real hardware.
    pending_at_exit = encoder.FramesInFlight();
    std::printf("  submitted=%d, in-flight at scope exit=%d\n", submitted,
                pending_at_exit);
    // Destructor runs here: it invalidates the VT session, cancels all
    // pending registrations (reclaiming the in-flight count inside the dying
    // object) and waits for any output callback already executing. It may
    // block briefly while a callback in progress finishes — that is
    // documented behaviour, not a drain of the pending queue.
  }
  Expect(pending_at_exit > 0,
         "frames genuinely pending at destruction (teardown path exercised)");
  // After destruction no output callback may fire: invalidation stops VT
  // output delivery and the destructor waits out callbacks already in
  // progress, so the count must be frozen from the moment the object is
  // gone. A window far longer than any encode latency gives stray callbacks
  // every chance to appear.
  const int count_after_destroy = slots.total.load(std::memory_order_acquire);
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  const int count_later = slots.total.load(std::memory_order_acquire);
  Expect(count_later == count_after_destroy,
         "no output callback fires after encoder destruction");
  Expect(slots.MaxSlotCount() <= 1,
         "no frame's output callback ever fired twice");
  // Reaching here at all proves the teardown did not crash and no
  // use-after-free in a late callback tore the process down — the scenario
  // this case guards against. Asserting a post-destruction FramesInFlight()
  // value is impossible by construction (the object is gone), so the
  // observable contract is crash freedom plus the frozen callback count.
  Expect(true, "teardown with pending frames did not crash");
}

// Case 4: resolution change forces VT session recreation, cancelling all
// pending registrations of the old session mid-stream.
void TestResolutionChangeCancelPath() {
  std::printf("case 4: resolution change mid-stream cancels pending frames, "
              "drains to zero\n");
  auto clock = std::make_shared<minirtc::SystemClock>();
  minirtc::VideoToolboxEncoder encoder(clock, minirtc::VideoCodecType::H264);
  if (encoder.Init(MakeConfig(kWidth, kHeight)) != 0) {
    std::printf("  FAIL: encoder Init failed\n");
    ++g_failures;
    return;
  }

  constexpr int kPreChange = 8;
  constexpr int kPostChange = 8;
  constexpr int64_t kPtsBase = 3'000'000;
  SlotCounter slots(kPreChange + kPostChange);
  auto on_encoded = slots.Callback(kPtsBase);

  {
    InFlightSampler sampler(&encoder);
    int pre_submitted = 0;
    for (int i = 0; i < kPreChange; ++i) {
      std::vector<uint8_t> nv12 = MakeNv12Frame(kWidth, kHeight, i + 200);
      minirtc::RawFrame frame(nv12.data(), nv12.size(), kWidth, kHeight);
      frame.SetCapturedTimestamp(kPtsBase + i);
      if (encoder.Encode(frame, on_encoded) != 0) {
        std::printf("  FAIL: pre-change Encode() rejected frame %d\n", i);
        ++g_failures;
        return;
      }
      ++pre_submitted;
    }
    // Same argument as case 3: the last pre-change frame cannot have left
    // the hardware pipeline microseconds after its submission.
    const int pending_before_change = encoder.FramesInFlight();
    std::printf("  in-flight immediately before resolution change: %d\n",
                pending_before_change);
    Expect(pending_before_change > 0,
           "frames pending when the session is recreated (cancel path "
           "exercised)");

    // Switch resolution: Encode() detects the dimension mismatch and calls
    // ResetEncodeResolution, which invalidates the running session and
    // cancels every registration still pending in it. Whether a given
    // pending frame resolves through the cancel path or happens to complete
    // first is a hardware race — but every registration is consumed exactly
    // once either way, which is what the count must reflect.
    int post_submitted = 0;
    for (int i = 0; i < kPostChange; ++i) {
      std::vector<uint8_t> nv12 = MakeNv12Frame(kAltWidth, kAltHeight, 50 + i);
      minirtc::RawFrame frame(nv12.data(), nv12.size(), kAltWidth, kAltHeight);
      frame.SetCapturedTimestamp(kPtsBase + kPreChange + i);
      if (encoder.Encode(frame, on_encoded) != 0) {
        std::printf("  FAIL: post-change Encode() rejected frame %d\n", i);
        ++g_failures;
        return;
      }
      ++post_submitted;
    }

    const bool drained = WaitUntil(
        [&] { return encoder.FramesInFlight() == 0; }, 10'000);
    Expect(drained, "FramesInFlight() == 0 after resolution-change drain");
    Expect(!sampler.negative_observed_.load(),
           "FramesInFlight() never went negative");
    std::printf("  callbacks: total=%d full-res=%d alt-res=%d "
                "(pre-submitted=%d post-submitted=%d)\n",
                slots.total.load(), slots.full_width_total.load(),
                slots.alt_width_total.load(), pre_submitted, post_submitted);
  }

  Expect(encoder.FramesInFlight() == 0,
         "FramesInFlight() still 0 after full drain");
  // The recreated session must be functional: frames encoded after the
  // resolution change are identifiable by their encoded dimensions.
  Expect(slots.alt_width_total.load() >= 1,
         "output callbacks arrive for post-resolution-change frames");
  // Exactly-once across every resolution path: cancelled frames produce no
  // callback, completed frames produce one, none ever produce two.
  Expect(slots.total.load() <= kPreChange + kPostChange,
         "output callbacks never exceed submissions");
  Expect(slots.MaxSlotCount() <= 1,
         "no frame's output callback ever fired twice");
}

// Case 5: synchronous swallow encoder — Encode() returns success and never
// invokes the output callback, exactly like a dropped frame in a synchronous
// software encoder. The MediaCodec base default keeps FramesInFlight() at 0,
// so the transport admission gate can never be wedged by swallowed frames.
class SwallowEncoder : public minirtc::MediaCodec {
 public:
  int Encode(const minirtc::RawFrame&,
             std::function<int(const minirtc::EncodedFrame&)>) override {
    // Frame accepted and silently dropped: no output callback, ever.
    return 0;
  }
};

void TestSynchronousSwallowEncoder() {
  std::printf("case 5: synchronous swallow encoder never gated\n");
  SwallowEncoder encoder;
  constexpr int kSwallowed = 32;
  for (int i = 0; i < kSwallowed; ++i) {
    std::vector<uint8_t> nv12 = MakeNv12Frame(kWidth, kHeight, i);
    minirtc::RawFrame frame(nv12.data(), nv12.size(), kWidth, kHeight);
    if (encoder.Encode(frame, [](const minirtc::EncodedFrame&) -> int {
          return 0;
        }) != 0) {
      std::printf("  FAIL: swallow encoder rejected frame %d\n", i);
      ++g_failures;
      return;
    }
    if (encoder.FramesInFlight() != 0) {
      std::printf("  FAIL: swallow encoder reported %d in flight after frame "
                  "%d\n",
                  encoder.FramesInFlight(), i);
      ++g_failures;
      return;
    }
  }
  Expect(encoder.FramesInFlight() == 0,
         "synchronous swallow encoder FramesInFlight() always 0");
}

}  // namespace

int main() {
#if defined(__APPLE__)
  TestDrainToZero();
  TestTeardownWithPending();
  TestResolutionChangeCancelPath();
  TestSynchronousSwallowEncoder();

  if (g_failures == 0) {
    std::printf("PASS\n");
    return 0;
  }
  std::printf("FAIL (%d assertion(s) failed)\n", g_failures);
  return 1;
#else
  std::printf("PASS (skipped: VideoToolbox is macOS-only)\n");
  return 0;
#endif
}
