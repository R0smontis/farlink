/*
 * @Author: DI JUNKUN
 * @Date: 2026-09-09
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _SESSION_DEVICE_MANAGER_H_
#define _SESSION_DEVICE_MANAGER_H_

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <remote_action.h>

#include "common/screen_lock_monitor.h"
#include "device_controller.h"
#include "display_info.h"
#include "features/input/captured_keyboard_queue.h"
#include "features/input/captured_mouse_queue.h"
#include "features/input/remote_mouse_buttons.h"
#include "screen_sharing_power.h"
#include "speaker_capture_controller.h"

struct SDL_AudioStream;

namespace crossdesk {

#ifdef __APPLE__
class PlatformMouseCapturer;
#endif

class GuiRuntime;
class DeviceControllerFactory;
class ScreenCapturerFactory;
class ScreenCapturer;

// Owns media capture/playback and remote input devices for the active GUI
// session. GuiRuntime supplies application intent; this class handles device
// creation, updates and teardown.
class SessionDeviceManager {
public:
  explicit SessionDeviceManager(GuiRuntime &owner);
  ~SessionDeviceManager();

  void Initialize();
  void UpdateInteractions();
  // Call after changing the controller map. UpdateInteractions consumes this
  // notification on the UI thread before reading the map under its mutex.
  void OnControllerConnectionsChanged() {
    cursor_capture_dirty_.store(true, std::memory_order_relaxed);
  }
  void DestroyDevices();
  void DestroyFactories();

  int InitializeScreenCapturer();
  int EnsureCaptureReady();
  int StartScreenCapturer();
  int StopScreenCapturer();
  void SetTrueColorEnabled(bool enabled);
  bool ReleaseRemoteMouseButtons();
  bool ReleaseRemoteMouseButtons(const std::string& remote_id,
                                 unsigned mask = 7);
  template <typename Send>
  InputOutcome ApplyRemoteMouse(const std::string& remote_id, MouseFlag flag,
                                 Send send) {
    return remote_mouse_buttons_.Apply(remote_id, flag, std::move(send));
  }
#ifdef _WIN32
  bool OwnsNativeCursor() const;
  void SetNativeCursorPolicy(bool default_enabled, bool current_enabled);
  std::string SendServiceMouseCommand(int x, int y, int wheel, int flag);
#endif
  // Requests are asynchronous; Stop drains/revokes audio delivery before
  // returning so the connection peer can be destroyed safely.
  void StartSpeakerCapturer();
  void StopSpeakerCapturer();
  int StartMouseController();
  // Returns 1 while background releases are pending. force is only used
  // during final destruction after the input dispatcher has been stopped.
  int StopMouseController(bool force = false);
  int StartKeyboardCapturer();
  int StopKeyboardCapturer();

#ifdef __APPLE__
  // 0 = shortcuts go to the remote session (the tap swallows them locally),
  // 1 = local shortcuts preferred (listen-only tap; copies still reach the
  // peer). Applied at the next hook; when the capturer is already running
  // natively the mode switches immediately by re-hooking, with a forced
  // key release first so the peer never sees stuck modifiers. Inert while
  // the Slint window-event fallback is active.
  void SetKeyboardShortcutMode(int mode);
  int GetKeyboardShortcutMode() const { return keyboard_shortcut_mode_; }

  // Game mode captures raw mouse deltas behind a hidden, frozen cursor.
  // scale_x/scale_y convert device points into the wire unit (fraction of
  // the controlled display per axis) and can be refreshed while active.
  // freeze_x/freeze_y are global Core Graphics display
  // coordinates: the capturer warps the cursor there before freezing it.
  int StartMouseCapturer(double scale_x, double scale_y, double freeze_x,
                         double freeze_y);
  int StopMouseCapturer();
  bool MouseCapturerActive() const;
  void SetMouseCapturerScale(double scale_x, double scale_y);
#endif

  int DestroyAudioOutput();
  // Serialized by GuiRuntime::audio_delivery_mutex_. Each peer has its own
  // stream so SDL mixes simultaneous playback and muting clears only that peer.
  // Open the output on the first frame and close it after the last stream ends.
  void ClearAudio(const std::string& remote_id);
  void PushAudio(const std::string& remote_id, const char *data, size_t size);

  bool SupportsTextInput() const;
  bool SendTextCommand(const std::string& text);
  bool SendKeyboardCommand(int key_code, bool is_down, uint32_t scan_code,
                           bool extended);
  bool SendMouseCommand(const RemoteAction &action, int selected_display);
  int SwitchDisplay(int display_id);
  InputOutcome ResetToInitialDisplay();

  // Snapshot of the capture displays. The UI thread refreshes the list every
  // tick while transport threads build host information and map mouse input
  // from it, so callers get a copy taken under the lock.
  std::vector<DisplayInfo> display_info_list() const;
  // The same snapshot in the wire's host information form.
  std::vector<HostDisplay> host_display_list() const;

  // Host lock-screen level as of the latest monitor poll. Host status and
  // automation report this; capture forwarding uses the same atomic.
  bool IsHostScreenLocked() const {
    return host_screen_locked_.load(std::memory_order_relaxed);
  }

private:
  int InitializeAudioOutput();
  unsigned ReleaseMouseButtonMask(unsigned mask);
  void DrainCapturedKeyboardInput();
#ifdef __APPLE__
  // One consumer on the input queue: the exit transition travels on this
  // atomic flag instead, because a second drainer would steal and discard
  // input — and a lost button-up is a stuck button on the host. Set on the
  // capturer's tap thread, consumed with exchange(false) on the UI tick.
  std::atomic<bool> game_mode_exit_requested_{false};
  void ServiceGameModeExit();
#endif
  bool ShouldSendCapturedFrame(std::chrono::steady_clock::time_point now,
                               int fps);
  bool ShouldCaptureCursor() const;
  bool SyncDisplayInfo();
  void SetDisplayInfoList(std::vector<DisplayInfo> displays);
  void ResetCaptureMetrics();
  uint64_t RecordCaptureCadence(std::chrono::steady_clock::time_point now,
                                int fps, bool from_secure_desktop);

  GuiRuntime &owner_;
  uint32_t output_device_ = 0;
  std::unordered_map<std::string, SDL_AudioStream*> output_streams_;
  // Bookkeeping for the playback queue cap: how many bytes were trimmed from
  // the head of each stream the last time its backlog exceeded the cap.
  std::unordered_map<std::string, uint64_t> output_stream_dropped_bytes_;
  std::chrono::steady_clock::time_point next_audio_output_retry_{};
  ScreenCapturerFactory *screen_capturer_factory_ = nullptr;
  ScreenCapturer *screen_capturer_ = nullptr;
  ScreenSharingPower screen_sharing_power_;
  std::atomic<bool> cursor_capture_dirty_{true};
  SpeakerCaptureController speaker_capture_;
  DeviceControllerFactory *device_controller_factory_ = nullptr;
  MouseController *mouse_controller_ = nullptr;
  RemoteMouseButtons remote_mouse_buttons_;
  // Input is executed on a worker while the UI starts/stops this controller.
  std::mutex mouse_controller_mutex_;
  KeyboardCapturer *keyboard_capturer_ = nullptr;
#ifdef __APPLE__
  std::unique_ptr<PlatformMouseCapturer> mouse_capturer_;
  CapturedMouseQueue captured_mouse_inputs_;
  // Sender thread woken by the queue's condition variable: game-mode input
  // reaches the transport at wake latency instead of the 16 ms UI tick.
  std::thread game_mode_sender_thread_;
  std::atomic<bool> game_mode_sender_stop_{false};
  void GameModeSenderMain();
  int keyboard_shortcut_mode_ = 0;
#endif
  // Guards display_info_list_ (UI thread writes, transport threads read).
  mutable std::mutex display_info_mutex_;
  std::vector<DisplayInfo> display_info_list_;
  size_t registered_display_stream_count_ = 0;
  CapturedKeyboardQueue captured_keyboard_inputs_;
#if defined(_WIN32) || defined(__OHOS__)
  std::chrono::steady_clock::time_point next_keyboard_capture_retry_{};
#endif
#ifdef _WIN32
  std::mutex service_mouse_mutex_;
  unsigned service_mouse_buttons_ = 0;
  int service_mouse_x_ = 0, service_mouse_y_ = 0;
#endif
  std::chrono::steady_clock::time_point last_frame_time_{};
  std::chrono::steady_clock::time_point next_frame_deadline_{};
  std::chrono::steady_clock::time_point next_capture_start_retry_{};
  std::mutex capture_metrics_mutex_;
  uint64_t capture_metrics_generation_ = 0;
  std::chrono::steady_clock::time_point capture_metrics_started_{};
  std::chrono::steady_clock::time_point last_capture_callback_{};
  uint64_t capture_callbacks_ = 0, capture_rate_drops_ = 0;
  uint64_t capture_secure_callbacks_ = 0;
  uint64_t capture_forwarded_ = 0, capture_send_errors_ = 0;
  int64_t capture_max_gap_us_ = 0;
  int64_t capture_send_us_ = 0, capture_max_send_us_ = 0;
  std::string last_video_frame_stream_id_;
  bool invalid_video_stream_id_logged_ = false;
  // Host screen-lock awareness: polled on the UI tick inside
  // UpdateInteractions; the capture callback reads the atomic level to skip
  // forwarding while locked and consumes the one-shot keyframe flag on the
  // first frame after unlock.
  ScreenLockMonitor screen_lock_monitor_;
  std::atomic<bool> host_screen_locked_{false};
  std::atomic<bool> force_keyframe_on_next_frame_{false};
};

} // namespace crossdesk

#endif
