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
- **macOS HEVC true color** — the stream-window `4:4:4` switch persists the host's
  capture preference. New peers negotiate chroma at connection time; ScreenCaptureKit
  can change format without restarting. Metal samples 8- and 10-bit 4:4:4 frames
  directly; they have no CPU NV12 preview or fallback. The default remains 4:2:0.
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
- **Zero-config LAN auto-discovery (D7b)** — the host and controller discover each other
  on the local network without manual address entry, using a cross-platform UDP broadcast
  / multicast beacon-and-listen exchange (`network/lan/lan_discovery`). Discovered peers
  surface in the main window for one-click connect. Implemented in the portable LAN layer
  with Windows (`Ws2_32`/`Iphlpapi`) and POSIX backends.
- **4 MiB UDP socket buffers (D7a)** — the bundled minirtc's libnice is patched to request
  4 MiB UDP send/receive buffers (best-effort, clamped by the OS ceiling), reducing
  kernel-level packet drops on high-bitrate high-frame-rate sessions.
- **Dynamic high-FPS degradation ladder (D6a)** — when bandwidth or encoder pressure
  forces degradation, the bundled minirtc now steps the per-stream frame-rate cap down
  (toward 96/60/48) *before* lowering spatial resolution, preserving razor-sharp native
  pixels on high-FPS screen content; see `deps/submodules/minirtc/docs/FORK_CHANGES.md`.
- **macOS hardware HEVC 4:4:4 10-bit true color (D10)** — end-to-end 4:4:4 10-bit HEVC on
  Apple Silicon: ScreenCaptureKit captures `xf44`, a Metal RG16 full-range shader renders
  it without a CPU NV12 fallback, and a `Color 4:2:0 / 4:4:4` toggle in the stream window
  persists the host preference. Chroma is negotiated at connection time
  (`a=x-minirtc-video-444:1`); peers that do not advertise it fall back to 4:2:0. Default
  remains 4:2:0.
- **Discreet wallpaper privacy cover (W3D)** — the privacy screen can now imitate the
  host's own desktop picture instead of an obvious branded/black cover, so bystanders
  see nothing unusual while ScreenCaptureKit keeps capturing the real desktop for the
  remote controller (the cover windows stay excluded exactly as before). A new
  cross-platform `PrivacyCoverStyle` (`branded` / `black` / `wallpaper`) persists in
  `config.ini` as `privacy_cover_style`; pre-style profiles carrying only
  `privacy_black_screen` migrate exactly, and the legacy boolean stays in sync for
  downgraded builds. On macOS the per-screen wallpaper loads through the public
  `NSWorkspace` API with aspect-fill center-crop scaling matching the macOS desktop and
  a calm dark-slate gradient fallback; the instant local `Control+Option+Shift+Esc`
  recovery chord is untouched and works identically in every style. Windows keeps its
  high-band cover and maps the wallpaper choice to the branded fallback; Linux is
  gamma-black only, as before. The default remains the branded cover.
- **High-bitrate ceiling and evidence-tuned start (D2)** — the bundled minirtc's
  network/encoder bitrate ceilings rise to 80/100 Mbps (env-overridable via
  `MINIRTC_MAX_NETWORK_BITRATE_BPS` / `MINIRTC_MAX_ENCODER_BITRATE_BPS`) and the direct
  session start bitrate defaults to 8 Mbps instead of 2.5 Mbps, eliminating the slow
  congestion-control ramp on modern LAN/Wi-Fi paths; A/B evidence in `wan/D2_BITRATE_AB.md`.
- **Bidirectional image clipboard sync (W3B)** — copying a screenshot or image on either
  endpoint pastes straight into remote applications. A new `supports_clipboard_image`
  capability gates chunked binary frames on the existing reliable clipboard stream
  (magic-prefixed so legacy text peers never see them): 32 KiB chunks with out-of-order
  reassembly, a hard 8 MiB compressed-PNG safety cap, 15 s stale-transfer eviction,
  cancel frames, duplicate suppression and FNV-1a echo control. PNG/TIFF on macOS,
  cross-platform tests in `clipboard_image_wire_test` / `clipboard_image_mac_test`.
- **Opt-in macOS virtual display (W3C)** — a runtime-resolved RAII wrapper for Apple's
  private `CGVirtualDisplay` SPI enables headless capture provisioning behind
  `FARLINK_MAC_VIRTUAL_DISPLAY=1`. The integration honestly reports two macOS 27.x
  platform defects verified on 27.2.1: teardown does not remove the display from
  WindowServer (leaked displays persist until logout) and mode switching after creation
  is immutable; the policy layer therefore provisions the correct resolution at creation
  time and never relies on teardown for correctness. Gated by
  `mac_virtual_display_policy_test`; the interactive smoke test is compile-only because
  of the teardown leak.

