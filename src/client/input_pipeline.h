// =============================================================================
//  input_pipeline.h - 可靠输入发送管线（v2 第一阶段）
//
//  职责：把捕获层产生的事件经 **TCP 控制通道**可靠、有序地送到被控端。
//
//  设计要点（对应诊断方案第一阶段）：
//   * 主路径走 TCP（InputBatch）：不丢、有序。丢一个包不再等于丢一次点击。
//   * 服务端不认识 InputBatch（旧版）时自动回退 UDP 老路径，界面可提示。
//   * 专用发送线程：捕获回调（钩子/Raw Input）只入队，网络超时不阻塞钩子。
//   * 合并只针对连续 MouseMove（≤1ms 窗口），遇到按钮/按键/滚轮立即提交，
//     且绝不跨越按下/抬起边界 —— 拖拽、点击的顺序语义由传输层保证。
//   * batchId 会话内递增；收到 InputApplied 扣减在途计数，作为"已提交"依据。
//   * 队列满：丢「可合并的移动」，绝不丢按下/抬起（宁可终止本轮也要发 ReleaseAll）。
// =============================================================================
#pragma once

#include "../common/types.h"
#include "../common/protocol.h"
#include "../common/net.h"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace ms::client {

class InputPipeline {
public:
    InputPipeline() = default;
    ~InputPipeline();

    // 启动配置（不可变快照）：会话层在完成认证、地址解析、UDP 创建、
    // 输入通道绑定（含备线握手）之后，把路由策略一次性显式传入。
    // start() 只负责按这份配置启动线程，绝不擅自修改路由策略 ——
    // 之前的版本在 start() 里无条件 useTcp_=true，把会话层精心设置的
    // 路由（UDP 主路径 / 备线 TCP）覆盖掉，是 P0 级 bug。
    struct Config {
        net::TcpChannel* control = nullptr;   // 主线路 TCP 通道（不拥有）
        u32 sessionId = 0;
        int udpSock = -1;                     // UDP 回退/主路径 socket（可为 <0）
        sockaddr_in serverAddr{};             // 被控端 UDP 地址（sin_family==0 表示未解析）
        bool useTcp = false;                  // 初始路由策略：true=TCP（备线/控制通道）
    };

    // 启动发送线程。必须在 UDP socket 创建、地址解析、备线握手全部完成后调用。
    void start(const Config& cfg);

    // 停止并 join。会尽力把剩余事件发完（含兜底 ReleaseAll）。
    void stop();

    // 捕获层调用（任意线程，含钩子线程）：入队事件。
    // 返回 false 表示队列已满且该事件不可丢弃（按下/抬起），调用方可重试或放弃。
    bool enqueue(const proto::InputEvent& e);

    // 会话级开关：false 时只放行 ReleaseAll（退出捕获/断开时用）
    void set_forwarding(bool on);
    // 请求发送兜底 ReleaseAll（切模式/重连/断开前调用），优先级最高
    void request_release_all();

    // 主路径选择（会话层按视频传输模式切换）：
    //   true  = 走 TCP 控制通道（可靠有序；视频走 UDP 时控制通道很空，最优）
    //   false = 走 UDP + 多份冗余（视频已经挤在 TCP 上时必须这样，否则键鼠
    //           会和几十 KB 的视频帧排同一条 TCP 流，表现就是「鼠标一顿一顿」）
    void set_use_tcp(bool on);
    // 双线：主/备 TCP 通道切换。sendFailures 连续 3 次后认为当前通道死了，
    // 立即切到另一条（不卡半天）。切换是原子的，发送线程下一把就用新通道。
    void set_main_channel(net::TcpChannel* ch) { mainCh_ = ch; }
    void set_backup_channel(net::TcpChannel* ch) { backupCh_ = ch; }
    void set_backup_active(bool on) { backupActive_.store(on); }
    bool using_backup() const { return useBackup_.load(std::memory_order_relaxed); }
    void set_use_backup(bool on) { useBackup_.store(on); }

