# MiniStream 2.1.2 · 正式版 / Regular release

两端原生界面、真实连接状态与已验证的多网卡 UDP 修复进入正式发行。Powered by jianhx2026。

## 下载与升级

- **Host**：`MiniStream-Host-Setup.exe`，安装在运行游戏/桌面的电脑。
- **Client**：`MiniStream-Client-Setup.exe`，安装在操作电脑。建议两端配套升级。
- **Source**：`MiniStream-2.1.2-Source.zip`，包含完整修改版 Moonlight 源码和检出的子模块、mslink、控制台、脚本和交接文档；Qt/外部 SDK/Sunshine 来源见清单。
- **校验**：`SHA256SUMS.txt` 与 `release-manifest.json`。后者记录组件来源、文件和打包输入。

先结束当前串流、保存配置，在已有安装目录覆盖升级。安装器识别旧安装路径并保留已有隧道配置和 Sunshine 账号；旧 Client 连接可能需要在新向导填写。仅打开新面板不能替换旧后台。

## 本轮改进

- 原创品牌图标、主控端设备工作区、连接向导、小屏适配和串流菜单。
- 被控端原生控制台：认证设备/时长、传输、RTT/抖动、发送趋势、服务、设置、日志与诊断。串流媒体统计来自新版主控端，过期显示“—”。
- 修复 UDP 探测到达一个本机地址、回包却从另一网卡发出的错误；使用 Windows IP_PKTINFO 固定 ACK 和 DATA 回包路径，真实转发连接已恢复 UDP。
- 递增序号、认证后防重放、持续探测/ACK 恢复、实际探测与回包计数。
- VC143 app-local 运行库和按需安装的微软签名 x64 Redistributable；升级信息采用实际配置，避免误报随机密码。
- 中英文介绍、使用教程、排障、源码来源和详细开发交接，官网与 GitHub Pages 同步。

## 验证范围

已通过两端编译、两份安装器编译、CMake/CTest、60/60 本机源地址保持、100/100 UDP 突发、重放窗口和认证遥测，以及受影响真实转发连接的 UDP 恢复。未完成所有干净 Win10/Win11、GPU/驱动组合及长时跨网游戏验收。RTT 不是端到端操作延迟，媒体丢帧不是 IP 丢包率。

自定义隧道认证与密钥交换未经过独立安全审计，不替代加密 VPN。请使用可信受保护的网络路径，勿公开管理页面、凭据或原始转储。

## English

This regular release brings the native controller and host console, actual connection/media telemetry, smaller-display layouts, original branding and the verified multi-interface UDP reply fix. The affected forwarding path recovered from TCP fallback to authenticated UDP.

Install Host on the streaming PC and Client on the PC you operate. End sessions, preserve configuration, upgrade over the existing directory and verify SHA-256. Runtime deployment and upgrade detection are included. The source bundle contains the modified Moonlight checkout/submodules and MiniStream components; external component sources and versions are documented in the manifest.

Builds, protocol tests, 60 source-preservation checks, 100 UDP burst packets and the reproduced forwarding issue were verified. Comprehensive clean-system/driver testing and long WAN game sessions remain ongoing. The custom key exchange is not a fully audited encrypted VPN; use trusted protected paths.

[官网 / Website](https://ministream.jianhx.com/) · [教程 / Guide](https://ministream.jianhx.com/tutorial.html) · [GitHub Pages](https://jianhx189.github.io/jianhx-game-stream/) · [Developer handoff](https://github.com/jianhx189/jianhx-game-stream/blob/main/docs/DEVELOPMENT.md)
