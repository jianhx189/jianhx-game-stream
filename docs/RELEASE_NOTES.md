# MiniStream 2.2.0 · 体验与遥测版 / Experience and telemetry

2.2.0 把重点放在“看得清”而不是“跑分更高”：主控端工作台深色重做并新增体验监测页，被控端控制台扩为四页，隧道在内存中有界保留遥测历史与事件，弱网与恢复行为第一次有可重复的端到端回归。没有新增媒体格式，也没有改变编码、端口或认证方式。Powered by jianhx2026。

## 下载与升级

- **Host**：`MiniStream-Host-Setup.exe`，安装在运行游戏/桌面的电脑。
- **Client**：`MiniStream-Client-Setup.exe`，安装在操作电脑。两端建议配套升级。
- **Source**：`MiniStream-2.2.0-Source.zip`，包含修改版 Moonlight 源码与检出的子模块、mslink、控制台、脚本和交接文档；Qt/外部 SDK/Sunshine 来源见清单。
- **校验**：`SHA256SUMS.txt` 与 `release-manifest.json`。哈希在本轮安装包编译完成后生成，不能沿用 2.1.4。

2.1.3 与 2.1.4 用户可直接覆盖升级，配置、隧道口令、Sunshine 配对与 `dpapi:v1:` 凭据保留。2.2.0 的主控端、被控端、两份安装包与源码包已在本轮构建；以 v2.2.0 Release 页面与同版 `release-manifest.json` 记录为准，不要混装两端版本。

## 本轮新增与变更

- **主控端工作台**：深色界面与折叠侧栏，入口为「我的设备 / 体验监测 / 连接配置 / 串流偏好 / 使用教程」。
- **体验监测页**：传输通道标识与定位提示；隧道 RTT / 抖动、实际渲染帧率（附接收帧率）、视频码率、发送队列（4 MiB 上限、重连次数）四张卡；60/300 秒窗口的 RTT、下行吞吐、发送队列趋势；视频体验（网络丢帧、呈现丢帧、分辨率、媒体流 RTT）；客户端耗时（解码、帧排队、渲染、主机处理）；链路计数与最近事件。
- **被控端四页**：主机概览、连接质量、媒体会话、服务与诊断；连接质量含四张 1 秒采样趋势和可按级别筛选的事件表。
- **遥测有界**：300 个约 1 秒采样点、100 条事件，只驻留内存；同一秒只采样一次；同类同文案事件 1 秒去重、单条截断到 163 字节；实时 JSON 不含历史且小于 64 KiB，历史 JSON 小于 128 KiB；状态文件仍按约 1 MiB × 2 轮转。
- **测试**：新增 `relay_telemetry`，mslink CTest 共七项；`tools/mslink_weaknet_e2e.py` 提供 7 场景弱网端到端。
- 2.1.3 的隧道加固、DPAPI、`--password-stdin`、ACL 与多网卡 UDP 修复保持不变；2.1.3 的两处回归由 2.1.4 修复，2.2.0 未回退。

## 指标口径

隧道 RTT 是心跳往返，**不是端到端操作延迟**；媒体网络丢帧是最终未恢复的帧比例，**不是 IP 丢包率**；发送队列是隧道内存排队，硬上限 4 MiB。队列 > 1 MiB、丢帧 > 2%、排队 > 30 ms、解码 > 16 ms 只是定位提示阈值，不是体验评分。缺失或超过 5 秒未更新的采样显示“—”，页面不生成演示曲线。

## 验证范围与限制

本机验证（2026-10-24）：

- mslink CTest **7/7**：`udp_reply_source`、`relay_queue_frame`、`relay_handshake_integration`、`relay_startup`、`relay_stream_open_failure`、`relay_telemetry`、`relay_selftest`。
- 弱网端到端 **7 场景 50/50 检查**：`baseline`、`rtt100_loss2`、`limit5`、`udp_down`、`recover`、`outage30`、`duplex`，结果在 `构建产物/mslink-e2e/latest.json` 与 `<run-id>/summary.json`。
- 主控端、被控端与 Qt 页面实际启动检查。

