# Changelog / 更新记录

## 2.2.0 — 体验与遥测版 / Experience and telemetry

- **主控端工作台重做**：深色界面与折叠侧栏，导航为「我的设备 / 体验监测 / 连接配置 / 串流偏好 / 使用教程」。设备卡片、空状态和小屏布局沿用，新增独立「体验监测」页。
- **体验监测（主控端新增页）**：顶部给出传输通道（`UDP · 低延迟通道`、`TCP · 兼容通道`、`正在探测 UDP`、`等待隧道连接`）与一条经验提示；四张指标卡为隧道 RTT / 抖动、实际渲染帧率（含接收帧率）、视频码率、发送队列（含 4 MiB 上限与重连次数）。趋势区可选 60 秒或 300 秒窗口，分别绘制隧道 RTT、下行吞吐（按 `udp_bytes_down` 差分）和发送队列；另有「视频体验」（网络丢帧、呈现丢帧、分辨率、媒体流 RTT）、「客户端耗时」（解码、帧排队、渲染、主机处理）、链路计数（活动 TCP/UDP 流、本地丢弃、UDP 探测/应答、认证拒绝/回包错误）与最多显示 6 条的连接事件。
- **提示阈值**：发送队列 > 1 MiB、媒体网络丢帧 > 2%、帧排队 > 30 ms、解码 > 16 ms 会给出对应文字提示。这是启发式定位提示，不是体验评分；未触发时显示“当前采样未触发提示阈值”。
- **被控端四页工作台**：`主机概览`（隧道往返延迟、实际渲染帧率、主机发送速率、网络丢帧率、解码耗时、帧排队耗时，以及已认证设备表的设备/传输/时长/状态）、`连接质量`（心跳抖动、活动 TCP/UDP 流、发送队列、隧道重连、本地丢弃、UDP 探测/应答、认证拒绝/回包错误，四张 1 秒采样趋势图，事件表可按「全部级别 / 提示与警告 / 仅警告」筛选）、`媒体会话`（接收帧率、实际渲染帧率、视频码率、分辨率、网络丢帧、解码/排队/渲染耗时、主机处理）、`服务与诊断`（服务启停、端口配置、事件与日志、导出诊断）。
- **遥测有界**：隧道在内存中保留最近 **300 个约 1 秒采样点** 和 **100 条事件**；同一秒内不重复采样，长时间跳变只补一个点；同类别同文案事件 1 秒内去重，单条文案截断到 163 字节并去除换行。实时遥测 JSON 不含历史且小于 64 KiB，历史/事件 JSON 小于 128 KiB，两者都不写盘；`mslink-status.jsonl` 仍按约 1 MiB × 2 轮转。
- **测试**：新增 `relay_telemetry`（历史与事件上限、采样节流、JSON 契约、事件不含口令/设备名/对端报告、媒体开始与结束事件），CTest 共七项，本机 7/7 通过。
- **弱网端到端**：`tools/mslink_weaknet_e2e.py` 以真实 `mslink.exe`、假 Sunshine 和 127.0.0.1 上的双侧对称损伤代理运行 7 个场景（`baseline`、`rtt100_loss2`、`limit5`、`udp_down`、`recover`、`outage30`、`duplex`），共 50 项检查全部通过。判物理 UDP 只看代理 UDP data 包增量，不用 TCP 字节推断。
- **弱网测试的边界**：回环单机、对称损伤、自造 UDP 回显数据而非 H.264/Opus 媒体、无 GPU 解码与呈现、不模拟 AQM、IP 分片、MTU、NAT 超时和运营商突发丢包，也不验证 Moonlight 的 FEC/NACK。它证明的是隧道在受控损伤下的 UDP 保活、回退、恢复与重连行为，不是真实跨公网游戏体验。
- **未执行的验收**：本轮没有运行 Windows 10/11 虚拟机安装（无 Windows ISO，当前会话非管理员；Hyper-V 可用但未创建虚拟机）。不宣称干净 Win10/11、所有 GPU/驱动组合或真实长时串流通过。
- **历史修复保留**：2.1.3 存在「`mslink.ini` 不存在时保存必定失败」与「`mslink` 启动失败残留监听 socket」两处回归，2.1.4 已修复；2.2.0 沿用 2.1.3 引入的隧道队列/帧/握手加固、DPAPI 凭据、`--password-stdin`、文件 ACL 与多网卡 UDP 修复。2.1.3 仍不建议用于全新安装，2.1.4 是上一个已发布版本。
- **发布状态**：2.2.0 的主控端、被控端、两份安装包与源码包已在本轮构建；具体哈希与线上资产以 v2.2.0 Release 和同版 `release-manifest.json` 为准。

