# jianhx 游戏串流

> 一个**自用向、极简、低延迟**的 Windows 游戏串流工具：被控端跑游戏，远控端全屏玩。  
> 采集用 DXGI Desktop Duplication，编码用 FFmpeg（NVENC/AMF/QSV/CPU），
> 传输是自定义的 TCP（控制）+ UDP（视频/音频/输入）协议，不依赖第三方串流中间件。

**特点一句话：不折腾、低延迟、鼠标不飘、能传声音、断流会自愈。**

---

## 为什么又造一个轮子

现成方案要么太"云端"（登录、中继、订阅），要么延迟/手感不可控。这个工具的取舍很明确：

- **只给自己用**：不需要账号体系、不需要公网中继服务，配好 FRP 或内网直连就能玩；
- **延迟优先**：UDP 传视频、输入零攒批、关垂直同步、NVENC 低延迟编码；
- **手感优先**：鼠标**绝对映射**（1:1 重合，不吃 Windows 鼠标加速），
  游戏进内场自动切相对原始位移，FPS 瞄准手感和本机一致；
- **看得见状态**：右上角悬浮条、客户端/被控端双日志、崩溃自动 minidump，出问题能自己查。

---

## 核心特性

| 模块 | 说明 |
|---|---|
| **视频采集** | DXGI Desktop Duplication，GPU 零拷贝（BGRA→NV12 走 GPU），只传变化帧 |
| **编码** | FFmpeg：NVENC → AMF → QSV → CPU(libx264) 自动挑可用；1080p60 / 20 Mbps 级别 |
| **传输** | 自定义协议：TCP 控制（握手/HMAC 挑战应答/心跳/参数协商）+ UDP 视频/音频/输入 |
| **UDP 自愈** | UDP 实测不通/丢包过高 → 自动切 TCP 传视频保画面；TCP 下周期性探测 UDP，恢复后自动切回低延迟 |
| **鼠标** | **绝对映射**（本机光标位置 → 远端像素坐标，`SetCursorPos`，1:1 不飘）<br>**相对模式**（被控端光标被游戏隐藏时，发原始位移，FPS 瞄准用）<br>按被控端光标可见性**自动切换**，也可 `mouse_mode=auto/absolute/relative` 强制 |
| **光标** | DXGI 光标形状（MONOCHROME/COLOR/MASKED 统一转预乘 BGRA）实时下发，形状/热点都对 |
| **声音** | WASAPI loopback 录系统混音 → Opus 48kHz 立体声 20ms → UDP 双发抗丢包；TCP 兜底时声音也走 TCP，不断音 |
| **画质面板** | 预设 / 分辨率 / 帧率 / 码率 / 关键帧间隔 / **锐化**，码率与锐化热调不断流，分辨率帧率走重协商 |
| **锐化** | 客户端着色器 Unsharp Mask（只作用亮度，无色边），关/低/中/高，热生效并记住设置 |
| **RustDesk 式工具栏** | 顶部小悬浮按钮（≡）点击展开；碰屏幕顶边 / `Ctrl+Alt+Shift+Q` 也能唤出；不抢焦点、不打断游戏捕获；3 秒自动收起 |
| **诊断** | 双端 rolling 日志、悬浮条实时指标（FPS/码率/丢包/端到端延迟/RTT/解码耗时）、崩溃 minidump + 调用栈还原脚本 |
| **自检** | `ministream-selftest.exe`：**125 条**协议/加密/序列化/生命周期断言，真机可跑 |

---

## 架构

```
┌────────────────────────── 被控端 (host) ──────────────────────────┐
│  DXGI Desktop Duplication ──► NV12 ──► FFmpeg 编码 (NVENC/...)    │
│        │                                    │                     │
│        ├─ 光标形状/位置 (DXGI + GetCursorInfo)                     │
│        └─ WASAPI loopback 系统声音 ──► Opus 编码                    │
│                                    │                              │
│         UDP ──── 视频分片 / 音频 / 光标位置 / 下行探针              │
│         TCP ──── 握手 / 心跳 / 参数协商 / TCP 视频兜底 / 音频兜底    │
│         UDP ◄─── 键鼠输入（Raw Input 注入：SendInput + scancode）    │
└───────────────────────────────────────────────────────────────────┘
                               │
                    （内网 / FRP / 任意 TCP+UDP 转发）
                               │
┌────────────────────────── 远控端 (client) ─────────────────────────┐
│  D3D11 解码 (D3D11VA) ──► 像素着色器 (BT.709 + 可选锐化) ──► 呈现    │
│  远端光标绘制 (D2D) / 悬浮条 (D2D) / 顶部工具栏 (Win32)             │
│  键鼠捕获 (Raw Input + 低层键盘钩子) ──► UDP 输入                   │
│  Opus 解码 ──► WASAPI 播放（40ms 抖动缓冲，超 90ms 自动裁剪）        │
└───────────────────────────────────────────────────────────────────┘
```

