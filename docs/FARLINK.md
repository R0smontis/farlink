# Farlink 技术说明

> 读者：本机的技术负责人。本文只陈述事实；每条结论都可追溯到某个提交哈希或
> `文件:行号`。所有实测数字均来自本机回环（loopback）环境：**M1 Pro，内置屏
> 3024×1964@120**。凡是没有实测来源的方面，文中明确标注「未实测」，不做估计。
>
> 法律层面的修改声明（GPL-3.0 §5(a) / LGPL-3.0 §5(a)）见
> [`docs/FORK_CHANGES.md`](FORK_CHANGES.md)；本文是面向运维的技术文档，二者不重复。

## 1. 这是什么

Farlink 是 **CrossDesk 1.7.7**（上游基线提交 `0551055`
"[fix] migrate legacy autostart settings on upgrade, refs #242"；捆绑媒体库 minirtc
上游基线提交 `c3b26b8`）的改造分支，目标是在体验上对标业界领先的商业远程桌面产品。它不是从零写的项目，
而是在上游代码上叠加了三条独立的改动线（高帧率、游戏/相对鼠标输入、有界音频抖动缓冲），
外加品牌、采集可靠性、启动/退出、LAN 直连等一批工程性修改。

## 2. 相对上游的改动

下表按改动领域列出。`实测数据` 列只收录有来源的数字（全部为本机回环实测）；`提交`
列给出可追溯的提交哈希或文件位置。

### 2.1 高帧率（90/120/144）协商与能力位

| 改动 | 用户可见效果 | 实测数据（本机回环，M1 Pro，3024×1964@120 内置屏） | 提交 / 位置 |
|------|--------------|------------------------------------------------------|-------------|
| 帧率档位从 30/60 扩展到 30/60/90/120/144 | 设置里出现更高帧率档位 | — | `libs/wire/include/video_frame_rate.h:18`（`kVideoFrameRateTiers[] = {30,60,90,120,144}`） |
| 新增 `supports_high_fps` 线上能力位 | 只有双方都支持才用高帧率 | — | `libs/wire/include/remote_action.h:169`、`libs/wire/src/remote_action.cpp:356,623-624` |
| 发送侧无能力时钳制到 60 | 对旧端不出现乱码/拒连，自动降到 60 | — | `libs/wire/include/video_frame_rate.h:42-47`（`NegotiatedVideoFrameRate`）；测试 `libs/wire/tests/video_frame_rate_wire_test.cpp:103-106` |
| 显示刷新率感知（119.88Hz 记作 120 档） | 档位不超过面板能呈现的 | 本机内置屏 3024×1964@120 | `libs/wire/include/video_frame_rate.h:68-87`；`7556160`、`f5ef339` |
| 控制器记住偏好档位，且不被能力/显示钳制改写 | 偏好跨会话保留 | — | `5c43f87`；测试 `apps/desktop/tests/config_video_codec_roundtrip_test.cpp:108-137` |
| GPU 原生 CVPixelBuffer 缩放、编码吞吐自适应、按原因统计丢帧 | 高帧率下控制编码压力 | — | 见 `docs/FORK_CHANGES.md` 修改文件表 |

**实测帧率（60fps 档，本机回环）**：matrix5 全部 5 次通过，presented fps 56.6–56.9，
codec H265，首帧约 1–2.9s（`e2e/NOTES.md` round-7）；round-9 验收矩阵
10/10 通过、全部 0 个 -3805、ice_to_host_sent 0.4–1.5s、fps 55.8–56.8
（`NOTES.md` round-9 item-5）。

### 2.2 macOS 默认 HEVC

| 改动 | 用户可见效果 | 实测数据 | 提交 / 位置 |
|------|--------------|----------|-------------|
| Apple 主机默认编码格式改为 HEVC(H265) | macOS 上默认走硬件 HEVC； SDP 协商仍可与不支持 H265 的对端回落 H.264 | matrix5 / round-9 全部 run 实测 codec=H265 | `apps/desktop/src/config_center/config_center.h:37-42`（Apple 平台 `kDefaultVideoEncodeFormat = H265`，其余平台 H264）；`6313f03` |
| 默认值与非法值回退一致 | 配置写坏时回落平台默认而非崩溃 | — | `config_center.cpp:139-148`；测试 `config_video_codec_roundtrip_test.cpp`（fresh profile / out-of-range 用例） |

### 2.3 音频接收路径

| 改动 | 用户可见效果 | 实测数据 | 提交 / 位置 |
|------|--------------|----------|-------------|
| 接收侧有界抖动缓冲（吸收乱序） | 网络抖动时声音不漂移 | 进程内仿真测试 `audio_receive_e2e_test`：6 场景（clean/loss2/jitter/reorder5/stallburst/lateanchor，`audio_receive_e2e_test.cpp:162,699-701`），逐包网络模型下测量接收路径驻留时间（residence），注释给出的设计目标为 p95≤80ms、max≤100ms（`:50-51`），实测值由 `PrintRow` 打印；守恒核算（`real+dropped==delivered`）不满足时打 `[CONSERVATION-FAIL]`（`:628`），进程退出码取决于 250 次 teardown 压力循环是否全部干净（`:711-713`） | 新增 `audio_jitter_buffer_test.cpp`、`audio_receive_e2e_test.cpp`；抖动缓冲目标 30–60ms 为 `session_device_manager.cpp:47` 注释 |
| SDL 输出修剪上限 150ms PCM | 播放队列不再无限堆积、音画不脱节 | —（无独立实测数字；上表音频行的进程内仿真覆盖接收路径，150ms 上限与「端到端约 180ms」均为 `session_device_manager.cpp:42-52` 注释里的设计预算，非测量值） | `apps/desktop/src/gui/features/devices/audio_output_trim.h`；`session_device_manager.cpp:42-52` 注释；`9a6a83c`（SDL trim 单位修正） |

### 2.4 鼠标 / 游戏输入

| 改动 | 用户可见效果 | 实测数据 | 提交 / 位置 |
|------|--------------|----------|-------------|
| 捕获鼠标（游戏）模式 + 相对鼠标注入 | 远程游戏时鼠标不撞屏幕边缘 | — | 新增 `relative_mouse_injection.h`、`mouse_capturer.mm` 等；`supports_relative_mouse` 能力位 `remote_action.h:176` |
| 游戏模式退出时先join发送线程再释放按键 | 退出时不卡键 | — | `5af03bd` |
| 游戏鼠标逃逸快捷键提示 | 用户知道怎么退出捕获 | — | `b624f39`、`6313f03` |

### 2.5 采集可靠性与首帧时间

| 改动 | 用户可见效果 | 实测数据（本机回环） | 提交 / 位置 |
|------|--------------|----------------------|-------------|
| 僵尸流检测超时 5s → 1.5s（后又细化） | 卡死的首流更快被判死重试 | round-9：僵尸检测在 completion+1.9–2.1s 触发（原 completion+5.0–5.2s），每个僵尸周期省约 3s | `53a09c5`、`3f04bc0`/`2b58526` |
| 音频枚举/启动超时 2s → 750ms（按操作细分） | 音频故障更快暴露 | 健康时枚举中位 160ms 最大 189ms，枚举+启动中位 376ms 最大 684ms | `2b58526`（round-9 item-4） |
| 启动时预热枚举缓存、澄清采集报错 | 首次采集更快、报错更准 | — | `2d5f2af` |
| fps 统计锚定到首帧 | 帧率数字反映稳态而非含连接期 | matrix5 run1 曾只因 fps 失败（全程 873 帧/18.7s 含连接=46.6 平均，稳态约 58） | minirtc `3439c99`、app `f84c697`（round-7） |

