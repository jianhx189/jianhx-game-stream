// =============================================================================
//  video_receiver.cpp - UDP 收包 + 分片重组（独立线程）
//
//  数据流：recvfrom -> 来源过滤 -> UdpHeader 解析 -> 按 (frameId, index) 归位
//          -> 凑齐整帧 -> 压入深度 2 的队列 -> 渲染线程 pop
//
//  设计取舍：宁可丢帧也不要排队。任何「等一等也许就齐了」的想法都会让端到端
//  延迟累积，所以缺片超过 120ms 直接丢整帧并请求关键帧。
// =============================================================================
#include "video_receiver.h"
#include "../common/clock.h"
#include "../common/fec.h"
#include "../common/log.h"
#include "../common/perf.h"

#include <chrono>
#include <cstring>
#include <utility>

namespace ms::client {
namespace {

// 未完成帧的等待上限（微秒）。1080p60 一帧 16.7ms，120ms 相当于最多容忍
// 7 帧的乱序；再等下去画面只会越来越旧，不如丢帧换关键帧。
constexpr i64 kAssemblyTimeoutUs = 120 * 1000;

// pending_ 里比 highestFrameId_ 落后超过这么多帧的直接清掉，防止内存增长。
constexpr i32 kMaxReorderBacklog = 16;

// NAT 漂移容错：连续多少个包都来自同一个新来源，才承认服务端换地址了。
constexpr u32 kDriftSwitchPackets = 200;

// 一轮里最多收多少个包就让出一次（保证 stop() 能及时生效、超时清理能跑）
constexpr int kPacketsPerRound = 512;

// 没收到包时的空转间隔（毫秒）
constexpr int kIdleSleepMs = 1;

// 帧号回绕安全的差值
inline i32 frame_diff(u32 a, u32 b) { return i32(u32(a - b)); }

} // namespace

VideoReceiver::~VideoReceiver() {
    // 析构必须保证线程已经退出，否则线程会踩到已析构的成员
    shutdown();
}

// -----------------------------------------------------------------------------
// 生命周期
// -----------------------------------------------------------------------------
bool VideoReceiver::init(u16 listenPort, std::string& err) {
    if (udpSock_ >= 0) return true;
    if (running_.load()) { err = "接收线程已启动"; return false; }

    // 端口与被控端 TCP 端口相同，只是协议不同
    const int s = net::udp_open("0.0.0.0", listenPort, err);
    if (s < 0) return false;

    udpSock_ = s;
    // 非阻塞 + 4MB 收缓冲：一帧 1080p 可能一次涌进上千个包，缓冲太小会丢在网卡队列里
    net::udp_set_nonblocking(udpSock_, true);
    net::udp_set_buffers(udpSock_, 4 * 1024 * 1024, 4 * 1024 * 1024);
    reset_reassembly();
    // 启动一次会话的收包计数归零（防火墙诊断用）
    packetsTotal_.store(0, std::memory_order_relaxed);
    return true;
}

void VideoReceiver::reset_reassembly() {
    // 调用点保证收包线程没在跑（init/start 时），pending_ 无需加锁
    pending_.clear();
    ready_.clear();
    haveHighest_ = false;
    highestFrameId_ = 0;
    haveDelivered_ = false;
    lastDeliveredId_ = 0;
    droppedStreak_.store(0);
    keyframeReq_.store(false);
    probeFrames_.store(0, std::memory_order_relaxed);
    lastPacketUs_.store(0, std::memory_order_relaxed);
    lastVideoUs_.store(0, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lk(ptrMtx_);
        havePtr_ = false;
        ptrDirty_ = false;
        ptrSeq_ = 0;
    }
    {
        std::lock_guard<std::mutex> lk(mtx_);
        queue_.clear();
    }
}

void VideoReceiver::start() {
    if (udpSock_ < 0) {
        MS_WARN("VideoReceiver::start 被调用但 UDP 未初始化");
        return;
    }
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) return;   // 已经在跑
    reset_reassembly();   // 新会话：丢掉上一个会话的残帧与帧号高水位
    try {
        th_ = std::thread(&VideoReceiver::thread_main, this);
    } catch (...) {
        running_.store(false);
        MS_ERR("VideoReceiver 线程创建失败");
    }
}

