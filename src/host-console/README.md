# MiniStream 被控端控制台

Qt 6 Widgets 原生 Windows 程序。通过 GetExtendedTcpTable 只读检查本机监听，不发送网络探测包，不代表公网连通。

- 概览：Sunshine HTTP / mslink TCP 监听状态。
- 管理：启动、停止、重启限定安装目录内的服务；通过 gui-action.ps1 申请 UAC，停止或重启前提示中断风险。
- 设置：保存隧道端口和 Sunshine ping_timeout，保留其他配置并备份原文件；保存后需手动重启与同步网络配置。
- 诊断：截取日志末尾并隐藏凭据行及 IPv4，可导出状态和脱敏日志。

运行 `MiniStreamHost.exe --root <已安装被控端目录>`。root 必须包含 mslink.exe、sunshine/sunshine.exe 与 _internal/gui-action.ps1；独立打开控制台只能查看本机端口。运行日志可能包含设备信息，分享前仍需人工检查。

使用 Qt 6.9.3 / MSVC 2022，qmake host-console.pro 后 nmake。中文路径下 Windows RC 需 code_page(65001)，或使用 ASCII 构建目录。需要与编译工具链匹配的 VC143 CRT。

不替用户自动修改防火墙，不扫描公网，不重置配对，不展示密码；高级 Sunshine 配置由侧栏打开本机管理页面。