- **Zero-cloud Wake-on-LAN for saved devices (W4A)** — hosts advertise their
  primary station MAC inside the existing `host_info` message, strictly to
  authenticated direct-LAN controllers; controllers persist it in the
  recent-connection record. A Wake action on the recent-device card menu
  (shown only when a MAC is recorded) broadcasts the standard 102-byte magic
  packet to `255.255.255.255` plus every active IPv4 interface subnet
  broadcast address. The UI feedback states honestly that "sent" means
  on-the-wire, not yet online — whether the target machine actually wakes
  depends on its own firmware/OS wake configuration, which is outside this
  feature's control and untested here. Headless use: `--wol <mac>
  [--wol-broadcast <ip>]` sends and exits before instance acquisition so
  scripts never disturb a running UI. The v1 LAN beacon format is unchanged.

- **Encrypted loopback TCP port forwarding (W4B)** — controller-side
  `--port-forward <local>:<remote>` (repeatable, session-scoped) binds a
  loopback listener on the controller and tunnels TCP streams through the
  encrypted session's reliable data channel to a loopback destination on the
  host. Host consent is explicit and narrow: persistent consent exists only
  for cryptographically verified Ed25519 trusted devices (keyed
  `trusted/<public_key>` after a fresh-nonce proof against a pinned key);
  for unverified controllers the operator mechanism is the host-side
  `--port-forward-allow <port>[,<port>...]` process-lifetime grant, which
  applies to any authorized session of that host process while it runs (it
  is not per-remote narrowing — the implementation deliberately does not
  scope the CLI grant to one peer). Admission requires a live interactive
  encrypted session: view-only sessions never forward, and admission is
  re-evaluated per frame so losing authorization or encryption detaches the
  endpoint immediately with in-flight bytes dying in place. Reverse rules
  (`--port-forward-reverse`) bind on the accepting side only for trusted
  devices or explicit one-shot live approval. Unapproved destinations are
  refused visibly with `kErrorDenied` before any host-side socket opens;
  unreachable destinations answer `kErrorRefused`. Loopback-only at both
  ends by design — no LAN exposure is created beyond the existing session.
- **Stream control ergonomics (W4C)** — in-session bitrate cap presets
  (Auto/8M/20M/50M/80M) in the stream settings menu, per-device persisted,
  folded into the subscription `max_bitrate_bps` wire path so caps apply
  live without reconnecting (the stricter of cap and gallery allocator
  wins); stream hotkeys Ctrl+Alt+Left/Right cycling the remote display
  (wrapping) and Ctrl+Alt+F toggling fullscreen, intercepted in the input
  path with the completing key and its repeats swallowed locally; and a
  session-scoped host speaker mute toggle (capability-gated
  `supports_host_mute`) that mutes the controlled machine's physical
  default output through CoreAudio on macOS with RAII restoration of the
  exact original state — reference-counted across concurrent controllers
  so a mute can never outlive its session. Linux/Windows carry inert
  stubs that never silently half-apply.
- **Wave-5 core media upgrades (W5A/W5B in minirtc, W5C/W5D here)** — four
  independent verification-backed upgrades:
  - *W5A transport & pacer (minirtc)*: thread-safe MPMC `RingBuffer` + `Data`
    UB fix; immediate `ikcp_flush` + deadline-driven `KcpUpdateTimer` +
    event-driven `RtpDataSender` (loopback reliable one-way 9.3 ms → ~20 µs,
    ping-pong RTT 18.3 ms → ~50 µs); sub-millisecond `PacedSender` (500 µs/1 pkt
    holdback, 8 ms @60fps direct burst, RTX fast lane, FEC parity floor). Detail:
    `deps/submodules/minirtc/docs/FORK_CHANGES.md`.
  - *W5B visual quality & decoder (minirtc)*: Crisp Static Frame Refresh
    (`kQualityRefresh` + high-budget P-frame refinement), Sunshine-derived
    VideoToolbox `DataRateLimits` omission under `EnableLowLatencyRateControl` +
    infinite on-demand screen-content GOP + `EnableLTR=false` (+4.4 dB
    early-static H.264 PSNR, +1.3 dB HEVC), `RealTime` hardware decode +
    single-pass zero-intermediate-copy `CMBlockBuffer` assembly.
  - *W5C macOS async Metal + CAS + SCK*: a dedicated
    `QOS_CLASS_USER_INTERACTIVE` async Metal presentation queue decoupling
    `nextDrawable` blocking from the Slint UI/input main thread, Retina-native
    drawable sizing + envelope-clamped Contrast-Adaptive Sharpening (CAS)
    shader (−32% stroke blur width, zero halo/overshoot, `FARLINK_METAL_SHARPEN`
    override), and ScreenCaptureKit `queueDepth = 3` (`FARLINK_SCK_QUEUE_DEPTH`
    override).
  - *W5D input wire & pixel scroll*: 32-byte zero-allocation binary input
    protocol (`FIN1`, ~58× faster than JSON at ~37 ns/op vs ~2150 ns/op, 0 heap
    allocations), cumulative loss-immune relative mouse motion with
    click-boundary recovery, pixel-precise smooth macOS trackpad scrolling
    (`CGEventCreateScrollWheelEvent2` continuous pixel + 16.16 fixed-point
    deltas), and unreliable sequenced cursor-position updates.

- **Wave-6 color, presentation, decode & loss-recovery upgrades (W6A/W6B in
  minirtc, W6C/W6D here)** — four independent verification-backed upgrades:
  - *W6A zero-hop inline decode & zero-copy reassembly (minirtc)*: steady-state
    VideoToolbox frames submit inline from the network thread
    (`CanDecodeInline`/`TryDecodeInline`, exact `kMaxDecodeBacklog=2` admission,
    `MINIRTC_INLINE_DECODE=0` A/B), single-copy direct-write RTP frame
    reassembly for H.264/H.265/AV1, and end-to-end `420f` full-range BT.709
    VUI propagation/decode. Detail: `deps/submodules/minirtc/docs/FORK_CHANGES.md`.
  - *W6B wide-block RS-FEC & FEC-aware NACK (minirtc)*: NEON GF(256)
    Reed-Solomon fast path widened to `k <= 48, r <= 16`, negotiated via
    `a=x-minirtc-fec-wide:48-16` with legacy `rs-v1` bit-exact fallback
    (45-packet keyframe burst: legacy 16/4 recovers 0/10, wide 48/16 recovers
    10/10 bit-exact), plus an FEC-aware first-NACK hold-off for isolated
    1–2 packet gaps with burst bypass.
  - *W6C macOS full-range BT.709 capture + CAMetalDisplayLink*: ScreenCaptureKit
    captures `kCVPixelFormatType_420YpCbCr8BiPlanarFullRange` + ITU-R BT.709
    instead of limited-range BT.601 (the legacy path crushed 256 luma codes
    into the 220-code 16–235 studio swing and pushed sRGB reds/greens through
    the wrong matrix; `FARLINK_MAC_CAPTURE_LIMITED_601=1` restores it for A/B),
    and a dedicated `QOS_CLASS_USER_INTERACTIVE` runloop thread runs
    CAMetalDisplayLink (preferredFrameLatency 1, 60–120 Hz range, auto-pausing
    after two idle ticks so a static desktop costs zero wakeups;
    `FARLINK_METAL_PACING=immediate` keeps the W5C render-queue mode for A/B).
    Measured `macos_color_and_displaylink_test`: 256/256 distinct monotonic
    output levels with max roundtrip error 0 LSB (420v+BT.601 control: 220
    levels), sRGB primaries max channel error 1 LSB, 20/20 rapid attach/detach
    stress cycles with zero leaked threads.
  - *W6D per-stage latency attribution & input fast-lane*: the D8 stream stats
    overlay/diagnostic export now attributes end-to-end latency per stage
    (network RTT / jitter-buffer target / decode / render) with a
    `ClassifyLatencyBottleneck` dominant-contributor classifier mirroring the
    reference-grade interactive budget (sub-25 ms RTT, sub-8 ms jitter, sub-6 ms
    decode, sub-10 ms render), plus a fast-lane input dispatch path. Verified
    by `stage_latency_attribution_test`.

## Modified upstream files

| File | Nature of change |
|------|------------------|
| `apps/desktop/src/config_center/config_center.cpp` | Defaults for high frame-rate and HEVC codec on macOS; `privacy_cover_style` persistence with legacy `privacy_black_screen` migration. |
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
| `apps/desktop/src/privacy/privacy_backend.h` | `PrivacyCoverStyle` plumbing in `PrivacyScreenText`. |
| `apps/desktop/src/privacy/privacy_controller.cpp` | Cover-style change detection in `SetText`. |
| `apps/desktop/src/platform/macos/privacy/privacy_backend_mac.mm` | Per-screen cover style wiring. |
| `apps/desktop/src/platform/macos/privacy/privacy_cover_view.mm` | Wallpaper cover rendering with aspect-fill crop and gradient fallback. |
| `apps/desktop/src/platform/windows/privacy/privacy_band_guard.cpp` | Cross-platform cover-style mapping; wallpaper falls back to the branded cover. |
| `apps/desktop/src/platform/windows/privacy/privacy_band_ipc.h` | Band IPC enum renamed `PrivacyBandCoverStyle` for the cross-platform style type. |
| `apps/desktop/src/gui/runtime/privacy_runtime.cpp` | Cover-style sync into the privacy controller. |
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
| `apps/desktop/src/platform/macos/gui/metal_video_renderer.mm` | W5C async `QOS_CLASS_USER_INTERACTIVE` Metal presentation queue decoupling `nextDrawable` from the Slint UI main thread, Retina-native drawable sizing, CAS sharpening integration. |
| `apps/desktop/src/platform/macos/gui/metal_video_renderer.h` | Declarations for the above. |
| `apps/desktop/src/platform/macos/screen_capturer/screen_capturer_sck_impl.mm` | W5C ScreenCaptureKit `queueDepth = 3` (`FARLINK_SCK_QUEUE_DEPTH` override). |
| `apps/desktop/src/gui/features/input/remote_input_dispatcher.cpp` | W5D binary `InputWire` decode dispatch, pixel-scroll accumulation, unreliable cursor updates. |
| `apps/desktop/src/gui/features/input/scroll_accumulator.h` | W5D 16.16 fixed-point fractional scroll carry. |
| `libs/wire/src/remote_action.cpp` | W5D input-wire related capability serialization. |
| `apps/desktop/src/platform/macos/input/mouse/mouse_controller.cpp` | W5D pixel-precise `CGEventCreateScrollWheelEvent2` continuous-pixel scrolling. |
| `apps/desktop/src/platform/macos/gui/metal_video_renderer.mm` | W6C `CrossDeskMetalDisplayLinkDriver` — dedicated `QOS_CLASS_USER_INTERACTIVE` runloop thread running CAMetalDisplayLink with pre-acquired drawables, idle auto-pause, latched wakeup and synchronous invalidate+join teardown. |
| `apps/desktop/src/platform/macos/screen_capturer/screen_capturer_sck_impl.mm` | W6C full-range BT.709 capture (`kCVPixelFormatType_420YpCbCr8BiPlanarFullRange` + ITU_R_709_2; `FARLINK_MAC_CAPTURE_LIMITED_601=1` restores legacy limited-range BT.601). |
| `apps/desktop/src/gui/rendering/video_latency.cpp` | W6D per-stage rolling-mean latency attribution plumbing. |
| `apps/desktop/src/gui/rendering/video_latency.h` | W6D `LatencyBottleneck` per-stage classifier (`ClassifyLatencyBottleneck`/`LatencyBottleneckName`). |
| `apps/desktop/src/gui/features/input/remote_input_dispatcher.cpp` | W6D input fast-lane dispatch path. |
| `apps/desktop/src/gui/features/input/remote_input_dispatcher.h` | Declarations for the above. |
| `apps/desktop/src/gui/features/feedback/diagnostic_export.cpp` | W6D per-stage latency attribution in the redacted diagnostic bundle. |
| `apps/desktop/src/gui/features/feedback/diagnostic_export.h` | Declarations for the above. |
| `apps/desktop/src/gui/runtime/peer_media_callbacks.cpp` | W6D per-stage latency metric feed from the decode/render pipeline. |
| `apps/desktop/src/gui/ui/stream_window.slint` | W6D taller HUD layout rendering the per-stage latency attribution rows. |

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
| `apps/desktop/src/platform/macos/gui/metal_cas_shader_source.h` | W5C envelope-clamped Contrast-Adaptive Sharpening Metal shader source. |
| `apps/desktop/tests/metal_cas_renderer_test.mm` | W5C CAS shader + async renderer verification (−32% stroke blur width, zero halo/overshoot). |
| `libs/wire/include/input_wire.h` | W5D 32-byte zero-allocation binary input protocol (`FIN1`). |
| `libs/wire/tests/input_wire_test.cpp` | W5D input-wire round-trip/loss-recovery/benchmark tests. |
| `apps/desktop/tests/pixel_scroll_and_dispatcher_test.cpp` | W5D pixel-scroll fixed-point + dispatcher binary-input tests. |
| `apps/desktop/tests/macos_color_and_displaylink_test.mm` | W6C full-range BT.709 round-trip exactness (256/256 levels, 0 LSB error) + CAMetalDisplayLink lifecycle verification against the production renderer translation unit. |
| `apps/desktop/tests/stage_latency_attribution_test.cpp` | W6D per-stage latency attribution classifier and plumbing tests. |
