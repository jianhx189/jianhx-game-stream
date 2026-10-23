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

## 为什么进入 TCP 兼容通道

客户端 2.5 秒内没有确认 UDP 时临时回退，仍会持续探测。先看 `udp_probe_sent`、`udp_probe_ack`、`udp_last_send_error`、`udp_data_received`；不要仅凭 `udp_auth_rejected=0` 判断没有收到包。

2.1.2 被控端新增 `udp_hello_received`、`udp_hello_ack_sent`、`udp_session_rejected`、`udp_reply_errors` 和 `udp_reply_route_pinned`。客户端探测增长但 Host 没有 HELLO：查 UDP 目标地址/端口和转发。Host 有 HELLO/ACK 但客户端 ACK 为零：核对返回路径及应答源端点。认证/会话拒绝增长：查两端版本、会话与实现，不要绕过认证来“修好网络”。

已修复案例：多网卡 wildcard 监听收包地址 A，普通 sendto 从地址 B 回包。2.1.2 改用 IP_PKTINFO。实测恢复 UDP；详见 UDP 多网卡修复说明。普通 UDP 字符串不会得到 mslink 回显，不能把静默丢弃当作端口关闭。

## 鼠标与偶发卡顿

记录精确时间，确认画面/声音是否继续；区分桌面与游戏、窗口焦点、菜单、鼠标相对/绝对模式。网络 RTT/丢帧变化与解码/排队变化应分别对照。请不要只看到 TCP 或 UDP 就判断输入注入链路是否正常。

真实崩溃收集事件查看器 Application Error 的故障模块、异常码、时间和两端版本；窗口隐藏或进托盘不等于崩溃。分享日志前人工脱敏，不公开完整配置、私钥和转储。

## English

Separate listening, authenticated TCP, authenticated UDP probes and actual media reporting. Check executable paths/PIDs and telemetry freshness before diagnosing a blank console. Persistent fallback needs valid-reply counters and source-endpoint inspection, not just a UDP send timeout. For input issues record focus/menu/mouse mode and whether video/audio continue. Share only redacted evidence and precise reproduction times.
