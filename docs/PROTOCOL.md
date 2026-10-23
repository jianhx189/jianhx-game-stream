# MiniStream 协议规范 v1

一个刻意做小的自用协议。目标只有两个：**把画面尽快送过去**，**把键鼠尽快送回来**。
不追求通用性，不追求向后兼容，不做加密体系。

---

## 1. 总体结构

```
远控端 Client  ──TCP(控制通道)──▶  被控端 Host
               ◀────────────────
               ──UDP(视频)──────▶
               ──UDP(输入/反馈)──▶
               ◀────────────────
```

- **一个端口号同时承载 TCP 与 UDP**。默认 `20000`，要求 `> 10000`。
- FRP 侧需要**两条规则**：一条 TCP、一条 UDP，都指向同一个端口。
- 所有多字节整数一律**小端**。协议结构体全部手写序列化（`src/common/serialize.h`），
  不依赖编译器 packing，保证 MSVC / GCC 生成同样的字节。

为什么控制走 TCP、视频输入走 UDP：
TCP 的按序重传对握手和心跳是好事，对流媒体是灾难（一个丢包会卡住后面所有数据）；
UDP 丢一帧的代价远小于等一个重传。

---

## 2. TCP 控制通道

### 2.1 帧格式

```
偏移  长度  字段
0     4     length    = 4 + payload_size（不含自身这 4 字节）
4     2     type      MsgType
6     2     reserved  必须为 0
8     N     payload
```

`length` 上限 4 MiB（`kMaxTcpPayload`）。解析见 `proto::parse_tcp_header`。

### 2.2 消息类型

| 值 | 名称 | 方向 | 用途 |
|----|------|------|------|
| 1 | `Hello` | C→S | 版本、客户端 nonce、期望的视频参数 |
| 2 | `Challenge` | S→C | 服务端 nonce + salt |
| 3 | `Auth` | C→S | 口令应答 proof |
| 4 | `AuthResult` | S→C | 校验结果 + sessionId |
| 5 | `StreamReady` | S→C | 实际生效的编码参数 |
| 6 | `Ping` | C→S | 心跳 + 时钟同步 T1 |
| 7 | `Pong` | S→C | T1 回显 + T2/T3 |
| 8 | `KeyframeReq` | C→S | 请求 IDR |
| 9 | `SetBitrate` | C→S | 动态调整码率 |
| 10 | `Feedback` | C→S | 统计/延迟反馈（也走 UDP，TCP 版本为备用） |
| 11 | `Disconnect` | 双向 | 断开并说明原因 |
| 12 | `SetKeyframeInterval` | C→S | 热调关键帧间隔（秒） |
| 13 | `Reconfigure` | C→S | 请求重协商（分辨率/帧率变化） |
| 14 | `Reconfigured` | S→C | 重协商完成，参数同 `StreamReady` |
| 15 | `VideoOverTcp` | C→S | 传输模式/探测请求：`[u8 mode][u8 probe][u8 rsv[2]]`（mode 0=UDP 1=TCP；probe=1 只探测不改模式；空 body = 切 TCP，兼容旧版） |
| 16 | `VideoFrame` | S→C | TCP 视频整帧：`[u32 frameId][u8 flags][u8 rsv[3]][u32 sendTsUs][u32 frameBytes][data]` |
| 17 | `PointerShape` | S→C | 光标形状：`[u32 shapeId][u16 w][u16 h][u16 hotX][u16 hotY][w*h*4 预乘BGRA]`（只在形状变化时发） |
| 18 | `PointerPos` | S→C | 光标位置/可见性（TCP 兜底；正常走 UDP）：`[u32 seq][u16 x][u16 y][u8 visible][u8 rsv]` |

### 2.2.1 视频传输模式与自愈探测（`VideoOverTcp` / `VideoFrame`）

UDP 视频在 FRP / 防火墙等环境下可能单向不通。远控端的策略是**双向自愈**：

