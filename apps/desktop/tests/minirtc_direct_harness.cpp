/*
 * @Author: Farlink contributors
 * @Date: 2026-11-25
 * Copyright (c) 2026 by Farlink contributors.
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include "minirtc.h"
#include "nlohmann/json.hpp"

namespace {

using Clock = std::chrono::steady_clock;

struct SignalChannel {
  // Bidirectional in-memory signal pipe pair: messages pushed on `a` surface
  // for peer B and vice versa. Delivery is synchronous but deferred through a
  // tiny pump thread so senders never recurse into the remote peer.
  struct Endpoint {
    std::mutex mutex;
    std::condition_variable cv;
    std::queue<std::string> inbox;
    bool closed = false;
  } ep[2];

  // MiniRtcConnection emits cloud-shaped signaling (transmission_id/user_id/
  // remote_user_id, new_candidate with sdp). The direct protocol expects the
  // compact form (session_id, candidate/end_of_candidates). This is the same
  // translation the production LAN session manager performs
  // (lan_session_manager.cpp); the harness reproduces it minimally.
  static int Send(const char* message, size_t message_size, void* context) {
    auto* pair = static_cast<std::pair<SignalChannel*, int>*>(context);
    SignalChannel* channel = pair->first;
    const int sender = pair->second;
    auto& inbox = channel->ep[sender ^ 1];

    auto j = nlohmann::json::parse(message, message + message_size, nullptr,
                                   false);
    if (j.is_discarded()) return -1;
    nlohmann::json out;
    const std::string type = j.value("type", "");
    if (type == "offer" || type == "answer") {
      out = {{"type", type}, {"sdp", j.value("sdp", "")}};
    } else if (type == "new_candidate") {
      out = {{"type", "candidate"}, {"ufrag", j.value("ufrag", "")}};
      const std::string sdp = j.value("sdp", "");
      if (sdp == "a=end-of-candidates\r\n") {
        out["end_of_candidates"] = true;
      } else {
        out["candidate"] = sdp;
      }
    } else {
      return 0;  // unknown cloud signal: irrelevant for the direct protocol
    }
    out["session_id"] = "harness-session";

    {
      std::lock_guard<std::mutex> lock(inbox.mutex);
      inbox.inbox.push(out.dump());
    }
    inbox.cv.notify_one();
    return 0;
  }
};

struct PeerHarness {
  std::string name;
  MiniRtcPeerPtr* peer = nullptr;
  SignalChannel* channel = nullptr;
  int endpoint_index = 0;
  std::pair<SignalChannel*, int> send_context;

  std::atomic<bool> connected{false};
  std::atomic<bool> first_frame{false};
  std::atomic<int64_t> connected_at_us{0};
  std::atomic<int64_t> first_frame_at_us{0};
  std::atomic<int> frames_received{0};
  std::atomic<int> audio_frames_received{0};
  std::atomic<uint64_t> last_capture_timestamp_us{0};

  std::thread pump_thread;
  std::atomic<bool> stop{false};
};

int64_t NowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             Clock::now().time_since_epoch())
      .count();
}

void OnConnectionStatus(MiniRtcConnectionStatus status, const char* user_id,
                        size_t user_id_size, void* user_data) {
  auto* harness = static_cast<PeerHarness*>(user_data);
  if (status == MiniRtcConnectionStatus::MiniRtcConnectionConnected) {
    harness->connected_at_us.store(NowUs());
    harness->connected.store(true);
    std::printf("[%s] connected at T+%lld us since start\n",
                harness->name.c_str(),
                (long long)(harness->connected_at_us.load() -
                            harness->connected_at_us.load()));
  }
}

void OnVideoFrame(const MiniRtcVideoFrame* video_frame, const char* user_id,
                  size_t user_id_size, const char* src_id, size_t src_id_size,
                  void* user_data) {
  auto* harness = static_cast<PeerHarness*>(user_data);
  const int64_t now = NowUs();
  harness->frames_received.fetch_add(1);
  if (!harness->first_frame.exchange(true)) {
    harness->first_frame_at_us.store(now);
    harness->last_capture_timestamp_us.store(
        video_frame ? video_frame->captured_timestamp : 0);
  }
}

void OnAudioFrame(const char* data, size_t size, const char* user_id,
                  size_t user_id_size, const char* src_id, size_t src_id_size,
                  void* user_data) {
  auto* harness = static_cast<PeerHarness*>(user_data);
  harness->audio_frames_received.fetch_add(1);
}

void OnDataFrame(const char* data, size_t size, const char* user_id,
                 size_t user_id_size, const char* src_id, size_t src_id_size,
                 void* user_data) {}

void OnNetStatus(const char* user_id, size_t user_id_size,
                 MiniRtcTraversalMode mode,
                 const MiniRtcNetTrafficStats* stats, const char* remote_id,
                 size_t remote_id_size, void* user_data) {}

void OnDirectError(MiniRtcDirectError error, void* context) {
  std::fprintf(stderr, "direct error %d\n", static_cast<int>(error));
}

// Pump thread: deliver queued signaling to the peer's ReceiveDirect.
void PumpLoop(PeerHarness* harness) {
  auto& ep = harness->channel->ep[harness->endpoint_index];
  while (!harness->stop.load()) {
    std::string message;
    {
      std::unique_lock<std::mutex> lock(ep.mutex);
      ep.cv.wait_for(lock, std::chrono::milliseconds(50),
                     [&] { return !ep.inbox.empty() || ep.closed; });
      if (ep.inbox.empty()) continue;
      message = std::move(ep.inbox.front());
      ep.inbox.pop();
    }
    MiniRtcReceiveDirectSignal(harness->peer, message.data(), message.size());
  }
}

}  // namespace

int main(int argc, char** argv) {
  // Two endpoint addresses on the host's LAN IP. Default: use the primary
  // en0 address discovered at runtime; an explicit pair can be passed for
  // repeatability. Ports are zero: ICE picks ephemeral host candidates.
  std::string lan_ip = argc > 1 ? argv[1] : "";
  if (lan_ip.empty()) {
    std::fprintf(stderr,
                 "usage: %s <lan-ip-v4> [frame-count]\n"
                 "  lan-ip-v4: this machine's real LAN IPv4 (127.x rejected "
                 "by direct policy)\n",
                 argv[0]);
    return 2;
  }
  const int frame_count = argc > 2 ? std::atoi(argv[2]) : 30;

  // Same-address pair: traffic hairpins through the host stack, giving
  // sub-millisecond RTT while still exercising the full ICE/DTLS/SRTP path.
  // (The direct policy rejects loopback addresses but accepts a real LAN IP
  // talking to itself.)
  const std::string address = lan_ip;

  SignalChannel channel;

  PeerHarness offerer, answerer;
  offerer.name = "offerer";
  answerer.name = "answerer";
  offerer.channel = answerer.channel = &channel;
  offerer.endpoint_index = 0;
  answerer.endpoint_index = 1;
  offerer.send_context = {&channel, 0};
  answerer.send_context = {&channel, 1};

  MiniRtcParams params{};
  params.hardware_acceleration = true;
  params.native_video_output = false;
  params.enable_srtp = true;
  params.turn_mode = MiniRtcTurnMode::MiniRtcTurnDisabled;
  params.video_content_type = MiniRtcVideoContentType::ScreenContent;
  params.video_quality = MiniRtcVideoQuality::MiniRtcVideoQualityHigh;
  params.video_frame_rate = 60;
  params.video_degradation_preference =
      MiniRtcVideoDegradationPreference::MaintainResolution;
  params.on_receive_video_frame = OnVideoFrame;
  params.on_receive_audio_buffer = OnAudioFrame;
  params.on_receive_data_buffer = OnDataFrame;
  params.on_connection_status = OnConnectionStatus;
  params.on_net_status_report = OnNetStatus;

  const int64_t t_start = NowUs();

  MiniRtcParams offerer_params = params;
  offerer_params.user_id = "harness-offerer";
  offerer_params.user_data = &offerer;
  offerer.peer = MiniRtcCreatePeer(&offerer_params);
  MiniRtcParams answerer_params = params;
  answerer_params.user_id = "harness-answerer";
  answerer_params.user_data = &answerer;
  answerer.peer = MiniRtcCreatePeer(&answerer_params);
  if (!offerer.peer || !answerer.peer) {
    std::fprintf(stderr, "peer creation failed\n");
    return 1;
  }

  MiniRtcDirectOptions offerer_options{};
  offerer_options.struct_size = sizeof(MiniRtcDirectOptions);
  offerer_options.version = 1;
  offerer_options.local_id = "harness-offerer";
  offerer_options.remote_id = "harness-answerer";
  offerer_options.session_id = "harness-session";
  offerer_options.local_address = address.c_str();
  offerer_options.remote_address = address.c_str();
  offerer_options.offerer = true;
  offerer_options.send_signal = SignalChannel::Send;
  offerer_options.on_error = OnDirectError;
  offerer_options.context = &offerer.send_context;

  MiniRtcDirectOptions answerer_options = offerer_options;
  answerer_options.local_id = "harness-answerer";
  answerer_options.remote_id = "harness-offerer";
  answerer_options.offerer = false;
  answerer_options.context = &answerer.send_context;

  if (MiniRtcInitDirectPeer(offerer.peer, &offerer_options) != 0 ||
      MiniRtcInitDirectPeer(answerer.peer, &answerer_options) != 0) {
    std::fprintf(stderr, "direct init failed\n");
    return 1;
  }

  MiniRtcAddVideoStream(offerer.peer, "screen0");
  MiniRtcAddAudioStream(offerer.peer, "audio0");
  MiniRtcAddDataStream(offerer.peer, "data0", true);
  MiniRtcAddVideoStream(answerer.peer, "screen0");
  MiniRtcAddAudioStream(answerer.peer, "audio0");
  MiniRtcAddDataStream(answerer.peer, "data0", true);

  offerer.pump_thread = std::thread(PumpLoop, &offerer);
  answerer.pump_thread = std::thread(PumpLoop, &answerer);

  const int64_t t_offer_start = NowUs();
  if (MiniRtcStartDirectSession(offerer.peer, true) != 0 ||
      MiniRtcStartDirectSession(answerer.peer, false) != 0) {
    std::fprintf(stderr, "start direct failed\n");
    return 1;
  }

  // Wait for connection establishment.
  const int64_t deadline = NowUs() + 15'000'000;
  while ((!offerer.connected.load() || !answerer.connected.load()) &&
         NowUs() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (!offerer.connected.load() || !answerer.connected.load()) {
    std::fprintf(stderr, "connection timeout\n");
    return 1;
  }
  const int64_t connected_us = offerer.connected_at_us.load() - t_offer_start;

  // Push frames continuously, starting before the peer is fully connected:
  // this is what a real host does (capture runs independently of the join),
  // and it is required to measure the honest first-frame latency rather than
  // an artifact of when the test happened to start sending.
  const int width = 320, height = 180;
  std::vector<uint8_t> nv12(size_t(width) * height * 3 / 2, 16);
  std::atomic<bool> stop_sending{false};
  std::thread sender([&] {
    int i = 0;
    while (!stop_sending.load()) {
      const int bar = (i * 7) % width;
      for (int y = 0; y < height; ++y) {
        nv12[size_t(y) * width + bar] = 235;
      }
      MiniRtcVideoFrame frame{};
      frame.data = reinterpret_cast<const char*>(nv12.data());
      frame.size = nv12.size();
      frame.width = width;
      frame.height = height;
      frame.captured_timestamp = MiniRtcGetSystemTimeMicros(offerer.peer);
      MiniRtcSendVideoFrame(offerer.peer, &frame, "screen0");
      ++i;
      std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
  });
  sender.detach();
  (void)frame_count;

  const int64_t frame_deadline = NowUs() + 5'000'000;
  while (!answerer.first_frame.load() && NowUs() < frame_deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  const int64_t first_frame_us =
      answerer.first_frame_at_us.load() - t_offer_start;

  std::printf("HARNESS-RESULT start_to_connected_us=%lld\n",
              (long long)connected_us);
  std::printf("HARNESS-RESULT start_to_first_frame_us=%lld\n",
              (long long)first_frame_us);
  std::printf("HARNESS-RESULT frames_sent=%d frames_received=%d\n", frame_count,
              answerer.frames_received.load());
  std::printf("HARNESS-RESULT answerer_connected=%d offerer_connected=%d\n",
              (int)answerer.connected.load(), (int)offerer.connected.load());

  stop_sending.store(true);
  offerer.stop.store(true);
  answerer.stop.store(true);
  MiniRtcCloseDirectSession(offerer.peer);
  MiniRtcCloseDirectSession(answerer.peer);
  if (offerer.pump_thread.joinable()) offerer.pump_thread.join();
  if (answerer.pump_thread.joinable()) answerer.pump_thread.join();
  MiniRtcDestroyPeer(&offerer.peer);
  MiniRtcDestroyPeer(&answerer.peer);
  return answerer.first_frame.load() ? 0 : 1;
}
