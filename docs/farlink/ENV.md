# 本机构建/测试能力（Farlink 环境探测）

> 探测时间：2026-11-25。全部数据来自本机实测（`swift` 脚本、`sysctl`、`codesign`、`ps`），命令与输出摘要一并记录；未能测到的项在文末"缺口"列出。
> 探测脚本为临时文件，已删除（`/tmp/farlink-probe/`）；关键调用以代码片段保留在本文。

## 1. 硬件

| 项 | 值 | 命令 / 输出 |
| --- | --- | --- |
| 机型 | MacBook Pro（MacBookPro18,3） | `system_profiler SPHardwareDataType` → `Model Identifier: MacBookPro18,3` |
| 芯片 | Apple M1 Pro | `sysctl -n machdep.cpu.brand_string` |
| CPU 核心 | 8（6 性能 + 2 能效） | `hw.ncpu=8`、`hw.perflevel0.physicalcpu=6`、`hw.perflevel1.physicalcpu=2`（逻辑同为 6/2） |
| 内存 | 16 GB（17179869184 B） | `hw.memsize` |
| GPU | 14 核，Metal 4，统一内存 | `system_profiler SPDisplaysDataType`；`MTLCreateSystemDefaultDevice().hasUnifiedMemory=true`，`maxThreadsPerThreadgroup=1024×1024×1024` |
| OS | macOS 27.2.1（build 26B131） | `sw_vers`；`ProcessInfo.operatingSystemVersion` |
| 磁盘占用参考 | 对标商业远程桌面产品 185 MB、CrossDesk 61 MB | `du -sh` |

## 2. 显示器

| # | 像素 | 点（pt） | Scale | 刷新率 | maximumFramesPerSecond | 备注 |
| --- | --- | --- | --- | --- | --- | --- |
| 0 | 3024 × 1964 | 1512 × 982 | 2.0 | 120 Hz | 120 | 内置 Liquid Retina XDR，主屏 |

- 屏幕数量：**1**（无外接显示器，`NSScreen.screens.count = 1`）。
- 采集方式：`CGGetActiveDisplayList` + `CGDisplayCopyDisplayMode`（`width/height/pixelWidth/pixelHeight/refreshRate`）+ `NSScreen.maximumFramesPerSecond`。
- 含义：本机**无法目视验证 144 Hz**（面板上限 120 Hz），只能验证 120 fps 出帧与"144 档位可配置但降级到 120 显示"。多屏/副屏场景需要虚拟屏或第二台机器。

## 3. TCC 状态与责任进程

| 检查 | 结果 |
| --- | --- |
| `CGPreflightScreenCaptureAccess()` | **true** |
| `AXIsProcessTrusted()` | **true** |
| 是否触发授权弹窗 | 否（仅 preflight，未调用 request） |

执行的父进程链（`ps -o ppid=,comm=` 逐级上溯）：

```
python3(73134) → omp(59801) → -zsh(59795) → login(59794)
              → iTermServer-3.7.7(93025) → iTerm2(93023, $HOME/Applications/iTerm.app)
```

结论（`[INFERENCE]`，依据 TCC 按 responsible process 归属的机制）：这两项授权属于 **iTerm2**，不是当前工程或任何新 App。含义：

- 现有 shell 里跑 `swift` 探针可以直接采屏、发 AX 事件；
- **新打包的 Farlink.app 首次运行仍会各自弹一次"屏幕录制"和"辅助功能"**，且修改签名/重打包后可能需要重新授权；
- 自动化 UI 测试若由 iTerm 启动可继承授权，若由 Xcode/`launchd` 启动则不会。

## 4. VideoToolbox

### 4.1 编码器列表（`VTCopyVideoEncoderList`，共 27 条）

| 编码 | 硬件加速 | Encoder ID（节选） |
| --- | --- | --- |
| H.264 (`avc1`) | HW | `com.apple.videotoolbox.videoencoder.ave.avc` |
| H.264 (`avc1`) | SW | `com.apple.videotoolbox.videoencoder.h264` |
| HEVC (`hvc1`) | HW | `com.apple.videotoolbox.videoencoder.ave.hevc` |
| HEVC (`hvc1`) | SW | `com.apple.videotoolbox.videoencoder.hevc.vcp` |
| HEVC 低延迟 (`hvc1`) | HW | `com.apple.videotoolbox.videoencoder.hevc.rtvc` |
| H.264 低延迟 (`avc1`) | HW | `com.apple.videotoolbox.videoencoder.h264.rtvc` |
| ProRes 422/422HQ/422LT/Proxy/4444/4444XQ | HW | `...appleproreshw.*` |
| JPEG / JPEG HW | SW / HW | `...jpeg` / `...jpeg.ajpeg` |
| H.263 | SW | `...h263` |
| HEVC + Alpha (`muxa`)、Depth、Disparity | HW/SW | `...hevc-with-alpha:*`、`...depth-hevc:*` |

