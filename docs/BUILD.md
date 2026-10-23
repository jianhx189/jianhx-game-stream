# 当前版本构建指南 / Current build guide

本文适用于 2.1.2。仓库早期根 CMake 客户端不是当前发布主控端；本机旧 `文档/BUILD.md` 已作为历史材料保留，维护者应以本文和 DEVELOPMENT 为准。

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

## mslink 和回归

```powershell
cmake -S src/mslink -B build/mslink -DBUILD_TESTING=ON
cmake --build build/mslink --config Release
ctest --test-dir build/mslink -C Release --output-on-failure
```

本地中文目录映射为 `源码/mslink`。CTest 包含源地址保持与隧道自检；额外网卡测试需传入实际可达本机地址对。不能因为同机跨接口模拟被 Windows 路由阻止就宣称远程路径不通。

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

```powershell
& 'C:\Program Files (x86)\Inno Setup 6\ISCC.exe' 源码/安装包脚本/MiniStream-Client.iss
& 'C:\Program Files (x86)\Inno Setup 6\ISCC.exe' 源码/安装包脚本/MiniStream-Host.iss
```

等待编译成功后才计算 SHA256。中断编译产生的 EXE 可能不完整。发布前核对包输入清单，准备 `release-manifest.json`、`SHA256SUMS.txt`、源码包和 release notes。不要从旧包或旧进程推断新包内容。
