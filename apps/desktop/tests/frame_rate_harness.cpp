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

#include "udp_shaper.h"

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
  // When > 0, raise the video bitrate ceiling so congestion control can ramp
  // toward the LAN's real capacity instead of the default budget.
  int max_bitrate_bps = 0;
  // Direct-session start bitrate. Zero keeps the harness default; set it to
  // the production value to measure the real start-of-session ramp.
  int start_bitrate_bps = 0;
  // Bottleneck shaping. Zero keeps the direct baseline path; a positive
  // rate routes both endpoints' media through the in-process UDP shaper
  // by rewriting the port of every signaled ICE candidate to the shaper's
  // relay port. See udp_shaper.h for the mechanism.
  int bottleneck_kbps = 0;
  int bottleneck_delay_ms = 0;
  int bottleneck_queue_bytes = 65536;
  int bottleneck_burst_bytes = 1500;
  int bottleneck_port = 39201;
};

struct Endpoint {
  MiniRtcPeerPtr* peer = nullptr;
  std::mutex signal_mutex;
  std::string pending_signals;
};

// One decoded frame's timing and resolution, recorded for offline bucketing.
struct FrameRecord {
  int64_t capture_us = 0;
  int64_t decoded_us = 0;
  int width = 0;
  int height = 0;
};

struct SharedState {
  Endpoint sender;
  Endpoint receiver;
  std::atomic<bool> connected{false};
  std::atomic<bool> failed{false};
  std::atomic<uint64_t> decoded_frames{0};
  std::mutex latency_mutex;
  std::vector<int64_t> latencies_us;
  // Per-frame decode records for fine-grained (100 ms) bucketing: capture
  // instant, decode-completion instant (both library clock, microseconds),
  // and decoded resolution. Exact frame identity comes from the barcode;
  // this log preserves the per-frame timing the aggregate latency vector
  // discards, so the start-of-session ramp can be bucketed offline.
  std::vector<FrameRecord> frame_records;
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
  // Most recently reported per-second video bitrate (max of in/outbound).
  std::atomic<int64_t> last_video_bitrate{0};
  // Resolution of the most recent decoded frame. Because the harness reads the
  // decoded pixels, this is the resolution the encoder actually produced after
  // any adaptation downscale — not the capture resolution requested.
  std::atomic<int> decoded_width{0};
  std::atomic<int> decoded_height{0};
  // Per-cause video admission drop counters from the sender peer's network
  // stats reports. Reports arrive from both endpoints; the receiver's are
  // zero, so the maximum across reports tracks the sender.
  std::atomic<uint64_t> capture_input_frames{0};
  std::atomic<uint64_t> rate_dropped_frames{0};
  std::atomic<uint64_t> cwnd_dropped_frames{0};
  std::atomic<uint64_t> pacer_rejected_frames{0};
  std::atomic<uint64_t> encode_queue_dropped_frames{0};
  std::atomic<uint64_t> encode_queue_dropped_pending_tasks{0};
  std::atomic<uint64_t> encode_queue_dropped_in_flight{0};
};

SharedState g_state;
std::string g_session_id;
// Active in shaped runs only. Owned by RunHarness; the signal rewrite reads
// just the relay port, which never changes after startup.
crossdesk::UdpShaper* g_shaper = nullptr;
uint16_t g_shaper_port = 0;

