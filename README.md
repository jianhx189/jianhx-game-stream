> **v2.0.5 已重新发布（隐私清理完成）**：此前公开的安装包与源码含私人测试地址、隧道口令等信息，现已全部脱敏并重新打包。安装包不再包含任何预填的服务器地址或口令；隧道口令在安装时随机生成。**请勿继续分发 v2.0.4 及更早的安装包。**

# MiniStream

低延迟游戏串流工具：**Sunshine + Moonlight + mslink 两端口隧道**

用一台电脑操作另一台电脑玩游戏。主控端是重做过界面的 Moonlight 客户端（向导式连接、自动配对、串流浮层菜单），
被控端是便携版 Sunshine 加一层 mslink 隧道。

## 为什么需要 mslink

Moonlight / Sunshine 正常要 6 个端口（3 TCP + 3 UDP）。如果你的 FRP 只给 2 个端口（1 TCP + 1 UDP），
mslink 就把这 6 条流复用到那 2 个端口上，Sunshine 和 Moonlight 的协议一行都不用改。

## 快速开始

从 [Releases](https://github.com/jianhx189/jianhx-game-stream/releases) 下载：

| 文件 | 装在哪 |
|---|---|
| `MiniStream-Host-Setup.exe` | 被控端（玩游戏的那台） |
| `MiniStream-Client-Setup.exe` | 主控端（操作的那台） |
| `Moonlight.exe` | 主控端便携版（免安装） |
| `mslink.exe` | 隧道命令行工具 |

1. 被控端双击安装，装完会显示**隧道端口 / 隧道口令 / Sunshine 管理口令**，抄下来
2. 主控端双击安装，首次运行弹三步向导：选连接方式 → 填上面那几项 → 测试连接
3. 主机列表出现电脑后双击 → 自动配对 → 选「桌面」开始串流
4. 串流中按 **F12**（或 Ctrl+Alt+Shift+X）呼出菜单：断开 / 性能 / 鼠标模式 / 全屏 / 最小化

## 当前版本 v2.0.5 改了什么

- **隐私** 安装包与文档中不再包含任何真实服务器地址、隧道口令、Sunshine 密码与本机内网信息；
  被控端安装包不再打包预填的 `mslink-host.ini`，隧道口令改为安装时随机生成。
- **隐私** 仓库中移除了内嵌测试凭据的诊断脚本（`tools/ssh_*.py` 等）。

## v2.0.4 修了什么

- **P1-1** 拆分原始字节转发与隧道帧发送：`send_raw_queued()` 给本地 Moonlight / Sunshine 用，
  `send_tcp_frame_queued()` 只在两个 mslink 之间用。修掉「发给 Sunshine 的数据多一层帧头」。
- **P1-2** `FrameReader` 改用 offset 游标并保留剩余数据，修掉 TCP 粘包导致握手失败。
- **P1-3** 隧道 socket 握手后显式切非阻塞；所有转发统一走各自的发送队列；队列超 4MB 或出错即关闭重建。
- **P1-4** UDP 包加 `[type][flowId][seq]` 认证标签与序号防重放；HELLO 不再携带明文口令。
- **P2-5** `Relay` 暴露 `udpDirectOk()` / `udpViaTcp()`，向导显示真实传输模式（UDP 直连 / TCP 兼容模式）。
- **P2-6** 补齐交付源码缺失的 `Theme.qml`、`BrandButton.qml` 及图标，源码与二进制对齐；
  `manifest.json` 记录各产物 SHA-256。

### 验收记录

- `mslink.exe selftest`：隧道建立 / TCP 往返 / 并发 TCP×3 / UDP 往返 —— 全通过
- 跨机（主控 → FRP → 被控）：`GET /serverinfo` 经隧道返回 576 字节 `200 OK`
- 跨机：Sunshine 管理 API 经隧道返回 `{"pairings":[]}`
- 主控端安装包：静默安装 → 启动 → 卸载无残留；安装后 exe 哈希与构建产物一致
- 被控端安装包：静默安装后 14738/14739/47989/47990/48010 全部监听

## 已知限制

- **UDP 回程**：若 FRP 只转发 UDP 去程，隧道会自动回退到 TCP 承载音视频。
  这是兼容模式，功能可用但实时性略降，根因在 FRP 服务端配置。
- 会话密钥仍经外层 TCP 明文下发，认证用轻量 keyed hash 而非 HMAC-SHA256（后续加固项）。
- 被控端 Sunshine 需在**控制台会话**运行（RDP 会话里检测不到显示器）。

## 源码结构

```
src/
  mslink/                       隧道：1 TCP + 1 UDP 复用 6 条流
    src/relay.cpp                 多路复用实现
    include/mslink/relay.h        协议定义
  moonlight-qt/                 主控端（基于 Moonlight-Qt 修改）
    app/gui/MslinkSetupDialog.qml 三步连接向导
    app/gui/Theme.qml             深色主题令牌
    app/gui/BrandButton.qml       工具栏按钮
    app/mslinkbridge.cpp          mslink 隧道桥接
    app/backend/computermanager.cpp  Sunshine 自动配对
    app/streaming/input/input.cpp    鼠标模式实时切换
docs/
  manifest.json                 版本 / 构建时间 / 产物 SHA-256
  交付说明.txt                   本轮修复与验收详情
```

## 编译

**主控端**（需要 Qt 6.9 + MSVC 2022 + 仓库自带的 `setup-deps.ps1`）

```bash
qmake app/app.pro -o build/Makefile && cd build && nmake
```

**被控端**

```bash
cd src/mslink && cmake -B build -S . -G Ninja && cmake --build build
```

**安装包**（Inno Setup 6）

```bash
ISCC.exe MiniStream-Host.iss
ISCC.exe MiniStream-Client.iss
```

## 许可

GPLv3。基于 [Sunshine](https://github.com/LizardByte/Sunshine) 与
[Moonlight-Qt](https://github.com/moonlight-stream/moonlight-qt)。


## Discover MiniStream

MiniStream is an open-source Windows game and desktop streaming project built on Sunshine and Moonlight, with an mslink tunnel that multiplexes services into one TCP and one UDP port for self-hosted / FRP deployments.

- [Website / 官网](https://ministream.jianhx.com/)
- [Downloads / 下载与 SHA-256](https://github.com/jianhx189/jianhx-game-stream/releases/latest)
- [Issues / 问题反馈](https://github.com/jianhx189/jianhx-game-stream/issues)

适用方向：个人 Windows 游戏串流、远程桌面、自部署 FRP 连接。项目仍在完善传输可靠性与认证；请阅读发布说明中的已知限制。


## 2.1.0 UI 预览版

新增原创图标、主控端设备首页与被控端原生控制台，可查看本机状态、保存基础设置和导出诊断。预览版与正式版分开发行，尚未完成 Win10、UAC 服务启停和长时间跨网游戏验收。

- [预览版下载与 SHA-256](https://github.com/jianhx189/jianhx-game-stream/releases/tag/v2.1.0-preview)
- [真实运行截图](https://ministream.jianhx.com/#screenshots) · [使用教程](https://ministream.jianhx.com/tutorial.html)
- [源码覆盖层、构建说明与验收范围](docs/UI改版与被控端控制台.md)