### 2.6 启动 / 退出

| 改动 | 用户可见效果 | 实测数据 | 提交 / 位置 |
|------|--------------|----------|-------------|
| SIGTERM 时先停采集、强停 LAN reactor | 会话中 SIGTERM 能快速退出 | 退出耗时从 4.6–5.5s 降到 0.22–0.26s（要求 <2s） | `c3db1a3`（round-7 记录） |

### 2.7 LAN 直连命令行

| 改动 | 用户可见效果 | 提交 / 位置 |
|------|--------------|-------------|
| `--lan-connect <ip:port>`、`--view-only`、`--no-local-capture` | 不经服务器直接连 LAN 主机；可只读；可不初始化本地采集（纯控制端） | 解析 `apps/desktop/src/app/launch_options.cpp`；usage `main.cpp`；`no_local_capture` 在 `session_device_manager.cpp` 强制 |
| 口令经 `FARLINK_LAN_PASSWORD` 环境变量传入，从不入 argv | 口令不出现在进程列表 | `launch_options.cpp` |

### 2.8 品牌与移除的功能

| 改动 | 用户可见效果 | 提交 / 位置 |
|------|--------------|-------------|
| 用户可见产品名改为 Farlink | UI、日志、本地 IPC、OS 名称显示 Farlink | `7b61542`；`machine_identity.h:26`（`kMachineAppName="Farlink"`） |
| 移除自动更新功能 | 不再触发上游更新通道 | `70fe265`、`4e8d7f3`、`d903604` |
| 移除以 Farlink 名义呈现的上游渠道 | 反馈面板改作诊断用途、不再指向上游 | `249493f`、`aa50882` |
| 修正 About 面的归属 | About 正确反映 fork 归属 | `08b819d` |

### 2.9 编码吞吐上限与分辨率恢复（本轮合并线）

这一节是本次合并进来的编码器吞吐/分辨率自适应工作。全部实测数字来自
**合成噪声帧 harness**（`frame_rate_harness` 用动画噪声帧制造恒定运动，`frame_rate_harness.cpp:416`）、
**M1 Pro 单编码器 VideoToolbox HEVC**、**loadavg 5–13 的高负载笔记本**——不是真实采集内容、不是
空闲机基准。逐项机制与证据见 `docs/farlink/GAP.md` round-7/8/9；此处只摘事实与数字。

| 改动 | 用户可见效果 | 实测数据（合成噪声帧 harness，M1 Pro，loadavg 5–13） | 提交 / 位置 |
|------|--------------|------------------------------------------------------|-------------|
| 修复「探测失败成本」根因：测量改与 `FitVideoResolution` 拟合尺寸匹配（原按阶梯 raw 值精确匹配，宽差 2px 永不命中 → 测量从未启动，每次失败探测空等 5s 超时） | 升档探测失败时快速回滚而非卡 5s | 探测开始→回滚从 ~5s 降到 **0.579s/0.576s**（loadavg 12.86；apply ~70ms + 测量 ~505ms） | GAP round-7-1；子模块 `7db702a`；`video_adaptation_policy.h`（`ProbeEncodedSize`/`IsProbeFrameRateConclusivelyLow`） |
| ceiling 门从「ceiling 面积固定比例」改为「最大安全基准面积」（原比例假设在 floor/cap 钳制步上破裂） | 升档被正确限在编码器可持续尺寸内，既不误堵也不漏堵 | 阶梯性质测试 **336 对相邻档全绿 violations=0**（7 源 × 3 画质档） | GAP round-7-2；`encoder_throughput_ceiling.h`（增 `base_area`）；门 `video_adaptation_policy.h:239`；子模块 `40e7a51` |
| 首秒画质：初始快速恢复（`initial_fast_recovery`）去掉 `balanced_screen_content` 门，前 30s 内用多档直跳 + 快探测节奏 | 直连会话开局几秒内升到原生分辨率，而非逐档慢爬 | time-to-native ≈ **3.8s**（修复前 10s 末无一达原生；修复后 10s 末全配置达原生，含 4K 单跳 828×538→3024×1964） | GAP round-8；`ice_transport_controller.cpp:1474-1477`（合并后行号）；子模块 `74ce077` |
| 多档直跳失败回滚改为「跳前基档」+ ceiling 基准=失败档之下最高梯档（修复 round-8 引入的钉死回归） | 直跳失败后能逐档恢复，而非钉死在起始档 30s | 4K@120/144 直跳失败后逐档快爬，t15 恢复面积高于慢速 parent；time-to-native 压缩到 **3.22–3.30s**（n=5 交错，前后区间不重叠） | GAP round-9-1/9-3；`ice_transport_controller.cpp:1992-1994`（合并后行号）；子模块 `051688d`、`65942f7` |
| 编码吞吐 ceiling 改 rung 粒度 + 指数退避重探（非永久） | 编码器饱和时降级稳定，条件改善后能重探回升 | 4K@144 重探测退避实测阶梯 **30→60→120s 封顶**（相邻重探测间距 60.6s/120.6s 与阶梯吻合） | GAP round-9-6a；`encoder_throughput_ceiling.h`；子模块 `e86c201` |

**补充事实（GAP round-8/9）**：

- **真瓶颈是分辨率自适应节奏而非码率爬坡**：即使从生产 2.5 Mbps 起始，encoder target 也在
  ~220ms 内冲到 78 Mbps，码率不是限制；限制来自分辨率逐档探测的节奏（round-8 item-2）。
- **受限链路退避正确**：4 Mbps 经 `UdpShaper` 时零丢包、正确退避到 ~1102×620 而非硬冲原生；
  对比高起始（15M）在受限链路反而更差（fps 44 vs 58、142–167 丢包）——故生产起始码率保持低位
  （round-8 item-4、round-9-2）。
- **encoder target 上限是平帽**：`ClampEncoderTargetBitrate` 平帽 `kDefaultMaxEncoderBitrateBps=30Mbps`
  （`bitrate_limits.h:41`、`:12`），无分辨率缩放；且实测编码器对噪声内容产出远低于帽
  （1080p60 peak 5.37 Mbps、4K@60 peak 6.45 Mbps），编码器是内容约束而非 target/帽约束（round-9-5）。

### 2.10 Wave-4 操作面（唤醒 / 端口转发 / 码率帽 / 快捷键 / 被控端静音）