// Rewrite the port field of one ICE candidate string (with or without the
// leading "a="). The candidate grammar the direct protocol accepts is fixed:
// "candidate:<foundation> 1 UDP <priority> <ip> <port> typ host" — the port
// is the sixth whitespace-separated token. Validation inside minirtc checks
// only the IP against the pinned remote address and re-emits the candidate
// with the port intact (minirtc direct_ice_policy.h NormalizeDirectCandidate:
// the equality check at line 62 compares addresses, not ports, and the
// re-emission at line 66-67 carries the original port through), so a
// rewritten port survives validation and reaches libnice unmodified.
std::string RewriteCandidatePort(const std::string& candidate, uint16_t port) {
  // Tokenize on spaces, keeping offsets so the string can be reassembled
  // with a single substitution.
  size_t positions[6] = {};
  size_t token = 0;
  size_t i = 0;
  // Skip a leading "a=" prefix; it is part of the first token.
  while (i < candidate.size() && token < 6) {
    while (i < candidate.size() && candidate[i] == ' ') ++i;
    if (i >= candidate.size()) break;
    positions[token++] = i;
    while (i < candidate.size() && candidate[i] != ' ') ++i;
  }
  if (token < 6) return candidate;
  // The port token starts at positions[5] and runs to the next space.
  size_t port_start = positions[5];
  size_t port_end = candidate.find(' ', port_start);
  if (port_end == std::string::npos) port_end = candidate.size();
  const std::string replacement = std::to_string(port);
  return candidate.substr(0, port_start) + replacement +
         candidate.substr(port_end);
}

