// =============================================================================
//  video_receiver.h - UDP 接收 + 分片重组
//
//  跑在独立线程：收包 -> 按 frameId 重组 -> 完整帧入队。
//  队列故意做得很浅（默认 2 帧）：串流要的是「最新画面」而不是「完整画面」，
//  积压只会让延迟越滚越大。
// =============================================================================
#pragma once

#include "../common/types.h"
#include "../common/protocol.h"
#include "../common/net.h"
#include "../common/stats.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <thread>

namespace ms::client {

// 一个收到的音频包（20ms Opus）。UDP 收包线程入队，主线程取走解码播放。
using AudioChunk = proto::AudioPacket;

struct ReceivedFrame {
    u32   frameId = 0;
    bool  keyframe = false;
    i64   sendTsUs = 0;    // 被控端时间轴（发送时刻低 32 位已还原）
    i64   recvTsUs = 0;    // 本地时间轴（最后一片到达）
    Bytes data;
};

class VideoReceiver {
public:
    VideoReceiver() = default;
    ~VideoReceiver();

    // 打开 UDP 监听端口（与被控端 TCP 端口号相同）
    bool init(u16 listenPort, std::string& err);
    void shutdown();

    void start();
    void stop();

    void set_session(u32 id) { sessionId_ = id; }
    // 学习被控端 UDP 来源；之后忽略其它来源的发包
    void set_server(const sockaddr_in& a);

    // 阻塞取一帧完整帧。返回 false 表示超时或已停止。
    bool pop(ReceivedFrame& out, int timeoutMs);

    // TCP 视频回退：控制通道收到一整帧后直接塞进播放队列
    // （不走 UDP 分片重组，所以不存在丢片/乱序）
    void push_tcp_frame(u32 frameId, bool keyframe, i64 sendTsUs, const Bytes& data);

    // 帧被消费后调用，用于统计「已处理」
    void notify_consumed(u32 frameId);

    // 是否建议请求关键帧（连续丢帧时置位，读取后自动清除）
    bool take_keyframe_request();

    // ---- 光标（UDP 小包，低延迟优先）----
    // 有新的位置/可见性时返回 true（按 seq 过滤乱序/重复包）
    bool take_pointer(proto::PointerPosMsg& out);

    // ---- 音频（UDP）--------------------------------------------------------
    // 取一个待播放的音频包（移动语义，避免每包多一次拷贝）。空了返回 false。
    bool take_audio(AudioChunk& out);

    // 最近一次收到「任意 UDP 包」/「UDP 视频包」的本地时刻（微秒）。
    // 传输自愈只看视频包：光标小包能过来不代表 20Mbps 的视频也能过来。
    i64 last_packet_us() const { return lastPacketUs_.load(std::memory_order_relaxed); }
    i64 last_video_packet_us() const { return lastVideoUs_.load(std::memory_order_relaxed); }

    // ---- UDP 探测（TCP 模式下判断"能不能切回 UDP"）----
    // 打开后完整 UDP 帧只计数、不入队（避免和 TCP 传的同一帧重复解码）
    void set_probe_mode(bool on) { probeMode_.store(on, std::memory_order_relaxed); }
    u32  take_probe_frames() { return probeFrames_.exchange(0, std::memory_order_relaxed); }

    struct Stats {
        u64    packets = 0;
        u64    bytes = 0;
        u64    framesOk = 0;
        u64    framesLost = 0;      // 缺片被丢弃
        u64    framesOverwritten = 0; // 队列满被顶掉
        u64    parseErrors = 0;
        u64    fecReceived = 0;     // 收到的 FEC 校验包
        u64    fecRecovered = 0;    // 靠 FEC 救回来的数据分片
        // 包级统计（诊断用）：成帧/被丢弃的帧一共需要多少分片、实际收到多少。
        // 用它算出来的「分片丢失率」比帧级丢包率更能反映链路质量。
        u64    dataPktsNeeded = 0;
        u64    dataPktsRecv = 0;
        u64    latePackets = 0;     // 所属帧已经完成/丢弃之后才到的包（诊断用）
        u64    nackSent = 0;        // 发出的 NACK 请求数
        u64    nackRecovered = 0;   // 靠重传救回来的帧数（发过 NACK 且最终收齐）
        double fps = 0;
        double kbps = 0;
        double lossPercent = 0;
        // 收包线程「过手」的总包数（含被来源/session 过滤丢弃的），诊断防火墙用
        u64    packetsTotal = 0;
    };
    Stats stats() const;

    int udp_socket() const { return udpSock_; }
    size_t queue_depth() const;

    // 供诊断：任何类型、任何来源的包都计入（区分「防火墙全拦」vs「过滤丢」）
    u64 packets_total() const { return packetsTotal_.load(std::memory_order_relaxed); }
    u32 highest_frame_id() const { return highestFrameId_; }
    bool have_highest() const { return haveHighest_; }

private:
    struct Assembly;   // 定义见下方「重组中的帧」（方法签名要先用到这个名字）

