# MiniStream 开发交接与维护手册

适用版本：2.1.3。此文是当前实现的接管入口，历史实验、旧原型说明与过往测试记录不能替代本文。发布的具体文件哈希、上游来源和验证结果以同版本 `release-manifest.json` 为准。

## 1. 产品与实现现状

项目为 Windows x64 个人游戏/桌面串流：Sunshine 负责采集、编码与输入注入，修改版 Moonlight-Qt 负责解码、渲染、用户输入与 UI，mslink 将多路 TCP/UDP 服务复用到一对外部端口。原生被控端控制台负责监控和有限的本机管理。

当前隧道只接纳一个认证客户端，尚非多用户远程支持平台。局域网或 FRP 需要用户自行配置，没有内置公共中继、账号系统或自动 NAT 打洞。正式发行代表交付渠道和版本状态，不代表所有系统/网络组合完成认证。

## 2. 本机目录与公开仓库映射

| 本机位置 | 仓库位置 / 作用 |
| --- | --- |
| `D:\远程项目\源码\moonlight-qt\app` | `src/moonlight-qt/app`，交付覆盖层，修改的权威副本 |
| `D:\远程项目\源码\mslink` | `src/mslink`，CLI、复用协议、测试 |
| `D:\远程项目\源码\host-console` | `src/host-console`，Qt Widgets 控制台 |
| `D:\远程项目\源码\安装包脚本` | `scripts/installer`，Inno Setup |
| `构建产物/host/_internal` | `scripts/host`，当前有效的安装/服务脚本；不要漏同步 |
| `文档` | `docs`，通过发布工具的显式清单同步当前文档 |
| `web` / `web/dist` | 静态站点源码 / 实际部署输出 |
| `构建环境/mlqt` | 完整 Moonlight-Qt 构建树，包含上游源码与依赖 |
| `D:\MiniStreamDev` | 指向完整构建树的 ASCII 路径 junction，减轻工具链对中文路径的限制 |
| `构建产物/client-preview` | 当前 Client 安装输入；目录名为历史遗留，不表示安装包仍是预览版 |
| `构建产物/host` | 当前 Host 安装输入；Sunshine config 是私人运行数据，必须排除 |
| `构建产物/setup` / `安装包/<版本>` | 本轮生成文件 / 冻结的版本归档（2.1.2 为上一版） |
| `社区测试` | 私有日志与用户材料；禁止整体同步到公开仓库 |

本地工作区根目录不是 Git checkout。不能把 `git status` 在根目录失败当作 GitHub 未更新。完整构建树的 origin 是上游 Moonlight，不是本项目；不要向该 origin 推送 MiniStream 改动。发布工具通过 GitHub API 将显式清单映射到本项目 main。

不要从旧的 `构建产物/client`、`client-ui` 或旧安装目录抓取 EXE 覆盖新包。`host-ui` 的 GUI 曾是旧程序；打包权威输入是 `host/console`。版本判断必须结合实际运行路径、PID、构建时间和哈希。

## 3. 上游与工具链锁定

- Moonlight-Qt：`dc92b6d2f361064d3e9e53ce5de92f611db77220`，上游基线 6.2.0；MiniStream 对外版本单独为 2.1.3（`app/version.txt`、控制台、两份安装脚本与网站需一致）。
- 子模块：moonlight-common-c `04053843c90c4268583f2496b997d54d59f03957`；qmdnsengine `920c097ffa742e2968290f15d4dde6693aec02e5`；SDL_GameControllerDB `8d9fefd7b810f2541f78cc7a8ccbd185bc84c7a5`。
- Windows 预编译依赖：Moonlight `moonlight-qt-deps` 的 `v19`，由基线 `setup-deps.ps1` 获取；位于完整树的 `libs/windows`。
- Qt：6.9.3 `msvc2022_64`，本机 `D:\Qt\6.9.3\msvc2022_64`。
- MSVC：VS2022，14.44；`D:\VSBuildTools\VC\Auxiliary\Build\vcvars64.bat`；CMake/Ninja 可用 VS 自带版本。
- Sunshine：原发行组件 `v2026.914.233613`，提交 `63d35f702ee9e362e43263742981836ec0710384`，不是本项目修改的采集内核。
- 安装器：Inno Setup 6。VC++ Redist 为微软签名 x64 14.51.36247.0，最低检测 14.44.35112；app-local CRT 与编译器匹配。