方法：真实 `mslink.exe` + 假 Sunshine + 127.0.0.1 上双侧对称的损伤代理，只改中间链路；判物理 UDP 用代理侧 UDP data 包增量，不用 TCP 字节。

限制：单机回环、对称损伤、回显是自造数据而非 H.264/Opus 媒体、无 GPU 解码与呈现、不模拟 AQM/IP 分片/MTU/NAT 超时/运营商突发，也不验证 Moonlight FEC/NACK。**Windows 10/11 虚拟机安装本轮未执行**（无 ISO、当前会话非管理员；Hyper-V 可用但未建机）。因此不宣称干净 Win10/11 安装、所有 GPU/驱动组合或真实长时跨网串流通过；这些结果只覆盖本机回环与白盒/集成路径。

## 安全边界

自定义隧道认证与密钥交换仍未经过独立安全审计，会话密钥仍通过 TCP 明文传送，不替代加密 VPN。DPAPI 与 ACL 只降低本机文件泄露面：换用户或回退旧版会读不到口令，分享过口令的用户应尽快轮换。遥测历史与事件只在内存中、复用已认证的 TCP（类型 11，单条 8 KiB），不新增公网管理口；`mslink-status.json`/`.jsonl` 仍含设备名等信息，导出与分享前必须人工脱敏。请使用可信、受保护的网络路径。

## English

MiniStream 2.2.0 focuses on observability rather than new media formats. The controller workspace is rebuilt in a dark shell (My Devices, Experience, Connection, Streaming Preferences, Guide) with a new Experience page: transport badge and one diagnostic hint, RTT/jitter, rendered and received frame rate, bitrate, bounded send queue, 60/300-second RTT, downlink-throughput and queue trends, network and pacing frame drops, resolution, media stream RTT, decode/queue/render/host timings, link counters and recent events. The host console grows to four pages (Overview, Connection Quality, Media Session, Services & Diagnostics) with level-filtered events. Telemetry is bounded in memory — at most 300 one-second samples and 100 events, one sample per second, events deduplicated per category/message within a second and truncated to 163 bytes; live telemetry JSON stays under 64 KiB and history JSON under 128 KiB; nothing new is written to disk. A new `relay_telemetry` test brings CTest to seven cases, and `tools/mslink_weaknet_e2e.py` passes 50/50 checks across seven scenarios (baseline, 100 ms RTT with 2% loss, 5 Mbit/s shaping, UDP blocked, recovery, 30 s outage, duplex) using the real binaries behind a symmetric impairing proxy on 127.0.0.1.

**Limits.** The weak-network run is loopback and symmetric, uses synthetic echo payloads instead of H.264/Opus media, exercises no GPU decode or presentation, and models no AQM, IP fragmentation, MTU or NAT timeout behaviour; it does not replace real WAN gaming tests. No Windows 10/11 virtual machine run was performed this round (no ISO, non-admin session), so clean-install, all-GPU and long-session behaviour are not claimed. The 2.2.0 installers and source bundle were rebuilt in this round; hashes and final build results come from `SHA256SUMS.txt` and `release-manifest.json`.

**Upgrade.** Controller credentials remain current-user DPAPI (`dpapi:v1:`), and the 2.1.3 tunnel hardening, `--password-stdin` handling, ACLs and multi-interface UDP fix are unchanged. The two 2.1.3 regressions (first controller save, leaked listening sockets on failed start) stay fixed as shipped in 2.1.4. The custom authentication and plaintext session key are not independently audited and this is not an encrypted VPN; telemetry and diagnostics are local to the same Windows user, and exported reports still require manual redaction.

[官网 / Website](https://ministream.jianhx.com/) · [教程 / Guide](https://ministream.jianhx.com/tutorial.html) · [GitHub Pages](https://jianhx189.github.io/jianhx-game-stream/) · [Developer handoff](https://github.com/jianhx189/jianhx-game-stream/blob/main/docs/DEVELOPMENT.md)