1. 会话建立后默认走 UDP；
2. **真的不能用**才切 TCP（三个判据，全部带冷却与观察期，避免来回抖）：
   ① 连续 2 秒一个 UDP 包都没收到（被控端每 500ms 至少发一个光标包，等于 UDP 被拦/断流）；
   ② 整帧丢失率 >30% 持续 3.5 秒；
   ③ 连续 2.5 秒等不到 IDR（丢包重到关键帧都拼不出来）。
   切之前还要满足：开局 6 秒后、距上次切换 >15 秒、不在"刚切回 UDP"的观察期内。
   切换后远控端发 `VideoOverTcp{mode=1}`，被控端此后每帧编码结果**整帧**通过控制通道
   以 `VideoFrame` 发出（控制通道的 `send_frame` 已加锁，控制循环回 Pong 与串流线程
   发帧不会交错）。TCP 模式零丢片、画面稳定，代价是延迟略高（实测 11~30ms）；
3. TCP 模式下远控端每 5 秒发一次 `VideoOverTcp{probe=1}`：被控端在 **1.2 秒**窗口内
   **同时**用 UDP 发视频（不改当前模式）。远控端以同一窗口内的 TCP 帧数为分母
   计算 UDP 到达率，**连续两次 ≥95%** 才发 `VideoOverTcp{mode=0}` 切回 UDP；
   切回后进入观察期（20 秒起，每次抖动翻倍，最长 5 分钟），期间不再切走。
   窗口内一帧 TCP 帧都没有（画面静止）时这次探测作废、不下结论；
4. 探测窗口内丢失的 UDP 帧**不计入**丢帧统计、也不触发"等 IDR"——它们和画面无关；
5. 被控端每次模式切换都强制补一个 IDR，保证立刻能出画。

### 2.2.2 远端光标（`PointerShape` / `PointerPos` + `UdpType::Pointer`）

Desktop Duplication 的桌面纹理里**不含光标**，必须单独传，否则远控端只能靠本机
光标——而捕获游戏时本机光标被 `ShowCursor(FALSE)` 藏起来了，表现为"连上后没有鼠标"。

- 被控端每帧取 `GetCursorInfo`（可见性 + 位置）和
  `IDXGIOutputDuplication::GetFramePointerShape`（光标图片），把三种 DXGI 光标
  （`MONOCHROME` / `COLOR` / `MASKED_COLOR`）统一转成**预乘 alpha 的 BGRA**；
- **位置/可见性**走 UDP（`UdpType::Pointer`，净荷同 `PointerPos`）以追求低延迟，
  同时它也是"被控端→远控端"这条 UDP 路径的持续探针；TCP 模式下再走
  `PointerPos` 兜一份；
- **形状**走 `PointerShape`（TCP，几 KB，只在形状变化时发）；
- 远控端按 `seq` 丢弃乱序旧包，按当前缩放模式把远端坐标映射成屏幕坐标后用 D2D
  画在视频之上，并隐藏本机系统光标（避免"两个鼠标"）。

**`visible` 的语义**就是被控端「系统光标此刻是否可见」：游戏进内场会自己
`ShowCursor(FALSE)`，该值变 0；回菜单又变 1。远控端照抄即可，因此菜单里有鼠标、
进游戏自动消失，无需猜测窗口类型。`seq` 为 `u32` 单调递增，比较时按有符号差值
处理以兼容回绕。

### 2.2.3 音频（AudioPacket / UdpType::Audio）

被控端把**系统正在播放的声音**（游戏/音乐）传给远控端。格式固定、不协商，
两端版本不一致时靠 selftest 里的固定参数断言兜住：

| 参数 | 值 | 说明 |
|---|---|---|
| 采样率 | 48000 Hz | 固定 |
| 声道 | 2（立体声） | 固定 |
| 帧长 | 20ms = 960 采样 | 固定 |
| 编码 | Opus（libopus） | 默认 96kbps，被控端可配 [audio] bitrate_kbps |
| 时间戳 | u32 ts | 48kHz 采样计数，约 24.8 小时回绕 |