void VideoReceiver::stop() {
    running_.store(false);
    cv_.notify_all();          // 唤醒可能正在 pop 的线程
    if (!th_.joinable()) return;
    if (th_.get_id() == std::this_thread::get_id()) {
        // 从接收线程内部调 stop()（例如回调里）：join 自己会死锁，只能 detach
        th_.detach();
        return;
    }
    th_.join();
}

void VideoReceiver::shutdown() {
    stop();
    if (udpSock_ >= 0) {
        ::closesocket(SOCKET(udpSock_));
        udpSock_ = -1;
    }
    {
        std::lock_guard<std::mutex> lk(mtx_);
        queue_.clear();
    }
    pending_.clear();
}

void VideoReceiver::set_server(const sockaddr_in& a) {
    std::lock_guard<std::mutex> lk(serverMtx_);
    server_ = a;
    serverKnown_.store(true);
    altStreak_ = 0;
}

// -----------------------------------------------------------------------------
// 收包线程
// -----------------------------------------------------------------------------
void VideoReceiver::thread_main() {
    // MMCSS：收包循环节奏硬实时（UDP 包不等人），注册为 Pro Audio；
    // 视频包若被普通后台任务抢占 5ms，整帧就缺片要等 NACK/FEC。
    const ms::PerfThreadScope perfScope(L"Pro Audio");

    Bytes pkt(65536);

    while (running_.load(std::memory_order_relaxed)) {
        bool gotAny = false;
        for (int budget = 0; budget < kPacketsPerRound; ++budget) {
            sockaddr_in from{};
            const int n = net::udp_recv_from(udpSock_, pkt.data(), pkt.size(), from);
            if (n <= 0) break;      // 非阻塞：没包了 / 出错都直接结束这一轮
            gotAny = true;
            handle_packet(pkt.data(), size_t(n), from);
        }

        // 每轮都清理超时的半成品帧
        sweep_pending(now_us());
        // 缺片的帧发 NACK（重传请求）——FEC 之后的主要恢复手段
        poll_nack(now_us());
        // 按帧号顺序交付（顺带处理"等前序帧超时 → 跳号"）
        deliver_ready(now_us());

        // socket 收空了就让一下 CPU：1ms 对 60fps 足够，也避免空转吃满一个核
        if (!gotAny) ms::sleep_ms(kIdleSleepMs);
    }

    // 退出前把半成品清掉，避免下一轮 start() 拿到上一个会话的残帧
    pending_.clear();
}