参见 `THIRD_PARTY_NOTICES.md`。不要无意升级 SDK、Qt、子模块或 Sunshine 后仍复用旧的验收结论。

## 4. 模块与数据流

### 主控端

`mslinkbridge.cpp/.h` 读取配置、启动 Relay、提供 QML 属性、保存连接与导出诊断。`gui/MslinkSetupDialog.qml` 为向导；`PcView.qml`、`Theme.qml`、`BrandButton.qml` 构成设备工作区。`streammenu.*` 与 `StreamMenuWindow.qml` 管理串流菜单。

`streaming/input/input.*` 处理本地输入，`session.*` 管理会话，`streaming/video/ffmpeg.cpp` 发布真实媒体窗口统计。媒体采样不能依赖用户是否打开性能浮层，否则后台面板会长期无数据。

`backend/autoupdatechecker.cpp` 使用本项目官网 `updates.json`，不要恢复上游 Moonlight 的更新通道。修改 `app/version.txt` 并重新 qmake 后，必须重编 main/systemproperties/autoupdatechecker 等使用 VERSION_STR 的对象；NMake 不一定因命令行宏变化自动重编旧对象。不能只核对 Windows 文件属性版本。

主控端连接配置位于 Qt `AppLocalDataLocation` 下的 `mslink.ini`。路径由程序统一计算，不应向 Program Files 写入用户配置。隧道口令与 Sunshine 口令以 `dpapi:v1:` 密文保存，读取时优先密文、其次旧版明文；载入旧配置后用 `QSaveFile` 原子迁移（先加密好两份再整体写入，失败不动原文件）。DPAPI 为当前 Windows 用户范围：内存中仍是明文，同一用户可解密，不是端到端保护；迁移后旧版本读不到口令。`MINISTREAM_UI_PREVIEW=1` 为隔离 QA 组织名和单实例使用；测试不要读取真实连接或抢占用户流。

### mslink

`src/relay.cpp` 是 Windows C++17 实现，私有 Impl 负责非阻塞套接字、收发队列、TCP frame reader、UDP 流表、认证、防重放、探测/恢复和遥测。`include/mslink/sequence_window.h` 提供 64 包窗口。`src/main.cpp` 提供 CLI、自检和每秒落盘状态；口令只接受 `--password-stdin`（推荐）或 `--password`，两者互斥，空 stdin 直接失败。

2.1.3 的资源与握手约束（实现位置以代码为准）：每连接发送队列 `OutQueue::kMaxQueueBytes = 4 MiB`，`push()` 先整帧预留空间再复制，空间不足时**整帧拒绝**并返回 `false`，不做截断；部分已发送的缓冲只把未发送的尾部计入队列。TCP 单帧 `kMaxTcpFrame = 64 KiB`，`FrameReader` 缓冲上限为 8 字节头 + 64 KiB + 16 KiB，读到 8 字节头即可拒绝超长帧。被控端握手为非阻塞，`kMaxPendingHandshakes = 8`、`kHandshakeTimeoutMs = 5000`；新会话晋升时关闭其余 pending，仍只保留一个已认证客户端。`SequenceWindow::duplicate()` 现在把 `seq == highest` 也算作重复副本，冗余副本与真实丢包分别统计。

TCP 帧为 `[u32长度][u16类型][u16流ID][payload]`；UDP 当前认证头为 20 字节：magic/type/flow/seq/session-prefix/tag。勿再按早期 8 字节头解释所有预算。类型和字节序以代码为准，历史 PROTOCOL 文档仅作背景。

认证后交换会话密钥；UDP 在校验会话和 MAC 后才更新重放窗口和客户端地址。当前密钥通过 TCP 传送，不是完整的加密传输方案。遥测类型 11 复用认证后的 TCP，单条最多 8 KiB，不新增公网管理口。

### 被控端控制台

`host-console/main.cpp` 为 Qt Widgets GUI。每秒读取安装根目录 `mslink-status.json`，检查 schema/role/PID、路径和 5 秒新鲜度；管理员进程路径不可读取时核对 PID+名称并提示身份核验限制。旧后台不会产生新状态，仅启动新 GUI 无法修复。

