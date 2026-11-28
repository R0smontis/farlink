/*
 * Copyright (c) 2026 Farlink contributors
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */

// Test-only static-link interposer for the zero-encoded failure-path test.
//
// Linked ONLY into the frame_rate_fail_harness test target, never into the
// production app or the healthy frame_rate_harness. It defines every externally
// referenced VideoEncoderFactory symbol so the real video_encoder_factory.cpp
// member is never pulled out of libminirtc.a at link time, and substitutes an
// encoder whose Init() succeeds but whose Encode() returns -1 for every frame.
// That is exactly the Windows zero-encoded session shape (codec created,
// bitrate negotiated, every encode rejected) reproduced through the real
// IceTransportController encode queue, so the failure-side send-stage
// diagnostics (streak log, stall WARN, teardown funnel) can be verified
// without touching production code.
//
// Mechanism notes:
// - ld64 pulls archive members only for undefined symbols; the harness objects
//   precede libminirtc.a on the link line, so these definitions win and the
//   factory TU inside the archive is never extracted. Every factory symbol an
//   external TU can reference is defined here so no undefined reference can
//   pull it back in.
// - Platform reach: built and verified on macOS ld64 only (2026-11-28, including
//   a binary-level check that the real factory's strings are absent). The same
//   archive-member semantics are expected of GNU ld on Linux but are untested
//   there. The target is gated off Windows: MSVC link.exe pulls archive members
//   under different rules and this file's coverage of the factory's
//   Windows-only symbols is deliberately absent, so interposition there is
//   unproven by construction.
// - The decoder factory is a separate TU and is NOT interposed: the receiver
//   peer decodes with the real decoder chain, exactly as in the healthy harness.
// - TryInitEncoder is private and referenced only inside the factory TU, so it
//   needs no definition here.

#include <cstdlib>
#include <memory>
#include <string>

#include "media_codec.h"
#include "video_encoder_factory.h"
#ifdef __APPLE__
#include <TargetConditionals.h>
#endif
#if defined(__APPLE__) && TARGET_OS_OSX
#include "video_toolbox_encoder.h"
#endif

namespace minirtc {

namespace {

// Encode() always returns -1 without invoking the callback, mirroring an
// encoder whose session accepts submission but rejects every frame. Init
// succeeds so the transport never retries codec creation: the failure is
// purely per-frame, which is what the consecutive_failures streak and the
// encode_fail funnel counters measure.
class FailingVideoEncoder : public MediaCodec {
 public:
  explicit FailingVideoEncoder(std::shared_ptr<SystemClock> clock) {
    (void)clock;
  }

  int Init(const MediaCodecConfig& config) override {
    width_ = config.init_width;
    height_ = config.init_height;
    return 0;
  }

  int Encode(const RawFrame& raw_frame,
             std::function<int(const EncodedFrame&)> on_encoded_image) override {
    (void)raw_frame;
    (void)on_encoded_image;
    return -1;
  }

  int GetResolution(int* width, int* height) const override {
    if (width) *width = width_;
    if (height) *height = height_;
    return 0;
  }

  std::string GetEncoderName() const override {
    return "FailingVideoEncoder(test)";
  }

 private:
  int width_ = 0;
  int height_ = 0;
};

#if defined(__APPLE__) && TARGET_OS_OSX

// Opt-in intermittent mode: fail K consecutive Encode() calls, then delegate M
// calls to a REAL VideoToolboxEncoder, repeating. Activated only when both
// MINIRTC_TEST_FAIL_K >= 1 and MINIRTC_TEST_SUCCEED_M >= 1 are present in the
// environment (read once at construction); absent or zero values mean the
// encoder is never constructed and the always-fail path is byte-for-byte the
// original behaviour. Exists to exercise the consecutive_encode_failures
// reset-on-success path with genuine encoded output: success calls pass the
// frame and callback through untouched, so encoded callbacks, encoded/sent
// counters and the receiver's decode are all real.
//
// The delegate is constructed directly with `new VideoToolboxEncoder` — never
// through the factory, which this file interposes. video_toolbox_encoder.mm
// references no VideoEncoderFactory symbol (its PrimeHardwareEncoder is the
// class's own static), so pulling it out of libminirtc.a cannot re-import the
// real factory TU; the interposition is unaffected. The whole class is
// compiled only on macOS (the delegate is Apple-only): on every other
// platform this class does not exist, MakeTestEncoder never selects it, and
// the env knobs have no effect — the always-fail encoder is unconditional.
class IntermittentVideoEncoder : public MediaCodec {
 public:
  IntermittentVideoEncoder(std::shared_ptr<SystemClock> clock,
                           VideoCodecType codec_type, int fail_k, int succeed_m)
      : clock_(std::move(clock)),
        codec_type_(codec_type),
        fail_k_(fail_k),
        succeed_m_(succeed_m) {}

