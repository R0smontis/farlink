/*
 * @Author: Farlink contributors
 * @Date: 2026-11-25
 * Copyright (c) 2026 by Farlink contributors, All Rights Reserved.
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Playback queue cap trim for remote-audio SDL output streams.
 */

#ifndef CROSSDESK_AUDIO_OUTPUT_TRIM_H_
#define CROSSDESK_AUDIO_OUTPUT_TRIM_H_

#include <cstdint>
#include <vector>

#include "SDL3/SDL.h"

#include "rd_log.h"

namespace crossdesk {

// Constants describing the remote-audio PCM shape and the playback latency
// budget. 10 ms s16 mono at 48 kHz is what the Opus decoder emits.
constexpr int kAudioOutputSampleRate = 48000;
constexpr int kAudioOutputInputFrameBytes = 960;   // 10 ms s16 mono
constexpr int kAudioOutputQueueCapBytes = 14400;   // 150 ms s16 mono

// Enforce the playback queue cap on a bound audio stream by discarding the
// oldest queued audio. Correctness depends on SDL 3.2's unit semantics:
//
//   SDL_GetAudioStreamQueued()  - bytes of *input*-format data still in the
//                                 stream (unconverted).
//   SDL_GetAudioStreamData()    - bytes of *output* (device) format produced
//                                 by conversion on demand.
//
// Because SDL_BindAudioStream rewrites the stream's output format to the
// device format (e.g. 48 kHz float32 stereo = 8 bytes/frame vs the 2
// bytes/frame s16 mono input), draining a fixed number of input-sized bytes
// would remove the wrong amount of media. Everything below is therefore
// computed in whole input-format 10 ms frames and converted to output bytes
// through the stream's own format ratio.
//
// Trimming drops the oldest data, in whole input frames, because dropping
// the newest would discard the freshest audio without ever shortening
// playback latency. dropped_input_bytes accumulates input-format bytes
// discarded over the stream's lifetime, for diagnostics.
inline void TrimAudioStreamToCap(SDL_AudioStream* stream,
                                 uint64_t& dropped_input_bytes) {
  if (!stream) return;
  const int queued_input_bytes = SDL_GetAudioStreamQueued(stream);
  if (queued_input_bytes <= kAudioOutputQueueCapBytes) return;

  SDL_AudioSpec src{}, dst{};
  if (!SDL_GetAudioStreamFormat(stream, &src, &dst)) return;
  const int src_frame_bytes = SDL_AUDIO_BYTESIZE(src.format) * src.channels;
  const int dst_frame_bytes = SDL_AUDIO_BYTESIZE(dst.format) * dst.channels;
  if (src_frame_bytes <= 0 || dst_frame_bytes <= 0 || src.freq <= 0 ||
      dst.freq <= 0)
    return;

  const int excess_input_bytes =
      queued_input_bytes - kAudioOutputQueueCapBytes;
  // Whole input frames to drop, rounded up so the queue ends below the cap.
  const int drop_input_frames =
      (excess_input_bytes + kAudioOutputInputFrameBytes - 1) /
      kAudioOutputInputFrameBytes;
  const int drop_input_bytes = drop_input_frames * kAudioOutputInputFrameBytes;
  // Output bytes corresponding to that input: scale by sample count ratio.
  const int64_t drop_output_bytes =
      static_cast<int64_t>(drop_input_bytes) / src_frame_bytes * dst.freq /
      src.freq * dst_frame_bytes;
  if (drop_output_bytes <= 0) return;

  std::vector<uint8_t> scratch(static_cast<size_t>(drop_output_bytes));
  const int got = SDL_GetAudioStreamData(stream, scratch.data(),
                                         static_cast<int>(scratch.size()));
  if (got > 0) {
    dropped_input_bytes += static_cast<uint64_t>(drop_input_bytes);
    LOG_DEBUG(
        "Audio playback backlog trimmed {} input bytes ({} output bytes, "
        "queued {} > cap {})",
        drop_input_bytes, got, queued_input_bytes, kAudioOutputQueueCapBytes);
  }
}

}  // namespace crossdesk

#endif  // CROSSDESK_AUDIO_OUTPUT_TRIM_H_
