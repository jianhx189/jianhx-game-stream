# Changelog / 更新记录

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
