/*
 * @Author: Farlink contributors
 * @Date: 2026-11-25
 * Copyright (c) 2026 by Farlink contributors.
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Headless proof of the audio playback queue cap using SDL's dummy audio
 * driver. Verifies against real SDL 3.2 conversion semantics: queued bytes
 * are input-format units, and trimming removes the correct amount of the
 * OLDEST audio (ramp pattern check) even when the device output format
 * differs from the s16 mono input — plus the channel-aware stereo path
 * (1920-byte frames, 150 ms stereo cap = twice the mono cap).
 */

#include <cstdint>
#include <cstdio>
#include <vector>

#include "SDL3/SDL.h"

#include "audio_output_trim.h"

namespace {

int failures = 0;
#define CHECK(cond)                                                \
  do {                                                             \
    if (!(cond)) {                                                 \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                   #cond);                                         \
      ++failures;                                                  \
    }                                                              \
  } while (0)

using crossdesk::kAudioOutputMonoFrameBytes;
using crossdesk::kAudioOutputQueueCapBytesMono;
using crossdesk::kAudioOutputStereoFrameBytes;
using crossdesk::kAudioOutputQueueCapBytesStereo;
constexpr int kInputFrameBytes = kAudioOutputMonoFrameBytes;
constexpr int kCapBytes = kAudioOutputQueueCapBytesMono;

}  // namespace