void VideoReceiver::handle_packet(const u8* p, size_t n, const sockaddr_in& from) {
    // 任何类型、任何来源的包都计数：防火墙/网络丢包诊断全靠它区分场景
    packetsTotal_.fetch_add(1, std::memory_order_relaxed);

    // --- 来源过滤 -----------------------------------------------------------
    {
        std::lock_guard<std::mutex> lk(serverMtx_);
        if (!serverKnown_.load(std::memory_order_relaxed)) {
            // 第一个包决定服务端地址（首帧到达前 sessionId_ 还是 0，不做校验）
            server_ = from;
            serverKnown_.store(true, std::memory_order_relaxed);
            altStreak_ = 0;
            MS_INFO("UDP 视频来源已锁定: %s:%u", net::addr_str(from).c_str(),
                    unsigned(net::addr_port(from)));
        } else if (!net::addr_same(from, server_)) {
            // NAT 漂移容错：经过 FRP/运营商 NAT 时出口端口可能变化，来源会突然不同。
            // 简易策略：记住这个新来源，只有连续 kDriftSwitchPackets 个包都来自它，
            // 才承认服务端换了地址；在此之前新来源的包一律丢弃（单播视频丢几包没关系，
            // 缺片自然会触发关键帧请求），这样既能自愈又不容易被伪造来源带偏。
            if (altStreak_ == 0 || !net::addr_same(from, altAddr_)) {
                altAddr_ = from;
                altStreak_ = 1;
            } else {
                ++altStreak_;
            }
            if (altStreak_ >= kDriftSwitchPackets) {
                MS_INFO("UDP 视频来源切换: %s:%u", net::addr_str(from).c_str(),
                        unsigned(net::addr_port(from)));
                server_ = from;
                altStreak_ = 0;
            } else {
                return;
            }
        } else {
            altStreak_ = 0;
        }
    }

    // --- 头部解析 -----------------------------------------------------------
    if (n < sizeof(proto::UdpHeader)) return;

    proto::UdpHeader h{};
    if (!proto::parse_udp_header(p, n, h)) {
        std::lock_guard<std::mutex> lk(statMtx_);
        ++statCounters_.parseErrors;
        return;
    }
    if (sessionId_ != 0 && h.sessionId != sessionId_) return;   // 别人的会话

    const size_t avail = n - sizeof(proto::UdpHeader);
    const i64 nowLocal = now_us();
    lastPacketUs_.store(nowLocal, std::memory_order_relaxed);

    // --- 光标位置/可见性（UDP 小包，不走分片重组）---------------------------
    if (h.type == u8(proto::UdpType::Pointer)) {
        if (size_t(h.payloadSize) > avail) return;
        proto::PointerPosMsg pm;
        if (proto::PointerPosMsg::parse(p + sizeof(proto::UdpHeader), h.payloadSize, pm)) {
            std::lock_guard<std::mutex> lk(ptrMtx_);
            // seq 回绕安全的"更新才采用"：UDP 乱序/重复时不会把光标拽回旧位置
            if (!havePtr_ || i32(u32(pm.seq - ptrSeq_)) > 0) {
                ptrPos_  = pm;
                ptrSeq_  = pm.seq;
                havePtr_ = true;
                ptrDirty_ = true;
            }
        }
        return;
    }

    // --- 音频包（20ms Opus，不走分片重组）-----------------------------------
    if (h.type == u8(proto::UdpType::Audio)) {
        if (h.payloadSize == 0 || size_t(h.payloadSize) > avail ||
            size_t(h.payloadSize) > proto::kMaxAudioPayload) {
            return;
        }
        AudioChunk c;
        c.seq = h.id;
        c.ts  = h.sendTsUs;
        c.data.assign(p + sizeof(proto::UdpHeader),
                      p + sizeof(proto::UdpHeader) + h.payloadSize);
        {
            std::lock_guard<std::mutex> lk(audioMtx_);
            if (audioQueue_.size() >= kMaxAudioQueue) audioQueue_.pop_front();
            audioQueue_.push_back(std::move(c));
        }
        return;
    }

    if (h.type != u8(proto::UdpType::Video)) return;   // 本类只管视频/光标/音频
    lastVideoUs_.store(nowLocal, std::memory_order_relaxed);

    if (size_t(h.payloadSize) > avail || size_t(h.payloadSize) > proto::kMaxVideoPayload ||
        h.count == 0 || h.frameBytes == 0 || h.frameBytes > proto::kMaxFrameBytes) {
        std::lock_guard<std::mutex> lk(statMtx_);
        ++statCounters_.parseErrors;
        return;
    }

    {
        std::lock_guard<std::mutex> lk(statMtx_);
        ++statCounters_.packets;
        statCounters_.bytes += u64(n);
    }
    rateBytes_.add(n);

    const u8* payload = p + sizeof(proto::UdpHeader);
    const u32 fid = h.id;

    // --- 重组 ---------------------------------------------------------------
    // 【迟到包处理】帧已经完整上交过了，之后才到的包（典型是最后一批 FEC 校验包，
    // 数据片先齐、校验片后到）必须直接丢掉。以前这里会为它新建一个空的
    // 重组状态，120ms 后超时被记成"丢了一整帧"—— 统计上会凭空多出 30~50% 丢包，
    // 自适应码率/FEC 全被它带偏（实测：0% 真实丢包也报 48% 分片丢失）。
    if (pending_.find(fid) == pending_.end() && haveHighest_ &&
        frame_diff(fid, highestFrameId_) <= 0) {
        {
            std::lock_guard<std::mutex> lk(statMtx_);
            ++statCounters_.latePackets;
        }
        return;
    }
    auto it = assembly_for(fid, h.count, h.frameBytes, i64(h.sendTsUs));
    if (it == pending_.end()) return;
    Assembly& as = it->second;

    // 发送端只把关键帧标志挂在 0 号分片上；UDP 乱序时先到的可能是别的分片，
    // 所以任何一个分片带标志就认定整帧是关键帧（否则会误判成非关键帧而丢帧）
    if ((h.flags & proto::kUdpFlagKeyframe) != 0) as.keyframe = true;
    as.lastArriveUs = nowLocal;

    // --- FEC 校验包：立刻尝试救回本组缺的那一片 -----------------------------
    // 这是「不卡、不花屏」的关键一层：没有它，链路丢 1 个包就等于丢整帧，
    // 而丢整帧之后所有 P 帧都会参考错帧（花屏），只能等 IDR。
    if ((h.flags & proto::kUdpFlagFec) != 0) {
        if (h.extra == 0 || h.extra > proto::kFecMaxGroup || h.payloadSize == 0) return;
        Assembly::Parity par;
        par.parityLen = h.payloadSize;
        par.groupSize = u16(h.extra);      // 发送端组大小 G（末组也用它算起始下标）
        par.data.assign(payload, payload + h.payloadSize);
        // 同一个组的校验重复到达（多发/乱序）只认第一份
        if (!as.parity.emplace(h.index, std::move(par)).second) return;
        {
            std::lock_guard<std::mutex> lk(statMtx_);
            ++statCounters_.fecReceived;
        }
        try_recover_group(it, h.index);
        finalize_if_complete(it);
        return;
    }

    if (h.index >= as.totalCount) return;      // 下标越界：坏包
    if (as.have[h.index]) return;              // 重复分片：忽略（幂等）

    // 帧内偏移由发送端约定：offset = index * kMaxVideoPayload，末片按整帧长度截断
    const size_t off = size_t(h.index) * proto::kMaxVideoPayload;
    if (off >= size_t(as.frameBytes)) return;

    size_t len = size_t(h.payloadSize);
    if (off + len > size_t(as.frameBytes)) len = size_t(as.frameBytes) - off;
    std::memcpy(as.data.data() + off, payload, len);

    as.have[h.index] = true;
    as.gotBytes = u32(as.gotBytes + u32(len));
    ++as.gotCount;
    ++as.rawCount;

    finalize_if_complete(it);
}

