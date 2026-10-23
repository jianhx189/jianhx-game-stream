// =============================================================================
//  input_pipeline.cpp - 可靠输入发送管线实现
// =============================================================================
#include "input_pipeline.h"

#include "../common/clock.h"
#include "../common/log.h"
#include "../common/perf.h"

#include <cstring>

namespace ms::client {

InputPipeline::~InputPipeline() { stop(); }

void InputPipeline::start(const Config& cfg) {
    stop();
    control_ = cfg.control;
    sessionId_ = cfg.sessionId;
    udpSock_ = cfg.udpSock;
    serverAddr_ = cfg.serverAddr;

    {
        std::lock_guard<std::mutex> lk(qMtx_);
        head_ = tail_ = 0;
        havePendMove_ = false;
    }
    nextBatchId_ = 1;
    lastBatchId_.store(0);
    lastAcked_.store(0);
    batchesSent_.store(0);
    eventsSent_.store(0);
    batchesAcked_.store(0);
    sendFailures_.store(0);
    droppedEvents_.store(0);
    serverAcks_.store(false);
    transportTcp_.store(true);
    // 【P0 修复】只按会话层显式传入的 Config 设置初始路由，
    // 不再无条件 useTcp_=true 覆盖调用方设置的路由策略（UDP 主路径/备线）。
    useTcp_.store(cfg.useTcp);
    lastSendUs_ = 0;
    stopReq_.store(false);
    releaseAllReq_.store(false);

    th_ = std::thread([this] {
        // MMCSS：500Hz 合并节奏 + 2ms 冲刷间隔，注册 Pro Audio 拿最短调度周期；
        // 否则系统默认 15.6ms 分辨率会让 sleep_until 变成 sleep 15ms，鼠标会粘。
        const ms::PerfThreadScope perfScope(L"Pro Audio");
        try {
            run();
        } catch (const std::exception& e) {
            MS_ERR("输入发送线程异常: %s（已抑制）", e.what());
        } catch (...) {
            MS_ERR("输入发送线程未知异常（已抑制）");
        }
    });
}

void InputPipeline::stop() {
    if (!th_.joinable()) return;
    stopReq_.store(true);
    {
        std::lock_guard<std::mutex> lk(qMtx_);
        qCv_.notify_all();
    }
    th_.join();
}

bool InputPipeline::enqueue(const proto::InputEvent& e) {
    {
        std::lock_guard<std::mutex> lk(qMtx_);
        const size_t used = (head_ + kQueueCap - tail_) % kQueueCap;
        if (used == kQueueCap - 1) {
            // 满了。策略：丢队头（最旧的）事件。
            //   - 丢掉的若是移动/滚轮：影响只是一瞬间的位移。
            //   - 丢掉的若是按下/抬起：由两端的按下状态表兜底（服务端
            //     InputInjector 记录按下状态，断开时 release_all；客户端
            //     检测到积压时主动 request_release_all），不会永久卡键。
            tail_ = (tail_ + 1) % kQueueCap;
            droppedEvents_.fetch_add(1, std::memory_order_relaxed);
            if (droppedEvents_.load(std::memory_order_relaxed) % 200 == 1) {
                MS_WARN("输入队列已满（钩子线程被阻塞或链路严重积压），丢弃最旧事件 %u 次",
                        droppedEvents_.load(std::memory_order_relaxed));
            }
        }
        ring_[head_] = e;
        head_ = (head_ + 1) % kQueueCap;
    }
    qCv_.notify_one();
    return true;
}

void InputPipeline::set_forwarding(bool on) { forwarding_.store(on); }

void InputPipeline::request_release_all() { releaseAllReq_.store(true); }

void InputPipeline::set_use_tcp(bool on) {
    if (useTcp_.exchange(on, std::memory_order_relaxed) != on) {
        MS_INFO("输入主路径 -> %s", on ? "TCP（可靠有序）"
                                       : "UDP（多份冗余，避免被 TCP 视频帧堵住）");
    }
}

void InputPipeline::set_server_supports_batch(bool on) {
    serverSupportsBatch_.store(on);
    transportTcp_.store(on);
}

void InputPipeline::on_input_applied(u32 batchId, u32 /*applied*/, u8 status) {
    serverAcks_.store(true, std::memory_order_relaxed);
    batchesAcked_.fetch_add(1, std::memory_order_relaxed);
    // u32 差值比较，容忍回绕：只接受比当前已确认更新（或相等）的批次
    const u32 cur = lastAcked_.load(std::memory_order_relaxed);
    if (u32(batchId - cur) <= 0x7FFFFFFFu) {
        lastAcked_.store(batchId, std::memory_order_relaxed);
    }
    if (status == 1) {
        // 部分提交：有事件被拒绝（UIPI 等）。记日志，由上层展示。
        MS_WARN("InputApplied batch=%u status=部分提交（%u 个事件被注入系统拒绝）",
                batchId, 0u);
    } else if (status == 2) {
        MS_WARN("InputApplied batch=%u status=拒绝（被控端输入已禁用）", batchId);
    }
}

// -----------------------------------------------------------------------------
// 发送线程
// -----------------------------------------------------------------------------
void InputPipeline::run() {
    std::vector<proto::InputEvent> batch;
    batch.reserve(proto::kMaxInputBatchEvents);

    while (!stopReq_.load()) {
        bool hasWork = false;
        bool doForward = false;
        bool wantRelease = false;

        // ---- 收集一批：连续移动合并（≤1ms），状态事件立即截断 ----
        {
            std::unique_lock<std::mutex> lk(qMtx_);
            // 等待：有事件 / 请求兜底 / 停止 / 合并窗口到期
            qCv_.wait_for(lk, std::chrono::milliseconds(1), [this] {
                return tail_ != head_ || releaseAllReq_.load() || stopReq_.load();
            });
            doForward = forwarding_.load();
            wantRelease = releaseAllReq_.exchange(false);

            // 优先处理兜底 ReleaseAll：独立于捕获开关（退出捕获/断开也能发）
            if (wantRelease && current_channel_open()) {
                lk.unlock();
                send_release_all_now();
                batch.clear();
                havePendMove_ = false;
                continue;
            }

            while (tail_ != head_ && batch.size() < proto::kMaxInputBatchEvents) {
                proto::InputEvent e = ring_[tail_];
                tail_ = (tail_ + 1) % kQueueCap;
                hasWork = true;

                const auto t = proto::InputType(e.type);
                if (t == proto::InputType::MouseMove &&
                    (e.flags & proto::kInputFlagAbsMove) == 0) {
                    // 相对移动：累加合并（同一事件包住两轴）
                    if (!havePendMove_) {
                        pendMove_ = e;
                        havePendMove_ = true;
                    } else {
                        pendMove_.a += e.a;
                        pendMove_.b += e.b;
                    }
                    continue;
                }
                // 状态事件（按钮/按键/滚轮/绝对移动/ReleaseAll）：
                // 先把挂起的移动提交，保持全局顺序，绝不跨越按下/抬起边界
                if (havePendMove_) {
                    batch.push_back(pendMove_);
                    havePendMove_ = false;
                }
                batch.push_back(e);
            }
            // 队列读完，挂起的移动也可以提交了（合并窗口 ≤1ms 已足够）
            if (havePendMove_) {
                batch.push_back(pendMove_);
                havePendMove_ = false;
            }
        }

        if (!hasWork) continue;
        if (batch.empty()) continue;

        if (!doForward) {
            // 非转发态：只允许 ReleaseAll 通过，其它事件直接丢
            batch.clear();
            continue;
        }
        const bool tcpPath = useTcp_.load(std::memory_order_relaxed) &&
                              serverSupportsBatch_.load(std::memory_order_relaxed) &&
                              current_channel_open();
        if (!tcpPath && (udpSock_ < 0 || serverAddr_.sin_family == 0)) {
            // 两条路都不可用（比如刚断线）：事件无处可去，清空等会话收尾
            batch.clear();
            continue;
        }

        // ---- 鼠标移动合并到约 250Hz ----------------------------------------
        // 1000Hz 游戏鼠标如果每个回报都发一个包，控制通道会被上千个/秒的小包
        // 刷满，被控端的注入线程也跟着抖。游戏画面本身就是按帧采样，
        // 250Hz（4ms）对瞄准完全够用，且延迟感知不到。
        bool onlyMoves = true;
        for (const auto& e : batch) {
            if (proto::InputType(e.type) != proto::InputType::MouseMove) {
                onlyMoves = false;
                break;
            }
        }
        if (onlyMoves) {
            const i64 now = now_us();
            const i64 waitUs = kMoveFlushIntervalUs - (now - lastSendUs_);
            if (waitUs > 0 && waitUs <= kMoveFlushIntervalUs) {
                sleep_ms(int(waitUs / 1000) + 1);
            }
        }

        // ---- 发送 ----------------------------------------------------------
        if (tcpPath) {
            const u32 bid = nextBatchId_;
            ++nextBatchId_;
            send_batch_tcp(bid, batch.data(), batch.size());
            lastBatchId_.store(bid, std::memory_order_relaxed);
        } else {
            // UDP 主路径：状态事件（按下/抬起/滚轮）发 3 份 —— 丢一个「抬起」
            // 会让角色一直往前跑，3 份冗余把概率压到万分之一级；
            // 纯鼠标移动发 2 份（丢一份只是少一点位移，下一包就补回来）。
            bool hasState = false;
            for (const auto& e : batch) {
                if (proto::InputType(e.type) != proto::InputType::MouseMove) {
                    hasState = true;
                    break;
                }
            }
            send_batch_udp(batch.data(), batch.size(), hasState ? 3 : 2);
        }
        lastSendUs_ = now_us();
        batchesSent_.fetch_add(1, std::memory_order_relaxed);
        eventsSent_.fetch_add(u32(batch.size()), std::memory_order_relaxed);
        batch.clear();
    }
}

void InputPipeline::send_batch_tcp(u32 batchId, const proto::InputEvent* ev, size_t n) {
    // 双线快速切换：连续 3 次发送失败 = 当前通道已死，立即切到另一条。
    // 不卡半天等超时 —— 键鼠是交互，延迟 100ms 就能感觉到。
    net::TcpChannel* ch = useBackup_.load() && backupActive_.load() && backupCh_
                              ? backupCh_ : (mainCh_ ? mainCh_ : control_);
    if (!ch || !ch->is_open()) {
        consecFail_.fetch_add(1, std::memory_order_relaxed);
        maybe_failover();
        return;
    }
    const Bytes pl = proto::encode_input_batch(batchId, ev, n);
    if (ch->send_frame(proto::MsgType::InputBatch, pl)) {
        if (consecFail_.load() > 0) {
            MS_INFO("输入通道已恢复（%s）", useBackup_.load() ? "备线" : "主线");
            consecFail_.store(0);
        }
    } else {
        consecFail_.fetch_add(1, std::memory_order_relaxed);
        maybe_failover();
    }
}

void InputPipeline::maybe_failover() {
    if (consecFail_.load() < 3) return;
    const bool wasBackup = useBackup_.load();
    const bool canBackup = backupCh_ && backupActive_.load();
    const bool canMain   = mainCh_ && mainCh_->is_open();
    if (wasBackup && canMain) {
        useBackup_.store(false);
        consecFail_.store(0);
        MS_WARN("备线连续 3 次发送失败，键鼠切回主线");
    } else if (!wasBackup && canBackup) {
        useBackup_.store(true);
        consecFail_.store(0);
        MS_WARN("主线连续 3 次发送失败，键鼠切到备线");
    } else if (wasBackup && !canMain && !canBackup) {
        // 两条都死了：回主线（至少等重连时键鼠会回来）
        useBackup_.store(false);
        consecFail_.store(0);
    }
}

void InputPipeline::send_batch_udp(const proto::InputEvent* ev, size_t n, int copies) {
    if (udpSock_ < 0 || serverAddr_.sin_family == 0) return;
    if (copies < 1) copies = 1;
    if (copies > 3) copies = 3;
    const Bytes pl = proto::encode_input_packet(ev, n);
    std::vector<u8> pkt(sizeof(proto::UdpHeader) + pl.size());
    proto::UdpHeader h{};
    h.type = u8(proto::UdpType::Input);
    h.sessionId = sessionId_;
    h.id = ++udpPktSeq_;          // 包序号：被控端按它整包去重（3 份只注入一次）
    h.payloadSize = u16(pl.size());
    h.extra = u32(n);
    std::memcpy(pkt.data(), &h, sizeof(h));
    std::memcpy(pkt.data() + sizeof(h), pl.data(), pl.size());
    // 同一份连发多次：被控端按 seq/包序号去重（重复份是幂等的），
    // 相当于「不等待的廉价重传」，既抗丢包又不会引入重传延迟。
    // 状态事件（3 份）之间隔 2ms：链路丢包常常是"突发丢一串"，
    // 紧挨着发的 3 份会一起被丢掉；拉开 2ms 让它们落在不同的突发窗口里
    // （UU远程/GameViewer 实测也是间隔 2~3ms 发 3 份）。
    // 纯移动事件（2 份）不拉开：移动包要的是密度和低延迟，丢一份下一包就补上了。
    for (int i = 0; i < copies; ++i) {
        if (i > 0 && copies >= 3) sleep_ms(2);
        if (net::udp_send_to(udpSock_, pkt.data(), pkt.size(), serverAddr_) < 0) {
            sendFailures_.fetch_add(1, std::memory_order_relaxed);
            break;
        }
    }
}

void InputPipeline::send_release_all_now() {
    proto::InputEvent e;
    e.type = u8(proto::InputType::ReleaseAll);
    e.flags = 0;
    e.seq = 0;
    e.a = 0;
    e.b = 0;
    const bool tcpPath = useTcp_.load(std::memory_order_relaxed) &&
                          serverSupportsBatch_.load(std::memory_order_relaxed) &&
                          current_channel_open();
    if (tcpPath) {
        const u32 bid = nextBatchId_;
        ++nextBatchId_;
        send_batch_tcp(bid, &e, 1);
        lastBatchId_.store(bid, std::memory_order_relaxed);
    } else {
        send_batch_udp(&e, 1, 3);
    }
    MS_INFO("输入管线：已发送兜底 ReleaseAll");
}

} // namespace ms::client
