# Security / 安全说明

MiniStream 面向自有或已获授权的设备。mslink 自定义认证、UDP 标签、防重放和密钥交换尚未经过独立安全审计；当前 TCP 握手会传送会话密钥，**不能视为完整的端到端加密 VPN 或零信任接入**。认证成功不代表整条路径具备机密性。

使用可信的加密网络或受保护的转发路径、独立强凭据，并仅开放必要服务。Sunshine 管理页面应留在受控网络，项目不提供公共服务器。

普通问题可提交 Issues，包含版本、环境、精确时间和脱敏复现。涉及认证绕过或密钥泄露时优先使用 GitHub 私密漏洞报告入口（如已启用）；否则先提交不含利用细节的联系请求。不要公开有效凭据、令牌、私钥、完整配置或原始转储。

导出器的自动脱敏并不完整；分享前检查域名、IPv6、设备名和其他标识。

## English

Use only authorized devices. The custom authentication, UDP tags, replay protection and key exchange are not independently audited. The TCP handshake transfers a session key; mslink must not be treated as a complete encrypted VPN or zero-trust system. Use a trusted protected path and keep Sunshine management private.

Prefer GitHub private vulnerability reporting when available; otherwise request a private contact without exploit details. Never post working credentials, keys, raw configuration or unredacted dumps. Review diagnostics manually before sharing.
