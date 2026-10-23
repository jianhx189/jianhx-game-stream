# 从 Moonlight / RustDesk 抄来的工程设定（以及为什么这么抄）

这一轮针对"**TCP/UDP 一直切换、鼠标卡顿、画面局部花屏**"，把两个成熟串流项目的关键设定
逐条对照实现。下面每一项都写明：来源、具体数值、我们怎么落地、解决了什么现象。

> 参考源码：`D:\xi\ref\moonlight-qt-master`（含 moonlight-common-c，commit f900dd4）、
> `D:\xi\ref\rustdesk-master`（fork）。行号以这两份为准。

---

## 0. 一句话对照表

| 设定 | 来源 | 我们的实现 | 解决的现象 |
|---|---|---|---|
| 缺帧/丢帧 → **严格等 IDR**，期间所有 P 帧丢弃 | Moonlight `VideoDepacketizer.c:106-114, 1079-1100` | `client_session.cpp` 的 `waitingIdr` 状态机 + 帧号连续性检查 | **画面局部花屏** |
| 丢包后**等第一个完整帧**再请求 IDR（防拥塞崩溃） | Moonlight `VideoDepacketizer.c:1084-1089` | `pendingIdrRequest`：丢包当刻只记录，收到完整帧后才发 KeyframeReq（100ms 节流、120 帧兜底） | 越救越卡 |
| 每帧独立 **FEC**，20% 量级开销 | Moonlight `SdpGenerator.c:225-231`、`RtpVideoQueue.c:261-269` | `src/common/fec.h` + 被控端每组 1 个 XOR 校验包；组大小按丢帧率自适应 4/5/8/12/20（25%~5% 开销） | 丢 1 片就整帧报废 → 帧率腰斩 |
| FEC 能补几个要跟着丢包率走 | Moonlight `minRequiredFecPackets=2`、RustDesk 自适应思路 | 被控端按 `FeedbackMsg` 的丢帧率增量改组大小（3 次同向才生效 + 2 档死区） | 校验开销与链路质量不匹配 |
| 解码器：`LOW_DELAY`、`OUTPUT_CORRUPT`、`SHOW_ALL`、`AV_EF_EXPLODE`、硬解单线程 | Moonlight `ffmpeg.cpp:513-547` | `decoder.cpp` 全部照设（硬解 `thread_count=1`、软解自动） | 解码内部攒帧、周期性顿一下 |
| 1 参考帧 / 1 slice / 无 B 帧 / 按需 IDR | Moonlight `SdpGenerator.c:463-475`、RustDesk `util.cpp` | `encoder.cpp`：`refs=1`、`slices=1`、`max_b_frames=0`、`forced-idr`、`repeat-headers=1` | DPB 重排延迟 |
| **关掉 intra-refresh** | （两边都不依赖它；RustDesk `hwcodec` 无此项） | `encoder.cpp` 删除该设置并写清理由 | 刷新周期内"一条持续糊块"= 局部花屏 |
| 呈现队列 3 帧封顶、满则丢最旧 | Moonlight `pacer.cpp:16-23, 394-401` | 接收队列 `kMaxQueue=3`（原 2 太小，偶尔顶掉整帧就等 IDR） | 偶尔卡一下 |
| 交换链多缓冲 + 帧延迟等待对象，**不阻塞 Present** | Moonlight `d3d11va.cpp:561-577, 826-827` | `renderer.cpp`：BufferCount 3、`FRAME_LATENCY_WAITABLE_OBJECT`、`SetMaximumFrameLatency(2)`、渲染前等句柄（8ms 超时） | Present 阻塞主循环 → 鼠标顿 |
| 鼠标位移合并 + 状态事件不合并 | Moonlight `InputStream.c:719-771`、RustDesk 输入处理 | 输入管线按事件类型分开：移动合并到 2ms（~500Hz），按键/滚轮立即发 | 1000Hz 鼠标刷爆通道 |
| 输入**不能**和大块视频排同一条可靠流 | RustDesk `connection.rs:480-498`（视频独立队列）、`video_qos/tests/sim.rs:82-84`（可靠流丢包 = 200-400ms 停顿） | 视频走 UDP 时输入走 TCP；**视频切 TCP 时键鼠自动改走 UDP**（状态事件 3 份冗余、移动 2 份） | 鼠标一顿一顿 |
| 可靠流上重复发送要能去重 | RustDesk `connection.rs:1248-1324`（单线程 FIFO） | 输入包带包序号 `UdpHeader.id`，被控端 64 个包的环形窗口整包去重 | 3 份冗余导致滚轮滚三格/位移三倍 |
| 相对位移钳制 ±10000 | RustDesk `input_service.rs:1351-1361` | `input_inject.cpp` 的 `clamp_delta()` | 异常巨值 → 视角甩飞 |
| 自适应码率：小步、滞回、呆滞期 | RustDesk `video_qos.rs:227-248, 775-795` + Moonlight `SdpGenerator.c:357-359` 的警告 | 3 秒一次：>20% 砍 30%、>10% 砍 15%、>4% 砍 5%、<0.5% 才 +3%；地板 3Mbps；两次调整至少隔 3 秒 | 清晰度忽高忽低、码率来回弹 |
| **用排队延迟判断拥塞**（不等丢包） | RustDesk `video_qos.rs:66, 104-116`（excess ≥150ms 记坏） | 客户端算"端到端延迟 − 近期最好值"= 排队延迟；>150ms 直接降档、>60ms 提前降档；同时显示在悬浮条/日志 | bufferbloat 时先花屏才发现 |
| 传输切换的滞回与冻结期 | RustDesk `io_loop.rs:273-430`（会话中途不换传输）、Moonlight 丢包阈值 3s 窗口 | 断流 2s / 整帧丢失 >30% 持续 3.5s / 等 IDR >2.5s 才切 TCP；TCP 侧 8s 稳定期、1.2s 探测窗、连续两次 ≥95% 才切回、观察期 20s 起翻倍 | **TCP/UDP 一直来回切换** |
| 关键帧要能独立解码（SPS/PPS 内联） | Moonlight `VideoDepacketizer.c:450-467`（SPS 判定 IDR） | Annex-B + `repeat-headers=1`，IDR 自带 SPS/PPS | 切流/丢包后解不了 |
| 静态画面靠心跳/光标包维持 UDP 映射 | Moonlight `VideoStream.c:68-81`（500ms ping） | 被控端每 500ms 至少发一个光标包；客户端据此判断"UDP 是否还活着" | 静止桌面误判断流 |