传输：

- 正常走 **UDP**（UdpType::Audio），每个包**发两份**抗丢包；远控端按 seq
  去重，丢一包用 Opus PLC/静音补，听感影响很小；
- 视频切到 **TCP 兜底**时（§2.2.1），被控端**同时**用 AudioPacket 走控制通道
  发一份，保证"UDP 不通时声音也不断"；
- 远控端播放侧有 40ms 抖动缓冲、超过 90ms 自动裁剪到 40ms 压延迟；
  被控端没有可用声卡时只是**没声音**，视频/键鼠不受影响。

### 2.3 握手与密码校验

需求写的是"明文 + 密码校验"，但直接传明文密码在网络上是白白送人。这里用一个
最简挑战-应答，代价是 40 行 HMAC，收益是密码不出本机、且能防重放：

```
key   = SHA256("MiniStream/v1" || password)
proof = HMAC_SHA256(key, clientNonce || serverNonce || salt)
```

时序：

```
Client                                          Server
  |-- Hello{version, nonce[16], 期望参数} ------->|
  |<-- Challenge{serverNonce[16], salt[16]} -----|
  |-- Auth{proof[32]} -------------------------->|  服务端本地算 proof 并常量时间比较
  |<-- AuthResult{ok, sessionId, message} -------|
```

- 密码不来不回传，只传 HMAC 结果。
- `clientNonce` / `serverNonce` / `salt` 每次连接都重新随机（`BCryptGenRandom`）。
- 比较用常量时间实现，避免时序侧信道。
- 校验失败服务端回 `AuthResult{ok=0, message="验证码错误"}` 后立即断开。

> 注意：视频流本身**不加密**。这是明确的设计取舍（自用 + 性能优先）。
> 若将来要加密，最小改动是在 UDP 载荷上用 ChaCha20，密钥由上面这个 `key` 派生。

### 2.4 会话建立之后的顺序

1. `AuthResult{ok=1, sessionId}` 发出。
2. 远控端**立刻连发 3 个 UDP 保活包**（`UdpType::Keepalive`，带 sessionId）。
   目的是让被控端学到 NAT/FRP 之后远控端真实的 UDP 源地址。
3. 被控端在 1.5 秒内等到保活包就用它作为视频发送目标；
   等不到则退化为使用 TCP 对端的 IP + 同端口（FRP 双向映射一致时也能通），并记一条警告。
4. 被控端初始化采集与编码器，然后发 `StreamReady{codec, width, height, fps, bitrateKbps, monitorName}`。
5. 之后远控端每 250 ms 发一次反馈，每 100 ms 轮询一次控制消息；`Ping` 由控制线程按周期发出。

### 2.5 心跳与断线检测

- 远控端周期性发 `Ping{seq, t1}`（t1 = 本地微秒时钟）。
- 被控端回 `Pong{seq, t1, t2, t3}`，其中 t2 = 收到 Ping 的时刻，t3 = 发出 Pong 的时刻。
- 远控端按迷你 NTP 公式估计时钟偏移与 RTT：

```
rtt    = (t4 - t1) - (t3 - t2)
offset = ((t2 - t1) + (t3 - t4)) / 2         // 远端时钟 - 本地时钟
```

  只采信 RTT 最小的样本（网络抖动会污染 offset），再做 1/64 的慢速跟随防漂移。
- 被控端**20 秒**没收到任何控制消息才判定掉线；远控端**25 秒**没收到任何消息才判死。
  阈值放这么宽是有实测原因的：有的机器在会话刚建立时会整进程停顿 5~10 秒
  （CPU 时间完全不动，连 Ping 都发不出来），3 秒/8 秒都会把这种暂时卡顿误判成掉线，
  表现就是"连上黑屏几秒然后断开"。
