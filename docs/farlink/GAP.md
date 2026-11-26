# Farlink 差距分析：CrossDesk → 对标商业远控 体验

> 分析日期 2026-11-25。所有 CrossDesk/MiniRTC 行为论断均带 `path:line` 证据；商业对标产品侧事实来自 `BENCHMARK.md`（标注来源编号），无法溯源的标 `未证实`。
> 路径缩写：`CD` = `/tmp/crossdesk`，`MR` = `/tmp/minirtc`。
> **版本核对**：`/tmp/minirtc` 检出 = CrossDesk submodule 钉住的 `c3b26b8f0f03fc43666e985052635c2573ded8f8`（`git -C /tmp/crossdesk ls-tree HEAD deps/submodules/minirtc`），**两者一致，无偏差**。

---

## 1. 管线现状（macOS 端到端）

### 1.1 视频：采集 → 编码 → 发送（被控端）

| 阶段 | 实际行为 | 关键参数 | 证据 |
|---|---|---|---|
| 采集 API | ScreenCaptureKit（`SCStream`+`SCStreamOutputTypeScreen`），要求 macOS 14+ | `pixelFormat=420YpCbCr8BiPlanarVideoRange`（NV12 video-range）、`colorMatrix=ITU_R_601_4`、`width/height=contentRect×pointPixelScale`（Retina 物理像素）、`captureResolution=Automatic`、`showsCursor` 按控制器类型决定 | `CD/apps/desktop/src/platform/macos/screen_capturer/screen_capturer_sck_impl.mm:705-712` |
| 采集帧率上限 | `minimumFrameInterval=CMTimeMake(1, fps_)`；**fps 恒为 60**，Init 时写死、永不按 30fps 重启（30fps 订阅是采集后丢帧实现） | `const int fps = 60` | `CD/.../session_device_manager.cpp:190`、`screen_capturer_sck_impl.mm:712,362`；不重启注释 `session_device_manager.cpp:186-190` |
| 采集→发送拷贝 | **零拷贝**：SCK 回调直接产出借用型 `CVPixelBuffer` native 描述符（`data=nullptr`），只有按行 memcpy 的 `copy_to_nv12` 后备 | native 描述符构造 | `screen_capturer_sck_impl.mm:966-980`；逐行拷贝后备 `:64-118` |
| 采集侧节奏闸 | 60fps 截止时间闸 `ShouldSendCapturedFrame`（1ms 容忍），之后每连接 `VideoFrameCadence`（1ms 容忍、不累积欠帧） | — | `session_device_manager.cpp:78-92`、`MR/src/transport/video_frame_cadence.h:19-30` |
| 发送闸门 | ①采集闸 ②每连接 cadence ③`VideoEncoderRateControl::DropFrame`（CWND 丢帧+WebRTC FrameDropper）④**编码队列已有 1 个 pending 就丢新帧**（latest-wins 深度 1） | — | `MR/src/transport/ice_transport_controller.cpp:849,864,894-897`、`MR/src/transport/video_encoder_rate_control.h:63-92` |
| 缩放 | 目标分辨率==采集分辨率时真零拷贝；**但 viewer 默认 v2 订阅发视口物理像素宽高**（通常 < Retina 全幅）→ 多数情况落到 `copy_to_nv12` 全帧 CPU 拷贝 + libyuv `ScaleNv12ViaI420`（屏幕内容 `kFilterBox`） | v2 视口计算带 200ms 去抖 | `MR/src/frame/raw_frame.h:52-88`、`ice_transport_controller.cpp:1056-1074`、`MR/src/media/resolution_adapter/resolution_adapter.cpp:54-58,378-400`、`CD/apps/desktop/src/gui/application/gui_application.cpp:6985-6992` |
| 编码器 | VideoToolbox 硬编，H.264 默认（HEVC 可被对端偏好协商触发） | `RealTime=true`、`MoreFramesBeforeStart=false`、`AllowFrameReordering=false`（**无 B 帧**）、`MaxFrameDelayCount=1`、profile `H264_High_AutoLevel`（低延迟会话）或 `H264_High_5_2` | `MR/src/media/video/encode/video_toolbox/video_toolbox_encoder.mm:269-280,303-308` |
| 低延迟码控 | `EnableLowLatencyRateControl` 作为 encoder spec 传入（选中 `*.rtvc` 编码器），失败时回退标准码控；macOS 15+ 且 desktop_realtime_rate_control 时尝试 `SpatialAdaptiveQPLevel=Disable` | 默认开（`config_center` 只在用户显式 disable 时关） | `video_toolbox_encoder.mm:234-247,311-323`、`MR/src/pc/peer_connection.cpp:213-214` |
| 码率控制 | 低延迟会话用 `AverageBitRate`+`DataRateLimits={bitrate/8, 1s}`；非低延迟优先 `ConstantBitRate`（macOS 13+）；GCC 估计 → `SetTargetBitrate` → 重写 `AverageBitRate` | — | `video_toolbox_encoder.mm:378-430,732-746`、`ice_transport_controller.cpp:4320-4350` |
| 关键帧间隔 | `MINIRTC_KEY_FRAME_INTERVAL=3000` **帧**（60fps 下 50s、30fps 下 100s），另有断流 500ms/切屏/恢复时 `ForceIdr` | — | `MR/src/media/media_codec.h:41`、`video_toolbox_encoder.mm:288-291,582-585`、`session_device_manager.cpp:34,252-275` |
| 编码速度/质量 | `degradation_preference==MaintainFrameRate` 时 `PrioritizeEncodingSpeedOverQuality=true`；**默认偏好 MaintainResolution=不加** | — | `video_toolbox_encoder.mm:169-171,432-449` |
| 4:4:4 | **完全不支持**：采集 NV12 4:2:0、编码器只接受 420 NV12 输入、profile 无 444 变体 | — | `screen_capturer_sck_impl.mm:705`、`video_toolbox_encoder.mm:470,527-530` |
| 帧率上限 | **硬上限 60**：`NormalizeVideoFrameRate` 非 30 即 60；wire 校验 `frame_rate∈{30,60}`、`fps` 值域 0-60；VT `SetTargetFrameRate` 拒绝 >60 | — | `MR/src/pc/peer_connection.cpp:72-74`、`CD/libs/wire/include/preview_subscription.h:86-98`、`video_toolbox_encoder.mm:750` |

### 1.2 网络：打包 → 拥塞控制 → 抗丢包

| 阶段 | 实际行为 | 关键参数 | 证据 |
|---|---|---|---|
| RTP 打包 | 视频最大载荷 1150B；`fec+rtx` 同开时降到 **1100B** | H264 FU-A | `MR/src/rtp/rtp_packet/rtp_defines.h:14`、`MR/src/transport/channel/video_channel_send.cpp:129` |
| Pacer | WebRTC `PacingController` leaky bucket，1ms 高精度定时；bitrate=BWE×**2.5**；burst interval **direct 40ms / relay 5ms** | 屏幕内容队列时限 100ms（超时拒新帧） | `MR/src/qos/pacing_controller.cc:43-49`、`MR/src/qos/congestion_control.cpp:19-20`、`ice_transport_controller.cpp:34,45,197-202` |
| 优先级 | audio 0 → audio-RTX 1 → video-RTX 2 → **video 与 FEC 同级 3** → padding 4 | FEC 对媒体无优先级 | `MR/src/qos/prioritized_packet_queue.cc:34-61` |
| 拥塞控制 | 混合 GCC：delay-based（trendline）+ loss-based v2；起步 **direct 2.5Mbps / relay 1.5Mbps**，min 300k、max **80Mbps** | 启动探测 3×→6×（2.5→7.5→15Mbps）；丢包阈值 2%/10% | `congestion_control.cpp:55-70`、`MR/src/common/bitrate_limits.h:14,19-21`、`MR/src/qos/probe_controller.h:32-42`、`MR/src/qos/send_side_bandwidth_estimation.cc:34-45` |
| FEC | 私有 RS-FEC（GF(2⁸) Reed-Solomon，`x-minirtc-fec:rs-v1` PT122，**非标 ULPFEC/FlexFEC**）；CrossDesk↔CrossDesk 默认开（offer 恒带），自适应 0-25%，关键帧+5%；窗口 150ms | 小帧 `rs_small`、大帧 OpenFEC；RTX 预算 = `video_budget/2 − fec` | `MR/src/fec/rtp_fec.h:13-22`、`MR/src/fec/fec_encoder.cpp:43-80`、`MR/src/fec/fec_adaptation_controller.cpp:150-165`、`ice_transport.cpp:1228,1270-1274` |
| NACK/RTX | 首 NACK 延迟 `clamp(rtt/4,0,10)ms`；RTX 走 pacer 优先级 2；RTX token bucket + 200ms 延迟队列；capture 龄 ≥1s 丢 | — | `MR/src/qos/nack_requester.cc:22,26-32`、`video_channel_send.cpp:404-465`、`paced_sender.cpp:96-110,259-287` |
| SRTP | AES-128-GCM 16B tag，DTLS 握手（20s 超时）后装密钥 | 每包 +16B、SSRC map+memcpy | `MR/src/srtp/srtp_engine.h:20-22`、`ice_transport_controller.cpp:2609-2636`、`ice_agent.h:204` |
| 打洞 | 仅在 relay 已选+DTLS 完成后启动（+1.5s 触发延迟），128 socket/1024 端口/128pps/3 轮×10s；relay-upgrade 窗口 30s | — | `MR/src/ice/punch_config.h:17-31`、`ice_agent.cpp:739,1680-1700` |

### 1.3 视频：接收 → 解码 → 渲染（控制端）

| 阶段 | 实际行为 | 关键参数 | 证据 |
|---|---|---|---|
| 抖动缓冲 | **没有 jitter buffer**：帧凑齐即投解码；缓冲只发生在帧粒度排序。恢复等待：无 RTT 时 soft 200ms/hard 600ms；有 RTT 时 soft `clamp(2·rtt+30,100,500)`、hard `clamp(5·rtt+80,350,1000)`、绝对上限 1500ms | 缺包帧会阻塞后续帧投递直到恢复截止 | `MR/src/transport/channel/rtp_video_receiver.cpp:38,845-856,1213-1330`、`MR/src/qos/video_recovery_timing.h:39-59` |
| 解码 | VideoToolbox 异步解码，`DisplayImmediately=true`+`NotSync`、硬解优先（H.265 require）；**未设 RealTime/输出队列限制**；输出 IOSurface-backed NV12 CVPixelBuffer，Metal 兼容 | 参数集变化或解码失败仅在关键帧重建会话 | `MR/src/media/video/decode/video_toolbox/video_toolbox_decoder.mm:334-357,405-441` |
| 解码→渲染 | **真零拷贝**：CVPixelBuffer→`CVMetalTextureCacheCreateTextureFromImage`（Y=R8、UV=RG8）→ 直接采样；无 CPU 拷贝无 upload | CPU 回落路径才有 2 次 memcpy | `CD/apps/desktop/src/platform/macos/gui/metal_video_renderer.mm:898-921,1069-1076` |
| 呈现调度 | 无 fps 节流；解码回调→renderer 3 槽 latest-wins（pending 被替换即丢）→ 单次 coalesced `dispatch_async(main)` → `nextDrawable`（**vsync 阻塞**，`displaySyncEnabled=YES`、`maximumDrawableCount=2`）→ present | macOS 上显式关掉 60Hz 兜底定时器 | `metal_video_renderer.mm:258,936-975,1125-1129,1309`、`CD/.../video_frame_scheduler.mm:57-70`、`gui_application.cpp:4383-4394` |
| 附加开销 | 桌面墙预览可见时**每帧**在解码线程做 `copy_to_nv12`+libyuv 缩放+`NV12ToABGR` 全帧 CPU 转换 | — | `CD/.../peer_media_callbacks.cpp:63-114` |

### 1.4 音频

| 阶段 | 实际行为 | 关键参数 | 证据 |
|---|---|---|---|
| 采集 | SCK 音频输出 48kHz 单声道 float→s16，**固定 960B（10ms）切块，尾部不足直接丢** | 无自有队列，SCK 串行队列直发 | `CD/.../speaker_capturer_macosx.mm:120-175,199,242-244` |
| 编码 | libopus `OPUS_APPLICATION_VOIP`、强制 10ms 帧；**桌面↔桌面实际无 inband FEC、无 DTX、码率 libopus 默认**（仅 SDP 声明 useinbandfec） | 开 FEC 才设 32kbps | `MR/src/media/audio/encode/audio_encoder.cpp:31-53`、`MR/src/media/media_codec.h:76` |
| 发送 | **绕过 pacer**（入队行被注释），直发 RTP | CC 预算按 32kbps/流预留 | `MR/src/transport/channel/audio_channel_send.cpp:60-79` |
| 接收 | **完全无 jitter buffer/neteq**：RTP→直接同步 Opus 解码；PLC 仅对单包洞；播放端 SDL `SDL_OpenAudioDevice` 默认缓冲（**未指定 samples，SDL 默认通常 4096 样本≈85ms@48k 级**——SDL3 流式排队，实际排队深度无上限控制，积压即延迟） | 解码缓冲上限 120ms | `MR/src/transport/channel/rtp_audio_receiver.cpp:64-68`、`MR/src/media/audio/decode/audio_decoder.cpp:53-80`、`CD/.../session_device_manager.cpp:625-720` |

### 1.5 输入路径（控制端 → 被控端）