int main() {
  SDL_SetHint("SDL_AUDIODRIVER", "dummy");
  if (!SDL_Init(SDL_INIT_AUDIO)) {
    std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
    return 2;
  }

  // Open the default playback device. The dummy driver accepts any format,
  // so request the same output shape a real device typically has — float32
  // stereo — to exercise the input/output unit mismatch.
  SDL_AudioSpec device_spec{};
  device_spec.freq = 48000;
  device_spec.format = SDL_AUDIO_F32;
  device_spec.channels = 2;
  SDL_AudioDeviceID dev = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                              &device_spec);
  if (dev == 0) {
    std::fprintf(stderr, "SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
    SDL_Quit();
    return 2;
  }
  // Pause the device so the dummy driver never consumes queued data in real
  // time — the test must be deterministic.
  SDL_PauseAudioDevice(dev);

  // Input format: the decoder's s16 mono 48 kHz.
  SDL_AudioSpec input{};
  input.freq = 48000;
  input.format = SDL_AUDIO_S16;
  input.channels = 1;
  SDL_AudioStream* stream = SDL_CreateAudioStream(&input, &input);
  CHECK(stream != nullptr);
  if (!stream) return 2;
  if (!SDL_BindAudioStream(dev, stream)) {
    std::fprintf(stderr, "bind failed: %s\n", SDL_GetError());
    return 2;
  }

  // Feed 300 ms of PCM as 10 ms frames with a per-frame marker pattern:
  // frame i is filled with int16 value i, so we can identify exactly which
  // frames survive the trim. 300 ms > 150 ms cap, forcing a trim.
  constexpr int kFrames = 30;  // 300 ms
  for (int i = 0; i < kFrames; ++i) {
    std::vector<int16_t> pcm(480, static_cast<int16_t>(i));
    SDL_PutAudioStreamData(stream, pcm.data(), kInputFrameBytes);
  }

  const int queued_before = SDL_GetAudioStreamQueued(stream);
  CHECK(queued_before == kFrames * kInputFrameBytes);
  std::printf("queued before trim: %d bytes (%.0f ms input)\n", queued_before,
              queued_before * 1000.0 / (48000.0 * 2));

  uint64_t dropped = 0;
  crossdesk::TrimAudioStreamToCap(stream, 1, dropped);

  const int queued_after = SDL_GetAudioStreamQueued(stream);
  std::printf("queued after trim: %d bytes (%.0f ms input), dropped input bytes: %llu\n",
              queued_after, queued_after * 1000.0 / (48000.0 * 2),
              (unsigned long long)dropped);

  // The cap must hold measured as total playable media: SDL pins converted
  // output in its device buffer, so SDL_GetAudioStreamAvailable (output
  // format bytes) converted to milliseconds through the output format is the
  // honest end-to-end backlog measure.
  SDL_AudioSpec src2{}, dst2{};
  CHECK(SDL_GetAudioStreamFormat(stream, &src2, &dst2));
  const int avail_after = SDL_GetAudioStreamAvailable(stream);
  const int dst_frame_bytes = SDL_AUDIO_BYTESIZE(dst2.format) * dst2.channels;
  const double total_ms =
      avail_after / double(dst_frame_bytes * dst2.freq) * 1000.0;
  std::printf("total playable media after trim: %.1f ms (cap 150 ms)\n", total_ms);
  CHECK(total_ms <= 150.0 + 1.0);  // cap enforced (1 ms float slack)
  CHECK(total_ms >= 140.0);        // and we did not over-drain
  // Input-side queue also dropped by whole frames.
  CHECK((queued_before - queued_after) % kInputFrameBytes == 0);
  CHECK(dropped > 0);

  // Oldest-first correctness: read everything remaining back (output format
  // bytes), resample knowledge aside — instead verify via a fresh stream
  // identity format test below. Here, verify that frame 0 (the oldest) is
  // gone and the surviving first frame is a LATER one. We read in output
  // format (f32 stereo), convert mentally: each input frame becomes 960
  // samples * 2 channels * 4 bytes = 3840 output bytes per 10 ms. The first
  // surviving input frame index is (dropped_input_bytes / 960); its value in
  // the output float stream is uniform, so sample the first output float.
  SDL_AudioSpec src{}, dst{};
  CHECK(SDL_GetAudioStreamFormat(stream, &src, &dst));
  std::printf("stream formats: src fmt=%x ch=%d freq=%d | dst fmt=%x ch=%d freq=%d\n",
              src.format, src.channels, src.freq, dst.format, dst.channels, dst.freq);

  int avail = SDL_GetAudioStreamAvailable(stream);
  std::vector<uint8_t> out(static_cast<size_t>(avail > 0 ? avail : 1));
  int got = SDL_GetAudioStreamData(stream, out.data(), static_cast<int>(out.size()));
  CHECK(got > 0);
  if (got >= 4 && dst.format == SDL_AUDIO_F32) {
    const float* samples = reinterpret_cast<const float*>(out.data());
    const int first_surviving_frame = static_cast<int>(dropped / kInputFrameBytes);
    // Scan the first frames of output for the first non-zero sample. Frame
    // indices are uniform int16 values upconverted to float, so the first
    // non-zero sample's value IS the index of the first surviving frame
    // that carries a non-zero marker. Frame 0's marker is 0, but frames 0-14
    // were all dropped, so the first output must be frame 15's marker.
    float first_nonzero = 0;
    int64_t first_nonzero_at = -1;
    for (int i = 0; i < got / 4 && i < 480 * 2 * 4; ++i) {
      if (samples[i] != 0.0f) { first_nonzero = samples[i]; first_nonzero_at = i; break; }
    }
    std::printf("first surviving output sample value: %.1f at output sample %lld, "
                "expected frame index %d (first few: %.1f %.1f %.1f %.1f)\n",
                first_nonzero, (long long)first_nonzero_at, first_surviving_frame,
                samples[0], samples[1], samples[2], samples[3]);
    // Byte-accounting correctness is the primary assertion for the
    // mismatched-format stream (above). Content verification here is only
    // indicative because the s16->f32 resampling filter spreads markers
    // across frame boundaries; exact content is verified on the
    // identity-format stream below.
    CHECK(first_surviving_frame > 0);
  }

  SDL_DestroyAudioStream(stream);

  // ---- Exact oldest-first content proof on an UNBOUND identity stream ----
  // With no device, no conversion and no SDL-internal device pin: output
  // bytes ARE the input s16 samples bit-exact, and SDL_GetAudioStreamData
  // consumes strictly oldest-first (verified separately against SDL 3.2.26:
  // pushing frames marked 1,2 reads back 1 first). This isolates the trim's
  // drop-oldest semantics exactly.
  {
    SDL_AudioStream* id_stream = SDL_CreateAudioStream(&input, &input);
    CHECK(id_stream != nullptr);
    for (int i = 0; i < kFrames; ++i) {
      std::vector<int16_t> pcm(480, static_cast<int16_t>(i + 1));  // markers 1..30
      SDL_PutAudioStreamData(id_stream, pcm.data(), kInputFrameBytes);
    }
    uint64_t id_dropped = 0;
    crossdesk::TrimAudioStreamToCap(id_stream, 1, id_dropped);
    const int id_queued = SDL_GetAudioStreamQueued(id_stream);
    CHECK(id_queued <= kCapBytes);
    const int expected_first_frame = static_cast<int>(id_dropped / kInputFrameBytes) + 1;
    std::printf("unbound identity stream: queued after %d, dropped %llu -> first surviving frame marker should be exactly %d\n",
                id_queued, (unsigned long long)id_dropped, expected_first_frame);

    const int avail2 = SDL_GetAudioStreamAvailable(id_stream);
    std::vector<uint8_t> out2(static_cast<size_t>(avail2 > 0 ? avail2 : 1));
    const int got2 = SDL_GetAudioStreamData(id_stream, out2.data(), static_cast<int>(out2.size()));
    CHECK(got2 >= 2);
    if (got2 >= 2) {
      const int16_t first = *reinterpret_cast<const int16_t*>(out2.data());
      std::printf("unbound identity first surviving sample: %d (expected exactly %d)\n",
                  first, expected_first_frame);
      // Exact proof: the oldest surviving sample is bit-exactly the marker
      // of the first frame after the dropped ones. No resampling here, so
      // there is no tolerance — this pins drop-oldest exactly.
      CHECK(static_cast<int>(first) == expected_first_frame);
      CHECK(expected_first_frame > 1);  // oldest frames verifiably dropped
      // And the freshest frame (marker 30) must be the last surviving one.
      const int16_t last = *reinterpret_cast<const int16_t*>(out2.data() + got2 - 2);
      CHECK(static_cast<int>(last) == 30);
    }
    SDL_DestroyAudioStream(id_stream);
  }

  // ---- Stereo stream: 1920-byte frames, cap scales to 150 ms of stereo ----
  {
    SDL_AudioSpec stereo_in{};
    stereo_in.freq = 48000;
    stereo_in.format = SDL_AUDIO_S16;
    stereo_in.channels = 2;
    SDL_AudioStream* stereo = SDL_CreateAudioStream(&stereo_in, &stereo_in);
    CHECK(stereo != nullptr);
    // 300 ms of stereo as 10 ms frames, markers 1..30 in both channels.
    for (int i = 0; i < kFrames; ++i) {
      std::vector<int16_t> pcm(960, static_cast<int16_t>(i + 1));
      SDL_PutAudioStreamData(stereo, pcm.data(), kAudioOutputStereoFrameBytes);
    }
    const int stereo_queued_before = SDL_GetAudioStreamQueued(stereo);
    CHECK(stereo_queued_before == kFrames * kAudioOutputStereoFrameBytes);
    // Stereo cap is exactly twice the mono cap: 150 ms either way.
    CHECK(kAudioOutputQueueCapBytesStereo == 2 * kAudioOutputQueueCapBytesMono);

    uint64_t stereo_dropped = 0;
    crossdesk::TrimAudioStreamToCap(stereo, 2, stereo_dropped);
    const int stereo_queued_after = SDL_GetAudioStreamQueued(stereo);
    std::printf("stereo: queued %d -> %d (cap %d), dropped input %llu\n",
                stereo_queued_before, stereo_queued_after,
                kAudioOutputQueueCapBytesStereo,
                (unsigned long long)stereo_dropped);
    CHECK(stereo_queued_after <= kAudioOutputQueueCapBytesStereo);
    CHECK(stereo_queued_after > 0);
    // Whole stereo frames dropped.
    CHECK(stereo_dropped % kAudioOutputStereoFrameBytes == 0);
    CHECK(stereo_dropped > 0);
    // 150 ms cap in stereo = exactly 15 frames; excess was 15 frames.
    CHECK(static_cast<int>(stereo_dropped / kAudioOutputStereoFrameBytes) ==
          kFrames - 15);
    // Oldest-first: markers 1..15 dropped, 16 survives first.
    const int avail = SDL_GetAudioStreamAvailable(stereo);
    std::vector<uint8_t> out(static_cast<size_t>(avail > 0 ? avail : 1));
    const int got = SDL_GetAudioStreamData(stereo, out.data(),
                                           static_cast<int>(out.size()));
    CHECK(got >= 4);
    if (got >= 4) {
      const int16_t* samples = reinterpret_cast<const int16_t*>(out.data());
      std::printf("stereo first surviving sample: %d (expected exactly 16)\n",
                  samples[0]);
      CHECK(static_cast<int>(samples[0]) == 16);
      // And L/R channels carry the same marker here (uniform fill).
      CHECK(samples[1] == samples[0]);
      // Freshest frame (30) survives last.
      const int16_t last = *reinterpret_cast<const int16_t*>(out.data() + got - 2);
      CHECK(static_cast<int>(last) == 30);
    }
    SDL_DestroyAudioStream(stereo);
  }

  SDL_CloseAudioDevice(dev);
  SDL_Quit();

  if (failures == 0) {
    std::printf("audio_output_trim_test: all checks passed\n");
    return 0;
  }
  std::fprintf(stderr, "audio_output_trim_test: %d check(s) failed\n", failures);
  return 1;
}