    void thread_main();
    // 处理单个 UDP 包（来源过滤 -> 解析 -> 重组）
    void handle_packet(const u8* p, size_t n, const sockaddr_in& from);
    // 取出（或按需新建）某个 frameId 的重组状态；太旧的帧返回 pending_.end()
    std::map<u32, Assembly>::iterator assembly_for(u32 frameId, u32 count,
                                                   u32 frameBytes, i64 sendTsUs);
    // 用组内其它分片 + 校验包恢复缺的那一片（只缺 1 片时必然成功）
    void try_recover_group(std::map<u32, Assembly>::iterator it, u16 groupIdx);
    // 凑齐整帧 -> 入队（或探测模式只计数）
    void finalize_if_complete(std::map<u32, Assembly>::iterator it);
    // 清理超时/落后的未完成帧
    void sweep_pending(i64 nowUs);
    // 给缺片的帧发 NACK（重传请求）
    void poll_nack(i64 nowUs);
    // 按帧号顺序交付（NACK 救回来的帧可能比后续帧完成得晚，先短暂缓冲）
    void deliver_ready(i64 nowUs);
    // 把完整帧压入浅队列（满了顶掉最旧的）
    void push_frame(ReceivedFrame f);
    // 会话边界清空重组状态（被控端重启后帧号会从头开始，旧的高水位必须丢掉）
    void reset_reassembly();

    int   udpSock_ = -1;
    u32   sessionId_ = 0;
    std::atomic<bool> running_{false};
    std::atomic<bool> serverKnown_{false};
    sockaddr_in server_{};
    std::thread th_;

    // 服务端地址的读写保护（set_server 与收包线程并发）
    std::mutex serverMtx_;
    // NAT 漂移容错：疑似的新来源 + 连续命中计数
    sockaddr_in altAddr_{};
    u32         altStreak_ = 0;

    mutable std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<ReceivedFrame> queue_;
    // 队列深度：串流要的是"最新画面"，所以做得很浅；但太小（原来是 2）会在
    // 解码/呈现偶尔多花几毫秒时把整帧顶掉 —— H.264 少一帧就得等 IDR，
    // 表现是"偶尔顿一下"。3 帧 ≈ 50ms 上限，既不会积压延迟，也留了一点余量。
    static constexpr size_t kMaxQueue = 3;

    // 重组中的帧
    struct Assembly {
        u32 frameId = 0;
        bool keyframe = false;
        u32 frameBytes = 0;
        u32 gotBytes = 0;
        u32 gotCount = 0;      // 含 FEC 恢复出来的分片
        u32 rawCount = 0;      // 真正从网络收到的数据分片（不含 FEC 恢复，诊断用）
        u32 totalCount = 0;
        i64 firstArriveUs = 0;
        i64 lastArriveUs = 0;
        i64 sendTsUs = 0;
        Bytes data;
        std::vector<bool> have;
        // ---- NACK（缺片重传请求）----
        // 依据（UU远程/GameViewer 逆向报告）：FEC 只是恒定打底，真正的恢复主力是
        // NACK（重传量随丢包 1:1 增长，实测恢复 42% + FEC 50% = 90~94%）。
        // 规则：等 12ms 给 FEC/乱序机会 → 每 10ms 最多请求一次、每帧最多 2 次。
        i64 lastNackUs = 0;
        u8  nackCount = 0;
        // ---- FEC 校验包（key = 组下标）----
        // 丢 1 片的组可以立刻用「同组其它片 + 校验」恢复，不必等到超时丢整帧。
        // 只存本帧的校验，帧一完成/被丢弃就一起释放，内存有界。
        struct Parity {
            u32  parityLen = 0;
            u16  groupSize = 0;    // 发送端的组大小 G（= UdpHeader.extra）
            Bytes data;
        };
        std::map<u16, Parity> parity;
    };
    std::map<u32, Assembly> pending_;
    // 已凑齐但还在等"按帧号顺序交付"的帧（NACK 救回的帧可能后完成）
    std::map<u32, ReceivedFrame> ready_;
    static constexpr i64 kReorderHoldUs = 25000;   // 最多等 25ms 让前序帧补齐
    u32 highestFrameId_ = 0;
    bool haveHighest_ = false;
    // 最近一次真正交给解码器的帧号：NACK 可能让"老帧"晚于新帧才凑齐，
    // 这种补回来的老帧必须直接丢掉（解码必须严格按帧号递增，否则参考链会乱）。
    u32 lastDeliveredId_ = 0;
    bool haveDelivered_ = false;

    std::atomic<bool> keyframeReq_{false};
    std::atomic<u64> droppedStreak_{0};
    // 收包计数：给「防火墙拦了 UDP」诊断用。任何来源、任何类型的包进来都 +1，
    // 能区分「一个包都没进来（防火墙）」和「进来了但被 session 过滤/来源过滤丢」。
    std::atomic<u64> packetsTotal_{0};

    // ---- 光标 / 探测 ----
    std::atomic<i64> lastPacketUs_{0};
    std::atomic<i64> lastVideoUs_{0};
    std::atomic<bool> probeMode_{false};
    std::atomic<u32>  probeFrames_{0};
    // ---- 音频队列（UDP）----
    static constexpr size_t kMaxAudioQueue = 64;   // 1.28 秒，够吸收抖动
    std::mutex audioMtx_;
    std::deque<AudioChunk> audioQueue_;

    std::mutex ptrMtx_;
    proto::PointerPosMsg ptrPos_{};
    u32  ptrSeq_ = 0;
    bool havePtr_ = false;
    bool ptrDirty_ = false;

    // 统计
    mutable std::mutex statMtx_;
    Stats statCounters_;
    RateCounter rateBytes_;
    RateCounter rateFrames_;
    u32 lastStatsFrameId_ = 0;
};

} // namespace ms::client
