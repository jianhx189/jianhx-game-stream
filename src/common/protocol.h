// =============================================================================
//  protocol.h - MiniStream 自定义协议（TCP 控制通道 + UDP 视频/输入通道）
//
//  设计要点
//   * 一个端口号同时开 TCP 与 UDP；TCP 负责握手/校验/心跳/参数协商，
//     UDP 负责视频与输入（低延迟优先，容忍丢包）。
//   * 所有多字节整数都是小端；结构体一律手写序列化，避免编译器 packing 差异。
//   * 视频分片：一帧编码数据切成 <= kMaxVideoPayload 的若干 UDP 包，
//     收端按 (frameId) 重组，缺包就丢整帧并请求关键帧。
// =============================================================================
#pragma once

#include "types.h"
#include "serialize.h"
#include <string>
#include <cstring>

namespace ms::proto {

constexpr u32 kMagic   = 0x5254534Du;   // 'MSTR'
constexpr u16 kVersion = 1;

constexpr size_t kNonceLen = 16;
constexpr size_t kSaltLen  = 16;
constexpr size_t kProofLen = 32;

// -----------------------------------------------------------------------------
// TCP 控制通道
// -----------------------------------------------------------------------------
enum class MsgType : u16 {
    Hello       = 1,   // C->S 版本/能力/期望参数/客户端 nonce
    Challenge   = 2,   // S->C 服务端 nonce + salt
    Auth        = 3,   // C->S 口令应答 proof
    AuthResult  = 4,   // S->C 校验结果 + sessionId
    StreamReady = 5,   // S->C 实际生效的编码参数
    Ping        = 6,   // C->S 心跳 + 时钟同步 T1
    Pong        = 7,   // S->C T1 回显 + T2/T3
    KeyframeReq = 8,   // C->S 请求 IDR
    SetBitrate  = 9,   // C->S 动态调码率
    Feedback    = 10,  // C->S 统计/延迟反馈
    Disconnect  = 11,  // 双向
    SetKeyframeInterval = 12,  // C->S 热调关键帧间隔（秒）；丢包恢复速度旋钮
    Reconfigure = 13,  // C->S 请求重协商（分辨率/帧率变了，需要重建采集与编码器）
    Reconfigured = 14, // S->C 重协商完成，带回新参数（格式同 StreamReady）
    // ---- TCP 视频回退（UDP 不通时用控制通道传视频，RustDesk 也这么做）----
    VideoOverTcp = 15, // C->S 传输模式/探测请求（格式见 TransportModeMsg；空 body=切 TCP）
    VideoFrame   = 16, // S->C 一整个视频帧（格式见下面 encode_tcp_video_frame）
    // ---- 光标 --------------------------------------------------------------
    // Desktop Duplication **不会**把光标画进桌面纹理，必须单独传。
    // 不传的话远控端就只能靠本机光标，而捕获游戏时本机光标被 ShowCursor(FALSE)
    // 藏起来了 —— 表现就是"连上去鼠标不见了"。
    PointerShape = 17, // S->C 光标形状（预乘 BGRA，形状变化时才发）
    PointerPos   = 18, // S->C 光标位置/可见性（TCP 兜底；UDP 用 UdpType::Pointer）
    // ---- 音频 --------------------------------------------------------------
    // S->C 一个 Opus 音频包（20ms）。UDP 正常时走 UdpType::Audio（更省 8 字节头），
    // TCP 视频兜底模式下再走控制通道一份，保证"切 TCP 时声音也不断"。
    AudioPacket  = 19,
    // ---- 可靠输入（v2）------------------------------------------------------
    // C->S 键鼠批次，走 TCP 控制通道（可靠、有序）。旧版本忽略即可；
    // v2 客户端以「服务端是否回 InputApplied」判断对端支持可靠输入。
    InputBatch    = 20,
    // S->C 注入结果确认（已提交 SendInput 的事件数 / 最近错误）
    InputApplied  = 21,
};

const char* msg_type_name(MsgType t);

// TCP 帧：[u32 length][u16 type][u16 reserved][payload...]
// length = 4 + payload.size()（不含自身 4 字节）
constexpr size_t kTcpHeaderSize = 8;
constexpr u32    kMaxTcpPayload = 4u * 1024u * 1024u;

Bytes encode_tcp_frame(MsgType t, const Bytes& payload);
struct TcpHeader { u32 length = 0; MsgType type = MsgType::Hello; };
bool parse_tcp_header(const u8* p, size_t n, TcpHeader& out);

// --- 消息体 -----------------------------------------------------------------
struct HelloMsg {
    u16 version = kVersion;
    u16 flags = 0;
    u8  nonce[kNonceLen]{};
    u16 wantWidth = 0;      // 0 = 跟随被控端原生分辨率
    u16 wantHeight = 0;
    u16 wantFps = 60;
    u32 wantBitrateKbps = 0; // 0 = 自适应
    u8  displayIndex = 0;
    std::string clientName;

