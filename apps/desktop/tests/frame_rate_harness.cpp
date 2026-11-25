/*
 * Copyright (c) 2026 Farlink contributors
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */

// Headless two-endpoint measurement harness. Both endpoints are MiniRTC direct
// peers in this process, connected over the machine's LAN loopback with a
// HostOnly ICE policy. The sender submits NV12 frames through the real
// VideoToolbox encode path at a requested rate; the receiver decodes and reads
// a per-frame sequence barcode that survives the lossy codec to measure
// delivered frame rate and encode-to-decode latency with exact frame identity.
// Output is a single JSON line.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>

#include <nlohmann/json.hpp>

#include "minirtc.h"

#if defined(__APPLE__)
#include "iosurface_frame_source.h"
#endif

namespace {


struct Config {
  int width = 1920;
  int height = 1080;
  int fps = 60;
  MiniRtcVideoCodec codec = MiniRtcVideoCodec::H264;
  int seconds = 12;
  const char* address = nullptr;
  MiniRtcVideoDegradationPreference preference =
      MiniRtcVideoDegradationPreference::MaintainResolution;
  // Feed frames as zero-copy IOSurface-backed CVPixelBuffers (the production
  // macOS capture path) instead of CPU NV12. macOS-only; ignored elsewhere.
  bool native_source = false;
};

struct Endpoint {
  MiniRtcPeerPtr* peer = nullptr;
  std::mutex signal_mutex;
  std::string pending_signals;
};

struct SharedState {
  Endpoint sender;
  Endpoint receiver;
  std::atomic<bool> connected{false};
  std::atomic<bool> failed{false};
  std::atomic<uint64_t> decoded_frames{0};
  std::mutex latency_mutex;
  std::vector<int64_t> latencies_us;
  // Capture instant (library clock, microseconds) indexed by absolute frame
  // sequence number. Decoded frames carry their sequence in a codec-robust
  // barcode, so latency pairing is exact even when the encoder drops frames.
  std::vector<int64_t> send_time_by_seq;
  std::atomic<int64_t> last_bitrate{0};
  // Peak video outbound bitrate observed over the session; the final sample is
  // often zero because the report can arrive after the stream winds down.
  std::atomic<int64_t> peak_video_bitrate{0};
  double rtt_ms = -1;
  char video_encoder[128] = {};
  char video_decoder[128] = {};
  // Resolution of the most recent decoded frame. Because the harness reads the
  // decoded pixels, this is the resolution the encoder actually produced after
  // any adaptation downscale — not the capture resolution requested.
  std::atomic<int> decoded_width{0};
  std::atomic<int> decoded_height{0};
};

SharedState g_state;
std::string g_session_id;

bool ReadSequence(const uint8_t* nv12, int width, int height, uint32_t* out);

void OnReceiveVideoFrame(const MiniRtcVideoFrame* frame, const char*, size_t,
                         const char*, size_t, void*) {
  // Resolve the decoded pixels, recovering CPU NV12 even when a native
  // descriptor is delivered, then read the embedded sequence barcode. The
  // barcode gives exact frame identity, so latency pairing survives encoder
  // drops and the lossy codec alike.
  g_state.decoded_frames.fetch_add(1);
  g_state.decoded_width.store(frame->width);
  g_state.decoded_height.store(frame->height);
  std::vector<uint8_t> copy;
  const uint8_t* nv12 = nullptr;
  if (frame->data) {
    nv12 = reinterpret_cast<const uint8_t*>(frame->data);
  } else if (frame->native_frame && frame->native_frame->copy_to_nv12) {
    copy.resize(static_cast<size_t>(frame->width) * frame->height * 3 / 2);
    if (frame->native_frame->copy_to_nv12(frame->native_frame->owner,
                                          copy.data(), copy.size()) == 0)
      nv12 = copy.data();
  }
  if (!nv12 || !frame->decoded_timestamp) return;
  uint32_t sequence = 0;
  if (!ReadSequence(nv12, frame->width, frame->height, &sequence)) return;
  std::lock_guard<std::mutex> lock(g_state.latency_mutex);
  if (sequence >= g_state.send_time_by_seq.size()) return;
  const int64_t capture = g_state.send_time_by_seq[sequence];
  if (!capture) return;
  const int64_t latency =
      static_cast<int64_t>(frame->decoded_timestamp) - capture;
  g_state.latencies_us.push_back(latency);
}

void AppendSignal(Endpoint* endpoint, const char* message, size_t size) {
  std::lock_guard<std::mutex> lock(endpoint->signal_mutex);
  endpoint->pending_signals.append(message, size);
  endpoint->pending_signals.push_back('\n');
}

// Translate one outgoing MiniRTC signal object into the wire format the remote
// direct peer validates. The library emits {transmission_id, user_id,
// remote_user_id, type, sdp, ufrag}; the peer protocol accepts only
// {type, session_id, sdp} for offer/answer and {type: "candidate", session_id,
// ufrag, candidate|end_of_candidates}, with user identifiers stripped and
// new_candidate remapped to candidate. Each send_signal call carries exactly
// one complete JSON object, so transformation happens inline.
std::string TranslateSignal(const char* message, size_t size,
                            const std::string& session_id) {
  try {
    const auto j = nlohmann::json::parse(message, message + size);
    const auto type = j.at("type").get<std::string>();
    nlohmann::json out;
    if (type == "offer" || type == "answer") {
      out = {{"type", type}, {"sdp", j.at("sdp")}};
    } else if (type == "new_candidate") {
      out = {{"type", "candidate"}, {"ufrag", j.at("ufrag")}};
      if (j.at("sdp") == "a=end-of-candidates\r\n")
        out["end_of_candidates"] = true;
      else
        out["candidate"] = j.at("sdp");
    } else {
      return {};
    }
    out["session_id"] = session_id;
    return out.dump();
  } catch (...) {
    return {};
  }
}

int SendSignalTo(const char* message, size_t size, void* context) {
  auto* target = static_cast<Endpoint*>(context);
  const std::string wire = TranslateSignal(message, size, g_session_id);
  if (wire.empty()) return -1;
  if (std::getenv("HARNESS_DUMP_SIGNALS"))
    std::fprintf(stderr, "sig> %s\n", wire.c_str());
  AppendSignal(target, wire.data(), wire.size());
  return 0;
}

void OnError(MiniRtcDirectError error, void* context) {
  std::fprintf(stderr, "direct error %d on %s\n", static_cast<int>(error),
               context == &g_state.receiver ? "sender-err" : "receiver-err");
  g_state.failed = true;
}

void NoopSignalStatus(MiniRtcSignalStatus, const char*, size_t, void*) {}

void OnConnectionStatus(MiniRtcConnectionStatus status, const char*, size_t,
                        void*) {
  if (status == MiniRtcConnectionStatus::MiniRtcConnectionConnected)
    g_state.connected = true;
  if (status == MiniRtcConnectionStatus::MiniRtcConnectionFailed)
    g_state.failed = true;
}

void OnNetStatus(const char*, size_t, MiniRtcTraversalMode,
                 const MiniRtcNetTrafficStats* stats, const char*, size_t,
                 void*) {
  if (!stats) return;
  g_state.last_bitrate =
      static_cast<int64_t>(stats->total_outbound_stats.bitrate);
  // The video stream rate appears as outbound bitrate on the sender peer and
  // as inbound bitrate on the receiver peer. Track the largest of both so the
  // measurement does not depend on which endpoint this report came from.
  const int64_t video = std::max<int64_t>(
      static_cast<int64_t>(stats->video_outbound_stats.bitrate),
      static_cast<int64_t>(stats->video_inbound_stats.bitrate));
  int64_t peak = g_state.peak_video_bitrate.load();
  while (video > peak &&
         !g_state.peak_video_bitrate.compare_exchange_weak(peak, video)) {
  }
  // Copy whichever backend names the reporting endpoint carries.
  if (stats->video_encoder[0]) {
    std::strncpy(g_state.video_encoder, stats->video_encoder,
                 sizeof(g_state.video_encoder) - 1);
  }
  if (stats->video_decoder[0]) {
    std::strncpy(g_state.video_decoder, stats->video_decoder,
                 sizeof(g_state.video_decoder) - 1);
  }
  if (stats->rtt_ms >= 0) g_state.rtt_ms = stats->rtt_ms;
}



void PumpSignals() {
  for (;;) {
    bool any = false;
    // Each endpoint's pending_signals is its inbox: messages the peer must
    // consume. Deliver every queued wire object to the endpoint itself.
    for (auto* endpoint : {&g_state.sender, &g_state.receiver}) {
      std::string batch;
      {
        std::lock_guard<std::mutex> lock(endpoint->signal_mutex);
        if (!endpoint->pending_signals.empty()) {
          batch.swap(endpoint->pending_signals);
          any = true;
        }
      }
      if (batch.empty()) continue;
      // Messages are newline-framed JSON objects.
      size_t start = 0;
      for (;;) {
        const size_t nl = batch.find('\n', start);
        if (nl == std::string::npos) break;
        if (nl > start)
          MiniRtcReceiveDirectSignal(endpoint->peer, batch.data() + start,
                                     nl - start);
        start = nl + 1;
      }
    }
    if (!any) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

MiniRtcParams MakeParams(const char* user_id, void* user_data, bool hardware, int frame_rate,
                         MiniRtcVideoDegradationPreference preference) {
  MiniRtcParams params{};
  params.use_cfg_file = false;
  params.hardware_acceleration = hardware;
  params.native_video_output = false;
  params.av1_encoding = false;
  params.turn_mode = MiniRtcTurnMode::MiniRtcTurnDisabled;
  params.enable_srtp = true;
  params.video_content_type = MiniRtcVideoContentType::ScreenContent;
  params.video_quality = MiniRtcVideoQuality::MiniRtcVideoQualityHigh;
  params.video_frame_rate = frame_rate;
  params.video_degradation_preference = preference;
  params.desktop_encoding_mode = MiniRtcDesktopEncodingMode::LowLoad;
  params.direct_start_bitrate_bps = 20000000;
  params.relay_start_bitrate_bps = 1500000;
  params.disable_receiver_padding_probing = true;
  params.on_signal_status = NoopSignalStatus;
  params.on_connection_status = OnConnectionStatus;
  params.on_net_status_report = OnNetStatus;
  params.on_receive_video_frame = OnReceiveVideoFrame;
  // NOTE: on_receive_encoded_video is deliberately left unset. The library
  // treats that callback as an H.264-only consumer and withholds H265 from
  // codec negotiation when it is registered (minirtc_connection.cpp), which
  // would silently disable hardware HEVC for every h265 measurement here.
  params.user_id = user_id;
  params.user_data = user_data;
  return params;
}

// Pre-rendered set of distinct noise frames. Consecutive frames differ
// completely, so the screen-content encoder is genuinely exercised at the
// requested rate instead of collapsing onto a static desktop; pre-rendering
// once keeps the per-frame cost low. On top of the noise each frame carries a
// machine-readable sequence barcode that survives the lossy codec, giving every
// decoded frame a exact identity immune to drops and reordering.
constexpr int kFrameSetCount = 8;
constexpr int kSequenceBits = 32;

std::vector<std::vector<uint8_t>> BuildFrameSet(int width, int height) {
  const size_t y_size = static_cast<size_t>(width) * height;
  const size_t uv_size = y_size / 2;
  std::vector<std::vector<uint8_t>> frames(kFrameSetCount);
  for (int f = 0; f < kFrameSetCount; ++f) {
    auto& nv12 = frames[f];
    nv12.assign(y_size + uv_size, 0);
    // Compressible screen-like content: a smooth gradient field with a moving
    // set of sharp-edged blocks. Large flat and smoothly varying regions keep
    // the bitrate inside the encoder budget so measurement reflects frame-rate
    // throughput rather than rate-control starvation, while the per-frame
    // translation forces real motion-compensated encode work every frame.
    const int shift = f * 13;
    for (int row = 0; row < height; ++row) {
      uint8_t* line = nv12.data() + static_cast<size_t>(row) * width;
      for (int col = 0; col < width; ++col) {
        // Smooth diagonal gradient base, very compressible.
        // Smooth diagonal gradient base, very compressible.
        int value = 64 + (((col + row) >> 5) & 0x3f);
        // Sharp-edged blocks on a coarse grid, translated by the frame index so
        // consecutive frames differ. Blocks mimic UI/text structure.
        const int bx = (col + shift) >> 6;
        const int by = (row + shift) >> 6;
        if ((((bx * 31 + by * 17) >> 2) & 7) == 0 &&
            ((col + shift) & 63) < 40 && ((row + shift) & 63) < 22)
          value = (bx + by) & 1 ? 200 : 30;
        line[col] = static_cast<uint8_t>(value);
      }
    }
    const uint8_t uval = static_cast<uint8_t>(0x80 + f * 2);
    std::memset(nv12.data() + y_size, uval, uv_size);
  }
  return frames;
}

// Side of one barcode cell in pixels, scaled to the frame so the cells stay
// large enough to survive quantisation even at the smallest tested resolution.
int BarcodeCell(int width) { return std::max(8, width / 48); }

// Stamp the frame sequence number as a row of high-contrast cells. Each cell is
// filled uniformly dark (0) or bright (1) so the lossy codec preserves its
// centre value; only the centre is sampled on decode, making the readout
// robust to edge ringing. Cells occupy the top rows of the luma plane.
void StampSequence(uint8_t* nv12, int width, int height, uint32_t sequence) {
  const int cell = BarcodeCell(width);
  if ((kSequenceBits + 2) * cell > width || 2 * cell > height) return;
  for (int bit = 0; bit < kSequenceBits; ++bit) {
    const bool one = (sequence >> bit) & 1u;
    const uint8_t value = one ? 235 : 16;
    const int x0 = (bit + 1) * cell;
    for (int row = 0; row < cell; ++row) {
      std::memset(nv12 + static_cast<size_t>(row) * width + x0, value, cell);
    }
  }
}

// Recover the embedded sequence number by sampling each cell centre. Returns
// false when the frame is too small to carry a barcode.
bool ReadSequence(const uint8_t* nv12, int width, int height, uint32_t* out) {
  const int cell = BarcodeCell(width);
  if ((kSequenceBits + 2) * cell > width || 2 * cell > height) return false;
  uint32_t sequence = 0;
  for (int bit = 0; bit < kSequenceBits; ++bit) {
    const int cx = (bit + 1) * cell + cell / 2;
    const int cy = cell / 2;
    const uint8_t value = nv12[static_cast<size_t>(cy) * width + cx];
    if (value > 128) sequence |= (1u << bit);
  }
  *out = sequence;
  return true;
}

int64_t NowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

int Percentile(std::vector<int64_t> sorted, double p) {
  if (sorted.empty()) return 0;
  std::sort(sorted.begin(), sorted.end());
  const double rank = p / 100.0 * (sorted.size() - 1);
  const size_t index = static_cast<size_t>(rank + 0.5);
  return static_cast<int>(sorted[std::min(index, sorted.size() - 1)] / 1000);
}

int RunHarness(const Config& config) {
  const std::string session_id = "harnesssession01";
  g_session_id = session_id;
  const std::string sender_local = "sender0000000001";
  const std::string receiver_local = "receiver00000001";

  // Sender: hardware acceleration to use the real VideoToolbox encoder.
  MiniRtcParams sender_params = MakeParams(sender_local.c_str(), nullptr, true, config.fps, config.preference);
  // Receiver: decode side does not need hardware encoding.
  MiniRtcParams receiver_params = MakeParams(receiver_local.c_str(), nullptr, true, config.fps, config.preference);

  MiniRtcPeerPtr* sender =
      MiniRtcCreatePeerWithVideoCodec(&sender_params, "", config.codec);
  MiniRtcPeerPtr* receiver =
      MiniRtcCreatePeerWithVideoCodec(&receiver_params, "", config.codec);
  if (!sender || !receiver) {
    std::fprintf(stderr, "peer creation failed\n");
    return 2;
  }
  g_state.sender.peer = sender;
  g_state.receiver.peer = receiver;

  MiniRtcAddVideoStream(sender, "Display1");
  MiniRtcAddVideoStream(receiver, "Display1");

  // Wire the signal channels: each peer's outgoing signal is queued for the
  // other. local/remote ids are mirrored across the pair.
  MiniRtcDirectOptions sender_options{};
  sender_options.struct_size = sizeof(sender_options);
  sender_options.version = 1;
  sender_options.local_id = sender_local.c_str();
  sender_options.remote_id = receiver_local.c_str();
  sender_options.session_id = session_id.c_str();
  sender_options.local_address = config.address;
  sender_options.remote_address = config.address;
  sender_options.offerer = true;
  sender_options.send_signal = SendSignalTo;
  sender_options.on_error = OnError;
  sender_options.context = &g_state.receiver;

  MiniRtcDirectOptions receiver_options = sender_options;
  receiver_options.local_id = receiver_local.c_str();
  receiver_options.remote_id = sender_local.c_str();
  receiver_options.offerer = false;
  receiver_options.context = &g_state.sender;

  if (MiniRtcInitDirectPeer(sender, &sender_options) != 0 ||
      MiniRtcInitDirectPeer(receiver, &receiver_options) != 0) {
    std::fprintf(stderr, "direct init failed\n");
    return 2;
  }

  std::thread pump(PumpSignals);
  pump.detach();

  if (MiniRtcStartDirectSession(sender, true) != 0 ||
      MiniRtcStartDirectSession(receiver, false) != 0) {
    std::fprintf(stderr, "direct start failed\n");
    return 2;
  }

  // Wait for the transport to become usable.
  const int64_t deadline = NowUs() + 15000000;
  while (!g_state.connected && !g_state.failed && NowUs() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  if (!g_state.connected) {
    std::fprintf(stderr, "connection not established\n");
    return 2;
  }

  // Pre-render the cycling frame set, then warm up briefly so DTLS/SRTP/codec
  // spin-up and initial keyframe exchange settle before measurement.
  auto frameset = BuildFrameSet(config.width, config.height);
  const size_t frame_bytes = frameset[0].size();

#if defined(__APPLE__)
  // In native mode each pool slot is seeded with the noise of the matching
  // frame-set slot so encode cost is identical to CPU mode; only the barcode
  // differs per submission. The pool reproduces production's zero-copy path.
  crossdesk::IoSurfaceFrameSource* native_source = nullptr;
  if (config.native_source) {
    // Frame-set slots share one noise field (they differ only by the baked
    // shift), so a single template seeds every pool slot.
    native_source = crossdesk::IoSurfaceFrameSource::Create(
        config.width, config.height, kFrameSetCount, frameset[0].data());
    if (!native_source) {
      std::fprintf(stderr, "failed to create IOSurface frame source\n");
      return 2;
    }
  }
#endif
  const int64_t warmup_end = NowUs() + 1500000;
  uint32_t sequence = 0;
  // Each frame is submitted from one of eight persistent noise slots, stamped in
  // place with the per-frame sequence barcode. The send call copies the pixels
  // before returning and a slot is reused only eight submissions later, so no
  // per-frame full-frame copy is needed even at the highest resolution and rate.
  // Send instants are recorded by absolute sequence so the decoder can pair them
  // exactly via the barcode.
  auto submit = [&](uint32_t seq) {
    const int64_t cap = MiniRtcGetSystemTimeMicros(sender);
    MiniRtcVideoFrame frame{};
#if defined(__APPLE__)
    if (native_source) {
      // Zero-copy path: stamp the barcode into the slot's pixel buffer and hand
      // the CVPixelBuffer over by reference, exactly as production capture does.
      native_source->FillFrame(seq % kFrameSetCount, seq, cap, &frame);
    } else
#endif
    {
      auto& base = frameset[seq % kFrameSetCount];
      StampSequence(base.data(), config.width, config.height, seq);
      frame.data = reinterpret_cast<const char*>(base.data());
      frame.size = frame_bytes;
      frame.width = config.width;
      frame.height = config.height;
      frame.captured_timestamp = cap;
    }
    MiniRtcSendVideoFrame(sender, &frame, "Display1");
    std::lock_guard<std::mutex> lock(g_state.latency_mutex);
    if (seq >= g_state.send_time_by_seq.size())
      g_state.send_time_by_seq.resize(seq + 1, 0);
    g_state.send_time_by_seq[seq] = cap;
  };
  while (NowUs() < warmup_end) {
    submit(sequence);
    ++sequence;
    std::this_thread::sleep_for(std::chrono::microseconds(1000000 / config.fps));
  }

  // Brief settle so the warmup tail clears the encode/decode pipeline; warmup
  // latency samples are discarded below regardless, so this only needs to be
  // long enough to avoid contaminating the first measured frames.
  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  // Measured interval. Reset latency accumulation under the lock so a decode
  // completing concurrently cannot interleave with the clear.
  {
    std::lock_guard<std::mutex> lock(g_state.latency_mutex);
    g_state.latencies_us.clear();
  }
  const uint64_t decoded_before = g_state.decoded_frames.load();
  const int64_t measure_start = NowUs();
  const int64_t measure_end = measure_start + config.seconds * 1000000LL;
  const uint32_t first_sequence = sequence;
  // Pace on a steadily advancing local-clock slot so per-frame work does not
  // accumulate cadence drift; the capture timestamp itself is stamped in the
  // library clock domain used only for end-to-end latency.
  const int64_t interval_us = 1000000 / config.fps;
  int64_t next_slot = NowUs();
  while (NowUs() < measure_end) {
    submit(sequence);
    ++sequence;
    next_slot += interval_us;
    const int64_t remaining = next_slot - NowUs();
    if (remaining > 0) std::this_thread::sleep_for(std::chrono::microseconds(remaining));
  }
  const int64_t measure_duration = NowUs() - measure_start;
  // Allow trailing frames to decode.
  std::this_thread::sleep_for(std::chrono::milliseconds(800));

  const uint64_t decoded_after = g_state.decoded_frames.load();
  const uint64_t delivered = decoded_after - decoded_before;
  const uint32_t sent = sequence - first_sequence;

  std::vector<int64_t> latencies;
  {
    std::lock_guard<std::mutex> lock(g_state.latency_mutex);
    latencies = g_state.latencies_us;
  }

  // Dropped = sent but never decoded (approximate; excludes warmup).
  const int64_t dropped = static_cast<int64_t>(sent) - static_cast<int64_t>(delivered);
  const double seconds = measure_duration / 1000000.0;
  const double delivered_fps = seconds > 0 ? delivered / seconds : 0;

  const char* codec_name = config.codec == MiniRtcVideoCodec::H265 ? "h265" : "h264";
  std::printf(
      "{\"width\":%d,\"height\":%d,\"fps\":%d,\"codec\":\"%s\","
      "\"sent\":%u,\"delivered\":%llu,\"dropped\":%lld,"
      "\"delivered_fps\":%.2f,\"lat_p50_ms\":%d,\"lat_p95_ms\":%d,"
      "\"lat_p99_ms\":%d,\"peak_video_bitrate\":%lld,\"rtt_ms\":%.2f,"
      "\"encode_width\":%d,\"encode_height\":%d,"
      "\"encoder\":\"%s\",\"decoder\":\"%s\"}\n",
      config.width, config.height, config.fps, codec_name, sent,
      static_cast<unsigned long long>(delivered), static_cast<long long>(dropped),
      delivered_fps, Percentile(latencies, 50), Percentile(latencies, 95),
      Percentile(latencies, 99),
      static_cast<long long>(g_state.peak_video_bitrate.load()),
      g_state.rtt_ms,
      g_state.decoded_width.load(), g_state.decoded_height.load(),
      g_state.video_encoder, g_state.video_decoder);
  std::fflush(stdout);

  MiniRtcCloseDirectSession(sender);
  MiniRtcCloseDirectSession(receiver);
  MiniRtcDestroyPeer(&sender);
  MiniRtcDestroyPeer(&receiver);
#if defined(__APPLE__)
  // Destroy the pool only after the peers are gone so no encoder still holds a
  // retained buffer the pool is about to release.
  if (native_source) native_source->Destroy();
#endif
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  Config config;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto next = [&](const char* name) -> const char* {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "missing value for %s\n", name);
        std::exit(2);
      }
      return argv[++i];
    };
    if (arg == "--width") config.width = std::atoi(next("--width"));
    else if (arg == "--height") config.height = std::atoi(next("--height"));
    else if (arg == "--fps") config.fps = std::atoi(next("--fps"));
    else if (arg == "--codec") {
      const std::string c = next("--codec");
      config.codec = c == "h265" ? MiniRtcVideoCodec::H265 : MiniRtcVideoCodec::H264;
    } else if (arg == "--seconds") config.seconds = std::atoi(next("--seconds"));
    else if (arg == "--address") config.address = next("--address");
    else if (arg == "--preference") {
      const std::string p = next("--preference");
      if (p == "maintain_frame_rate" || p == "fps")
        config.preference = MiniRtcVideoDegradationPreference::MaintainFrameRate;
      else if (p == "balanced")
        config.preference = MiniRtcVideoDegradationPreference::Balanced;
      else
        config.preference = MiniRtcVideoDegradationPreference::MaintainResolution;
    }
    else if (arg == "--native") config.native_source = true;
  }
  if (!config.address) {
    std::fprintf(stderr, "--address is required\n");
    return 2;
  }
  if (config.width % 2 || config.height % 2 || config.width < 2 || config.height < 2) {
    std::fprintf(stderr, "width/height must be even and >= 2\n");
    return 2;
  }
  return RunHarness(config);
}
