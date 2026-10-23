// =============================================================================
//  control_client.h - 远控端 TCP 控制通道
//
//  职责：握手 -> 密码挑战应答 -> 拿 sessionId -> 心跳/时钟同步 -> 断线检测
// =============================================================================
#pragma once

#include "../common/types.h"
#include "../common/protocol.h"
#include "../common/net.h"
#include "../common/clock.h"
#include <deque>
#include <functional>
#include <mutex>
#include <string>

namespace ms::client {

struct ConnectParams {
    std::string host = "127.0.0.1";
    u16         port = 20000;
    std::string password;
    int  fps = 60;
    int  bitrateKbps = 0;
    int  width = 0;
    int  height = 0;
    int  displayIndex = 0;
    std::string clientName = "jianhx-Client";
    // 双线：备用线路（键鼠/控制走备线，主线全留给视频）。
    // 空 = 不启用；端口 0 = 与主线相同。
    std::string backupHost;
    u16         backupPort = 0;
};

struct StreamParams {
    proto::VideoCodec codec = proto::VideoCodec::H264;
    u16 width = 1920;
    u16 height = 1080;
    u16 fps = 60;
    u32 bitrateKbps = 20000;
    std::string monitorName;
    // v2 附加字段（旧服务端缺省 0）：绝对鼠标换算用
    i32 monitorOriginX = 0;
    i32 monitorOriginY = 0;
    u16 desktopPhysW = 0;   // 0 = 未知
    u16 desktopPhysH = 0;
};

class ControlClient {
public:
    ControlClient() = default;
    ~ControlClient();

    // 完整握手（阻塞）。失败时 err 给出面向用户的中文原因。
    bool connect_and_auth(const ConnectParams& p, StreamParams& out,
                          std::string& err, int timeoutMs = 8000);
    void close();
    bool is_open() const { return ch_.is_open(); }
    // 底层 TCP 通道（可靠输入管线用 InputBatch 复用这条连接）。
    // 只暴露发送能力所在的对象本身；send_frame 自带发送锁，跨线程安全。
    net::TcpChannel& ch() { return ch_; }

    // 处理控制消息 / 发心跳 / 检测断线。
    //  返回 1 = 处理了新消息; 0 = 无; <0 = 连接已断开
    int poll(int timeoutMs);

    bool request_keyframe(u8 reason = 1);
    bool send_feedback(const proto::FeedbackMsg& f);
    bool send_set_bitrate(u32 kbps);
    bool send_disconnect(const std::string& reason);

    // ---- 运行期热调（游戏优化面板用）----
    // 关键帧间隔（秒）：越小丢包后恢复越快，代价是码率升高
    bool send_set_keyframe_interval(int seconds);
    // 分辨率/帧率/编码格式变了必须重建采集与编码器 -> 请求重协商。
    // 服务端完成后会回 Reconfigured（格式同 StreamReady），由 poll() 处理并回调。
    // codec: 0=不变，1=H.264，2=HEVC（H.265）
    bool send_reconfigure(int width, int height, int fps, int codec = 0);

    // UDP 视频收不到时：请求改用 TCP 传视频（控制通道已经验证可用）
    bool request_tcp_video();
    // 传输模式（自愈）：mode 0=UDP（低延迟）1=TCP（零丢包兜底）；
    // probe=true 表示只请求一次探测（不改当前模式）——被控端会在 ~1.2 秒窗口内
    // 同时用 UDP 发视频，客户端统计到达率决定要不要切回 UDP（连续两次 >=95%）。
    bool set_transport_mode(int mode, bool probe);
    bool request_udp_probe() { return set_transport_mode(0, true); }

    // ---- 音频（TCP 兜底路径）-----------------------------------------------
    // 视频切 TCP 时声音也从控制通道走（丢包会卡顿但不会没声音）。
    bool take_audio(proto::AudioPacket& out);

    // ---- 光标（TCP 兜底路径；UDP 正常时光标走 receiver 的 UdpType::Pointer）----
    // 有更新的位置/可见性时返回 true（按 seq 过滤乱序）
    bool take_pointer_pos(proto::PointerPosMsg& out);
    // 光标形状有更新时返回 true（带完整 BGRA 数据）
    bool take_pointer_shape(proto::PointerShapeMsg& out);

    ClockSync& clock() { return clock_; }
    i64 rtt_us() const { return clock_.last_rtt_us(); }
    u32 session_id() const { return sessionId_; }

    // 被控端主动断开或心跳超时时回调
    std::function<void(const std::string& reason)> on_disconnected;
    // 重协商完成回调（画质/帧率/分辨率改动生效），参数同 StreamReady
    std::function<void(const StreamParams& sp)> on_reconfigured;
    // TCP 视频帧回调（在控制线程里执行）
    std::function<void(u32 frameId, bool keyframe, u32 sendTsUs,
                       const u8* data, size_t len)> on_video_frame;
    // 可靠输入回执（v2）：控制线程收到 InputApplied 时触发
    std::function<void(u32 batchId, u32 applied, u8 status)> on_input_applied;
    // 是否已经收到过 TCP 视频帧
    bool tcp_video_active() const { return tcpVideoActive_; }

    // 最近一次被控端给的断开原因（空 = 尚无）
    const std::string& disconnect_reason() const { return lastDisconnect_; }

private:
    bool read_auth_result(proto::AuthResultMsg& out, std::string& err, int timeoutMs);
    bool do_handshake(const ConnectParams& p, std::string& err, int timeoutMs);

    net::TcpChannel ch_;
    u32  sessionId_ = 0;
    u32  pingSeq_ = 0;
    i64  lastPingUs_ = 0;
    i64  lastRecvUs_ = 0;
    ClockSync clock_;
    std::string lastDisconnect_;        // 会话进行中服务端给的断开原因（显示给用户）
    proto::DisconnectMsg lastReject_;   // 握手阶段服务端回的拒绝原因（排障用）
    bool tcpVideoActive_ = false;       // 已从 TCP 通道收到过视频帧

    // ---- 音频队列（控制线程写 / 主循环读）----
    static constexpr size_t kMaxAudioQueue = 64;
    std::mutex audioMtx_;
    std::deque<proto::AudioPacket> audioQueue_;

    // ---- 光标（控制线程写 / 主循环读）----
    std::mutex ptrMtx_;
    proto::PointerPosMsg  ptrPos_{};
    u32  ptrSeq_ = 0;
    bool havePtrPos_ = false;
    bool ptrPosDirty_ = false;
    proto::PointerShapeMsg ptrShape_{};
    bool ptrShapeDirty_ = false;
};

} // namespace ms::client