| 阶段 | 实际行为 | 关键参数 | 证据 |
|---|---|---|---|
| 采集（键盘） | macOS `CGEventTap`（session 级、head-insert、**吞掉本地按键**），keyDown/keyUp/flagsChanged；未判 autorepeat（OS 重复也逐条转发） | 注入事件带 `kCrossDeskInjectedEvent` 放行 | `CD/apps/desktop/src/platform/macos/input/keyboard/keyboard_capturer.cpp:105-227` |
| 采集（鼠标） | Slint `TouchArea.pointer-event`/`scroll-event`，**无相对鼠标/无 pointer lock/无鼠标 event tap**；坐标=视频区归一化 float | 灵敏度增益 `PointerMotion`（非相对模式） | `CD/.../stream_window.slint:817-865`、`gui_application.cpp:5385-5469`、`CD/.../pointer_motion.h:19-32` |
| 线格式 | JSON：`mouse{x,y,s,flag}`（绝对归一化坐标）、`keyboard{key_value=Windows VK,scan_code,extended,flag}`；无时间戳、无相对量字段 | — | `CD/libs/wire/include/remote_action.h:41-82`、`CD/libs/wire/src/remote_action.cpp:238-262` |
| 通道 | **move 走不可靠裸 RTP `"data"` 流**（不等待 KCP 重传）；点击/滚轮/按键走可靠流 `"mouse"`/`"keyboard"` = **KCP over RTP**（`nodelay(1,10,2,1)`、10ms update、wnd 256） | 无客户端限速/合批；`RtpDataSender` 5ms 线程每轮最多 10 包 | `CD/.../gui_runtime.cpp:134-143`、`CD/.../connection_runtime.cpp:734-741`、`MR/src/transport/channel/data_channel_send.cpp:130-150`、`MR/src/transport/channel/rtp_data_sender.cpp:16,133-143` |
| 被控侧队列 | `RemoteInputDispatcher` 单 worker 串行化：连续 move 合并最新一条（coalesced）、每控制器上限 128 条、断连清理重试 | — | `CD/.../remote_input_dispatcher.h:29-57,103-121`、`remote_input_dispatcher.cpp:47-57` |
| 注入 | `CGEventPost(kCGHIDEventTap)` 绝对坐标 `kCGEventMouseMoved`/dragged/按键；滚轮 `kCGScrollEventUnitLine`（**行单位，非像素**）；双击 500ms/8px 判定 | **无相对移动注入**（无 deltaX/deltaY 字段使用） | `CD/apps/desktop/src/platform/macos/input/mouse/mouse_controller.cpp:150-257` |
| 卡键保护 | 键盘状态心跳 500ms、超时 2500ms、`ReconcileKeyboardState` 按差异纠正；失焦/断连 `ForceReleasePressedKeys` | — | `CD/.../keyboard_controller.cpp:31-32,140-168,344-453` |
| 光标 | 线上只传**语义形状枚举**（28 值）+位置/可见性/embedded 模式；客户端用系统光标 overlay 绘制；macOS 采集默认不内嵌光标（`showsCursor` 仅 web 控制器开） | — | `CD/libs/wire/include/remote_cursor_shape.h:14-45`、`CD/.../cursor_presentation.h:22-96`、`screen_capturer_sck_impl.mm:652-666,708` |
| 手柄 | **完全缺失**：wire 无类型、SDL 只 INIT_VIDEO\|AUDIO、无采集/注入代码 | — | `CD/.../gui_application.cpp:1411`、`CD/libs/wire/include/remote_action.h:20-39` |

---

## 2. 对标差距表

