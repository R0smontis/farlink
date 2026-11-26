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
上游基线提交 `c3b26b8`）的改造分支，目标是在体验上对标 对标商业远控。它不是从零写的项目，
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
codec H265，首帧约 1–2.9s（`/tmp/farlink-build-e2e/NOTES.md` round-7）；round-9 验收矩阵
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
source /tmp/farlink-build-tools/env.sh

# 3. 配置。本机 xmake 缓存（.xmake/macosx/arm64/xmake.conf）记录的生效配置为
#    mode=release、plat=macosx、arch=arm64、CROSSDESK_BUILD_DESKTOP=true、
#    CROSSDESK_ENABLE_UPDATER=false、CROSSDESK_PORTABLE=false——即全部默认值
#    （release 是 xmake 未指定 -m 时的默认模式；另外两个开关默认即 false），
#    因此 `xmake b` 在无缓存时会以这套默认自动配置。history 中仅有的两条显式
#    配置行都是便携实验：`xmake f -c -y --CROSSDESK_PORTABLE=y`，及事后还原用的
#    `xmake f -y --CROSSDESK_PORTABLE=n --CROSSDESK_ENABLE_UPDATER=n`。
#    干净检出下要显式固化这套默认配置：
xmake f -y -m release

# 4. 构建主程序（history 中反复出现的实际命令）
xmake b -y -j4 crossdesk
```

产物是裸 Mach-O 可执行文件（非 .app 包）：

- 主程序：`build/macosx/arm64/release/crossdesk`（约 32 MB，arm64）。
- 运行期依赖 `build/macosx/arm64/release/libslint_cpp.dylib`（约 30 MB）——xmake 二进制
  没有 `@rpath` 安装路径，运行时从同目录解析该 dylib（见 `/tmp/farlink-build-e2e/run_host.sh:70-71`），
  因此直接双击/移动单个 `crossdesk` 文件会起不来，必须连同该 dylib 一起。

xmake 选项（`apps/desktop/xmake/options.lua`）：

- `CROSSDESK_PORTABLE`（默认 `false`，`options.lua:31-35`）：便携包，数据存到可执行文件旁。
- `CROSSDESK_ENABLE_UPDATER`（默认 `false`，`options.lua:37-41`）：自动更新。本 fork 关闭；
  便携构建无论此项如何都不带更新器。

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
官方 CrossDesk.app 与 BenchmarkRemote 在整个 round-8 验收期间也从未触发。

**机制未查明**：复现了控制器 SCK 使用方式的所有已记录维度（API 形态、签名身份、相同二进制）
都无法触发该效应。唯一能确定的触发条件是「同应用的第二个完整、已初始化采集的实例」——
但到底需要这个实例的哪一项属性（超出其启动时的枚举之外），目前**未识别**。

**对真实部署的影响未实测**：测试 harness 的同机双实例正是触发场景，但真实使用里是否可能
撞到——例如快速用户切换（fast user switching）下两个登录会话各跑一个本应用实例——
**未实测**，无法据此下结论。规避方式（harness 采用的）：控制端用 `--no-local-capture`
作为纯控制端运行，不初始化本地采集。

## 6. 服务器与数据去向

以下为明确陈述，不作淡化：

- **默认信令服务器是上游的**。全新 profile 会登录 `wss://api.crossdesk.cn:9099`
  （`config_center.h`、`runtime_state.h`），自托管默认关闭，本 fork **未改动**这一默认。
- **TURN/中继凭证由信令服务器按连接下发**，不硬编码；全新 profile 用的是上游 Coturn。
- 线上的版本标识仍是 `1.7.7`，协议标识符未改（见第 3 节）——上游服务器看到的与
  原版 CrossDesk 无异。
- 中继带宽策略与原版一致，唯一新增的需求维度是可选的高帧率档（默认 60，需对端
  `supports_high_fps` 且面板刷新率够），且不超过既有的带宽上限。
- **自托管**：上游提供 `docs/SELF_HOSTING.md`，设置里有相应字段可指向自建服务器。
- **LAN 直连**（`--lan-connect`）不需要任何服务器。

**需要明确的决定**：在默认仍指向**上游服务器**的状态下分发本构建，是一个
**负责人尚未做出的决定**。换句话说：当前构建默认会把本机注册进上游的基础设施；
要不要这样分发、以及是否先切到自托管，需要负责人拍板。

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
- **游戏模式相对鼠标只经单元测试与注入工具验证**（`macos_relative_mouse_injection_test`、
  各 `macos_relative_mouse_*_tool`），从未由人在真实会话里实操过。
- **音频路径只经进程内仿真验证**（`audio_receive_e2e_test` 的网络模型），从未真正用耳朵
  听过声音——音质、杂音、音画同步在真实听觉下如何，未验证。
- **从未对 对标商业远控 本身做过任何测量**：「对标 对标商业远控」是目标，不是结果；没有任何一组
  数字是与 对标商业远控 并排测出来的。
- **工作树在 `/tmp` 下**（`/tmp/farlink-build-audio` 及兄弟目录 `/tmp/farlink-build-e2e`、`/tmp/farlink-build-tools`
  等）：`/tmp` 在重启后会被系统清理，这些树**扛不过一次重启**。要长期保留，需把仓库树
  移到 `/tmp` 之外的位置（例如用户 home 下的某个目录）——本次只指出位置风险，未实际移动。
- `/tmp/farlink-build-dist/Farlink.app` 是**过时**构建（早于更新器移除，仍带更新器），
  **不要启动它**，最终重新打包时会被替换。