English: Reworks the controller workspace into a dark shell with device, experience, connection, preference and help entries, and adds a real "experience" page (transport badge, RTT/jitter, rendered and received frame rate, bitrate, bounded send queue, 60/300-second RTT, downlink-throughput and queue trends, media drop/pacing/resolution/stream RTT, decode/queue/render/host timings, link counters and recent events). Heuristic hints fire above 1 MiB queued, 2% media drop, 30 ms queueing or 16 ms decoding and are explicitly not a score. The host console gains four pages (overview, connection quality, media session, service and diagnostics) fed by the same 1-second telemetry, with a level filter on events. Telemetry stays in memory: at most 300 one-second samples and 100 events, samples throttled to one per second, events deduplicated per category/message within a second and truncated to 163 bytes; live telemetry JSON excludes history and stays under 64 KiB, history JSON stays under 128 KiB, and nothing new is written to disk. A new `relay_telemetry` test brings CTest to seven cases, all passing locally. A weak-network harness drives the real binaries through a symmetric impairing proxy on 127.0.0.1 across seven scenarios (baseline, 100 ms RTT with 2% loss, 5 Mbit/s shaping, UDP blocked, recovery, 30 s outage, duplex) and passes 50/50 checks, using proxy UDP data counts rather than TCP bytes to prove physical UDP; the echo payload is synthetic, decoding and presentation are not exercised, and AQM/MTU/NAT behaviour is not modelled. No Windows 10/11 virtual machine run was performed (no ISO, non-admin session), and clean-machine, all-GPU and long real-session behaviour are still not verified. The 2.1.3 regressions (first controller save, leaked listening sockets on failed start) remain fixed as shipped in 2.1.4, whose hardening is unchanged here. The 2.2.0 applications, host console and both installers were rebuilt in this round; hashes and online assets are recorded by the v2.2.0 release and its manifest.

## 2.1.4 — 热修复版 / Hotfix

- **修复 2.1.3 回归（首次连接保存）**：主控端首次连接时 `mslink.ini` 尚不存在，保存连接必定失败，之后每次都要重新填写。2.1.4 在文件不存在时同样通过 `QSaveFile` 原子创建配置，保存一次即可保留。
- **修复 2.1.3 回归（启动失败）**：`mslink` 启动失败会泄漏已创建的监听 socket，带 47990 的隧道启动失败后回退重试可能二次 bind 并卡住首次连接。2.1.4 在失败路径关闭全部已创建 socket 并调用 `WSACleanup`，同时校验 listen/bind 地址必须为 IPv4。
- **空口令不再破坏凭据**：空口令直接拒绝，不会删除已保存的 `dpapi:v1:` 密文。
- **上游失败不再等超时**：上游 Sunshine 目标连接失败时发送 `StreamClose`，客户端立即结束对应流，而不是一直等待超时。
- **测试与构建**：2.1.3 的基线为本机 CTest 4/4 与 `--password-stdin` 启动实测；2.1.4 新增 `relay_startup`（启动失败清理与重试）和 `relay_stream_open_failure`（上游失败返回 `StreamClose`），CTest 共六项，并重建全部组件。最终通过数、构建与安装包结果以发布清单记录为准，本说明不预先宣称已通过。
- **安装建议**：2.1.3 存在上述回归，**不建议用于全新安装**；全新安装请使用 2.1.4，已安装 2.1.3 的用户覆盖升级即可，配置、隧道与 DPAPI 凭据保留。
- 其余 2.1.3 能力（隧道队列/帧/握手加固、DPAPI 凭据、`--password-stdin`、ACL、多网卡 UDP 修复）在 2.1.4 中保持不变。

English: Hotfix for two 2.1.3 regressions: saving a controller connection failed whenever `mslink.ini` did not exist (2.1.4 creates it atomically), and a failed `mslink` start leaked listening sockets so the 47990 fallback retry could bind twice and hang the first connection (failure paths now close every socket, call `WSACleanup` and validate IPv4 listen/bind addresses). An empty password is rejected without deleting stored `dpapi:v1:` ciphertext, and a failed upstream Sunshine connection now sends `StreamClose` instead of leaving the controller to time out. 2.1.4 adds `relay_startup` (failure cleanup, IPv4 checks and a retry on the same ports) and `relay_stream_open_failure` (upstream failure returns `StreamClose`), bringing CTest to six cases, and rebuilds all components; final pass counts and build/installer results are recorded in the release manifest and are not claimed here. 2.1.3 should not be used for a fresh install; 2.1.3 users should upgrade in place and keep their configuration.

## 2.1.3 — 正式版 / Regular release

> 已知回归：该版本在 `mslink.ini` 不存在时保存主控端连接必定失败，且 `mslink` 启动失败会残留监听 socket（47990 回退可能二次 bind 卡住首次连接）。**不建议用于全新安装**；请使用 2.1.4，已安装用户覆盖升级。