**列表中没有 `av01`（AV1）与 `vp09`（VP9）编码器** → 本机 VideoToolbox 无 AV1/VP9 编码入口（软、硬均无）；AV1 只有解码（见 4.2）。

### 4.2 解码器列表（动态符号 `VTCopyVideoDecoderList`，共 56 条）

| 编码 | 硬件 | Decoder ID |
| --- | --- | --- |
| HEVC (`hvc1`) | HW | `com.apple.videotoolbox.videodecoder.avd.hevc` |
| H.264 (`avc1`) | HW | `...avd.avc` |
| AV1 (`av01`) | **SW only** | `com.apple.videotoolbox.videodecoder.av1.sw` |
| ProRes / JPEG / Dolby Vision / MPEG-2 / MP4V | HW 或 SW | 见 `...prores`、`...ajpeg`、`...dolbyvision.*` 等 |

### 4.3 建会话 + 编一帧（1920×1080，`VTCompressionSessionCreate` + `EncodeFrame` + 回调）

| 测试 | 结果 |
| --- | --- |
| HEVC 1080p + `kVTVideoEncoderSpecification_EnableLowLatencyRateControl` | 创建 0，选中 `hevc.rtvc`，编码 0，回调 0 |
| HEVC 1080p 默认 | 创建 0，`ave.hevc`，编码 0，回调 0 |
| HEVC 1080p **4:4:4 8-bit**（`444YpCbCr8BiPlanarFullRange`）+ profile `HEVC_Main444_AutoLevel` | `VTSessionSetProperty(ProfileLevel)=0`，编码 0，输出 `hvc1 1920×1080`，`BitsPerComponent=8` |
| HEVC 1080p **4:4:4 10-bit** + `HEVC_Main44410_AutoLevel` | profile 0，编码 0，`BitsPerComponent=10` |
| HEVC 1080p 4:2:0 10-bit + `HEVC_Main10_AutoLevel` | profile 0，编码 0，`BitsPerComponent=10` |
| H.264 1080p + 低延迟 flag（spec 级） | 创建 0，`h264.rtvc`，编码 0，回调 0 |
| HEVC 4K（3840×2160） | 创建 0，编码 0，回调 0 |
| H.264 4K | 创建 0，编码 0，回调 0 |
| HEVC 1080p + `RequireHardwareAcceleratedVideoEncoder` | 创建 0，编码 0，回调 0 |

注意（实测差异，不是失败）：把 `EnableLowLatencyRateControl` 当作 **session property** 再设一次会返回 `-12900`（`kVTPropertyNotSupportedErr`）；作为 **encoderSpecification** 传入才生效并选中 `*.rtvc`。

保留意见：4:4:4 只验证到"会话接受 444 像素缓冲并成功编码，格式描述标 8/10 bit"，**没有解析 SPS 的 `chroma_format_idc`**，因此"码流确实是 4:4:4"属未证实；要闭环需解码回读或解析码流。

## 5. ScreenCaptureKit

```swift
let content = try await SCShareableContent.current
```

| 项 | 结果 |
| --- | --- |
| 调用结果 | 成功，无 error |
| displays | 1（id=1，1512×982 pt，frame 0,0,1512,982） |
| windows | 291 |
| applications | 66 |

未采集任何帧，仅枚举。成功的前提是第 3 节的屏幕录制授权（归属 iTerm2）。未授权进程会报什么错**未实测**（常见为 `SCStreamError -3801` "user declined"，属经验值，不作为本机结论）。

## 6. AudioToolbox（PCM → Opus）

| 测试 | 结果 |
| --- | --- |
| `AudioConverterNew`：PCM s16le 48 kHz 立体声 → `kAudioFormatOpus`，20 ms 帧（960 frames/packet） | **OSStatus 0（noErr）** |
| 设置码率 96 kbps（`'brat'`） | 0 |
| 读回输出 ASBD | `formatID=1869641075 ('opus')`、48000 Hz、2 ch、960 frames/packet |
| `AudioComponentFindNext`（Apple Opus 编码器） | 存在（返回非 nil） |

