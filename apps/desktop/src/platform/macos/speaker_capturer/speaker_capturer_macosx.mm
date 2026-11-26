#import <AVFoundation/AVFoundation.h>
#import <Foundation/Foundation.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <utility>

#include "rd_log.h"
#include "speaker_capturer_macosx.h"

namespace {
// Per-operation dead-request bounds, from the round-9 measured distribution
// of healthy starts in an uncontended system (n=16): enumeration median
// 160 ms, max 189 ms; startCapture median ~220 ms, max ~495 ms. A dead
// request — the same replayd disruption that kills video streams — never
// completes at all, so the timeout is purely a dead-request detector and its
// cost is retry delay. Enumeration is bounded at 750 ms (4x the worst healthy
// measurement); startCapture keeps 2 s because healthy completions have been
// observed within 1.5x of 750 ms, and a false expiry costs a retry attempt —
// after 3 failed attempts audio is disabled for the session
// (speaker_capture_controller.cpp max_attempts).
constexpr auto kEnumerationTimeout = std::chrono::milliseconds(750);
constexpr auto kCaptureTimeout = std::chrono::milliseconds(2000);

std::string NSErrorToString(NSError* error) {
  if (!error) return "";
  NSString* message = [NSString
      stringWithFormat:@"%@ (%@:%ld), reason=%@", error.localizedDescription, error.domain,
                       (long)error.code, error.localizedFailureReason ?: @""];
  return message.UTF8String ?: "";
}

// Late system replies own only their result, never the capturer or stack data.
// Complete() also tells a late successful start to stop its abandoned stream.
template <typename T>
class CaptureReply {
 public:
  bool Complete(T value) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (expired_) return false;
    value_ = std::move(value);
    ready_ = true;
    wake_.notify_one();
    return true;
  }
  bool Wait(T& value, std::chrono::milliseconds timeout = kCaptureTimeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!wake_.wait_for(lock, timeout, [&] { return ready_; })) {
      expired_ = true;
      return false;
    }
    value = value_;
    return true;
  }

 private:
  std::mutex mutex_;
  std::condition_variable wake_;
  bool ready_ = false;
  bool expired_ = false;
  T value_{};
};

struct ContentReply {
  SCShareableContent* content = nil;
  NSError* error = nil;
};
struct StreamReply {
  NSError* error = nil;
};
struct AudioCallbackState {
  std::mutex mutex;
  crossdesk::SpeakerCapturer::speaker_data_cb callback;
  std::atomic<bool> running{false};
  // ScreenCaptureKit callbacks do not align to the encoder's fixed 10 ms
  // stereo grid (1920 bytes), so PCM carries over between callbacks here
  // instead of dropping the unaligned tail of every callback.
  std::vector<uint8_t> frame_carry;
};

// Normalize interleaved s16 of any channel count to stereo interleaved:
// stereo passes through unchanged, mono duplicates into both channels, and
// wider layouts fold the extra channels into the first two at reduced gain
// so nothing clips. Output frame count equals input frames per channel.
inline std::vector<short> NormalizeToStereo(const short* input, int samples,
                                            int channels) {
  std::vector<short> out;
  if (!input || samples <= 0 || channels <= 0) return out;
  const int frames = samples / channels;
  if (channels == 2) {
    out.assign(input, input + samples);
    return out;
  }
  out.resize(frames * 2);
  if (channels == 1) {
    for (int i = 0; i < frames; ++i) {
      out[2 * i] = input[i];
      out[2 * i + 1] = input[i];
    }
    return out;
  }
  // >2 channels: fold extra channels into L/R at 1/4 gain each so the sum
  // cannot clip beyond the original peak.
  for (int i = 0; i < frames; ++i) {
    int32_t left = input[i * channels];
    int32_t right = input[i * channels + 1];
    for (int c = 2; c < channels; ++c) {
      const int32_t extra = input[i * channels + c] / 4;
      if (c % 2 == 0) {
        left += extra;
      } else {
        right += extra;
      }
    }
    if (left > 32767) left = 32767;
    if (left < -32768) left = -32768;
    if (right > 32767) right = 32767;
    if (right < -32768) right = -32768;
    out[2 * i] = (short)left;
    out[2 * i + 1] = (short)right;
  }
  return out;
}
}  // namespace

@interface SpeakerCaptureDelegate : NSObject <SCStreamDelegate, SCStreamOutput> {
  std::shared_ptr<AudioCallbackState> state_;
}
- (instancetype)initWithState:(std::shared_ptr<AudioCallbackState>)state;
@end

@implementation SpeakerCaptureDelegate
- (instancetype)initWithState:(std::shared_ptr<AudioCallbackState>)state {
  self = [super init];
  if (self) state_ = std::move(state);
  return self;
}

