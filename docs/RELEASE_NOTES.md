# MiniStream 2.1.4 · 热修复版 / Hotfix

2.1.4 修复 2.1.3 发布后发现的两处回归，并补上两处会影响首次使用和失败恢复的问题。没有新增媒体格式或性能改动。Powered by jianhx2026。

## 下载与升级

- **Host**：`MiniStream-Host-Setup.exe`，安装在运行游戏/桌面的电脑。
- **Client**：`MiniStream-Client-Setup.exe`，安装在操作电脑。两端建议配套升级。
- **Source**：`MiniStream-2.1.4-Source.zip`，包含修改版 Moonlight 源码与检出的子模块、mslink、控制台、脚本和交接文档；Qt/外部 SDK/Sunshine 来源见清单。
- **校验**：`SHA256SUMS.txt` 与 `release-manifest.json`。哈希在 2.1.4 安装包编译完成后生成，不能沿用 2.1.3。

**2.1.3 用户**：请结束串流后覆盖升级到 2.1.4。配置、隧道口令、Sunshine 配对和 `dpapi:v1:` 凭据都会保留，不需要重新配对。**全新安装请只使用 2.1.4**：2.1.3 在主控端首次保存连接和 `mslink` 启动失败清理上存在回归，不建议用于新部署。回退到 2.1.2 及更早版本仍需重新填写主控端口令。

## 本轮修复

- **首次保存失败（2.1.3 回归）**：主控端首次连接时 `mslink.ini` 还不存在，保存连接必定失败，导致之后每次连接都要重填。2.1.4 在配置文件不存在时也通过 `QSaveFile` 原子创建，保存一次即可保留。
- **启动失败泄漏 socket（2.1.3 回归）**：`mslink` 启动失败时未清理已创建的监听 socket；带 47990 的隧道启动失败后回退为三个标准端口重试，可能在同一进程内二次 bind 并挂住首次连接。2.1.4 在失败路径关闭全部已创建 socket 并调用 `WSACleanup`，同时校验 listen/bind 地址必须为 IPv4，非 IPv4 直接报错。
- **空口令不再删除凭据**：空口令会被拒绝，已保存的 `dpapi:v1:` 密文保持不变，不会因为一次空提交而丢失连接配置。
- **上游失败发送 `StreamClose`**：上游 Sunshine 目标连接失败时主动发送 `StreamClose`，客户端立即结束对应流，不再一直等待超时。
- **测试与构建**：2.1.3 的基线为本机 CTest 4/4 与 `--password-stdin` 启动实测；2.1.4 增加 `relay_startup`（启动失败清理与重试）和 `relay_stream_open_failure`（上游失败返回 `StreamClose`），CTest 共六项，并在发布前重建两端应用与安装包。2.1.4 的最终测试、构建与安装包结果以发布清单记录为准，本文不预先宣称已通过。
- 2.1.3 的隧道加固、DPAPI 凭据、`--password-stdin`、ACL 与多网卡 UDP 修复在 2.1.4 中保持不变。

## 验证范围

2.1.3 已完成本机 CTest 4/4、`--password-stdin` 启动实测、两端应用与安装包构建。2.1.4 增加 `relay_startup` 与 `relay_stream_open_failure` 测试并重建全部组件；**最终结果以同版本 `release-manifest.json` 与发布记录为准**，本文不预先给出通过结论。

仍未覆盖：干净 Win10/11 安装/卸载、所有 GPU/驱动组合、高码率弱网与长时间跨网串流。4 MiB 队列、64 KiB 帧与 8 pending 握手上限只限制资源增长，不构成抗 DoS 保证；2.1.2 的真实转发 UDP 修复结论继续沿用。RTT 不是端到端操作延迟，媒体丢帧不是 IP 丢包率。

自定义隧道认证与密钥交换仍未经过独立安全审计，会话密钥仍通过 TCP 明文传送，不替代加密 VPN。DPAPI 与 ACL 只降低本机文件泄露面：换用户或回退旧版会读不到口令，分享过口令的用户应尽快轮换。请使用可信、受保护的网络路径。

## English

MiniStream 2.1.4 is a hotfix for two regressions found after 2.1.3 and two related failure-handling issues. There are no new media formats or performance claims.

**Downloads.** Host and Client are installed on the gaming PC and the PC you operate; use matching versions. The source bundle contains the modified Moonlight checkout and its submodules plus the mslink tunnel, host console, scripts and handoff documents. Verify against `SHA256SUMS.txt` and `release-manifest.json`; 2.1.4 hashes are generated after the installers are rebuilt and cannot be reused from 2.1.3.

**Upgrade.** Existing 2.1.3 users should upgrade in place: configuration, tunnel password, Sunshine pairing and `dpapi:v1:` credentials are preserved and no re-pairing is required. Use 2.1.4 for every fresh install — 2.1.3 should not be used for new deployments because of the first-save and startup-cleanup regressions. Downgrading to 2.1.2 or earlier still requires re-entering controller passwords.

**Fixes.** 2.1.4 creates `mslink.ini` atomically when it does not exist, so the first controller save succeeds and the password persists. Failed `mslink` starts now close every created listening socket and call `WSACleanup`, validate that listen/bind addresses are IPv4, and therefore keep the 47990 fallback retry from double-binding and hanging the first connection. Empty passwords are rejected without deleting the stored ciphertext, and a failed upstream Sunshine connection sends `StreamClose` instead of leaving the controller to time out.

**Verification.** 2.1.3 was verified locally with CTest 4/4, the `--password-stdin` startup check and rebuilt applications/installers. 2.1.4 adds `relay_startup` (failure cleanup and retry) and `relay_stream_open_failure` (upstream failure returns `StreamClose`), bringing CTest to six cases, and rebuilds all components; final test, build and installer results are recorded in the release manifest and are **not** claimed here. Clean Windows 10/11 installs, all GPU/driver combinations, high-bitrate weak networks and long WAN sessions remain unverified. The tunnel limits bound resource growth but are not a DoS guarantee. 2.1.3's DPAPI, ACL and hardening behaviour is unchanged in 2.1.4: local files are still decryptable by the same Windows user, passwords stay plaintext in memory, the session key is sent in plaintext over TCP, and this is not an encrypted VPN.

[官网 / Website](https://ministream.jianhx.com/) · [教程 / Guide](https://ministream.jianhx.com/tutorial.html) · [GitHub Pages](https://jianhx189.github.io/jianhx-game-stream/) · [Developer handoff](https://github.com/jianhx189/jianhx-game-stream/blob/main/docs/DEVELOPMENT.md)
