# 排障指南 / Troubleshooting

## 先分清四层状态

| 层 | 可确认的事实 | 不能据此确认 |
| --- | --- | --- |
| 监听 | 本机进程绑定端口 | 公网可达、认证、媒体 |
| TCP 隧道 | 握手与认证成功 | UDP 回包、视频/输入流畅 |
| UDP 探测 | 有效认证应答 | 持续吞吐、MTU、大包与媒体 |
| 媒体上报 | 新鲜的解码/渲染指标 | 所有游戏与所有输入场景 |

## 有连接，但控制台没有数据

检查打开的 GUI 路径、`--root`、安装根目录的 `mslink-status.json`、其中 PID 是否仍存活、schema=1/role=host、时间是否在 5 秒内。旧 mslink 不生成该文件；仅打开新窗口不会替换后台。管理员进程可能无法由普通权限读取完整路径，界面会说明核验限制。

媒体项为“—”时，再检查 `peer_report_age_ms` 与 `media_age_ms`；超过 5 秒或未启动媒体时不应继续显示旧帧率。当前只有一个认证会话，配对列表不是在线数量。

## 遥测页面：空曲线、缺指标、事件不刷新

2.2 的主控端「体验监测」和被控端「连接质量 / 媒体会话」都直接消费真实采样，以下现象多数是预期行为：

- **趋势图空白**：历史只保留最近 300 个约 1 秒采样点（约 5 分钟），刚启动、刚重连或断线期间没有点。主控端可切 60/300 秒窗口；被控端读的是安装根目录 `mslink-status.jsonl` 的尾部，文件被清空或后台未更新时同样为空。不要用截图或旧日志推断当前曲线。
- **只有一部分卡片是“—”**：媒体类指标依赖主控端正在串流并回传，未开始串流、两端版本不一致或超过 5 秒无新报告都会显示“—”。隧道 RTT 与队列等隧道指标不需要媒体即可有值。
- **事件表不刷新**：事件最多 100 条，同类别同文案 1 秒内只记一次；稳定运行时不产生新事件是正常的。只有真实状态变化（连接、认证拒绝、回包错误、媒体开始/结束）才写入。
- **队列接近 4 MiB**：发送队列是隧道内存排队，达到上限时整帧会被拒绝并计入错误，不会静默截断。先看吞吐是否被限速或链路拥塞，再看是否需要降低码率。
- **主控端显示“当前采样未触发提示阈值”**：说明没有越过 1 MiB 队列、2% 丢帧、30 ms 排队、16 ms 解码这四条经验线，不是“体验评分通过”。
- **旧后台 + 新界面**：2.2 页面需要同轮构建的 `mslink`；只替换 GUI 或只替换隧道都会让新字段缺失。用状态文件 PID、路径与 `updated_unix_ms` 核对，必要时结束旧进程后重启。
- **导出的诊断包含设备名或地址**：导出只做有限脱敏，分享前人工检查域名、IPv6、设备名和路径。

## 复现弱网行为

本机可用 `tools/mslink_weaknet_e2e.py` 复现受控损伤（延迟、抖动、丢包、重排、限速、UDP 阻断、恢复、30 秒中断），结果写入 `构建产物/mslink-e2e/latest.json`。它只改中间链路，不修改隧道；判物理 UDP 看代理侧 UDP data 包增量。注意它是单机回环、对称损伤、回显为自造数据、无 GPU 解码呈现，不能据此判断真实跨公网游戏是否流畅，也不能替代真实转发链路抓包。

## 为什么进入 TCP 兼容通道

客户端 2.5 秒内没有确认 UDP 时临时回退，仍会持续探测。先看 `udp_probe_sent`、`udp_probe_ack`、`udp_last_send_error`、`udp_data_received`；不要仅凭 `udp_auth_rejected=0` 判断没有收到包。

2.1.2 被控端新增 `udp_hello_received`、`udp_hello_ack_sent`、`udp_session_rejected`、`udp_reply_errors` 和 `udp_reply_route_pinned`。客户端探测增长但 Host 没有 HELLO：查 UDP 目标地址/端口和转发。Host 有 HELLO/ACK 但客户端 ACK 为零：核对返回路径及应答源端点。认证/会话拒绝增长：查两端版本、会话与实现，不要绕过认证来“修好网络”。