- (void)stream:(SCStream*)stream didStopWithError:(NSError*)error {
  state_->running = false;
  SPDLOG_LOGGER_WARN(crossdesk::get_logger(), "System audio stream stopped: {}",
                     NSErrorToString(error));
}

- (void)stream:(SCStream*)stream
    didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
                   ofType:(SCStreamOutputType)type {
  if (type != SCStreamOutputTypeAudio) return;
  const auto state = state_;
  std::lock_guard<std::mutex> lock(state->mutex);
  if (!state->callback || !state->running) return;

  CMBlockBufferRef blockBuffer = CMSampleBufferGetDataBuffer(sampleBuffer);
  if (!blockBuffer) {
    return;
  }

  size_t length = CMBlockBufferGetDataLength(blockBuffer);
  char* dataPtr = NULL;
  OSStatus dataStatus = CMBlockBufferGetDataPointer(blockBuffer, 0, NULL, NULL, &dataPtr);
  if (dataStatus != noErr || dataPtr == nullptr || length == 0) {
    return;
  }

  CMAudioFormatDescriptionRef formatDesc = CMSampleBufferGetFormatDescription(sampleBuffer);
  if (!formatDesc) {
    return;
  }

  const AudioStreamBasicDescription* asbd =
      CMAudioFormatDescriptionGetStreamBasicDescription(formatDesc);
  if (!asbd || asbd->mChannelsPerFrame == 0) {
    return;
  }

  if (state->callback) {
    // Convert whatever the system delivers into stereo-interleaved s16.
    // ScreenCaptureKit is asked for 2 channels, so channelCount is 2 in the
    // common case; a mono or >2ch source is normalized to stereo here
    // (mono duplicated, extra channels folded into the first two) so the
    // downstream contract never depends on the source device.
    std::vector<short> out_pcm16;
    const int channels = asbd->mChannelsPerFrame;
    if (asbd->mFormatFlags & kAudioFormatFlagIsFloat) {
      const int samples = (int)(length / sizeof(float));
      const float* floatData = (const float*)dataPtr;
      std::vector<short> pcm16(samples);
      for (int i = 0; i < samples; ++i) {
        float v = floatData[i];
        if (v > 1.0f) v = 1.0f;
        if (v < -1.0f) v = -1.0f;
        pcm16[i] = (short)(v * 32767.0f);
      }
      out_pcm16 = NormalizeToStereo(pcm16.data(), samples, channels);
    } else if (asbd->mBitsPerChannel == 16) {
      const int samples = (int)(length / 2);
      out_pcm16 = NormalizeToStereo((const short*)dataPtr, samples, channels);
    }

    // 10 ms stereo frames: 480 samples x 2 channels x 2 bytes = 1920 bytes.
    // PCM carries across callbacks so every captured sample is emitted
    // exactly once even when the delivery sizes do not align to the grid.
    const size_t frame_bytes = 1920;
    auto& carry = state->frame_carry;
    const uint8_t* pcm_bytes = reinterpret_cast<const uint8_t*>(out_pcm16.data());
    carry.insert(carry.end(), pcm_bytes,
                 pcm_bytes + out_pcm16.size() * sizeof(short));
    size_t consumed = 0;
    while (carry.size() - consumed >= frame_bytes) {
      if (!state->callback) {
        return;
      }
      state->callback(carry.data() + consumed, frame_bytes, "audio");
      consumed += frame_bytes;
    }
    carry.erase(carry.begin(), carry.begin() + consumed);
  }
}
@end