---

## 1. 为什么"等 IDR"是抗花屏的核心
H.264 的 P 帧参考前一帧。只要中间少一帧，后面所有帧都基于错误的参考解出来 ——
这就是玩家看到的"局部花屏"。Moonlight 在 H.264 + Windows 硬解下是**严格等 IDR**的
（`waitingForIdrFrame`，因为 D3D11VA 只对 HEVC/AV1 有 RFI 能力，AVC 没有）。

我们的实现（`client_session.cpp`）：
1. 帧号不连续 / 接收端报告丢帧 / 解码报错 → `waitingIdr = true`；
2. 等待期间所有非关键帧**不喂解码器**（不是"解出来看着烂"，是直接丢）；
3. 请求 IDR 的时机：**不在丢包瞬间**，而是等丢包后第一个完整帧到了再请求
   （Moonlight 注释：丢包瞬间求 IDR 会造成 congestion collapse）；
4. 120 帧（≈2 秒）还没等到 IDR 就强制再求一次，防请求本身丢在网络上。

代价是丢包后会冻结一小会儿（通常 1~2 帧），换来的是**永不花屏**。

## 2. 为什么 FEC 用 XOR 组码而不是 Reed-Solomon
Moonlight 每帧做一个 RS(data, parity)：1080p 用 20% 校验、最多能补 20% 的丢包。
RS 需要几百行 GF(256) 运算和整套编解码状态；而游戏串流实际遇到的绝大多数是
"随机丢 1~3 个包"。用**组大小为 G 的 XOR 组码**（每 G 片配 1 个校验包）：