- 任何一侧断开都要调用 `release_all()` 抬起所有按键（见 §5.4）。

---

## 3. UDP 视频通道

### 3.1 固定 28 字节头

```c
struct UdpHeader {          // 小端，sizeof == 28
    u8  type;               // UdpType
    u8  flags;              // kUdpFlagKeyframe / kUdpFlagFec
    u16 payloadSize;        // 净荷字节数（不含本头）
    u32 sessionId;          // 来自 AuthResult，不匹配的包直接丢
    u32 id;                 // 视频: frameId 递增；输入: 输入包序号（整包去重用）
    u16 index;              // 视频: 分片下标；FEC 校验包: 组下标
    u16 count;              // 视频: 本帧分片总数
    u32 sendTsUs;           // 发送时刻微秒（低 32 位）
    u32 frameBytes;         // 视频: 整帧编码字节数
    u32 extra;              // 输入: 事件个数；FEC 校验包: 组大小 G；其它: 0
};
```

`sendTsUs` 只存 32 位：60 fps 下微秒计数要 71 分钟才回绕一次，而一次会话里
帧龄永远是毫秒级，用远控端估计出的时钟偏移把高位补回来即可
（`client_session.cpp::rebuild_remote_ts`）。

### 3.1.1 `type` 字段

| 值 | 名称 | 方向 | 净荷 |
|----|------|------|------|
| 1 | `Video` | S→C | 视频分片（见 §3.2）。`flags` 带 `kUdpFlagFec` 时是 FEC 校验包（§3.4），带 `kUdpFlagRetransmit` 时是 NACK 重传副本（§3.6） |
| 2 | `Input` | C→S | 输入事件包（见 §4.1）。`id` = 输入包序号（整包去重用） |
| 3 | `Feedback` | C→S | 实时统计（见 §4.4） |
| 4 | `Keepalive` | C→S | 空包，维持 NAT/FRP 的 UDP 映射 |
| 5 | `Pointer` | S→C | 光标位置/可见性，净荷同 `PointerPos`（见 §2.2.2）。也是下行 UDP 探针 |
| 6 | `Audio` | S→C | 裸 Opus 数据；seq 在 `id`，时间戳在 `sendTsUs`（§2.2.3） |
| 7 | `Nack` | C→S | 缺片重传请求（见 §3.6）：`id` = frameId，`payload = [u16 count][u16 fragIdx × count]` |

`flags` 位定义：`0x01` = KEYFRAME，`0x02` = FEC 校验包，`0x04` = NACK 重传副本。

### 3.2 分片规则

- 每帧编码数据从偏移 0 开始切成 `kMaxVideoPayload = 1200` 字节的块。
- 第 i 片在整帧中的偏移**恒为** `i * 1200`，长度是该片净荷长度（最后一片可能更短）。
- 头部 28 + 净荷 1200 = 1228，加 UDP/IP 头 28 = 1256 < 1500，不会 IP 分片；
  也为 FRP 隧道封装留了余量。
- **关键帧标志只打在 index == 0 的包上**。

### 3.3 收端重组与低延迟取舍

- 按 `frameId` 重组，维护 `have[]` 位图；**FEC 恢复之后**仍缺片才丢整帧。
- 未完成帧超过 **120 ms** 未收齐即丢弃（但 FEC 校验包一到就会先尝试救回缺片）。
- **连续丢 2 帧**即置位"建议请求关键帧"。
- 完整帧进队列，队列深度**最大 3 帧**；满了就丢**最旧**的。
  串流要的是"最新画面"而不是"完整画面"，积压只会让延迟越滚越大；
  但太小（2）会在解码/呈现偶尔多花几毫秒时顶掉整帧 → 又要等 IDR，所以留 3 帧余量。