  int Init(const MediaCodecConfig& config) override {
    width_ = config.init_width;
    height_ = config.init_height;
    delegate_ = std::make_unique<VideoToolboxEncoder>(clock_, codec_type_);
    if (delegate_->Init(config) != 0) delegate_.reset();
    return 0;
  }

  int Encode(const RawFrame& raw_frame,
             std::function<int(const EncodedFrame&)> on_encoded_image) override {
    // Position within the repeating K/M cycle, counted in Encode calls.
    const uint64_t position = encode_calls_++ % static_cast<uint64_t>(fail_k_ + succeed_m_);
    if (position < static_cast<uint64_t>(fail_k_) || !delegate_) {
      return -1;
    }
    return delegate_->Encode(raw_frame, std::move(on_encoded_image));
  }

  int GetResolution(int* width, int* height) const override {
    if (width) *width = width_;
    if (height) *height = height_;
    return 0;
  }

  std::string GetEncoderName() const override {
    return "IntermittentVideoEncoder(test)";
  }

  int SetTargetBitrate(int bitrate) override {
    return delegate_ ? delegate_->SetTargetBitrate(bitrate) : 0;
  }

  int ForceIdr() override { return delegate_ ? delegate_->ForceIdr() : 0; }

  void RequestQualityRefresh() override {
    if (delegate_) delegate_->RequestQualityRefresh();
  }

  int FramesInFlight() const override {
    return delegate_ ? delegate_->FramesInFlight() : 0;
  }

  bool SupportsNativeFrameInput(
      MiniRtcNativeVideoFrameType type) const override {
    return delegate_ ? delegate_->SupportsNativeFrameInput(type) : false;
  }

 private:
  std::shared_ptr<SystemClock> clock_;
  VideoCodecType codec_type_;
  int fail_k_;
  int succeed_m_;
  uint64_t encode_calls_ = 0;
  int width_ = 0;
  int height_ = 0;
  std::unique_ptr<VideoToolboxEncoder> delegate_;
};

#endif  // defined(__APPLE__) && TARGET_OS_OSX

}  // namespace

VideoEncoderFactory::VideoEncoderFactory() {}
VideoEncoderFactory::~VideoEncoderFactory() {}

namespace {

#if defined(__APPLE__) && TARGET_OS_OSX
// Intermittent-mode knobs, read once per encoder construction. Both must be
// positive integers for the mode to engage; any absent/zero/negative value
// keeps the always-fail default exactly. Compiled only on macOS, the sole
// platform where the intermittent encoder exists.
int ReadPositiveEnv(const char* name) {
  const char* value = std::getenv(name);
  if (!value || !*value) return 0;
  const long parsed = std::strtol(value, nullptr, 10);
  return parsed > 0 ? static_cast<int>(parsed) : 0;
}
#endif

std::unique_ptr<MediaCodec> MakeTestEncoder(std::shared_ptr<SystemClock> clock,
                                            VideoCodecType codec_type) {
#if defined(__APPLE__) && TARGET_OS_OSX
  const int fail_k = ReadPositiveEnv("MINIRTC_TEST_FAIL_K");
  const int succeed_m = ReadPositiveEnv("MINIRTC_TEST_SUCCEED_M");
  if (fail_k > 0 && succeed_m > 0) {
    return std::make_unique<IntermittentVideoEncoder>(clock, codec_type, fail_k,
                                                      succeed_m);
  }
#else
  (void)codec_type;
#endif
  return std::make_unique<FailingVideoEncoder>(clock);
}

}  // namespace

std::unique_ptr<MediaCodec> VideoEncoderFactory::CreateVideoEncoder(
    std::shared_ptr<SystemClock> clock, bool hardware_acceleration,
    VideoCodecType codec_type) {
  (void)hardware_acceleration;
  return MakeTestEncoder(std::move(clock), codec_type);
}

std::unique_ptr<MediaCodec> VideoEncoderFactory::CreateInitializedVideoEncoder(
    std::shared_ptr<SystemClock> clock, const MediaCodecConfig& config,
    bool hardware_acceleration, VideoCodecType codec_type) {
  (void)hardware_acceleration;
  auto encoder = MakeTestEncoder(std::move(clock), codec_type);
  if (encoder->Init(config) != 0) return nullptr;
  return encoder;
}

bool VideoEncoderFactory::CheckIsHardwareAccelerationSupported(
    VideoCodecType codec_type) {
  // Reported as supported so codec negotiation proceeds exactly as on a
  // healthy machine; the failure is injected at Encode(), not at creation.
  (void)codec_type;
  return true;
}

void VideoEncoderFactory::PrimeHardwareEncoder(bool hardware_acceleration,
                                               VideoCodecType codec_type) {
  (void)hardware_acceleration;
  (void)codec_type;
}

}  // namespace minirtc