- 1080p60 一帧约 30~40 个分片；按 G=4 时，5% 丢包下整帧存活率从 ~17% 提到 ~93%；
- 校验开销 = 1/G，可随丢包率在 5%~25% 之间自适应；
- 零依赖、零额外延迟（校验包跟数据片同一趟发出），实现只有 `fec.h` 几十行；
- 丢 2 片以上仍然救不回来 → 走"等 IDR"，而 IDR 我们做了**整帧重发两遍**，
  保证它自己能活下来（IDR 是唯一的恢复手段，却比 P 帧大一个数量级，最容易丢）。

## 3. 为什么不完全照抄 RustDesk 的"长 GOP / 无限 GOP"
RustDesk 默认 GOP=INT_MAX，靠"出问题就重启整个编码器"兜底。我们对齐的是它文档里
承认的代价：无限 GOP + 丢参考帧 = 长期花屏/黑屏，只能靠重启恢复。
所以保留 **2 秒 GOP（120 帧）+ 按需 IDR**：最坏情况 2 秒自然恢复，正常情况
一次 KeyframeReq 下一帧（≈16ms）就到。

## 4. 为什么输入要"跟着视频传输模式切换"
RustDesk 的仿真注释写得很直白：可靠流一旦丢包，会变成"200~400ms 停顿 + 1 秒半速"。
视频走 TCP 时，一帧几十~几百 KB 塞在同一条流里，键鼠包排在它后面 —— 这就是
用户感受到的"鼠标一顿一顿"。

所以：
- 视频走 UDP（正常情况）：控制通道很空，键鼠走 TCP `InputBatch`，可靠有序；
- 视频走 TCP（UDP 不能用）：键鼠**立刻改走 UDP**，状态事件连发 3 份、移动 2 份，
  被控端按包序号整包去重（重复份幂等，不等待重传，不会被大帧堵住）。

## 5. 还故意没抄的
| 设定 | 不抄的理由 |
|---|---|
| 动态分辨率切换（DRC） | Moonlight 明确关掉；切分辨率必须重协商 + 重建解码器，游戏中途切会黑屏 |
| 会话中途"按延迟换传输" | RustDesk 干脆不做；我们保留 UDP↔TCP 兜底，但加了强滞回+冻结期，实测不会来回抖 |
| 音频 10ms 帧 / 3s 环形缓冲 | 我们 20ms Opus + 40ms 抖动缓冲，延迟比 RustDesk 低一个量级；10ms 帧对音乐质量不友好 |
| Reed-Solomon 全帧 FEC | 见第 2 节：XOR 组码在 1080p60 的收益/复杂度比更高 |
| 固定码率（不做自适应） | 用户链路跨越 FRP/公网，带宽会变；我们做的是"带滞回的慢自适应"，比 Moonlight 的固定码率更适合这种环境 |

---

# 第二批：来自《UU远程 / GameViewer 串流流畅性工程设定分析报告》

报告位置：`D:\1\GM\GameViewer\串流流畅性工程设定分析报告.md`（对 GameViewer 4.31.1 发行版的
逆向分析，含 18 份会话统计 + 57,953 行逐秒时序）。它给出的实测数字补上了我们缺的几块。

## 6. 这一批采纳的设定

