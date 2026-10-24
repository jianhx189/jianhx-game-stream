# 当前版本构建指南 / Current build guide

本文适用于 2.2.0。仓库早期根 CMake 客户端不是当前发布主控端；旧原型的构建方式见 `文档/历史构建指南-旧原型.md`，维护者应以本文和 DEVELOPMENT 为准。

## 版本与组件

2.2.0 需要同时改到位的版本位置：主控端 `app/version.txt`、被控端控制台 `app.setApplicationVersion("2.2.0")`、两份 Inno 脚本的 `AppVersion`，以及 CHANGELOG、文档和网站。任何一处漏改都会让“关于/安装器/更新检查”显示不同版本。

2.2.0 的构建输入与 2.1.4 相同（主控端覆盖层 + 根 mslink + Qt Widgets 控制台 + Inno 脚本），变化集中在主控端界面（新增 `gui/ExperienceView.qml`、`gui/TrendChart.qml`、`gui/MetricCard.qml` 并登记在 `qml.qrc`）、隧道遥测历史/事件和被控端四页控制台。**本轮没有重新编译 2.2.0 安装包**，仓库当前 Release 仍是 2.1.4；打包前必须按本文重新编译两端，不能用旧包推断新版本内容。

## Moonlight 主控端

准备 Windows x64、VS2022 C++/Windows SDK、Qt 6.9.3 msvc2022_64、Git。用 ASCII 工作路径便于 Windows 工具链处理。

```powershell
git clone --recursive https://github.com/moonlight-stream/moonlight-qt.git C:\MiniStreamBuild\moonlight-qt
git -C C:\MiniStreamBuild\moonlight-qt checkout dc92b6d2f361064d3e9e53ce5de92f611db77220
git -C C:\MiniStreamBuild\moonlight-qt submodule update --init --recursive
```

将本项目 `src/moonlight-qt/app` 覆盖到该树 `app`，将 `src/mslink` 放到其根 `mslink`。在完整树运行 `setup-deps.ps1` 获取锁定的 v19 Windows 依赖。首次从完整 `moonlight-qt.pro` 构建，保证 qmdnsengine、moonlight-common-c、h264bitstream、AntiHooking 都先生成；不要只复制最终 EXE。

在 **VS x64 Native Tools 命令提示符**中（以下 qmake/windeployqt 使用实际 Qt 路径）：

```bat
mkdir build
cd build
D:\Qt\6.9.3\msvc2022_64\bin\qmake.exe ..\moonlight-qt.pro -spec win32-msvc CONFIG+=release
nmake /NOLOGO
```

本机已有静态库，因此增量构建使用 `D:\MiniStreamDev\build` 对 `..\app\app.pro` qmake 后 `nmake /f Makefile.Release`。产物 `build/release/Moonlight.exe` 在发布输入重命名为 `MiniStream.exe`。部署 Qt/QML/SDL/FFmpeg/OpenSSL 等依赖，保留许可。主控端版本来自 `app/version.txt`。

改动 QML 页面（含 2.2 新增的体验监测）后要重新 qmake 让 `qml.qrc` 生效，并确认 `TrendChart.qml`、`MetricCard.qml` 已被收录；只重编 C++ 而不重新生成 qrc，新 QML 不会进入资源，运行期会报找不到组件。UI 可先用 `构建产物/client-ui-22-preview` 这类独立目录启动验证，不要覆盖用户正在使用的安装目录。

主控端凭据使用 Windows DPAPI：`app.pro` 仅在 win32 链接 `crypt32`，`app/mslinkbridge.cpp` 负责 `dpapi:v1:` 加解密与旧明文 ini 的原子迁移（`QSaveFile`）。修改该文件或口令字段后要重新构建并使用隔离的 `MINISTREAM_UI_PREVIEW=1` 配置验证迁移，不要读写真实的 `mslink.ini`。非 Windows 构建走降级桩，发布主控端仍只支持 Windows x64。