    Bytes serialize() const;
    static bool parse(const u8* p, size_t n, HelloMsg& out);
};

// Hello.flags 能力位（v2）
// bit0：绝对鼠标坐标走「虚拟桌面物理像素」约定（kInputFlagAbsPhysical）
constexpr u16 kHelloFlagAbsPhysical = 0x0001;
// bit1：这是「备用线路」上的副连接（双线模式：主线传视频，备线传键鼠/控制）。
// 被控端收到后不握手、不建会话，只接管这个连接作为第二条控制/输入通道。
constexpr u16 kHelloFlagBackupLine = 0x0002;
// bit2：客户端支持 HEVC 解码（v3）。被控端看到该位且 codec=auto 时自动选 H.265。
constexpr u16 kHelloFlagHevcCapable = 0x0004;

struct ChallengeMsg {
    u8 serverNonce[kNonceLen]{};
    u8 salt[kSaltLen]{};
    Bytes serialize() const;
    static bool parse(const u8* p, size_t n, ChallengeMsg& out);
};

struct AuthMsg {
    u8 proof[kProofLen]{};
    Bytes serialize() const;
    static bool parse(const u8* p, size_t n, AuthMsg& out);
};

struct AuthResultMsg {
    u8  ok = 0;
    u32 sessionId = 0;
    std::string message;
    Bytes serialize() const;
    static bool parse(const u8* p, size_t n, AuthResultMsg& out);
};

enum class VideoCodec : u8 { H264 = 1, HEVC = 2 };

struct StreamReadyMsg {
    VideoCodec codec = VideoCodec::H264;
    u16 width = 0;
    u16 height = 0;
    u16 fps = 60;
    u32 bitrateKbps = 0;
    std::string monitorName;
    // ---- v2 附加字段（可选尾部：旧服务端不发，解析时缺省）------------------
    // 绝对鼠标换算用：被采集显示器在虚拟桌面里的原点（多显示器可为负），
    // 以及该显示器的物理像素尺寸（视频分辨率 != 桌面物理分辨率时按比例修正）。
    i32 monitorOriginX = 0;
    i32 monitorOriginY = 0;
    u16 desktopPhysW = 0;   // 0 = 未知（客户端用视频尺寸）
    u16 desktopPhysH = 0;
    Bytes serialize() const;
    static bool parse(const u8* p, size_t n, StreamReadyMsg& out);
};

struct PingMsg {
    u32 seq = 0;
    i64 t1 = 0;
    Bytes serialize() const;
    static bool parse(const u8* p, size_t n, PingMsg& out);
};

struct PongMsg {
    u32 seq = 0;
    i64 t1 = 0, t2 = 0, t3 = 0;
    Bytes serialize() const;
    static bool parse(const u8* p, size_t n, PongMsg& out);
};

struct KeyframeReqMsg {
    u8 reason = 0;   // 1=丢帧 2=首帧 3=手动
    Bytes serialize() const;
    static bool parse(const u8* p, size_t n, KeyframeReqMsg& out);
};

struct SetBitrateMsg {
    u32 kbps = 0;
    Bytes serialize() const;
    static bool parse(const u8* p, size_t n, SetBitrateMsg& out);
};

struct SetKeyframeIntervalMsg {
    u16 seconds = 2;   // 1..10
    Bytes serialize() const;
    static bool parse(const u8* p, size_t n, SetKeyframeIntervalMsg& out);
};

// 请求重协商：分辨率/帧率/编码格式变了，需要重建采集与编码器
struct ReconfigureMsg {
    u16 width = 0;    // 0 = 原生
    u16 height = 0;
    u16 fps = 60;
    // v3 追加（可选尾部）：0 = 不变，1 = H.264，2 = HEVC。
    // 旧被控端只读前 6 字节，多余字节被忽略，所以与旧版本互通。
    u8  codec = 0;
    Bytes serialize() const;
    static bool parse(const u8* p, size_t n, ReconfigureMsg& out);
};

struct DisconnectMsg {
    u16 code = 0;
    std::string reason;
    Bytes serialize() const;
    static bool parse(const u8* p, size_t n, DisconnectMsg& out);
};

// ---- 传输模式 / 探测（自愈）------------------------------------------------
// 远控端 -> 被控端。空 body 的老格式等价于 {mode=1, probe=0}（兼容旧版本）。
//
// 【为什么需要探测】
//   UDP 走公网/FRP 会偶发不通（NAT 映射重建、链路抖动）。旧逻辑一旦切到 TCP
//   就再也不回来了，打游戏时等于一直吃 TCP 的延迟。现在远控端在 TCP 模式下
//   每隔几秒发一次 probe 请求，被控端在 ~1.2 秒窗口内**同时**用 UDP 发视频，
//   客户端统计到达率：连续两次 >=95% 才切回 UDP（滞回 + 观察期，避免来回抖）。
struct TransportModeMsg {
    u8 mode  = 0;   // 0=UDP（默认，低延迟）；1=TCP（零丢包，UDP 不可用时兜底）
    u8 probe = 0;   // 1=探测请求（mode 保持当前值不变，只是让被控端同时走 UDP）
    u8 reserved[2]{};
    Bytes serialize() const;
    static bool parse(const u8* p, size_t n, TransportModeMsg& out);
};

// ---- 光标位置 / 可见性 -----------------------------------------------------
// 语义：照抄被控端「系统光标现在是否可见、在哪」。
//
// 【为什么这样就能自动区分游戏菜单和游戏内场】
//   本地打游戏时，游戏进内场会自己 ShowCursor(FALSE) 把系统光标藏起来，
//   回菜单再显示。Desktop Duplication 的 PointerPosition.Visible 如实反映
//   这个状态，所以远控端只要跟着 visible 走：菜单里有光标、内场里没有，
//   和本机玩的手感完全一致，不需要去猜"哪个窗口是游戏"。
struct PointerPosMsg {
    u32 seq = 0;          // 单调递增（UDP 乱序时客户端按 seq 丢旧包）
    i16 x = 0, y = 0;     // 像素坐标，原点 = 被采集显示器左上角
    u8  visible = 0;      // 1=显示
    u8  reserved = 0;
    Bytes serialize() const;
    static bool parse(const u8* p, size_t n, PointerPosMsg& out);
};

// ---- 光标形状 --------------------------------------------------------------
// 被控端把 DXGI 的 MONOCHROME / COLOR / MASKED_COLOR 三种光标位图统一转成
// **预乘 alpha 的 BGRA** 再发；客户端用 D2D 直接画，不需要再理解 DXGI 格式。
constexpr size_t kPointerShapeHeaderSize = 12;  // shapeId(4)+w(2)+h(2)+hotX(2)+hotY(2)
constexpr u16    kMaxPointerShapeDim     = 512;

struct PointerShapeMsg {
    u32 shapeId = 0;      // 形状版本号（变化时客户端才重建 D2D 位图）
    u16 width = 0, height = 0;
    i16 hotX = 0, hotY = 0;   // 热点（光标坐标指向图片里的这个像素）
    Bytes bgra;               // width*height*4，预乘 alpha