| 设定 | 报告实测值 | 我们的实现 | 解决的问题 |
|---|---|---|---|
| **输入必须离开可靠有序流** | 输入延迟 18~22ms，`p90 = p99`（无长尾）；TCP 最小 RTO 200ms | **默认输入走 UDP + 每包 3 份冗余**（状态事件间隔 2ms 发，避免突发丢三份）；视频被迫切 TCP 时才回落到 TCP 输入 | **鼠标卡顿**（最大单项） |
| **光标不进视频帧** | `cursor_blend:false` | 早就这么做：桌面复制不含光标，位置/形状单独通道，客户端本地绘制，绝对映射 1:1 | 光标被"编码+网络+解码+呈现"四重延迟 |
| **无限 GOP + 按需 IDR + 600ms 节流** | 24 万帧只有 7 个 I 帧；`min_keyframe_send_interval_ms=600` | 默认 `keyframe_interval_sec=0`（无限 GOP，10 分钟兜底）；只在缺帧/换目标时出 IDR；**最小间隔 600ms**，未满的请求保留延后执行 | 周期 IDR 造成的"规律性卡顿"与码率锯齿 |
| **VBV = target × 5 ÷ fps** | 1615/1615 次重配置零误差；60fps → 83ms | `rc_buffer_size = bitrate*5/fps`（原来取 1 秒，等于攒了 1 秒的突发额度）；热调码率时同步缩 | 复杂场景瞬间打满队列导致的周期性顿 |
| **NACK 为主、FEC 打底** | FEC 保护率恒定 ~25%（恢复 50%）+ NACK 随丢包 1:1（恢复 42%）= 90~94% | 新增 NACK：缺片 12ms 后请求、10ms 节流、每帧最多 2 次；被控端保留 **1 秒/32 帧/24MB** 发送历史补发；重传包打 `RETRANSMIT` 标记；**重传限速 ≤15% 总发送量** | 丢一个分片就废整帧 → 帧率腰斩 |
| **NACK 救回的帧要按帧号顺序交付** | （报告要求"绝不把不完整帧送进解码器"） | 接收端加 25ms 重排缓冲：新帧等前序帧补齐再交付；超时才接受跳号并等 IDR | NACK 让老帧后完成 → 乱序喂解码器 = 花屏 |
| **BWE 按「每秒」AIMD** | 无丢包 ×1.08/秒；丢包 4~13% ×0.94/秒；≥15% ×0.80/秒；排队时延涨 ×0.90/秒；降额冷却 2s；下限/初始 | 自适应码率改成 1 秒一拍、按上面系数走；起步取码率上限的 55%，不再一上来顶满；队列延迟 >80ms 即视为拥塞 | 码率震荡、开局拥塞 |
| **排队延迟判拥塞（不等丢包）** | 延迟瓶颈在抖动缓冲/组帧；pacer 延迟随 media/BWE 指数上升 | 客户端算"端到端 − 近期最好值"，>80ms 降档、>150ms 重降；同时显示在悬浮条与日志 | 花屏前就发现拥塞 |
| **延迟分位数 + jank 分级** | 小 jank 33~100ms、中 100~1000ms、大 >1000ms；判据 jank% <0.5%、p99 <150ms | 日志每 2 秒输出 `p50/p90/p99` 与 `jank 小/中/大` 计数 | 可度量、可回归（否则只能说"感觉卡"） |
| **MTU 1200 / 帧内分片 / 整帧丢弃** | `max_packet_size=1200`、缺片丢整帧 | 一致（我们本来就是 1200B + 整帧重组 + 缺片丢弃） | IP 分片放大丢包损失 |
| **静态画面降帧 + 缓存帧重发** | moving 60 / still 30，静止画面重编上一帧 | 桌面无变化时不编码；等 IDR 时重编缓存帧（并修掉"清了 pending 导致永远等不到 IDR"的旧 bug） | 静止桌面白烧码率/等不到 IDR |

## 7. 这一批明确没抄的（附理由）

| 设定 | 理由 |
|---|---|
| **KCP + RS-FEC 承载控制/输入** | 报告自己也说"用 TCP 控制 + UDP 输入也能达到同等效果，关键是输入离开可靠有序流"；引入 KCP 依赖不值得。我们已把输入挪到 UDP。 |
| **slice ≤1200B（单包单 slice）** | 那是为"允许部分 slice 丢失仍解码整帧"服务的；我们的模型是「整帧重组 + 缺片绝不送解码器」（抗花屏铁律）。两条策略互斥，选后者更稳。 |
| **WebRTC 式 pacing（5ms tick / ≤4 包/tick）** | 需要独立发送线程 + 定时器队列，改动面大；我们先用「FEC + NACK + 降码率」把丢包补回来。**待做**：如果 FRP 上仍出现突发丢包再加。 |
| **BGRA→NV12 shader + 零 staging** | 现在已有 GPU VideoProcessor 转换，编码延迟实测 ~1ms 级，不是当前瓶颈。 |
| **NVAPI P-State / realtime 优先级 / 关闭 HAGS** | 需要管理员 + NVIDIA 私有接口；收益是"编码延迟抖动"的边角，风险（提权/驱动兼容）更高。 |
| **IDD 虚拟显示器** | 支持无头/120+Hz 是另一个产品级需求，超出这轮修流畅性的范围。 |
| **初始码率固定 8.1Mbps** | 那是远程办公产品的档位；我们按「协商码率 × 55% 起步、×1.08/秒 爬升」自适应，游戏画质更好。 |