- 重组缓冲中的 `frameId` 落后当前最高值超过 16 的条目会被清掉，防止内存增长。
- **帧号出现空洞**（接收端丢掉整帧 / 队列溢出 / 换传输的空档）时，远控端进入
  "WAIT_IDR"：后续 P 帧一律不喂解码器，只等关键帧 —— 这是"不花屏"的关键
  （H.264 的 P 帧参考前一帧，带着坏参考继续解就是花屏）。

### 3.4 FEC（XOR 组码）

v1 不做重传。抗丢包由"**FEC 校验包 + 关键帧重发 + 按需 IDR**"三层组成：

- 一帧切成 `total` 个数据分片，每 **G** 个连续分片组成一组，组内补 1 个校验包；
  校验净荷 = 组内各分片净荷「补零到组内最长」后逐字节 XOR，长度 = 组内最大分片长度；
- 校验包头部：`type=Video`、`flags |= kUdpFlagFec`（bit1）、`id=frameId`、`index=组下标`、
  `count=总分片数`、`frameBytes=整帧字节数`、`extra=组大小 G`、`payloadSize=校验长度`、
  `sendTsUs` 与数据片相同。**`extra` 存的是 G，不是本组成员数**（末组要用
  `min(G, count - index*G)` 推成员数与起始下标）；
- 校验包在每组数据片发完**立刻**发（不攒到帧尾），开销 = 1/G；
- 收端每收到一个校验包就检查该组是否"恰好只缺 1 片"，是就当场恢复并继续重组；
  缺 2 片以上仍按丢帧处理（走 WAIT_IDR）；
- G 由被控端按远控端反馈的**帧丢失率**自适应：<0.5%→20、<2%→12、<5%→8、<10%→5、
  更差→4（即 5%~25% 开销），连续 3 次同向且变化 ≥2 档才生效（防抖 + 死区）；
- **关键帧的每个分片发两遍**：关键帧比 P 帧大一个数量级、却是唯一恢复手段，
  两份独立丢包把单帧存活率从 ~70% 提到 99% 以上（代价平摊到 2 秒 GOP 上约 1% 码率）；
- 迟到包（所属帧已经完成/丢弃之后才到，典型是最后一批校验包）直接丢弃，
  否则会凭空造出"缺一整帧"的假丢包统计。

### 3.5 码流格式

- H.264（`VideoCodec::H264`），Annex-B 起始码。
- 编码器设置 **不启用 `AV_CODEC_FLAG_GLOBAL_HEADER`**，并显式开
  `repeat-headers=1`，因此 SPS/PPS 会内联在每个 IDR 之前，
  远控端**丢包后只要拿到下一个 IDR 就能独立恢复**，不需要带外参数集。
- 低延迟设置：无 B 帧、无 lookahead、CBR、`refs=1`、`slices=1`、
  `AV_CODEC_FLAG_LOW_DELAY`、NVENC 用 `preset=p1 / tune=ll / zerolatency=1 / forced-idr=1`。
- **不启用 intra-refresh**：它会把 IDR 摊成逐帧刷新的条带，刷新周期内丢包造成的
  糊块会滞留一整秒（表现为"局部花屏"），而且让远控端等不到干净的重同步点。

### 3.6 NACK（缺片重传）

FEC 只是"恒定打底"，**真正的丢包恢复主力是 NACK**（UU远程/GameViewer 实测：
FEC 恢复 50% + 重传恢复 42%，叠加覆盖 90~94%；重传量随丢包近似 1:1 增长）。

- 收端：帧缺片时先给乱序/FEC **12ms** 机会；仍缺则发 `UdpType::Nack`。
  **每 10ms 最多一次、每帧最多 2 次**（防 NACK 风暴）；单包最多 64 个下标。
- 发端：保留最近 **1 秒 / 32 帧 / 24MB** 的发送历史；收到 NACK 后按 `frameId` 找到原帧，
  把这些分片原样重发一遍（`flags |= kUdpFlagRetransmit`）。
  **重传流量限速 ≤ 总发送量 15%**，否则越补越丢。
