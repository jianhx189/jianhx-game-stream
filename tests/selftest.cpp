// =============================================================================
//  selftest.cpp - 无界面自检
//
//  验证最容易写错、又最难在串流时排查的部分：
//    * SHA-256 / HMAC-SHA256 是否符合标准测试向量
//    * INI 读写往返
//    * 协议序列化往返
//    * 输入包编解码
//  运行：ministream-selftest.exe
// =============================================================================
#include "../src/common/crypt.h"
#include "../src/common/serialize.h"
#include "../src/common/protocol.h"
#include "../src/common/fec.h"
#include "../src/common/config.h"
#include "../src/common/quality.h"
#include "../src/common/clock.h"
#include "../src/common/log.h"
#include "../src/common/crash.h"
#include "../src/common/net.h"
#include "../src/host/host_service.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

using namespace ms;

static int g_fail = 0;
static int g_pass = 0;

#define CHECK(cond, what)                                                        \
    do {                                                                         \
        if (cond) { ++g_pass; std::printf("  [ OK ] %s\n", what); }               \
        else { ++g_fail; std::printf("  [FAIL] %s  (%s:%d)\n", what, __FILE__, __LINE__); } \
    } while (0)

static std::string hex(const u8* d, size_t n) {
    static const char* h = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; ++i) { s.push_back(h[d[i] >> 4]); s.push_back(h[d[i] & 0xF]); }
    return s;
}

static void test_sha256() {
    std::printf("[SHA-256]\n");
    u8 d[32];

    crypt::sha256("", 0, d);
    CHECK(hex(d, 32) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
          "空串向量");

    crypt::sha256("abc", 3, d);
    CHECK(hex(d, 32) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "abc 向量");

    const char* s = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    crypt::sha256(s, std::strlen(s), d);
    CHECK(hex(d, 32) == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
          "长串向量");

    // 跨块边界（>64 字节）也要对
    std::string big(1000, 'a');
    crypt::sha256(big.data(), big.size(), d);
    CHECK(hex(d, 32) == "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3",
          "1000 个 a");
}

static void test_hmac() {
    std::printf("[HMAC-SHA256]\n");
    u8 key[20];
    std::memset(key, 0x0b, sizeof(key));
    u8 out[32];
    const char* msg = "Hi There";
    crypt::hmac_sha256(key, sizeof(key), (const u8*)msg, std::strlen(msg), out);
    CHECK(hex(out, 32) == "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7",
          "RFC 4231 case 1");

    // 长密钥（>64 字节）分支
    u8 longKey[131];
    std::memset(longKey, 0xaa, sizeof(longKey));
    const char* data = "Test Using Larger Than Block-Size Key - Hash Key First";
    crypt::hmac_sha256(longKey, sizeof(longKey), (const u8*)data, std::strlen(data), out);
    CHECK(hex(out, 32) == "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54",
          "RFC 4231 case 6（长密钥）");
}

static void test_proof_deterministic() {
    std::printf("[口令挑战应答]\n");
    u8 k1[32], k2[32];
    crypt::derive_key("hunter2", k1);
    crypt::derive_key("hunter2", k2);
    crypt::derive_key("hunter3", k2);
    CHECK(std::memcmp(k1, k2, 32) != 0, "不同口令派生出不同密钥");

    crypt::derive_key("hunter2", k2);
    CHECK(std::memcmp(k1, k2, 32) == 0, "相同口令派生出相同密钥");

    u8 cn[16], sn[16], salt[16], p1[32], p2[32];
    crypt::random_bytes(cn, 16); crypt::random_bytes(sn, 16); crypt::random_bytes(salt, 16);
    crypt::compute_proof(k1, cn, sn, salt, p1);
    crypt::compute_proof(k2, cn, sn, salt, p2);
    CHECK(std::memcmp(p1, p2, 32) == 0, "proof 可复现");

    salt[0] ^= 0xFF;
    crypt::compute_proof(k2, cn, sn, salt, p2);
    CHECK(std::memcmp(p1, p2, 32) != 0, "salt 变化导致 proof 变化");
}

static void test_serialize() {
    std::printf("[字节序列化]\n");
    Bytes b;
    ByteWriter w(b);
    w.u8v(0x12);
    w.u16v(0x3456);
    w.u32v(0x789ABCDE);
    w.i32v(-12345);
    w.i64v(-9876543210LL);
    w.str8("hello");

    ByteReader r(b);
    u8 a8; u16 a16; u32 a32; i32 s32; i64 s64; std::string str;
    CHECK(r.u8v(a8) && a8 == 0x12, "u8");
    CHECK(r.u16v(a16) && a16 == 0x3456, "u16 小端");
    CHECK(r.u32v(a32) && a32 == 0x789ABCDE, "u32 小端");
    CHECK(r.i32v(s32) && s32 == -12345, "有符号 i32");
    CHECK(r.i64v(s64) && s64 == -9876543210LL, "有符号 i64");
    CHECK(r.str8(str) && str == "hello", "短字符串");
    CHECK(r.left() == 0, "读完后无剩余");

    CHECK(b[0] == 0x12 && b[1] == 0x56 && b[2] == 0x34, "字节顺序正确");
}