std::map<u32, VideoReceiver::Assembly>::iterator
VideoReceiver::assembly_for(u32 frameId, u32 count, u32 frameBytes, i64 sendTsUs) {
    auto it = pending_.find(frameId);
    if (it != pending_.end()) return it;
    // 迟到太多的旧帧没有价值（画面只要最新的），直接不建
    if (haveHighest_ && frame_diff(frameId, highestFrameId_) < -kMaxReorderBacklog) {
        return pending_.end();
    }
    Assembly a;
    a.frameId      = frameId;
    a.keyframe     = false;
    a.frameBytes   = frameBytes;
    a.totalCount   = count;
    a.sendTsUs     = sendTsUs;
    a.firstArriveUs = now_us();
    a.lastArriveUs  = a.firstArriveUs;
    a.data.resize(frameBytes);
    a.have.assign(size_t(count), false);
    return pending_.emplace(frameId, std::move(a)).first;
}

void VideoReceiver::try_recover_group(std::map<u32, Assembly>::iterator it, u16 groupIdx) {
    Assembly& as = it->second;
    auto pit = as.parity.find(groupIdx);
    if (pit == as.parity.end()) return;
    const Assembly::Parity& par = pit->second;

    const u32 start   = u32(groupIdx) * u32(par.groupSize);
    const u32 members = fec::group_members(as.totalCount, start, par.groupSize);
    if (members == 0) return;

    // 这一组缺了哪几片？（XOR 组码只能救 1 片；缺 2 片以上交给「等 IDR」）
    int missingCount = 0;
    u32 missingIdx = 0;
    for (u32 i = 0; i < members; ++i) {
        const u32 idx = start + i;
        if (idx >= as.totalCount) break;
        if (!as.have[idx]) { ++missingCount; missingIdx = idx; }
    }
    if (missingCount != 1) return;

    const size_t outLen = fec::frag_len(as.frameBytes, missingIdx);
    if (outLen == 0 || outLen > par.parityLen) return;

    const u8* parts[proto::kFecMaxGroup] = {};
    size_t    lens[proto::kFecMaxGroup] = {};
    size_t    np = 0;
    for (u32 i = 0; i < members && np < proto::kFecMaxGroup; ++i) {
        const u32 idx = start + i;
        if (idx >= as.totalCount || idx == missingIdx || !as.have[idx]) continue;
        parts[np] = as.data.data() + size_t(idx) * proto::kMaxVideoPayload;
        lens[np]  = fec::frag_len(as.frameBytes, idx);
        ++np;
    }

    const size_t off = size_t(missingIdx) * proto::kMaxVideoPayload;
    if (!fec::xor_recover(as.data.data() + off, outLen, parts, lens, np,
                          par.data.data(), par.parityLen)) {
        return;
    }
    as.have[missingIdx] = true;
    as.gotBytes = u32(as.gotBytes + u32(outLen));
    ++as.gotCount;
    as.lastArriveUs = now_us();
    {
        std::lock_guard<std::mutex> lk(statMtx_);
        ++statCounters_.fecRecovered;
    }
}

