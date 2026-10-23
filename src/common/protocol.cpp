// =============================================================================
//  protocol.cpp - 消息体编解码实现
// =============================================================================
#include "protocol.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace ms::proto {

const char* msg_type_name(MsgType t) {
    switch (t) {
        case MsgType::Hello:       return "Hello";
        case MsgType::Challenge:   return "Challenge";
        case MsgType::Auth:        return "Auth";
        case MsgType::AuthResult:  return "AuthResult";
        case MsgType::StreamReady: return "StreamReady";
        case MsgType::Ping:        return "Ping";
        case MsgType::Pong:        return "Pong";
        case MsgType::KeyframeReq: return "KeyframeReq";
        case MsgType::SetBitrate:  return "SetBitrate";
        case MsgType::Feedback:    return "Feedback";
        case MsgType::Disconnect:  return "Disconnect";
        case MsgType::SetKeyframeInterval: return "SetKeyframeInterval";
        case MsgType::Reconfigure: return "Reconfigure";
        case MsgType::Reconfigured: return "Reconfigured";
        case MsgType::VideoOverTcp: return "VideoOverTcp";
        case MsgType::VideoFrame:   return "VideoFrame";
        case MsgType::PointerShape: return "PointerShape";
        case MsgType::PointerPos:   return "PointerPos";
        case MsgType::AudioPacket:  return "AudioPacket";
        case MsgType::InputBatch:   return "InputBatch";
        case MsgType::InputApplied: return "InputApplied";
    }
    return "Unknown";
}

// -----------------------------------------------------------------------------
// TCP 帧
// -----------------------------------------------------------------------------
Bytes encode_tcp_frame(MsgType t, const Bytes& payload) {
    Bytes out;
    out.reserve(kTcpHeaderSize + payload.size());
    ByteWriter w(out);
    w.u32v(u32(4 + payload.size()));   // length 不含自身 4 字节
    w.u16v(u16(t));
    w.u16v(0);                          // reserved
    w.raw(payload.data(), payload.size());
    return out;
}

bool parse_tcp_header(const u8* p, size_t n, TcpHeader& out) {
    if (n < kTcpHeaderSize) return false;
    const u32 len = u32(p[0]) | (u32(p[1]) << 8) | (u32(p[2]) << 16) | (u32(p[3]) << 24);
    const u16 ty  = u16(p[4] | (p[5] << 8));
    if (len < 4 || len > kMaxTcpPayload + 4) return false;
    out.length = len;
    out.type = MsgType(ty);
    return true;
}

// -----------------------------------------------------------------------------
// 音频包（TCP 兜底路径：u32 seq + u32 ts + 裸 Opus）
// -----------------------------------------------------------------------------
Bytes encode_audio_packet(u32 seq, u32 ts, const u8* data, size_t len) {
    Bytes b;
    b.reserve(8 + len);
    ByteWriter w(b);
    w.u32v(seq);
    w.u32v(ts);
    if (len) w.raw(data, len);
    return b;
}

bool parse_audio_packet(const u8* p, size_t n, AudioPacketView& out) {
    if (n < 8) return false;
    ByteReader r(p, n);
    if (!r.u32v(out.seq)) return false;
    if (!r.u32v(out.ts)) return false;
    out.data = p + 8;
    out.len = n - 8;
    return out.len <= kMaxAudioPayload;
}

// -----------------------------------------------------------------------------
// TCP 视频帧
// -----------------------------------------------------------------------------
Bytes encode_tcp_video_frame(u32 frameId, bool keyframe, u32 sendTsUs,
                             const u8* data, size_t len) {
    Bytes b;
    b.reserve(kTcpVideoHeaderSize + len);
    ByteWriter w(b);
    w.u32v(frameId);
    w.u8v(keyframe ? 1 : 0);
    w.u8v(0); w.u8v(0); w.u8v(0);      // 预留
    w.u32v(sendTsUs);
    w.u32v(u32(len));
    if (len) w.raw(data, len);
    return b;
}

bool parse_tcp_video_frame(const u8* p, size_t n, TcpVideoFrame& out) {
    ByteReader r(p, n);
    u8 flags = 0;
    u8 rsv[3]{};
    u32 bytes = 0;
    if (!r.u32v(out.frameId)) return false;
    if (!r.u8v(flags)) return false;
    if (!r.raw(rsv, 3)) return false;
    if (!r.u32v(out.sendTsUs)) return false;
    if (!r.u32v(bytes)) return false;
    if (bytes > kMaxTcpPayload) return false;
    if (n < kTcpVideoHeaderSize + size_t(bytes)) return false;
    out.keyframe = (flags & 1) != 0;
    out.data = p + kTcpVideoHeaderSize;
    out.len = size_t(bytes);
    return true;
}

