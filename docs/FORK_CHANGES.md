# Fork Changes

**Farlink** is a modified version of **CrossDesk** (<https://github.com/kunkundi/crossdesk>),
forked at upstream base commit `0551055` (`[fix] migrate legacy autostart settings on upgrade, refs #242`).
Fork modification date range: **2026-11-25 – 2026-11-27**.

This file is the prominent notice of modification required by the GNU General Public
License v3 §5(a) (and, for the bundled `deps/submodules/minirtc`, the GNU Lesser General
Public License v3 §5(a), which inherits the same clause). It records that this work has
been modified relative to the upstream project, gives the relevant dates, and summarizes
the nature of the changes. The authoritative, line-level detail is the git history of this
repository (fork commits are prefixed `[feat]` / `[fix]` / `[chore]` / `[test]`).

The bundled media library `deps/submodules/minirtc` is itself a modified version of
<https://github.com/kunkundi/minirtc>; see `deps/submodules/minirtc/docs/FORK_CHANGES.md`.

## Summary of modifications

The fork extends upstream CrossDesk across these independent areas:

- **High frame-rate capture/encode** — frame-rate tiers beyond 60 fps (30/60/90/120/144)
  negotiated via a new `supports_high_fps` wire capability, a GPU-native CVPixelBuffer
  scaler, encoder-throughput-driven adaptation, and per-cause frame-drop accounting.
- **Game / relative-mouse input mode** — a captured-mouse mode for remote gaming with
  relative-mouse injection, negotiated via a new `supports_relative_mouse` wire capability.
- **Bounded audio jitter buffer** — a receive-side jitter buffer with reorder absorption
  and output trimming for the audio path.
- **Stereo audio (D11)** — 48 kHz stereo Opus capture/playback with backward-compatible
  SDP negotiation, mono legacy fallback, mono downmix fan-out for mixed-peer sessions,
  and DTX; proven by an in-process stereo round-trip test.
- **Adaptive video playout (D4)** — a jitter-buffered video playout path in the bundled
  minirtc that smooths inter-presentation timing under jitter; the live target delays are
  surfaced in the stream stats overlay.
- **Static-screenshare power saving (D5)** — an allocation-free 0 Hz frame detector in the
  bundled minirtc that idles the encoder on static screens and wakes on change.
- **Loss-aware congestion recovery (D3)** — binary-band RTX duplicate suppression with a
  per-packet cap, a continuous 1%-quantized FEC protection curve with a 35% heavy ceiling,
  and a FIR minimum-interval throttle in the bundled minirtc.
- **Host screen-lock awareness** — the host pauses video forwarding while its screen is
  locked and forces an IDR on unlock so the viewer recovers immediately.
- **Live QoE overlay and redacted diagnostic export (D8)** — the stream stats overlay now
  renders the live D8 quality-of-experience metrics, and the diagnostic bundle export is
  redacted with a live session summary and a stream-window action.
- **macOS input ergonomics** — side-aware modifier tracking, modifier tap recovery, a
  shortcut policy, and a chord-leak fix for the macOS input path.
- **Session observability and encoder cold-start priming** — machine-readable session
  metrics (per-second fps histogram, resolution series, network samples, session-cumulative
  inter-presentation gap), a non-interactive `--lan-connect` direct-connect entry that keeps
  the password out of the command line, and hardware encoder priming in the bundled minirtc
  (see its own `docs/FORK_CHANGES.md`).

## Modified upstream files

| File | Nature of change |
|------|------------------|
| `apps/desktop/src/config_center/config_center.cpp` | Defaults for high frame-rate and HEVC codec on macOS. |
| `apps/desktop/src/gui/application/application_state.h` | State for capability flags and game mode. |
| `apps/desktop/src/gui/application/gui_application.cpp` | Capability gating, game-mode and high-fps wiring. |
| `apps/desktop/src/gui/application/gui_application.h` | Declarations for the above. |
| `apps/desktop/src/gui/assets/localization/localization_data.h` | Regenerated localization resources. |
| `apps/desktop/src/gui/features/devices/session_device_manager.cpp` | Frame-rate capture init, audio output, mouse capture. |
| `apps/desktop/src/gui/features/devices/session_device_manager.h` | Declarations for the above. |
| `apps/desktop/src/gui/features/input/remote_input_dispatcher.cpp` | Relative-mouse input dispatch. |
| `apps/desktop/src/gui/runtime/gui_runtime.cpp` | High-fps degradation-preference wiring. |
| `apps/desktop/src/gui/runtime/gui_runtime.h` | Declarations for the above. |
| `apps/desktop/src/gui/runtime/lan_runtime.cpp` | High-fps degradation-preference wiring. |
| `apps/desktop/src/gui/runtime/peer_data_callbacks.cpp` | Parse peer capability flags, video-settings renegotiation. |
| `apps/desktop/src/gui/runtime/peer_event_handler.cpp` | Advertise/reset capability flags. |
| `apps/desktop/src/gui/runtime/remote_session.h` | Per-session capability state. |
| `apps/desktop/src/gui/ui/common.slint` | UI strings/layout for new controls. |
| `apps/desktop/src/gui/ui/stream_window.slint` | Stream-window UI wiring. |
| `apps/desktop/src/gui/ui/ui_localization.h` | UI localization binding. |
| `apps/desktop/src/app/launch_options.cpp` | Non-interactive `--lan-connect` / `--view-only` launch options. |
| `apps/desktop/src/app/launch_options.h` | Declarations for the above. |
| `apps/desktop/src/gui/rendering/video_latency.cpp` | Session-cumulative inter-presentation gap metric and machine-readable session metrics emission. |
| `apps/desktop/src/gui/rendering/video_latency.h` | Declarations for the above. |
| `apps/desktop/src/platform/macos/gui/window_drag_mac.mm` | Window-drag interaction with capture mode. |
| `apps/desktop/src/platform/macos/input/mouse/mouse_controller.cpp` | Relative-mouse control integration. |
| `apps/desktop/src/platform/macos/input/mouse/mouse_controller.h` | Declarations for the above. |
| `apps/desktop/src/platform/macos/screen_capturer/screen_capturer_sck_impl.mm` | Zero-copy capture path for high frame-rate. |
| `apps/desktop/src/platform/window_drag.h` | Window-drag interface change. |
| `apps/desktop/src/platform/windows/input/mouse/mouse_controller.cpp` | Windows relative-mouse injection. |
| `apps/desktop/src/platform/windows/input/mouse/mouse_controller.h` | Declarations for the above. |
| `apps/desktop/xmake/targets.lua` | New test/harness build targets. |
| `libs/wire/include/preview_subscription.h` | Frame-rate field validation for high tiers. |
| `libs/wire/include/remote_action.h` | New capability flags in host-info message. |
| `libs/wire/src/remote_action.cpp` | Serialize/parse the new capability flags. |
| `libs/wire/tests/wire_test.cpp` | Wire round-trip test updates. |
| `localization/catalog.json` | New localized strings. |
| `xmake.lua` | Build wiring for new targets; pins `target_minver=14.0` so local macOS builds match upstream CI's deployment target (upstream passes it only as a CI flag). |

## Added files

| File | Purpose |
|------|---------|
| `apps/desktop/src/gui/features/devices/audio_output_trim.h` | Audio output trimming helper. |
| `apps/desktop/src/gui/features/input/captured_mouse_queue.h` | Thread-safe captured-mouse event queue. |
| `apps/desktop/src/gui/features/input/game_mode_buttons.h` | Game-mode button mapping. |
| `apps/desktop/src/gui/features/input/game_mode_capture_state.h` | Game-mode capture state machine. |
| `apps/desktop/src/gui/features/input/game_mode_scale.h` | Game-mode input scaling. |
| `apps/desktop/src/platform/macos/input/mouse/mouse_capturer.h` | macOS mouse capturer interface. |
| `apps/desktop/src/platform/macos/input/mouse/mouse_capturer.mm` | macOS mouse capturer implementation. |
| `apps/desktop/src/platform/macos/input/mouse/relative_mouse_capture.h` | Relative-mouse capture helper. |
| `apps/desktop/src/platform/macos/input/mouse/relative_mouse_injection.h` | Relative-mouse injection helper. |
| `apps/desktop/tests/audio_jitter_buffer_test.cpp` | Jitter-buffer unit tests. |
| `apps/desktop/tests/audio_output_trim_test.cpp` | Audio output-trim tests. |
| `apps/desktop/tests/audio_receive_e2e_test.cpp` | In-process audio receive-path end-to-end test. |
| `apps/desktop/tests/captured_mouse_queue_test.cpp` | Captured-mouse queue tests. |
| `apps/desktop/tests/captured_mouse_queue_tsan_stress.cpp` | TSan stress test for the mouse queue. |
| `apps/desktop/tests/encode_throughput_probe.cpp` | Encoder throughput probe. |
| `apps/desktop/tests/encoder_ceiling_reprobe_test.cpp` | Encoder-ceiling re-probe policy/property test. |
| `apps/desktop/tests/frame_rate_harness.cpp` | Two-endpoint frame-rate measurement harness. |
| `apps/desktop/tests/game_mode_buttons_test.cpp` | Game-mode button tests. |
| `apps/desktop/tests/game_mode_capture_state_test.cpp` | Game-mode capture-state tests. |
| `apps/desktop/tests/game_mode_exit_ordering_test.cpp` | Game-mode exit-ordering tests. |
| `apps/desktop/tests/game_mode_input_latency_bench.cpp` | Game-mode input-latency benchmark. |
| `apps/desktop/tests/game_mode_scale_test.cpp` | Game-mode scaling tests. |
| `apps/desktop/tests/iosurface_frame_source.h` | IOSurface native-frame source (harness). |
| `apps/desktop/tests/iosurface_frame_source.mm` | IOSurface native-frame source implementation. |
| `apps/desktop/tests/macos_capture_restart_tool.mm` | Capture-restart test tool. |
| `apps/desktop/tests/macos_capture_sigkill_tool.mm` | Capture-sigkill test tool. |
| `apps/desktop/tests/macos_chord_release_tool.mm` | Chord-release test tool. |
| `apps/desktop/tests/macos_relative_mouse_edge_tool.mm` | Relative-mouse edge test tool. |
| `apps/desktop/tests/macos_relative_mouse_injection_test.cpp` | Relative-mouse injection tests. |
| `apps/desktop/tests/macos_relative_mouse_roundtrip_tool.mm` | Relative-mouse roundtrip tool. |
| `apps/desktop/tests/minirtc_direct_harness.cpp` | Direct two-peer MiniRTC harness. |
| `apps/desktop/tests/udp_shaper.cpp` | Test-only UDP bottleneck shaper for constrained-link measurement. |
| `apps/desktop/tests/udp_shaper.h` | Declarations for the UDP bottleneck shaper. |
| `apps/desktop/tests/video_toolbox_in_flight_test.cpp` | VideoToolbox in-flight contract test. |
| `libs/wire/include/video_frame_rate.h` | Frame-rate tiers and degradation-preference logic. |
| `libs/wire/tests/video_frame_rate_wire_test.cpp` | Frame-rate wire round-trip tests. |
| `apps/desktop/tests/inter_present_emission_test.cpp` | Inter-presentation gap metric emission tests. |
| `apps/desktop/tests/inter_present_gap_test.cpp` | Session-cumulative inter-presentation gap tests. |
| `apps/desktop/src/common/screen_lock_monitor.h` | Host screen-lock state monitor (lock/unlock awareness). |
| `apps/desktop/src/gui/features/feedback/diagnostic_redaction.h` | Redaction helpers for the diagnostic bundle export. |
| `apps/desktop/src/platform/macos/input/macos_modifier_key_tracker.h` | Side-aware macOS modifier-key tracker. |
| `apps/desktop/tests/audio_stereo_roundtrip_test.cpp` | D11 stereo Opus round-trip proof (separation, mono fallback, downmix, DTX, loss+jitter). |
| `apps/desktop/tests/build_identity_stub.cpp` | Build-identity stub for the redaction test. |
| `apps/desktop/tests/diagnostic_export_redaction_test.cpp` | Diagnostic export redaction/ZIP assembly tests. |
| `apps/desktop/tests/macos_modifier_key_tracker_test.cpp` | Modifier-key tracker unit tests. |
| `apps/desktop/tests/screen_lock_monitor_test.cpp` | Screen-lock monitor unit tests. |