已修复案例：多网卡 wildcard 监听收包地址 A，普通 sendto 从地址 B 回包。2.1.2 改用 IP_PKTINFO。实测恢复 UDP；详见 UDP 多网卡修复说明。普通 UDP 字符串不会得到 mslink 回显，不能把静默丢弃当作端口关闭。

## 口令、本地文件与握手

- **首次连接保存总是失败、之后每次都要重填**：这是 2.1.3 的已知回归（`mslink.ini` 尚不存在时保存失败）。升级到 2.1.4 后保存会原子创建配置；升级不会清空已有连接。
- **启动失败后首次连接卡住或本机端口被占**：2.1.3 的 `mslink` 启动失败会残留监听 socket，带 47990 的回退重试可能二次 bind。升级到 2.1.4；临时可先完全退出主控端进程再重试。被控端未运行时，2.1.4 会收到 `StreamClose` 立即失败，而不是等到超时。
- **主控端提示“无法用当前 Windows 用户解密”**：`mslink.ini` 中的 `dpapi:v1:` 只能用保存它的 Windows 用户解密。换用户、换机器或回退到 2.1.2 及更早版本后需要重新填写口令，这是预期行为；不要手工删除密文来“修复”。
- **被控端读不到配置或日志**：`mslink-host.ini`、`安装信息.txt` 和安装日志的 ACL 只允许 SYSTEM、Administrators 与安装用户。其他账户读不到属于预期；分享安装目录前先删除 `安装信息.txt`。
- **命令行看不到口令**：2.1.3 起的隧道通过 `--password-stdin` 接收口令，任务计划 XML 与进程命令行都不会出现口令，安装日志也不再记录隧道/Sunshine 口令。若旧日志或截图曾经包含口令，按 SECURITY 的建议轮换。
- **握手等待/超时**：被控端最多保留 8 个待认证握手，单个 5 秒超时。日志出现占满或超时的提示时，先确认来源地址与防火墙规则，等待超时释放；不要通过绕过认证来处理。已认证会话始终只有一个。

## 鼠标与偶发卡顿

记录精确时间，确认画面/声音是否继续；区分桌面与游戏、窗口焦点、菜单、鼠标相对/绝对模式。网络 RTT/丢帧变化与解码/排队变化应分别对照。请不要只看到 TCP 或 UDP 就判断输入注入链路是否正常。

真实崩溃收集事件查看器 Application Error 的故障模块、异常码、时间和两端版本；窗口隐藏或进托盘不等于崩溃。分享日志前人工脱敏，不公开完整配置、私钥和转储。

## English

Separate listening, authenticated TCP, authenticated UDP probes and actual media reporting. Check executable paths/PIDs and telemetry freshness before diagnosing a blank console. Empty trend charts are expected after start, during outages or when the host status file has not been updated: charts come from 300 in-memory one-second samples (about five minutes) or the tail of the host status file, and stale media samples (>5 s) render as a dash — no demo curves are generated. Events are capped at 100, deduplicated per category/message within a second and only appear on real state changes. A queue near 4 MiB means the tunnel is dropping whole frames rather than truncating them. "Current samples did not cross the hint thresholds" means the 1 MiB queue, 2% drop, 30 ms queueing and 16 ms decode lines were not crossed; it is not a quality score. The 2.2 pages require the matching `mslink` build; replacing only the GUI or only the tunnel leaves new fields missing. Persistent fallback needs valid-reply counters and source-endpoint inspection, not just a UDP send timeout. Two 2.1.3 regressions are fixed in 2.1.4: saving a controller connection failed when the ini did not exist yet, and leaked listening sockets could hang the first connection after a failed start — upgrade in place if you see either. Passwords stored as `dpapi:v1:` can only be decrypted by the same Windows user; after a downgrade or user change, re-enter them instead of editing the ini. Host passwords arrive on standard input, so seeing none on the command line is expected. At most 8 host handshakes wait for 5 seconds each; a full queue means slow or unexpected peers and is not a reason to bypass authentication. `tools/mslink_weaknet_e2e.py` reproduces controlled impairment locally: it is loopback, symmetric, uses synthetic echo payloads and exercises no GPU decode or presentation, so it cannot stand in for real WAN gaming or replace packet captures on the actual forwarded path. For input issues record focus/menu/mouse mode and whether video/audio continue. Share only redacted evidence and precise reproduction times.