// -----------------------------------------------------------------------------
// Hello
// -----------------------------------------------------------------------------
Bytes HelloMsg::serialize() const {
    Bytes b;
    ByteWriter w(b);
    w.u16v(version);
    w.u16v(flags);
    w.raw(nonce, kNonceLen);
    w.u16v(wantWidth);
    w.u16v(wantHeight);
    w.u16v(wantFps);
    w.u32v(wantBitrateKbps);
    w.u8v(displayIndex);
    w.str8(clientName);
    return b;
}

bool HelloMsg::parse(const u8* p, size_t n, HelloMsg& out) {
    ByteReader r(p, n);
    if (!r.u16v(out.version) || !r.u16v(out.flags)) return false;
    if (!r.raw(out.nonce, kNonceLen)) return false;
    if (!r.u16v(out.wantWidth) || !r.u16v(out.wantHeight) || !r.u16v(out.wantFps)) return false;
    if (!r.u32v(out.wantBitrateKbps)) return false;
    if (!r.u8v(out.displayIndex)) return false;
    if (!r.str8(out.clientName)) return false;
    return true;
}

// -----------------------------------------------------------------------------
// Challenge / Auth / AuthResult
// -----------------------------------------------------------------------------
Bytes ChallengeMsg::serialize() const {
    Bytes b;
    ByteWriter w(b);
    w.raw(serverNonce, kNonceLen);
    w.raw(salt, kSaltLen);
    return b;
}

bool ChallengeMsg::parse(const u8* p, size_t n, ChallengeMsg& out) {
    ByteReader r(p, n);
    return r.raw(out.serverNonce, kNonceLen) && r.raw(out.salt, kSaltLen);
}

Bytes AuthMsg::serialize() const {
    Bytes b;
    ByteWriter w(b);
    w.raw(proof, kProofLen);
    return b;
}

bool AuthMsg::parse(const u8* p, size_t n, AuthMsg& out) {
    ByteReader r(p, n);
    return r.raw(out.proof, kProofLen);
}

Bytes AuthResultMsg::serialize() const {
    Bytes b;
    ByteWriter w(b);
    w.u8v(ok);
    w.u32v(sessionId);
    w.str8(message);
    return b;
}

bool AuthResultMsg::parse(const u8* p, size_t n, AuthResultMsg& out) {
    ByteReader r(p, n);
    return r.u8v(out.ok) && r.u32v(out.sessionId) && r.str8(out.message);
}

// -----------------------------------------------------------------------------
// StreamReady
// -----------------------------------------------------------------------------
Bytes StreamReadyMsg::serialize() const {
    Bytes b;
    ByteWriter w(b);
    w.u8v(u8(codec));
    w.u16v(width);
    w.u16v(height);
    w.u16v(fps);
    w.u32v(bitrateKbps);
    w.str8(monitorName);
    // v2 附加字段（可选尾部：旧客户端解析到 monitorName 就结束，不受影响）
    w.i32v(monitorOriginX);
    w.i32v(monitorOriginY);
    w.u16v(desktopPhysW);
    w.u16v(desktopPhysH);
    return b;
}

bool StreamReadyMsg::parse(const u8* p, size_t n, StreamReadyMsg& out) {
    ByteReader r(p, n);
    u8 c = 0;
    if (!r.u8v(c)) return false;
    out.codec = VideoCodec(c);
    if (!r.u16v(out.width) || !r.u16v(out.height) || !r.u16v(out.fps)) return false;
    if (!r.u32v(out.bitrateKbps)) return false;
    if (!r.str8(out.monitorName)) return false;
    // v2 附加字段：缺省 0（旧服务端没有这段）
    if (r.left() >= 12) {
        if (!r.i32v(out.monitorOriginX) || !r.i32v(out.monitorOriginY) ||
            !r.u16v(out.desktopPhysW) || !r.u16v(out.desktopPhysH)) {
            return false;
        }
    } else {
        out.monitorOriginX = 0;
        out.monitorOriginY = 0;
        out.desktopPhysW = 0;
        out.desktopPhysH = 0;
    }
    return true;
}

// -----------------------------------------------------------------------------
// Ping / Pong
// -----------------------------------------------------------------------------
Bytes PingMsg::serialize() const {
    Bytes b;
    ByteWriter w(b);
    w.u32v(seq);
    w.i64v(t1);
    return b;
}

bool PingMsg::parse(const u8* p, size_t n, PingMsg& out) {
    ByteReader r(p, n);
    return r.u32v(out.seq) && r.i64v(out.t1);
}

Bytes PongMsg::serialize() const {
    Bytes b;
    ByteWriter w(b);
    w.u32v(seq);
    w.i64v(t1);
    w.i64v(t2);
    w.i64v(t3);
    return b;
}

bool PongMsg::parse(const u8* p, size_t n, PongMsg& out) {
    ByteReader r(p, n);
    return r.u32v(out.seq) && r.i64v(out.t1) && r.i64v(out.t2) && r.i64v(out.t3);
}

