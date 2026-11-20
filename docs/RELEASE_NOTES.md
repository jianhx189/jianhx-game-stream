# MiniStream 2.3.0 · 被控端控制台

**为中国玩家打造，让远程游戏串流更流畅。**

- 被控端首页提供「局域网连接」和「FRP 穿透连接」。局域网自动选址，FRP 按输入的公网 IPv4 验证真实 TCP 本机会话与 UDP 回程后，生成浏览器邀请链接。公网映射端口可分别覆盖。
- Windows 主控端支持浏览器唤起和已有实例接收邀请。仅私发链接，首次 Sunshine 配对仍需授权。
- 主控端自动检测 GitHub 正式版。新版安装器识别旧版、确认更新并显示实际文件部署进度。
- 在控制台更换隧道口令。已运行的隧道会短暂重启并检查两条监听。更换后先在主控端更新口令，再重新连接。
- 「运行本机自检」核对 Sunshine 响应、TCP/UDP 监听归属、Windows 防火墙和登录自启。有独立的防火墙规则修复按钮。公网可达性和双向 UDP 请从另一台电脑测试。
- 关闭窗口时控制台收起到 Windows 托盘，后台服务继续运行。点击托盘图标或再次打开快捷方式可恢复窗口。

## 升级与校验

先结束当前串流，再在已有目录覆盖升级。安装器保留已有的隧道与 Sunshine 配置。下载后用 PowerShell `Get-FileHash -Algorithm SHA256` 对照 `SHA256SUMS.txt`。验证范围见同版本的 `release-manifest.json`。

## English

The Host Console adds LAN and FRP setup templates, a copyable connection summary, and tunnel password changes. A local self-check reports Sunshine, TCP and UDP listeners, firewall rules, and logon startup. The console minimizes to the Windows tray while streaming services keep running. Upgrade in the existing installation directory, and update the controller password after changing it on the host.

[中文使用教程](https://ministream.jianhx.com/tutorial.html) · [English project overview](https://ministream.jianhx.com/en.html)
