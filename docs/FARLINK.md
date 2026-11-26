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
| 接收侧有界抖动缓冲（吸收乱序） | 网络抖动时声音不漂移 | 抖动缓冲目标 30–60ms | `session_device_manager.cpp:47-48`；新增 `audio_jitter_buffer_test.cpp`、`audio_receive_e2e_test.cpp` |
| SDL 输出修剪上限 150ms PCM | 播放队列不再无限堆积、音画不脱节 | 注释预算：网络+播放端到端约 180ms 上限以内 | `apps/desktop/src/gui/features/devices/audio_output_trim.h`；`session_device_manager.cpp:42-52` 注释；`9a6a83c`（SDL trim 单位修正） |

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

环境准备（隔离工具链）：

```sh
source /tmp/farlink-build-tools/env.sh
```

`env.sh` 设置 `XMAKE_GLOBALDIR` / `CARGO_HOME` / `RUSTUP_HOME` /
`MACOSX_DEPLOYMENT_TARGET=14.0` 并把 `/tmp/farlink-build-tools/bin` 加到 `PATH`
（含一个 `pkg-config` → `pkgconf` 转发 shim）。

xmake 选项（`apps/desktop/xmake/options.lua`）：

- `CROSSDESK_PORTABLE`（默认 `false`，`options.lua:31-35`）：便携包，数据存到可执行文件旁。
- `CROSSDESK_ENABLE_UPDATER`（默认 `false`，`options.lua:37-41`）：自动更新。本 fork 关闭；
  便携构建无论此项如何都不带更新器。

测试：音频接收端到端测试 `audio_receive_e2e_test`（`targets.lua:338`）运行约 6 分钟，
且对运行目录（cwd）有要求——请在仓库根目录运行，使相对资源路径可解析。

## 5. 运行与权限

### 5.1 配置文件路径（非便携默认）

`kMachineAppName = "Farlink"`（`machine_identity.h:26`）。macOS 下（`path_backend.cpp`）：

| 内容 | 路径 |
|------|------|
| config.ini | `~/.cache/Farlink/config.ini` |
| 缓存 | `~/.cache/Farlink` |
| LAN 身份 | `~/.config/Farlink/lan/identity.json` |
| 日志 | `~/Library/Logs/Farlink` |

官方 CrossDesk 用 `~/.config|cache/CrossDesk`、`~/Library/Logs/CrossDesk`，
与本 fork **零重叠**，因此可与官方 CrossDesk 共存而互不干扰配置。

### 5.2 macOS 权限

采集屏幕需要「屏幕录制」权限；接收/注入输入需要「辅助功能」权限；声音回采需要相应授权。
这些是 macOS 对 ScreenCaptureKit / 输入注入的强制要求，与上游一致。

### 5.3 同一机器两个采集实例的实测规则

round-8/round-9 实测（`NOTES.md`）：当同机存在**第二个本应用、且已初始化采集**的实例时，
会干扰宿主机首条流（经 replayd），表现为首流 -3805 错误。该干扰**不**由第三方 SCK 工具、
截图工具、会议软件或同机的官方 CrossDesk.app / BenchmarkRemote 触发——它严格要求同应用的第二个
完整采集实例。这正是测试 harness 的配置，属 harness 工件而非产品缺陷。规避方式：控制端用
`--no-local-capture` 作为纯控制端运行（不初始化本地采集）。

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

- **只在本机回环（单台 Mac）验证过**；没有第二台物理机器；WAN / 中继路径完全没有走过。
- Windows / Linux 的打包改动从未执行过（NSIS / PowerShell 在 macOS 上无法测）。
- 诊断导出没有真正点过（无 GUI 交互操作）。
- `repository_structure_test` 是上游遗留的失败（rc=1，报 "legacy root directory still
  exists: scripts"），与本 fork 改动无关，但确实存在。
- replayd 干扰的具体机制**未查明**（round-9 item-3：复现了控制器 SCK 使用的所有已记录
  维度都无法触发该效应，唯一确定的是需要「同应用的第二个完整采集实例」）。
- 高帧率档（90/120/144）在真实硬件 / 中继路径下的行为未充分实测——目前实测都在 60fps 档。
- WAN / 中继路径未经测试（含中继上的帧率、编码、时延表现）。
- `/tmp/farlink-build-dist/Farlink.app` 是**过时**构建（早于更新器移除，仍带更新器），
  **不要启动它**，最终重新打包时会被替换。