static void test_protocol() {
    std::printf("[协议消息往返]\n");
    {
        proto::HelloMsg h;
        h.version = proto::kVersion;
        h.wantWidth = 2560; h.wantHeight = 1440; h.wantFps = 144;
        h.wantBitrateKbps = 50000; h.displayIndex = 1;
        h.clientName = "test-client";
        crypt::random_bytes(h.nonce, proto::kNonceLen);
        const Bytes b = h.serialize();
        proto::HelloMsg h2;
        CHECK(proto::HelloMsg::parse(b.data(), b.size(), h2), "Hello 解析成功");
        CHECK(h2.wantWidth == 2560 && h2.wantHeight == 1440 && h2.wantFps == 144,
              "Hello 参数一致");
        CHECK(h2.clientName == "test-client", "Hello 名称一致");
        CHECK(std::memcmp(h.nonce, h2.nonce, proto::kNonceLen) == 0, "Hello nonce 一致");
    }
    {
        proto::PongMsg p;
        p.seq = 7; p.t1 = 111111; p.t2 = 222222; p.t3 = 333333;
        const Bytes b = p.serialize();
        proto::PongMsg p2;
        CHECK(proto::PongMsg::parse(b.data(), b.size(), p2), "Pong 解析成功");
        CHECK(p2.seq == 7 && p2.t3 == 333333, "Pong 字段一致");
    }
    {
        proto::AuthResultMsg a;
        a.ok = 0; a.sessionId = 0xDEADBEEF; a.message = u8"验证码错误";
        const Bytes b = a.serialize();
        proto::AuthResultMsg a2;
        CHECK(proto::AuthResultMsg::parse(b.data(), b.size(), a2), "AuthResult 解析成功");
        CHECK(a2.ok == 0 && a2.sessionId == 0xDEADBEEF, "AuthResult 字段一致");
    }
    {
        const Bytes pl = proto::HelloMsg{}.serialize();
        const Bytes frame = proto::encode_tcp_frame(proto::MsgType::Hello, pl);
        proto::TcpHeader th;
        CHECK(proto::parse_tcp_header(frame.data(), frame.size(), th), "TCP 帧头解析");
        CHECK(th.type == proto::MsgType::Hello, "TCP 帧类型正确");
        CHECK(th.length == 4 + pl.size(), "TCP 帧长度正确");
    }
    {
        proto::FeedbackMsg f;
        f.lastFrameId = 12345; f.framesOk = 100; f.framesLost = 3;
        f.recvKbps = 20000; f.decodeUs = 1500; f.presentUs = 800;
        f.latencyUs = 12000; f.queueDepth = 2; f.rttMs = 18;
        const Bytes b = f.serialize();
        proto::FeedbackMsg f2;
        CHECK(proto::FeedbackMsg::parse(b.data(), b.size(), f2), "Feedback 解析成功");
        CHECK(f2.lastFrameId == 12345 && f2.framesLost == 3 && f2.rttMs == 18,
              "Feedback 字段一致");
    }
    {
        proto::DisconnectMsg d;
        d.code = 42; d.reason = "bye";
        const Bytes b = d.serialize();
        proto::DisconnectMsg d2;
        CHECK(proto::DisconnectMsg::parse(b.data(), b.size(), d2) &&
              d2.code == 42 && d2.reason == "bye", "Disconnect 往返");
    }
    {
        proto::SetKeyframeIntervalMsg k;
        k.seconds = 3;
        const Bytes b = k.serialize();
        proto::SetKeyframeIntervalMsg k2;
        CHECK(proto::SetKeyframeIntervalMsg::parse(b.data(), b.size(), k2) &&
              k2.seconds == 3, "SetKeyframeInterval 往返");
    }
    {
        proto::ReconfigureMsg r;
        r.width = 1920; r.height = 1080; r.fps = 120;
        const Bytes b = r.serialize();
        proto::ReconfigureMsg r2;
        CHECK(proto::ReconfigureMsg::parse(b.data(), b.size(), r2) &&
              r2.width == 1920 && r2.height == 1080 && r2.fps == 120,
              "Reconfigure 往返");
        // 0（跟随原生）也要能过
        proto::ReconfigureMsg r0;
        r0.width = 0; r0.height = 0; r0.fps = 60;
        const Bytes b0 = r0.serialize();
        proto::ReconfigureMsg r3;
        CHECK(proto::ReconfigureMsg::parse(b0.data(), b0.size(), r3) &&
              r3.width == 0 && r3.fps == 60, "Reconfigure 0x0 原生值");
    }
    {
        // ---- 传输模式 / 探测（自愈的核心消息）----
        proto::TransportModeMsg tm;
        tm.mode = 1; tm.probe = 0;
        const Bytes b = tm.serialize();
        proto::TransportModeMsg tm2;
        CHECK(proto::TransportModeMsg::parse(b.data(), b.size(), tm2) &&
              tm2.mode == 1 && tm2.probe == 0, "TransportMode(切 TCP) 往返");
        proto::TransportModeMsg tmd;
        tmd.mode = 0; tmd.probe = 1;
        const Bytes bd = tmd.serialize();
        proto::TransportModeMsg tmd2;
        CHECK(proto::TransportModeMsg::parse(bd.data(), bd.size(), tmd2) &&
              tmd2.mode == 0 && tmd2.probe == 1, "TransportMode(UDP 探测) 往返");
        // 兼容旧版：空 body 表示「切 TCP」
        proto::TransportModeMsg legacy;
        CHECK(proto::TransportModeMsg::parse(nullptr, 0, legacy) && legacy.mode == 1,
              "空 body 兼容为切 TCP");
    }
    {
        // ---- 光标位置（含游戏内场的 visible=0）----
        proto::PointerPosMsg p;
        p.seq = 0xDEADBEEFu; p.x = -12; p.y = 3456; p.visible = 1;
        const Bytes b = p.serialize();
        proto::PointerPosMsg p2;
        CHECK(proto::PointerPosMsg::parse(b.data(), b.size(), p2), "PointerPos 解析");
        CHECK(p2.seq == 0xDEADBEEFu && p2.x == -12 && p2.y == 3456 && p2.visible == 1,
              "PointerPos 字段一致（含负坐标）");
        p.visible = 0;
        const Bytes b0 = p.serialize();
        proto::PointerPosMsg p3;
        CHECK(proto::PointerPosMsg::parse(b0.data(), b0.size(), p3) && p3.visible == 0,
              "PointerPos 可见性=0（游戏内场藏光标）");
    }
    {
        // ---- 光标形状（预乘 BGRA）----
        proto::PointerShapeMsg s;
        s.shapeId = 3; s.width = 4; s.height = 2; s.hotX = 1; s.hotY = 2;
        s.bgra.assign(size_t(4) * 2u * 4u, 0x7F);
        const Bytes b = s.serialize();
        CHECK(b.size() == proto::kPointerShapeHeaderSize + 4u * 2u * 4u,
              "PointerShape 长度 = 头 + w*h*4");
        proto::PointerShapeMsg::View v;
        CHECK(proto::PointerShapeMsg::parse(b.data(), b.size(), v), "PointerShape 解析");
        CHECK(v.shapeId == 3 && v.width == 4 && v.height == 2 &&
              v.hotX == 1 && v.hotY == 2 && v.len == 4u * 2u * 4u,
              "PointerShape 字段一致");
        // 截断必须被拒绝（坏包不能造成客户端越界读）
        CHECK(!proto::PointerShapeMsg::parse(b.data(), b.size() - 1, v),
              "PointerShape 截断被拒绝");
        proto::PointerShapeMsg bad;
        bad.width = 0; bad.height = 2; bad.bgra.assign(8, 0);
        const Bytes bb = bad.serialize();
        CHECK(!proto::PointerShapeMsg::parse(bb.data(), bb.size(), v),
              "PointerShape 0 尺寸被拒绝");
        proto::PointerShapeMsg huge;
        huge.width = u16(proto::kMaxPointerShapeDim + 1); huge.height = 1;
        huge.bgra.assign(size_t(huge.width) * 4u, 0);
        const Bytes hb = huge.serialize();
        CHECK(!proto::PointerShapeMsg::parse(hb.data(), hb.size(), v),
              "PointerShape 超大尺寸被拒绝");
    }
    {
        // ---- 音频包（Opus，TCP 兜底路径的 8 字节头 + 数据）----
        const u8 opus[5] = { 0xFC, 0x01, 0x02, 0x03, 0x04 };
        const Bytes b = proto::encode_audio_packet(0x12345678u, 960u, opus, sizeof(opus));
        CHECK(b.size() == 8 + sizeof(opus), "AudioPacket 长度 = 8 字节头 + Opus 数据");
        proto::AudioPacketView v;
        CHECK(proto::parse_audio_packet(b.data(), b.size(), v), "AudioPacket 解析");
        CHECK(v.seq == 0x12345678u && v.ts == 960u && v.len == sizeof(opus) &&
              v.data[0] == 0xFC,
              "AudioPacket 字段一致（seq/采样时间戳/数据）");
        // 截断/超长必须被拒绝
        CHECK(!proto::parse_audio_packet(b.data(), 4, v), "AudioPacket 截断被拒绝");
        Bytes tooBig(8 + proto::kMaxAudioPayload + 1, 0);
        CHECK(!proto::parse_audio_packet(tooBig.data(), tooBig.size(), v),
              "AudioPacket 超长被拒绝");
        // 20ms 一帧的采样数不能改（两端必须一致，否则声音会变速）
        CHECK(proto::kAudioSamplesPerFrame == 960 && proto::kAudioFrameMs == 20 &&
              proto::kAudioSampleRate == 48000,
              "音频固定参数 48kHz/20ms/960 采样不变");
    }
    {
        // ---- 枚举值必须稳定（两端版本不一致时靠报错而不是错乱）----
        CHECK(u8(proto::UdpType::Video) == 1 && u8(proto::UdpType::Input) == 2,
              "UdpType Video/Input 编号不变");
        CHECK(u8(proto::UdpType::Feedback) == 3 && u8(proto::UdpType::Keepalive) == 4,
              "UdpType Feedback/Keepalive 编号不变");
        CHECK(u8(proto::UdpType::Pointer) == 5, "UdpType Pointer = 5");
        CHECK(u8(proto::UdpType::Audio) == 6, "UdpType Audio = 6");
        CHECK(u16(proto::MsgType::VideoOverTcp) == 15 && u16(proto::MsgType::VideoFrame) == 16,
              "MsgType VideoOverTcp/VideoFrame 编号不变");
        CHECK(u16(proto::MsgType::PointerShape) == 17 && u16(proto::MsgType::PointerPos) == 18,
              "MsgType PointerShape/PointerPos 编号不变");
        CHECK(u16(proto::MsgType::AudioPacket) == 19, "MsgType AudioPacket = 19");
        CHECK(std::strcmp(proto::msg_type_name(proto::MsgType::PointerPos), "PointerPos") == 0,
              "msg_type_name(PointerPos)");
        CHECK(std::strcmp(proto::msg_type_name(proto::MsgType::AudioPacket), "AudioPacket") == 0,
              "msg_type_name(AudioPacket)");
    }
    {
        quality::QualityProfile p = quality::default_profile(quality::Preset::Smooth);
        CHECK(p.fps == 120 && p.keyframeIntervalSec == 1, "流畅档预设值正确");
        p = quality::default_profile(quality::Preset::Sharp);
        CHECK(p.bitrateKbps == 50000, "画质档预设值正确");
        CHECK(!quality::valid_bitrate(-1) && quality::valid_bitrate(200000),
              "码率合法性检查");
        CHECK(!quality::valid_dimensions(1920, 0), "宽高必须同时为 0 或同时非 0");
    }
}