| 对标能力 | 商业产品表现（来源） | CrossDesk 现状（证据） | 差距 | 改造方案（涉及文件） | 规模 | 风险 | 本机可验证方式 |
|---|---|---|---|---|---|---|---|
| 设备码+验证码连接 | 9 位 ID 3+6 分组 + **8 位字母数字临时验证码**、周期自动刷新、眼睛/铅笔按钮、智能粘贴、免密/双重验证（S9 截图 OCR） | 9 位 ID（3-3-3 分组）+ **6 位**密码、仅手动刷新、无自动轮换、无验证方式选择（`docs/CONNECTIONS.md:26-42`、`lan_identity.cpp:366-370`） | 验证码长度/刷新策略/验证方式三选一的体验差；但核心模型同构 | 密码策略配置化+周期刷新计时器+UI 眼睛/铅笔（`config_center.cpp`、`gui/ui/main_window.slint`、`device_login.cpp`）；免密确认属 L | M | 与官方服务器密码规则耦合（服务端生成 6 位）——**改 8 位需自托管或服务器配合**；自动刷新可纯本地 | 双实例互连（见 §4）观察刷新后重连 |
| 首帧/连接耗时 | 官方口径内网 16-21ms 延迟、连接"秒开"（S12，未证实具体连接耗时） | 无线程级计时埋点（absent）；建连链：WSS 登录→在线探测 5s 超时→join→offer/answer→ICE gather→DTLS→SRTP→`FullIntraRequest`→首帧，串行往返多（`connection_runtime.cpp:270-310,620-787`、`ice_agent.cpp`） | 无测量、无优化基线；LAN 直连可跳过信令 | 先加埋点（见 §4），再优化：offer 在 join 前预建、ICE gathering 与 DTLS 并行度、LAN 直连首帧路径 | M | 信令时序改动影响兼容 | 环回/LAN 计时日志对比 |
| 端到端延迟 | 局域网 5-15ms（评测 R1）；官方 15-30ms 公网（S12） | 全链路无 jitter buffer（好），但：采集 60Hz 节拍≤16.7ms、编码队列串行、pacer burst 40ms（direct）、vsync+`maximumDrawableCount=2` 最多 2 帧渲染排队；音频 SDL 默认大缓冲无 jitter 控制 | 结构性余量：pacer burst 40ms 与 144fps 目标矛盾；渲染 vsync 排队 | 见 §3 第 2/3 项 | — | — | §4 测量方案 |
| 帧率上限 120/144/240 | 宣称最高 144 帧、1080P 120 帧模式（S1/S2/S12）；档位 30/60/90/144（评测 R2，未证实官方） | **硬上限 60**：归一化非 30 即 60（`peer_connection.cpp:72-74`）、wire 校验 `{30,60}`（`preview_subscription.h:97`）、VT `SetTargetFrameRate` 拒绝 >60（`video_toolbox_encoder.mm:750`）、采集恒 60（`session_device_manager.cpp:190`） | 全链路硬伤：wire 协议、归一化、采集、编码、节奏闸层层 60 | wire `frame_rate` 放开校验+版本协商；minirtc 归一化泛化；采集 fps 提升；pacer/节奏常数复查（§3 第 1 项） | L | **wire 兼容**：旧端 Parse 拒绝非 30/60 → 需能力位协商降级；VT 高帧率编码压力 | 120Hz 本机面板可测 120fps（144 只能验证可配置，见 `ENV.md` §2） |
| 码率与画质档位 | 自动/原画 20M/超清 8M/高清 2M/**自定义滑块至 500Mbps**（S2/S3、评测 R2/R3） | quality 仅低/中/高三档（分辨率上限 720p/1080p/4K），码率模型 `coeff×pixels`（1080p60≈7.9M、4K60≈31.7M min），CC max 80Mbps、起始 2.5M；`max_bitrate_bps` 订阅字段已存在（≤100M） | 无"原画/自定义"概念；80Mbps CC 上限 < 500M；起始爬升慢影响开局画质 | 档位文案映射+自定义码率滑块写 `max_bitrate_bps`；CC max/起始码率上调（LAN）（`gallery_stream_settings.h`、`gallery_settings.slint`、`congestion_control.cpp`、`bitrate_limits.h`） | M | 高码率下 pacer/编码器背压连锁；>100M 需改 wire 校验上限（`preview_subscription.h:119`） | LAN 环回灌满测量实际码率/fps/延迟 |
| 色彩 4:4:4/HDR | 真彩 4:4:4（有条件，macOS 部分支持）；HDR 官方博客称支持（S4/S20/S14） | 全链路 NV12 4:2:0 8bit：采集（`sck_impl.mm:705`）、编码输入校验（`video_toolbox_encoder.mm:527-530`）、渲染 shader；无 HDR 元数据 | 采集像素格式、编码 profile（HEVC Main444）、线格式色彩协商、渲染色彩转换全缺 | HEVC Main444 编码+444 采集+协商位+渲染（本机已验证 VT 可编 444 8/10bit，见 `ENV.md` §4.3） | L | 编解码双端必须同配；带宽 ×~2；**本机验证过编码但未验证码流 chroma_format_idc** | VT 编码会话实测+解码回读 chroma |
| 编解码器选择 | H.265 为主（官方游戏页）+H.264；AV1 未证实（S19/S20） | 默认 H264；H265 仅在对端 prefer 或手动设置时协商；AV1 有软编解但 macOS VT 无 AV1 硬编（`ENV.md` §4.1） | 对标产品默认 HEVC 高压缩比；CrossDesk 默认 H264 | macOS 默认改 HEVC（`config_center.cpp:137-139` 默认值、`minirtc_connection.cpp:22-30`） | S | 旧端无 HEVC 时回退已被 SDP 协商覆盖（offer 恒含 H264） | 双端互连看 stats 面板 encoder 字段 |
| 游戏键鼠 | **沉浸式鼠标（相对位移）**、组合键透传、减少系统拦截（S2/S8） | 无相对鼠标/pointer lock；绝对归一化坐标；macOS 端键鼠 event tap 吞键已有 | FPS 类游戏完全不可用（视角转动依赖相对位移） | wire 加相对 move 类型+客户端 CGEvent 相对采集（`CGEventGetIntegerValueField(kCGMouseEventDeltaX)`）+host delta 注入+光标锁定 UI（`remote_action.h/.cpp`、`stream_window.slint`、`gui_application.cpp`、`mouse_controller.cpp`） | L | wire 新类型旧端忽略 → 需能力位；macOS 光标隐藏/锁定体验 | 本机 loopback 验证相对位移注入（实测光标不撞墙） |
| 手柄 | 最多 4 手柄+震动+虚拟手柄+云键位 400+（S2/S10） | **完全缺失** | 空白项 | 客户端 SDL_GameController 采集+wire 类型+host 虚拟手柄（macOS 无内核态虚拟手柄方案——需用户态驱动或 GameController 框架注入，**macOS 上可行性存疑**） | XL | macOS 无 ViGEm 等价物；**很可能需要 Windows 才有意义** | 暂缓（见 §5） |
| 光标渲染 | 未证实细节 | 语义形状 overlay+可选内嵌；macOS 默认不内嵌 | 基本对齐，差距小 | 低优先级 | — | — | — |
| 音频延迟 | 未证实具体值 | Opus 10ms VOIP 无 FEC；接收**零 jitter buffer** + SDL 默认大缓冲排队无界 | 声音滞后画面（无 AV sync、无缓冲上限） | 接收侧加有界 jitter buffer（30-60ms）+SDL 流排队上限+音画同步（`rtp_audio_receiver.cpp`、`audio_decoder.cpp`、`session_device_manager.cpp:692-720`） | M | PLC 简单；改缓冲影响丢包容忍 | 环回拍掌/敲击测试音画差 |
| 多显示器/虚拟显示器/分辨率匹配 | 5 屏切换+3 虚拟屏+会话内"跟随被控端/跟随当前显示屏/缩放 100-200%"（S3/S7 截图 OCR） | 切屏有（非并发）；客户端可请求改被控分辨率（10s 协商）+断连恢复；macOS **无虚拟屏**；缩放只有窗口内 fit | 虚拟屏缺失；会话内缩放档位不如 商业产品直观；无多屏并发 | macOS 虚拟屏（CGVirtualDisplay，macOS 14.1+ 私有 API 公开化程度存疑）属 L；缩放入口 UI 调整属 S | M/L | CGVirtualDisplay 在 macOS 27 的可用性未证实 | 分辨率匹配已可双实例验证 |
| 会话工具栏+实时统计 | 控制中心 9 项一屏可达；会话计时；ms/loss 排障（S7 截图 OCR、S2 FAQ） | 工具栏 10 项+统计面板（FPS/Res/Video latency/RTT/loss/编解码器/加密/端点），信息其实更全 | UI 信息结构差异>能力差异；Video latency 已是 capture→presented 近似 glass-to-glass | 主界面/工具栏重组属 UI 重塑（§3 不优先） | M | — | 已有面板可直接读数 |
| 主界面信息结构 | 设备清单优先+大按钮"进入桌面"+快速启动游戏宫格（S7 截图 OCR） | ID 输入框优先+近期连接卡片+桌面墙多设备预览 | 形态差异：CrossDesk 无账号体系，桌面墙是差异化优势 | 不照搬；保留桌面墙，强化"一键进入"路径 | M | — | — |
| P2P 成功率/中继 | 自研 UDP 私有协议、200-300 节点专网中转（S1/S2/S13） | libnice ICE+TURN+DTLS 后打洞（1.5s 延迟触发、30s relay-upgrade 窗口） | 打洞启动晚、relay 首段必走中转；专网规模无法比 | 打洞提前到 ICE connected 即启动；relay 探测策略调优 | M | NAT 行为不可控，本机难验证真实 NAT 场景 | 本机只能验证 LAN/host-only 路径 |
| 安全模型 | ISO27001/27701、DTLS+AES 表述、免密+双重验证、客户端安全锁（S1/S3/S16） | Ed25519 设备密钥+挑战签名登录、SRTP AES-128-GCM（ID 模式对旧端有明文兼容分支！）、LAN Argon2id+限流、可信设备名单；**但本机 ID+密码明文落盘 `client_id_with_password`**（`credential_cache.h:18,32-33`）、近期连接密码 AES key/iv 同文件（`thumbnail.cpp:75-88`） | 明文落盘是硬伤；免密/双重验证缺失；ID 模式明文兼容分支 | 钥匙串迁移（macOS Keychain）属 M；明文兼容分支可视对端版本关闭 | M | 钥匙串迁移的升级兼容 | 静态检查+钥匙串条目验证 |

---

## 3. 首批改造路线（性价比最高、单机可验证）

> 排序依据：体验增益/ effort 比 × 单机可验证性 × 不依赖服务器/第二台机器/Windows。
> **wire 兼容性总原则**：用户其他端点全是 stock CrossDesk → 任何 wire 变更必须让旧端"无能力位则按旧行为"，新端间才启用新行为。

---

### 3.0 落地状态（2026-11-26，round-5 复测后）

下表区分**已落地**（改造分支已提交、单机 harness 复测绿）与**仍待做**。除标注外测量机为本机（M 系，LAN `192.168.1.107`），4K 指 3024×1964。每条命令中的 harness 二进制为 `build/macosx/arm64/release/frame_rate_harness`。

#### 已落地

| 改造 | 状态 | 关键提交 |
|---|---|---|
| 1a. 解开 fps 档位 30/60/90/120/144 + `supports_high_fps` 能力位 | ✅ 落地 | 子模块 `0ca83dc`，超级项目 `c167344`/`55c2590` |
| 2. macOS 默认 HEVC | ✅ 落地 | 超级项目 `0cc7778` |
| 编码 in-flight 准入闸（延迟核心） | ✅ 落地→**round-3 重写为防泄漏** | 子模块 `a865a82`→`21d881c` |
| 64 位码率溢出修复 | ✅ 落地 | 子模块 `a2c55db` |
| 高帧率 harness + 精确码率测量 | ✅ 落地 | 超级项目 `a9c0c18`/`082fcdb`/`4e2a23a` |
| **round-3-1 in-flight 计数移入编码器** | ✅ 落地 | 子模块 `21d881c`，超级项目 `f194b45` |
| **round-3-2 降级偏好=fps 纯函数** | ✅ 落地 | 超级项目 `78fbff4` |
| **round-3-3 IOSurface 零拷贝 harness 模式** | ✅ 落地 | 超级项目 `7aef49c` |
| **round-4-1 in-flight 行为测试替换文本匹配** | ✅ 落地 | 超级项目 `490354a` |
| **round-4-2 丢帧归因计数 + in-flight 闸根因测量** | ✅ 落地（测量+归因；**round-4 的"硬件上限"结论已被 round-5 推翻，round-5 的"高度%16 对齐"又被 round-6 推翻**，真根因为每帧固定编码成本） | 子模块 `3b77c76`，超级项目 `966ad7d` |
| **round-4-3 macOS GPU CVPixelBuffer 缩放器** | ✅ 落地（round-5 已证颜色逐位正确） | 子模块 `ddeaf88`/`3b77c76` |
| **round-4-4 编码吞吐驱动分辨率（消振荡）+ 码率上限传播 + 阶梯修复** | ✅ 落地（round-6 改为纯偶数阶梯 + rung 粒度 ceiling 退避重探） | 子模块 `c89d1ff`/`3b77c76` |
| **round-5-1 对齐分辨率阶梯（高度%16 根因利用）** | ⚠️ **已被 round-6 推翻并移除**（高度%16 机制不存在；保留中间档改纯偶数尺寸） | 子模块 `f111b35`（移除于 `f8d4d82`） |
| **round-6-1 高度%16 机制受控 A/B 推翻 + 对齐网格移除（纯偶数阶梯）** | ✅ 落地 | 子模块 `f8d4d82` |
| **round-6-2 ceiling 改 rung 粒度（消 4K@144 锯齿振荡）** | ✅ 落地 | 子模块 `da485ec` |
| **round-5-2 编码吞吐 ceiling 指数退避重探（非永久）** | ✅ 落地 | 子模块 `e86c201`，超级项目 `a5fdabd` |
| **round-5-3 GPU 缩放器颜色正确性验证** | ✅ 落地（逐位 Δ=0，无需改动） | 测试工件 `/tmp/gpu-scale-test/`（不入库） |
| **round-5-4 周期性 fps 跌落根因 + p5/worst-second 指标** | ✅ 落地 | 超级项目 `b95d35b` |
| **round-5-5 子模块许可证 LGPL-3.0-only** | ✅ 落地 | 子模块 `15d0e96`/`e86c201` |
| **round-5-6 ASan+UBSan/TSan 缩放器+编码器压测** | ✅ 落地（0 报告） | 压测 `/tmp/asan_stress/`（不入库） |
| **round-8-1 直连初始分辨率快速恢复（首秒画质核心）** | ✅ 落地（`initial_fast_recovery` 不再限 `balanced_screen_content`；10s 内全配置达原生，修复前无一达原生） | 子模块 `74ce077`，超级项目 `b968730` |
| **round-8-2 harness 爬坡测量计数 + 用户态 token-bucket 限速器** | ✅ 落地（per-100ms ramp100 桶、`--start-bitrate` 还原生产起始码率、`UdpShaper` 纯 harness 侧候选端口重写注入，零 minirtc 产品改动） | 超级项目 `3f36cc0` |
| **round-9-1 多档直跳失败恢复（上限交互回归修复）** | ✅ 落地（回滚=跳前基档，ceiling 基准=失败档之下最高梯档；恢复在区间内逐档以快节奏爬升。修复后 4K@120 t15-60 中位面积 2.42M vs parent 1.45M） | 子模块 `d1effcd`，超级项目 `a2e22ce`/`36c4150` |
| **round-9-3 探测前持续健康冗余去除（初始快速恢复）** | ✅ 落地（带宽已确认路径去掉 healthy_since 老化；time-to-native n=5 交错 1080p60 前3.72/3.73/3.78→后3.22/3.27/3.29、4K@60 前3.73/3.78/4.31→后3.24/3.27/3.30，min/med/max 区间不重叠，改进成立） | 子模块 `65942f7`，超级项目 `bca489a` |

#### 仍待做

| 改造 | 阻塞/原因 |
|---|---|
| 3. 音频延迟修复（jitter buffer + AV 同步） | 未开始；本轮明确不碰 `rtp_audio_receiver.cpp`/`audio_decoder.cpp` |
| 4. 连接耗时埋点 + LAN 首帧优化 | 未开始 |
| 5. 相对鼠标游戏模式 | 未开始；本轮明确不碰鼠标输入 |

#### round-3-1：in-flight 准入闸改为防泄漏（已落地）

**问题**：旧设计把 in-flight 计数放在 transport 层，依赖"每个编码器对每帧恰好回调一次"。这只对 VideoToolbox 成立——openh264 码控跳帧、同步软件编码、未来 NVENC/AV1 都可能"返回成功却不回调"，每帧泄漏一个槽位，两道泄漏后准入闸永久拒绝新帧 → 黑屏。

**修复**（按用户要求）：计数移入编码器。`media_codec.h:209` 新增 `virtual int FramesInFlight() const { return 0; }`——同步编码器 Encode() 返回前已了结该帧，永无 in-flight，默认 0 正确且**永不被闸**（行为与改造前完全一致）。VideoToolbox 用待回调注册表大小做计数（`video_toolbox_encoder.mm:130` 声明、`:687` 注册时 +1、`:706/:721/:732` 输出/取消/析构各 -1），每次注册恰好被消费一次，无论帧如何了结都归零。transport 只读不增减（`ice_transport_controller.cpp:924` `codec->FramesInFlight()`），跨组件不变式整体消除。

**grep 证明**：`frames_in_flight_` 仅在 `video_toolbox_encoder.mm` 内被 `fetch_add`/`fetch_sub`；controller 只在 `:924` 读取。

**并发回答**：`SendVideoFrame` 在 `ice_transport_controller.cpp:833` 持 `stream_senders_mutex_` unique_lock 贯穿准入/拷贝/PostTask，且 SCK 每流串行回调——同一流不会并发进入准入临界区。即便并发，计数在编码器内自我校正，worst case 瞬时多进一帧（略升延迟），永不锁死。`context->codec` 在准入时于同一互斥锁下拷贝 shared_ptr 保活，`FramesInFlight()` 是无锁原子读，采集线程调用安全。

**复测**（`--width 3024 --height 1964 --fps 120 --codec h265 --seconds 8 --address 192.168.1.107`）：delivered_fps=64.5、p50=39ms、p95=44ms——与 round-2 基线一致，重写无回归。1080p@144（`--width 1920 --height 1080 --fps 144 --codec h264`）：143.9fps、p50=13ms、dropped=0。防泄漏契约有结构测试 `encode_in_flight_contract_test`（绿）。

#### round-3-2：降级偏好=协商 fps 纯函数（已落地）

**问题**：4K@120 若 MaintainResolution，硬编只能出 64fps（卡）；若 MaintainFrameRate，降到半分辨率出 90+fps（流畅）。但偏好此前到处硬编码 MaintainResolution。

**修复**：新增纯函数 `crossdesk::DegradationPreferenceForFrameRate`（`libs/wire/include/video_frame_rate.h:76`）：fps>60→MaintainFrameRate，否则→MaintainResolution。单一决策点，用于 4 处：bootstrap `gui_runtime.cpp:342`、`lan_runtime.cpp:61`，以及重协商两个入口 `peer_data_callbacks.cpp:235`（deskhub_preview）、`:367`（video_settings）。偏好只能经 `MiniRtcUpdateVideoSettings` 改（无独立 setter），两个重协商入口都重导出偏好，故 60→120→60 全程跟随。

**多viewer 语义**：每 viewer 一个 `IceTransportController`（`UpdateVideoSettings` 按 `remote_id` 寻址），encoder 不跨 viewer 共享——120fps 与 60fps 两 viewer 可并存、各自独立降级。同一 viewer 的多显示器共享一个 `ResolutionAdapter`+偏好。

**阶梯分析（关键发现）**：分辨率阶梯**不是**几何 0.88 序列，是整数-k 严格宽高比量化。3024×1964 gcd=4→单元 756×491，`k_step=2`（491 奇）。合法档只有 k=4（3024×1964）与 k=2（1512×982）；k=3 因 1473 为奇非法。`GetNextLowerResolution` 一步 0.88² 面积仍四舍五入回 k=4 → 返 `{-1,-1}`，**阶梯在 4K 完全无法逐级降**。到 1512×982 只能靠码率映射 `GetResolution`（`resolution_adapter.cpp:279`）直接跳，或快速恢复路径。含义：4K 下降级实为"原生↔半分辨率"二态，无中间档。

**复测**（h264/h265 4K@120 `--preference maintain_frame_rate`）：降半分辨率 1512×982、delivered 90-94fps、p50 20-22ms；对照 `--preference maintain_resolution` 原生 3024×1964 仅 64fps、p50 40ms。命令例：`--width 3024 --height 1964 --fps 120 --codec h265 --seconds 6 --address 192.168.1.107 --preference maintain_frame_rate`。

#### round-3-3：4K 采集+拷贝上限是真实还是 harness 假象（已落地测量）

**结论：吞吐上限真实（编码器瓶颈），拷贝只是无缩放路径的轻微假象。** 新增 `--native` 模式复刻生产零拷贝路径（IOSurface-backed CVPixelBuffer 池，retain/release/copy_to_nv12 回调与 `screen_capturer_sck_impl.mm:966-975` 完全一致），对比 CPU memcpy 路径：

| 配置 | CPU harness | `--native` 零拷贝 | 判定 |
|---|---|---|---|
| 4K@144 MaintainResolution（无缩放） | 64.3fps / p50 40ms | 64.8fps / p50 39ms | 几乎一致 → 上限非拷贝 |
| 4K@120 MaintainFrameRate（缩半分辨率） | 94.3fps / p50 20ms | 94.3fps / p50 20ms | 完全一致 → 缩放路径生产也拷 |
| 4K@60 MaintainResolution（无缩放） | 59.9fps / p50 33ms | 60.0fps / p50 31ms | 60fps 下拷贝成本被隐藏 |

**拷贝记账**（4K NV12 一帧 8.91MB；半分辨率 2.23MB）：无缩放时生产 0 次全帧拷贝（采集 retain→transport `RawFrame` 引用→编码器 retain，`video_toolbox_encoder.mm:564`），CPU harness 却有 2 次（准入 malloc+memcpy + VT 上传）——**故无缩放路径 harness 偏悲观，是假象**；但实测吞吐仍一致，因为编码器本身就是瓶颈，拷贝再快也喂不饱它。缩放时**生产同样拷**：macOS 无原生 CVPixelBuffer 缩放器——`TryScaleNativeFrame` 仅支持 Windows D3D11（`raw_frame.h:52-66` 对其他类型返回 false），故高帧率 MaintainFrameRate 降分辨率时生产走 `MaterializeNativeFrame` 拷贝 4K→CPU（8.91MB）+ libyuv `ScaleNv12ViaI420`（写 2.23MB）+ `UpdateBuffer` 再拷 2.23MB + VT 上传 2.23MB ≈ 15.6MB/帧，与 CPU harness 完全相同——**故缩放路径上限真实**。零拷贝对吞吐影响实测可忽略（CPU 与 native 同 fps 同 ms），证明 4K HEVC 约 64fps（≈20ms/帧）上限是 **VideoToolbox 硬编吞吐上限**（≈390 Mpx/s ÷ 5.94 Mpx ≈ 65fps），不是采集/拷贝瓶颈。注意：item-2 让高帧率默认 MaintainFrameRate（即缩放路径），故生产相关路径本就拷贝；真正的零拷贝收益只在 MaintainResolution 无缩放时，而那里编码器已是瓶颈。

命令例：`--width 3024 --height 1964 --fps 144 --codec h265 --seconds 6 --address 192.168.1.107 --preference maintain_resolution [--native]`。

#### round-4-1：in-flight 行为测试替换文本匹配（已落地）

**问题**：`encode_in_flight_contract_test` 只对编码器源码做文本匹配，是结构断言而非行为证明。**修复**：删除之，新增 `video_toolbox_in_flight_test`（`apps/desktop/tests/video_toolbox_in_flight_test.cpp`）驱动真实 `VideoToolboxEncoder` 行为验证防泄漏：排空归零、待处理时为正、带帧析构、分辨率切换取消路径、逐帧恰好一次回调、同步吞咽编码器永不被闸。5 子项全绿（重复运行 rc=0）。目标注册 `apps/desktop/xmake/targets.lua`（macOS 守护，需 VideoToolbox/CoreMedia/CoreVideo/IOSurface/Foundation + minirtc 内部 include）。提交 `490354a`。

#### round-4-2：丢帧归因计数 + in-flight 闸根因（已落地测量；**round-4 的硬件上限结论已被 round-5 推翻**）

**问题**：1512×982 编码比 1920×1080 慢（像素更少却更慢）。**根因测量**（新增 per-cause 计数，`api/minirtc.h` 的 `MiniRtcNetTrafficStats` 增加 `video_capture_input_frames`/`video_rate_dropped_frames`/`video_cwnd_dropped_frames`/`video_pacer_rejected_frames`/`video_encode_queue_dropped_frames` + pending/in-flight 细分，harness 最终 JSON 打印）：丢帧 **100% 是编码准入闸**（`ice_transport_controller.cpp` in-flight 分支），码率 frame-dropper/CWND/pacer 全为 0。`--width 1512 --height 982 --fps 144 --codec h265 --native --preference maintain_resolution`：108fps、178 丢、in_flight 174/pending 4、rate 0/pacer 0；对照 1920×1080：143.9fps、0 丢。

**硬件上限证明**（**⚠️ 此"硬件上限"结论先被 round-5 推翻（改称高度%16对齐），round-5 的对齐说又被 round-6 受控 A/B 推翻**，见 round-6-1：真根因是**每帧固定编码成本 + 每像素成本**（`frame_seconds ≈ a + b·Mpx`），非硬件上限、亦非对齐——半分辨率 108fps 实为该尺寸像素吞吐 + 解码并发竞争媒体引擎所致，与高度%16 无关）：`encode_throughput_probe`（直驱 VT、无 transport、无解码）单跑 1512×982@144 输出间隔 ~6ms（~167fps）很快；**同机有解码活动的 harness 会话并发时**同探针掉到 ~9ms（~111fps），正好等于 transport 内的 ~108fps 上限。即 **VideoToolbox 媒体引擎在编解码头分时复用下，编码吞吐随像素量下降**。已排除：码率（5.5Mbps 仍卡）、输入格式、关键帧间隔、热/竞争（安静窗测量）。**in-flight 上限提升已测并回退**：cap 2→4/6 后 delivered fps 不变（VT 吞吐才是瓶颈），反压只是从 in-flight 迁移到阻塞的 `VTCompressionSessionEncodeFrame`，延迟反涨 25-50ms——故保持 `kMaxEncodeFramesInFlight=2`（latest-frame-wins 正确反压），评估记录于 `ice_transport_controller.cpp` 注释。

#### round-4-3：macOS GPU CVPixelBuffer 缩放器（已落地）

**修复 round-3-3 留下的"macOS 无原生缩放器"**：新增 `src/media/video/scale/cvpixelbuffer_scaler.{h,mm}`——`VTPixelTransferSession` 把采集帧在 GPU 上缩放到 encode 目标尺寸的池化 IOSurface NV12 CVPixelBuffer，零拷贝喂编码器（与 SCK 采集同路径），替换原 materialize→CPU(8.91MB)→libyuv 缩放→重上传（~11ms/帧）。集成于 `ice_transport_controller.cpp` 缩放分支（CVPixelBuffer 帧优先 GPU，失败回退 CPU 路径）；session+pool 按目标分辨率重建、构造函数内建（避免 encode 多 worker 懒建竞态）。**实测** `--width 3024 --height 1964 --fps 120 --codec h265 --native --preference maintain_frame_rate`：基线 91fps/p50 21ms → **GPU 缩放后 110.8fps、p50 17ms**（编码 1512×982）；对照 4K@60 MaintainResolution 不变（59.9fps、0 丢、编码 3024×1964、p50 31ms）。缩放本身吞吐开销归零（缩放路径 fps 反超无缩放对照）；残留 p50 17ms 中 ~3.4ms 是同步 transfer 成本。**注**：acceptance ≥115fps@120 未达系 round-4-2 的编解码并发硬件上限（108-110fps），非缩放开销。

#### round-4-4：编码吞吐驱动分辨率 + 码率上限传播 + 阶梯修复（已落地）

三个 bug。**(1) 拥塞控制 20M 硬顶**：`UpdateVideoBitrateLimit` 现经 `CongestionControl::SetMaxTargetBitrate`（`congestion_control.cpp`）+ `probe_controller->SetMaxBitrate` 把预算传入估计上限，LAN 可爬向真实容量（至 100M）；默认会话仍 20M 不变。**(2) 阶梯量化堵死升降**：strict-aspect 阶梯对 3024×1964 只有 k=4/k=2 两档，`GetNextHigher/LowerResolution` 目标面积四舍五入回当前档 → 升降都无法触发；修复 `resolution_adapter.cpp` 强制严格更低/更高档（`current_area ± 1`）。**(3) 核心振荡**：升级探测对编码吞吐无记忆——带宽充足时反复爬到 3024×1964、硬编只有 ~54-64fps、回滚、退避后再爬（60s 内 6+ 次切换）。修复：`ice_transport_controller` 记住探测失败/编码饱和降级测得的**per-session 编码吞吐 ceiling**，跳过目标面积 ≥ ceiling 的升级探测（同宽高比阶梯单调）；源几何/编码器重置时失效。**实测** 60s 4K@120 MFR `--max-bitrate 100000000`：仅 t≈4s 一次探测上 3024×1964（fps 塌到 64）、t≈6s 回滚，之后 **0 次再探测**，1512×982 稳定 ~100-108fps——**t=5s 后分辨率切换=2（一次上下探测对，无振荡）**；对照 4K@60 MFR 正控可成功爬升 3024×1964 并稳定（61fps ≥ 55 阈值），证明不会过度封锁。harness 新增 `--max-bitrate` + 每秒 stderr `traj t=Ns enc=WxH fps=N` 轨迹。提交 `c89d1ff`/`3b77c76`。

#### round-5-1：对齐分辨率阶梯（**⚠️ 高度%16 机制已被 round-6 受控 A/B 推翻**，对齐网格已移除，见 round-6-1；保留中间档但改纯偶数尺寸）

**问题重审（推翻 round-4-2"硬件上限"）**：round-5 用 `encode_throughput_probe` 扩展的**饱和间隔法**（`saturated_gap_us`，采集证明编码队列饱和的相邻帧完成间隔，把编码服务时间从提交节奏里隔离）+ 真实运动内容 + 解码并发，对一批尺寸扫描编码吞吐（Mpx/s 中位），发现根因**不是硬件上限也不是像素数**，而是 **VideoToolbox HEVC 在 Apple Silicon 有快慢两条编码路径，按 `height % 16 == 0` 划分**：对齐高度比非对齐快 1.5-2.5×/像素，与宽度/宽高比/像素数无关。VT 会话配置逐项读回（新增 `DumpSessionConfiguration()`，`video_toolbox_encoder.mm`）显示快慢两例**配置零差异**（同 `HEVC_Main_AutoLevel`、realtime、expected_fps、encoder_id `com.apple.videotoolbox.videoencoder.hevc.rtvc`）——是**尺寸本身（对齐）**决定路径。1512×982 慢正是因为 982≡6 mod 16。解码并发全局再削 ~30-45%。扫描表（解码并发，中位 Mpx/s，★=h%16==0）：1280×720★(>129)、1600×900(172)、1536×960★(>176)、1512×982(173)、1728×1116(203)、1920×1080(214)、1920×1088★(>215)、1920×1200(186)、1920×1246(225)、2016×1310(201)、2560×1440(266)、2560×1664(289)、3024×1964(320)。

**修复**：`resolution_adapter.cpp` 新增 `BuildAlignedAspectResolution`(:88)——按目标面积选偶数 (w,h) 且 h%16==0、宽高比误差 ≤`kMaxAspectError`(:72, 1%，实测 ≤0.081%)、偏好面积最接近目标者；回退链 对齐→严格 gcd→仅偶数。接入 `GetResolution`(:419)/`GetNextLowerResolution`(:545)/`GetNextHigherResolution`(:590)/`GetStartupResolution`(:374)；`RefitEncodeAligned`(:180) 重拟合 transport 偶数钳制（`ice_transport_controller.cpp:1094-1105`）使之不再破坏对齐。3024×1964 的对齐阶梯档：2660×1728、2340×1520、2240×1456、2094×1360、2068×1344、1970×1280、1822×1184、1674×1088、1600×1040、1232×800。**档位可持续吞吐**：120fps 档最大可持续 ≈1970×1280（2.52Mpx，118-121fps）；144fps 档 ≈1674×1088（1.82Mpx，141-144fps）。**端到端实测**（MFR `--native`，安静窗）：4K@120 现交付 **119.5fps、编码 1970×1280、fps_p5=118、worst=117、p50=17ms**（对照 round-4 旧 108fps@1512×982）；4K@144 用 1674×1088/1822×1184 档达 ~137fps、p5=127。提交 `f111b35`。

#### round-5-2：编码吞吐 ceiling 指数退避重探（非永久，已落地）
**问题**：round-4-4 的 per-session 编码吞吐 ceiling **只在源几何/重配时失效**——一次瞬时节流（热、竞争）会把流永久钉在低分辨率。**修复**：新增 `src/media/resolution_adapter/encoder_throughput_ceiling.h`（round-6 已移至 `src/transport/encoder_throughput_ceiling.h`）（`EncoderThroughputCeiling` 状态机，`kReprobeBaseMs=30000`，阶梯 30s→60s→120s 封顶 `kReprobeMaxBackoffMs=120000`，重学保留阶梯）。门谓词在 `video_adaptation_policy.h`；controller 集成 `ice_transport_controller.cpp`：探测失败重学+`BackoffReprobe`、成功清除、重探门经 `DeferReprobe`、几何/重配重置。**失效规则**：ceiling 不再是永久——探测失败后按 30/60/120s 退避重探，成功即清，失败重学并重置退避起点。**⚠️ 成本纠正（round-6 实测，解决与 round-5-4 的 ≤1s vs ~5s 矛盾）**：单次失败探测的**真实降级成本 ≈5-6s，非 ≤1s**——探测窗口本身 ~5s（在 probe 目标尺寸的降级 fps，如 1284×832→1458×946 时 fps 143→134 持续 5s）+ 回滚后恢复 ~1s；round-5-2 原"≤1s"只算了回滚尾巴，漏算探测窗口。**且 round-6 发现退避实际不衰减**（re-probe 间隔恒 29s，因 ceiling 门面积严格 `>=` 被舍入滑动绕开，见 round-6-2 已修）。测试 `apps/desktop/tests/encoder_ceiling_reprobe_test.cpp` 6 项全绿。提交 `e86c201`（子模块）/`a5fdabd`（超级项目）。

#### round-5-3：GPU 缩放器颜色正确性验证（已落地，无需改动）

验证 round-4-3 的 `VTPixelTransferSession` 缩放颜色：**逐位正确，无需改动**。VT 自动把 BT.601 video-range attachments 传播到池化目标；补丁测试值 **Δ=0.0 逐位一致**。采集侧打 `colorMatrix=ITU_R_601_4`、`colorSpaceName=sRGB`（`screen_capturer_sck_impl.mm:705-707`）。`kVTPixelTransferPropertyKey_DestinationColorPrimaries/TransferFunction/YCbCrMatrix` **不设置**——输出已与输入逐位一致，设置只徒增 GPU 颜色匹配开销（Apple 头文件警告）。缩放质量：`kVTScalingMode_Normal` 为抗锯齿 area/linear 滤波；端到端文字锐度比 libyuv `kFilterBox` 软 ~6%（视觉边际，可接受权衡，GPU 无更锐模式）。测试工件 `/tmp/gpu-scale-test/`（不入库）。

#### round-5-4：周期性 fps 跌落根因 + p5/worst-second 指标（已落地）

**根因**：周期性跌到 44-80fps **不是** ALR/码率探测（这些零成本——2× ALR 突发期间 fps 平直）。跌落实为 (a) ceiling 重探（有意为之，退避限界，单次成本 ~5s——容量恢复固有问题）与 (b) 边界阶梯尺寸的边缘可持续振荡。harness 现报 `fps_p5`/`fps_worst_second`（`frame_rate_harness.cpp:575-579,621-633,645,659`）。**注**：生产 BenchmarkRemote 常驻 ~22% CPU 下测量方差大，4K@120 在安静窗稳定 119.5fps@1970×1280 但竞争下会振荡降级（1624×1056 等中间档）——此为边缘可持续性的已知表现，非回归。

#### round-5-5/5-6：许可证 + 消毒器（已落地）

**许可证**：子模块为 LGPLv3（`deps/submodules/minirtc/LICENSE` = GNU LESSER GPL v3）。fork 新增的子模块文件仅 `cvpixelbuffer_scaler.h/.mm`（已 `LGPL-3.0-only`，`15d0e96`+超级项目 bump `9cd01c8`）与 `encoder_throughput_ceiling.h`（已 `LGPL-3.0-only`，`e86c201`）。超级项目文件保持 GPL-3.0-only（`encoder_ceiling_reprobe_test.cpp` 正确）。**消毒器**：ASan+UBSan 4/4 跑 0 报告；TSan 5/5 跑 0 警告；macOS leaks 0 字节；retain/release 精确平衡。无生产 bug、无接缝。压测 harness 在 `/tmp/asan_stress/`（`stress_asan`/`stress_tsan`/`stress_plain`，不入库）；仅发现的 bug 在压测 harness 自身（已修）。spdlog 为 header-only。

**round-5 回归修复**：round-5 把 transport 的 `src/media/resolution_adapter` include 目录改为 `{public=true}`（ceiling 头经 `ice_transport_controller.h`→`ice_transport.h`→`pc` 目标链需要），意外把 minirtc 的 `nv12_scaler.h` 泄漏进 GUI 编译，遮蔽了 crossdesk 自己的 `apps/desktop/src/common/nv12_scaler.h`（同名冲突）→ `slint_video_presenter.cpp`/`peer_media_callbacks.cpp` 里 `crossdesk::ScaleNv12ViaI420` 不再解析。修复：GUI 改以 `common/nv12_scaler.h` 包含（仅 crossdesk 树满足的路径），与 include 顺序无关。`crossdesk` GUI 目标恢复编译。提交 `b2bd4ba`。

#### round-6-1：高度%16 对齐机制被受控 A/B 推翻，对齐网格移除（已落地）

**推翻 round-5-1 的根因claim**：round-6 用扩展的 `encode_throughput_probe`（不饱和上限、闸控 2 in-flight、环形预生成帧，直报 `throughput_fps`/`throughput_mpx_s`）做**受控 A/B**：固定宽度，对每个三元组测对齐高（h%16==0）vs 非对齐高（h%16!=0），triple 内交错、多 rep、跨 solo/解码并发两模式。**结果：对齐高度从不更快**——解码并发下每个三元组平坦（差 <1fps，效应消失）；solo 下 1920×1080/3024×1964（均非对齐高、宽为 16 倍数）反而可复现地比其对齐邻居快 ~10-29fps（**与高度%16 方向相反**）。无任何三元组对齐更快。**结论：高度%16 快路径机制不存在，round-5-1 的扫描表把分辨率相关的吞吐差异误归因于对齐**。未解之谜（如实记录）：1920×1080/3024×1964 在 solo 可复现较快、解码并发下消失，疑为内存 stride/pitch 或驱动调优尺寸，但**不影响决策**（对齐从不更快，对齐网格反而选到更慢尺寸）。

**真根因（修正）**：小档能持续更高 fps 的真正原因是**每帧固定编码成本 + 每像素成本**，拟合 `frame_seconds ≈ a + b·Mpx`（solo 拟合 a≈0.76ms、b≈3.0ms/Mpx）。像素越少每帧越快，与对齐无关。叠加 round-4 原有的 strict-gcd 两档塌缩（k=2/k=4）。**修复**：移除 `kEncodeAlignH`/`kMaxAspectError`/`BuildAlignedAspectResolution`/`BuildStrictAspectResolution`/`RefitEncodeAligned`(+Impl) 及 controller 重拟合（`ice_transport_controller.cpp` 现直接用 `FitVideoResolution` 的偶数钳制结果）；4 处阶梯调用点（`GetResolution`/`GetNextLowerResolution`/`GetNextHigherResolution`/`GetStartupResolution`）统一用单个 `BuildEvenAspectResolution`——按目标面积 sqrt 缩放、最近偶数、宽高比尽可能精确（实测误差 <0.1%），**保留中间档**（round-5 的关键收益），严格升降档窗口逻辑不变。3024×1964 偶数阶梯档：2660×1728、2342×1520、2060×1338、1812×1178、1596×1036、1404×912、1236×802。子模块 `f8d4d82`。

#### round-6-2：ceiling 改 rung 粒度，消 4K@144 锯齿振荡（已落地）

**根因**：round-5-2 的 ceiling 门 `IsBlockedByEncoderThroughputCeiling` 用**严格面积 `>=`**。偶数阶梯对同一档有舍入抖动——重探刚失败的档会算得窄几像素（1456×946 vs 记忆的 1458×946），目标面积**恰低于 ceiling** → 完全绕开退避门 → 按普通升级冷却（非 ceiling 退避）重发 → **4K@144 出现不衰减的 29s 锯齿振荡**（实测 probe-up 间隔精确 29/29/29s，在 1284×832↔1458×946 间，远超 round-5 设计的 30→60→120s 退避）。**⚠️ round-6 此处对 ~5s 成本的解释已被 round-7-1 推翻**：当时以为 ~5s 是"探测窗口内降级 fps"，真相是测量从未启动、5s 全是 resolution-not-applied 超时（见 round-7-1）。**修复**：门改 rung 粒度 `target_area*8 >= ceiling_area*7`（阶梯步长 ~1.29×，低一整档 ~0.775×<0.875× 阈值，舍入变体正确归并为同档）→ 失败重探正确命中 ceiling → `BackoffReprobe` 真正升阶梯，振荡按 30→60→120s 退避衰减。`video_adaptation_policy.h`；ceiling 测试仍全绿。子模块 `da485ec`。

#### round-7-1：探测失败成本根因——测量从未启动（已修复，已落地）

**推翻 round-6-2 的成本解释**：round-6 以为 5s 是"探测窗口内降级 fps + 回滚"，**真相是测量根本没启动**。证据（harness stdout 的 controller 日志）：探测 `1974x1282 -> 2242x1456` 在 probe started 后 **精确 5.012s** 以 `reason=resolution_not_applied fps=0` 失败。**机制链（file:line）**：探测把 `probe_target` 存为阶梯 raw 值 2242×1456（`ice_transport_controller.cpp` probe 启动处）；每帧实际编码尺寸 = `FitVideoResolution(source, target)`（`:1092`），它重新推导 scale 并再取偶 → **2240×1456**（`video_resolution_limit.h:16-25`，宽差 2px）；而测量启动条件是 `EncodedWidth==probe_target_width && EncodedHeight==probe_target_height` **精确相等**（`:1744-1748`）——2240≠2242 永不匹配 → `measurement_started_ms` 永为 0 → 探测只能靠 `probe_start_timed_out`（`kUpgradeProbeInputWaitMs=5000`，`:1370`/`:1871`）收尾。**即每次失败探测的 ~5s 全是"等分辨率匹配不上而超时"，根本没测降级 fps**；且 ceiling 是从 `resolution_not_applied` 假象学的。round-6 我自己的"捕获样本积累慢"解释也错——MFR 下 `insufficient_capture` 恒 false（它要求 `balanced_screen_content`）。

**修复**（决策入纯策略 `video_adaptation_policy.h`）：`ProbeEncodedSize`/`IsProbeEncodedFrame`——测量改与 `FitVideoResolution(source, probe_target)` 的拟合尺寸匹配（构造性精确，因 SendVideo 用同一 fit）；`ProbeMeasuredFrameRate`；`IsProbeFrameRateConclusivelyLow` 用独立的**确诊失败窗** `kProbeFrameRateFailureWindowMs=500`。**重配/关键帧抖动的分离**：测量时钟只在编码器产出新尺寸首帧（关键帧）后启动，故重配+关键帧 stall 全在窗口之前、永不计入证据；窗内全是稳态 delta 帧。**成功标准未放松**——接受仍需完整测量窗（1000/2000ms），失败窗缩短不影响成功路径。**实测**（loadavg 12.86，判决延迟对负载鲁棒）：探测开始→回滚 **0.579s/0.576s**（apply ~70ms 负载无关 + 测量 ~505ms），原 ~5s。**副产验证**：ceiling 退避端到端升阶梯 30→60s（probe-up 间隔 30.6s，二次失败后 `next_reprobe_backoff_ms=60000`），坐实 round-6 rung 门修复。单测 `TestProbeVerdictPolicy`（确诊早失败/重配抖不失败/边缘跑满窗）。子模块 `7db702a`，超级项目 `3b3aa36`+`5d11e8e`。

#### round-7-2：ceiling 门改"最大安全基准面积"——固定比例无法兼顾舍入变体与压缩步（已修复，已落地）

**根因**：round-6-2 的门 `target_area*8 >= ceiling_area*7` 假设阶梯**处处 ~1.29× 均匀步长**。round-7 用真实 `ResolutionAdapter` 写阶梯性质测试（7 源 × 3 画质档，336 对相邻档），暴露**该假设在两类钳制步上破裂**，且**任何固定比例都无法同时正确分桶舍入变体（~0.998×）与压缩相邻档**：
- **floor-clamped-step（9 例）**：码率下限 floor 把最小档钳在一起，末步压缩到远超 0.775——`ratio_L/R=0.9603`（R=326×184、src 2560×1440）、`0.9328`（R=332×186、src 3840×2160）、`0.9486`（R=330×184、src 1366×768），L 全为 320×180。L·8<R·7 → **真正低一档的 floor 被错堵**。
- **cap-truncated-step（2 例）**：画质 cap 截断向上步——src=3840×2160 q=low，R=1376×774(1065024) L=1210×680(822800) U=1280×720(921600=cap)，`ratio_U/R=0.8653<0.875` → 回步**逃逸**；src=3440×1440 q=low 同（R=1594×666，U=1484×620，0.8667）。ceiling 高于 cap 在生产可达（初始帧按 native 编码、饱和降级沿途重学）。

**修复**：门从"ceiling 面积的固定比例"改为"**最大安全基准面积**"。`EncoderThroughputCeiling` 增存 `base_area`——即失败档的下一档（探测失败的回滚目标 / 饱和降级的降级目标），`Set` 增 base 参数（`encoder_throughput_ceiling.h`）；门改 `IsBlockedByEncoderThroughputCeiling(target_area, ceiling_base_area) = target_area > ceiling_base_area`（`video_adaptation_policy.h:239`）；三处门调用传 `BaseArea()`、两处 `Set` 传 base（`ice_transport_controller.cpp:1958` 传回滚 base、`:2319` 传降级目标 nw,nh）。**正确性构造性保证**：`GetNextHigherResolution` 窗口下界 `current_area+1` → 从 base 回步必严格大于 base → 必被堵（抗振荡）；base 本身不>base → 回滚处可驻留；失败档及其 ±2px 舍入变体因阶梯步远>2px 而严格高于 base → 正确归并。压缩步不再是问题，因为比较对象是真下一档而非比例。

**测试**：性质测试重构到 base-area 正确性不变量（非放松——测的是新机制真正依赖的条件）：(1) base 不被堵；(2) base 回步被堵（单调性， Anti-oscillation）；(3) R 及其舍入变体严格高于 base（归并）。**336 对全绿 `violations=0`**（floor/cap 压缩步下变体仍严格高于 base）。`TestRungGranularGateRegression` 改 base 语义仍验证变体归并 + 退避升阶梯 60/120/120/120。子模块 `40e7a51`，超级项目 `79d9e41`。

#### round-8：直连会话首秒画质——分辨率自适应节奏才是真瓶颈（已修复，已落地）

**测量方法（item 1）**：`frame_rate_harness` 指 LAN 地址 `192.168.1.107`，四配置（1512×982@60、1920×1080@60、3024×1964@60、1920×1080@120，HEVC，各 5 次）测首 10s——新增 per-100ms `ramp100` 桶（fps/编码分辨率/延迟 p50）、逐帧解码记录、`--start-bitrate`（**关键：harness 原本硬编码 20 Mbps 起始，会掩盖生产爬坡；必须 `--start-bitrate 2500000` 还原生产 2.5 Mbps 起始**）、控制器 `encoder_target_applied` 日志。逐跑记录 `vm.loadavg`；本机并行构建负载 4-21，数字为**负载指示性**。

**瓶颈命名（item 2，证据）**：**真瓶颈是编码分辨率自适应阶梯的节奏，不是码率爬坡**。证据：(a) 码率爬得飞快——即使从 2.5 Mbps 起始，encoder target 在 ~220ms 内冲到 78 Mbps（探测簇 `probe_controller.cc` 初始 3×/6× 后 2× 链到上限），码率不是限制。(b) 编码**分辨率**从起始码率推导的低档起步，**每 ~5s 才爬一档**（2000ms 探测 + 3000ms 冷却）。码率→分辨率模型（`resolution_adapter.cpp`）：`coeff=2.70·sqrt(fps/30)·headroom`（maintain_frame_rate headroom=1.25），pixels=bitrate/coeff——1080p60 需 ~9.9 Mbps、4K@60 ~28.3 Mbps、1080p120 ~14 Mbps。(c) 根机制：初始档被首帧 keyframe 预算锁定（`ice_transport_controller.cpp:1019-1072` `NoteKeyframeBudgetFailure:1309`），向上只能逐档探测；多档直跳 `mapped_target` 的快路径存在（`:2113-2121`）但 `initial_fast_recovery` 被 `balanced_screen_content` 门挡（`:1411-1413`），MaintainFrameRate 永远用不上。**修复前 10s 末**（无一次达原生）：1512×982@60→1068×694、1080p60→1152×648、4K@60→1068×694、1080p120→966×542；27s 时 1080p60 仍仅 1308×734。**该瓶颈仅适用直连**（relay 带宽低，`mapped_target` 本就低）。

**修复（item 3）**：`initial_fast_recovery`（`ice_transport_controller.cpp:1422-1425`）去掉 `balanced_screen_content` 要求——首 30s 内所有 screen-content 降级偏好都用快速多档直跳 `min(mapped_target, quality_max)` + 快探测节奏（冷却 200ms/探测 1000ms/健康 500ms）。**relay 起始不变**（未碰起始码率）。**修复后 10s 末全配置达原生**（3 reps 一致）：1512×982@60→1512×982、1080p60→1920×1080、4K@60→3024×1964（单跳 828×538→3024×1964 成功）、1080p120→1920×1080。time-to-native ≈ **3.8s**（首探测 ~2.7s 起跳 + ~1.1s 探测时长；探测验证编码器容量，失败廉价回滚）。

**受限链路退避验证（item 4）**：新增 `UdpShaper`（`apps/desktop/tests/udp_shaper.cpp/h`）——纯 harness 侧候选端口重写 + 用户态 UDP relay + per-direction token-bucket + 有界 drop-tail 队列，零 minirtc 产品改动；`--bottleneck-mbps`。泛洪实测 4 Mbps 下稳态 4.00-4.02 Mbps 不越顶。**对比低起始(2.5M)vs 高起始(15M)@1080p60**：

| 链路 | 起始 | delivered fps | lat p50/p95 | 10s 末分辨率 | shaper 丢包 |
|---|---|---|---|---|---|
| 4 Mbps | 2.5M（生产） | 58.7 | 15/26ms | 1102×620（正确退避） | **0** |
| 4 Mbps | 15M（高） | 44.3 | 14/41ms | 1150×646 | **167** |
| 10 Mbps | 2.5M（生产） | 55.1 | 23/41ms | 1332×748 | **0** |
| 10 Mbps | 15M（高） | 44.4 | 16/41ms | 1150×646 | **142** |

**结论**：高起始在受限链路**差于"短暂过冲"**——fps 持续劣化（44 vs 58）、142-167 丢包、分辨率反而更低。按用户"差过短暂过冲就换机制而非换测试"——**正确机制正是快速恢复（低起始+快恢复），非高起始**。快速恢复在受限链路正确退避（直跳目标被带宽确认的 `mapped_target` 钳制，4 Mbps 只爬到 ~1102×618 而非原生），零丢包低延迟；非受限 LAN 才跳原生。**生产起始码率保持低位不动**。

**pacer burst（item 5）**：直连 burst 间隔**已是帧率感知**（round-1-7 commit `0ca83dc` 引入，`ice_transport_controller.cpp:55-60` `DirectPacerBurstIntervalMs`）：≤60fps→40ms，否则 `max(5ms, min(40ms, frame_interval×2))`——120fps=16ms、144fps=12ms。§2 旧述"40ms 与 144fps 冲突"**已被预先部分修复**。临时把 `VideoSendDiagnostics` 的 send-timing 日志临时提 INFO（测后已还原）实测**帧发送完成延迟（encode→末包发出）**：144fps mean **0.33-0.58ms**、60fps mean 0.37-2.55ms——**均值亚毫秒**，远未逼近 burst 间隔；60fps（40ms burst）的 max 104ms 反而比 144fps（12ms burst）max 79ms 更差，说明 max 尖峰是机器负载下的系统/编码停顿而非 burst 计时器排队（若排队则 40ms burst 应更差且均值抬高）。`rejected=0`、oldest_wait 桶全 0——pacer 队列从未触限。**结论：120/144fps 下帧不在 burst 计时器后排队，无需修**——非受限 LAN 上 pacing rate 远超瞬时帧码率，帧即编即发。

**round-7 item-5 遗留（item 6）**：4K@144 探测间隔 30→60→120、4K@120 60s delivered/p5/worst——**负载门（1-min loadavg<3）始终未达**（本轮末 1-min≈9.45，并行构建所致），按规则**留作未验证**。

**仍未达 <1s 的残余**：快速恢复 time-to-native≈3.8s（首探测启动前 ~2.7s 健康窗+带宽确认 + ~1.1s 探测时长）。真正 <1s 只能高起始码率直达，但 item-4 已证其受限链路有害——**故 3.8s 是安全机制下的当前最优**，进一步压缩需缩短首探测健康窗（未做，涉探测可靠性权衡）。





#### round-9：多档直跳失败的回滚基准 + 探测前健康冗余（round-8 后续，已修复，已落地）

**round-9-1 上限交互回归（已修复）**：不可持续配置（3024×1964@120/@144，HEVC，MaintainFrameRate，60s，parent-of-`74ce077` vs `74ce077` vs HEAD-final，交错各 3 reps）。**回归确诊**：快速恢复直跳目标 `min(mapped_target, quality_max)`=原生（带宽已确认）；在 4K@120/144（编码器仅可持续 ~1930×1254@120）它从起始档直跳原生，探测失败 `low_frame_rate`，而修复前回滚目标**和** ceiling 基准都是跳前的起始档——**每个被跳过的中间档都在 ceiling 基准之上 → 全被 `IsBlockedByEncoderThroughputCeiling` 拦 → 钉死在起始档满 30s 重探测退避**。实测 broken(`74ce077`) 4K@120 钉死 694×452 全程（t15-60 中位面积 0.52M，t15 仍 694×452）。**最终修复**：①回滚改为**跳前基档**（最后实测健康的尺寸），非"失败档之下最高梯档"——后者对多档直跳而言仍高于可持续尺寸，会落进饱和区再付代价走下来（`ice_transport_controller.cpp:1965-1977`）；②ceiling 基准=失败档之下最高梯档（2830×1838），封死失败尺寸而放开区间内所有跳过的档；③探测目标就是**默认的上一档** `GetNextHigherResolution(base)`——曾实现"区间内最低未测档"块（继而又实现几何中点二分 `0b308f6`），但插桩证实该块是死代码：`rung[0]` 与目标初始值同为 `GetNextHigherResolution(base)`，能把目标压到一档以下的 cap 都会在 base 处提前 return，4K@120 60s 全程 720/720 求值 target==rung[0]、0 命中——已删除（`ice_transport_controller.cpp` 净 −62 行），爬升由默认逐档探测 + ceiling 闸（`:2248`）+ 快节奏解释；④快节奏与盲跳标志解耦（`ice_transport_controller.cpp:1456-1480`）——探测节奏（200ms 冷却）keyed off 30s 初始窗而非会被失败清掉的盲跳标志，使逐档爬升够快；ceiling 已封死原生，盲跳不会复发。**验证**（矩阵见下表，指标=t0-15/t15-60 低帧秒数即 fps<90% 配置值 + 两窗中位交付 fps/编码面积 + 最差单秒 fps）：判定准则"HEAD 的 t0-15 低帧秒数 ≤ parent+2（一次失败探测）且 t15-60 中位面积 ≥ parent"**6/6 全过**——HEAD t0-15 低帧秒数恒为 2（恰为初始直跳的 t1+t2，此后再无失败），t15-60 中位面积 4K@120 2.42M vs parent 1.45M（+67%）、4K@144 2.20M vs parent 1.02M（+116%），且 t15-60 低帧秒数 HEAD=2 vs parent=8-10（@120，parent 稳态仍探测不可持续尺寸反复振荡）；删块后 4K@120 复测 low015=2、marea1560=2.42M 不变（死代码证实）；1080p60/4K@60 time-to-native 无回归（见 round-9-3/item-4）。策略级测试 `TestMultiRungJumpDoesNotPinSkippedRungs` + `TestOneRungClimbConvergesToSustainableRung`（`apps/desktop/tests/encoder_ceiling_reprobe_test.cpp`）通过。子模块 `81cff4e`、超级项目 `b58caaf`（测试）/`fabb643`（gitlink）。

矩阵（每行一次 60s 运行；load 为该次运行起始时 1-min loadavg；数值为负载指示性——本机为笔记本，测量期并发负载 5-13，无空闲机 baseline）：

| 配置 | rep | 构建 | 低帧秒t0-15 | 低帧秒t15-60 | 中位fps t0-15 | 中位fps t15-60 | 中位面积t0-15 | 中位面积t15-60 | 最差fps | 起始load |
|---|---|---|---|---|---|---|---|---|---|---|
| 4K@120 | r1 | parent | 0 | 8 | 119 | 119 | 0.41M | 1.45M | 84 | 5.27 |
| 4K@120 | r1 | broken(74ce077) | 1 | 0 | 118 | 118 | 0.31M | 0.52M | 78 | 4.91 |
| 4K@120 | r1 | HEAD-final | 2 | 2 | 115 | 120 | 0.41M | 2.42M | 86 | 8.28 |
| 4K@120 | r2 | parent | 0 | 10 | 118 | 118 | 0.41M | 1.45M | 85 | 6.92 |
| 4K@120 | r2 | broken(74ce077) | 1 | 1 | 117 | 118 | 0.31M | 0.52M | 72 | 7.85 |
| 4K@120 | r2 | HEAD-final | 2 | 2 | 117 | 120 | 0.41M | 2.42M | 85 | 7.21 |
| 4K@120 | r3 | parent | 0 | 9 | 118 | 118 | 0.41M | 1.45M | 85 | 9.69 |
| 4K@120 | r3 | broken(74ce077) | 1 | 0 | 118 | 118 | 0.31M | 0.52M | 77 | 12.86 |
| 4K@120 | r3 | HEAD-final | 2 | 2 | 116 | 120 | 0.41M | 2.42M | 86 | 11.25 |
| 4K@144 | r1 | parent | 0 | 3 | 140 | 142 | 0.37M | 1.02M | 123 | 7.88 |
| 4K@144 | r1 | broken(74ce077) | 1 | 1 | 139 | 141 | 0.29M | 0.48M | 83 | 7.07 |
| 4K@144 | r1 | HEAD-final | 2 | 2 | 139 | 142 | 0.37M | 2.20M | 107 | 7.80 |
| 4K@144 | r2 | parent | 0 | 3 | 140 | 142 | 0.37M | 1.02M | 124 | 7.45 |
| 4K@144 | r2 | broken(74ce077) | 1 | 0 | 139 | 142 | 0.29M | 0.48M | 81 | 5.73 |
| 4K@144 | r2 | HEAD-final | 2 | 2 | 140 | 142 | 0.37M | 2.20M | 108 | 6.65 |
| 4K@144 | r3 | parent | 0 | 3 | 139 | 142 | 0.37M | 1.02M | 123 | 9.12 |
| 4K@144 | r3 | broken(74ce077) | 1 | 0 | 141 | 142 | 0.29M | 0.48M | 85 | 7.31 |
| 4K@144 | r3 | HEAD-final | 2 | 2 | 140 | 142 | 0.37M | 2.20M | 105 | 6.72 |

代表轨迹（4K@120 rep1，t0-15，格式 `t秒 宽x高 fps`）：parent 不直跳原生，慢速爬升 t15 仅 1022×662；broken 直跳失败后全程钉死 694×452；HEAD-final 直跳失败（仅 t1,t2 低帧）后逐档快爬，t15 达 1160×754 高于 parent：

```
P   : 1s:694x452@118 2s:694x452@119 4s:790x514@114 9s:898x584@113 14s:1022x662@115 15s:1022x662@119
B   : 1s:694x452@117 2s:694x452@78  3s-15s 全程钉死 694x452@115-120（无任何恢复）
H   : 1s:3024x1964@91 2s:694x452@102 8s:790x514@109 10s:898x584@114 12s:1022x662@112 15s:1160x754@114
```

**round-9-2 `74ce077` 本身的受限链路新旧对比**（round-8 表比的是起始码率非该提交）：4/10 Mbps 经 `UdpShaper`，1920×1080@60 + 3024×1964@60，parent vs HEAD，各 3 reps（15s，`--start-bitrate 2500000`）。**更正：round-8 报告里的"2.5M"行测的是 round-8 HEAD 构建（`74ce077`），不是 parent**。**结论：HEAD 在受限链路不差于 parent**——零丢包、零失败探测、fps 全等。4 Mbps：HEAD≈parent（1080p→~1100×620、4K→~970×640，p95 13-15ms）。10 Mbps：HEAD 达**更高**分辨率（1080p→1748×984 vs parent 1308×734；4K→1592×1032 vs 1212×788），因快速恢复跳到更高的带宽确认上限，代价是延迟尾略升（p95 23ms vs parent 15ms）但零丢包。这是正确自适应，非回归。

**round-9-3 3.8s 去哪了 + 压缩**：1080p60 时间线（日志时间戳相对首帧）：+0.000s 首帧 892×500；+0.139s 首 keyframe 解码；**+0.249s 码率 ≥9.9M（1080p 原生所需）——带宽确认极快**；**+2.681s 探测启动 ← 带宽确认后空等 2.43s**；+2.751s 探测测量开始（apply_latency 70ms）；+3.759s 探测接受→原生（探测时长 ~1.0s）。空窗构成：探测闸 `ice_transport_controller.cpp:2295-2305` 要求 `encoded_frame_rate_ready`+fps≥min+`healthy_since`≠0+**`healthy_since` 老化 ≥`kFrameRateHealthyDurationMs`（fast=500ms，`:1473`）**+qp≤0.60+退避+冷却。健康窗验证的是**编码器容量**（fps≥55/60，`video_adaptation_policy.h:70-81`）而非带宽。**已压缩项**：带宽已确认的初始快速恢复路径上，在首个健康测量窗之上再要求 500ms 持续老化**不携带信息**——探测本身在新尺寸测编码器帧率满 1s 且失败廉价回滚，乐观试错成本低。故去掉初始快速恢复的 `healthy_since` 老化要求（`need_healthy_age = !initial_fast_recovery`，`:2294`）；稳态探测保留完整持续健康要求。**效果**：time-to-native n=5 交错测量（min/median/max）：1080p60 前 3.72/3.73/3.78s→后 3.22/3.27/3.29s、4K@60 前 3.73/3.78/4.31s→后 3.24/3.27/3.30s——两配置前后区间均不重叠（前 min > 后 max），改进成立。**保留的空窗**：剩余 ~2.2s 是编码器暖机（采集→编码→keyframe 交换→首个 500ms 容量测量窗），**它携带真实信息**（编码器容量验证），删除会导致对已过载编码器盲目探测——4K@120 不可持续配置验证（final HEAD）：初始直跳 t2.8 失败→正确回滚基档→逐档快爬，t≈27.5s 撞上可持续上限 ~1930×1254@120（非饱和区赌博，失败仅此一次初始直跳），无假阳性探测。≤1s 目标在不牺牲容量验证下不可达。子模块 `65942f7`、超级项目 `bca489a`。

**round-9-4 测试二进制 hang 排查**（HEAD 结果全表，从仓库 cwd `/tmp/farlink-build` 跑）：11 个二进制 **10 过 1 败**。`connection_status_protocol_test` 的"hang"**根因=cwd 依赖**：它从 `std::filesystem::current_path()` 向上 `stat` 找 `xmake.lua`+`minirtc.h`（`FindRepoRoot`）；从 `/tmp/r9` 跑找不到仓库就一直 stat 上爬文件系统（采样显示主线程 stat 风暴，无网络）。**从 `/tmp/farlink-build` 跑 exit=0，非回归**——round-8 报告的"hangs in this environment"是从 `/tmp/r9` 跑的假象。`repository_structure_test` exit=1：`legacy root directory still exists: scripts`——`scripts/version.py` 是 git 跟踪文件，`79d9e41` 就在，round-8/9 未碰——**预先存在的仓库状态问题，非回归**。

**round-9-5 编码器目标的分辨率上限（事实，无改动）**：`ClampEncoderTargetBitrate(bitrate, max_bitrate)`=`min(bitrate, max_bitrate)`（`src/common/bitrate_limits.h:41`）——**平帽，无分辨率缩放**；`kDefaultMaxEncoderBitrateBps=30'000'000`（`:12`），`max_bitrate_` 初始化 `video_toolbox_encoder.mm:102`，钳制警告 `:937`。1080p 与 4K 同帽 30 Mbps（解释 ~78→30Mbps 钳制警告）。**实测编码器实际产出**（harness 用动画噪声帧=恒定运动）1080p60 target~78Mbps：**peak 5.37 Mbps**；4K@60 target~70Mbps 近原生 2822×1832：**peak 6.45 Mbps**——**远低于 target 也远低于 30Mbps 帽**。编码器是**内容约束**不是 target/帽约束（HEVC 对此内容产不出 78Mbps）；静态内容产出**更少**（HEVC 跳过未变宏块——harness 无静态旗标，噪声帧刻意避免塌缩成静态桌面，`frame_rate_harness.cpp:416`）。**relay 不适用同帽**：钳制是编码器**输入**帽（各编码器实现内 `ClampEncoderTargetBitrate`），relay 只转发已编码字节不再编码——`src/relay/` 无 bitrate clamp。

**round-9-6 round-7 item-5 遗留（已测，负载门已退役）**：负载门（loadavg<3）不再用作拒绝条件，改为逐次记录 1-min loadavg 并标注负载指示性。**(a) 4K@144 300s 探测间隔进展**：初始直跳 t2.8 失败 fps64 武装 ceiling（基础 30s）；恢复爬升 t27.5 撞上可持续上限失败 fps114（自然爬坡发现边界）；此后 ceiling 重探测退避阶梯实测 `next_reprobe_backoff` 依次 60s（t34.1）→120s（t94.7）→120s 封顶（t215.3），相邻重探测间距 60.6s、120.6s 与阶梯精确吻合——**30（基础）→60→120s 进展确认**。**(b) 4K@120 60s delivered fps（final HEAD，n=3）**：median 119/120/119、p5 94/91/87、worst 87/87/86（起始 load 5.61-6.98，负载指示性）；median 满帧，p5/worst 的低谷来自初始直跳的 t1-t2 与不可持续尺寸的偶发重探测，符合该配置（原生不可持续）的预期。

**round-9-7 未验证清单（本次工作未确立的事实，合并前须知）**：

1. **全部数值来自合成噪声帧 harness，非真实采集内容**。`frame_rate_harness` 用动画噪声帧制造恒定运动（`frame_rate_harness.cpp:416` 刻意避免塌缩成静态桌面）；真实桌面/游戏内容的编码器负载、码率曲线、keyframe 尺寸分布都不同。可持续上限 ~1930×1254@120 是噪声帧下的测量，真实内容可能更高（静态多）或更低（复杂运动多）。
2. **单一编码器：VideoToolbox HEVC，M1 Pro**。未测 H.264、未测其他 Apple silicon（M2/M3/M4 编码吞吐不同）、未测任何硬件/软件编码器组合。ceiling 基准、可持续档、恢复时序全都是这一颗芯片的实测。
3. **loopback 单机和用户态 shaper，非真实网络**。所有"带宽已确认"跑在 `192.168.1.107` loopback；受限链路行（round-9-2 的 4/10 Mbps）是 harness 侧 `UdpShaper`（用户态 token-bucket 重写候选端口），不是真实 WAN——无真实丢包、抖动、RTT、跨设备时钟。带宽估计与 ceiling 的交互在真实拥塞下未测。
4. **所有 4K 行跑在 loadavg 5-13，无空闲机 baseline**。测量期本机（笔记本）并发负载高（其他 worker + 冒烟工程师），热节流可能压低了编码器可持续上限；空闲机上可持续档、爬升速度、低帧秒数都可能更好。所有 4K 数值是负载指示性，不是干净基准。
5. **测试覆盖：本分支 11 个二进制 vs 集成分支 26 个**。本分支build 只产出 11 个测试二进制，跑了 10/11 过（`repository_structure_test` 预先存在 fail）；集成分支合并后有 26 个，其中 15 个本分支根本没有，从未在这套改动上跑过。
6. **30s 快节奏窗过期后的行为未测**。`fast_cadence` keyed off `now - source_resolution_initialized_ms <= 30000`（`ice_transport_controller.cpp:1467-1471`）；初始窗过期后的稳态探测节奏、以及会话中途源分辨率变化（窗重设）后的行为，都没有专门测量。矩阵的 60s 运行只有前 30s 在快窗内。
7. **ceiling 重探测的成功路径未测**。所有重探测都失败（2092×1358 在 4K@144 不可持续）；编码器条件改善后重探测成功→ceiling 清除的路径（`ice_transport_controller.cpp:2100`/`Clear()`）只在策略级测试覆盖，未在真实编码器上观察。
8. **MaintainResolution 之外的偏好未在矩阵覆盖**。矩阵只测 MaintainFrameRate；Balanced/MaintainResolution 在 4K 不可持续配置下的 ceiling 交互未测。
9. **多通道/多屏并发未测**。所有测量单 Display1 通道；多并发流共享同一编码器时的 ceiling 归因（哪个流触发的）未验证。
10. **长时间稳定性**。最长运行 300s（item-5a）；小时级会话的 ceiling 状态漂移、重探测退避封顶后的长期行为、内存/句柄累积未测。

---

### 改造 1：解开 60fps 上限，支持 90/120/144（体验核心，商业产品的立身之本）

**改哪里**：
1. `CD/libs/wire/include/preview_subscription.h:86-98`：`fps`/`frame_rate` 校验放开 —— 方案：`frame_rate` 接受 `{30,60,90,120,144}`；`fps`（模式标记）语义不变。新增 hostinfo 能力位 `supports_high_fps`（`remote_action.h:142-151` HostInfo 尾部加 bool，旧端 JSON 解析忽略未知字段——需确认 `remote_action.cpp` from_json 对未知键的容忍，**该结构是手写 JSON，未知键默认忽略，兼容**）。
2. `CD/apps/desktop/src/gui/features/gallery/gallery_stream_settings.h:25`：`kFrameRates{0,15,30,60,90,120,144}` + UI 档位（`gallery_settings.slint`、stream 窗口画质菜单）；`ApplyFrameRate` 按对端能力位 clamp 到 60。
3. `MR/src/pc/peer_connection.cpp:72-74` `NormalizeVideoFrameRate`：泛化为 `clamp(v,30,144)` 而非二值化；`direct_peer.cpp:74`、`ice_transport.cpp:325` 同步。
4. `MR/src/media/video/encode/video_toolbox/video_toolbox_encoder.mm:750`：`SetTargetFrameRate` 上限 60→144。
5. `CD/apps/desktop/src/gui/features/devices/session_device_manager.cpp:190`：采集 fps 60→按最高订阅需求（简化为恒 120/144，SCK `minimumFrameInterval` 支持）；`ShouldSendCapturedFrame` 节奏闸参数化。
6. `MR/src/media/media_codec.h:39-40`：`MINIRTC_MAX_FRAME_RATE 60→144`；帧健康阈值 `video_adaptation_policy.h:58-74`（45/55fps 判据）按配置 fps 比例化。
7. `CD/libs/controller`（CLI/mobile）：`preview_subscription` 同库，自动获益。

**设计要点**：编码端零拷贝链路已支持任意 fps；真正的瓶颈在 pacer burst（direct 40ms 间隔——144fps 下一帧 6.9ms，burst 内可能堆积多帧，需把 `ice_transport_controller.cpp:197-198` 的 direct burst interval 降为 5-10ms 或按 pacing rate 自适应）与采集/编码吞吐（M1 Pro 4K@120 H264 硬编需实测，不行则默认 1080p120 或 HEVC）。

**验收**：
- 环回/LAN 双实例（§4 方法 B/C）：stats 面板 FPS ≥115（120Hz 面板、144 配置下实际出帧受 vsync 120 限制——接受"配置 144/实测 120 出帧"）；`Video latency` 中位数相比 60fps 基线下降 ≥5ms。
- 单测：`NormalizeVideoFrameRate` 与 wire Parse 的参数化测试（仿 `libs/wire/tests/wire_test.cpp`）。
- **不破坏**：stock 客户端（无能力位）订阅时 `ApplyFrameRate` clamp 60——行为与今完全一致；stock host 收到 `{frame_rate:120}` 会 Parse 失败忽略 → 新客户端对旧 host 必须只在能力位存在时发高 fps（发送侧 clamp），**否则旧 host 整个订阅被丢弃会导致黑屏**——这是最大兼容风险点，必须先实现能力位协商再放开校验。

**wire 兼容**：半兼容——需双端新版才有高 fps；任何一端 stock 自动落回 30/60。**发送侧 clamp 是硬要求**。

### 改造 2：macOS 默认 HEVC + 低延迟编码参数复查

**改哪里**：
1. `CD/apps/desktop/src/config_center/config_center.cpp:137-139`：`video_encode_format` 缺省 0(H264)→2(H265)（**仅 macOS 构建**；`#ifdef __APPLE__`）。
2. 复查 HEVC 低延迟链路：`video_toolbox_encoder.mm:276-279` HEVC profile `Main_AutoLevel`+`AllowOpenGOP=false`；本机实测 `hevc.rtvc` 低延迟编码器存在且可用（`ENV.md` §4.3）——确认 `EnableLowLatencyRateControl` 在 HEVC 下同样生效（`video_toolbox_encoder.mm:229-237` 已对 HEVC require 硬件）。
3. 关键帧间隔 3000 帧在 120fps 下仅 25s→分组内码率 spike 频繁度需评估；维持现状即可（50s@60fps 已经偏稀，高 fps 下反而变密）——**不改，只记录**。

**验收**：双端新构建互连，stats 面板 encoder 显示 `VideoToolbox (H.265)`；同画质下码率下降（4K60 HEVC vs H264 对比 `bitrate` 读数）；对 stock 端互连回退 H264（SDP 协商已保证，`ice_transport.cpp:1656-1668`）。
**wire 兼容**：完全兼容——codec 本来就是 SDP 协商，旧端无 HEVC 自动 H264。

### 改造 3：音频延迟修复（接收侧有界缓冲 + AV 同步粗对齐）

**改哪里**：
1. `MR/src/transport/channel/rtp_audio_receiver.cpp:64-68` + `MR/src/media/audio/decode/audio_decoder.cpp`：加 30ms 起步、60ms 上限的帧级 jitter buffer（按 RTP 时间戳排序、乱序等待 ≤20ms、超时 PLC）；现在的"零缓冲直解"在 LAN 好、在公网抖动下反而爆音/不同步。
2. `CD/apps/desktop/src/gui/features/devices/session_device_manager.cpp:692-720` `PushAudio`：SDL stream 排队上限控制（`SDL_GetAudioStreamQueued` 超 100ms 则丢旧 PCM），防积压=防延迟累积。
3. （可选、小）opus 启用 inband FEC：`media_codec.h:76` `audio_fec_enabled` 桌面端置 true（SDP 已声明 useinbandfec，`ice_transport.cpp:1298`）——需双端新版才有意义，但开/关对旧端无害（inband FEC 是 opus 内特性，解码端自动利用）。

**验收**：环回会话播放系统提示音/视频，拍屏实测音画差 <80ms（改造前基线通常 150-300ms，源自 SDL 积压+网络）；丢包注入（`network link conditioner` 或 `pfctl` 环回丢包——无 sudo 用 `dnctl` 不行则用 minirtc 日志层丢包模拟，见 §4）下无连续爆音。
**wire 兼容**：完全兼容——接收侧行为，wire 不变；FEC 开关协商已有。

### 改造 4：连接耗时埋点 + LAN 直连首帧优化

**改哪里**：
1. 埋点先行：`connection_runtime.cpp` 在 join 发起/`MiniRtcConnectionConnected`/首帧 `on_receive_video_frame` 三处打 `steady_clock` 日志（`peer_media_callbacks.cpp:51` 区域已有首帧时机）；`ice_agent.cpp`/`ice_transport_controller.cpp` 已有 ICE/DTLS/SRTP 状态日志，补时间差字段。产出 §4 的测量基线。
2. 优化项（按测量结果选择）：host 侧在 `user_join_transmission` 到达前就预建 offer（`minirtc_connection.cpp` offer 生成时机）；LAN 模式 `HostOnly` ICE 候选只有 host 候选，gathering 应可秒完——验证并压缩 `ice_agent` 的 gathering 等待。

**验收**：LAN 直连从"点击连接"到首帧呈现的端到端计时（UI 时间戳日志差）改造后 <1.5s（基线先测）；ID 模式（本机自托管服务器或官方服）有前后对比数据。
**wire 兼容**：完全兼容——不改 wire，只改时序/并行度；offer 预建需保持 SDP 内容不变。

### 改造 5：相对鼠标"游戏模式"（wire 扩展，商业产品的标志性手感）

**改哪里**：
1. `CD/libs/wire/include/remote_action.h:41-61`：`MouseFlag` 增加 `move_relative`；`Mouse` 结构复用 `x,y` 存归一化位移（或 `s` 拆用——**设计决策：新增 `float dx,dy` 字段进 JSON，`mouse` 对象加键，旧端 from_json 忽略未知键后置 flag 未知→丢弃**，安全）。
2. `CD/apps/desktop/src/gui/ui/stream_window.slint` + `gui_application.cpp`：游戏模式开关（工具栏按钮+快捷键）：进入后隐藏本地光标、`CGAssociateMouseAndMouseCursorPosition` 或 warp 回中，逐事件取 `kCGMouseEventDeltaX/Y`（macOS 鼠标 move 事件自带 delta 字段，`CGEventGetIntegerValueField`）——需新增鼠标 event tap 或利用 Slint pointer move 的差分（差分法在边缘会饱和，** warp-to-center 是正解**）。
3. `CD/apps/desktop/src/platform/macos/input/mouse/mouse_controller.cpp`：注入用 `kCGEventMouseMoved` + `kCGMouseEventDeltaX/DeltaY` 字段而非绝对坐标（macOS 支持相对注入），或 `CGEventCreateMouseEvent` 后只设 delta。
4. 能力位：`remote_action.h` HostInfo 加 `supports_relative_mouse`；无能力位按下不进入游戏模式（UI 灰显）。

**验收**：环回注入：连续相对位移 1000 次，host 端光标位置累加误差 <5%（可用 `CGEventCreate(nullptr)` 读回位置对比）；游戏模式开关对 stock host 灰显不可用；退出游戏模式光标位置还原。
**wire 兼容**：半兼容——新 flag 旧端忽略（from_json 的 `MouseFlag` 枚举未知值按 invalid 丢弃，需验证 `remote_action.cpp` 的解析容错）；能力位 gating 保证不发送到旧端。

**规模**：1=M（3-5 天）、2=S（1 天）、3=M、4=S-M、5=M-L。
**顺序**：2（最快见效）→1（核心）→3→4→5。1 与 2 可并行但都需要双端新构建验证。

---

## 4. 测量方案（单机）

### 4.1 可行的验证拓扑（从代码确认）

| 方法 | 可行性 | 依据 |
|---|---|---|
| A. 同一用户双实例 GUI 互连 | **不可行**：单实例锁 `/tmp/CrossDesk-<euid>-client.lock` 写死、无环境变量/参数覆盖；`XDG_*` 只改配置目录不影响锁 | `posix_single_instance.cpp:29-33`、`main.cpp:468-478` |
| B. **CLI 控制端 + GUI 被控端** | **可行**：CLI（`crossdesk-cli`）是独立二进制、不占 client 锁、macOS arm64 已有完整构建记录；走 ID 模式经官方/自托管服务器 | `apps/cli/docs/validation.md`、`apps/cli/docs/terminal-client.md` |
| C. **LAN IP 直连环回** | **127.0.0.1 被硬拒**（`direct_address.h:34-44` 拒绝 loopback/组播/mapped）；但**可用本机真实 LAN IP（en0 地址）双端互连**：GUI 被控（已在跑或新构建）+ CLI/第二用户 GUI 控制，流量走本机网卡环回接口，RTT<1ms 近似理想网络 | `lan_transport.cpp:30-38`、`lan_session_manager.cpp`、`docs/connection-architecture.md` §3 |
| D. 第二 macOS 用户（fast user switching） | 可行但重：euid 不同→锁不同→GUI 双实例；屏幕采集的是当前活动会话，被控端需在另一会话 | `posix_single_instance.cpp:29-31`（锁含 euid） |
| E. 工程外 harness（推荐用于精确测量） | 可行：**直接链接 minirtc 写 send/recv 对**（`minirtc.h` C API），同进程或双进程 HostOnly 直连，可控注入帧/丢包/延迟，读取全部时间戳 | `MR/src/api/minirtc.h:636-654`（DirectOptions）、`docs/connection-architecture.md` §3.3 |

**结论**：日常体验验证用 B/C；精确延迟/ fps 测量用 E（harness 需要新写，约 300-500 行，是首批必须的基础设施）。

### 4.2 端到端延迟（capture→presented）

**已有**（无需新写）：
- stats 面板 `Video latency` = 采集时刻（RTT/2 校准到本机钟）→ Metal `presentedTime`，macOS 上近似 glass-to-glass（`video_latency.h:225-250`、`metal_video_renderer.mm:1429-1442`）。
- 环形链路全部同机（B/C）时 RTT≈0，时钟同源，**测量误差最小**。
- 细粒度日志：`LogVideoFrameDiagnostics` 每窗口输出 received/accepted/committed/presented 计数、receive_to_commit 均值/峰值、drawable wait、wake queue 延迟（`video_frame_diagnostics.h:18-68`）——DEBUG 级，需开日志。

**需要新写**（harness E 内）：
- **视频秒表法**：harness 在注入帧上绘制高对比度帧计数器/毫秒时间戳（NV12 直写 Y 平面数字），接收端解码后 OCR 或像素比对读回 → 真 glass-to-glass 延迟分布（含采集节拍对齐误差 ±8ms@60fps）。这在单机上就是"摄像机拍屏"的软件等价物，且比拍屏更精确。
- **input-to-photon**：harness 注入相对/绝对鼠标事件同时记录 `steady_clock`，host 侧注入后用 `CGEventCreate` 读回光标位置变化时刻；或更高层：控制端发键→host 注入→被控端屏幕内容变化（画面中出现字符）→编码回传→控制端呈现的全链路计时。后者等价"按键到像素"，用视频秒表法同帧对比即可，无需 host 配合。

### 4.3 delivered fps

- stats 面板 FPS（渲染提交计数/秒，`gui_application.cpp:4230-4256`）；`ReceivedVideoStats`（解码到达 fps，`received_video_stats.h:27-48`）。
- harness 内直接计数 `on_receive_video_frame` 回调频率 + dropped/replaced 计数（renderer 的 `VideoFrameStage::replaced`）。
- 采集端 cadence 日志已有：`Capture delivery: ... input_fps/forwarded_fps/rate_dropped`（`session_device_manager.cpp:120-132`）。

### 4.4 码率/丢包/RTT

- stats 面板 In/Out bitrate、loss_rate、RTT（`MiniRtcNetTrafficStats` 每秒上报）。
- 网络损伤：无 sudo 不能用 `pfctl/dnctl`；替代：harness E 在 `send_signal`/媒体路径之间插入用户态丢包/延迟代理（direct peer 的 UDP  socket 在 minirtc 内部——**更简单的做法：改 harness 让两端走本机 LAN IP 并用 `Network Link Conditioner`（Xcode 附加工具，不需 sudo  GUI）**做系统级损伤）。

---

## 5. 不做/暂缓清单

| 项 | 原因 |
|---|---|
| 手柄/虚拟手柄 | macOS 无用户态虚拟手柄注入方案（无 ViGEm 等价）；核心价值在 Windows 被控打游戏——**需要 Windows**；XL 规模 |
| 虚拟显示器（macOS） | CGVirtualDisplay 在 macOS 14.1+ 为受限 API、27.x 可用性未证实；商业产品的虚拟屏也主要服务 Windows 办公场景 |
| HDR 链路 | 本机无 HDR 显示/采集内容验证条件（`ENV.md` §10）；元数据协商是独立大项 |
| 4:4:4 真彩 | 技术上可行（本机已验证 VT 编码）但 wire/渲染全链路改造为 L 规模；且对 stock 端完全无意义（双端必须同新）——**排在高帧率之后，二期** |
| 8 位验证码/免密/双重验证/邀请链接/账号云同步 | 认证规则在**官方服务器**（密码 6 位由服务端生成，doc:185）；改需自托管服务器或服务器配合——**服务器改动，出范围** |
| P2P 打洞提前/专网中继 | 真实 NAT 场景本机无法验证；中继节点是运营资源不是代码 |
| WoL 远程开机 | 需要同网第二台常在线设备——硬件约束 |
| 500Mbps 自定义码率 | CC max 80Mbps 与 wire 100Mbps 上限都要改，且 >100Mbps 在公网无意义；LAN 100Mbps 已远超需求——**先做档位+自定义到 100M**（并入改造 1 的 UI 工作） |
| Web 客户端高帧率 | 浏览器 WebRTC 路径（datachannel_transport）是独立栈，不在首批 |
| UI 主界面重塑为"设备清单+一键进入" | 纯 UI 工作但体量大、不涉及管线正确性；CrossDesk 桌面墙是差异化优势，不应照搬商业产品——**推迟到管线体验达标后** |

---

## 6. 许可义务

**CrossDesk = GPL-3.0-only**（`CD/LICENSE:2`、`CD/xmake.lua:3`）。本 fork 必须：
1. **保留全部版权与许可声明**（源码头注释 `Copyright (c) 2026 by DI JUNKUN` 等不可删除；新增文件可加自己的声明但不得替换）。
2. **衍生作品整体按 GPL-3.0-only 发布**：任何分发（含提供给他人使用的构建）必须附带完整对应源码（含本 fork 的全部修改）或书面源码要约；不得附加额外限制（`GPL §10/§6`）。
3. **不可再许可**：不能改成 MIT/闭源；不能"部分文件闭源"。
4. 修改文件建议标注修改事实（`GPL §5(a)` 要求"prominent notices"），版本号/名称应区分（Farlink 而非 CrossDesk），避免商标性混淆（GPL 不保护商标，但 upstream 名称沿用有社区风险）。
5. **网络使用不构成分发**（GPL 无 AGPL 条款）：仅自用/内部远程控制不触发源码公开义务；一旦把客户端给他人用（哪怕免费下载）即触发。

**MiniRTC = LGPL-3.0-only**（`/tmp/minirtc/LICENSE`）：作为库被链接时，修改 minirtc 本身必须公开 minirtc 的修改源码（本 fork 计划大改 minirtc → 必须公开）；应用侧（CrossDesk/fork）本来已是 GPL，LGPL 不增加额外负担。静态链接时需提供可重链接的对象/源码——GPL 整体发布已满足。

**捆绑第三方许可约束**（`MR/thirdparty/xmake.lua`、`CD/deps/thirdparty`）：

| 组件 | 许可 | 对改造的实际约束 |
|---|---|---|
| Slint 1.17.1 | **GPL-3.0-only** OR 商业双许可（`CD/deps/thirdparty/slint/xmake.lua:4`） | 本项目按 GPL 分支使用——**与 fork 的 GPL 自洽，无额外约束**；但若未来想闭源任何部分则需购商业许可。`.slint` UI 文件改动属 GPL 覆盖范围 |
| OpenH264 2.6.0 | BSD-2-Clause（源码构建） | **专利注意**：Cisco 的专利许可只覆盖其官方预编译二进制；从源码构建/修改后专利许可状态模糊（仓库无任何 patent 声明，`ConnUxLicense` 审计确认 absent）。macOS 主线走 VideoToolbox 硬编（Apple 已覆盖专利），openh264 仅软编回退——风险低但存在 |
| OpenFEC 1.4.2 | **CeCILL-C**（弱 copyleft，法国法） | RS-FEC 大帧路径依赖它；CeCILL-C 与 GPL 兼容（类似 LGPL），修改 openfec 本身需公开 |
| libdatachannel | MPL-2.0 | 文件级 copyleft：改 libdatachannel 内文件需公开那些文件 |
| libnice/glib/gupnp/libsoup | LGPL-2.1+/MPL-1.1 | ICE 栈；修改需公开，链接使用无约束 |
| libsrtp/libopus/libyuv/dav1d/websocketpp/usrsctp/inih | BSD-2/3-Clause | 只需保留声明，无 copyleft |
| SVT-AV1 | BSD-3-Clause-Clear | 明确不授予专利许可——AV1 编码分发的理论专利风险自担 |
| OpenSSL 3.3.2 / Asio / nlohmann_json / spdlog / KCP / cpp-httplib / stb / SDL3(zlib) / tinyfiledialogs(zlib) / miniaudio | Apache-2.0/BSL/MIT/zlib/PD | 宽松，无实质约束；Apache-2.0 有专利授权与声明要求 |
| aom/dav1d | BSD-2-Clause | 同 BSD |

**结论**：GPL-3.0-only 是不可绕过的主线——fork 的一切分发都必须开源且同许可；真正需要写进流程的是：①保留所有头注释与 LICENSE；②分发时提供完整源码（Git 公开仓库即可）；③修改文件加修改标注；④OpenH264/SVT-AV1 的专利风险知悉（不改许可证，仅注意）；⑤不得把 Slint 用于闭源场景。

---

## 附：三大不确定项（影响路线排序）

1. **wire `frame_rate` 放开后 stock 端的确切行为**：已逐行验证——**stock host 收到 `{frame_rate:120}` 时 `PreviewSubscription::Parse` 返回 nullopt，`peer_data_callbacks.cpp:213` 直接 `return`，不应用任何设置、不回 ack，但会保留此前生效的旧订阅**（`previous` 不变，不会黑屏——只是这次高帧率请求被静默吞掉）。HostInfo/RemoteAction 方向：`remote_action.cpp` 对手写 JSON 未知键容忍（`find()`+`value()` 模式，`remote_action.cpp:364-500`；未知 `MouseFlag` 值会被 `static_cast` 后按未知 enum 注入——host 端 `mouse_controller.cpp` switch 无 default 拦截，未知 flag 落到 move 分支按绝对移动处理，**不会崩溃但语义错误**，所以相对鼠标也必须能力位 gating）。**结论：高帧率/相对鼠标的 wire 扩展对 stock 端"静默无效"而非"断流"，能力位协商是硬前置但风险比预想低。**仍需验证：stock **客户端**收到新版 host 的高位宽 `video_settings`/codec 协商行为（HEVC prefer 已有协商，路径安全）。
2. **M1 Pro 的 H264/HEVC 硬编在 4K/1080p 120-144fps 的实际吞吐**——`ENV.md` 只验证到"1080p 单帧可编"，没有连续高帧率吞吐数据；若 4K120 硬编跟不上，高帧率模式需要默认降分辨率或强制 HEVC，这决定改造 1 的默认档位设计。
3. **官方信令服务器对未知字段/行为的容忍度**：join/登录协议是服务端实现（独立仓库，未读）。设备 ID 模式首帧优化（改造 4）若涉及 join 时序调整，依赖服务端行为；自托管可绕开但用户实际用官方服—— LAN/环回不受影响，公网路径的优化验证手段受限。
