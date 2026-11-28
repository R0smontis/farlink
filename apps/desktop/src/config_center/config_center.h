/*
 * @Author: DI JUNKUN
 * @Date: 2024-05-29
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _CONFIG_CENTER_H_
#define _CONFIG_CENTER_H_

#include <minirtc.h>

#include <atomic>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <utility>
#include <string>
#include <vector>


namespace crossdesk {

class ConfigCenter {
 public:
  enum class LANGUAGE {
    CHINESE = 0, ENGLISH = 1, RUSSIAN = 2, TRADITIONAL_CHINESE = 3,
    JAPANESE = 4, KOREAN = 5, GERMAN = 6, FRENCH = 7, SPANISH = 8,
    BRAZILIAN_PORTUGUESE = 9, SYSTEM = 10
  };
  enum class CONNECTION_MODE { DEVICE_ID = 0, DIRECT_IP = 1 };
  using VIDEO_ENCODE_FORMAT = MiniRtcVideoCodec;
#if defined(__APPLE__)
  // Single source of truth for the platform's default codec, used by the member
  // initializer, by Load()'s missing-key fallback, and by its invalid-value
  // fallback. Apple hosts default to hardware HEVC; the SDP negotiation still
  // falls back to H.264 with peers that do not offer H.265.
  static constexpr VIDEO_ENCODE_FORMAT kDefaultVideoEncodeFormat =
      VIDEO_ENCODE_FORMAT::H265;
#else
  static constexpr VIDEO_ENCODE_FORMAT kDefaultVideoEncodeFormat =
      VIDEO_ENCODE_FORMAT::H264;
#endif
  enum class TURN_MODE {
    DISABLED = 0,
    AUTO_UDP_TCP = 1,
    FORCE_UDP = 2,
    FORCE_TCP = 3
  };

 public:
  explicit ConfigCenter(const std::string& config_path = "config.ini");
  ~ConfigCenter();

  struct StreamWindowSize {
    int width;
    int height;
    bool operator==(const StreamWindowSize& other) const {
      return width == other.width && height == other.height;
    }
  };

  struct BitrateTierConfig {
    std::string name;
    int mbps = 0;  // 0 = Auto, 1..100 = explicit Mbps ceiling
    bool operator==(const BitrateTierConfig& other) const {
      return name == other.name && mbps == other.mbps;
    }
  };

  struct NetworkOverlayConfig {
    float x_ratio = 0.015f;     // normalized position inside video area [0..1]
    float y_ratio = 0.03f;
    int width = 136;            // logical px (88..420)
    int height = 186;           // logical px (40..420)
    int font_size = 11;         // logical px (8..24)
    int opacity_percent = 78;   // 20..100
    int layout_mode = 0;        // 0 = vertical multi-line, 1 = compact horizontal
    bool show_clock = true;
    bool show_transport = true;
    bool show_fps = true;
    bool show_bitrate = true;
    bool show_rtt = true;
    bool show_frame_latency = true;
    bool show_loss = true;
    bool show_resolution_tier = true;
    bool show_codec = false;
    bool show_endpoints = false;
    bool operator==(const NetworkOverlayConfig& other) const {
      return x_ratio == other.x_ratio && y_ratio == other.y_ratio &&
             width == other.width && height == other.height &&
             font_size == other.font_size &&
             opacity_percent == other.opacity_percent &&
             layout_mode == other.layout_mode &&
             show_clock == other.show_clock &&
             show_transport == other.show_transport &&
             show_fps == other.show_fps &&
             show_bitrate == other.show_bitrate &&
             show_rtt == other.show_rtt &&
             show_frame_latency == other.show_frame_latency &&
             show_loss == other.show_loss &&
             show_resolution_tier == other.show_resolution_tier &&
             show_codec == other.show_codec &&
             show_endpoints == other.show_endpoints;
    }
  };

  // write config
  int SetLanguage(LANGUAGE language);
  int SetConnectionMode(CONNECTION_MODE mode);
  int SetIpDirectEnabled(bool enabled);
  int SetDesktopRealtimeRateControl(bool enabled);
  int SetVideoEncodeFormat(VIDEO_ENCODE_FORMAT video_encode_format);
  int SetVideoFrameRatePreference(int frame_rate);
  int SetVideoTrueColor444(bool enabled);
  int SetTurnMode(TURN_MODE turn_mode);
  int SetForceRelay(bool force_relay);
  int SetServerHost(const std::string& signal_server_host);
  int SetServerPort(int signal_server_port);
  int SetServerEndpoint(const std::string& address, int port);
  int SetSelfHosted(bool enable_self_hosted);
#if defined(__APPLE__) || (defined(__linux__) && !defined(__OHOS__))
  bool MigrateAutostart();
  static bool MigrateAutostartWhileRunning(const std::string& config_path);
#endif
  int SetAutostart(bool enabled);
  int SetDaemon(bool enable_daemon);
  int SetPrivacyScreen(bool enable_privacy_screen);
  int SetPrivacyLocalUnlock(bool allowed);
  int SetPrivacyBlackScreen(bool enabled);
  // style is a PrivacyCoverStyle value passed as int. Unknown values are
  // rejected; kBlack keeps the legacy privacy_black_screen key in sync.
  int SetPrivacyCoverStyle(int style);
  int SetLockOnDisconnect(bool enabled);
  int SetTrustedDevicesOnly(bool enabled);
  int SetSaveRemotePreviews(bool enabled);
  int SetPauseVideoWhenMinimized(bool enabled);
  int SetInputSensitivity(const std::string& device, int mouse_percent, int wheel_percent);
  // mode: 0 = send shortcuts to the remote session (swallow locally),
  // 1 = prefer local shortcuts (keys still reach the peer as copies).
  // Anything outside [0, 1] is rejected; unknown devices default to 0.
  int SetKeyboardShortcutMode(const std::string& device, int mode);
  // bps: 0 = Auto (no cap, congestion/encoder ceiling applies), otherwise
  // exactly one explicit preset ceiling in bits per second. Values outside
  // the preset set are rejected; unknown devices default to 0.
  int SetVideoBitrateCapBps(const std::string& device, int bps);
  int SetBitrateTiers(const std::vector<BitrateTierConfig>& tiers);
  int SetNetworkOverlayConfig(const NetworkOverlayConfig& config);
  int SetStreamWindowSize(const std::string& device, StreamWindowSize size);
  int SetPortableServicePromptSuppressed(bool suppressed);
  int SetLastPresentedRelease(const std::string& version);
  int SetFileTransferSavePath(const std::string& path);
  // Host-side consent: the exact destination TCP ports an incoming controller
  // device may ask this host to connect to on 127.0.0.1. Persisted per
  // device key; unknown devices default to the empty set (deny all). Only
  // ports 1-65535 are stored; zero and duplicates never reach the file.
  int SetPortForwardAllowlist(const std::string& device,
                              const std::set<std::uint16_t>& ports);
  // Controller-side rules for one outgoing device: pairs of
  // loopback local listen port -> remote loopback destination port.
  int SetPortForwardRules(const std::string& device,
                          const std::map<std::uint16_t, std::uint16_t>& rules);

  // read config

  LANGUAGE GetLanguage() const;
  CONNECTION_MODE GetConnectionMode() const;
  bool IsIpDirectEnabled() const;
  VIDEO_ENCODE_FORMAT GetVideoEncodeFormat() const;
  bool IsDesktopRealtimeRateControlEnabled() const { return desktop_realtime_rate_control_.load(); }
  MiniRtcDesktopEncodingMode GetDesktopEncodingMode() const {
    return IsDesktopRealtimeRateControlEnabled() ? MiniRtcDesktopEncodingMode::LowLoad
                                                : MiniRtcDesktopEncodingMode::EncoderDefault;
  }
  TURN_MODE GetTurnMode() const;
  int GetDirectStartBitrateBps() const;
  // The controller-side preferred capture frame-rate tier (30/60/90/120/144),
  // applied at session start as min(preference, peer capability, display rate).
  // It is the user's remembered choice, never rewritten by a clamp — only by
  // the user picking a tier. Default 60. Distinct from the legacy per-connection
  // video_frame_rate key the migration deletes.
  int GetVideoFrameRatePreference() const { return video_frame_rate_preference_.load(); }
  // Host-wide capture preference, also offered to new peers during negotiation.
  bool IsVideoTrueColor444() const { return video_true_color_444_.load() != 0; }
  int GetRelayStartBitrateBps() const;
  bool IsReceiverPaddingProbingDisabled() const;
  bool IsForceRelay() const;
  bool IsEnableSrtp() const;
  std::string GetSignalServerHost() const;
  int GetSignalServerPort() const;
  std::string GetDefaultServerHost() const;
  int GetDefaultSignalServerPort() const;
  bool IsSelfHosted() const;
  bool IsEnableAutostart() const;
  bool IsEnableDaemon() const;
  bool IsEnablePrivacyScreen() const;
  bool IsPrivacyLocalUnlockAllowed() const;
  bool IsPrivacyBlackScreen() const { return privacy_black_screen_.load(); }
  int GetPrivacyCoverStyle() const { return privacy_cover_style_.load(); }
  bool IsLockOnDisconnect() const;
  bool IsTrustedDevicesOnly() const;
  bool IsSaveRemotePreviews() const;
  bool IsPauseVideoWhenMinimized() const;
  int GetMouseSensitivityPercent() const { return mouse_sensitivity_percent_.load(); }
  int GetWheelSensitivityPercent(const std::string& device) const;
  int GetKeyboardShortcutMode(const std::string& device) const;
  int GetVideoBitrateCapBps(const std::string& device) const;
  std::vector<BitrateTierConfig> GetBitrateTiers() const;
  NetworkOverlayConfig GetNetworkOverlayConfig() const;
  std::optional<StreamWindowSize> GetStreamWindowSize(const std::string& device) const;
  bool IsPortableServicePromptSuppressed() const;
  std::string GetLastPresentedRelease() const;
  std::string GetFileTransferSavePath() const;
  std::set<std::uint16_t> GetPortForwardAllowlist(const std::string& device) const;
  std::map<std::uint16_t, std::uint16_t> GetPortForwardRules(
      const std::string& device) const;

  int Load();
  int Save();
  // Only connection/admission preferences cross into the SYSTEM host profile.
  std::string ExportSharedHostConfig() const;
  int ApplySharedHostConfig(const std::string& contents);

 private:
  int CommitIni(bool warn_only = false);
  int StoreValues(std::initializer_list<std::pair<const char*, std::string>> values);
  mutable std::mutex file_path_mutex_;
  std::string config_path_;
  struct IniStorage;
  std::unique_ptr<IniStorage> ini_;
  const char* section_ = "Settings";

  LANGUAGE language_ = LANGUAGE::SYSTEM;
  CONNECTION_MODE connection_mode_ = CONNECTION_MODE::DEVICE_ID;
  bool enable_ip_direct_ = true;
  std::atomic<bool> desktop_realtime_rate_control_{true};
  // This member initializer is what a fresh profile actually gets: Load()
  // returns early (before the key is read) when no config file exists yet, so
  // the in-memory default must already be the platform default.
  VIDEO_ENCODE_FORMAT video_encode_format_ = kDefaultVideoEncodeFormat;
  TURN_MODE turn_mode_ = TURN_MODE::AUTO_UDP_TCP;
  // 8 Mbps matches the library default since D2 — measured safe on a
  // constrained 4 Mbps WAN with probing enabled (wan/D2_BITRATE_AB.md) and
  // strictly better for the opening seconds of fast-LAN sessions. Persisted
  // user values are never rewritten upward by this change.
  static constexpr int kDefaultDirectStartBitrateBps = 8'000'000;
  static constexpr int kDefaultRelayStartBitrateBps = 1'500'000;
  int direct_start_bitrate_bps_ = kDefaultDirectStartBitrateBps;
  int relay_start_bitrate_bps_ = kDefaultRelayStartBitrateBps;
  static constexpr bool kDefaultDisableReceiverPaddingProbing = true;
  bool disable_receiver_padding_probing_ = kDefaultDisableReceiverPaddingProbing;
  std::string signal_server_host_ = "";
  // This fork operates no signalling infrastructure: the factory default is an
  // empty host, and the client stays offline until the user points it at a
  // self-hosted server (or uses direct LAN connections). The upstream public
  // service is never contacted.
  std::string signal_server_host_default_ = "";
  int signal_server_port_ = 0;
  int server_port_default_ = 9099;
  bool enable_self_hosted_ = false;
  bool enable_autostart_ = false;
  bool enable_daemon_ = false;
  std::atomic<bool> enable_privacy_screen_{true};
  std::atomic<bool> privacy_local_unlock_allowed_{true};
  std::atomic<bool> privacy_black_screen_{false};
  std::atomic<int> privacy_cover_style_{0};  // PrivacyCoverStyle::kBranded
  std::atomic<bool> lock_on_disconnect_{true};
  std::atomic<bool> trusted_devices_only_{false};
  std::atomic<bool> save_remote_previews_{true};
  std::atomic<bool> pause_video_when_minimized_{true};
  std::atomic<int> mouse_sensitivity_percent_{100};
  std::atomic<int> video_frame_rate_preference_{60};
  std::atomic<int> video_true_color_444_{0};
  int legacy_wheel_sensitivity_percent_ = 100;
  std::map<std::string, int> device_wheel_sensitivity_;
  std::map<std::string, int> device_keyboard_shortcut_mode_;
  std::map<std::string, int> device_video_bitrate_cap_bps_;
  std::vector<BitrateTierConfig> bitrate_tiers_;
  NetworkOverlayConfig network_overlay_config_;
  bool portable_service_prompt_suppressed_ = false;
  std::string last_presented_release_;
  std::string file_transfer_save_path_ = "";
  std::map<std::string, std::set<std::uint16_t>> device_port_forward_allowlist_;
  std::map<std::string, std::map<std::uint16_t, std::uint16_t>>
      device_port_forward_rules_;
};
}  // namespace crossdesk
#endif