static void test_input_packet() {
    std::printf("[输入包编解码]\n");
    proto::InputEvent evs[3];
    evs[0].type = u8(proto::InputType::MouseMove); evs[0].seq = 1; evs[0].a = -17; evs[0].b = 42;
    evs[1].type = u8(proto::InputType::Key); evs[1].seq = 2; evs[1].a = 0x11; evs[1].flags = 0;
    evs[2].type = u8(proto::InputType::Key); evs[2].seq = 2; evs[2].a = 0x11;
    evs[2].flags = proto::kInputFlagKeyUp | proto::kInputFlagKeyExt;

    const Bytes b = proto::encode_input_packet(evs, 3);
    CHECK(b.size() == 2 + 3 * sizeof(proto::InputEvent), "输入包长度 = 2 + n*12");

    proto::InputEvent out[8];
    size_t cnt = 0;
    CHECK(proto::decode_input_packet(b.data(), b.size(), out, 8, cnt), "输入包解析成功");
    CHECK(cnt == 3, "事件个数正确");
    CHECK(out[0].a == -17 && out[0].b == 42, "鼠标位移往返");
    CHECK(out[2].flags == (proto::kInputFlagKeyUp | proto::kInputFlagKeyExt),
          "按键标志位往返");
    CHECK(out[1].seq == out[2].seq, "连发事件 seq 相同");

    // 越界保护
    size_t c2 = 0;
    CHECK(!proto::decode_input_packet(b.data(), b.size(), out, 2, c2), "容量不足时拒绝解析");

    // 绝对鼠标映射（桌面/游戏菜单 1:1 跟手）：a/b 是远端像素坐标，必须原样往返
    {
        proto::InputEvent e{};
        e.type  = u8(proto::InputType::MouseMove);
        e.flags = proto::kInputFlagAbsMove;
        e.seq   = 7;
        e.a     = 1234;
        e.b     = 567;
        const Bytes pkt = proto::encode_input_packet(&e, 1);
        proto::InputEvent o2[4];
        size_t n2 = 0;
        CHECK(proto::decode_input_packet(pkt.data(), pkt.size(), o2, 4, n2) && n2 == 1 &&
              o2[0].flags == proto::kInputFlagAbsMove && o2[0].a == 1234 && o2[0].b == 567,
              "绝对鼠标事件往返（坐标不变）");
        CHECK((proto::kInputFlagAbsMove & (proto::kInputFlagKeyUp | proto::kInputFlagKeyExt)) == 0,
              "绝对鼠标标志与按键标志不冲突");
    }
}

