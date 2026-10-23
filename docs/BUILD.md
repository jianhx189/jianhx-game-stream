# 构建说明（Windows + MSVC）

## 1. 需要的东西

| 组件 | 说明 |
|------|------|
| VS2022 Build Tools | 工作负载 `使用 C++ 的桌面开发`（含 MSVC + Windows SDK） |
| CMake ≥ 3.20 | 任意来源；VS 自带的也行 |
| Ninja（可选） | 用 `-G Ninja` 时需要，否则用 VS 生成器 |
| FFmpeg 预编译包 | shared 版（含 DLL + 头文件 + 导入库） |

## 2. 准备 FFmpeg（一次性）

推荐 MSYS2 的 ucrt64 包（与本项目验证环境一致，且依赖链最干净）：

```bat
:: 安装 MSYS2 后，在其 shell 里：
pacman -S mingw-w64-ucrt-x86_64-ffmpeg
```

然后用仓库自带脚本把「只够 MiniStream 用的那 ~81 个 DLL」收进项目，
并生成 MSVC 可用的导入库：

```bat
powershell -ExecutionPolicy Bypass -File scripts\collect_ffmpeg_dlls.ps1 ^
    -Source D:\msys64\ucrt64 ^
    -VcVars "D:\VSBuildTools\VC\Auxiliary\Build\vcvars64.bat"
```

脚本做三件事：递归解析 `dumpbin /dependents` 依赖闭包 → 拷进
`third_party\ffmpeg\bin` → 把 `libav*.dll.a` 同时复制成 `av*.lib`。

> 为什么 `.dll.a` 直接改名就能给 MSVC 用：GNU ld 生成的导入库是标准 COFF
> 归档，`link.exe` 认。所以不必非得找 gyan.dev 的 MSVC 专用包。

## 3. 编译

```bat
scripts\build.bat
```

这个脚本内部做的是（也可以手动执行）：

```bat
call "D:\VSBuildTools\VC\Auxiliary\Build\vcvars64.bat"
set VSLANG=1033
cmake -S . -B build\msvc -G Ninja -DCMAKE_BUILD_TYPE=Release ^
      -DCMAKE_MAKE_PROGRAM=<ninja路径> -DFFMPEG_ROOT=%CD%\third_party\ffmpeg
cmake --build build\msvc
```

想用 VS 生成器（出 .sln，能用 IDE 打开）：

```bat
cmake -S . -B build\vs -G "Visual Studio 17 2022" -A x64 ^
      -DFFMPEG_ROOT=%CD%\third_party\ffmpeg
cmake --build build\vs --config Release
```