## mslink 和回归

```powershell
cmake -S src/mslink -B build/mslink -DBUILD_TESTING=ON
cmake --build build/mslink --config Release
ctest --test-dir build/mslink -C Release --output-on-failure
```

本地中文目录映射为 `源码/mslink`；本机 2.2 构建目录是 `D:\MiniStreamDev\build-mslink-22`（`构建产物/build-22-mslink.bat`，Ninja + Release + `-DBUILD_TESTING=ON`）。CTest 现有七项：`udp_reply_source`（回包源地址）、`relay_queue_frame`（4 MiB 队列与 64 KiB 帧边界，白盒包含 `relay.cpp`）、`relay_handshake_integration`（慢速/非法对端等待时的非阻塞握手、单认证客户端、超长帧与快速停止；测试超时 120 秒）、`relay_startup`（启动失败关闭已创建 socket、IPv4 校验、失败后同端口重启成功）、`relay_stream_open_failure`（上游目标不可达时返回 `StreamClose`）、`relay_telemetry`（300 采样点/100 事件上限、1 秒采样节流、事件去重与截断、遥测 JSON 不含历史、事件不含口令/设备名/对端报告、媒体会话开始与结束事件）、`relay_selftest`（隧道自检）。

2.2.0 的本机结果为 **7/7**（2026-10-24，`构建环境/mlqt/build-mslink-22/Testing/Temporary/LastTest.log`）。这是单机回环与白盒/集成结果，不覆盖公网链路、慢速扫描或真实媒体。额外网卡测试需传入实际可达本机地址对，不能因为同机跨接口模拟被 Windows 路由阻止就宣称远程路径不通。

修改 `relay.cpp`、`relay.h` 或 `sequence_window.h` 后，独立 mslink 与主控端内嵌 Relay 都要重建（主控端把 `src/mslink` 放在完整树根 `mslink`）。`app/mslink/relay.h` 只是转发头，禁止维护第二份定义。改动遥测常量（`kTelemetryHistoryMax`、`kTelemetryEventMax`、`kTelemetrySampleMinMs`）必须同步 `relay_telemetry`，并检查主控端桥接与客户端页面的假设仍是 300 点/100 条。

手工验证口令传递（口令只从 stdin 读，不进入命令行）：

```powershell
'YourTestPassword123' | .\mslink.exe host --tcp 14738 --udp 14739 --password-stdin --target 127.0.0.1
```

空 stdin、`--password` 与 `--password-stdin` 同时出现都应退出并报错；`构建产物/mslink-robustness/stdin_check.py` 是这类检查的现成脚本（使用独立端口，不占用 14738/14739 的真实会话）。

## 弱网端到端

`tools/mslink_weaknet_e2e.py` 启动假 Sunshine（TCP + UDP 回显）、两份真实 `mslink.exe` 和位于两者之间的 asyncio 损伤代理，只改中间链路，不修改隧道程序：

```powershell
python tools/mslink_weaknet_e2e.py --mslink 构建产物\host\mslink.exe
python tools/mslink_weaknet_e2e.py --scenarios baseline,recover,outage30 --outage-seconds 30
```

默认场景为 `baseline,rtt100_loss2,limit5,udp_down,recover,outage30,duplex`，结果写入 `构建产物/mslink-e2e/<run-id>/summary.json`，并复制一份到 `构建产物/mslink-e2e/latest.json`。每项检查都有名称与明细，判定物理 UDP 用代理侧的 UDP data 包增量，而不是 TCP 字节或应用计数。

方法限制必须随结果一起引用：客户端与被控端在同一台 Windows 主机、走 127.0.0.1 动态端口；损伤双侧对称；UDP 回显是自造数据，不是 H.264/Opus 媒体；没有 GPU 解码与呈现，不验证 Moonlight FEC/NACK；代理不模拟 AQM、IP 分片、MTU、NAT 超时或运营商突发丢包；`--outage-seconds` 默认 5 秒加速，跑完整的 30 秒中断要显式传参。它不能替代真实跨网/FRP 游戏串流验收。