// -----------------------------------------------------------------------------
// 视频 FEC（XOR 组码）：抗丢包的核心一层，算错了就是花屏/黑屏，必须单测
// -----------------------------------------------------------------------------
static void test_fec() {
    std::printf("[视频 FEC(XOR 组码)]\n");

    // --- 分片长度约定（收端不携带每片长度，全靠这条规则推）---
    CHECK(fec::frag_len(4000, 0) == proto::kMaxVideoPayload, "分片 0 为满长");
    CHECK(fec::frag_len(4000, 3) == 400, "末片按整帧长度截断");
    CHECK(fec::frag_len(4000, 4) == 0, "越界分片长度为 0");
    CHECK(fec::group_members(5, 3, 3) == 2, "末组成员数 = min(G, total-start)");
    CHECK(fec::group_members(5, 6, 3) == 0, "越界组没有成员");

    // --- 造一帧 5 个分片（4*1200 + 700 = 5500 字节）---
    const size_t frameBytes = 5500;
    const size_t total = (frameBytes + proto::kMaxVideoPayload - 1) / proto::kMaxVideoPayload;
    CHECK(total == 5, "分片数 = ceil(frameBytes / 1200)");
    std::vector<u8> frame(frameBytes);
    for (size_t i = 0; i < frameBytes; ++i) frame[i] = u8((i * 37 + 11) & 0xFF);

    const u16 G = 3;
    const u32 groups = (u32(total) + G - 1) / G;
    CHECK(groups == 2, "5 片按 3 片一组 = 2 组");

    // --- 发送端：每组算一个校验包 ---
    std::vector<std::vector<u8>> parity(groups);
    for (u32 g = 0; g < groups; ++g) {
        const u32 start = g * G;
        const u32 members = fec::group_members(u32(total), start, G);
        const size_t plen = fec::parity_len(frameBytes, start, members);
        parity[g].assign(plen, 0);
        for (u32 i = 0; i < members; ++i) {
            const u32 idx = start + i;
            const size_t off = size_t(idx) * proto::kMaxVideoPayload;
            fec::xor_into(parity[g].data(), plen, frame.data() + off,
                          fec::frag_len(frameBytes, idx));
        }
    }
    CHECK(!parity[0].empty() && parity[0].size() == proto::kMaxVideoPayload,
          "满组校验净荷 = 组内最大分片长");
    CHECK(!parity[1].empty(), "末组也产生校验包");

    // --- 收端：每组各丢 1 片，用校验恢复后必须逐字节等于原帧 ---
    std::vector<u8> rebuilt(frameBytes, 0);
    for (u32 g = 0; g < groups; ++g) {
        const u32 start = g * G;
        const u32 members = fec::group_members(u32(total), start, G);
        const u32 drop = start + (g % members);       // 每组丢一片
        const u8* parts[proto::kFecMaxGroup] = {};
        size_t lens[proto::kFecMaxGroup] = {};
        size_t np = 0;
        for (u32 i = 0; i < members; ++i) {
            const u32 idx = start + i;
            if (idx == drop) continue;
            const size_t off = size_t(idx) * proto::kMaxVideoPayload;
            const size_t len = fec::frag_len(frameBytes, idx);
            std::memcpy(rebuilt.data() + off, frame.data() + off, len);
            parts[np] = frame.data() + off;
            lens[np]  = len;
            ++np;
        }
        const size_t outLen = fec::frag_len(frameBytes, drop);
        const bool ok = fec::xor_recover(
            rebuilt.data() + size_t(drop) * proto::kMaxVideoPayload, outLen,
            parts, lens, np, parity[g].data(), parity[g].size());
        CHECK(ok, "缺 1 片可被 FEC 恢复");
    }
    bool same = (std::memcmp(rebuilt.data(), frame.data(), frameBytes) == 0);
    CHECK(same, "恢复后整帧与原帧逐字节一致");

    // --- 缺 2 片：XOR 组码救不回来，恢复结果必然错（上层据此整帧丢弃、等 IDR）---
    {
        const u32 members = fec::group_members(u32(total), 0, G);
        const u8* parts[1] = { frame.data() + proto::kMaxVideoPayload };
        const size_t lens[1] = { proto::kMaxVideoPayload };
        std::vector<u8> out(proto::kMaxVideoPayload, 0);
        fec::xor_recover(out.data(), proto::kMaxVideoPayload, parts, lens, 1,
                         parity[0].data(), parity[0].size());
        CHECK(std::memcmp(out.data(), frame.data(), proto::kMaxVideoPayload) != 0,
              "缺 2 片时恢复结果必然错误（不能当成功）");
        CHECK(members == 3, "满组成员数 = G");
    }

    // --- 常量合法性 ---
    CHECK(proto::kFecMinGroup >= 3 && proto::kFecMaxGroup <= 32, "FEC 组大小常量在合理区间");
    CHECK(proto::kFecDefaultGroup >= proto::kFecMinGroup &&
          proto::kFecDefaultGroup <= proto::kFecMaxGroup, "默认组大小落区间内");
    CHECK(fec::group_members(1, 0, 10) == 1, "单分片帧只有 1 个成员（不发校验）");
}

