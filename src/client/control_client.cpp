// =============================================================================
//  control_client.cpp - 远控端 TCP 控制通道实现
// =============================================================================
#include "control_client.h"

#include "../common/crypt.h"
#include "../common/clock.h"
#include "../common/log.h"

#include <cstring>

namespace ms::client {

namespace {
// 心跳周期与判死阈值
// 【实测教训】5 秒太苛刻：走公网/FRP 绕行的链路，中间设备（FRP 服务器、
// 家用路由器的 NAT 回流）对长连接的转发抖动可以到好几秒。判死阈值放宽到
// 12 秒（相当于连续 24 个 Ping 丢失），宁可晚一点发现掉线，也不要误杀。
constexpr i64 kPingIntervalUs = 500000;    // 500 ms
// 25 秒没收到任何消息才判死。除了公网/FRP 抖动，还要容忍本机偶发的
// "整进程停顿"（某些机器上显卡驱动/安全软件会让进程几秒拿不到 CPU，
// 实测卡 5~10 秒；这时连 Ping 都发不出去，阈值太紧就会误判成掉线）。
constexpr i64 kDeadTimeoutUs  = 25000000;  // 25 s
} // namespace

ControlClient::~ControlClient() { close(); }

bool ControlClient::do_handshake(const ConnectParams& p, std::string& err, int timeoutMs) {
    // ---- 1. Hello ----
    proto::HelloMsg hello;
    hello.version = proto::kVersion;
    // v2 能力位：绝对鼠标坐标走「虚拟桌面物理像素」约定
    hello.flags = proto::kHelloFlagAbsPhysical;
    // v3 能力位：本端支持 HEVC 解码（D3D11VA 硬解优先，软解兜底）。
    // 被控端 codec=auto 且看到该位时会自动选 H.265（同画质省 30~40% 码率）。
    hello.flags |= proto::kHelloFlagHevcCapable;
    crypt::random_bytes(hello.nonce, proto::kNonceLen);
    hello.wantWidth  = u16(p.width  > 0 ? p.width  : 0);
    hello.wantHeight = u16(p.height > 0 ? p.height : 0);
    hello.wantFps    = u16(p.fps > 0 ? p.fps : 60);
    hello.wantBitrateKbps = u32(p.bitrateKbps > 0 ? p.bitrateKbps : 0);
    hello.displayIndex = u8(p.displayIndex);
    hello.clientName = p.clientName;

    if (!ch_.send_frame(proto::MsgType::Hello, hello.serialize())) {
        err = "发送 Hello 失败";
        return false;
    }

    // ---- 2. Challenge ----
    // 返回值必须区分三种情况，否则排障时会误导：
    //   -1 = 对端把连接断了（端口后面没有 MiniStream，或被控端直接拒绝）
    //    0 = 到时间没收到（对端在但没回应，检查 FRP/防火墙/被控端是否在跑）
    proto::MsgType t;
    Bytes payload;
    const int r = ch_.recv_frame(t, payload, timeoutMs);
    if (r < 0) {
        // 如果对端在断开前回了一条 Disconnect，把原因带出来
        if (t == proto::MsgType::Disconnect &&
            proto::DisconnectMsg::parse(payload.data(), payload.size(), lastReject_)) {
            err = "服务端拒绝: " + (lastReject_.reason.empty() ? "(未说明)" : lastReject_.reason);
        } else {
            err = "连接被对端关闭（端口能通，但后面不是 MiniStream 服务，或被控端已退出）";
        }
        return false;
    }
    if (r == 0) {
        err = "等待服务端 Challenge 超时（端口能连上但 8 秒没回应：检查被控端是否在运行、"
              "FRP 转发的是不是这台机器的同一端口）";
        return false;
    }
    if (t == proto::MsgType::Disconnect) {
        proto::DisconnectMsg dm;
        if (proto::DisconnectMsg::parse(payload.data(), payload.size(), dm) && !dm.reason.empty()) {
            err = "服务端拒绝: " + dm.reason;
        } else {
            err = "服务端在握手阶段拒绝连接";
        }
        return false;
    }
    if (t != proto::MsgType::Challenge) {
        err = std::string("期望 Challenge，收到 ") + proto::msg_type_name(t) +
              "（端口后面可能不是 MiniStream）";
        return false;
    }
    proto::ChallengeMsg chal;
    if (!proto::ChallengeMsg::parse(payload.data(), payload.size(), chal)) {
        err = "Challenge 格式错误";
        return false;
    }

    // ---- 3. Auth ----
    u8 key[proto::kProofLen];
    crypt::derive_key(p.password, key);
    proto::AuthMsg auth;
    crypt::compute_proof(key, hello.nonce, chal.serverNonce, chal.salt, auth.proof);
    if (!ch_.send_frame(proto::MsgType::Auth, auth.serialize())) {
        err = "发送 Auth 失败";
        return false;
    }

    // ---- 4. AuthResult ----
    proto::AuthResultMsg ar;
    if (!read_auth_result(ar, err, timeoutMs)) return false;
    if (!ar.ok) {
        err = ar.message.empty() ? "验证码错误" : ar.message;
        return false;
    }
    sessionId_ = ar.sessionId;
    return true;
}

bool ControlClient::read_auth_result(proto::AuthResultMsg& out, std::string& err, int timeoutMs) {
    proto::MsgType t;
    Bytes payload;
    const int r = ch_.recv_frame(t, payload, timeoutMs);
    if (r < 0) {
        err = "连接在等待校验结果时被对端关闭";
        return false;
    }
    if (r == 0) {
        err = "等待服务端校验结果超时（连接正常但对端没有回 AuthResult）";
        return false;
    }
    if (t == proto::MsgType::Disconnect) {
        proto::DisconnectMsg dm;
        if (proto::DisconnectMsg::parse(payload.data(), payload.size(), dm) && !dm.reason.empty()) {
            err = "服务端拒绝: " + dm.reason;
        } else {
            err = "服务端在握手阶段拒绝连接";
        }
        return false;
    }
    if (t != proto::MsgType::AuthResult) {
        err = std::string("期望 AuthResult，收到 ") + proto::msg_type_name(t);
        return false;
    }
    if (!proto::AuthResultMsg::parse(payload.data(), payload.size(), out)) {
        err = "AuthResult 格式错误";
        return false;
    }
    return true;
}

bool ControlClient::connect_and_auth(const ConnectParams& p, StreamParams& out,
                                     std::string& err, int timeoutMs) {
    close();

    if (p.password.empty()) { err = "验证码不能为空"; return false; }
    if (!is_valid_port(p.port)) {
        err = "端口必须在 10001-65535 之间";
        return false;
    }

    if (!ch_.connect(p.host, p.port, timeoutMs, err)) return false;

    if (!do_handshake(p, err, timeoutMs)) {
        ch_.close();
        return false;
    }

    // ---- 5. StreamReady：服务端实际生效的参数 ----
    proto::MsgType t;
    Bytes payload;
    if (ch_.recv_frame(t, payload, timeoutMs) <= 0) {
        err = "等待 StreamReady 超时（服务端可能采集/编码初始化失败）";
        ch_.close();
        return false;
    }
    if (t == proto::MsgType::Disconnect) {
        proto::DisconnectMsg dm;
        if (proto::DisconnectMsg::parse(payload.data(), payload.size(), dm)) err = dm.reason;
        else err = "服务端拒绝串流";
        ch_.close();
        return false;
    }
    if (t != proto::MsgType::StreamReady) {
        err = std::string("期望 StreamReady，收到 ") + proto::msg_type_name(t);
        ch_.close();
        return false;
    }
    proto::StreamReadyMsg sr;
    if (!proto::StreamReadyMsg::parse(payload.data(), payload.size(), sr)) {
        err = "StreamReady 格式错误";
        ch_.close();
        return false;
    }

    out.codec = sr.codec;
    out.width = sr.width;
    out.height = sr.height;
    out.fps = sr.fps ? sr.fps : u16(p.fps);
    out.bitrateKbps = sr.bitrateKbps;
    out.monitorName = sr.monitorName;
    out.monitorOriginX = sr.monitorOriginX;
    out.monitorOriginY = sr.monitorOriginY;
    out.desktopPhysW = sr.desktopPhysW;
    out.desktopPhysH = sr.desktopPhysH;

    lastRecvUs_ = now_us();
    pingSeq_ = 0;
    // 【别写 0】"从未发过 Ping"用 0 当哨兵，一旦时钟返回非正数（历史上真发生过
    // QPC 换算溢出，开机 >10.7 天后 now_us() 变负）就永远发不出第一个 Ping。
    lastPingUs_ = now_us();
    return true;
}

void ControlClient::close() {
    if (ch_.is_open()) ch_.close();
    sessionId_ = 0;
}

bool ControlClient::request_keyframe(u8 reason) {
    if (!ch_.is_open()) return false;
    proto::KeyframeReqMsg m;
    m.reason = reason;
    return ch_.send_frame(proto::MsgType::KeyframeReq, m.serialize());
}

bool ControlClient::send_feedback(const proto::FeedbackMsg& f) {
    if (!ch_.is_open()) return false;
    return ch_.send_frame(proto::MsgType::Feedback, f.serialize());
}

bool ControlClient::send_set_bitrate(u32 kbps) {
    if (!ch_.is_open()) return false;
    proto::SetBitrateMsg m;
    m.kbps = kbps;
    return ch_.send_frame(proto::MsgType::SetBitrate, m.serialize());
}

bool ControlClient::send_set_keyframe_interval(int seconds) {
    if (!ch_.is_open()) return false;
    // 【必须允许 0】0 = 按需（无限 GOP，只在缺帧/换目标时出 IDR）。
    // 这里以前写 `if (seconds < 1) seconds = 1;`，把工具栏「按需（推荐）」悄悄
    // 变成"每 1 秒一个 IDR" —— 用户看到的就是规律性顿一下 + 码率锯齿。
    if (seconds < 0)  seconds = 0;
    if (seconds > 10) seconds = 10;
    proto::SetKeyframeIntervalMsg m;
    m.seconds = u16(seconds);
    return ch_.send_frame(proto::MsgType::SetKeyframeInterval, m.serialize());
}

bool ControlClient::send_reconfigure(int width, int height, int fps, int codec) {
    if (!ch_.is_open()) return false;
    proto::ReconfigureMsg m;
    m.width  = u16(width  > 0 ? width  : 0);
    m.height = u16(height > 0 ? height : 0);
    m.fps    = u16(fps    > 0 ? fps    : 60);
    m.codec  = u8(codec == 2 ? 2 : (codec == 1 ? 1 : 0));   // 0=不变
    return ch_.send_frame(proto::MsgType::Reconfigure, m.serialize());
}

bool ControlClient::request_tcp_video() {
    return set_transport_mode(1, false);
}

bool ControlClient::set_transport_mode(int mode, bool probe) {
    if (!ch_.is_open()) return false;
    proto::TransportModeMsg m;
    m.mode  = u8(mode ? 1 : 0);
    m.probe = probe ? 1 : 0;
    if (!ch_.send_frame(proto::MsgType::VideoOverTcp, m.serialize())) return false;
    if (probe) MS_DEBUG("已请求一次 UDP 探测");
    else       MS_INFO("已请求传输模式 -> %s", mode ? "TCP" : "UDP");
    return true;
}

// -----------------------------------------------------------------------------
// 音频（TCP 兜底路径）：取一个待播放的包
// -----------------------------------------------------------------------------
bool ControlClient::take_audio(proto::AudioPacket& out) {
    std::lock_guard<std::mutex> lk(audioMtx_);
    if (audioQueue_.empty()) return false;
    out = std::move(audioQueue_.front());
    audioQueue_.pop_front();
    return true;
}
bool ControlClient::take_pointer_pos(proto::PointerPosMsg& out) {
    std::lock_guard<std::mutex> lk(ptrMtx_);
    if (!ptrPosDirty_) return false;
    ptrPosDirty_ = false;
    out = ptrPos_;
    return true;
}

bool ControlClient::take_pointer_shape(proto::PointerShapeMsg& out) {
    std::lock_guard<std::mutex> lk(ptrMtx_);
    if (!ptrShapeDirty_) return false;
    ptrShapeDirty_ = false;
    out = ptrShape_;
    return true;
}

bool ControlClient::send_disconnect(const std::string& reason) {
    if (!ch_.is_open()) return false;
    proto::DisconnectMsg m;
    m.code = 0;
    m.reason = reason;
    // 尽力而为：发不出去也无所谓，对端有超时兜底
    return ch_.send_frame(proto::MsgType::Disconnect, m.serialize());
}

int ControlClient::poll(int timeoutMs) {
    if (!ch_.is_open()) return -1;

    const i64 now = now_us();

    // 定时发 Ping（顺便做时钟同步）
    if (now - lastPingUs_ >= kPingIntervalUs) {
        lastPingUs_ = now;
        proto::PingMsg pm;
        pm.seq = ++pingSeq_;
        pm.t1 = now;
        if (!ch_.send_frame(proto::MsgType::Ping, pm.serialize())) {
            MS_WARN("发送 Ping 失败，判定连接断开");
            if (on_disconnected) on_disconnected("发送心跳失败");
            close();
            return -1;
        }
    }

    int handled = 0;
    // 一次 poll 内把收到的消息都处理掉
    for (;;) {
        proto::MsgType t;
        Bytes payload;
        const int r = ch_.recv_frame(t, payload, handled ? 0 : timeoutMs);
        if (r == 0) break;                       // 超时
        if (r < 0) {
            const std::string why = lastDisconnect_.empty() ? "控制通道已断开（TCP）" : lastDisconnect_;
            MS_INFO("%s", why.c_str());
            if (on_disconnected) on_disconnected(why);
            close();
            return -1;
        }
        lastRecvUs_ = now_us();
        ++handled;

        switch (t) {
            case proto::MsgType::Pong: {
                proto::PongMsg po;
                if (proto::PongMsg::parse(payload.data(), payload.size(), po)) {
                    // t4 用收到 Pong 的本地时刻；po.t1/t2/t3 来自往返双方
                    clock_.update(po.t1, po.t2, po.t3, lastRecvUs_);
                }
                break;
            }
            case proto::MsgType::VideoFrame: {
                // TCP 视频回退：一整帧直接入队（不分片，不存在丢片乱序）
                proto::TcpVideoFrame vf;
                if (proto::parse_tcp_video_frame(payload.data(), payload.size(), vf)) {
                    if (!tcpVideoActive_) {
                        tcpVideoActive_ = true;
                        MS_INFO("TCP 视频通道已建立（整帧 %u 字节）", unsigned(vf.len));
                    }
                    if (on_video_frame) {
                        on_video_frame(vf.frameId, vf.keyframe, vf.sendTsUs, vf.data, vf.len);
                    }
                } else {
                    MS_WARN("TCP 视频帧解析失败（len=%u，可能被截断）",
                            unsigned(payload.size()));
                }
                break;
            }
            case proto::MsgType::PointerPos: {
                // 光标位置（TCP 兜底；UDP 正常时走 receiver）
                proto::PointerPosMsg pm;
                if (proto::PointerPosMsg::parse(payload.data(), payload.size(), pm)) {
                    std::lock_guard<std::mutex> lk(ptrMtx_);
                    if (!havePtrPos_ || i32(u32(pm.seq - ptrSeq_)) > 0) {
                        ptrPos_ = pm;
                        ptrSeq_ = pm.seq;
                        havePtrPos_ = true;
                        ptrPosDirty_ = true;
                    }
                }
                break;
            }
            case proto::MsgType::AudioPacket: {
                // 音频（TCP 兜底）：视频切 TCP 时声音也从这条通道走，保证不断。
                proto::AudioPacket ap;
                proto::AudioPacketView v;
                if (proto::parse_audio_packet(payload.data(), payload.size(), v)) {
                    ap.seq = v.seq;
                    ap.ts  = v.ts;
                    ap.data.assign(v.data, v.data + v.len);
                    std::lock_guard<std::mutex> lk(audioMtx_);
                    if (audioQueue_.size() >= kMaxAudioQueue) audioQueue_.pop_front();
                    audioQueue_.push_back(std::move(ap));
                }
                break;
            }
            case proto::MsgType::PointerShape: {
                // 光标形状：一次性几 KB，交给主循环去建 D2D 位图
                proto::PointerShapeMsg::View v;
                if (proto::PointerShapeMsg::parse(payload.data(), payload.size(), v)) {
                    std::lock_guard<std::mutex> lk(ptrMtx_);
                    ptrShape_.shapeId = v.shapeId;
                    ptrShape_.width   = v.width;
                    ptrShape_.height  = v.height;
                    ptrShape_.hotX    = v.hotX;
                    ptrShape_.hotY    = v.hotY;
                    ptrShape_.bgra.assign(v.data, v.data + v.len);
                    ptrShapeDirty_ = true;
                } else {
                    MS_WARN("光标形状解析失败（%u 字节）", unsigned(payload.size()));
                }
                break;
            }
            case proto::MsgType::InputApplied: {
                // 可靠输入回执：v2 服务端对每个 InputBatch 回一条
                proto::InputAppliedMsg m;
                if (m.parse(payload.data(), payload.size())) {
                    if (on_input_applied) {
                        on_input_applied(m.batchId, m.applied, m.status);
                    }
                }
                break;
            }
            case proto::MsgType::Disconnect: {
                proto::DisconnectMsg dm;
                std::string why = "被控端主动断开";
                if (proto::DisconnectMsg::parse(payload.data(), payload.size(), dm) &&
                    !dm.reason.empty()) {
                    why += ": " + dm.reason;
                }
                MS_INFO("%s", why.c_str());
                lastDisconnect_ = why;
                if (on_disconnected) on_disconnected(why);
                close();
                return -1;
            }            case proto::MsgType::StreamReady:
                // 会话中途重发参数：按 Reconfigured 处理（格式相同）
                // fall through
            case proto::MsgType::Reconfigured: {
                proto::StreamReadyMsg sr;
                if (proto::StreamReadyMsg::parse(payload.data(), payload.size(), sr)) {
                    StreamParams sp;
                    sp.codec = sr.codec;
                    sp.width = sr.width;
                    sp.height = sr.height;
                    sp.fps = sr.fps;
                    sp.bitrateKbps = sr.bitrateKbps;
                    sp.monitorName = sr.monitorName;
                    sp.monitorOriginX = sr.monitorOriginX;
                    sp.monitorOriginY = sr.monitorOriginY;
                    sp.desktopPhysW = sr.desktopPhysW;
                    sp.desktopPhysH = sr.desktopPhysH;
                    if (on_reconfigured) on_reconfigured(sp);
                }
                break;
            }
            default:
                MS_DEBUG("控制通道收到未处理消息: %s", proto::msg_type_name(t));
                break;
        }
    }

    // 心跳判死
    if (now - lastRecvUs_ > kDeadTimeoutUs) {
        const std::string why = "心跳超时（25 秒未收到被控端消息：网络中断，"
                                "或公网/FRP 转发不稳定——两台机器在同一内网时"
                                "请直接填内网 IP）";
        MS_WARN("%s", why.c_str());
        if (on_disconnected) on_disconnected(why);
        close();
        return -1;
    }

    return handled;
}

} // namespace ms::client