    // ---- 状态（工具栏/日志用）----
    u32  batches_sent()   const { return batchesSent_.load(std::memory_order_relaxed); }
    u32  events_sent()    const { return eventsSent_.load(std::memory_order_relaxed); }
    u32  batches_acked()  const { return batchesAcked_.load(std::memory_order_relaxed); }
    u32  send_failures()  const { return sendFailures_.load(std::memory_order_relaxed); }
    u32  dropped_events() const { return droppedEvents_.load(std::memory_order_relaxed); }
    bool server_acked()   const { return serverAcks_.load(std::memory_order_relaxed); }
    // 待确认批次数（>0 说明服务端还没提交完）
    u32  in_flight()      const { return u32(lastBatchId_.load(std::memory_order_relaxed) -
                                                 lastAcked_.load(std::memory_order_relaxed)); }
    bool using_tcp()      const { return useTcp_.load(std::memory_order_relaxed) &&
                                         serverSupportsBatch_.load(std::memory_order_relaxed); }

    // 控制线程收到 InputApplied 时调用（多线程安全）
    void on_input_applied(u32 batchId, u32 applied, u8 status);
    // 服务端声明支持可靠输入（Hello/StreamReady 之后由会话层设置）
    void set_server_supports_batch(bool on);

private:
    void run();
    void send_batch_tcp(u32 batchId, const proto::InputEvent* ev, size_t n);
    void send_batch_udp(const proto::InputEvent* ev, size_t n, int copies);
    void send_release_all_now();
    void maybe_failover();   // 连续失败 3 次后主/备通道切换
    bool current_channel_open() const {
        if (useBackup_.load() && backupActive_.load() && backupCh_)
            return backupCh_->is_open();
        return mainCh_ ? mainCh_->is_open() : (control_ && control_->is_open());
    }

    net::TcpChannel* control_ = nullptr;   // 不拥有（当前生效的通道：主或备）
    net::TcpChannel* mainCh_ = nullptr;    // 主线路（视频+控制）
    net::TcpChannel* backupCh_ = nullptr;  // 备用线路（双线：键鼠专用）
    std::atomic<bool> backupActive_{false};  // 备线已就绪
    std::atomic<bool> useBackup_{false};   // 当前键鼠是否走备线
    std::atomic<int>  consecFail_{0};      // 连续发送失败次数（>3 切通道）
    u32 sessionId_ = 0;
    int udpSock_ = -1;
    sockaddr_in serverAddr_{};
    // UDP 输入包序号（单调递增，写进 UdpHeader.id）。被控端用它整包去重：
    // 冗余连发的 3 份完全一样，去重后只注入一次。
    u32 udpPktSeq_ = 0;

    // ---- 队列（钩子线程写 / 发送线程读）----
    // 固定容量环形缓冲，容量取服务端批上限的 4 倍：发送线程短暂卡顿时不丢状态事件
    static constexpr size_t kQueueCap = 512;
    proto::InputEvent ring_[kQueueCap];
    size_t head_ = 0, tail_ = 0;            // head_: 写入位; tail_: 读出位
    mutable std::mutex qMtx_;
    std::condition_variable qCv_;

    // 发送线程侧的移动合并缓冲（不经 ring_）
    proto::InputEvent pendMove_{};          // 待合并的相对移动
    bool havePendMove_ = false;

    // 鼠标移动的发送节流：1000Hz 回报合并到 ~500Hz（2ms，Moonlight 是 1ms）。
    // 游戏只按帧采样，再高的发送频率只会把控制通道和注入线程刷爆。
    static constexpr i64 kMoveFlushIntervalUs = 2000;
    i64 lastSendUs_ = 0;

    std::thread th_;
    std::atomic<bool> stopReq_{false};
    std::atomic<bool> forwarding_{false};
    std::atomic<bool> releaseAllReq_{false};

    // 会话内批次号（u32 回绕无害：in_flight 用差值比较）
    u32 nextBatchId_ = 1;
    std::atomic<u32> lastBatchId_{0};
    std::atomic<u32> lastAcked_{0};

    std::atomic<bool>   serverSupportsBatch_{false};
    std::atomic<bool>   serverAcks_{false};      // 收到过至少一个 InputApplied
    std::atomic<bool>   transportTcp_{true};     // 服务端能力（认识 InputBatch）
    std::atomic<bool>   useTcp_{true};           // 当前会话选用的主路径
    std::atomic<u32>    batchesSent_{0};
    std::atomic<u32>    eventsSent_{0};
    std::atomic<u32>    batchesAcked_{0};
    std::atomic<u32>    sendFailures_{0};
    std::atomic<u32>    droppedEvents_{0};
};

} // namespace ms::client