namespace crossdesk {
class SpeakerCapturerMacosx::Impl {
 public:
  speaker_data_cb callback;
  SCStream* stream = nil;
  SpeakerCaptureDelegate* delegate = nil;
  dispatch_queue_t queue = nil;
  std::shared_ptr<AudioCallbackState> state;
};

SpeakerCapturerMacosx::SpeakerCapturerMacosx() : impl_(new Impl) {}
SpeakerCapturerMacosx::~SpeakerCapturerMacosx() {
  Destroy();
  delete impl_;
}

int SpeakerCapturerMacosx::Init(speaker_data_cb cb) {
  impl_->callback = std::move(cb);
  if (!impl_->queue)
    impl_->queue = dispatch_queue_create("SpeakerAudio.Queue", DISPATCH_QUEUE_SERIAL);
  return 0;
}

int SpeakerCapturerMacosx::Start() {
  @autoreleasepool {
    if (!impl_->callback) return -1;
    if (IsRunning()) return 0;
    if (Stop() != 0) return -1;

    // A cached SCDisplay becomes invalid after hotplug, sleep or a display
    // reconfiguration. Obtain a fresh snapshot on every start/reconnection.
    const auto enumeration_started = std::chrono::steady_clock::now();
    auto content_reply = std::make_shared<CaptureReply<ContentReply>>();
    [SCShareableContent
        getShareableContentWithCompletionHandler:^(SCShareableContent* content, NSError* error) {
          content_reply->Complete({content, error});
        }];
    ContentReply content;
    if (!content_reply->Wait(content, kEnumerationTimeout)) {
      LOG_ERROR("Timed out refreshing system audio capture displays");
      return -1;
    }
    if (content.error || !content.content) {
      LOG_ERROR("Failed to refresh system audio capture displays: {}",
                NSErrorToString(content.error));
      return -1;
    }
    SCDisplay* display = nil;
    const auto main_id = CGMainDisplayID();
    for (SCDisplay* candidate in content.content.displays) {
      if (candidate.displayID == main_id) {
        display = candidate;
        break;
      }
    }
    if (!display) display = content.content.displays.firstObject;
    if (!display) {
      LOG_ERROR("No display available for system audio capture");
      return -1;
    }
    const auto enumeration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - enumeration_started).count();
    LOG_INFO("Starting system audio capture with current display {} (enumeration took {} ms)",
             display.displayID, enumeration_ms);

    SCStreamConfiguration* config = [[SCStreamConfiguration alloc] init];
    config.capturesAudio = YES;
    config.sampleRate = 48000;
    // Stereo capture: desktop audio is overwhelmingly stereo content, and
    // the encoder negotiates per-peer mono downmix internally, so one
    // capture feeds mixed mono/stereo sessions without reconfiguration.
    config.channelCount = 2;
    SCContentFilter* filter = [[SCContentFilter alloc] initWithDisplay:display
                                                      excludingWindows:@[]];
    impl_->state = std::make_shared<AudioCallbackState>();
    impl_->state->callback = impl_->callback;
    impl_->delegate = [[SpeakerCaptureDelegate alloc] initWithState:impl_->state];
    impl_->stream = [[SCStream alloc] initWithFilter:filter
                                       configuration:config
                                            delegate:impl_->delegate];
    NSError* output_error = nil;
    if (!impl_->stream || ![impl_->stream addStreamOutput:impl_->delegate
                                                     type:SCStreamOutputTypeAudio
                                       sampleHandlerQueue:impl_->queue
                                                    error:&output_error]) {
      LOG_ERROR("Failed to attach system audio output: {}", NSErrorToString(output_error));
      Stop();
      return -1;
    }

    auto start_reply = std::make_shared<CaptureReply<StreamReply>>();
    SCStream* stream = impl_->stream;
    impl_->state->running = true;
    [stream startCaptureWithCompletionHandler:^(NSError* error) {
      if (!start_reply->Complete({error}) && !error) {
        [stream stopCaptureWithCompletionHandler:nil];
      }
    }];
    StreamReply started;
    if (!start_reply->Wait(started)) {
      LOG_ERROR("Timed out starting system audio capture");
      Stop();
      return -1;
    }
    if (started.error || !impl_->state->running) {
      LOG_ERROR("Failed to start system audio capture: {}", NSErrorToString(started.error));
      Stop();
      return -1;
    }
    const auto start_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - enumeration_started).count();
    LOG_INFO("System audio stream start completed in {} ms", start_ms);
    return 0;
  }
}

int SpeakerCapturerMacosx::Stop() {
  @autoreleasepool {
    if (auto state = std::exchange(impl_->state, {})) {
      std::lock_guard<std::mutex> lock(state->mutex);
      state->running = false;
      state->callback = nullptr;
    }
    SCStream* stream = impl_->stream;
    impl_->stream = nil;
    impl_->delegate = nil;
    if (!stream) return 0;
    auto stop_reply = std::make_shared<CaptureReply<StreamReply>>();
    [stream stopCaptureWithCompletionHandler:^(NSError* error) {
      stop_reply->Complete({error});
    }];
    StreamReply stopped;
    if (!stop_reply->Wait(stopped)) {
      LOG_ERROR("Timed out stopping system audio capture; callbacks revoked");
      return -1;
    }
    // A start failure or daemon restart can leave an already stopped stream.
    // Its references have been discarded, so future starts can recover.
    if (stopped.error) LOG_WARN("System audio stop reply: {}", NSErrorToString(stopped.error));
    return 0;
  }
}

bool SpeakerCapturerMacosx::IsRunning() const { return impl_->state && impl_->state->running; }

int SpeakerCapturerMacosx::Destroy() {
  const int result = Stop();
  impl_->callback = nullptr;
  impl_->queue = nil;
  return result;
}
int SpeakerCapturerMacosx::Pause() { return Stop(); }
int SpeakerCapturerMacosx::Resume() { return Start(); }
}  // namespace crossdesk
