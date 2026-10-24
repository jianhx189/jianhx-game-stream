# 使用与升级 / User guide

## 两端角色与安装

Host 安装在实际运行游戏/桌面的 Windows x64 主机，Client 安装在操作电脑。获取同一正式 Release 的两份安装包，按 SHA256SUMS 校验。程序携带匹配的 VC143 运行库；安装器检测并按需运行微软 x64 Redistributable，失败时不继续。当前仍需更多干净 Win10/Win11 环境验证。

被控端安装时设置自己的隧道口令与端口，保存安装信息。打开桌面「MiniStream 被控端」查看控制台。主控端向导填写主机地址、TCP/UDP 端口和隧道口令。Sunshine 管理密码为可选自动配对信息；没有时使用手动 PIN。

2.1.3 起，新安装的隧道口令要求 12–64 个字母、数字或 `_ . @ -`；升级时已有口令可以原样保留，但建议尽快更换。隧道进程通过标准输入接收口令，任务计划与命令行里不会出现口令；安装日志也不再记录口令。主控端保存口令时使用当前 Windows 用户的 DPAPI（`dpapi:v1:`），旧版明文配置会在载入后原子迁移。2.1.4 修复了 2.1.3 在主控端首次保存配置和 `mslink` 启动失败清理上的回归；2.2.0 沿用这些修复与保护。安装包版本以 v2.2.0 Release 页和同版 `release-manifest.json` 为准；两端应配套升级。

## 2.2 的两端界面

**主控端**使用深色工作台，左侧导航为：

- **我的设备**：设备卡片、在线与配对状态、连接向导入口；空状态会说明尚未添加主机。
- **体验监测**：连接质量页。顶部显示当前通道（`UDP · 低延迟通道` / `TCP · 兼容通道` / `正在探测 UDP` / `等待隧道连接`）和一条定位提示，下面是四项指标卡、60/300 秒趋势、视频体验、客户端耗时、链路计数和最近事件。
- **连接配置**：打开连接向导，填写主机地址、端口与隧道口令。
- **串流偏好**：进入设置页调整码率、分辨率等串流参数。
- **使用教程**：打开官网教程。

串流中按 `F12` 或 `Ctrl+Alt+Shift+X` 打开菜单，可切换鼠标模式、全屏与统计。

**被控端控制台**同样是四个页面：

- **主机概览**：隧道往返延迟、实际渲染帧率、主机发送速率、网络丢帧率、解码耗时、帧排队耗时，以及已认证设备表的设备/传输模式/连接时长/状态。
- **连接质量**：心跳抖动、活动 TCP/UDP 流、发送队列、隧道重连、本地丢弃、UDP 探测/应答、认证拒绝/回包错误，四张 1 秒采样趋势图，事件表可按「全部级别 / 提示与警告 / 仅警告」筛选。
- **媒体会话**：接收帧率、实际渲染帧率、视频码率、分辨率、网络丢帧、解码/排队/渲染耗时与主机处理。
- **服务与诊断**：启动/重启/停止本机服务、端口配置、事件与日志、导出诊断。危险操作会要求确认。

侧栏固定说明“真实遥测 · 1 秒采样”“无数据时显示 —”“不生成演示指标”。任何页面都不会补造曲线。

## 指标口径

| 指标 | 实际含义 | 不要当成 |
| --- | --- | --- |
| 隧道 RTT / 抖动 | 隧道控制面心跳往返及其抖动 | 端到端鼠标/操作延迟 |
| 媒体流 RTT | 媒体层（Moonlight）报告的往返时间 | 隧道心跳 |
| 实际渲染帧率 / 接收帧率 | 主控端真正呈现的帧数 / 到达的帧数 | 编码端帧率上限或 GPU 能力 |
| 视频码率 | Sunshine 编码出的媒体平均码率 | 隧道重压缩后的码率；隧道不重压缩 |
| 网络丢帧 / 呈现丢帧 | 最终无法恢复的帧比例 / 呈现节奏丢弃的帧比例 | IP 丢包率 |
| 解码 / 帧排队 / 渲染 / 主机处理 | 主控端每帧各环节的平均耗时 | 网络延迟或服务器性能结论 |
| 发送队列 | 隧道内存排队字节，硬上限 4 MiB | 带宽或链路容量 |
| 主机发送速率 | 隧道 TCP + UDP 有效字节的差分 | 物理 UDP 是否可用 |
| UDP 探测 / 应答、认证拒绝 / 回包错误 | 隧道探测与拒绝计数 | 媒体质量评分 |
| 本地丢弃 | 未就绪、超限或重放拒绝导致的丢弃 | 网络丢包 |

判断顺序建议：先看通道是不是 `TCP · 兼容通道`，再看网络丢帧与 RTT，最后看解码与排队。解码正常而排队升高，优先检查呈现节奏与显示刷新率。