CLI 原子写 JSON 并轮转 JSONL（约 1 MiB × 2）。GUI 不生成示例性能数据。发送速率由连续字节差分计算。媒体数据由客户端经现有隧道回传；没有新媒体报告时应保留“—”。

## 5. 必须保持的修复和不变量

1. **多网卡回包**：被控端 wildcard UDP 接收使用 `WSARecvMsg` + `IP_PKTINFO`，认证后缓存本机目标地址/接口，ACK 与 DATA 都用 `WSASendMsg` 原地址回包。不要改回普通 `sendto()`，也不要把某用户的 IP 写入代码。
2. **序号**：使用递增计数，不能使用毫秒时间戳；同毫秒突发会重号。认证前不能修改防重放窗口。
3. **恢复**：TCP 兼容通道持续探测，恢复条件基于真实认证 ACK，不能按事件循环次数累计。
4. **TCP 原始流**：Moonlight 与 Sunshine 两侧原样转发；只有复用隧道加帧头。FrameReader 必须保留粘包剩余数据。
5. **头文件 ABI**：`app/mslink/relay.h` 转发到根 `mslink/include/mslink/relay.h`，禁止维护两份不一致的类定义；布局变更需重建相关对象。
6. **配置与运行隔离**：打包不含 ini、Sunshine config、日志、转储、私钥或状态历史。导出器的脱敏不完整，分享前仍要人工复核。
7. **队列与帧上限**：每连接发送队列 4 MiB 硬上限，整帧先预留再入队；空间不足时整帧拒绝并让错误向上返回，不能截断帧、不能静默丢字节。TCP 单帧 64 KiB，`FrameReader` 必须有界，读到 8 字节头即拒绝超长帧。调整这些常量要同时更新 `relay_queue_frame` 测试。
8. **握手**：被控端握手必须保持非阻塞，最多 8 个 pending、5 秒超时；新会话认证成功后关闭其余 pending，始终只保留一个已认证客户端。不要恢复“主线程阻塞等待认证”或“多客户端并存”的写法。
9. **重复统计**：`SequenceWindow::duplicate()` 必须把 `seq == highest` 计为重复副本；改动窗口逻辑时不要让它改变窗口状态（只读判定）。
10. **凭据存储**：主控端口令只写 `dpapi:v1:` 密文，迁移走 `QSaveFile` 原子替换；不要为了兼容旧版把明文写回 ini，也不要对外宣称 DPAPI 等于端到端加密。
11. **口令传递与 ACL**：被控端口令只经 `--password-stdin` 或安装脚本 stdin 传递，日志不得出现隧道/Sunshine 口令；`mslink-host.ini`、`安装信息.txt`、安装日志的 ACL 保持仅 SYSTEM / Administrators / 安装用户，不能放宽到 Users 或 Everyone。

## 6. 构建和本地操作

完整命令见 BUILD。修改先写入交付覆盖层，再同步完整树；独立 mslink 与客户端内嵌 Relay 都要重建，不能只更新其中一个。已有 app 静态子库不能代替首次完整构建。

当前本机被控端历史安装根目录是 `C:\Program Files (x86)\MiniStream-Host`，新版 x64 默认目录可能不同。使用安装器检测的已有目录，避免装出两套服务。入口为 `console/MiniStreamHost.exe --root <根目录>`。以 `mslink-status.json` 的 PID 对照实际监听，确认真正更新了后台。

Sunshine 与隧道由登录计划任务 `MiniStreamSunshine` / `MiniStreamTunnel` 隐藏启动。隧道任务只保存 `mslink host ... --password-stdin` 与端口参数，启动脚本从受限的 `mslink-host.ini` 读取口令后写入标准输入，口令不进入任务 XML、命令行或临时文件。GUI 经 `_internal/gui-action.ps1` 请求 UAC，按安装目录限定进程。保存配置先准备两份内容、备份后替换，失败尝试回滚；保存不立即重启。端口改动还需用户同步 FRP 与防火墙。

安装脚本对 `mslink-host.ini`、`安装信息.txt` 和安装日志调用 `Protect-MiniStreamFile`，ACL 仅保留 SYSTEM、Administrators 与安装用户；安装日志末尾只写“口令不写入日志”。不要把 `安装信息.txt` 打进发布包或提交到仓库。

停止/升级会中断串流。不要为截图、常规编译或测试擅自停止使用中的后台。自动测试使用独立端口、独立目录；不要复用 14738/14739 做第二个认证客户端。