- 隧道健壮性：每个连接的发送队列改为 **4 MiB 硬上限**，整帧先预留空间再入队，空间不足时整帧拒绝而不是截断；入队与发送失败一律向上返回，不再静默丢数据。
- TCP 帧上限 **64 KiB**，`FrameReader` 缓冲区有界；读到 8 字节帧头即可判定超长帧并断开，不再按对端声明无限增长。
- 被控端握手改为**非阻塞**：最多 8 个等待中的握手、5 秒超时；新会话认证成功后关闭其余 pending，仍然只保留一个已认证客户端。
- 修正 `seq == highest` 的重复副本未被计数的问题，冗余副本与真实丢包的统计分开。
- 新增 CTest 队列/帧与握手集成测试，共 4 项（`udp_reply_source`、`relay_queue_frame`、`relay_handshake_integration`、`relay_selftest`），本机 4/4 通过；`--password-stdin` 启动脚本实测全部通过。
- 主控端连接口令与 Sunshine 口令改用当前 Windows 用户的 DPAPI（`dpapi:v1:`）存储，旧明文 INI 由 `QSaveFile` 原子迁移；口令在内存中仍为明文，同一用户的进程可解密，**不是端到端保护**；回退旧版本需重新填写口令。
- 被控端 `mslink` 新增 `--password-stdin`，隧道口令由安装脚本经标准输入传递，不进入命令行、计划任务 XML 或临时文件；安装日志不再记录隧道/Sunshine 口令。
- 新安装要求隧道口令 **12–64 字符**（旧口令升级时保留并提示更换）；`mslink-host.ini`、`安装信息.txt`、安装日志的 ACL 收紧到 SYSTEM / Administrators / 安装用户。
- 文档与官网源文件同步到 2.1.3（`web/dist` 由发布流程重新生成）；`docs/DEVELOPMENT.md` 补充本轮不变量并修正旧 MTU 描述，`docs/RELEASE_NOTES.md` 改为 2.1.3 双语说明。
- 验证范围：本地回环 CTest 4/4 与 `--password-stdin` 启动实测。干净 Win10/11 安装、所有 GPU/驱动组合、高码率弱网长时串流**尚未验证**；两端最终构建与安装包编译以发布清单生成时的记录为准。

English: Hardens the tunnel (4 MiB per-connection queue hard cap with whole-frame atomic enqueue and propagated failures, 64 KiB TCP frame cap with a bounded FrameReader, a non-blocking host handshake with at most 8 pending peers and a 5 s timeout, and correct counting of `seq == highest` duplicates), adds queue/frame and handshake integration tests (CTest 4/4 locally), protects client credentials with current-user DPAPI plus an atomic INI migration, and moves host passwords to `--password-stdin` with restricted file ACLs and no passwords in install logs. The custom key exchange and plaintext session key remain; this is not a full encrypted VPN. Verification is limited to local loopback and CTest; clean-machine, all-GPU, high-bitrate weak-network and final build/installer checks remain outstanding.

## 2.1.2 — 正式版 / Regular release

- 修复多网卡 UDP 回包源地址变化：使用 `WSARecvMsg` / `WSASendMsg`、`IP_PKTINFO` 对 ACK 和媒体 DATA 保持目标地址及网卡，真实转发连接已验证恢复 UDP。
- 新增被控端 HELLO、ACK、会话拒绝、回包错误和固定路径计数；首次探测计入发送数。空闲但探测有效时不再仅因缺少媒体流显示探测中。
- 纳入此前预览版的递增 UDP 序号、认证后滑动窗口、真实 ACK 恢复、真实客户端与媒体遥测。
- 将原创图标、主控端工作区、原生被控端控制台、小屏适配和 VC143 运行库部署纳入正式发行。
- 控制台直接从 `console` 子目录运行时自动识别上级安装根目录；升级识别已有安装路径并保留原隧道配置。已有 Sunshine 账号不再被错误记录为随机新密码。
- 中英文介绍、使用/排障/开发交接、构建、安全与贡献文档；官网教程、英文页与更新说明，同步 Cloudflare 和 GitHub Pages。
- 程序内版本与安装版本统一为 2.1.2，更新检查改用本项目清单，避免提示用户升级成上游原版程序。
- 验证：源地址测试 60/60、UDP 突发 100/100、CMake/CTest。干净系统安装、所有驱动组合与长期游戏串流仍需继续验收。

English: Promotes the desktop UI and host console, fixes UDP replies leaving the wrong interface, improves upgrade/runtime handling, and adds bilingual documentation and release handoff. The forwarding fix was verified on the affected connection; this does not mean all streaming/input issues are resolved.

## 2.1.1-preview / 2.1.0-preview

界面、控制台、遥测、图标、小屏布局和运行库部署预览。保留历史 Release 供版本核对。

## 2.0.5

此前正式版，包含交付包脱敏与连接相关修正。新部署建议使用最新正式版。
