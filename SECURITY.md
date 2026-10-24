# Security / 安全说明

MiniStream 面向自有或已获授权的设备。mslink 的自定义认证、UDP 标签、防重放和密钥交换尚未经过独立安全审计；当前 TCP 握手会传送会话密钥，**不能视为完整的端到端加密 VPN 或零信任接入**。认证成功只证明口令正确，不代表整条路径具备机密性。

## 2.1.3 起的本地保护边界（2.2.0 沿用）

2.1.3 起增加了本地凭据与进程参数的防护，2.1.4 修复了该轮回归，2.2.0 保持相同边界。这些措施只降低“本机文件或命令行被同机其他进程看到”的风险，不改变网络层的机密性：

- **主控端凭据（DPAPI）**：连接口令与 Sunshine 口令以 `dpapi:v1:` 前缀保存在当前 Windows 用户的 `mslink.ini`。旧版明文由 `QSaveFile` 原子迁移，失败时保留原文件并提示。DPAPI 是**用户范围**保护：同一 Windows 用户（以及能加载该用户配置的管理员或离线攻击者）仍可解密；口令在程序内存中仍是明文；它不是端到端加密，也不保护传输。
- **回退与迁移**：迁移后旧版程序读不到口令，回退到 2.1.2 及更早版本需要重新填写。换 Windows 用户或重装系统后，原密文可能无法解密，同样需要重新填写。
- **被控端口令传递**：`mslink` 支持 `--password-stdin`，安装脚本把隧道口令经标准输入交给后台，口令不进入进程命令行、计划任务 XML 或临时文件。手工使用 `--password` 仍会暴露在命令行，程序会给出警告。
- **日志与文件 ACL**：安装日志不再记录隧道或 Sunshine 口令；`mslink-host.ini`、`安装信息.txt` 与安装日志的 ACL 收紧为仅 SYSTEM、Administrators 和安装用户。管理员仍可读取这些文件，`安装信息.txt` 本身就含口令，分享目录前必须删除。
- **资源上限**：发送队列 4 MiB 硬上限、TCP 单帧 64 KiB、有界帧读取与被控端 8 个 pending / 5 秒握手超时用于限制内存增长和慢速握手占用，属于健壮性措施，不能防止凭据泄露或认证绕过。

## 2.2.0 遥测与诊断的边界

2.2.0 新增主控端体验监测、被控端四页工作台和遥测历史/事件。它们扩大的是**本机可读的信息面**，不是网络暴露面：

- **有界且只驻留内存**：隧道最多保留 300 个约 1 秒采样点和 100 条事件，采样 1 秒一次、事件同类别同文案 1 秒去重、单条截断到 163 字节；实时遥测 JSON 不含历史且小于 64 KiB，历史 JSON 小于 128 KiB。上限只限制内存增长，**不构成抗 DoS 保证**。
- **不新增端口**：遥测复用已认证的 TCP 隧道（类型 11，单条上限 8 KiB），没有新增公网管理口，也不能绕过认证读取。
- **落盘内容不变**：`mslink-status.json` / `mslink-status.jsonl` 仍是本机状态文件（含设备名、地址类信息），按约 1 MiB × 2 轮转；同一 Windows 用户和管理员可读。分享前必须脱敏。
- **事件不含口令**：`relay_telemetry` 测试断言事件与遥测 JSON 不出现口令、设备名和对端报告；这是当前实现的回归保护，不是对所有日志路径的审计结论。
- **导出仍需人工复核**：主控端“导出脱敏诊断”和被控端“导出诊断”只做有限脱敏，域名、IPv6、设备名、路径和版本组合仍可能识别部署。

## 使用建议

使用可信的加密网络或受保护的转发路径、独立强凭据，并仅开放必要服务。2.1.3 起新安装要求隧道口令 12–64 字符；升级保留的旧口令应尽快更换。**曾通过聊天、工单、截图、日志或旧版安装信息分享过隧道口令或 Sunshine 管理口令的，请立即轮换**，并确认旧口令不能继续连接。不要复用其他服务的口令，也不要把 Sunshine 管理页面暴露到公网。分享遥测曲线、事件表或诊断包前，先确认其中是否含设备名与内网地址。

普通问题可提交 Issues，包含版本、环境、精确时间和脱敏复现。涉及认证绕过或密钥泄露时优先使用 GitHub 私密漏洞报告入口（如已启用）；否则先提交不含利用细节的联系请求。不要公开有效凭据、令牌、私钥、完整配置或原始转储。

导出器的自动脱敏并不完整；分享前检查域名、IPv6、设备名、安装信息和日志中的其他标识。

## English

Use only authorized devices. The custom authentication, UDP tags, replay protection and key exchange are not independently audited. The TCP handshake transfers a session key; mslink must not be treated as a complete encrypted VPN or zero-trust system.

2.1.3 and 2.1.4 harden local handling only; 2.2.0 keeps the same boundary. Controller passwords are stored with current-user Windows DPAPI (`dpapi:v1:`) and legacy plaintext INIs are migrated atomically, but the same Windows user (or an administrator able to load that profile) can still decrypt them, values remain plaintext in memory, and network traffic is not protected end to end. Downgrading to 2.1.2 or earlier, or changing the Windows user/machine, requires re-entering the passwords. The host tunnel accepts `--password-stdin`; the installer passes the tunnel password on standard input so it stays out of process command lines, scheduled-task XML and temporary files. Install logs no longer contain tunnel or Sunshine passwords, and ini/install-info/log ACLs are limited to SYSTEM, Administrators and the installing user. Resource bounds (4 MiB send queue, 64 KiB TCP frames, a bounded frame reader and a non-blocking handshake with 8 pending peers and a 5 s timeout) limit memory growth and slow-handshake abuse; they do not prevent credential theft or authentication bypass.

2.2.0 adds the controller Experience page, four host console pages and bounded telemetry history, which widen only what the local user can read. History and events stay in memory: at most 300 one-second samples and 100 events, sampling throttled to once per second, events deduplicated per category and message within a second and truncated to 163 bytes; live telemetry JSON excludes history and stays below 64 KiB, history JSON below 128 KiB. Telemetry reuses the authenticated TCP tunnel (type 11, 8 KiB per message) and opens no new port. The existing `mslink-status.json`/`.jsonl` files remain local, include device names and rotate at about 1 MiB × 2; the `relay_telemetry` test asserts that events and telemetry JSON carry no passwords, device names or peer reports, but that is regression coverage, not an audit of every log path. Diagnostic exports redact only partially — check domains, IPv6 addresses, device names and paths before sharing curves, event tables or reports. The bounds limit memory growth; they are not a denial-of-service guarantee.

Use a trusted protected path, independent strong credentials and only the ports you need. Rotate any tunnel or Sunshine password that was shared through chat, tickets, screenshots, logs or an older install-info file, and keep Sunshine management private.

Prefer GitHub private vulnerability reporting when available; otherwise request a private contact without exploit details. Never post working credentials, keys, raw configuration or unredacted dumps. Review diagnostics manually before sharing.