| 改动 | 用户可见效果 | 提交 / 位置 |
|------|--------------|-------------|
| 保存设备的远程唤醒（W4A） | 最近设备卡片菜单出现「远程唤醒」（仅当主机在认证 LAN 会话中上报过 MAC）；发送 102 字节 magic packet 到全网广播+各接口子网广播。UI 反馈明确「已发送」≠「已上线」——目标机是否唤醒取决于其固件/OS 唤醒配置，**本 fork 未在真实睡眠主机上验证过端到端唤醒**。无头用法 `--wol <mac> [--wol-broadcast <ip>]` 发送即退出 | `network/lan/wake_on_lan.{h,cpp}`；`wake_on_lan_test`（含 loopback UDP 字节级往返）|
| 加密回环端口转发（W4B） | 控制端 `--port-forward <local>:<remote>`（可重复、仅本次会话）在控制端回环绑定监听，经加密会话的可靠数据通道转发到被控端回环目标。被控端同意机制：持久同意仅属加密验证过的可信设备（`trusted/<公钥>`，新鲜 nonce 证明+本地钉钥）；未验证控制端用被控端 `--port-forward-allow <端口>[,...]` 进程级显式授权——该授权适用于宿主进程运行期间**任何**已授权会话（实现刻意不按对端收窄）。只读会话（view-only）绝不转发；准入了逐帧重估，失去授权或加密立即脱离端点、在途字节就地销毁。未批准目标在打开任何被控端 socket 之前以 `kErrorDenied` 显式拒绝；不可达目标回 `kErrorRefused`。两端均仅回环——不新增 LAN 暴露面 | `tools/port_forward_tunnel.{h,cpp}`；`port_forward_{tunnel,consent,reverse}_test` |
| 反向转发规则（W4B followup） | `--port-forward-reverse <local>:<remote>` 仅在接入侧为可信设备或显式一次性实时批准时绑定；出站规则绝不推断反向意图 | 同上 |
| 码率帽预设（W4C） | 流设置菜单 Auto/8M/20M/50M/80M，按设备持久化，折叠进订阅 `max_bitrate_bps` 线路路径实时生效无需重连；画廊分配器更严格时取严格值。帽值经 `MiniRtcUpdateVideoBitrateLimit` 到达拥塞控制 `SetMaxTotalAllocatedBitrate`——**应用预算是上限不是承诺：编码器实际产出仍受内容与链路约束，未经端到端实测验证具体 bps 是否打满** | `features/settings/bitrate_cap_preset.h`；`stream_control_ergonomics_test` |
| 流快捷键（W4C） | Ctrl+Alt+←/→ 循环远程显示器（回绕）、Ctrl+Alt+F 切换全屏；完成键及其自动重复与抬起在本地吞掉（与游戏模式和弦一致） | `features/input/stream_hotkey_matcher.h` |
| 被控端扬声器静音（W4C） | 会话级开关静音被控机物理默认输出（macOS CoreAudio `kAudioDevicePropertyMute`），RAII 恢复精确原始状态；多控制端引用计数，最后请求者解除或断连时恢复——静音绝不比会话活得更久。Linux/Windows 为惰性 stub，绝不半应用。能力位 `supports_host_mute` 门控 | `common/host_audio_mute.h`、`platform/macos/host_audio_mute.mm` |

**已知限制（如实）**：未受信对端的交互会话在线路层无法与有人值守会话区分——harness
不声称覆盖 Slint 同意 UI 本身；持久同意条目需要可信设备证明链，LAN 直连密码认证
不满足该链（LAN 协议甚至不传输控制端设备身份），故 LAN 直连场景下唯一可用的同意
机制就是 `--port-forward-allow` 进程级授权。

### 2.11 Wave-5 核心媒体升级（传输/编解码/渲染/输入线）

四条独立验证线，全部实测数字来自本机（M1 Pro、macOS 27.2 arm64）：

| 线 | 改动 | 实测数据 | 提交 / 位置 |
|----|------|----------|-------------|
| **W5A minirtc 传输与 pacing** | `RingBuffer` 改互斥保护 MPMC 有界队列（上游零同步，MPMC 压测丢 29225/40000 包 + 401 重复）+ `Data` UB 修复，TSan 干净；KCP 在 send/input/recv 后立即 `ikcp_flush`、nodelay 间隔收紧 5ms、`KcpUpdateTimer::ProcessDelayHintMs` 用 `ikcp_check` 死线驱动；`RtpDataSender::Enqueue` 经 `RequestProcess` 唤醒发送线程；`PacedSender` 亚毫秒精度（holdback 5ms/3pkt → 500µs/1pkt、µs 调度 + 100µs 自旋地板、直连 burst 间隔减半帧间隔 8ms@60fps、RTX 快车道、Sunshine 移植 FEC 最小校验地板） | 进程内 loopback（`data_channel_latency_test`）：可靠单向 p50 **9309µs → 18µs**、p95 18118µs → 41µs；ping-pong RTT p50 **18285µs → 50µs**；20ms RTT+2% 丢包下 500/500 保序送达；MPMC 压测 40000/40000 零丢失零重复 | 子模块 `3cf74bf9`；`ringbuffer.h`、`rtp_data_sender.cpp`、`data_channel_send/receive.cpp`、`paced_sender.cpp`、`rtp_fec.cpp` |
| **W5B minirtc 视觉质量与解码** | Crisp Static Frame Refresh（`ZeroHzFrameDetector::Verdict::kQualityRefresh` 每次静止判定仅一次高预算 P 帧精修）；Sunshine 移植 VideoToolbox 码控：LowLatencyRC 下完全省略 `DataRateLimits`、屏幕内容无限按需 GOP、`EnableLTR=false`；解码侧 `kVTDecompressionPropertyKey_RealTime` + 单趟零中间拷贝 `CMBlockBuffer` 装配 | `vt_crisp_refresh_quality_test`（真机 VideoToolbox 编码+解码，滚动后静止文本 PSNR）：**H.264 早期静止均值 PSNR +4.40 dB（38.37→42.78）、refresh 帧 +4.24 dB、稳态帧 +1.64 dB、静止总字节 55390→43856、解码延迟均值 7851→5272µs**；HEVC：早期静止 +1.32 dB、refresh 帧 +1.32 dB、静止总字节 23459→21521、解码延迟均值 7966→5500µs（max 78848→10274µs） | 子模块 `0f78f674`；`zero_hz_frame_detector.h`、`video_toolbox_encoder.mm`、`video_toolbox_decoder.mm` |
| **W5C macOS 异步 Metal + CAS + SCK** | 专用 `QOS_CLASS_USER_INTERACTIVE` 异步 Metal 呈现队列把 `nextDrawable` 阻塞从 Slint UI/输入主线程解耦；Retina 原生 drawable 尺寸 + 包络钳制 CAS 锐化 shader；ScreenCaptureKit `queueDepth = 3`（`FARLINK_SCK_QUEUE_DEPTH` 覆盖；`FARLINK_METAL_SHARPEN` 覆盖锐化） | `metal_cas_renderer_test`：笔画模糊宽度 **−32%**、零 halo/过冲；全部用例通过 | `2ecd5a78`、`4bae0a26`；`metal_video_renderer.mm`、`metal_cas_shader_source.h`、`screen_capturer_sck_impl.mm` |
| **W5D 输入线路与像素滚动** | 32 字节零分配二进制输入协议（`FIN1`）；累积式丢包免疫相对鼠标位移 + 点击边界恢复；macOS 触控板像素级平滑滚动（`CGEventCreateScrollWheelEvent2` 连续像素 + 16.16 定点 delta）；不可靠带序光标位置更新 | `input_wire_test`：二进制编解码 **37.2 ns/op vs JSON 2151.7 ns/op（~58×）、32 vs 51 字节/帧、0 堆分配**；丢包恢复：注入 310 丢包后 truth_dx 误差 3.1e-6、wire 位移 3570/3570 + 16006/16006 全额还原；`pixel_scroll_and_dispatcher_test` 全过 | `010b5911`；`libs/wire/include/input_wire.h`、`remote_input_dispatcher.cpp`、`scroll_accumulator.h` |

**端到端 harness 复核（合并后最终 HEAD，`frame_rate_harness`，loopback ICE HostOnly）**：

| 场景 | delivered_fps | lat_p50/p95/p99 | 备注 |
|------|---------------|------------------|------|
| LAN 干净 1080p60 HEVC 8s | **59.45**（sent 480/delivered 476） | **14ms / 21ms / 32ms** | RTT 0.27ms，peak 9.87 Mbps，零 cwnd/pacer 丢帧 |
| LAN 干净 1080p120 HEVC 8s | **90.98**（sent 960/delivered 728；编码器吞吐约束非网络） | **16ms / 28ms / 70ms** | RTT 0.17ms，peak 15.3 Mbps，呈现间隔 p95 上界 25ms |
| 受限 WAN 10Mbps/30ms/2%loss 1080p60 HEVC 8s | **55.0**（sent 481/delivered 441） | **56ms / 120ms / 182ms** | RTT 62ms，shape 均值 2.48 Mbps——编码器按拥塞控制正确收敛到链路预算内而非硬冲，loss 下持续交付未断流 |

