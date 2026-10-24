# Changelog / 更新记录

## 2.1.3 — 正式版 / Regular release

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