## 7. 验证与发布

最低验证：CMake/CTest 四项（`udp_reply_source`、`relay_queue_frame`、`relay_handshake_integration`、`relay_selftest`）、`--password-stdin` 启动检查、100 包 UDP 突发、窗口/序号边界、真实媒体上报新鲜度；界面检查低分辨率、空状态和过期状态。多网卡修复还需真实转发抓取证明 ACK 源地址与入包目标一致，普通 UDP 字符串超时不算端口不通证据。

2.1.3 在本地回环验证：CTest 4/4（含队列/帧白盒测试，以及慢速/非法对端等待时的握手集成、单认证客户端、超长帧与快速停止）、`stdin_check.py` 全部通过（argv 不含口令、host 从 stdin 获取口令并监听、空 stdin 与参数互斥被拒绝、telemetry 正常）。这些结果只覆盖本机回环、白盒与集成测试；两端最终构建、安装包编译、真实干净系统和真实网络待发布清单生成时确认。2.1.2 记录过真实 HELLO 到达但 ACK 从错误网卡发出，修复后客户端切回 UDP、有效 ACK 增长、发送错误为 0；该结论继续有效，但新的长时间媒体串流仍非完整验收。

发布流程：重建应用 → 同步安装输入 → 脱敏清单检查 → 编译两端安装包 → 生成 SHA256/manifest/源码包 → 创建 draft Release → 上传并核对 GitHub digest → 发布正式版 → 提交网站输出并部署 Cloudflare/GitHub Pages → 核对线上页面与下载。

`tools/publish_release.py` 负责受控清单提交与上传，凭据从隐藏输入/环境变量获取，不保存到仓库。沿用本机代理 `http://127.0.0.1:7897` 可由参数指定。Cloudflare 项目名 `jianhx-game-stream`，自定义域名 `ministream.jianhx.com`；账号和 token 不写入本文。

GitHub Pages 由远端仓库的工作流负责（此前交接记为 `.github/workflows/pages.yml`：main 的 `web/dist/**` 变更后同步 gh-pages，再由 GitHub 的 Pages 构建发布）。本地工作区不是 Git checkout 且没有该工作流文件，本轮无法核对远端实际内容；发布时先在 GitHub Actions 确认触发路径与运行结果，再声称两站均已更新。不能只部署 Cloudflare 就声称两站均已更新。

## 8. 已知限制与接手顺序

- 缺少所有干净 Win10/Win11 安装/卸载、不同 GPU/驱动和长时弱网游戏的覆盖；稳定发布不应写成兼容性认证。
- UDP 预算：常量已按 20 字节认证头计算（`kMaxUdpPayload = 1472 - 20`，上行 DATA 再减 2 字节目标端口，视频载荷上限 1450 字节），旧文档“仍按 8 字节头估预算”的说法已过时。真正未解决的是固定 1500 MTU 假设：没有路径 MTU 发现，PPPoE、VPN、FRP 等更小 MTU 链路可能分片或被静默丢弃，1450 上限也不随链路调整。下一轮应做 MTU 探测或分段策略，并用大包实测。
- 队列 4 MiB、TCP 帧 64 KiB、8 个 pending 握手与 5 秒超时目前只在本地回环与白盒/集成测试中验证，没有公网慢速链路或高并发扫描压测；这些上限只限制资源增长，不构成抗 DoS 保证。
- 自定义认证和明文会话密钥传递需独立安全设计；DPAPI 只保护同一用户可解的本地文件，ACL 只限制本机读取，二者都不改变网络机密性；不要只增加一个加密开关便宣称完成审计。
- 主控端完整构建依赖外部 SDK/子模块；应优先维护可复现构建和持续集成。
- 老的性能、压测、优化或产品对比文档中可能包含旧原型（自研采集/编码/会话）的参数与设计，应先核对当前 Sunshine + Moonlight + mslink 代码再引用。不要把旧实验通过数套用到当前主控端。

建议接手先完成：从发布源码在新目录构建 → 运行 CTest → 观察实际后台 PID/遥测 → 局域网桌面输入 → 真实转发 UDP → 小屏/缩放 → Win10 干净机 → 长时卡顿与 MTU 专项。保留每一步的版本、配置类别、指标和未覆盖范围。
