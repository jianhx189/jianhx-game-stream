# 当前版本构建指南 / Current build guide

本文适用于 2.1.4。仓库早期根 CMake 客户端不是当前发布主控端；旧原型的构建方式见 `文档/历史构建指南-旧原型.md`，维护者应以本文和 DEVELOPMENT 为准。

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

主控端凭据使用 Windows DPAPI：`app.pro` 仅在 win32 链接 `crypt32`，`app/mslinkbridge.cpp` 负责 `dpapi:v1:` 加解密与旧明文 ini 的原子迁移（`QSaveFile`）。修改该文件或口令字段后要重新构建并使用隔离的 `MINISTREAM_UI_PREVIEW=1` 配置验证迁移，不要读写真实的 `mslink.ini`。非 Windows 构建走降级桩，发布主控端仍只支持 Windows x64。

## mslink 和回归

```powershell
cmake -S src/mslink -B build/mslink -DBUILD_TESTING=ON
cmake --build build/mslink --config Release
ctest --test-dir build/mslink -C Release --output-on-failure
```

本地中文目录映射为 `源码/mslink`。CTest 现有六项：`udp_reply_source`（回包源地址）、`relay_queue_frame`（4 MiB 队列与 64 KiB 帧边界，白盒包含 `relay.cpp`）、`relay_handshake_integration`（慢速/非法对端等待时的非阻塞握手、单认证客户端、超长帧与快速停止；测试超时 120 秒）、`relay_startup`（启动失败关闭已创建 socket、IPv4 校验、失败后同端口重启成功）、`relay_stream_open_failure`（上游目标不可达时返回 `StreamClose`）、`relay_selftest`（隧道自检）。2.1.3 的本机结果为 4/4，但那只是当时的四项；2.1.4 的最终 CTest 结果以发布清单为准。额外网卡测试需传入实际可达本机地址对，不能因为同机跨接口模拟被 Windows 路由阻止就宣称远程路径不通。

修改 `relay.cpp`、`relay.h` 或 `sequence_window.h` 后，独立 mslink 与主控端内嵌 Relay 都要重建（主控端把 `src/mslink` 放在完整树根 `mslink`）。`app/mslink/relay.h` 只是转发头，禁止维护第二份定义。

手工验证口令传递（口令只从 stdin 读，不进入命令行）：

```powershell
'YourTestPassword123' | .\mslink.exe host --tcp 14738 --udp 14739 --password-stdin --target 127.0.0.1
```

空 stdin、`--password` 与 `--password-stdin` 同时出现都应退出并报错；`构建产物/mslink-robustness/stdin_check.py` 是这类检查的现成脚本（使用独立端口，不占用 14738/14739 的真实会话）。

## 被控端控制台

```bat
mkdir build-host-console
cd build-host-console
D:\Qt\6.9.3\msvc2022_64\bin\qmake.exe ..\src\host-console\host-console.pro
nmake /NOLOGO
```

使用 windeployqt 部署 Widgets/Network/SVG/platform 插件，复制图标。中文资源路径下自动生成 RC 需要 UTF-8 code page（`#pragma code_page(65001)`），或改用 ASCII 构建路径。运行时用 `--root` 指定真实被控端根目录。

## 安装包与发布

当前 Inno 脚本的 source/output 是本机交付路径；换机需修改顶部路径，不要把私人配置补进包。Client 输入为完整运行目录；Host 输入包括 Sunshine、mslink、console 和 `_internal`，排除 Sunshine config、ini、日志、转储和运行状态。微软 redist 位于 `构建产物/prerequisites`。

Host 安装器的口令流程：向导页收集口令并写入 `mslink-host.ini`，`install.ps1` 从受限 ini 读取后以标准输入传给隧道，计划任务只保留 `--password-stdin`。新安装校验 12–64 字符（字母、数字、`_ . @ -`）；检测到已有 ini 时允许旧口令原样保留并提示更换。安装脚本对 ini、`安装信息.txt` 和安装日志调用 `Protect-MiniStreamFile`，ACL 仅 SYSTEM / Administrators / 安装用户；安装日志不写入口令。

两端安装包必须重新编译后才计算 SHA256，不能沿用 2.1.3 的哈希。2.1.4 修复了首次保存与启动失败清理，重建时应确认 `mslink`、控制台和主控端内嵌 Relay 都来自同一轮源码。中断的编译会留下不完整的 EXE；本文不预先宣称 2.1.4 安装包已编译完成，最终结果以发布清单生成时的记录为准。

```powershell
& 'C:\Program Files (x86)\Inno Setup 6\ISCC.exe' 源码/安装包脚本/MiniStream-Client.iss
& 'C:\Program Files (x86)\Inno Setup 6\ISCC.exe' 源码/安装包脚本/MiniStream-Host.iss
```

等待编译成功后才计算 SHA256。中断编译产生的 EXE 可能不完整。发布前核对包输入清单，准备 `release-manifest.json`、`SHA256SUMS.txt`、源码包和 release notes。不要从旧包或旧进程推断新包内容。
