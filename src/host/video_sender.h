// =============================================================================
//  video_sender.h - UDP 视频分片发送
//
//  一帧编码数据 -> N 个 <= kMaxVideoPayload 的 UDP 包。
//  发送端不做重传：低延迟场景下「丢一帧等下一个 IDR」比「等重传」更快。
//  但要做过载保护：若发送速率跟不上，宁可丢帧也不要排队，否则延迟会累积。
//
//  【为什么要额外发 XOR 校验包（FEC）】
//   1080p60 一帧约 30~40 个分片，链路只要丢 2% 的包，整帧到达率就只有
//   0.98^35 ≈ 49% —— 表现是「帧率腰斩 + 不时卡住」。而丢一片就整帧丢弃、
//   再等一个 IDR，代价太大；重传又要多花一个 RTT，串流场景已经过期。
//   所以在每组 G 个数据分片之后**立刻**补发 1 个 XOR 校验包：组内任意丢 1 片
//   都能被收端无损恢复，代价只有 1/G 的带宽、零额外延迟（校验包跟着数据片
//   同趟发出，不等任何回复）。组大小 G 由远控端反馈的丢帧率自适应调整，
//   见 host_service.cpp 的 on_video_feedback()。
// =============================================================================
#pragma once

#include "../common/types.h"
#include "../common/protocol.h"
#include "../common/net.h"
#include "../common/stats.h"
#include <atomic>
#include <deque>

namespace ms::host {

class VideoSender {
public:
    VideoSender() = default;

    bool init(int udpSock, u32 sessionId, std::string& err);
    void set_peer(const sockaddr_in& peer);
    void set_session(u32 sessionId) { sessionId_ = sessionId; }
    bool has_peer() const { return hasPeer_; }

    // 发送一整帧。
    //  返回 >=0：实际发出的 UDP 包数（含 FEC 校验包）
    //  返回  -1：因为过载主动丢弃该帧（调用方应尽快请求关键帧或降码率）
    int send_frame(u32 frameId, const u8* data, size_t len, bool keyframe, i64 captureTsUs);

    // ---- FEC 组大小 ---------------------------------------------------------
    // 0 = 关闭（不发校验包）；其它值 clamp 到 [kFecMinGroup, kFecMaxGroup]。
    // 实现只写一个 atomic，所以 Feedback 处理线程（accept / input 线程）可以
    // 直接调用，不需要和 stream 线程握手；下一帧发送时自然生效。
    void set_fec_group(u16 g);    u16  fec_group() const { return fecGroup_.load(std::memory_order_relaxed); }

    // ---- 发送整形（pacing）--------------------------------------------------
    // 为什么要整形：一帧数据（尤其关键帧）如果一次性怼进网卡，会在 Wi-Fi/FRP/中继
    // 上形成微突发（micro-burst），排队丢包就是从这里来的 —— 表现为"平时没事，
    // 一转身/一开镜就卡"。目标码率越高、突发越狠。
    // 这里用令牌桶把每个 UDP 包的发送摊到时间轴上：桶里最多允许 ~4 个包即时发出，
    // 其余按 kbps 匀速，单包最多等 3ms（绝不为了整形牺牲帧率）。
    void set_pace_bitrate(int kbps);     // 目标码率（含 FEC 开销的余量由内部放大）
    u64  pace_stalls() const { return paceStalls_.load(std::memory_order_relaxed); }

    // ---- 拥塞窗口（cwnd，Parsec/SCTP 同款思路）------------------------------
    // Parsec 的传输是 SCTP-over-DTLS：拥塞控制是「在途数据量 ≤ cwnd」
    // （cg event: cwindow/dz/rtt），而不是只看码率。我们这里没有逐包 ACK，
    // 用「过去一个 RTT 窗口里发出的字节数 ≤ cwnd」近似：
    //   cwnd = 码率 × RTT × 1.5（带宽延迟积 + 余量），RTT 来自远控端反馈。
    // 超出时直接丢整帧（不排队）——拥塞时主动少发比堆缓冲区便宜得多。
    void update_cwnd(int kbps, int rttMs);
    u64  cwnd_drops() const { return cwndDrops_; }

    // TCP 视频回退用：只记账不发送（实际发送由控制通道完成）
    void note_tcp_frame(size_t len);

