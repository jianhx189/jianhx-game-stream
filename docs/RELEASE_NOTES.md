# MiniStream 2.1.3 · 正式版 / Regular release

这一版集中加固 mslink 隧道、保护本机凭据并收紧口令的传递方式。没有新增媒体格式或性能承诺。Powered by jianhx2026。

## 下载与升级

- **Host**：`MiniStream-Host-Setup.exe`，安装在运行游戏/桌面的电脑。
- **Client**：`MiniStream-Client-Setup.exe`，安装在操作电脑。两端建议配套升级。
- **Source**：`MiniStream-2.1.3-Source.zip`，包含修改版 Moonlight 源码与检出的子模块、mslink、控制台、脚本和交接文档；Qt/外部 SDK/Sunshine 来源见清单。
- **校验**：`SHA256SUMS.txt` 与 `release-manifest.json`。发布清单在安装包编译完成后生成，哈希不能沿用 2.1.2。

先结束当前串流、保存配置，在已有安装目录覆盖升级。安装器识别旧安装路径并保留已有隧道配置和 Sunshine 账号。2.1.3 首次载入旧主控端配置时会把明文口令迁移为当前用户的 `dpapi:v1:` 密文，因此**回退到 2.1.2 及更早版本需要重新填写口令**。新安装要求隧道口令 12–64 字符，升级保留的旧口令仍可用，但建议更换。

## 本轮改进

- **队列与帧有界**：每个连接的发送队列 4 MiB 硬上限，整帧先预留空间再入队，空间不足时整帧拒绝而不是截断；入队和发送失败向上返回，不再静默丢数据。TCP 单帧上限 64 KiB，`FrameReader` 缓冲区有界，读到 8 字节帧头即可拒绝超长帧。
- **握手不阻塞**：被控端最多同时保留 8 个待认证握手，单个 5 秒超时；新会话认证成功后关闭其余 pending，始终只有一个已认证客户端。慢速或异常连接不再拖住已认证会话。
- **重复统计修正**：`seq == highest` 的副本现在计为重复，冗余副本与真实丢包分开统计。
- **测试**：新增队列/帧白盒测试与握手集成测试，CTest 现有四项（`udp_reply_source`、`relay_queue_frame`、`relay_handshake_integration`、`relay_selftest`），本机 4/4 通过；`--password-stdin` 启动脚本实测通过。
- **主控端凭据保护**：连接口令与 Sunshine 口令以当前 Windows 用户的 DPAPI（`dpapi:v1:`）保存，旧明文 ini 由 `QSaveFile` 原子迁移。口令在内存中仍是明文，同一 Windows 用户可解密，**不是端到端保护**。
- **被控端口令传递**：`mslink` 新增 `--password-stdin`，隧道口令由安装脚本经标准输入交给后台，不进入命令行、计划任务 XML 或临时文件；日志不再记录隧道/Sunshine 口令；`mslink-host.ini`、`安装信息.txt` 与安装日志的 ACL 收紧到 SYSTEM / Administrators / 安装用户。
- **文档与网站**：README、CHANGELOG、SECURITY、开发交接、构建与更新说明同步到 2.1.3；旧优化文档加入历史校订提示，不再把自研采集/手柄原型当作当前实现。

## 验证范围

本轮已完成：本机 CTest 4/4、`--password-stdin` 启动实测（argv 不含口令、监听与遥测正常、空 stdin 与参数互斥被拒绝）。这些结果只覆盖**本地回环、白盒与集成测试**。

两端 2.1.3 应用构建、两份 Inno 安装包编译和隔离 UI 启动检查已完成。未覆盖：干净 Win10/11 安装/卸载、所有 GPU/驱动组合、高码率弱网与长时间跨网串流。队列 4 MiB、帧 64 KiB 与 8 pending 上限只限制资源增长，不构成抗 DoS 保证；2.1.2 的真实转发 UDP 修复结论继续沿用，新的真实链路媒体串流仍需单独验收。RTT 不是端到端操作延迟，媒体丢帧不是 IP 丢包率。

自定义隧道认证与密钥交换仍未经过独立安全审计，会话密钥仍通过 TCP 明文传送，不替代加密 VPN。DPAPI 与 ACL 只降低本机文件泄露面：换用户或回退旧版会读不到口令，分享过口令的用户应尽快轮换。请使用可信、受保护的网络路径。

## English

MiniStream 2.1.3 hardens the mslink tunnel, protects local credentials and removes passwords from process command lines. There are no new media formats or performance claims.

**Changes.** Each connection's send queue now has a hard 4 MiB cap; frames are reserved and queued as a whole and enqueue/overflow failures propagate instead of silently dropping bytes. TCP frames are capped at 64 KiB with a bounded frame reader that rejects an oversized frame from its 8-byte header. The host handshake no longer blocks: at most 8 peers wait, each with a 5 s timeout, a newly authenticated session closes the other pending handshakes, and only one authenticated controller is kept. Copies with `seq == highest` are now counted as duplicates. New white-box queue/frame and handshake integration tests bring CTest to four cases (`udp_reply_source`, `relay_queue_frame`, `relay_handshake_integration`, `relay_selftest`); all four passed locally, and the `--password-stdin` startup check passed. The controller stores tunnel and Sunshine passwords with current-user Windows DPAPI (`dpapi:v1:`) and migrates a legacy plaintext ini atomically with `QSaveFile`. The host tunnel accepts `--password-stdin`; the installer passes the tunnel password on standard input, install logs no longer contain tunnel or Sunshine passwords, and ini/install-info/log ACLs are limited to SYSTEM, Administrators and the installing user. New installs require a 12–64 character tunnel password; existing passwords stay valid.

**Upgrade.** End sessions, keep your configuration and install over the existing directories. Because 2.1.3 migrates plaintext controller passwords to DPAPI, downgrading to 2.1.2 or earlier requires entering them again. Rotate any tunnel or Sunshine password that was shared before this release.

**Verification.** Verified locally: CTest 4/4 and the `--password-stdin` startup check. These cover loopback, white-box and integration paths only. Both applications and both installers were built successfully, and isolated application launch checks passed; clean Windows 10/11 installs, all GPU/driver combinations, high-bitrate weak networks and long WAN sessions remain unverified. The 4 MiB queue, 64 KiB frame and 8-pending handshake limits bound resource growth but are not a DoS guarantee. The 2.1.2 forwarded-connection UDP result still applies; a new real-network media session requires its own acceptance.

DPAPI and the ACLs only reduce local file exposure: the same Windows user can still decrypt the values, passwords remain plaintext in memory, the custom authentication still sends a plaintext session key over TCP, and this is not an encrypted VPN. Use a trusted protected path.

[官网 / Website](https://ministream.jianhx.com/) · [教程 / Guide](https://ministream.jianhx.com/tutorial.html) · [GitHub Pages](https://jianhx189.github.io/jianhx-game-stream/) · [Developer handoff](https://github.com/jianhx189/jianhx-game-stream/blob/main/docs/DEVELOPMENT.md)