产物在 `build\msvc\bin\`（或 `build\vs\bin\Release\`）：
`ministream-host.exe`、`ministream-client.exe`、`ministream-selftest.exe`、
`ministream-videoprobe.exe`，旁边是 FFmpeg 的运行 DLL 和两个默认 ini。

## 4. 自检（建议每次改完都跑）

```bat
build\msvc\bin\ministream-selftest.exe     :: 125 项协议/加密/序列化/生命周期断言
build\msvc\bin\ministream-videoprobe 5 0 60 auto        :: 采集+编码 5 秒
build\msvc\bin\ministream-videoprobe 5 0 60 auto 1280x720   :: 顺带验证 GPU 缩放
```

videoprobe 不联网、不开界面，是排查"黑屏/没有画面"的第一站。

## 5. 单文件快速语法检查

```bat
scripts\check.bat src\host\capture.cpp
```

## 6. 本仓库的三条铁律（都是踩过坑的）

1. **`.bat`/`.cmd`/`.ps1` 必须纯 ASCII。** cmd.exe 按本机代码页（这里是 GBK）读批处理，
   PowerShell 5.1 对没有 BOM 的 `.ps1` 同样按 ANSI 解码——里面的中文字面量会被
   当成乱码命令/乱码路径。脚本里需要中文（比如匹配日志关键字）就用
   `[char]0xXXXX` 拼出来。
2. **改 UTF-8 源码不要用 PowerShell 的 `Get-Content`/`Set-Content`。**
   Windows PowerShell 5.1 对无 BOM 文件按 ANSI 解码，中文会被双重编码损坏。
   要用 `[System.IO.File]::ReadAllText/WriteAllText`（显式 UTF-8）或编辑器。
3. **打开文件走 `src/common/fsutil.h`。** `std::fopen`/`std::ifstream` 在 Windows
   上按 ANSI 解释路径，安装路径里只要有中文，日志和配置就会静默创建失败。
4. **改了头文件里类的成员/布局之后，必须删 `build\msvc` 全量重编。**
   本项目出现过两次"增量构建残留旧 .obj → 同一个类在两个 TU 里布局不一致"的
   诡异 bug：一次是自检随机崩溃，一次是工具栏"第一次能弹、之后完全失效"
   （旧 obj 里 `ClientSession::Impl` 按老的 `SessionToolbar` 大小算偏移，
   构造时把工具栏的成员内存踩了）。表现都很像业务 bug，其实是构建脏了。
   ```bat
   rmdir /s /q build\msvc
   scripts\build.bat
   ```

## 7. 辅助脚本

| 脚本 | 用途 |
|------|------|
| `scripts\build.bat` | 正常构建（Release，Ninja） |
| `scripts\check.bat <相对路径.cpp>` | 单文件语法检查 |
| `scripts\build-sym.bat` | 带 PDB + MAP 的构建（`build\sym`），用于事后分析 |
| `scripts\build-map.bat` | **只加 `/MAP`** 的构建（`build\map`）。若产物与发布版一致，`.map` 就是发布版的符号表 |
| `scripts\abtest.ps1` | A/B 回归：串流中停止服务，旧版应崩 `0xC0000409`、修复版应退 0 |
| `scripts\regress.ps1` | 单实例守卫 + 保存配置不打断会话 |
| `scripts\motion.ps1` | 制造屏幕动态（DXGI 只在画面变化时出帧，静止桌面只有 5~15 fps，测吞吐必须用它） |
| `tools\minidump_info.py` | 解析 minidump：异常码、fast-fail 子码、崩溃线程栈、跨线程查看 |
| `tools\stack_code_frames.py` | 把崩溃线程栈上落在代码区的地址全部列出来（配合 `.map`） |

## 8. 构建"卡住/没输出/卡在拷贝 DLL"的排查（中文路径 + 代码页坑）

工作区路径含中文（`D:\远程项目`）时，CMake/Ninja 的自定义命令会生成
`build\msvc\CMakeFiles\xxx.bat` 辅助批处理。**这个 .bat 的编码取决于执行
configure 时的控制台代码页**：

- 代码页 936（GBK）下 configure：路径正常，构建正常；
- 代码页 65001（UTF-8，某些编辑器/终端默认）下 configure：CMake 写出的
  .bat 里路径是 UTF-8 字节，cmd 按 936 读成 `D:\杩滅▼椤圭洰\...` →
  `系统找不到指定的路径`，构建在"拷贝 FFmpeg DLL"那一步 FAILED，
  看起来就像**一直卡在编译**。

已做的加固：

1. 拷贝 FFmpeg DLL 改成 **configure 阶段 `file(COPY)`**（不生成 .bat，不受代码页影响）。
   代价：换了 FFmpeg 目录/版本后要重新 configure（或删 `build\msvc` 重来）；
2. `scripts\build.bat` 开头固定 `chcp 936`，保证以后重新 configure 时编码稳定；
3. `build.bat` 增加并发保护：检测到已有 `ninja.exe` 在跑就直接报
   `ANOTHER_BUILD_RUNNING`，避免两个构建抢同一个 build 目录（表现也是"卡住"）；
4. 上次构建被强杀留下的 `build\msvc\.ninja_lock` 会让下一次构建干等，
   `build.bat` 会在没有 ninja 进程时自动清掉它。

如果仍然怀疑卡死，按这个顺序排查：

```bat
tasklist /fi "imagename eq ninja.exe"
tasklist /fi "imagename eq cmake.exe"
:: 有残留就结束；然后删锁重来
del /q build\msvc\.ninja_lock
scripts\build.bat
```

另外 `.bat` 文件必须是 **纯 ASCII + CRLF**：LF 换行 + 中文注释会让 cmd 把
`if (...)` 块解析错（报 `'ing)' 不是内部或外部命令`，甚至空转卡住）。