## 协议速览

- TCP 帧：`[u32 len][u16 type][u16 rsv][payload]`；握手 = 密码 → `SHA256("MiniStream/v1"||password)` 派生密钥 → HMAC 挑战应答；
- UDP 头固定 28 字节：`type/flags/payloadSize/sessionId/id/index/count/sendTsUs/frameBytes/extra`；
- 视频按 ≤1200 字节分片，收端按 frameId 重组，缺片丢整帧 + 请求关键帧；
- 打字/点击等**按键与按钮事件连发 3 份**（同 seq 去重），抗 UDP 丢包；位移不重发避免"多走一段"；
- 详细规范见 [`docs/PROTOCOL.md`](docs/PROTOCOL.md)。

---

## 快速开始

### 1. 构建（Windows / MSVC）

依赖：

- Visual Studio 2022 Build Tools（MSVC 14.4x）或 VS 2022
- CMake ≥ 3.20 + Ninja（脚本里默认用 `D:\msys64\ucrt64\bin` 的 cmake/ninja，可按需修改）
- **FFmpeg shared 预编译包**（需包含 `libopus`）：放到 `third_party/ffmpeg/`（含 `include/`、`lib/`、`bin/`）
  - 推荐 gyan.dev `ffmpeg-release-full-shared` 或 BtbN `win64-gpl-shared`

```bat
scripts\build.bat
```

产物在 `build\msvc\bin\`：`ministream-host.exe`、`ministream-client.exe`、`ministream-selftest.exe` 以及 FFmpeg DLL。

> ⚠️ 改过头文件/类布局后**必须删掉 `build\msvc` 重新全量编译**，
> 否则增量编译会留下旧 ABI 的 .obj，出现"莫名其妙的状态错乱"（详见 `docs/BUILD.md`）。

### 2. 被控端

```bat
ministream-host.exe            :: 托盘模式，读同目录 host.ini
```

```ini
[server]
port=14738
password=改成你自己的
bind=0.0.0.0

[audio]
enabled=true
bitrate_kbps=96
```

### 3. 远控端

连接面板填被控端 IP/端口/密码即可；或命令行：

```bat
ministream-client.exe --auto --host 1.2.3.4 --port 14738 --password xxx --fullscreen 1
```

公网使用：在中间机上用 FRP 把 **TCP 和 UDP 同一个端口**都转发到被控端（两端都要放行 UDP）。

### 4. 操作

| 操作 | 说明 |
|---|---|
| `Ctrl+Alt+Shift+Q` | 释放 / 重新捕获键鼠（`client.ini` 可改） |
| 顶部小悬浮按钮 / 鼠标碰屏幕顶边 | 唤出工具栏（3 秒自动收起） |
| 工具栏「画质 ▾」 | 画质面板：预设/分辨率/帧率/码率/关键帧/锐化；「收起」或点一下画面关闭 |
| 工具栏「声音」 | 本机静音开关（被控端继续发流，打开立刻有声） |
| 未捕获时 `ESC` | 退出会话 |

---

## 实测数据（自用环境）

- 被控端 RTX 4060 + 1080p 显示器，千兆内网 + FRP 公网中转；
- 1080p60 / 20 Mbps：**解码 ~0.8–1.0 ms，端到端 ~26–49 ms，丢帧 0–3%**（悬浮条实时值）；
- 声音：50 包/秒、40 ms 抖动缓冲、RMS 与源音频一致（440 Hz 测试音实测 RMS≈0.17）；
- 自检：`ministream-selftest.exe` **125/125**，含 8 次连续启停、断线重连、协议编号稳定性。

## 目录结构

```
src/common/    协议、网络、配置、日志、加密、时钟、质量参数
src/host/      DXGI 采集、FFmpeg 编码、WASAPI loopback 音频、输入注入、托盘服务
src/client/    D3D11 解码/渲染、Opus 播放、键鼠捕获、RustDesk 式工具栏
tests/         自检（协议/加密/生命周期）+ 视频链路探针
docs/          PROTOCOL / BUILD / USAGE 文档
config/        host.ini / client.ini 示例
scripts/       构建与打包脚本
tools/         崩溃 minidump 分析等排障脚本
```

## 已知限制 & 声明

- 仅 Windows（被控端需 Win10 1809+，DXGI Desktop Duplication 限制）；
- 视频/音频走自定义加密（HMAC 鉴权 + 会话密钥派生），**没有 TLS**，适合可信网络/FRP 隧道，不建议裸奔公网；
- 没有剪贴板同步、文件传输、多显示器同时串流、麦克风回传（声音是"系统声音 → 远控端播放"单向）；
- 本项目是**个人自用工具**，请只在你自己拥有/授权的设备上使用；请勿用于任何违反法律法规或侵犯他人权益的场景。

## License

未附带开源许可证（个人自用项目）。如需引用/二次分发，请先联系作者。