minirtc 侧独立测试全绿：`data_channel_latency_test`、`fec_parity_floor_test`、
`zero_hz_detector_test`（含 4K NV12 哈希 22µs/帧 bench）、`vt_crisp_refresh_quality_test`
（HEVC+H.264 两臂）、`video_playout_buffer_test`、`hevc_sps_chroma_test`（35/35）、
`rtx_suppression_test`、`fec_curve_test`、`fir_throttle_test`。

### 2.12 Wave-6 色彩 / 呈现 / 解码 / 抗丢包升级

四条独立验证线（W6A/W6B 在 minirtc，W6C/W6D 在本仓库），全部实测数字来自本机
（M1 Pro、macOS 27.2 arm64）：

| 线 | 改动 | 实测数据 | 提交 / 位置 |
|----|------|----------|-------------|
| **W6A 零跳内联解码与零拷贝重组（minirtc）** | 稳态 VideoToolbox 帧从网络线程直接内联提交解码（`CanDecodeInline`/`TryDecodeInline`，try_lock 门控，参数集字节比对；会话创建/恢复仍走 `task_queue_decode_`），精确 `kMaxDecodeBacklog=2` 准入 + 关键帧恢复旁路 + 每流 `on_worker` 排序计数器；H.264/H.265/AV1 单拷贝直接写入式 RTP 重组（一次容量分配，`SetSize` 封口）；解码器原生请求 `420f` 全幅输出、编码器把输入矩阵写到 VTCompressionSession 色彩属性（每会话一次），BT.709 端到端往返 | `inline_decode_and_reassembly_test`：H.264+H.265 两臂 Y=0/2/253/255 极值不削波往返、内联分类矩阵全对；端到端 harness A/B 见下表 | minirtc `268c89fc`、`632b2283`；`MINIRTC_INLINE_DECODE=0` 可同二进制 A/B |
| **W6B 宽块 RS-FEC 与 FEC 感知 NACK（minirtc）** | NEON GF(256) Reed-Solomon 快路径扩到 `k <= 48, r <= 16`（credit/burst 上限 4800→24000 字节），经 `a=x-minirtc-fec-wide:48-16` SDP 属性协商（应答仅在对方宣告时回显）；系数矩阵与宽度无关，旧 `rs-v1` 块（k≤16/r≤4）修复字节逐位一致——旧端无损失回退；`NackRequester::SetFecActive` 在 FEC 激活时对孤立 1–2 包缺口把首次 NACK 推迟到 clamp(rtt/3,6,12)ms，突发丢失（≥3）完全绕过 | `wide_fec_and_nack_test`：45 包关键帧突发旧 16/4 恢复 **0/10**、宽 48/16 恢复 **10/10 逐位一致**；热缓存编码 ~36µs/解码 ~41µs；k=48 r=16 连丢 16 包 100% 逐位恢复；SDP 回退与 NACK 时序契约全过 | minirtc `1a99c122` |
| **W6C 全幅 BT.709 采集 + CAMetalDisplayLink 呈现** | ScreenCaptureKit 采集改 `kCVPixelFormatType_420YpCbCr8BiPlanarFullRange` + ITU-R BT.709（旧 420v/BT.601 把桌面 256 个亮度码压进 16–235 的 220 码演播室摆幅，暗色 UI 渐变可见色带，sRGB 红绿走错矩阵；`FARLINK_MAC_CAPTURE_LIMITED_601=1` 回退旧行为）；新增 `CrossDeskMetalDisplayLinkDriver`：专用 `QOS_CLASS_USER_INTERACTIVE` runloop 线程跑 CAMetalDisplayLink（preferredFrameLatency 1、60–120Hz、静态桌面两次空闲 tick 后自动暂停零后台唤醒、提交经闭锁 CFRunLoopSource 唤醒不丢），`FARLINK_METAL_PACING=immediate` 保留 W5C 队列模式供 A/B | `macos_color_and_displaylink_test`（编译产线渲染器本体）：420f+BT.709 256 步进斜坡 **256/256 个互不相同的单调输出级、最大往返误差 0 LSB、0..31 与 224..255 零压碎**（420v+BT.601 对照组只有 220 级）；sRGB 三基色最大通道误差 1 LSB；DisplayLink 300ms 活动期 36–37 tick、闲置后零 tick、invalidate+join 0.09–0.14ms、20/20 快速 attach/detach 压力循环零线程泄漏 | `metal_video_renderer.mm`、`screen_capturer_sck_impl.mm` |
| **W6D 分级延迟归因 HUD/诊断 + 输入快车道** | D8 流统计浮层与脱敏诊断导出新增逐段延迟归因（网络 RTT / 抖动缓冲目标 / 解码 / 呈现四段滚动均值），`ClassifyLatencyBottleneck` 按参考级交互预算（RTT<25ms、抖动<8ms、解码<6ms、呈现<10ms）选出超预算最多主导段；输入派发快车道；HUD 加高 20px（174→194）容纳新行 | `stage_latency_attribution_test`：分类器契约 + 输入队列 10000 样本 mean 11.7µs/p95 14µs/max 72µs 全过；`slint_stream_render_test` 在 194px 新 HUD 下全过 | `video_latency.h`、`remote_input_dispatcher.cpp`、`stream_window.slint` |

**端到端 harness 复核（合并后最终 HEAD，`frame_rate_harness`，loopback ICE HostOnly）**：