void VideoReceiver::finalize_if_complete(std::map<u32, Assembly>::iterator it) {
    Assembly& as = it->second;
    if (as.gotCount != as.totalCount || as.gotBytes != as.frameBytes) return;

    const u32 fid = as.frameId;
    if (!haveHighest_ || frame_diff(fid, highestFrameId_) > 0) {
        highestFrameId_ = fid;
        haveHighest_ = true;
    }
    // 包级统计：整帧需要多少片、实际从网络收到多少片（FEC 恢复的不算）
    const u32 needPk = as.totalCount;
    const u32 recvPk = as.rawCount;
    {
        std::lock_guard<std::mutex> lk(statMtx_);
        statCounters_.dataPktsNeeded += needPk;
        statCounters_.dataPktsRecv += recvPk;
        if (as.nackCount > 0) ++statCounters_.nackRecovered;
    }
    // NACK 重传可能让"老帧"晚于新帧才凑齐。补回来的老帧直接丢掉：
    // 画面只要最新的，而且解码必须严格按帧号递增（乱序喂解码器 = 参考链错乱）。
    if (haveDelivered_ && frame_diff(fid, lastDeliveredId_) <= 0) {
        pending_.erase(it);
        return;
    }

    // 探测窗口：这一帧只用来统计"UDP 到达率"，不送解码
    // （TCP 模式下同一帧已经从控制通道收到了，再解一遍会重复）
    if (probeMode_.load(std::memory_order_relaxed)) {
        pending_.erase(it);
        probeFrames_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    ReceivedFrame f;
    f.frameId   = fid;
    f.keyframe  = as.keyframe;
    f.sendTsUs  = as.sendTsUs;
    f.recvTsUs  = as.lastArriveUs;
    f.data      = std::move(as.data);
    pending_.erase(it);

    // 先放进"待交付"缓冲，由 deliver_ready() 严格按帧号顺序交给解码器。
    // 为什么不能直接入队：NACK 救回来的帧可能比它后面的帧晚 10~20ms 才凑齐，
    // 直接入队就会变成"先交付 N+1、再交付 N"——解码器拿到乱序的参考帧会直接花屏。
    if (haveDelivered_ && frame_diff(fid, lastDeliveredId_) <= 0) return;  // 老帧，丢了
    ready_.emplace(fid, std::move(f));
    deliver_ready(now_us());
}

void VideoReceiver::deliver_ready(i64 nowUs) {
    (void)nowUs;
    for (;;) {
        if (ready_.empty()) return;
        // 期望的下一帧：还没交付过任何帧时，从缓冲里最小的开始
        u32 want = haveDelivered_ ? (lastDeliveredId_ + 1u) : ready_.begin()->first;
        auto it = ready_.find(want);
        if (it == ready_.end()) {
            // 前序帧还没凑齐：给它 kReorderHoldUs 的机会（NACK 一般 1 个 RTT 内到）。
            // 超时后接受"跳号"，让上层进入等 IDR（这是唯一正确的选择：
            // 参考链已断，继续解后续帧就是花屏）。
            const u32 oldest = ready_.begin()->first;
            const i64 waitedUs = nowUs - ready_.begin()->second.recvTsUs;
            if (waitedUs < kReorderHoldUs) return;
            haveDelivered_ = true;
            lastDeliveredId_ = oldest - 1u;
            continue;
        }
        ReceivedFrame f = std::move(it->second);
        ready_.erase(it);
        lastDeliveredId_ = f.frameId;
        haveDelivered_ = true;
        push_frame(std::move(f));
    }
}

void VideoReceiver::poll_nack(i64 nowUs) {
    if (udpSock_ < 0 || pending_.empty()) return;
    if (!serverKnown_.load(std::memory_order_relaxed)) return;

    sockaddr_in target{};
    {
        std::lock_guard<std::mutex> lk(serverMtx_);
        target = server_;
    }
    if (target.sin_family == 0) return;

    for (auto& kv : pending_) {
        Assembly& as = kv.second;
        if (as.gotCount >= as.totalCount) continue;       // 已经齐了
        if (as.nackCount >= 2) continue;                  // 每帧最多求 2 次，防 NACK 风暴
        if (nowUs - as.firstArriveUs < 12000) continue;   // 先给乱序/FEC 12ms
        if (as.lastNackUs != 0 && nowUs - as.lastNackUs < 10000) continue;  // 10ms 节流
        // 重传截止时间（Parsec PR-SCTP 的 FORWARD-TSN 思路：过期数据直接放弃）。
        // 我们的有序交付缓冲只有 25ms —— 40ms 以后救回来的片，交付顺序上已经
        // 被更年轻的帧越过了，就算补齐也没法在不乱序的前提下送解码器，
        // 等于白占重传预算。所以 40ms 后不再发 NACK，交给「等 IDR」路径。
        if (nowUs - as.firstArriveUs > 40000) continue;

        // 收集缺片下标（单包最多 64 个；缺得更多就分多次，反正每帧只求 2 次）
        u16 idx[64];
        size_t n = 0;
        for (u32 i = 0; i < as.totalCount && n < 64; ++i) {
            if (!as.have[i]) idx[n++] = u16(i);
        }
        if (n == 0) continue;

        u8 buf[sizeof(proto::UdpHeader) + 2 + 64 * 2]{};
        proto::UdpHeader h{};
        h.type        = u8(proto::UdpType::Nack);
        h.sessionId   = sessionId_;
        h.id          = as.frameId;      // 哪个帧
        h.extra       = u32(n);          // 请求的重传片数
        h.payloadSize = u16(2 + n * 2);
        std::memcpy(buf, &h, sizeof(h));
        u8* p = buf + sizeof(h);
        p[0] = u8(n & 0xFF);
        p[1] = u8(n >> 8);
        for (size_t i = 0; i < n; ++i) {
            p[2 + i * 2]     = u8(idx[i] & 0xFF);
            p[2 + i * 2 + 1] = u8(idx[i] >> 8);
        }
        if (net::udp_send_to(udpSock_, buf, sizeof(h) + 2 + n * 2, target) >= 0) {
            as.lastNackUs = nowUs;
            ++as.nackCount;
            std::lock_guard<std::mutex> lk(statMtx_);
            ++statCounters_.nackSent;
        }
    }
}

void VideoReceiver::sweep_pending(i64 nowUs) {
    if (pending_.empty()) return;

    // 探测窗口（视频正走 TCP）里，这些未完成帧只是"UDP 到达率测量"的副产品：
    // 丢了也不影响画面。绝不能把它们算进 framesLost / 请求关键帧 ——
    // 否则每 5 秒探测一次就会在 TCP 模式下制造一次"假丢帧 → 等 IDR → 卡一下"。
    if (probeMode_.load(std::memory_order_relaxed)) {
        for (auto it = pending_.begin(); it != pending_.end();) {
            const bool timedOut = (nowUs - it->second.lastArriveUs) > kAssemblyTimeoutUs;
            const bool tooFarBehind = haveHighest_ &&
                                      frame_diff(it->first, highestFrameId_) < -kMaxReorderBacklog;
            if (timedOut || tooFarBehind) it = pending_.erase(it);
            else ++it;
        }
        return;
    }

    u32 lost = 0;
    u32 needPk = 0;
    u32 recvPk = 0;
    for (auto it = pending_.begin(); it != pending_.end();) {
        const bool timedOut = (nowUs - it->second.lastArriveUs) > kAssemblyTimeoutUs;
        // 落后太多的帧：后面更年轻的帧都已经发出去/正在组装了，留着只是占内存
        const bool tooFarBehind = haveHighest_ &&
                                  frame_diff(it->first, highestFrameId_) < -kMaxReorderBacklog;
        if (timedOut || tooFarBehind) {
            if (timedOut) {
                // 只有"等够了时间仍然缺片"才算真正的丢包；被更年轻帧挤掉的
                // （tooFarBehind）是过载的自然后果，计进去会把链路质量算得更差。
                ++lost;
                needPk += it->second.totalCount;
                recvPk += it->second.rawCount;
                // 诊断：前几次超时丢帧把细节打出来（缺了几片、收了几个包、等了多久）
                static int dbgLeft = 6;
                if (dbgLeft > 0) {
                    --dbgLeft;
                    MS_INFO("重组超时丢帧 frame=%u 收到 %u/%u 片（原始 %u），"
                            "等了 %lld ms，FEC 校验包 %zu 个",
                            it->first, it->second.gotCount, it->second.totalCount,
                            it->second.rawCount,
                            (long long)((nowUs - it->second.lastArriveUs) / 1000),
                            it->second.parity.size());
                }
            }
            it = pending_.erase(it);
        } else {
            ++it;
        }
    }
    if (lost == 0 && needPk == 0) return;

    {
        std::lock_guard<std::mutex> lk(statMtx_);
        statCounters_.framesLost += lost;
        statCounters_.dataPktsNeeded += needPk;
        statCounters_.dataPktsRecv += recvPk;
    }
    if (lost == 0) return;
    // 连续丢帧（中间没有完整帧）累积到 2 帧就请求 IDR
    if (droppedStreak_.fetch_add(lost) + lost >= 2) keyframeReq_.store(true);
}

void VideoReceiver::push_frame(ReceivedFrame f) {
    bool overwrote = false;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        // 队列故意做浅：满了丢最旧的，保证 pop 出来的永远是最新画面
        while (queue_.size() >= kMaxQueue) {
            queue_.pop_front();
            overwrote = true;
        }
        queue_.push_back(std::move(f));
    }
    {
        std::lock_guard<std::mutex> lk(statMtx_);
        ++statCounters_.framesOk;
        if (overwrote) ++statCounters_.framesOverwritten;
    }
    droppedStreak_.store(0);       // 收到了完整帧 -> 丢帧连击清零
    rateFrames_.add(1);
    cv_.notify_all();
}

