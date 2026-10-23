// =============================================================================
//  video_sender.cpp - UDP 视频分片发送
//
//  发送策略（低延迟优先）：
//   * 一帧切成 <= kMaxVideoPayload 的分片，头部 28 字节固定。
//   * 每 G 个连续分片组成一组，组内数据片发完**立刻**补 1 个 XOR 校验包，
//     组内任意丢 1 片可被收端无损恢复（帧不再因为缺一片就整帧报废）。
//     校验算法全部来自 ../common/fec.h，两端共用同一份公式，避免各写一套漂移。
//   * socket 设为非阻塞：一旦内核发送缓冲满，立刻放弃该帧剩余分片并计一次丢帧，
//     绝不阻塞采集线程。丢帧由远控端通过 TCP 请求关键帧来恢复。
// =============================================================================
#include "video_sender.h"
#include "../common/fec.h"
#include "../common/clock.h"
#include "../common/log.h"

#include <cstdlib>
#include <cstring>

namespace ms::host {

bool VideoSender::init(int udpSock, u32 sessionId, std::string& err) {
    if (udpSock < 0) { err = "无效的 UDP 套接字"; return false; }
    udpSock_ = udpSock;
    sessionId_ = sessionId;
    // 发不出去就丢帧，而不是把采集线程堵死
    if (!net::udp_set_nonblocking(udpSock_, true)) {
        err = "无法把 UDP 套接字设为非阻塞";
        return false;
    }
    // 自测钩子：模拟链路丢包（0~100%），用于在回环上验证 FEC 与等 IDR 逻辑
    {
        wchar_t v[16]{};
        if (::GetEnvironmentVariableW(L"MINISTREAM_TEST_LOSS_PCT", v, 16) > 0) {
            int pct = _wtoi(v);
            if (pct < 0) pct = 0;
            if (pct > 100) pct = 100;
            testLossPct_ = pct;
            if (pct > 0) {
                MS_WARN("[自测] 模拟 UDP 丢包 %d%%（仅用于验证 FEC/等 IDR，请勿用于正常使用）",
                        pct);
            }
        }
    }
    return true;
}

void VideoSender::set_peer(const sockaddr_in& peer) {
    peer_ = peer;
    hasPeer_ = true;
}

// FEC 组大小由远控端的丢帧率反馈来调（见 host_service.cpp）。
// 0 = 关闭；非 0 时钳到协议允许区间，避免调用方传 1 造成 100% 开销、
// 或传 200 导致组大到连续丢 2 个包就永远恢复不了。
void VideoSender::set_fec_group(u16 g) {
    u16 v = g;
    if (v != proto::kFecOffGroup) {
        if (v < proto::kFecMinGroup) v = proto::kFecMinGroup;
        if (v > proto::kFecMaxGroup) v = proto::kFecMaxGroup;
    }
    fecGroup_.store(v, std::memory_order_relaxed);
}

void VideoSender::set_pace_bitrate(int kbps) {
    if (kbps < 0) kbps = 0;
    paceKbps_.store(kbps, std::memory_order_relaxed);
}

// 令牌桶整形。默认不启用（paceKbps_=0 直接返回），由 MINISTREAM_PACE_KBPS 打开。
//
// 参数口径（第一版把系数写错，实测把回环端到端从 14ms 拉到 1200ms，这里记录清楚）：
//   bytes/s = kbps * 1000 / 8 = kbps * 125
//   桶容量 = 50ms 的预算（8Mbps -> 50KB）：一帧普通 16KB、关键帧几十 KB 都能
//   即时发出，只有连续超量时才按速率摊开。桶太小（比如只给 4 个包）会让每帧
//   都被强行摊到 30ms 以上，帧率直接崩掉。
void VideoSender::pace(size_t bytes) {
    const int kbps = paceKbps_.load(std::memory_order_relaxed);
    if (kbps <= 0) { paceLastUs_ = 0; paceBudget_ = 0; return; }

    const i64 kBucketBytes = i64(kbps) * 125 / 1000 * 50;   // 50ms 预算
    constexpr i64 kMaxStallUs = 2000;                        // 单包最长等待 2ms

    const i64 now = ms::now_us();
    if (paceLastUs_ == 0) {
        paceLastUs_ = now;
        paceBudget_ = kBucketBytes;
    }
    const i64 dt = now - paceLastUs_;
    if (dt > 0) {
        paceBudget_ += dt * i64(kbps) * 125 / 1000000;   // 按 bytes/s 补充令牌
        paceLastUs_ = now;
        if (paceBudget_ > kBucketBytes) paceBudget_ = kBucketBytes;
    }

    paceBudget_ -= i64(bytes);
    if (paceBudget_ >= 0) return;

    const i64 needBytes = -paceBudget_;
    i64 waitUs = needBytes * 8 * 1000000 / (i64(kbps) * 1000);   // 需要的时间
    if (waitUs > kMaxStallUs) waitUs = kMaxStallUs;
    if (waitUs > 0) {
        ::Sleep(DWORD((waitUs + 999) / 1000));
        paceStalls_.fetch_add(1, std::memory_order_relaxed);
    }
    paceBudget_ = 0;
    paceLastUs_ = ms::now_us();
}

void VideoSender::update_cwnd(int kbps, int rttMs) {
    if (kbps <= 0 || rttMs <= 0 || rttMs > 2000) { cwndBytes_ = 0; return; }
    // 带宽延迟积 ×1.5 余量；下限 64KB（约 5 帧 1080p 的突发），上限 2MB
    i64 cwnd = i64(kbps) * 125 * rttMs * 3 / 2000;
    if (cwnd < 64 * 1024) cwnd = 64 * 1024;
    if (cwnd > 2 * 1024 * 1024) cwnd = 2 * 1024 * 1024;
    cwndBytes_ = cwnd;
    windowUs_  = i64(rttMs) * 1000;
}

// 拥塞窗口：过去一个 RTT 窗口里发出的字节超过 cwnd 就直接丢整帧（不排队）。
// Parsec/SCTP 的做法是阻塞在发送端，但我们的模型是「宁可丢帧也不排队」，
// 所以超窗就丢 —— 这等价于 Parsec 里 cwnd 满时 SCTP 把 PR 数据标记放弃。
bool VideoSender::cwnd_full(i64 nowUs, size_t frameBytes) {
    if (cwndBytes_ <= 0) return false;                       // 未启用
    if (windowStartUs_ == 0 || nowUs - windowStartUs_ >= windowUs_) {
        windowStartUs_ = nowUs;                              // 新窗口
        windowBytes_   = 0;
    }
    if (windowBytes_ + i64(frameBytes) > cwndBytes_) {
        ++cwndDrops_;
        return true;                                         // 窗口满：丢这一帧
    }
    windowBytes_ += i64(frameBytes);
    return false;
}

int VideoSender::send_frame(u32 frameId, const u8* data, size_t len,
                            bool keyframe, i64 captureTsUs) {
    if (!hasPeer_ || udpSock_ < 0) return -1;
    if (!data || len == 0 || len > proto::kMaxFrameBytes) {
        framesDropped_.fetch_add(1, std::memory_order_relaxed);        return -1;
    }

    // cwnd：过去一个 RTT 窗口已经发满 -> 这一帧整帧放弃（关键帧除外，IDR 是唯一
    // 的恢复手段，丢了远端会黑屏等下一个）。丢非关键帧会让对端"跳帧等 IDR"，
    // 代价小于把延迟堆上去。
    if (!keyframe && cwnd_full(now_us(), len)) {
        framesDropped_.fetch_add(1, std::memory_order_relaxed);
        if ((cwndDrops_ % 60) == 1) {
            MS_DEBUG("cwnd 满 -> 丢整帧（累计 %llu 次）",
                     (unsigned long long)cwndDrops_);
        }
        return -1;
    }

    const size_t payloadMax = proto::kMaxVideoPayload;
    const u16 total = u16((len + payloadMax - 1) / payloadMax);
    if (total == 0) return -1;

    // 复用缓冲，避免每包一次分配。
    //   buf    : 包头 + 净荷（数据片和校验包共用，sendto 会立刻拷进内核）
    //   parity : 校验累加器，最长也就是一片
    static thread_local Bytes buf;
    static thread_local Bytes parity;
    buf.resize(sizeof(proto::UdpHeader) + payloadMax);
    parity.resize(payloadMax);

    const u32 ts32 = u32(u64(captureTsUs) & 0xFFFFFFFFu);

    // FEC 开关：组大小取当前配置（0=关闭）。单分片帧没有可保护的对象
    // （丢它就是丢整帧，校验也补不出来），所以直接不发校验包。
    const u16 fecG = fec_group();
    const bool fecOn = (fecG != 0) && (total > 1);
    const u16 groupSize = fecOn ? fecG : u16(0);

    // 一个包的发送动作：填头 + 拷净荷 + sendto。
    // 失败一律按「丢整帧」处理（保持原有语义）：非阻塞套接字一旦缓冲满，
    // 继续硬塞只会把延迟堆起来，不如尽快让远控端请求 IDR 重来。
    auto send_one = [&](const proto::UdpHeader& h, const u8* pl, size_t plLen) -> bool {
        // 自测钩子：按百分比模拟链路丢包（数据片与校验包一视同仁）。
        // 返回 true = "当作发出去了"，于是帧继续发，效果等价于网络上丢了这些包。
        if (testLossPct_ > 0) {
            lossRng_ = lossRng_ * 1664525u + 1013904223u;
            if (((lossRng_ >> 16) % 100u) < u32(testLossPct_)) return true;
        }
        std::memcpy(buf.data(), &h, sizeof(h));
        if (plLen) std::memcpy(buf.data() + sizeof(h), pl, plLen);
        const int sent = net::udp_send_to(udpSock_, buf.data(), sizeof(h) + plLen, peer_);
        if (sent < 0) {
            const int e = ::WSAGetLastError();
            if (e == WSAEWOULDBLOCK) {
                // 内核缓冲满：直接放弃整帧，避免延迟累积
                framesDropped_.fetch_add(1, std::memory_order_relaxed);
                if ((framesDropped_.load() % 30) == 1) {
                    MS_WARN("UDP 发送缓冲已满，丢弃第 %u 帧（已丢 %llu 帧）",
                            frameId, (unsigned long long)framesDropped_.load());
                }
            } else {
                MS_WARN("UDP sendto 失败, 错误码 %d", e);
                framesDropped_.fetch_add(1, std::memory_order_relaxed);
            }
            return false;
        }
        bytes_.add(size_t(sent));
        // 发送整形（pacing）：默认关闭，只有 MINISTREAM_PACE_KBPS 显式设置才启用。
        // 先前的默认开启版本在回环上把端到端延迟从 14ms 拉到 1200ms（整形太激进），
        // 需要先在真机/网络上验证参数，暂不默认开启。
        pace(size_t(sent));
        return true;
    };

    u32 sentPkts = 0;          // 返回给调用方的「实际发出包数」（含校验包）
    u32 groupIdx = 0;          // 当前组下标 = 首片全局下标 / G
    u32 groupStart = 0;        // 当前组首片的全局下标
    size_t parityLen = 0;      // 当前组的校验净荷长度 = 组内最长分片

    for (u16 i = 0; i < total; ++i) {
        const size_t off = size_t(i) * payloadMax;
        // 分片长度用 fec::frag_len（与收端、与校验计算完全同一条公式）。
        // 换算结果和原来的 min(payloadMax, len-off) 完全一致。
        const size_t n = fec::frag_len(len, u32(i));

        proto::UdpHeader h{};
        h.type        = u8(proto::UdpType::Video);
        h.flags       = (keyframe && i == 0) ? proto::kUdpFlagKeyframe : u8(0);
        h.payloadSize = u16(n);
        h.sessionId   = sessionId_;
        h.id          = frameId;
        h.index       = i;
        h.count       = total;
        h.sendTsUs    = ts32;
        h.frameBytes  = u32(len);
        h.extra       = 0;

        if (!send_one(h, data + off, n)) return -1;
        ++sentPkts;

        // ---- 关键帧整体发两遍 ----
        // 关键帧比 P 帧大一个数量级（几百个分片），而它恰恰是丢包后唯一的
        // 恢复手段：只要少一片，整帧就废，远控端会一直"等 IDR"，画面长时间冻结。
        // 把关键帧的每个分片发两份（两份独立丢包），单帧存活率从 ~70% 提到 ~99%
        // 以上；代价只是一次 IDR 的带宽翻倍，平摊到 2 秒 GOP 上约 1% 码率，
        // 换来的是"丢包时能真的恢复"。P 帧不复制（丢了下一帧就补上了）。
        if (keyframe) {
            if (!send_one(h, data + off, n)) return -1;
            ++sentPkts;
        }

        if (!fecOn) continue;

        // ---- 维护本组的校验累加器 ----
        const u32 gi = u32(i) % u32(groupSize);
        if (gi == 0) {
            // 新的一组开始：先按组内实际成员数算出校验净荷长度，再清零累加器。
            // 成员数用 fec::group_members（末组不足 G 片时按实际数量算），
            // 长度用 fec::parity_len（不依赖"分片长度随下标单调不增"这个隐含前提，
            // 以后协议改分片规则也不会悄悄算错）。
            groupIdx   = u32(i) / u32(groupSize);
            groupStart = u32(i);
            const u32 members = fec::group_members(u32(total), groupStart, groupSize);
            parityLen = fec::parity_len(len, groupStart, members);
            if (parityLen > payloadMax) parityLen = payloadMax;   // 理论上不会发生
            std::memset(parity.data(), 0, parityLen);
        }

        // 本片补零到 parityLen 后逐字节 XOR 进累加器（零填充由 memset 造出，
        // 短分片不参与的高位保持为 0，等价于收端按同样规则补零）。
        fec::xor_into(parity.data(), parityLen, data + off, n);

        // 本组发完了（满 G 片，或已经到帧尾）：立刻发校验包，不攒到帧尾。
        // 攒到帧尾会让最后一组之前的校验依赖更多后续包先到达，恢复时延变长；
        // 而且一旦后续发不出去（过载丢帧），前面这些校验也一起作废。
        const bool groupDone = (gi == u32(groupSize) - 1) || (u32(i) + 1 == u32(total));
        if (!groupDone) continue;

        proto::UdpHeader ph{};
        ph.type        = u8(proto::UdpType::Video);
        ph.flags       = proto::kUdpFlagFec;      // 只带 FEC 位，不带关键帧位
        ph.payloadSize = u16(parityLen);
        ph.sessionId   = sessionId_;
        ph.id          = frameId;
        ph.index       = u16(groupIdx);           // 组下标；收端用 groupStart = index*G 还原
        ph.count       = total;                   // 与数据片一致的整帧分片总数
        ph.sendTsUs    = ts32;                    // 与数据片同一采集时刻（延迟统计要对齐）
        ph.frameBytes  = u32(len);
        // 【协议约定，别改成"本组成员数"】extra 存的是**组大小 G**：
        // 末组不足 G 片时，收端只能靠 min(G, count - index*G) 推出成员数；
        // 若这里存实际成员数，末组就永远定位不到自己的起始分片下标。
        ph.extra       = u32(groupSize);

        if (!send_one(ph, parity.data(), parityLen)) return -1;
        ++sentPkts;
        fecPacketsSent_.fetch_add(1, std::memory_order_relaxed);
    }

    framesSent_.fetch_add(1, std::memory_order_relaxed);
    // 存一份发送历史，供 NACK 重传（只在串流线程访问，不需要锁）
    remember_frame(frameId, data, len, keyframe, now_us());
    return int(sentPkts);
}

// -----------------------------------------------------------------------------
// NACK 重传
// -----------------------------------------------------------------------------
void VideoSender::remember_frame(u32 frameId, const u8* data, size_t len,
                                 bool keyframe, i64 nowUs) {
    if (data == nullptr || len == 0) return;
    HistFrame f;
    f.id       = frameId;
    f.atUs     = nowUs;
    f.keyframe = keyframe;
    f.data.assign(data, data + len);
    history_.push_back(std::move(f));

    // 淘汰窗口：最近 1 秒、最多 32 帧、最多 24MB（够覆盖 RTT 尖峰 + 乱序）
    while (!history_.empty() &&
           ((nowUs - history_.front().atUs > kHistoryUs) ||
            history_.size() > kMaxHistoryFrames)) {
        history_.pop_front();
    }
    size_t total = 0;
    for (const auto& h : history_) total += h.data.size();
    while (total > kMaxHistoryBytes && history_.size() > 1) {
        total -= history_.front().data.size();
        history_.pop_front();
    }
}

int VideoSender::resend(u32 frameId, const u16* indexes, size_t count) {
    if (!hasPeer_ || udpSock_ < 0 || indexes == nullptr || count == 0) return 0;

    // 重传限速：重传流量不超过总发送量的 15%（UU远程/GameViewer 的
    // RetransmissionRateLimit 同款）。否则丢包一多，重传会把上行挤爆，
    // 反而让新的媒体包丢得更多（越补越烂）。
    {
        const double mediaKbps = bytes_.kbps();
        const double retxKbps  = retxBytes_.kbps();
        if (mediaKbps > 0 && retxKbps > mediaKbps * 0.15 && retxKbps > 200.0) {
            return 0;
        }
    }

    const HistFrame* f = nullptr;
    for (const auto& h : history_) {
        if (h.id == frameId) { f = &h; break; }
    }
    if (f == nullptr || f->data.empty()) return 0;

    const size_t total = (f->data.size() + proto::kMaxVideoPayload - 1) / proto::kMaxVideoPayload;
    if (total == 0 || total > 0xFFFFu) return 0;

    // 单次最多补 32 片（远控端每帧最多发 2 次 NACK，这里再兜一层，防 NACK 风暴）
    if (count > 32) count = 32;

    static thread_local Bytes buf;
    buf.resize(sizeof(proto::UdpHeader) + proto::kMaxVideoPayload);
    const u32 ts32 = u32(u64(f->atUs) & 0xFFFFFFFFu);
    int sentCount = 0;

    for (size_t k = 0; k < count; ++k) {
        const u32 idx = u32(indexes[k]);
        if (idx >= total) continue;
        const size_t off = size_t(idx) * proto::kMaxVideoPayload;
        size_t n = f->data.size() - off;
        if (n > proto::kMaxVideoPayload) n = proto::kMaxVideoPayload;

        proto::UdpHeader h{};
        h.type        = u8(proto::UdpType::Video);
        // 重传标记：远控端据此不计入"媒体码率"采样（否则重传会污染带宽估计）
        h.flags       = proto::kUdpFlagRetransmit;
        if (f->keyframe && idx == 0) h.flags |= proto::kUdpFlagKeyframe;
        h.payloadSize = u16(n);
        h.sessionId   = sessionId_;
        h.id          = frameId;
        h.index       = u16(idx);
        h.count       = u16(total);
        h.sendTsUs    = ts32;
        h.frameBytes  = u32(f->data.size());
        h.extra       = 0;

        std::memcpy(buf.data(), &h, sizeof(h));
        std::memcpy(buf.data() + sizeof(h), f->data.data() + off, n);
        if (net::udp_send_to(udpSock_, buf.data(), sizeof(h) + n, peer_) < 0) break;
        bytes_.add(sizeof(h) + n);
        retxBytes_.add(sizeof(h) + n);
        ++sentCount;
    }
    if (sentCount > 0) retxPkts_.fetch_add(u64(sentCount), std::memory_order_relaxed);
    return sentCount;
}

// TCP 视频回退：帧已经通过控制通道发走了，这里只更新统计
void VideoSender::note_tcp_frame(size_t len) {
    bytes_.add(len);
    framesSent_.fetch_add(1, std::memory_order_relaxed);
}

// 光标位置包：UDP 小包，60Hz 也就几 kbps，但换来"光标跟手"。
bool VideoSender::send_pointer(const proto::PointerPosMsg& pos) {
    if (!hasPeer_ || udpSock_ < 0) return false;
    const Bytes pl = pos.serialize();
    u8 buf[sizeof(proto::UdpHeader) + 16]{};
    static_assert(sizeof(proto::UdpHeader) + 16 >= sizeof(proto::UdpHeader) + 10,
                  "PointerPosMsg 净荷不超过 16 字节");
    if (pl.size() > 16) return false;
    proto::UdpHeader h{};
    h.type        = u8(proto::UdpType::Pointer);
    h.payloadSize = u16(pl.size());
    h.sessionId   = sessionId_;
    h.id          = pos.seq;
    std::memcpy(buf, &h, sizeof(h));
    std::memcpy(buf + sizeof(h), pl.data(), pl.size());
    return net::udp_send_to(udpSock_, buf, sizeof(h) + pl.size(), peer_) >= 0;
}

// 音频包：UDP 小包 + 裸 Opus 数据。id=seq、sendTsUs=采样时间戳（48kHz 采样计数）。
// 发两份抗丢包（远控端按 seq 去重，第二份只是"保险"）。
bool VideoSender::send_audio(u32 seq, u32 ts, const u8* data, size_t len, int copies) {
    if (!hasPeer_ || udpSock_ < 0 || data == nullptr || len == 0) return false;
    if (len > proto::kMaxAudioPayload) return false;
    if (copies < 1) copies = 1;
    if (copies > 3) copies = 3;

    u8 buf[sizeof(proto::UdpHeader) + proto::kMaxAudioPayload]{};
    proto::UdpHeader h{};
    h.type        = u8(proto::UdpType::Audio);
    h.payloadSize = u16(len);
    h.sessionId   = sessionId_;
    h.id          = seq;
    h.sendTsUs    = ts;
    h.count       = 1;
    std::memcpy(buf, &h, sizeof(h));
    std::memcpy(buf + sizeof(h), data, len);

    bool any = false;
    for (int i = 0; i < copies; ++i) {
        if (net::udp_send_to(udpSock_, buf, sizeof(h) + len, peer_) >= 0) {
            any = true;
            bytes_.add(len);
        }
    }
    return any;
}
} // namespace ms::host