| 场景 | delivered_fps | lat_p50/p95/p99 | 备注 |
|------|---------------|------------------|------|
| LAN 干净 1080p60 HEVC 8s，`MINIRTC_INLINE_DECODE=0` | **59.86**（sent 481/delivered 480/dropped 1） | **11ms / 14ms / 18ms** | RTT 0.22ms，peak 9.82 Mbps |
| LAN 干净 1080p60 HEVC 8s，`MINIRTC_INLINE_DECODE=1`（默认） | **60.00**（sent 480/delivered 480/dropped 0） | **11ms / 12ms / 13ms** | RTT 0.27ms，peak 9.87 Mbps——内联解码满分交付且 p99 尾延迟 18→13ms |
| 受限 WAN 10Mbps/30ms/**3%loss** 1080p60 HEVC 8s（默认配置） | **58.98**（sent 480/delivered 472） | **53ms / 134ms / 198ms** | RTT 59.7ms，shape 均值 2.58 Mbps——比 Wave-5 的 2%loss 场景更苛刻（丢包率 +50%）下交付率反而 55.0→58.98，宽块 FEC + FEC 感知 NACK 下持续交付未断流 |

minirtc 侧独立测试全绿（含 Wave-6 新增）：`inline_decode_and_reassembly_test`、
`wide_fec_and_nack_test`、`vt_crisp_refresh_quality_test`（HEVC+H.264 两臂）、
`data_channel_latency_test`、`fec_parity_floor_test`、`zero_hz_detector_test`、
`video_playout_buffer_test`、`fec_curve_test`、`fir_throttle_test`、`hevc_sps_chroma_test`（35/35）。
桌面侧全绿（含 Wave-6 新增与 194px HUD 复测）：`macos_color_and_displaylink_test`、
`stage_latency_attribution_test`、`metal_cas_renderer_test`、`input_wire_test`、
`pixel_scroll_and_dispatcher_test`、`diagnostic_export_redaction_test`、
`inter_present_gap_test`、`inter_present_emission_test`、`slint_ui_smoke_test`、
`slint_stream_render_test`、`slint_popup_render_test`、`crossdesk_wire_test`、
`wake_on_lan_test`、`port_forward_{tunnel,reverse,consent,wire}_test`、
`stream_control_ergonomics_test`、`clipboard_image_wire_test`、`keyboard_state_wire_test`、
`video_frame_rate_wire_test`、`bitrate_limits_test`、`fps_cap_ladder_test`、
`config_video_codec_roundtrip_test`、`lan_discovery_test`、`screen_lock_monitor_test`、
`cursor_position_test`、`video_toolbox_in_flight_test`、`encoder_ceiling_reprobe_test`、
`connection_status_protocol_test`。

### 2.13 Wave-6 整合烟雾暴露的呈现/指标缺陷与修复

合并后的端到端烟雾（`e2e/smoke.sh`，loopback 双实例真实会话）暴露了单元测试覆盖不到的三处缺陷，均已定位根因并修复：

| 缺陷 | 根因 | 修复 | 证据 |
|------|------|------|------|
| **displaylink  pacing 下约半数会话零呈现**（`frames_presented=0`，40s 内首帧不上屏） | `Attach()` 在 `addSubview`/布局/几何生效**之前**创建 `CAMetalDisplayLink`；在 WindowServer 尚未合成该 layer 时创建的链接可能永久不绑定任何显示器——对象保持有效、接受 `paused=NO`、整会话不回调一次，同 layer 上重建也无法恢复（实测 watchdog 重建 25+ 次无一复活）；故障率约 50%，与窗口出现时序竞争 | 驱动创建推迟到视频视图入窗、布局与几何应用之后（`metal_video_renderer.mm` `Attach` 重排） | 修复后连续 24 次默认配置烟雾零闩锁；`presentedTime=0` 被拒呈现比例从 2–8% 降到 ~0.2% |
| **inline 解码 + immediate  pacing 呈现约 2s 后停摆**（`counts=34,29` 随后归零） | 同一闩锁的 immediate 形态：layer 从未合成时 `nextDrawable` 在禁用超时的 render queue 上无限阻塞，串行队列卡死、合并标志永不复位——与内联解码本身无关 | 同上（重排后 layer 必然进入合成） | 修复后 immediate 3/3 通过：1829/1852、1842/1842、1849/1850 呈现 |
| **会话指标恒空**（`decode_samples=0 render_samples=0 inter_present=none`，即便呈现健康） | 退出路径先触发连接断开事件，`peer_event_handler` 的 `video_latency_->Reset()` 先于 `LogVideoSessionSummary` 运行，滚动窗口在读取前被清空；`fps_seconds` 因不参与 Reset 而幸存 | 断开处理器在任何重置前捕获 codec/帧率档，并在视频重置前发射一次性会话摘要（consume-once 保证后续发射为空操作） | 修复后全量填充：decode_mean 1.79ms/inter_present 931 区间/render_mean 实测；`session_teardown_ordering_test` 锁定该顺序契约 |
| **首帧行半数 `codec=unknown`** | 首个传输统计样本（携带解码器名）比首帧晚到约 30ms，镜像竞态 | 解码器名未就绪时延迟发射首帧行，宽限 1500ms（自首次呈现计），逾期以已知信息发射 | 修复后首帧行稳定 `codec=H265` |

**2×2 验收矩阵**（loopback 烟雾，`MINIRTC_INLINE_DECODE` ∈ {0,1} × `FARLINK_METAL_PACING` ∈ {displaylink,immediate}，每格 ≥30s 60fps 档）：

| 单元格 | 时长 | 呈现/解码 | 平均呈现 fps | fps_seconds |
|--------|------|-----------|--------------|-------------|
| inline=1 + displaylink（默认） | 32s+ | 932/948、940/944、946/950（98.3–99.8%） | 59.1–60.0 | 全程无零计数秒 |
| inline=1 + immediate | 32.2–32.3s | 1829/1852、1842/1842、1849/1850（98.8–100%） | 59.5–60.0 | 全程无零计数秒 |
| inline=0 + displaylink | 33.3s | 1873/1877（99.8%） | 60.0 | 全程无零计数秒 |
| inline=0 + immediate | 32.3s | 1829/1851（98.8%） | 60.1 | 全程无零计数秒 |

**pacing 模式实测权衡**（同机 120Hz ProMotion 面板，60fps 流）：

| 模式 | 呈现间隔 p95_max | render_mean（解码回调→真实上屏） | 备注 |
|------|------------------|----------------------------------|------|
| displaylink（默认） | **17ms**（cadence 贴准 vsync） | ~45ms | 相位锁定逐帧贴 VBlank，代价是 tick 等待 + 预取 drawable 各约一拍 |
| immediate | 25ms | **~21ms** | 提交即渲染，端到端延迟更低但间隔抖动更大 |

两模式 decode_mean 均 ~1.8ms（VideoToolbox 硬解），解码与 pacing 选择无关。

### 2.14 Wave-7 发送路径可观测性 / 剪贴板隐私门 / Windows 被控端静音

本轮聚焦 Windows 发送侧可观测性、剪贴板隐私加固与被控端静音补全。关键改动：

| 线 | 改动 | 位置 |
|----|------|------|
| **原生帧物化封口修复（minirtc）** | Wave-6A 容量构造器刻意保持 `Size()==0` 直到显式 `SetSize()` 封口，但 `RawFrame`、D3D11/WMF 解码器、Harmony 解码器三处要么过早封口、要么从不封口。现在三者都只在拷贝/转换/解映射**完成之后**封口一次——过早封口会把半成品当有效帧暴露，从不封口会丢弃每一帧 | `raw_frame.h`、`d3d11_video_decoder.cpp`、`harmony_video_decoder.cpp`；回归测试 `native_frame_materialization_test` |
| **发送侧漏斗计数器 + 5s 停摆检测（minirtc）** | 每一帧视频在每个准入阶段（paused/bitrate/not-ready/cadence/rate-control/pacer/encode-queue/zero-hz/task-generation/task-not-ready/codec-create/materialize/scale）加上 encode 调用/失败/回调都被原子计数，会话拆除时按流发射一次机读漏斗；单调的"编码器适用水位线"在帧死于非编码器自身的守卫时重启，提交了帧却连续 5 秒零编码输出时发射一次性 stall WARN——沉默的零编码会话由此可判定。会话摘要的传输角色改由 `video_send_frames_in` 推导，而非 `frames_encoded`，使编码器全拒帧的宿主仍被正确标记为发送方 | `ice_transport_controller.{h,cpp}`、`ice_transport.cpp` |
| **WMF 编码器可观测性（minirtc）** | `Initialize()` 失败在 release 模式也保留 HRESULT（此前仅在 DEBUG 可见）；准入守卫与未初始化拒绝路径维护原子连续拒绝计数，首次与每第 300 次记录完整帧/几何上下文，任何接受的帧清零 | `wmf_video_encoder.cpp`；测试 `wmf_encode_synthetic_test` |
| **Windows 被控端静音后端** | `HostAudioMute` 新增真实 Windows 实现：Core Audio MMDevice + `IAudioEndpointVolume`，作用于默认渲染端点。静音时固定端点 id，恢复时经 `IMMDeviceEnumerator::GetDevice` 重解析该确切端点——会话中途切换默认输出（插耳机）不会把原扬声器滞留为静音，也不会写入从未静音过的设备状态。记账全部移入平台无关的 `HostMutePolicy`，单元测试经 mock 后端覆盖 100% 静音/恢复状态机 | `host_audio_mute_policy.h`、`host_audio_mute_win.cpp`；测试 `host_audio_mute_test` |
| **`--view-only` / 剪贴板许可门** | 只读或控制关闭的 viewer 会话既不得应用剪贴板内容、也不得为其组装入站载荷，现已端到端强制：图像剪贴板在 `clipboard_allowed_` 为假时丢弃入站帧、快照输入代次，并在递延 OS 写入前与文本一样重新校验会话/许可/代次；文件 offer 入站包丢弃、`ApplyPendingFiles` 在焦点/控制之外再查 `clipboard_allowed_`。会话保留以便恢复访问时无需重新协商 | `clipboard_image_controller.cpp`、`clipboard_files_controller.cpp`；测试 `clipboard_view_only_gate_test` |
| **macOS 渲染可见性与漏斗计数** | Metal 渲染器在 attach 时即发布初始光栅几何/遮挡/可见性（此前 `visible` 一直为假直到首次 resize，首批提交看到的是过期不可见状态），监听 `NSWindowDidChangeOcclusionStateNotification` 以重算可见性并在解除遮挡时唤醒 display link，并喂入零分配原子渲染阶段计数（submit/replace/busy-drop、commit、present-callback/presented、display-link tick/pause、遮挡切换），每会话发射一次伴随的机读 `Video render stages (controller):` 漏斗行 | `metal_video_renderer.mm`、`video_latency.{h,cpp}` |
| **Windows 端口转发半关闭修复** | `shutdown()` 写侧用 winsock2 的 `SD_SEND` 取代仅 POSIX 的 `SHUT_WR` 拼写，修复 Windows 端口转发构建 | `port_forward_tunnel.cpp` |
| **LAN 测试配置播种工具** | `lan_profile_seed_tool` 只走生产身份路径（`Identity::Load/RegisterPassword/SetPassword/SaveListenerMode` + `private_file`），使播种配置与应用自举逐字节一致（含证书与私钥）；密码经 stdin 传入，目标目录必须不存在 | `lan_profile_seed_tool.cpp` |
| **杂项** | `targets.lua` 为 macOS 增加 `@executable_path` rpath，使二进制不依赖 `DYLD_*`（macOS 会在启动链出现系统签名解释器时剥掉它）即可解析内 bundle Slint 运行时；`fail_encode_interposer` 失败注入 harness 复现零编码失败侧的发送阶段诊断 | `targets.lua`、`fail_encode_interposer.cpp` |

## 3. 与原版 / 旧端互通

关键事实：`machine_identity.h:20-25` 的注释明确说明——**线上协议 token、JSON type 值、
信令主机名、上游服务 URL 这些上游互通面保持字节级不变**，否则 fork 将无法再与对端通信。
`Farlink` 只出现在 UI、日志、本地 IPC、OS 名称，不出现在协议里。

下表逐方向给出互通行为。「依据」列：能 pointing 到测试的给测试；只能由代码阅读支持的标注「未实测」。

| 方向 | 维度 | 行为 | 依据 |
|------|------|------|------|
| Farlink(发) → 原版 1.7.7(收) | 帧率 | 无 `supports_high_fps` 时，任何 >60 的档在发送侧钳到 60，旧端收到的字节与现状一致 | `video_frame_rate.h:42-47`；测试 `libs/wire/tests/video_frame_rate_wire_test.cpp:103-106` |
| Farlink(发) → 原版 1.7.7(收) | 编码 | SDP 协商，对端不支持 H265 时回落 H.264 | `config_center.h:37` 注释；`ice_transport.cpp:233`（H265 rtpmap）；协商路径 **未实测** |
| Farlink(发) → 原版 1.7.7(收) | 输入 | 无 `supports_relative_mouse` 时按绝对位置注入，绝不下发 move_relative | `remote_action.h:174` 注释「legacy host … must never receive move_relative input」；**端到端未实测** |
| Farlink(发) → 原版 1.7.7(收) | 音频 | Opus，与上游一致 | `rtp_audio_receiver.cpp`（Opus 接收）；编码参数同上游 **未实测** |
| 原版 1.7.7(发) → Farlink(收) | 帧率 | 旧端只发 30/60，Farlink 按 30/60 收，无兼容问题 | 由能力位缺省即旧行为推出；**未实测** |
| 原版 1.7.7(发) → Farlink(收) | 输入 | Farlink 作被控时按上游原有绝对/相对逻辑 | 未改动被控侧解析；**未实测** |
| 原版 1.7.7(发) → Farlink(收) | 音频 | Opus，与上游一致 | 同音频行 |
| 原版 1.7.7(发) → Farlink(收) | 编码 | 旧端默认 H.264，Farlink 正常解码 | 解码路径未改动；**未实测** |

> 注：本表多数单元格只做了代码阅读级确认（标注「未实测」）。真正双机、跨进程的
> Farlink↔原版互通没有实测过——目前所有实测都是本机回环、且两端都是同一 Farlink 构建。

## 4. 构建

以下命令序列是本机实际产出二进制的命令。`xmake` 的构建/配置命令取自本机
`.xmake/macosx/arm64/cache/history`（xmake 自己记录的命令历史），不是上游 BUILD.md。

```sh
# 1. 子模块（minirtc 是 git 子模块，.gitmodules 指向 github.com/kunkundi/minirtc.git）
git submodule update --init --recursive

# 2. 隔离工具链环境（XMAKE_GLOBALDIR/CARGO_HOME/RUSTUP_HOME/PATH/TMPDIR，
#    MACOSX_DEPLOYMENT_TARGET=14.0，含 pkg-config→pkgconf 转发 shim）
source ~/Projects/farlink/toolchain/env.sh

# 3. 配置。release 是 xmake 未指定 -m 时需要显式给出的模式；其余开关全部默认。
#    历史上缓存里还有 CROSSDESK_ENABLE_UPDATER 开关——该选项已随更新器一并删除，
#    传入会报未知选项。
xmake f -y -m release

# 4. 构建主程序（2026-11-28 起目标名从 crossdesk 改为 farlink）
xmake b -y -j4 farlink
```

产物是裸 Mach-O 可执行文件（非 .app 包）：

- 主程序：`build/macosx/arm64/release/farlink`（约 32 MB，arm64）。
- 运行期依赖 `build/macosx/arm64/release/libslint_cpp.dylib`（约 30 MB）——xmake 二进制
  没有 `@rpath` 安装路径，运行时从同目录解析该 dylib（见 `e2e/run_host.sh:70-71`），
  因此直接双击/移动单个 `farlink` 文件会起不来，必须连同该 dylib 一起。

xmake 选项（`apps/desktop/xmake/options.lua`）：

- `CROSSDESK_PORTABLE`（默认 `false`，`options.lua:31-35`）：便携包，数据存到可执行文件旁。
- ~~`CROSSDESK_ENABLE_UPDATER`~~：已删除——自动更新功能整体移除（2026-11-28），
  任何配置下都不会访问上游更新端点。

### 4.1 测试

这些测试目标是**普通可执行文件**（`targets.lua` 未注册 xmake test 动作），编译产物落在
`build/macosx/arm64/release/` 下，作为二进制直接运行。

快速测试批次的**编译**用 history 中记录的这条命令：

```sh
xmake b -y -j4 audio_jitter_buffer_test audio_output_trim_test audio_receive_e2e_test captured_mouse_queue_test config_video_codec_roundtrip_test connection_status_protocol_test cursor_position_test game_mode_buttons_test game_mode_capture_state_test game_mode_exit_ordering_test game_mode_scale_test instance_scope_test macos_keyboard_modifier_state_test macos_relative_mouse_injection_test path_manager_portable_test repository_structure_test slint_popup_render_test slint_stream_render_test slint_ui_smoke_test update_checker_test version_checker_test video_callback_lifetime_test video_toolbox_in_flight_test video_frame_rate_wire_test macos_capture_sigkill_tool
```

编译后逐个直接运行对应二进制（在仓库根目录运行；`repository_structure_test` 会从 cwd 向上
找仓库根，见 `repository_structure_test.cpp` 的 `FindRepoRoot`，其余测试不依赖仓库文件）：

```sh
./build/macosx/arm64/release/config_video_codec_roundtrip_test
./build/macosx/arm64/release/video_frame_rate_wire_test
# ……其余同名二进制同理
```

`audio_receive_e2e_test` 的运行：

```sh
./build/macosx/arm64/release/audio_receive_e2e_test
```

默认每场景 3000 帧（30 秒×10ms 帧，`audio_receive_e2e_test.cpp:695`），6 个场景
（clean/loss2/jitter/reorder5/stallburst/lateanchor）各跑旁路+缓冲两遍，再加 250 次
teardown 压力循环，全程约 6 分钟。它是纯进程内仿真，不读写仓库文件，**不依赖 cwd**。
需说明：本机 xmake history 只记录 `xmake` 自身的调用（即上面的编译命令），测试二进制是
作为普通可执行文件直接运行的——直接运行不经过 xmake，故其逐条调用没有留下 xmake 记录；
上面给出的运行形式是这些二进制唯一支持的调用方式。

## 5. 运行与权限

### 5.1 配置文件路径（非便携默认）

`kMachineAppName = "Farlink"`（`machine_identity.h:26`）。调用链：`path_manager.cpp` 在非便携
下直接委托给 `platform/macos/path_backend.cpp`（便携则重定向到可执行文件旁的 `data/`、`logs/`，
见 `path_manager.cpp:42,50,58`）。macOS 后端的三条规则（`path_backend.cpp`）：

- `GetConfigPath`（`path_backend.cpp:37-38`）= `$XDG_CONFIG_HOME/Farlink`，缺省 `~/.config/Farlink`。
- `GetCachePath`（`path_backend.cpp:41-42`）= `$XDG_CACHE_HOME/Farlink`，缺省 `~/.cache/Farlink`。
- `GetLogPath`（`path_backend.cpp:45-46`）= `~/Library/Logs/Farlink`，**不看任何 XDG 变量**。

| 内容 | 路径（缺省，未设 XDG） | 走哪条后端 + 调用点 |
|------|------------------------|---------------------|
| config.ini | `~/.cache/Farlink/config.ini` | **cache**：`gui_application.cpp:1294` 用 `cache_path_ + "/config.ini"`，`cache_path_` 来自 `GetCachePath()`（`:1291`） |
| announcement_reads | `~/.cache/Farlink/announcement_reads` | cache：`gui_application.cpp:1292` |
| trusted-devices | `~/.config/Farlink/trusted-devices` | config：`gui_application.cpp:1308` 用 `GetConfigPath()` |
| LAN 身份 identity.json | `~/.config/Farlink/lan/identity.json` | config：`gui_application.cpp:5283` `GetConfigPath()/"lan"`；文件名 `identity.json` 见 `:1745`、`device_identity_store.cpp:75` |
| 日志 | `~/Library/Logs/Farlink` | log：`path_backend.cpp:45-46`（固定 `~/Library/Logs`，无 XDG） |

即 config.ini 落在 **cache** 而 LAN 身份落在 **config**——这组看似反常的划分是代码实际行为
（`config.ini` 走的是 cache 路径，不是 config 路径），上表逐行给了调用点。

**XDG 变量在 macOS 上确实生效**：`XDG_CONFIG_HOME` / `XDG_CACHE_HOME` 只要设成绝对路径就会被
采用（`path_backend.cpp:16-21` 的 `EnvOrDefault` 仅在值为绝对路径时用环境变量，否则回落
`~/.config` / `~/.cache`）。测试 harness 正是靠重定向这两个变量把实例隔离到临时目录的。
注意 `GetLogPath` 不受 XDG 影响——日志始终写 `~/Library/Logs/Farlink`，除非用便携构建。

官方 CrossDesk 用 `~/.config|cache/CrossDesk`、`~/Library/Logs/CrossDesk`，与本 fork
**零重叠**，因此可与官方 CrossDesk 共存而互不干扰配置。

### 5.2 macOS 权限

下表按 macOS 在「系统设置 → 隐私与安全性」里的命名逐项列出。本机系统版本 macOS 27.2.1。

| 权限（系统设置里的名称） | 触发它的代码路径 | `--no-local-capture` 纯控制端是否需要 | 拒绝后的行为 |
|--------------------------|------------------|----------------------------------------|--------------|
| **屏幕录制**（Screen Recording，`Privacy_ScreenCapture`） | 屏幕采集初始化用 `CGPreflightScreenCaptureAccess` 检查（`screen_capturer_sck_impl.mm:411`），UI 触发 `CGRequestScreenCaptureAccess` 弹系统授权框（`mac_permission_runtime.mm:74`） | **不需要**。`--no-local-capture` 下 `InitializeScreenCapturer` 直接返回（`session_device_manager.cpp:213-216`），完全不碰 ScreenCaptureKit | 采集初始化返回 -1，日志 "Screen recording permission not granted"（`:411-415`）；重配时同样拦截（`:1144-1146`）。**视频采集无法进行**，但作为控制端操控别人不受影响 |
| **辅助功能**（Accessibility，`Privacy_Accessibility`） | 被控端把收到的输入事件注入本机用 `CGEventPost(kCGHIDEventTap,…)`（`keyboard_capturer.cpp:318`、鼠标在 `mouse_controller.cpp`）；macOS 要求投递 HID 事件的进程有辅助功能权限。UI 用 `AXIsProcessTrustedWithOptions` 检查/弹窗（`mac_permission_runtime.mm:53-66`），状态经 `gui_application.cpp:2293-2296,3954` 反映到界面 | **不需要本地注入权限**。纯控制端只把输入事件发往远端，注入发生在**被控（host）那一台**；本机不注入 | 被控端注入失败：收到的鼠标/键盘事件无法落到本机（远端看着连上了却操作不动）。界面据 `accessibility_granted` 提示去开权限 |

**声音回采（系统音频）**——具体化：本 fork 的系统音频走 ScreenCaptureKit，
`speaker_capturer_macosx.mm` 用 `SCStream` + `SCContentFilter`（`initWithDisplay`，`:260`）
并设 `config.capturesAudio = YES`（`:257`）。它与屏幕采集**共用同一条 ScreenCaptureKit 显示
采集通道**，在 macOS 27 上落入**同一个「屏幕录制」TCC 权限**（`Privacy_ScreenCapture`）——
不存在独立的「系统音频」权限开关。speaker 采集器自身**不做**单独权限预检（文件内无
`CGPreflight` 调用），而是依赖枚举 `SCShareableContent` 时系统对该权限的强制。换言之：
能录屏的实例就能回采系统音频；没有屏幕录制权限，音频枚举/启动同样失败。
注意：macOS 27.2.1 上 SCK 音频确实归入屏幕录制权限这一点，是按代码路径（同一条 SCStream
显示采集通道）推出的判断，未单独用系统授权弹窗逐项验证，标注**未核实**到 TCC 面板粒度。

### 5.3 同一机器两个采集实例的实测规则

实测现象（round-8/round-9，`NOTES.md`）：当同机存在**第二个本应用、且已初始化采集**的实例时，
宿主机的首条视频流会出现 -3805 错误（经 replayd）。round-8 对照实验：基线 8 轮 19 个错误
（仅 1/8 轮无错，首帧中位 4717ms）；而让控制端实例跳过采集初始化后，8/8 轮零错误、首帧
中位 1107ms。

**什么不触发它**（round-9 item-3 探针矩阵，探针已确认在会话期间活跃枚举）：
简单 API 枚举、重签名为 `id=crossdesk` 的探针、模拟本应用 API 调用序列的探针，全部 0 错误；
两个相同二进制副本（同 cdhash，一个正以 58fps 推流、一个在枚举）也不触发；同机常驻的
官方 CrossDesk.app 与商业对照产品在整机 round-8 验收期间也从未触发。

**机制未查明**：复现了控制器 SCK 使用方式的所有已记录维度（API 形态、签名身份、相同二进制）
都无法触发该效应。唯一能确定的触发条件是「同应用的第二个完整、已初始化采集的实例」——
但到底需要这个实例的哪一项属性（超出其启动时的枚举之外），目前**未识别**。

**对真实部署的影响未实测**：测试 harness 的同机双实例正是触发场景，但真实使用里是否可能
撞到——例如快速用户切换（fast user switching）下两个登录会话各跑一个本应用实例——
**未实测**，无法据此下结论。规避方式（harness 采用的）：控制端用 `--no-local-capture`
作为纯控制端运行，不初始化本地采集。

## 6. 服务器与数据去向

以下为明确陈述，不作淡化：

- **本 fork 不运营、也不默认指向任何信令服务器**。全新 profile 的服务器字段为空
  （`config_center.h`、`runtime_state.h`，2026-11-28 解耦 wave 修改），不做任何
  信令登录；用户在「设置 → 服务器」配置自托管服务器后才上线。上游的
  `api.crossdesk.cn:9099` 从所有默认值中移除（桌面、CLI controller 库、MCP、
  Android/iOS/Harmony 移动端一致）。
- **上游自动更新通道整体移除**：`updater/`、`version_checker/` 源码与
  `crossdesk_updater` 构建目标已删除，`version.crossdesk.cn` /
  `downloads.crossdesk.cn` 在任何配置下都不会被访问（此前只要使用上游公共服务器
  就会周期检查更新）。移动端（Android/iOS/Harmony）的版本检查同样不再发起网络请求。
- **TURN/中继凭证由信令服务器按连接下发**，不硬编码；使用哪台服务器，中继就是
  哪台服务器下发的——完全取决于用户自己的自托管部署。
- 线上的版本标识仍是 `1.7.7`，协议标识符未改（见第 3 节）——协议层面与原版
  CrossDesk 兼容。
- 中继带宽策略与原版一致，唯一新增的需求维度是可选的高帧率档（默认 60，需对端
  `supports_high_fps` 且面板刷新率够），且不超过既有的带宽上限。
- **自托管**：上游提供 `docs/SELF_HOSTING.md`，设置里有相应字段可指向自建服务器。
- **LAN 直连**（`--lan-connect`）不需要任何服务器。

## 7. 已知限制与未验证项

以下逐条列出，未因「不好看」而删减任何一条：

- **只在本机回环（单台 Mac）验证过**：没有第二台物理机器；WAN / 中继路径完全没有走过——
  中继上的帧率、编码、时延表现均未测试（高帧率档 90/120/144 也只在 60fps 档实测过，
  真实硬件/中继下的高帧率行为未充分实测）。
- Windows / Linux 的打包改动从未执行过（NSIS / PowerShell 在 macOS 上无法测）。
- 诊断导出没有真正点过（无 GUI 交互操作）。
- `repository_structure_test` 是上游遗留的失败（rc=1，报 "legacy root directory still
  exists: scripts"），与本 fork 改动无关，但确实存在。
- replayd 干扰的具体机制**未查明**（round-9 item-3：复现了控制器 SCK 使用的所有已记录
  维度都无法触发该效应，唯一确定的是需要「同应用的第二个完整采集实例」）。真实部署
  （如快速用户切换下同机两个会话各跑一个实例）是否会撞到，**未实测**。
- **没有跑过任何真实游戏 / 3D 负载**：整条采集→编码→传输→解码→呈现流水线从未用真实
  游戏画面验证过，所有画面都是桌面/测试内容。
- **编码吞吐/分辨率自适应线（§2.9）的未验证项**——见 `docs/farlink/GAP.md` round-9-7 的十条清单，
  逐条一句话概括：
  1. 全部数值来自合成噪声帧 harness，非真实采集内容（真实桌面/游戏的编码器负载与码率曲线不同）。
  2. 单一编码器 VideoToolbox HEVC、M1 Pro；未测 H.264 及其他 Apple silicon / 软硬件编码器组合。
  3. loopback 单机 + 用户态 `UdpShaper`，非真实网络（无真实丢包/抖动/RTT/跨设备时钟）。
  4. 所有 4K 行跑在 loadavg 5–13，无空闲机 baseline（数值为负载指示性，非干净基准）。
  5. 该线只在本分支 11 个测试二进制上验证过；集成分支合并后有 26 个，其中 15 个该线从未跑过。
  6. 30s 快节奏窗过期后的稳态探测节奏、以及会话中途源分辨率变化后的行为，未专门测量。
  7. ceiling 重探测的**成功**路径未在真实编码器上观察（所有重探测都失败；成功→清除只覆盖在策略级测试）。
  8. MaintainResolution 之外的偏好（Balanced/MaintainResolution 在 4K 不可持续配置下）未在矩阵覆盖。
  9. 多通道/多屏并发共享同一编码器时的 ceiling 归因未验证。
  10. 长时间稳定性未测（最长 300s；小时级 ceiling 状态漂移、退避封顶后长期行为、内存/句柄累积未知）。
- **macOS 部署目标现已固定 14.0，但旧机实测仍缺**：仓库在 `xmake.lua` 顶层以
  `set_config("target_minver", "14.0")` 固定了部署目标（上游只在 CI 传 `--target_minver` 旗标；
  此前本机构建默认为宿主机 SDK 版本 27.2，低版本系统上的行为是未定义的——已修复并验证
  bundle 内全部 Mach-O 均为 `minos 14.0 / sdk 27.2`、`pkg_arm64.sh` 自带的 `verify_app.sh`
  通过）。但**没有任何一台低于 27.2 的 Mac 实际运行过此包**，14.0–26.x 上的真实行为未验证。
- **游戏模式相对鼠标只经单元测试与注入工具验证**（`macos_relative_mouse_injection_test`、
  各 `macos_relative_mouse_*_tool`），从未由人在真实会话里实操过。
- **音频路径只经进程内仿真验证**（`audio_receive_e2e_test` 的网络模型），从未真正用耳朵
  听过声音——音质、杂音、音画同步在真实听觉下如何，未验证。
- **从未对业界商业产品本身做过任何测量**：「对标商业体验」是目标，不是结果；没有任何一组
  数字是与商业产品并排测出来的。
- **工作树已完成搬迁**：仓库、打包产物、端到端脚本与文档已移至 `~/Projects/farlink/`
  （自包含，含两仓库全历史 git bundle 备份；工具链环境为 `~/Projects/farlink/toolchain/env.sh`，
  xmake 本体需按其中说明另行下载）。`/tmp` 下的原始工作树（实验分支构建目录等）
  保留未动但仍**扛不过一次重启**；`/tmp` 下的旧分发 `Farlink.app` 是**过时**构建
  （早于更新器移除，仍带更新器），**不要启动它**。