- 超过重组窗口（120ms）仍缺片 → 丢弃整帧、进入"等 IDR"。
- 收端对重传包按 `index` 幂等处理；并且**已交付帧号的包一律丢弃**。
- 重排缓冲：NACK 救回来的帧可能比后续帧晚完成。收端把已凑齐的帧暂存 **25ms**，
  严格按 `frameId` 递增顺序交付；超时才接受跳号（并等 IDR）。
  —— 乱序把帧喂给解码器一定会花屏，这条和"不完整帧不送解码器"同等重要。

---

## 4. UDP 输入与反馈通道

### 4.1 输入事件（固定 12 字节）

```c
struct InputEvent {         // 小端，sizeof == 12
    u8  type;               // InputType
    u8  flags;              // kInputFlagKeyUp / kInputFlagKeyExt / kInputFlagAbsMove
    u16 seq;                // 事件序号，用于去重
    i32 a;
    i32 b;
};
```

净荷 = `u16 count` + `count × 12` 字节（`proto::encode_input_packet`）。

| type | 名称 | a | b |
|------|------|---|---|
| 1 | `MouseMove` | dx（相对）；带 `kInputFlagAbsMove` 时是远端像素 x | dy / 远端像素 y |
| 2 | `MouseButton` | 1左 2右 3中 4侧1 5侧2 | 1按下 / 0抬起 |
| 3 | `MouseWheel` | 垂直增量（120 的倍数） | 水平增量 |
| 4 | `Key` | **扫描码** | — |
| 5 | `ReleaseAll` | — | — |

`MouseMove` 有两种语义（见 `kInputFlagAbsMove`）：

- **相对模式**（不带标志）：a/b 是原始位移，被控端用 `SendInput(MOUSEEVENTF_MOVE)`
  注入，游戏从 Raw Input 读位移。用于游戏内场（本机光标被隐藏）。
- **绝对映射**（带 `kInputFlagAbsMove`）：a/b 是**远端桌面像素坐标**，被控端用
  `SetCursorPos` 直接摆放光标。用于桌面/游戏菜单：远控端按当前缩放模式把本机
  光标位置换算成远端坐标发过去，所以远端光标和用户本机鼠标 **1:1 重合**，
  不受系统鼠标加速影响（这也是"光标乱飘"的根治办法）。

`Key` 用扫描码而不是虚拟键码：DirectInput / Raw Input 的游戏只认扫描码。
远控端用 `MapVirtualKeyW(vk, MAPVK_VK_TO_VSC_EX)` 转换，扩展键的高位字节转成
`flags` 的 `kInputFlagKeyExt` 位（`proto::make_scancode`）。

远控端的鼠标模式是**自动**的：被控端上报的"系统光标是否可见"为真（桌面/菜单）
就走绝对映射；光标被隐藏（游戏内场）就切回相对模式。`client.ini` 的
`input.mouse_mode = auto|absolute|relative` 可强制。

### 4.2 丢包容忍策略

- **输入主路径默认是 UDP**（不是 TCP）。理由：TCP 的队头阻塞 + 最小 RTO 200ms，
  一次丢包就让鼠标"粘住" 200ms 以上；而鼠标移动是可丢弃的状态量。
  UU远程/GameViewer 实测输入延迟 18~22ms 且 `p90 = p99`（无长尾），前提就是
  输入不排在任何可靠有序流后面。
  只有视频被迫切到 TCP（说明 UDP 基本不通）时，才回落到 TCP `InputBatch`。
- **鼠标移动**：远控端把连续位移**合并到 2 ms（约 500Hz）**再发。1000Hz 游戏鼠标
  如果每个回报都单独成包，会把输入通道和被控端注入线程一起刷爆，手感没有区别；
  合并只发生在"纯移动"批次里，遇到按键/滚轮立刻截断，顺序不会乱。