## 被控端控制台

```bat
mkdir build-host-console
cd build-host-console
D:\Qt\6.9.3\msvc2022_64\bin\qmake.exe ..\src\host-console\host-console.pro
nmake /NOLOGO
```

使用 windeployqt 部署 Widgets/Network/SVG/platform 插件，复制图标。中文资源路径下自动生成 RC 需要 UTF-8 code page（`#pragma code_page(65001)`），或改用 ASCII 构建路径。运行时用 `--root` 指定真实被控端根目录。

2.2 控制台有四个页面：`主机概览`、`连接质量`、`媒体会话`、`服务与诊断`（侧栏另有“真实遥测 · 1 秒采样”“无数据时显示 — / 不生成演示指标”说明）。趋势图来自安装根目录 `mslink-status.jsonl` 的尾部 300 行，事件来自当前 `mslink-status.json` 的 `events` 数组；两者都要求后台已更新。验证时同时打开 GUI 和状态文件，确认 PID、`updated_unix_ms` 与图表点数一致，不要用旧后台的界面反推新功能。

## 安装包与发布

当前 Inno 脚本的 source/output 是本机交付路径；换机需修改顶部路径，不要把私人配置补进包。Client 输入为完整运行目录；Host 输入包括 Sunshine、mslink、console 和 `_internal`，排除 Sunshine config、ini、日志、转储和运行状态。微软 redist 位于 `构建产物/prerequisites`。

Host 安装器的口令流程：向导页收集口令并写入 `mslink-host.ini`，`install.ps1` 从受限 ini 读取后以标准输入传给隧道，计划任务只保留 `--password-stdin`。新安装校验 12–64 字符（字母、数字、`_ . @ -`）；检测到已有 ini 时允许旧口令原样保留并提示更换。安装脚本对 ini、`安装信息.txt` 和安装日志调用 `Protect-MiniStreamFile`，ACL 仅 SYSTEM / Administrators / 安装用户；安装日志不写入口令。

两端安装包必须在本轮源码上重新编译后才计算 SHA256，不能沿用 2.1.4 的哈希。2.2.0 没有改动安装器逻辑，但界面文件、`mslink.exe` 和控制台都要来自同一轮构建；中断的编译会留下不完整的 EXE。

```powershell
& 'C:\Program Files (x86)\Inno Setup 6\ISCC.exe' 源码/安装包脚本/MiniStream-Client.iss
& 'C:\Program Files (x86)\Inno Setup 6\ISCC.exe' 源码/安装包脚本/MiniStream-Host.iss
```

等待编译成功后才计算 SHA256。中断编译产生的 EXE 可能不完整。发布前核对包输入清单，准备 `release-manifest.json`、`SHA256SUMS.txt`、源码包和 release notes。不要从旧包或旧进程推断新包内容。

## 验收环境与未执行项

2.2.0 的验证在本机 Windows 11（26200）完成：CTest 7/7、弱网 e2e 7 场景 50/50、两端程序与 Qt 页面实际启动检查。**Windows 10/11 虚拟机安装本轮没有执行**：本机没有 Windows ISO，当前会话不是管理员（无法创建/配置虚拟机与挂载镜像），虽然 Hyper-V 可用（`HypervisorPresent=True`、`New-VM` 存在）也已在虚拟化层下运行。因此不能把本轮结果写成“干净系统安装通过”。

界面截图由 `构建产物/capture-ui.ps1` 对真实进程窗口调用 `PrintWindow` 抓取，存放在 `构建环境/mlqt/*.png`；`web/assets` 目前仍是 2.1 系列图片。任何截图都要注明版本、窗口状态（未连接/无采样）和“不是性能承诺”，不要用示意图或旧图代替新页面。