static void test_ini() {
    std::printf("[INI 读写]\n");
    const std::string path = "selftest_tmp.ini";
    std::remove(path.c_str());

    IniFile ini;
    ini.set_str("server", "password", "p@ss=word");   // 值里有 '=' 也要能读回
    ini.set_int("server", "port", 21000);
    ini.set_bool("input", "enabled", true);
    CHECK(ini.save(path), "写入 ini");

    IniFile in2;
    CHECK(in2.load(path), "读取 ini");
    CHECK(in2.get_str("server", "password", "") == "p@ss=word", "字符串值往返");
    CHECK(in2.get_int("server", "port", 0) == 21000, "整数值往返");
    CHECK(in2.get_bool("input", "enabled", false) == true, "布尔值往返");
    CHECK(in2.get_int("server", "missing", 123) == 123, "缺省值");
    CHECK(in2.get_bool("nope", "nope", true) == true, "不存在的节返回缺省值");

    std::remove(path.c_str());
}

static void test_config_structs() {
    std::printf("[配置结构]\n");
    HostConfig h;
    h.port = 20000;
    h.password = "x";
    std::string err;
    CHECK(h.validate(err), "默认被控端配置合法");

    h.port = 9000;
    CHECK(!h.validate(err), "端口 <=10000 被拒绝");

    h.port = 20000;
    h.fps = 75;
    CHECK(!h.validate(err), "非法帧率被拒绝");

    h.fps = 60;
    h.password.clear();
    CHECK(!h.validate(err), "空密码被拒绝");

    ClientConfig c;
    c.host = "1.2.3.4"; c.port = 20000; c.password = "y";
    CHECK(c.validate(err), "默认远控端配置合法");
    c.port = 10000;
    CHECK(!c.validate(err), "端口 10000 被拒绝（要求 >10000）");

    CHECK(auto_bitrate_kbps(1920, 1080, 60) >= 15000, "1080p60 自适应码率合理");
    CHECK(auto_bitrate_kbps(1280, 720, 60) < auto_bitrate_kbps(1920, 1080, 60),
          "720p 码率低于 1080p");
}