- **按键与鼠标按钮**：不合并，立即发。同一批事件**连发 3 份**（`UdpHeader.id` 相同，
  被控端整包去重），三份之间隔 2ms —— 突发丢包常会一次丢一串，紧挨着发的三份会一起丢。
- **滚轮**：同按键（不合并、连发 3 份）。滚轮被重复执行是可见 bug，所以去重必须可靠。
- **相对位移钳制**：单轴 ±10000（异常巨值会把视角甩飞）。
- **包序号**：`UdpHeader.id` 每个包 +1、同一包的 3 份副本共用同一个 id、**不要用 0**
  （0 表示"未填序号"）；被控端用最近 64 个 id 的环形窗口整包去重。

### 4.3 兜底：不能有卡键

`InputInjector::release_all()` 在以下时机必须被调用：
- 控制通道断开
- 会话结束
- 远控端退出捕获

它会把 `keysDown_` / `buttonsDown_` 里记录的所有按下项逐个抬起。
没有这一步，一次断线就会留下"W 键一直按着"的经典事故。

### 4.4 Feedback（远控端 → 被控端）

`FeedbackMsg` 每 250 ms 发一次，字段：`lastFrameId, framesOk, framesLost, recvKbps,
decodeUs, presentUs, latencyUs, queueDepth, rttMs`。

**`framesOk` / `framesLost` 是「上次反馈以来的增量」，不是累计值**（被控端据此算
整窗丢帧率，不要把累计值再相减一次）。被控端用它做两件事：

- 自适应 **FEC 组大小**（丢帧率越低组越大、开销越小，见 §3.4）；
- 会话级状态展示。

码率自适应由**远控端**决定（它同时看丢帧率和"网络排队延迟"），通过 `SetBitrate`
下发；被控端收到后热调编码器，不需要重建管线。

---

## 5. 时序与延迟预算

端到端延迟的组成（1080p60，本机回环实测值见 `docs/USAGE.md`）：

| 环节 | 典型值 | 备注 |
|------|--------|------|
| DXGI 采集 | 1–3 ms | 取决于桌面刷新与是否有新帧 |
| BGRA→NV12（GPU VideoProcessor） | <1 ms | 走 ID3D11VideoProcessorBlt |
| NVENC 编码 | 2–5 ms | `p1` + `zerolatency` |
| 网络 | — | 局域网 <1 ms；FRP 取决于线路 |
| 解码 | 1–4 ms | 硬解 D3D11VA |
| 呈现 | 0–1 ms | **必须关垂直同步** |

两条最容易踩的坑：
1. **垂直同步**。开着 `Present(1, 0)` 每帧至少多等一个刷新周期（60 Hz = 16.7 ms），
   端到端 <30 ms 直接不可能。所以默认 `Present(0, ALLOW_TEARING)`。
2. **队列积压**。任何一级缓冲只要允许堆积，延迟就会单调上升且不会自愈。

---

## 6. 常量速查

| 常量 | 值 | 位置 |
|------|-----|------|
| 协议版本 | 1 | `proto::kVersion` |
| 默认端口 | 20000 | `defaults::kPort` |
| 端口下限 | 10001 | `defaults::kMinPort` |
| TCP 帧头 | 8 字节 | `proto::kTcpHeaderSize` |
| UDP 头 | 28 字节 | `proto::UdpHeader` |
| 视频分片净荷上限 | 1200 | `proto::kMaxVideoPayload` |
| 单帧字节上限 | 8 MiB | `proto::kMaxFrameBytes` |
| 输入事件 | 12 字节 | `proto::InputEvent` |
| 重排队列深度 | 2 帧 | `VideoReceiver::kMaxQueue` |
| 未完成帧超时 | 120 ms | `video_receiver.cpp` |
| 心跳超时 | 20 s（被控端）/ 25 s（远控端） | `host_service.cpp` / `control_client.cpp` |
| UDP 保活学习窗口 | 1500 ms | `host_service.cpp` |