    // 发送一个音频包（UDP）：UdpHeader.id=seq、sendTsUs=48kHz 采样时间戳，
    // 净荷是裸 Opus 数据。copies 默认 2 份 —— 20ms 的包丢了就是明显的"咔"一下，
    // 重复发一份的代价只有 ~100kbps，比任何重传/纠删码都便宜。
    bool send_audio(u32 seq, u32 ts, const u8* data, size_t len, int copies = 2);

    // 发送一个「光标位置/可见性」UDP 小包（几十字节）。
    // 它同时是**下行 UDP 探针**：远控端靠它持续确认被控端→自己这条路径还活着。
    bool send_pointer(const proto::PointerPosMsg& pos);

    // NACK：远控端报告某帧缺了哪些分片，这里从最近的历史里把这几片重发一遍。
    //  返回实际重传的包数；帧已经不在历史窗口里就返回 0（远控端会改求 IDR）。
    // 线程安全：由串流线程调用（历史缓冲加锁），与 send_frame 串行。
    int resend(u32 frameId, const u16* indexes, size_t count);

    // 历史窗口参数（依据 UU远程/GameViewer 逆向报告：nack.rtp_history_ms=1000ms）
    static constexpr i64 kHistoryUs = 1000 * 1000;   // 保留最近 1 秒的帧
    static constexpr size_t kMaxHistoryFrames = 32;  // 最多 32 帧
    static constexpr size_t kMaxHistoryBytes = 24u * 1024u * 1024u;

    // 统计
    double sent_kbps() const { return bytes_.kbps(); }
    u64    frames_sent() const { return framesSent_; }
    u64    frames_dropped() const { return framesDropped_; }
    u64    fec_packets_sent() const { return fecPacketsSent_.load(std::memory_order_relaxed); }
    u64    retransmitted_packets() const { return retxPkts_.load(std::memory_order_relaxed); }
    double send_fps() const { return bytes_.rate(); }
    void   reset_stats() {
        bytes_.reset();
        framesSent_ = 0;
        framesDropped_ = 0;
        fecPacketsSent_ = 0;
    }

private:
    // 一帧的发送历史（给 NACK 重传用）。只保留最近 1 秒、最多 32 帧 / 24MB。
    struct HistFrame {
        u32   id = 0;
        i64   atUs = 0;
        bool  keyframe = false;
        Bytes data;
    };
    std::deque<HistFrame> history_;
    void remember_frame(u32 frameId, const u8* data, size_t len, bool keyframe, i64 nowUs);

    int  udpSock_ = -1;
    sockaddr_in peer_{};
    bool hasPeer_ = false;
    u32  sessionId_ = 0;
    std::atomic<u64> framesSent_{0};
    std::atomic<u64> framesDropped_{0};
    std::atomic<u64> fecPacketsSent_{0};
    std::atomic<u64> retxPkts_{0};          // NACK 重传的包数（统计用）
    // 默认就按协议默认组大小发校验（10% 开销）：这样第一帧起就具备抗丢包
    // 能力，而不是等远控端第一轮反馈（1~2 秒）之后才开始保护。
    std::atomic<u16> fecGroup_{proto::kFecDefaultGroup};
    // 发送整形状态（只在 stream 线程使用）
    std::atomic<int> paceKbps_{0};        // 0 = 不整形
    i64 paceLastUs_ = 0;
    i64 paceBudget_ = 0;                  // 令牌桶剩余字节数
    std::atomic<u64> paceStalls_{0};      // 实际限速等待的次数（诊断用）
    void pace(size_t bytes);
    // cwnd 状态：windowUs_ 窗口内已发 windowBytes_ 字节；超出 cwndBytes_ 就丢帧
    i64  cwndBytes_ = 0;               // 0 = 不启用
    i64  windowStartUs_ = 0;
    i64  windowUs_ = 0;
    i64  windowBytes_ = 0;
    u64  cwndDrops_ = 0;
    bool cwnd_full(i64 nowUs, size_t frameBytes);
    mutable RateCounter bytes_;
    mutable RateCounter retxBytes_;    // 重传流量（限速用，≤ 总发送量 15%）

    // 自测用：模拟 UDP 丢包（环境变量 MINISTREAM_TEST_LOSS_PCT，0~100）。
    // 在回环上验证 FEC 恢复率与「缺帧等 IDR」是否真的工作；正常使用恒为 0、零开销。
    int testLossPct_ = 0;
    u32 lossRng_ = 12345u;
};

} // namespace ms::host