---

# 第三批：来自《逆向补充发现 v2》（二进制级）

报告位置：`D:\uu\逆向补充发现_v2.md`（PE 结构 / RTTI / HID 描述符 / EDID / 清单）。
这一轮是**二进制级**的，找到了前两轮看不到的运行时开关。

## 8. 这一批采纳的设定

| 设定 | 报告实测证据 | 我们的实现 | 解决的问题 |
|---|---|---|---|
| **MMCSS 多媒体线程调度** | streamer.dll 显式导入 `AVRT.dll!AvSetMmThreadCharacteristics` | 新增 `src/common/perf.h`（RAII 包装）；<br>被控端：`stream_loop`→`Games`、`input_loop`→`Pro Audio`；<br>远控端：主循环（解码+渲染）→`Games`、`VideoReceiver::thread_main`→`Pro Audio`、`InputPipeline` 线程→`Pro Audio` | **偶发的 10ms+ 帧间隔抖动（jank）**：MMCSS 会让调度器给这些线程更长连续时间片，并把系统计时器分辨率拉到 0.5ms，`Sleep(2)` / `MsgWaitForMultipleObjects(...,2,...)` 才真正精确（默认 15.6ms 分辨率下会睡到 15ms，帧间隔直接被抖成 30/60 来回跳） |
| **强制独显导出** | PE 显式导出 `NvOptimusEnablement` / `AmdPowerXpressRequestHighPerformance` | 被控端/远控端 `main.cpp` 各加两个 `__declspec(dllexport) DWORD` | 双显卡机器上被分到核显：被控端 DXGI 收不到独显显示器的内容，或远控端解码走核显慢路径 |
| **不要 requireAdministrator** | 所有二进制的 RT_MANIFEST 都是 `asInvoker`；高权限动作走 Windows 服务代办 | 我们本来就不提权（`tray.cpp` + 用户态进程）；文档里明确这条约束，防止以后有人改错 | 每次启动弹 UAC、RemoteApp/企业分发被拦 |

## 9. 这一批明确没抄的（附理由）

| 设定 | 理由 |
|---|---|
| **控制面换 protobuf RPC + oneof 信封** | 我们已经有自定义二进制协议（`protocol.h`），字段固定且只有 20 来个消息；换 protobuf 要引入依赖、重写收发两端、重新联调，收益主要在"字段可演进"。**待办**：如果将来要加文件传输/剪贴板/多端协商再考虑。 |
| **FeatureFlag 远端开关（11 个）** | 它们是商业产品要的灰度能力；我们是自用工具，所有开关本来就在 `config/*.ini` 里。 |
| **虚拟 HID 驱动（gvinput.sys 的 5 个报告描述符）** | 反作弊检测才是它的核心收益；我们走 `SendInput` 够用，做一个 KMDF 驱动 + 签名成本太高。 |
| **虚拟显示器复用真实 EDID（Dell S2719DGF / Lenovo Y27fA）** | 仅在做 IDD 无头虚拟屏时才有意义；当前先不做 IDD。 |
| **NVENC SDK 双版本静态编译（11.1.5 + 8.0.14）** | 我们用 FFmpeg 动态链接到系统 NVIDIA 驱动，不需要把 SDK 静态编进去。 |
| **FFmpeg 软解兜底（H.264/HEVC/VP9/AV1 + DXVA2）** | 已有 `MINISTREAM_FORCE_SW_DECODE` 排障开关；多 codec 支持超出当前范围。 |
| **OpenEXR / Sentry / 神策埋点** | 与"流畅性"无关；崩溃转储已有 `crash.cpp` + dumpbin 手工分析够用。 |
| **Poco / Qt5 / Duilib / cpp-httplib / websocketpp** | 依赖引入代价大；当前 UI 是 Win32 + D3D11，够用。 |
| **27MB 高熵段 / IOCTL 反汇编 / protobuf 完整 schema** | 报告自己也标"未解决"；等真要用到那些能力再回去挖。 |