    Bytes serialize() const;
    // 解析只返回视图（指向 payload 内部），避免每帧多一次拷贝
    struct View {
        u32 shapeId = 0;
        u16 width = 0, height = 0;
        i16 hotX = 0, hotY = 0;
        const u8* data = nullptr;
        size_t len = 0;
    };
    static bool parse(const u8* p, size_t n, View& out);
};

// ---- 音频包 ---------------------------------------------------------------
// 固定格式（不协商）：48kHz / 立体声 / 20ms 一帧 / Opus。
// UDP 净荷 = 裸 Opus 数据，seq 与时间戳在 UdpHeader.id / sendTsUs 里（省 8 字节）；
// TCP 兜底净荷 = [u32 seq][u32 ts][opus...]，见 encode_audio_packet。
constexpr u32 kAudioSampleRate      = 48000;
constexpr int kAudioChannels        = 2;
constexpr int kAudioFrameMs         = 20;
constexpr u32 kAudioSamplesPerFrame = kAudioSampleRate * kAudioFrameMs / 1000;  // 960
constexpr size_t kMaxAudioPayload   = 1200;   // 96kbps 一帧约 240B，留足余量

Bytes encode_audio_packet(u32 seq, u32 ts, const u8* data, size_t len);
// 已拷出来的一份音频包（UDP/TCP 线程 -> 解码播放线程 跨线程传递用）
struct AudioPacket {
    u32   seq = 0;
    u32   ts  = 0;      // 48kHz 采样计数
    Bytes data;         // 裸 Opus
};
struct AudioPacketView {
    u32 seq = 0;              // 单调递增，客户端用它丢重复/乱序
    u32 ts  = 0;              // 48kHz 采样计数（u32 约 24.8 小时回绕）
    const u8* data = nullptr; // 指向 Opus 数据（视图，不拷贝）
    size_t len = 0;
};
bool parse_audio_packet(const u8* p, size_t n, AudioPacketView& out);

// ---- TCP 视频帧 -----------------------------------------------------------
// 一帧一个 TCP 消息（不分片）：控制通道的帧上限 4MiB，1080p 20Mbps 的关键帧
// 通常几百 KB，够用；超限的直接丢弃（UDP 路径那边也会丢）。
constexpr size_t kTcpVideoHeaderSize = 16;   // frameId(4)+flags(1)+rsv(3)+ts(4)+bytes(4)
Bytes encode_tcp_video_frame(u32 frameId, bool keyframe, u32 sendTsUs,
                             const u8* data, size_t len);
struct TcpVideoFrame {
    u32  frameId = 0;
    bool keyframe = false;
    u32  sendTsUs = 0;
    const u8* data = nullptr;
    size_t len = 0;
};
bool parse_tcp_video_frame(const u8* p, size_t n, TcpVideoFrame& out);

// 远控端 -> 被控端 的实时反馈（跑在 UDP 上，丢了也无所谓）
struct FeedbackMsg {
    u32 lastFrameId   = 0;
    u32 framesOk      = 0;   // 上次反馈以来的「完整收到」帧数（增量，不是累计）
    u32 framesLost    = 0;   // 上次反馈以来的「缺包丢弃」帧数（增量；FEC 恢复成功的不算）
    u32 recvKbps      = 0;
    u32 decodeUs      = 0;   // 平均解码耗时
    u32 presentUs     = 0;   // 平均渲染耗时
    u32 latencyUs     = 0;   // 估算端到端（采集->呈现）
    u16 queueDepth    = 0;
    u16 rttMs         = 0;
    Bytes serialize() const;
    static bool parse(const u8* p, size_t n, FeedbackMsg& out);
};

// -----------------------------------------------------------------------------
// UDP 通道
// -----------------------------------------------------------------------------
enum class UdpType : u8 {
    Video    = 1,
    Input    = 2,
    Feedback = 3,
    Keepalive = 4,
    // 光标位置/可见性：几十字节的小包，60Hz 发也不到 6 kbps。
    // 放在 UDP 上是为了低延迟（光标跟手）；同时它也是一条「下行 UDP 探针」——
    // 远控端能持续确认"被控端发过来的 UDP 还通不通"，传输自愈判断用得上。
    Pointer  = 5,
    // 音频包：净荷 = 裸 Opus 数据；seq 在 UdpHeader.id，时间戳在 sendTsUs
    // （48kHz 采样计数）。20ms 一帧约 240 字节，丢一两包用 Opus PLC 补，听感影响很小。
    Audio    = 6,
    // NACK（C→S）：缺片重传请求。id = frameId，extra = 下标个数，
    // 净荷 = 若干个 u16 分片下标（最多 64 个/包）。
    // 依据（UU远程/GameViewer 逆向报告）：FEC 只是恒定打底（~25%），
    // 真正的恢复主力是 NACK（重传量与丢包近似 1:1，实测恢复 42% + FEC 50%）。
    Nack     = 7,
};

constexpr u8 kUdpFlagKeyframe = 0x01;
// 该包是 XOR 纠删码校验包（不是数据分片）。字段约定见下面 kFec* 注释。
constexpr u8 kUdpFlagFec      = 0x02;
// 该包是 NACK 重传的副本（内容与原始分片相同，收端按 index 幂等处理）。
// 单独标记是为了不被计入"媒体码率"采样（否则重传会污染带宽估计）。
constexpr u8 kUdpFlagRetransmit = 0x04;

// 视频分片净荷上限：留足 IPv4/UDP 头 + 可能的 FRP 隧道封装余量
constexpr size_t kMaxVideoPayload = 1200;
constexpr u32    kMaxFrameBytes   = 8u * 1024u * 1024u;

// -----------------------------------------------------------------------------
// 视频 FEC（XOR 组纠删码）—— 抗丢包的关键一层
// -----------------------------------------------------------------------------
// 为什么必须有：
//   1080p60 一帧约 30~40 个 UDP 分片。链路只要丢 2% 的包，
//   整帧到达率就只有 0.98^35 ≈ 49% —— 表现就是「帧率只剩一半 + 时不时卡住」。
//   只靠「缺片丢整帧 + 请求 IDR」是救不回来的（IDR 更大、更容易丢）。
//   Moonlight 用 Reed-Solomon 也是这个原因；这里用实现最简单、零依赖的
//   XOR 组码：每 G 个数据分片配 1 个校验包，任意丢 1 片可无损恢复。
//
// 校验包字段约定（UdpHeader，type=Video 且 flags 带 kUdpFlagFec）：
//   id          = frameId
//   index       = 组下标（0 起）
//   count       = 整帧数据分片总数（和数据包一致，便于校验）
//   frameBytes  = 整帧字节数
//   extra       = 组大小 G（配置值，1..kFecMaxGroup；末组成员数用 min 求出）
//   payloadSize = 校验净荷长度 = 本组各分片净荷长度的最大值
//   sendTsUs    = 与数据分片相同（采集时刻低 32 位）
//
// 组内成员：全局分片下标 [group*G, min((group+1)*G, count))，
// 末组成员数 = min(G, count - group*G) —— 所以 extra 必须存「组大小 G」而不是
// 「本组成员数」，否则末组无法定位起始下标。
// 分片 i 的净荷长度恒为 min(kMaxVideoPayload, frameBytes - i*kMaxVideoPayload)，
// 所以收端不需要额外携带每个分片的长度。
// 校验净荷 = 组内各分片净荷按「补零到 payloadSize」后逐字节 XOR。
constexpr u16 kFecMaxGroup   = 32;   // 组大小上限（越大开销越低、越怕连续丢包）
constexpr u16 kFecMinGroup   = 3;    // 组大小下限（丢包严重时 1/3 开销换恢复率）
constexpr u16 kFecOffGroup   = 0;    // 0 = 关闭 FEC
constexpr u16 kFecDefaultGroup = 10; // 默认 10% 开销

// 固定 28 字节头
#pragma pack(push, 1)
struct UdpHeader {
    u8  type;         // UdpType
    u8  flags;        // kUdpFlag*
    u16 payloadSize;  // 净荷字节数（不含本头）
    u32 sessionId;
    u32 id;           // 视频: frameId 递增；输入: 事件包序号
    u16 index;        // 视频: 分片下标
    u16 count;        // 视频: 该帧分片总数
    u32 sendTsUs;     // 发送时刻低 32 位微秒（用于延迟统计）
    u32 frameBytes;   // 视频: 整帧字节数
    u32 extra;        // 输入: 事件个数；视频FEC校验包: 组内数据分片数；其它：0
};
#pragma pack(pop)
static_assert(sizeof(UdpHeader) == 28, "UdpHeader must be 28 bytes");

inline void write_udp_header(u8* dst, const UdpHeader& h) {
    std::memcpy(dst, &h, sizeof(h));
}
inline bool parse_udp_header(const u8* p, size_t n, UdpHeader& out) {
    if (n < sizeof(UdpHeader)) return false;
    std::memcpy(&out, p, sizeof(out));
    return true;
}

// -----------------------------------------------------------------------------
// 输入事件
// -----------------------------------------------------------------------------
enum class InputType : u8 {
    MouseMove   = 1,   // a = dx, b = dy（相对）
    MouseButton = 2,   // a = 键(1左2右3中4侧1 5侧2), b = 1按下/0抬起
    MouseWheel  = 3,   // a = 垂直增量, b = 水平增量
    Key         = 4,   // a = scancode, flags bit0=抬起 bit1=扩展键
    ReleaseAll  = 5,   // 断线/释放时用的兜底
};

constexpr u8 kInputFlagKeyUp   = 0x01;
constexpr u8 kInputFlagKeyExt  = 0x02;
// MouseMove 专用：a/b 不是相对位移，而是**远端桌面像素坐标**（绝对映射）。
// 绝对模式用于桌面/游戏菜单：远控端把它本机光标位置按显示缩放换算成远端坐标发过来，
// 被控端直接 SetCursorPos —— 光标和用户本机鼠标 1:1 重合，不受系统鼠标加速影响。
// 相对模式（游戏内场）不带这个标志：游戏从原始输入里读位移，本机光标被隐藏。
constexpr u8 kInputFlagAbsMove = 0x04;
// v2：绝对坐标是「虚拟桌面物理像素」。旧实现是「采集显示器内坐标 + 注入端加显示器
// 原点」，缩放串流（视频分辨率 != 桌面分辨率）时点击位置会偏；v2 由客户端一次性
// 换算到物理桌面坐标，注入端 SetCursorPos 直接用，不再做任何二次换算。
constexpr u8 kInputFlagAbsPhysical = 0x08;

// ---- 可靠输入批次（TCP 控制通道）--------------------------------------------
// 一个会话内单调递增；服务端按 batchId 去重/回执。批次里的事件保持全局顺序。
constexpr u32 kMaxInputBatchEvents = 128;

// 固定 12 字节：type, flags, seq(u16), a(i32), b(i32)
struct InputEvent {
    u8  type  = 0;
    u8  flags = 0;
    u16 seq   = 0;
    i32 a     = 0;
    i32 b     = 0;
};
static_assert(sizeof(InputEvent) == 12, "InputEvent must be 12 bytes");

// C->S：[u32 batchId][u16 count][count 个 InputEvent]
Bytes encode_input_batch(u32 batchId, const InputEvent* ev, size_t count);
// 解析只返回事件数组视图（指向 out 内部；调用方提供事件存储并保证其生命周期）
struct InputBatchView {
    u32 batchId = 0;
    size_t count = 0;
    const InputEvent* events = nullptr;
};
bool parse_input_batch(const u8* p, size_t n, InputEvent* out, size_t maxOut,
                       InputBatchView& view);

// S->C：[u32 batchId][u32 applied][u8 status]；status: 0=ok 1=部分提交 2=拒绝(输入关闭)
Bytes encode_input_applied(u32 batchId, u32 applied, u8 status);
struct InputAppliedMsg {
    u32 batchId = 0;
    u32 applied = 0;
    u8  status  = 0;
    bool parse(const u8* p, size_t n);
};

// kInputFlagAbsMove 事件的 a/b 约定（v2，鼠标模式语义见 flags 位定义）：
//   旧（v1）          : a/b = 被采集显示器内的像素坐标（原点 = 显示器左上角）
//   v2（kInputFlagAbsPhysical）: a/b = 虚拟桌面物理像素坐标（SetCursorPos 直接可用），
//     多显示器 / 负坐标 / DPI 缩放都由客户端换算好，注入端零逻辑。
// 客户端发能力位 kHelloFlagAbsPhysical（Hello.flags bit0）声明自己走 v2 约定；
// 服务端不认该位时仍按 v1 解（加显示器原点）。

// 输入包净荷：u16 count + count 个 InputEvent
Bytes encode_input_packet(const InputEvent* ev, size_t count);
bool decode_input_packet(const u8* p, size_t n, InputEvent* out, size_t maxOut, size_t& outCount);

// 本机按键码 -> 传给被控端的 scancode（用 MapVirtualKey 转，保持注入端零逻辑）
u16 make_scancode(u32 vkey, bool& isExtended);

} // namespace ms::proto
