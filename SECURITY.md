# Security / 安全说明

MiniStream 面向自有或已获授权的设备。mslink 的自定义认证、UDP 标签、防重放和密钥交换尚未经过独立安全审计；当前 TCP 握手会传送会话密钥，**不能视为完整的端到端加密 VPN 或零信任接入**。认证成功只证明口令正确，不代表整条路径具备机密性。

## 2.1.3 / 2.1.4 的本地保护边界

2.1.3 起增加了本地凭据与进程参数的防护，2.1.4 保持相同边界。这些措施只降低“本机文件或命令行被同机其他进程看到”的风险，不改变网络层的机密性：

- **主控端凭据（DPAPI）**：连接口令与 Sunshine 口令以 `dpapi:v1:` 前缀保存在当前 Windows 用户的 `mslink.ini`。旧版明文由 `QSaveFile` 原子迁移，失败时保留原文件并提示。DPAPI 是**用户范围**保护：同一 Windows 用户（以及能加载该用户配置的管理员或离线攻击者）仍可解密；口令在程序内存中仍是明文；它不是端到端加密，也不保护传输。
- **回退与迁移**：迁移后旧版程序读不到口令，回退到 2.1.2 及更早版本需要重新填写。换 Windows 用户或重装系统后，原密文可能无法解密，同样需要重新填写。
- **被控端口令传递**：`mslink` 支持 `--password-stdin`，安装脚本把隧道口令经标准输入交给后台，口令不进入进程命令行、计划任务 XML 或临时文件。手工使用 `--password` 仍会暴露在命令行，程序会给出警告。
- **日志与文件 ACL**：安装日志不再记录隧道或 Sunshine 口令；`mslink-host.ini`、`安装信息.txt` 与安装日志的 ACL 收紧为仅 SYSTEM、Administrators 和安装用户。管理员仍可读取这些文件，`安装信息.txt` 本身就含口令，分享目录前必须删除。
- **资源上限**：发送队列 4 MiB 硬上限、TCP 单帧 64 KiB、有界帧读取与被控端 8 个 pending / 5 秒握手超时用于限制内存增长和慢速握手占用，属于健壮性措施，不能防止凭据泄露或认证绕过。

## 使用建议

使用可信的加密网络或受保护的转发路径、独立强凭据，并仅开放必要服务。2.1.3 起新安装要求隧道口令 12–64 字符；升级保留的旧口令应尽快更换。**曾通过聊天、工单、截图、日志或旧版安装信息分享过隧道口令或 Sunshine 管理口令的，请立即轮换**，并确认旧口令不能继续连接。不要复用其他服务的口令，也不要把 Sunshine 管理页面暴露到公网。

普通问题可提交 Issues，包含版本、环境、精确时间和脱敏复现。涉及认证绕过或密钥泄露时优先使用 GitHub 私密漏洞报告入口（如已启用）；否则先提交不含利用细节的联系请求。不要公开有效凭据、令牌、私钥、完整配置或原始转储。

导出器的自动脱敏并不完整；分享前检查域名、IPv6、设备名、安装信息和日志中的其他标识。

## English

Use only authorized devices. The custom authentication, UDP tags, replay protection and key exchange are not independently audited. The TCP handshake transfers a session key; mslink must not be treated as a complete encrypted VPN or zero-trust system.

2.1.3 and 2.1.4 harden local handling only. Controller passwords are stored with current-user Windows DPAPI (`dpapi:v1:`) and legacy plaintext INIs are migrated atomically, but the same Windows user (or an administrator able to load that profile) can still decrypt them, values remain plaintext in memory, and network traffic is not protected end to end. Downgrading to 2.1.2 or earlier, or changing the Windows user/machine, requires re-entering the passwords. The host tunnel accepts `--password-stdin`; the installer passes the tunnel password on standard input so it stays out of process command lines, scheduled-task XML and temporary files. Install logs no longer contain tunnel or Sunshine passwords, and ini/install-info/log ACLs are limited to SYSTEM, Administrators and the installing user. Resource bounds (4 MiB send queue, 64 KiB TCP frames, a bounded frame reader and a non-blocking handshake with 8 pending peers and a 5 s timeout) limit memory growth and slow-handshake abuse; they do not prevent credential theft or authentication bypass.

Use a trusted protected path, independent strong credentials and only the ports you need. Rotate any tunnel or Sunshine password that was shared through chat, tickets, screenshots, logs or an older install-info file, and keep Sunshine management private.

Prefer GitHub private vulnerability reporting when available; otherwise request a private contact without exploit details. Never post working credentials, keys, raw configuration or unredacted dumps. Review diagnostics manually before sharing.