---

# 第四批：码率与拥塞控制对齐 RustDesk 源码（本轮最关键）

用户反馈「同一个 FRP 服务器，RustDesk 比我这个流畅」。逐条比对 RustDesk 源码
（`D:\xi\ref\rustdesk-master\rustdesk-master`）后，找到**数量级级别的差异**：

| 项 | RustDesk | 我们（改前） | 改后 |
|---|---|---|---|
| 1080p 基准码率 | 2073 kbps（`libs/scrap/src/common/codec.rs:929-959`），Balanced ×0.67，H.264 硬编 ×2 ≈ **2.8 Mbps** | **20 Mbps**（4K 给 80 Mbps） | **8 Mbps**（1080p60，按像素/帧率缩放，上限 30 Mbps） |
| 码率上限来源 | `BR_MAX=40.0`、按质量系数缩放 | 协商码率 ×0.85 | 同左，但基数小了 2.5 倍 |
| 拥塞信号 | `TestDelay` 探针在**同一条视频 TCP 流**上往返，测的是**队列**而不是链路 RTT（`video_qos.rs:42-47`） | 丢包 + 「端到端 − 最好延迟」 | 排队延迟为主信号 + 丢包为辅 |
| 降档力度 | 排队 >500ms ×0.5、>300ms ×0.7、>150ms ×0.9（`video_qos.rs:227-248`） | 仅 >80ms ×0.90 | 同 RustDesk 分档 |
| 升档步长 | ×1.10，且每步最多 +150 kbps（`:775-795`） | ×1.08/秒，无绝对上限 | ×1.10，每秒最多 +450 kbps |
| TCP 发送 | 一帧一次 `Stream::send`，写超时 12s（`connection.rs:1074-1084`） | 同左（整帧一次写） | 同左 + **码率压到 12 Mbps** + 发送超一帧间隔就跳帧追赶 |
| 跳帧策略 | 服务端不跳帧，只降 fps/码率；客户端队列 120 帧溢出丢最旧 | 只有队列 3 帧 | TCP 模式主动跳帧保延迟 |

**为什么这一条最关键**：码率是「管道里的水」。高 7~10 倍的码率打在中继上，
无论 TCP 还是 UDP 都只会把数据堆进缓冲区 —— 延迟随之上涨，用户感觉就是
「越用越卡、点了半天才动」。降到 RustDesk 同一量级后，同一条链路才有余量。

**实测（1080p60 回环）**：端到端 12.6~14 ms、丢帧 0.0%、码率从 4.4 Mbps 平滑爬到
6.8 Mbps（ABR 上限），未再出现「排队 150ms+ → 猛降 → 再爬」的锯齿。

---

# 第五批：Parsec 客户端静态逆向（`D:\xi\parsec-windows.exe`）

该文件是 **Parsec 官方 NSIS 安装包**（Unity Technologies SF 签名，4.4MB），内嵌完整客户端：
`parsecd-150-104a.dll`（6.2 MB，真正的串流引擎）、`parsecd.exe`、`pservice.exe`、
虚拟显示器/虚拟 USB 驱动。外层 exe 里没有任何调参字符串，全部在 `parsecd` 内。

## 挖到的、直接照抄的配置面（163 个键）

| 类别 | Parsec 键 | 我们的落地 |
|---|---|---|
| 编码 | `encoder_bitrate`、`encoder_fps`、**`encoder_min_qp`、`encoder_max_qp`**、**`encoder_slices`**、**`encoder_vbv_max/initial/multi`**、`encoder_idr_interval` | 已加 `min_qp / max_qp / slices / vbv_multi` 四个 `host.ini` 键（`encoder_idr_interval` 对应已有 `keyframe_interval_sec`） |
| 解码 | `client_decoder_h265`、`client_zero_copy`、`client_vsync` | 我们默认就零拷贝 + 关 vsync；H.265 已在画质面板可切 |
| 网络 | `network_protocol`、`network_latency`、`network_cg_level`、`network_fast_relay_ping`、`network_sendmsg/recvmsg` | 我们的对应物：UDP/TCP 传输切换、排队延迟降档、探测窗口 |
| 带宽档位 | UI 预设 **3/5/7/10/15/20/25/30/35/40/45/50 Mbps**，文档示例 `# encoder_bitrate = 10` | 印证了「默认 10 Mbps 量级」才是常态——我们把 1080p60 默认从 20 降到 8 Mbps 是对的方向 |