结论：macOS 自带 Opus 编码器可用，48 kHz 立体声实时音频链路不需要自带编解码库。旁证：基线 AAC 探针因我方 ASBD 构造错误返回 `1718449215 ('fmt?')`，属探针缺陷，不作为 AAC 能力结论。

## 7. 工具链与签名

| 项 | 值 |
| --- | --- |
| Swift | 6.4.2（swiftlang-6.4.2.1.9，clang-2100.4.1.1），target `arm64-apple-macosx27.2.0` |
| Xcode | 27.2（27B52），`xcode-select -p` = `/Applications/Xcode.app/Contents/Developer` |
| macOS SDK | 27.2 |
| Node / npm | v24.22.2 / 11.19.1 |
| 代码签名身份 | **1 个**：`Apple Development: <redacted-dev-cert>`（`security find-identity -v -p codesigning`，仅计数，未记录私钥信息） |

含义：本地开发签名够用；**没有 Developer ID Application 身份，也没有公证配置** → 对外分发前必须补 Developer ID + notarytool。

## 8. CrossDesk.app（现有竞品安装态，只读）

| 项 | 值 | 命令 |
| --- | --- | --- |
| Bundle ID | `cn.crossdesk.app` | `plistlib` 解析 `Contents/Info.plist` |
| 版本 | `v1.7.7-20261124`（ShortVersion 与 Build 相同） | 同上 |
| 最低系统 | macOS 14.0 | `LSMinimumSystemVersion` |
| 架构 / 体积 | arm64 thin；61 MB | `lipo -archs`、`du -sh` |
| 签名 | `Developer ID Application: Hangzhou ChunkDash Technology Co., Ltd. (RJZC936RL7)`；hardened runtime；**非 ad-hoc** | `codesign -dv --verbose=2` |
| Entitlements | `automation.apple-events`、`device.audio-input`、`device.camera`（无 sandbox） | `codesign -d --entitlements -` |
| 权限声明 | `NSCameraUsageDescription`、`NSMicrophoneUsageDescription`、`NSAppleEventsUsageDescription`、`NSScreenCaptureUsageDescription` | Info.plist |
| 是否在运行 | **是** | `pgrep -fl -i crossdesk` |
| 进程 | `39239 /Applications/CrossDesk.app/Contents/MacOS/CrossDesk`（ppid 1，已运行 1 天 4 小时）；`39275 ... --macos-daemon`（子进程） | `ps -o pid=,ppid=,etime=,comm=` |

未启动、未退出、未修改该 App。注意：它正在运行且有常驻 daemon，自身持有屏幕录制/辅助功能授权，并**已监听 `TCP *:52888`**（`lsof -nP -iTCP:52888` → `CrossDesk 39239 ... TCP *:52888 (LISTEN)`）。本机联调前先确认端口占用与 TCC 归属，避免把 CrossDesk 的会话或授权误判为本项目进程。

## 9. 对 Farlink 的直接结论

1. 采集：ScreenCaptureKit 可用且已授权（借 iTerm 的 TCC），Display 1 为 3024×1964@120；实现 4K/144 只能"可配置"，本机验证上限 120 fps。
2. 编码：HEVC 硬编（含低延迟 `rtvc`）与 **HEVC 4:4:4 8/10-bit** 均通过建会话+编帧；H.264 硬编同样可用；**AV1 编码不可用**（列表无 av01），解码仅软解。
3. 音频：Opus 48 kHz 立体声编码器可用，无需第三方库。
4. 网络/输入：无外接屏，多屏与副屏需虚拟屏或第二台机器；AX 已授权，可做键鼠注入与端到端 UI 测试。
5. 签名：仅 1 个 Apple Development 证书，CI 打包与分发链路尚未具备。

## 10. 未获得 / 未证实

- 144 Hz 实机验证（面板 120 Hz 上限）。
- 未授权进程下 ScreenCaptureKit 的具体错误码（本次全程有授权）。
- HEVC 4:4:4 码流的 `chroma_format_idc`（只验证到编码成功与位深）。
- AV1 编码能力（VideoToolbox 列表无 `av01`；第三方编码器未测）。
- Developer ID / 公证 / 分发签名（本机只有 Apple Development）。
- 外接显示器、HDR 元数据、10-bit 显示链路（无对应硬件/内容）。