// Rewrite every "a=candidate:" line's port inside an SDP blob. Trickle ICE
// means these rarely carry candidates in this harness, but the rewrite is
// cheap insurance for partial-gathering offers.
std::string RewriteSdpPorts(const std::string& sdp, uint16_t port) {
  std::string out;
  out.reserve(sdp.size());
  size_t pos = 0;
  while (pos < sdp.size()) {
    size_t line_end = sdp.find('\n', pos);
    if (line_end == std::string::npos) line_end = sdp.size();
    std::string line = sdp.substr(pos, line_end - pos);
    if (line.rfind("a=candidate:", 0) == 0) {
      // Strip the trailing \r for rewriting, then restore it.
      const bool cr = !line.empty() && line.back() == '\r';
      std::string body = cr ? line.substr(0, line.size() - 1) : line;
      line = RewriteCandidatePort(body, port) + (cr ? "\r" : "");
    }
    out += line;
    if (line_end < sdp.size()) out += '\n';
    pos = line_end + 1;
  }
  return out;
}

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
  FrameRecord record;
  record.capture_us = capture;
  record.decoded_us = static_cast<int64_t>(frame->decoded_timestamp);
  record.width = frame->width;
  record.height = frame->height;
  g_state.frame_records.push_back(record);
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
      if (g_shaper_port)
        out["sdp"] = RewriteSdpPorts(out["sdp"].get<std::string>(),
                                     g_shaper_port);
    } else if (type == "new_candidate") {
      out = {{"type", "candidate"}, {"ufrag", j.at("ufrag")}};
      if (j.at("sdp") == "a=end-of-candidates\r\n")
        out["end_of_candidates"] = true;
      else if (g_shaper_port)
        out["candidate"] = RewriteCandidatePort(j.at("sdp").get<std::string>(),
                                                g_shaper_port);
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
  g_state.last_video_bitrate = video;
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
  // Per-cause admission counters are cumulative on the sender and zero on the
  // receiver; keep the maximum seen so the final report reflects the sender's
  // last report even if a receiver report arrives afterwards.
  auto track_max = [](std::atomic<uint64_t>& slot, uint64_t value) {
    uint64_t current = slot.load();
    while (value > current &&
           !slot.compare_exchange_weak(current, value)) {
    }
  };
  track_max(g_state.capture_input_frames, stats->video_capture_input_frames);
  track_max(g_state.rate_dropped_frames, stats->video_rate_dropped_frames);
  track_max(g_state.cwnd_dropped_frames, stats->video_cwnd_dropped_frames);
  track_max(g_state.pacer_rejected_frames, stats->video_pacer_rejected_frames);
  track_max(g_state.encode_queue_dropped_frames,
            stats->video_encode_queue_dropped_frames);
  track_max(g_state.encode_queue_dropped_pending_tasks,
            stats->video_encode_queue_dropped_pending_tasks);
  track_max(g_state.encode_queue_dropped_in_flight,
            stats->video_encode_queue_dropped_in_flight);
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
                         MiniRtcVideoDegradationPreference preference,
                         int start_bitrate_bps) {
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
  // The harness historically forced a 20 Mbps direct start, which sits above
  // the steady-state need of every tested configuration and therefore hides
  // the production start-of-session ramp entirely. Zero selects the 20 Mbps
  // default for backward compatibility; pass the production value (or any
  // value) to measure the real ramp from the actual session-start bitrate.
  params.direct_start_bitrate_bps =
      start_bitrate_bps > 0 ? start_bitrate_bps : 20000000;
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

  // Shaped mode: bring up the relay before the peers so the port rewrite is
  // active from the very first signal. The shaper binds on the same LAN IP
  // the endpoints use, so relayed traffic arrives from an address byte-
  // identical to the rewritten candidates — ICE sees a consistent path.
  crossdesk::UdpShaper shaper;
  if (config.bottleneck_kbps > 0) {
    crossdesk::UdpShaperConfig shaper_config;
    shaper_config.rate_bps =
        static_cast<uint64_t>(config.bottleneck_kbps) * 1000;
    shaper_config.burst_bytes =
        static_cast<uint32_t>(config.bottleneck_burst_bytes);
    shaper_config.queue_bytes =
        static_cast<uint32_t>(config.bottleneck_queue_bytes);
    shaper_config.delay_ms = static_cast<uint32_t>(config.bottleneck_delay_ms);
    if (!shaper.Start(config.address,
                      static_cast<uint16_t>(config.bottleneck_port),
                      shaper_config)) {
      std::fprintf(stderr, "udp shaper failed to start\n");
      return 2;
    }
    g_shaper = &shaper;
    g_shaper_port = shaper.BoundPort();
  }
  const std::string sender_local = "sender0000000001";
  const std::string receiver_local = "receiver00000001";

  // Sender: hardware acceleration to use the real VideoToolbox encoder.
  MiniRtcParams sender_params = MakeParams(sender_local.c_str(), nullptr, true, config.fps, config.preference, config.start_bitrate_bps);
  // Receiver: decode side does not need hardware encoding.
  MiniRtcParams receiver_params = MakeParams(receiver_local.c_str(), nullptr, true, config.fps, config.preference, config.start_bitrate_bps);

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

  if (config.max_bitrate_bps > 0) {
    // Raise the video budget ceiling so GCC can ramp toward the real path
    // capacity (up to 100 Mbps) instead of sitting at the default start.
    if (MiniRtcUpdateVideoBitrateLimit(sender, receiver_local.c_str(),
                                       receiver_local.size(),
                                       config.max_bitrate_bps) != 0) {
      std::fprintf(stderr, "warning: bitrate limit update rejected\n");
    }
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
    g_state.frame_records.clear();
  }
  const uint64_t decoded_before = g_state.decoded_frames.load();
  const int64_t measure_start = NowUs();
  const int64_t measure_end = measure_start + config.seconds * 1000000LL;
  const uint32_t first_sequence = sequence;
  // Snapshot the admission counters as of the latest stats report so the
  // final JSON can attribute drops to the measured window approximately.
  const uint64_t counters_before[7] = {
      g_state.capture_input_frames.load(), g_state.rate_dropped_frames.load(),
      g_state.cwnd_dropped_frames.load(), g_state.pacer_rejected_frames.load(),
      g_state.encode_queue_dropped_frames.load(),
      g_state.encode_queue_dropped_pending_tasks.load(),
      g_state.encode_queue_dropped_in_flight.load()};
  // Pace on a steadily advancing local-clock slot so per-frame work does not
  // accumulate cadence drift; the capture timestamp itself is stamped in the
  // library clock domain used only for end-to-end latency.
  const int64_t interval_us = 1000000 / config.fps;
  int64_t next_slot = NowUs();
  // Per-second trajectory sampling: emitted on stderr with a distinct prefix
  // so the final JSON line on stdout stays machine-consumable.
  uint64_t last_sample_decoded = decoded_before;
  int64_t next_sample_us = measure_start + 1000000LL;
  int sample_index = 0;
  // Per-second delivered-fps samples feed both the stderr trajectory and the
  // steady-state distribution metrics (p5/worst) in the final JSON: a stream
  // that averages 108 fps but dips to 44 periodically is a different product
  // than one holding 108 every second, and the mean alone cannot see it.
  std::vector<uint64_t> per_second_fps;
  // Shaper throughput sampling, one sample per trajectory tick. Direction 0
  // carries the sender's media in practice, but the sum of both directions
  // is the bottleneck's total achieved throughput either way.
  uint64_t last_shaper_bytes = 0;
  if (g_shaper) {
    const auto snapshot = g_shaper->Snapshot();
    last_shaper_bytes =
        snapshot.forwarded_bytes[0] + snapshot.forwarded_bytes[1];
  }
  std::vector<double> per_second_shape_mbps;
  while (NowUs() < measure_end) {
    submit(sequence);
    ++sequence;
    next_slot += interval_us;
    const int64_t now = NowUs();
    if (now >= next_sample_us) {
      const uint64_t decoded_now = g_state.decoded_frames.load();
      const int64_t video_bitrate = g_state.last_video_bitrate.load();
      const uint64_t second_fps = decoded_now - last_sample_decoded;
      per_second_fps.push_back(second_fps);
      std::fprintf(stderr,
                   "traj t=%ds enc=%dx%d fps=%llu video_bitrate=%lld",
                   ++sample_index, g_state.decoded_width.load(),
                   g_state.decoded_height.load(),
                   static_cast<unsigned long long>(second_fps),
                   static_cast<long long>(video_bitrate));
      if (g_shaper) {
        const auto snapshot = g_shaper->Snapshot();
        const uint64_t total_bytes =
            snapshot.forwarded_bytes[0] + snapshot.forwarded_bytes[1];
        const double mbps =
            (total_bytes - last_shaper_bytes) * 8.0 / 1000000.0;
        per_second_shape_mbps.push_back(mbps);
        std::fprintf(stderr,
                     " shape_mbps=%.2f shape_drops=%llu shape_qmax=%llu",
                     mbps,
                     static_cast<unsigned long long>(
                         snapshot.dropped_packets[0] +
                         snapshot.dropped_packets[1]),
                     static_cast<unsigned long long>(std::max(
                         snapshot.max_queue_bytes[0],
                         snapshot.max_queue_bytes[1])));
        last_shaper_bytes = total_bytes;
      }
      std::fprintf(stderr, "\n");
      last_sample_decoded = decoded_now;
      next_sample_us += 1000000LL;
    }
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
  std::vector<FrameRecord> records;
  {
    std::lock_guard<std::mutex> lock(g_state.latency_mutex);
    latencies = g_state.latencies_us;
    records = g_state.frame_records;
  }

  // Bucket the per-frame records into 100 ms bins keyed off capture time so
  // the start-of-session ramp can be read as a time series. The first frame's
  // capture instant is t=0. Each bin reports the number of frames decoded in
  // it (x10 for fps), the encoded resolution of its last frame, and the p50
  // encode-to-decode latency of the frames in it. Emitted one "ramp100" line
  // per non-empty bin on stderr alongside the per-second "traj" lines, so the
  // final JSON on stdout stays machine-consumable.
  if (!records.empty()) {
    std::sort(records.begin(), records.end(),
              [](const FrameRecord& a, const FrameRecord& b) {
                return a.capture_us < b.capture_us;
              });
    const int64_t t0 = records.front().capture_us;
    const int64_t kBucketUs = 100000;
    size_t bin_index = 0;
    size_t i = 0;
    while (i < records.size()) {
      const int64_t bin_end = t0 + static_cast<int64_t>(bin_index + 1) * kBucketUs;
      std::vector<int64_t> bin_lat;
      int bw = 0, bh = 0;
      size_t count = 0;
      while (i < records.size() && records[i].capture_us < bin_end) {
        bin_lat.push_back(records[i].decoded_us - records[i].capture_us);
        bw = records[i].width;
        bh = records[i].height;
        ++count;
        ++i;
      }
      if (count > 0) {
        std::sort(bin_lat.begin(), bin_lat.end());
        const int64_t p50 = bin_lat[bin_lat.size() / 2] / 1000;
        std::fprintf(stderr,
                     "ramp100 t=%lldms fps=%llu enc=%dx%d lat_p50_ms=%lld\n",
                     static_cast<long long>(bin_index * 100),
                     static_cast<unsigned long long>(count * 10), bw, bh,
                     static_cast<long long>(p50));
      }
      ++bin_index;
    }
  }

  // Dropped = sent but never decoded (approximate; excludes warmup).
  const int64_t dropped = static_cast<int64_t>(sent) - static_cast<int64_t>(delivered);
  const double seconds = measure_duration / 1000000.0;
  const double delivered_fps = seconds > 0 ? delivered / seconds : 0;

  // Distribution of the per-second delivered fps: p5 captures periodic dips
  // that the mean hides, worst_second captures the deepest single second.
  double fps_p5 = 0;
  double fps_worst_second = 0;
  if (!per_second_fps.empty()) {
    std::vector<uint64_t> sorted_fps = per_second_fps;
    std::sort(sorted_fps.begin(), sorted_fps.end());
    const double rank = 0.05 * (sorted_fps.size() - 1);
    const size_t index = static_cast<size_t>(rank + 0.5);
    fps_p5 = static_cast<double>(
        sorted_fps[std::min(index, sorted_fps.size() - 1)]);
    fps_worst_second = static_cast<double>(sorted_fps.front());
  }

  const char* codec_name = config.codec == MiniRtcVideoCodec::H265 ? "h265" : "h264";
  const uint64_t counters_after[7] = {
      g_state.capture_input_frames.load(), g_state.rate_dropped_frames.load(),
      g_state.cwnd_dropped_frames.load(), g_state.pacer_rejected_frames.load(),
      g_state.encode_queue_dropped_frames.load(),
      g_state.encode_queue_dropped_pending_tasks.load(),
      g_state.encode_queue_dropped_in_flight.load()};
  // Shaper totals across both directions. Achieved throughput is measured
  // over the measurement window only; the warmup/session-tail forwarding is
  // excluded by differencing counters sampled at window start and end.
  uint64_t shaper_bytes_end = 0, shaper_dropped_end = 0, shaper_qmax = 0;
  if (g_shaper) {
    const auto snapshot = g_shaper->Snapshot();
    shaper_bytes_end = snapshot.forwarded_bytes[0] + snapshot.forwarded_bytes[1];
    shaper_dropped_end = snapshot.dropped_packets[0] + snapshot.dropped_packets[1];
    shaper_qmax =
        std::max(snapshot.max_queue_bytes[0], snapshot.max_queue_bytes[1]);
  }
  double shape_mean_mbps = 0;
  if (!per_second_shape_mbps.empty()) {
    for (double v : per_second_shape_mbps) shape_mean_mbps += v;
    shape_mean_mbps /= per_second_shape_mbps.size();
  }
  std::printf(
      "{\"width\":%d,\"height\":%d,\"fps\":%d,\"codec\":\"%s\","
      "\"sent\":%u,\"delivered\":%llu,\"dropped\":%lld,"
      "\"delivered_fps\":%.2f,\"fps_p5\":%.1f,\"fps_worst_second\":%.1f,\"lat_p50_ms\":%d,\"lat_p95_ms\":%d,"
      "\"lat_p99_ms\":%d,\"peak_video_bitrate\":%lld,\"rtt_ms\":%.2f,"
      "\"encode_width\":%d,\"encode_height\":%d,"
      "\"capture_input_frames\":%llu,\"rate_dropped_frames\":%llu,"
      "\"cwnd_dropped_frames\":%llu,\"pacer_rejected_frames\":%llu,"
      "\"encode_queue_dropped_frames\":%llu,"
      "\"win_capture_input\":%llu,\"win_rate_dropped\":%llu,"
      "\"win_cwnd_dropped\":%llu,\"win_pacer_rejected\":%llu,"
      "\"win_encode_queue_dropped\":%llu,"
      "\"win_encode_queue_dropped_pending\":%llu,"
      "\"win_encode_queue_dropped_in_flight\":%llu,"
      "\"bottleneck_kbps\":%d,\"shape_mean_mbps\":%.2f,"
      "\"shape_forwarded_bytes\":%llu,\"shape_dropped_packets\":%llu,"
      "\"shape_max_queue_bytes\":%llu,"
      "\"encoder\":\"%s\",\"decoder\":\"%s\"}\n",
      config.width, config.height, config.fps, codec_name, sent,
      static_cast<unsigned long long>(delivered), static_cast<long long>(dropped),
      delivered_fps, fps_p5, fps_worst_second, Percentile(latencies, 50), Percentile(latencies, 95),
      Percentile(latencies, 99),
      static_cast<long long>(g_state.peak_video_bitrate.load()),
      g_state.rtt_ms,
      g_state.decoded_width.load(), g_state.decoded_height.load(),
      static_cast<unsigned long long>(counters_after[0]),
      static_cast<unsigned long long>(counters_after[1]),
      static_cast<unsigned long long>(counters_after[2]),
      static_cast<unsigned long long>(counters_after[3]),
      static_cast<unsigned long long>(counters_after[4]),
      static_cast<unsigned long long>(counters_after[0] - counters_before[0]),
      static_cast<unsigned long long>(counters_after[1] - counters_before[1]),
      static_cast<unsigned long long>(counters_after[2] - counters_before[2]),
      static_cast<unsigned long long>(counters_after[3] - counters_before[3]),
      static_cast<unsigned long long>(counters_after[4] - counters_before[4]),
      static_cast<unsigned long long>(counters_after[5] - counters_before[5]),
      static_cast<unsigned long long>(counters_after[6] - counters_before[6]),
      config.bottleneck_kbps, shape_mean_mbps,
      static_cast<unsigned long long>(shaper_bytes_end),
      static_cast<unsigned long long>(shaper_dropped_end),
      static_cast<unsigned long long>(shaper_qmax),
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
  // Tear the globals down before the stack-local shaper so no late signal
  // callback can observe a dangling relay port.
  g_shaper = nullptr;
  g_shaper_port = 0;
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
    else if (arg == "--max-bitrate") config.max_bitrate_bps = std::atoi(next("--max-bitrate"));
    else if (arg == "--start-bitrate") config.start_bitrate_bps = std::atoi(next("--start-bitrate"));
    else if (arg == "--bottleneck-mbps")
      config.bottleneck_kbps =
          static_cast<int>(std::atof(next("--bottleneck-mbps")) * 1000);
    else if (arg == "--bottleneck-delay-ms")
      config.bottleneck_delay_ms = std::atoi(next("--bottleneck-delay-ms"));
    else if (arg == "--bottleneck-queue-bytes")
      config.bottleneck_queue_bytes = std::atoi(next("--bottleneck-queue-bytes"));
    else if (arg == "--bottleneck-burst-bytes")
      config.bottleneck_burst_bytes = std::atoi(next("--bottleneck-burst-bytes"));
    else if (arg == "--bottleneck-port")
      config.bottleneck_port = std::atoi(next("--bottleneck-port"));
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