## 传输层证据（比参数更重要）

- 字符串里**完全没有** `udp`/`nack`/`fec`/`jitter`，但有
  `TSN %8.8x retransmitted %d times, giving up`、
  `Exceeding mtu of %d out size is %d`、
  `Warning chunk of %d bytes > mtu:%d and yet PMTU disc missed`
  → Parsec 的 UDP 是**带 TSN 序号 + 重传 + PMTU 探测的可靠 UDP**（类 UDT/SCTP），
  不是裸 RTP；这和我们「NACK 重传 + 固定 1200B 分片」是同一思路。
- `cg event: cwindow: %u, dz: %u, rtt: %f` + `congestion_algorithm` 键
  → 拥塞控制是**基于 RTT 的拥塞窗口（cwnd）+ 延迟区间（dz）**，
  不是单纯码率 AIMD。我们下一步可以把「每帧在途包数上限」也做成 cwnd 形式，
  比现在只调码率更贴近它。
- `nvEncodeAPI64.dll` / `avcodec-*.dll` 都是 `LoadLibrary` 动态加载（可降级）；
  音频走 WASAPI + Opus，**没有 avrt.dll** —— 说明 MMCSS 不是它流畅的必要条件，
  真正起作用的是码率与拥塞控制。

## 本轮落地

- `host.ini` 新增 `min_qp / max_qp / slices / vbv_multi`（默认 -1/-1/1/5，即与之前行为一致）
- 实测（1080p60 回环）：**61.0 fps / 7651 kbps / 丢帧 0.0% / 端到端 13.0 ms / 硬解 0.66 ms**

---

# 第六批：Parsec 传输层落地（cwnd + NACK 截止）

Parsec 逆向确认其传输栈是 **DTLS + SCTP(dcsctp) + PR-SCTP(FORWARD-TSN)**：
拥塞控制是 SCTP 的 cwnd（`cg event: cwindow/dz/rtt`），过期数据用 FORWARD-TSN
直接放弃不重传。对应落地：

| Parsec | 我们 |
|---|---|
| cwnd（在途数据量上限） | `VideoSender::update_cwnd`：cwnd = 码率 × RTT × 1.5；过去一个 RTT 窗口发满就丢整帧（不排队）。RTT 来自客户端 250ms 反馈 |
| PR-SCTP 过期放弃 | NACK 40ms 截止：有序交付缓冲只有 25ms，40ms 后救回来的片交付时必然乱序，不再请求，交给"等 IDR" |
| PMTU 探测 | 固定 1200B（FRP/公网够用，暂不自适应） |

---

# 第七批：双线（主线视频 + 备线键鼠）

用户实际场景：主 FRP 带宽大但偶尔抖（UDP 视频），备线延迟低但带宽小（TCP）。
思路：**键鼠走备线 TCP，视频走主线 UDP**，互不抢占。

| 实现 | 说明 |
|---|---|
| 被控端 `backup_port` | 第二条 TCP 监听，只收 InputBatch/Ping/Pong，不传视频 |
| 客户端 `backup_host`/`backup_port` | 主连接成功后自动连备线（简化握手，只做口令校验） |
| 键鼠迁移 | InputBatch 自动走备线；备线断开时回退主线 |
| 实测（回环双线） | 60 fps / 0% 丢帧 / 端到端 12.5 ms / 备线握手正常 |

配置示例：
```ini
# 被控端 host.ini
[server]
port=14738
backup_port=14740        # 0 = 不启用

# 远控端 client.ini
[client]
host=192.0.2.10
port=14738
backup_host=192.0.2.11 # 备线公网地址
backup_port=14740
```