// -----------------------------------------------------------------------------
// 其它控制消息
// -----------------------------------------------------------------------------
Bytes KeyframeReqMsg::serialize() const {
    Bytes b;
    ByteWriter w(b);
    w.u8v(reason);
    return b;
}

bool KeyframeReqMsg::parse(const u8* p, size_t n, KeyframeReqMsg& out) {
    ByteReader r(p, n);
    return r.u8v(out.reason);
}

Bytes SetBitrateMsg::serialize() const {
    Bytes b;
    ByteWriter w(b);
    w.u32v(kbps);
    return b;
}

bool SetBitrateMsg::parse(const u8* p, size_t n, SetBitrateMsg& out) {
    ByteReader r(p, n);
    return r.u32v(out.kbps);
}

Bytes SetKeyframeIntervalMsg::serialize() const {
    Bytes b;
    ByteWriter w(b);
    w.u16v(seconds);
    return b;
}

bool SetKeyframeIntervalMsg::parse(const u8* p, size_t n, SetKeyframeIntervalMsg& out) {
    ByteReader r(p, n);
    return r.u16v(out.seconds);
}

Bytes ReconfigureMsg::serialize() const {
    Bytes b;
    ByteWriter w(b);
    w.u16v(width);
    w.u16v(height);
    w.u16v(fps);
    w.u8v(codec);      // v3 追加字段；旧被控端忽略尾部多余字节
    return b;
}

bool ReconfigureMsg::parse(const u8* p, size_t n, ReconfigureMsg& out) {
    ByteReader r(p, n);
    if (!(r.u16v(out.width) && r.u16v(out.height) && r.u16v(out.fps))) return false;
    // 可选尾部：老客户端不发 codec，保持 0（= 不改变当前编码格式）
    out.codec = 0;
    u8 c = 0;
    if (r.u8v(c)) out.codec = c;
    return true;
}

Bytes DisconnectMsg::serialize() const {
    Bytes b;
    ByteWriter w(b);
    w.u16v(code);
    w.str8(reason);
    return b;
}

bool DisconnectMsg::parse(const u8* p, size_t n, DisconnectMsg& out) {
    ByteReader r(p, n);
    return r.u16v(out.code) && r.str8(out.reason);
}

// -----------------------------------------------------------------------------
// 传输模式 / 探测
// -----------------------------------------------------------------------------
Bytes TransportModeMsg::serialize() const {
    Bytes b;
    ByteWriter w(b);
    w.u8v(mode);
    w.u8v(probe);
    w.u8v(reserved[0]);
    w.u8v(reserved[1]);
    return b;
}

bool TransportModeMsg::parse(const u8* p, size_t n, TransportModeMsg& out) {
    // 兼容旧版：空 body 表示"切 TCP"
    if (n == 0) {
        out = TransportModeMsg{};
        out.mode = 1;
        return true;
    }
    ByteReader r(p, n);
    return r.u8v(out.mode) && r.u8v(out.probe);
}

// -----------------------------------------------------------------------------
// 光标
// -----------------------------------------------------------------------------
Bytes PointerPosMsg::serialize() const {
    Bytes b;
    ByteWriter w(b);
    w.u32v(seq);
    w.u16v(u16(x));
    w.u16v(u16(y));
    w.u8v(visible);
    w.u8v(reserved);
    return b;
}

bool PointerPosMsg::parse(const u8* p, size_t n, PointerPosMsg& out) {
    ByteReader r(p, n);
    u16 ux = 0, uy = 0;
    if (!r.u32v(out.seq) || !r.u16v(ux) || !r.u16v(uy)) return false;
    if (!r.u8v(out.visible) || !r.u8v(out.reserved)) return false;
    out.x = i16(ux);
    out.y = i16(uy);
    return true;
}

Bytes PointerShapeMsg::serialize() const {
    Bytes b;
    b.reserve(kPointerShapeHeaderSize + bgra.size());
    ByteWriter w(b);
    w.u32v(shapeId);
    w.u16v(width);
    w.u16v(height);
    w.u16v(u16(hotX));
    w.u16v(u16(hotY));
    w.raw(bgra.data(), bgra.size());
    return b;
}

bool PointerShapeMsg::parse(const u8* p, size_t n, View& out) {
    if (n < kPointerShapeHeaderSize) return false;
    ByteReader r(p, n);
    u16 hx = 0, hy = 0;
    if (!r.u32v(out.shapeId) || !r.u16v(out.width) || !r.u16v(out.height) ||
        !r.u16v(hx) || !r.u16v(hy)) {
        return false;
    }
    out.hotX = i16(hx);
    out.hotY = i16(hy);
    if (out.width == 0 || out.height == 0 ||
        out.width > kMaxPointerShapeDim || out.height > kMaxPointerShapeDim) {
        return false;
    }
    const size_t need = size_t(out.width) * size_t(out.height) * 4u;
    if (r.left() < need) return false;
    out.data = r.cur();
    out.len = need;
    return true;
}