static void test_clock() {
    std::printf("[时钟]\n");
    const i64 a = now_us();
    sleep_ms(20);
    const i64 b = now_us();
    CHECK(b - a >= 15000 && b - a <= 200000, "sleep 20ms 测得 15~200ms");

    // 时钟同步验算。链路对称（去程/回程各 d 微秒），远端相对本地偏移 D：
    //   T2 = T1 + d + D      T3 = T2 + proc      T4 = T3 + d - D
    {
        const i64 d = 60000, proc = 1000, D = 250000;
        const i64 t1 = 1000000;
        const i64 t2 = t1 + d + D;
        const i64 t3 = t2 + proc;
        const i64 t4 = t3 + d - D;
        ClockSync cs;
        cs.update(t1, t2, t3, t4);
        CHECK(cs.valid(), "时钟样本被接受");
        CHECK(cs.offset_us() >= D - 1000 && cs.offset_us() <= D + 1000,
              "对称链路 offset ≈ 真实偏移 D");
        CHECK(cs.last_rtt_us() > 0, "RTT 为正");
        // remote_to_local：远端时间戳 5,000,000 对应本地 5,000,000 - D
        CHECK(cs.remote_to_local(5000000) == 5000000 - cs.offset_us(),
              "remote_to_local 使用估计偏移（符号：本地 = 远端 - offset）");
        CHECK(cs.local_to_remote(5000000) == 5000000 + cs.offset_us(),
              "local_to_remote 使用估计偏移（符号：远端 = 本地 + offset）");
    }

    // 完全对称且偏移为 0 时，估计值必须接近 0
    {
        const i64 d = 60000, proc = 1000;
        const i64 t1 = 2000000;
        const i64 t2 = t1 + d;
        const i64 t3 = t2 + proc;
        const i64 t4 = t3 + d;
        ClockSync cs;
        cs.update(t1, t2, t3, t4);
        CHECK(cs.offset_us() >= -100 && cs.offset_us() <= 100, "零偏移链路 offset ≈ 0");
    }

    ClockSync cs2;
    cs2.update(1000000, 1560000, 1561000, 2120000);
    CHECK(cs2.offset_us() > 0, "远端时钟超前时 offset 为正");
    CHECK(cs2.remote_to_local(5000000) < 5000000,
          "远端时钟超前时，同一时刻的远端时间戳大于本地（换算变回较小本地值）");
    // 反向换算必须能回到原值
    CHECK(cs2.local_to_remote(cs2.remote_to_local(5000000)) == 5000000,
          "两种换算互为逆运算");

    // 【回归】QPC 换算不许溢出：freq=10MHz、开机 20.61 天时，
    // 老实现 (ticks*1000000)/freq 会溢出成负数（真机就是这么坏掉的：
    // now_us() 变负 -> "距上次心跳超过 500ms" 永远不成立 -> 一个 Ping 都发不出去）。
    {
        const i64 freq  = 10000000;
        const i64 ticks = 17807998027789LL;      // 20.61 天
        const i64 us    = us_from_ticks(ticks, freq);
        CHECK(us > 0, "开机 20 天时 now_us 换算仍为正数（旧实现为负）");
        CHECK(us == 1780799802778LL, "开机 20 天换算值精确（1780799802778 us）");
        // 换算必须单调、且 1 秒增量准确
        CHECK(us_from_ticks(ticks + freq, freq) - us == 1000000,
              "开机 20 天时 1 秒增量仍准确");
        // 普通小值也正确：1.234567 s
        CHECK(us_from_ticks(12345678LL, freq) == 1234567LL,
              "小计数换算正确（1.234567 s）");
    }
}

static void test_scancode() {
    std::printf("[扫描码]\n");
    bool ext = false;
    const u16 scA = proto::make_scancode('A', ext);
    CHECK(scA == 0x1E, "A 键扫描码 = 0x1E");
    CHECK(!ext, "A 键不是扩展键");

    const u16 scRight = proto::make_scancode(VK_RIGHT, ext);
    CHECK(scRight == 0x4D, "方向键右键扫描码 = 0x4D");

    const u16 scEnter = proto::make_scancode(VK_RETURN, ext);
    CHECK(scEnter == 0x1C, "回车扫描码 = 0x1C");
}