// -----------------------------------------------------------------------------
// 消费侧
// -----------------------------------------------------------------------------
bool VideoReceiver::pop(ReceivedFrame& out, int timeoutMs) {
    std::unique_lock<std::mutex> lk(mtx_);
    if (queue_.empty()) {
        if (timeoutMs > 0) {
            cv_.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                         [this] { return !queue_.empty() || !running_.load(); });
        }
    }
    if (queue_.empty()) return false;
    out = std::move(queue_.front());
    queue_.pop_front();
    return true;
}

// -----------------------------------------------------------------------------
// TCP 视频回退：整帧直接入队（不分片、不重组、不丢片）
// -----------------------------------------------------------------------------
void VideoReceiver::push_tcp_frame(u32 frameId, bool keyframe, i64 sendTsUs,
                                   const Bytes& data) {
    if (!running_.load(std::memory_order_relaxed)) return;
    // 与 UDP 路径共用"只前进不后退"的交付帧号，避免换传输/重传造成乱序交付
    if (haveDelivered_ && frame_diff(frameId, lastDeliveredId_) <= 0) return;
    lastDeliveredId_ = frameId;
    haveDelivered_ = true;
    ReceivedFrame f;
    f.frameId  = frameId;
    f.keyframe = keyframe;
    f.sendTsUs = sendTsUs;
    f.recvTsUs = now_us();
    f.data     = data;
    packetsTotal_.fetch_add(1, std::memory_order_relaxed);   // 统计口径与 UDP 一致
    push_frame(std::move(f));
}