// -----------------------------------------------------------------------------
// Feedback
// -----------------------------------------------------------------------------
Bytes FeedbackMsg::serialize() const {
    Bytes b;
    ByteWriter w(b);
    w.u32v(lastFrameId);
    w.u32v(framesOk);
    w.u32v(framesLost);
    w.u32v(recvKbps);
    w.u32v(decodeUs);
    w.u32v(presentUs);
    w.u32v(latencyUs);
    w.u16v(queueDepth);
    w.u16v(rttMs);
    return b;
}

bool FeedbackMsg::parse(const u8* p, size_t n, FeedbackMsg& out) {
    ByteReader r(p, n);
    return r.u32v(out.lastFrameId) && r.u32v(out.framesOk) && r.u32v(out.framesLost) &&
           r.u32v(out.recvKbps) && r.u32v(out.decodeUs) && r.u32v(out.presentUs) &&
           r.u32v(out.latencyUs) && r.u16v(out.queueDepth) && r.u16v(out.rttMs);
}

// -----------------------------------------------------------------------------
// 输入包
// -----------------------------------------------------------------------------
Bytes encode_input_packet(const InputEvent* ev, size_t count) {
    Bytes b;
    b.reserve(2 + count * sizeof(InputEvent));
    ByteWriter w(b);
    w.u16v(u16(count));
    for (size_t i = 0; i < count; ++i) {
        const InputEvent& e = ev[i];
        w.u8v(e.type);
        w.u8v(e.flags);
        w.u16v(e.seq);
        w.i32v(e.a);
        w.i32v(e.b);
    }
    return b;
}

bool decode_input_packet(const u8* p, size_t n, InputEvent* out, size_t maxOut, size_t& outCount) {
    ByteReader r(p, n);
    u16 count = 0;
    if (!r.u16v(count)) return false;
    if (count > maxOut) return false;
    for (u16 i = 0; i < count; ++i) {
        InputEvent e;
        if (!r.u8v(e.type) || !r.u8v(e.flags) || !r.u16v(e.seq)) return false;
        if (!r.i32v(e.a) || !r.i32v(e.b)) return false;
        out[i] = e;
    }
    outCount = count;
    return true;
}

// -----------------------------------------------------------------------------
// 可靠输入批次（TCP）
// -----------------------------------------------------------------------------
Bytes encode_input_batch(u32 batchId, const InputEvent* ev, size_t count) {
    if (count > kMaxInputBatchEvents) count = kMaxInputBatchEvents;
    Bytes b;
    b.reserve(6 + count * sizeof(InputEvent));
    ByteWriter w(b);
    w.u32v(batchId);
    w.u16v(u16(count));
    for (size_t i = 0; i < count; ++i) {
        const InputEvent& e = ev[i];
        w.u8v(e.type);
        w.u8v(e.flags);
        w.u16v(e.seq);
        w.i32v(e.a);
        w.i32v(e.b);
    }
    return b;
}

bool parse_input_batch(const u8* p, size_t n, InputEvent* out, size_t maxOut,
                       InputBatchView& view) {
    view = InputBatchView{};
    ByteReader r(p, n);
    if (!r.u32v(view.batchId)) return false;
    u16 count = 0;
    if (!r.u16v(count)) return false;
    if (count > maxOut) return false;
    for (u16 i = 0; i < count; ++i) {
        InputEvent e;
        if (!r.u8v(e.type) || !r.u8v(e.flags) || !r.u16v(e.seq)) return false;
        if (!r.i32v(e.a) || !r.i32v(e.b)) return false;
        out[i] = e;
    }
    view.count = count;
    view.events = out;
    return true;
}

Bytes encode_input_applied(u32 batchId, u32 applied, u8 status) {
    Bytes b;
    ByteWriter w(b);
    w.u32v(batchId);
    w.u32v(applied);
    w.u8v(status);
    return b;
}

bool InputAppliedMsg::parse(const u8* p, size_t n) {
    ByteReader r(p, n);
    return r.u32v(batchId) && r.u32v(applied) && r.u8v(status);
}

u16 make_scancode(u32 vkey, bool& isExtended) {
#ifdef _WIN32
    const UINT sc = ::MapVirtualKeyW(vkey, MAPVK_VK_TO_VSC_EX);
    if (sc & 0xE000u) {
        isExtended = true;
        return u16(sc & 0xFFu);
    }
    isExtended = false;
    return u16(sc & 0xFFu);
#else
    isExtended = false;
    return u16(vkey & 0xFF);
#endif
}

} // namespace ms::proto