static void test_net_addr() {
    std::printf("[地址解析]\n");
    sockaddr_in a{};
    std::string err;
    CHECK(net::resolve_ipv4("127.0.0.1", 20000, a, err), "解析 127.0.0.1");
    CHECK(net::addr_str(a) == "127.0.0.1", "地址字符串正确");
    CHECK(net::addr_port(a) == 20000, "端口正确");
    CHECK(net::resolve_ipv4("localhost", 12345, a, err), "解析 localhost");
    CHECK(net::addr_port(a) == 12345, "localhost 端口正确");
}

// -----------------------------------------------------------------------------
// 被控端生命周期并发回归
//
// 真实故障（用户实测）：被控端在托盘里"停止服务 / 退出"时整个进程崩溃，
// Windows 事件日志记的是 ministream-host.exe 异常码 0xC0000409，
// 崩溃转储显示 fastfail 子码 7（abort），栈上是 VCRUNTIME 的异常帧，
// 异常来自 accept 线程 —— 即 std::terminate：有 C++ 异常没人接。
//
// 根因：HostService::stop() 与 accept 线程的会话收尾会**同时 join 同一个
// std::thread**（两个 stop() 并发、或 stop() 撞上会话收尾）。MSVC 的
// join() 在句柄已被另一个线程 join 掉时抛 std::system_error，线程函数里
// 没有 catch 就直接 std::terminate -> abort。
//
// 这里直接对着真实 HostService 打并发 stop，并跑一个真实会话，
// 保证"并发停止"永远安全（修复前这段会让自检进程自己崩掉）。
// -----------------------------------------------------------------------------
static ms::HostConfig lifecycle_cfg(u16 port) {
    ms::HostConfig c;
    c.bind = "127.0.0.1";
    c.port = port;
    c.password = "test123";
    c.fps = 30;
    c.bitrateKbps = 2000;
    c.monitor = 0;
    c.inputEnabled = false;
    c.keyframeIntervalSec = 2;
    return c;
}

// 极简远控端：跑完 Hello/Challenge/Auth 握手，然后保持连接直到被断开
static void lifecycle_fake_client(u16 port, std::string password,
                                  std::atomic<bool>* connected,
                                  std::atomic<bool>* quit) {
    std::string err;
    net::TcpChannel ch;
    if (!ch.connect("127.0.0.1", port, 3000, err)) return;

    proto::HelloMsg hello;
    hello.version = proto::kVersion;
    crypt::random_bytes(hello.nonce, proto::kNonceLen);
    hello.wantFps = 30;
    hello.wantBitrateKbps = 2000;
    if (!ch.send_frame(proto::MsgType::Hello, hello.serialize())) return;

    proto::MsgType t;
    Bytes pl;
    if (ch.recv_frame(t, pl, 4000) <= 0 || t != proto::MsgType::Challenge) return;
    proto::ChallengeMsg chal;
    if (!proto::ChallengeMsg::parse(pl.data(), pl.size(), chal)) return;

    proto::AuthMsg auth;
    u8 key[32];
    crypt::derive_key(password, key);
    crypt::compute_proof(key, hello.nonce, chal.serverNonce, chal.salt, auth.proof);
    if (!ch.send_frame(proto::MsgType::Auth, auth.serialize())) return;

    if (ch.recv_frame(t, pl, 4000) <= 0 || t != proto::MsgType::AuthResult) return;
    proto::AuthResultMsg ar;
    if (!proto::AuthResultMsg::parse(pl.data(), pl.size(), ar) || !ar.ok) return;

    connected->store(true);
    while (!quit->load()) {
        if (ch.recv_frame(t, pl, 200) < 0) break;
    }
    ch.close();
}

