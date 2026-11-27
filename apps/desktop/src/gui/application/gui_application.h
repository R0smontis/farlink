/*
 * @Author: DI JUNKUN
 * @Date: 2026-09-04
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _GUI_APPLICATION_H_
#define _GUI_APPLICATION_H_

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>

#include <remote_action.h>

#include "application/signal_connection_indicator.h"
#include "features/input/game_mode_buttons.h"
#include "features/input/game_mode_scale.h"
#include "features/input/scroll_accumulator.h"
#include "features/input/stream_hotkey_matcher.h"
#include "rendering/video_geometry.h"
#include "runtime/cursor_state_provider.h"
#include "runtime/gui_runtime.h"

namespace crossdesk {

class UpdateChecker;
namespace updater { class Controller; }

// Slint application shell. GuiRuntime remains the owner of transport, media,
// device, clipboard, transfer and settings behavior.
class GuiApplication final : private GuiRuntime {
public:
  GuiApplication();
  ~GuiApplication();

  int Run(bool login_launch = false);
#ifdef _WIN32
  int RunUnattended(void* stop_event);
#endif

private:
  struct SlintUi;
  struct StreamView;
  StreamView* FindStreamView(const std::string& id);
  struct CursorDeliveryState {
    CursorState last_sent{};
    bool has_sent = false;
    bool feedback_pending = false;
    std::chrono::steady_clock::time_point last_sent_time{};
  };

  void InitializeLogger();
  void InitializeSettings();
  bool InitializeSDL();
  void InitializeModules();
  void InitializeUi();
  void InitializeSystemTray();
  void ShowMainWindow(const std::string& activation_token = {});
  bool MinimizeMainWindowToTray();
  void BindMainCallbacks(bool gallery_window = false);
  void BindTrustedDeviceCallbacks(bool gallery_window = false);
  void BindStreamCallbacks(StreamView& view);
  void Tick();
#ifdef _WIN32
  void TickLocalHost();
#endif
  void PollUpdateCheck();
  void ExportDiagnosticLogs();
  void PollDiagnosticExport();
  void ShareLocalCursorState();
  bool RequestPasswordChange(const std::string &password);
  void HandlePasswordChangeResult();
  void HandleCredentialRecovery();
  void SyncMainWindow();
  void ShowGalleryWindow();
  void CloseGalleryWindow();
  void SyncWindowGeometry(bool gallery_window);
  void ToggleGalleryFullscreen();
  void RestoreWindowGeometry(bool before_show = false,
                             bool gallery_window = false);
  void SyncGallery();
  void GalleryAction(const std::string& action, const std::string& value,
                     bool gallery_window = true);
  bool AddGalleryDevice(const std::string& id, const std::string& password,
                        const std::string& group, bool remember_password);
  bool ImportGalleryRecent(const std::string& group);
  void DeviceGroupAction(const std::string& action, const std::string& value,
                         bool gallery_window);
  bool SaveDeviceGroups(const std::string& new_group, bool gallery_window);
  void ReleasePressedInput(bool preserve_local_ui = false);
  void ReleaseControl(bool preserve_local_ui = false);
  void SyncAudioPlayback();
  void SaveGalleryWindowState();
  void SyncAnnouncements();
  void SyncLanDiscoveredPeers();
  void SyncConnectionDialog();
  void SyncPlatformDialogs();
  void SyncStreamWindows();
  void InitializeStreamWindow(StreamView& view);
  void SyncStreamWindow(StreamView& view);
  void ShowStreamWindow(const std::shared_ptr<RemoteSession>& session);
  void SyncStreamVideoFrame(StreamView& view, std::optional<double> timer_lateness_ms);
  void SyncVideoFrameGapStats();
  void ScheduleNextVideoFrame();
  void PresentPendingVideoFrame(std::optional<double> timer_lateness_ms = std::nullopt);
  // Posted from a decoder thread when a frame becomes presentable. Coalesces
  // multiple wakeups into one event-loop task so a busy UI cannot queue work
  // faster than it drains it.
  void RequestVideoPresent();
#if defined(__APPLE__)
  void UpdateVideoFrameScheduler();
#endif
  void SyncStreamKeyboardFocus();
  void SetStreamKeyboardFocus(StreamView& view, bool focused);
  bool AcceptStreamInput(StreamView& view);
  void SyncIncomingControllers();
  void SyncTrustedDevices();
  void SyncFileTransferNotifications();
  void SyncClipboardFileNotification();
  void OpenFileManager(const std::string& remote);
  std::string SessionDisplayName(const std::string& remote);
  void CancelFileTransfer(uint32_t file_id);
#if (defined(__linux__) && !defined(__OHOS__)) && !defined(__APPLE__)
  void SyncXWaylandWindowActivation();
#endif
  void UpdateLocalization();
  void ResetSettingsUi(bool gallery_window = false);
  bool ValidateSelfHostedSettingsFromUi(bool gallery_window = false);
  bool SaveSettingsFromUi(bool gallery_window = false);
  void ApplySettingsFromConfig(bool reconnect);
#if _WIN32 && CROSSDESK_PORTABLE
  void CheckPortableWindowsService();
  void StartPortableWindowsServiceInstall();
  void JoinPortableWindowsServiceInstallThread();
#endif
  void ConnectFromUi(const std::string &remote_id);
  void ProcessLanConnectLaunchOption();
  void InstallTerminationSignalHandlers();
  void DisconnectSession(const std::string &remote_id);
  void CloseDisconnectedSessions();
  void ResetPointerMotion();
  std::optional<VideoPoint> MapRemotePointer(RemoteSession& session, float x, float y);
  void SendPointerInput(int button, int kind, float x, float y);
  void SendScrollInput(float delta_x, float delta_y, float x, float y);
  void SendKeyInput(const std::string &text, bool pressed, bool control,
                    bool alt, bool shift, bool meta);
  // Stream control chords (Ctrl+Alt+←/→ display cycling, Ctrl+Alt+F
  // fullscreen) are local actions: consumed ahead of the wire in
  // SendRemoteInput, active only while a stream window holds input focus
  // with control enabled. Swallowing mirrors the game-mode escape chord:
  // the completing key-down, its auto-repeats and its key-up never cross.
  bool ConsumeLocalKeyChord(const RemoteAction& action) override;
  void FireStreamHotkey(StreamHotkey hotkey);
  StreamHotkeyMatcher stream_hotkey_matcher_;
  // Shared bodies of the control-bar callbacks so both the UI controls and
  // the local stream chords drive the identical path: display switching
  // resets capture/cursor state and sends the exact same display_id wire
  // command; fullscreen toggling drives the same window transition.
  void SwitchStreamDisplay(StreamView& view, int index);
  void ToggleStreamFullscreen(StreamView& view);
#ifdef __APPLE__
  // Relative-mouse game mode. Enter is capability-gated by the peer's
  // advertised supports_relative_mouse; exit always works and also fires
  // automatically on focus loss, window close, disconnect and display
  // switches. Deltas reaching SendRelativeMouseInput are already scaled to
  // the wire unit by the capturer.
  void EnterGameMode(StreamView& view);
  // Single source of the capture-scale math, shared by entry and the
  // per-tick refresh in SyncGameMode. False means the geometry is
  // momentarily unusable; callers keep the previous scale.
  bool ResolveGameModeScale(const std::shared_ptr<RemoteSession>& props,
                            GameModeScale& out);
  void ExitGameMode();
  void SendRelativeMouseInput(double dx, double dy) override;
  void SendGameModeButtonInput(int button, bool down) override;
  void SendGameModeWheelInput(double line_dx, double line_dy, double point_dx,
                              double point_dy) override;
  void RequestGameModeExit() override;
  void SyncGameMode();
  // Thread-safe direct send used by the game-mode hooks on the sender
  // thread: bypasses the UI-coupled SendRemoteInput path, validates the
  // session under the shared map lock and uses the MiniRtc entry points
  // documented as internally synchronized.
  void SendGameModeActionDirect(const std::string& target, RemoteAction& action,
                                float wheel_delta);
  // device-manager-facing entry points are declared on GuiRuntime instead.
  // Game-mode input hooks run on the device manager's sender thread, not
  // the UI thread. This mutex protects everything they share with the UI:
  // the pressed-button set used for exit cleanup, the wheel accumulator
  // dedicated to game mode (the Slint scroll path cannot fire while the
  // cursor is frozen, so the UI accumulator is never involved) and the
  // sensitivity snapshot taken at entry.
  std::mutex game_mode_mutex_;
  GameModePressedButtons game_mode_pressed_buttons_;
  ScrollAccumulator game_mode_scroll_;
  int game_mode_wheel_percent_ = 100;
  // Last scale pushed to the capturer, kept so the per-tick refresh in
  // SyncGameMode only calls SetMouseCapturerScale when the value actually
  // changed. UI-thread only; the capturer copies it into its own atomics.
  double game_mode_scale_x_ = 0;
  double game_mode_scale_y_ = 0;
#endif

  void Cleanup();

  void OpenFileDialog(const std::string& title,
                      std::function<void(std::string)> completion);
  bool OpenUrl(const std::string &url);

  std::unique_ptr<SlintUi> ui_;
#if CROSSDESK_HAS_UPDATER
  std::unique_ptr<UpdateChecker> update_checker_;
  std::unique_ptr<updater::Controller> updater_;
#endif
  CursorStateProvider cursor_state_provider_;
  std::unordered_map<std::string, CursorDeliveryState>
      cursor_delivery_states_;
  uint32_t cursor_state_sequence_ = 0;
  std::chrono::steady_clock::time_point next_video_frame_time_{};
  std::chrono::steady_clock::time_point last_video_frame_gap_log_time_{};
  std::atomic<bool> video_present_queued_{false};
  SignalConnectionIndicator signal_connection_indicator_;
#if (defined(__linux__) && !defined(__OHOS__)) && !defined(__APPLE__)
  bool use_xwayland_gui_ = false;
  bool use_x11_custom_titlebar_ = false;
#endif
};

} // namespace crossdesk

#endif
