# Contributing / 参与维护

先阅读 [开发交接](docs/DEVELOPMENT.md)、[构建](docs/BUILD.md) 和 [安全说明](SECURITY.md)。当前主控端是 Moonlight-Qt 覆盖层，不要误用早期根 CMake 原型。

问题报告包含两端版本、Windows/缩放、GPU/驱动、连接方式、实际传输模式、精确时间、复现步骤和脱敏诊断。鼠标问题还需注明画面/声音是否继续、桌面还是游戏、焦点/菜单与鼠标模式。

修改传输代码运行 CMake/CTest；认证、序号和状态机改动应覆盖负面路径。界面改动检查小屏、空状态、过期状态和文字溢出。发布前核对真实安装包输入和 SHA-256，不能只改版本名或复用旧二进制。

PR 说明触发条件、改变后的行为、验证和限制。不要提交个人配置、日志、转储、令牌、密钥或生成缓存。保留上游署名与许可。

English: Start with the development/build guides. Provide versions, environment, exact reproduction time, transport mode and redacted evidence. Run relevant tests and check affected UI states. Explain the problem, behavior, verification and limits in PRs. Keep secrets/generated caches out of commits and preserve upstream licensing.