static void test_host_lifecycle() {
    std::printf("[被控端生命周期 / 并发停止]\n");

    // (1) 启动后三个线程同时 stop()
    {
        ms::host::HostService svc;
        std::string err;
        const bool started = svc.start(lifecycle_cfg(u16(28101)), err);
        CHECK(started, "被控端启动成功");
        if (started) {
            std::atomic<int> done{0};
            std::atomic<int> thrown{0};
            std::string what[3];
            auto stopper = [&](int i) {
                try {
                    svc.stop();
                } catch (const std::exception& e) {
                    what[i] = e.what();
                    ++thrown;
                } catch (...) {
                    what[i] = "非 std::exception";
                    ++thrown;
                }
                ++done;
            };
            std::thread a(stopper, 0);
            std::thread b(stopper, 1);
            std::thread c(stopper, 2);
            a.join();
            b.join();
            c.join();
            CHECK(done.load() == 3, "三个线程并发 stop() 全部正常返回（不崩溃）");
            if (thrown.load() != 0) {
                std::printf("        并发 stop() 抛了 %d 个异常: [0]%s [1]%s [2]%s\n",
                            thrown.load(), what[0].c_str(), what[1].c_str(), what[2].c_str());
            }
            CHECK(thrown.load() == 0, "并发 stop() 不抛异常（修复前 MSVC join() 抛 system_error）");
            CHECK(!svc.running(), "停止后 running() == false");
        }
    }

    // (2) 会话进行中并发 stop()：最接近实际崩溃场景
    {
        ms::host::HostService svc;
        std::string err;
        const ms::HostConfig c = lifecycle_cfg(u16(28102));
        const bool started = svc.start(c, err);
        CHECK(started, "被控端启动成功（会话场景）");
        if (started) {
            std::atomic<bool> connected{false};
            std::atomic<bool> quit{false};
            std::thread cli(lifecycle_fake_client, c.port, c.password,
                            &connected, &quit);
            for (int i = 0; i < 300 && !connected.load(); ++i) sleep_ms(10);
            CHECK(connected.load(), "远控端握手通过（Hello/Challenge/Auth）");

            std::atomic<int> thrown2{0};
            std::string what2[2];
            auto stopper2 = [&](int i) {
                try {
                    svc.stop();
                } catch (const std::exception& e) {
                    what2[i] = e.what();
                    ++thrown2;
                } catch (...) {
                    what2[i] = "非 std::exception";
                    ++thrown2;
                }
            };
            std::thread a(stopper2, 0);
            std::thread b(stopper2, 1);
            a.join();
            b.join();
            if (thrown2.load() != 0) {
                std::printf("        会话中并发 stop() 抛了 %d 个异常: [0]%s [1]%s\n",
                            thrown2.load(), what2[0].c_str(), what2[1].c_str());
            }
            CHECK(thrown2.load() == 0, "会话进行中并发 stop() 未崩溃且不抛异常");

            quit.store(true);
            cli.join();
            svc.stop();
        }
    }

    // (3) 远控端断开后，被控端必须自己回到监听状态（不点"停止服务"）
    //     【回归】修复前：输入线程的循环条件没检查 sessionActive，会话结束后
    //     输入线程永远不退，join_session_threads() 里 join(thInput) 永久阻塞，
    //     被控端从此不再监听（TCP 端口消失），远控端重连只能黑屏/连不上。
    //     真机上的表现就是"连上一次，之后再也连不上"，自测原来的启停用例发现不了。
    {
        ms::host::HostService svc;
        std::string err;
        const ms::HostConfig c = lifecycle_cfg(u16(28103));
        const bool started = svc.start(c, err);
        CHECK(started, "被控端启动成功（断线重连场景）");
        if (started) {
            std::atomic<bool> connected{false};
            std::atomic<bool> quit{false};
            std::thread cli(lifecycle_fake_client, c.port, c.password, &connected, &quit);
            for (int i = 0; i < 400 && !connected.load(); ++i) sleep_ms(10);
            CHECK(connected.load(), "首个远控端握手通过（断线重连场景）");

            sleep_ms(800);            // 让会话真正跑起来（起串流/输入线程）
            quit.store(true);         // 模拟远控端断开
            cli.join();

            bool relisten = false;
            for (int i = 0; i < 60; ++i) {
                sleep_ms(100);
                net::TcpChannel probe;
                std::string e2;
                if (probe.connect("127.0.0.1", c.port, 300, e2)) {
                    relisten = true;
                    probe.close();
                    break;
                }
            }
            CHECK(relisten, "远控端断开后被控端自动回到监听（修复前永久卡死）");

            std::atomic<bool> connected2{false};
            std::atomic<bool> quit2{false};
            std::thread cli2(lifecycle_fake_client, c.port, c.password, &connected2, &quit2);
            for (int i = 0; i < 500 && !connected2.load(); ++i) sleep_ms(10);
            CHECK(connected2.load(), "断线后可以再次建立会话（重连可用）");
            quit2.store(true);
            cli2.join();
            svc.stop();
        }
    }

    // (4) 连续启停：线程与套接字必须被干净回收
    {
        bool all = true;
        for (int i = 0; i < 8; ++i) {
            ms::host::HostService svc;
            std::string err;
            if (!svc.start(lifecycle_cfg(u16(28110 + i)), err)) { all = false; break; }
            sleep_ms(5);
            svc.stop();
        }
        CHECK(all, "连续 8 次启动/停止都成功（端口与线程已回收）");
    }
}

int main(int argc, char** argv) {
    std::printf("MiniStream 自检\n");
    std::printf("========================================\n");

    // 自检自己也装崩溃处理器：并发/生命周期用例万一崩了，
    // 要能拿到 what() 和调用栈，而不是只留一个 0xC0000409。
    ms::crash::install("selftest");
    std::printf("崩溃日志: %s\n", ms::crash::log_path());

    // 验证崩溃处理器本身有效：ministream-selftest.exe --terminate-selftest
    if (argc > 1 && std::strcmp(argv[1], "--terminate-selftest") == 0) {
        std::printf("故意触发 std::terminate（验证崩溃处理器）\n");
        std::terminate();
    }

    net::init();
    // 自检是控制台工具：把被控端/远控端的日志也回显出来，失败时才有上下文可看
    log_set_console(true);
    test_sha256();
    test_hmac();
    test_proof_deterministic();
    test_serialize();
    test_protocol();
    test_fec();
    test_input_packet();
    test_ini();
    test_config_structs();
    test_clock();
    test_scancode();
    test_net_addr();
    test_host_lifecycle();
    net::shutdown();

    std::printf("========================================\n");
    std::printf("通过 %d 项，失败 %d 项\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