## 趋势、事件与新鲜度

- 趋势只保存最近 **300 个约 1 秒采样点**（约 5 分钟），页面可切换 60 秒或 300 秒窗口；被控端的趋势读取本机状态文件的尾部 300 行。
- 刚开始连接、断线期间或后台未更新时曲线是空的；超过 5 秒没有新报告，媒体指标显示“—”。这是设计行为，不是故障。
- 事件最多 **100 条**，同类别同文案 1 秒内只记一次，单条超长会被截断；主控端展示最近 6 条，被控端可按级别筛选。
- 历史与事件只驻留内存，重启程序即清空；需要留档请用“导出脱敏诊断”，并人工复核内容。

## 提示阈值

发送队列 > 1 MiB、媒体网络丢帧 > 2%、帧排队 > 30 ms、解码 > 16 ms 会给出文字提示；被控端在已有媒体采样时，另用 RTT > 100 ms、网络丢帧 > 2% 或排队 > 30 ms 判定“连接需关注”。这些阈值用于**定位问题**，不是体验评分或通过标准；未触发时页面会明确说明“当前采样未触发提示阈值”。

## 网络

同一局域网先填被控端 LAN IPv4。跨网配置两条独立映射：公网 TCP → 本机 TCP 14738，公网 UDP → 本机 UDP 14739（实际端口以配置为准）。公网端口可以与本机不同，客户端必须填写公网实际值。示例使用 `host.example.com`，不是公共服务。

不要把 Sunshine 管理页面作为日常公网串流入口。自定义隧道不替代加密 VPN，请阅读 SECURITY。FRP 的 UDP 类型也可能由其他底层传输承载，实际延迟需实测。

## 连接与串流

测试连接后确认已配对，再打开应用或桌面。隧道连接不代表媒体已开始。控制台每秒显示实际后台上报；旧后台、错误根目录或过期采样不会显示伪造数据。媒体指标需要新版主控端正在串流且持续上报，两端版本过旧时对应项目保持“—”。

显示 TCP 兼容通道时媒体仍可能可用，但可能受队头阻塞影响。UDP 字节数包含逻辑媒体搬运（回退时走 TCP），不能单独证明物理 UDP 可用。游戏通常使用相对鼠标，桌面按需使用绝对鼠标。遇到输入异常先记录焦点、菜单、模式、画面/声音状态和时间。

## 升级与回退

结束串流后保存自己的配置与安装信息。Host 在已有目录覆盖升级，安装器会尝试识别旧 x86/x64 路径并保留隧道配置、Sunshine 配对与账号。不要只打开新 GUI 而继续运行旧 mslink；用状态文件 PID 对照实际后台。旧 Client 配置可能需要在新版向导重新填写。

2.1.3 首次载入旧主控端配置时，会把明文口令迁移为当前 Windows 用户的 `dpapi:v1:` 密文。迁移只影响本机文件：口令在内存中仍是明文，同一 Windows 用户可解密，网络传输也没有加密。回退到 2.1.2 或更早版本时旧程序读不到密文，需要重新填写口令；更换 Windows 用户或重装系统后同样需要重填。

如果正在使用 2.1.3：首次连接时保存总是失败、之后每次都要重填，或启动失败后首次连接卡住，属于 2.1.3 的已知回归，覆盖升级到 2.1.4 或更新版本即可，配置、口令和配对会保留。**全新安装不要使用 2.1.3。** 直接安装 2.2.0，并让两端同时升级，才能使用新增的体验监测页面与历史/事件。

升级后核对两端版本、实际 EXE 路径、状态更新时间、认证设备和传输模式，再启动媒体。回退前同样结束连接并保留配置，避免不同版本组件混装。

## English quick start

Install Host on the PC running your game, and Client on the PC you operate. Use matching releases and verify SHA-256. On LAN enter the host's LAN IP; with FRP configure separate public TCP and UDP mappings and enter the real public ports. Keep your credentials private, complete pairing and launch a desktop/application. Telemetry appears only when the corresponding background process and media reports are current.

The controller's dark workspace has My Devices, Experience, Connection, Streaming Preferences and Guide entries; the Experience page shows the transport badge, tunnel RTT/jitter, rendered and received frame rate, bitrate, send queue, 60/300-second trends, media drop/pacing details, decode/queue/render/host timings, link counters and recent events. The host console has Overview, Connection Quality, Media Session and Services & Diagnostics pages with an event level filter. History holds at most 300 one-second samples and 100 events in memory; a 60-second or 300-second window can be selected, samples older than five seconds render as a dash, and no demo curves are generated. Queue above 1 MiB, media drop above 2%, queueing above 30 ms and decode above 16 ms only raise diagnostic hints, not a score. Tunnel RTT is not end-to-end input latency and media frame loss is not IP packet loss. End sessions before upgrading over the existing host directory and upgrade both ends together.