void VideoReceiver::notify_consumed(u32 frameId) {    std::lock_guard<std::mutex> lk(statMtx_);
    lastStatsFrameId_ = frameId;
}

bool VideoReceiver::take_keyframe_request() {
    return keyframeReq_.exchange(false);
}

bool VideoReceiver::take_pointer(proto::PointerPosMsg& out) {
    std::lock_guard<std::mutex> lk(ptrMtx_);
    if (!ptrDirty_) return false;
    ptrDirty_ = false;
    out = ptrPos_;
    return true;
}

size_t VideoReceiver::queue_depth() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return queue_.size();
}

VideoReceiver::Stats VideoReceiver::stats() const {
    Stats s;
    {
        std::lock_guard<std::mutex> lk(statMtx_);
        s = statCounters_;
    }
    // 速率类字段实时算，不缓存
    s.fps  = rateFrames_.rate(1.0);
    s.kbps = rateBytes_.kbps(1.0);
    const u64 total = s.framesOk + s.framesLost;
    s.lossPercent = total ? (double(s.framesLost) * 100.0 / double(total)) : 0.0;
    s.packetsTotal = packetsTotal_.load(std::memory_order_relaxed);
    return s;
}

// -----------------------------------------------------------------------------
// 音频：取一个待播放的包
// -----------------------------------------------------------------------------
bool VideoReceiver::take_audio(AudioChunk& out) {
    std::lock_guard<std::mutex> lk(audioMtx_);
    if (audioQueue_.empty()) return false;
    out = std::move(audioQueue_.front());
    audioQueue_.pop_front();
    return true;
}
} // namespace ms::client
