// =============================================================================
//  host_service.cpp
// =============================================================================
#include "host_service.h"

#include "capture.h"
#include "encoder.h"
#include "video_sender.h"
#include "audio_sender.h"
#include "input_inject.h"

#include "../common/clock.h"
#include "../common/crypt.h"
#include "../common/log.h"
#include "../common/net.h"
#include "../common/perf.h"
#include "../common/stats.h"

#include <windows.h>   // GetFileAttributesW / GetEnvironmentVariableW（自测钩子用）

#include <atomic>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace ms::host {
namespace {

// 等待客户端第一个 UDP 包（用来学习 NAT 后的真实来源地址）的上限
constexpr int kUdpPeerLearnMs = 1500;
// 静态画面下，如果有关键帧请求迟迟等不到新帧，就重编码缓存的上一帧
constexpr int kStaticKeyframeMs = 200;

// 【与远控端的语义约定 —— 两端必须一致，否则自适应会静默失效】
// FeedbackMsg.framesOk / framesLost 是**「上次反馈以来的增量」**：客户端
// control_loop 每 250ms 发一次，只填这段区间的帧数（见 client_session.cpp）。
// 所以这里不做累计基线，直接把本包数值累进采样窗口；好处是丢一个反馈包只是
// 少一段样本，不会被误读成"计数倒退"。
// 注意：protocol.h 里这两个字段的注释还写着「累计」，与客户端实现不一致，
// 已在任务汇报里提给 Lead（若将来改回累计口径，这里要恢复"取增量"的算法）。

// ---- 自适应 FEC 组大小用到的阈值 --------------------------------------------
// 采样帧数下限：单次反馈只有 ~15 帧（250ms@60fps），样本太小算出的丢帧率噪声
// 很大（丢 1 帧就 3%），照它调组大小会让 G 在两档之间来回抖。所以这里用
// 「累积窗口」：攒够 20 帧再判断 —— 既压住噪声，又不会像"丢弃不足 20 帧的
// 样本"那样在反馈周期短的时候永远触发不了。
constexpr u32 kFecMinSamples = 20;
// 防抖：连续三次判断都指向同一个方向（要变大 / 要变小）才真正生效，
// 并且目标与当前相差不到 2 档时不动（死区）。
// 一次抖动就改的话，链路轻微波动就会让校验开销反复跳变
// （实测 5% 丢包时会在 G=4/5/8 之间来回跳，带宽跟着抖）。
constexpr int kFecDebounce = 3;
constexpr int kFecDeadband = 2;

// 丢帧率 -> 目标组大小。组越大校验开销越低（1/G），但越怕"同组连丢两片"，
// 所以丢包越重就该用越小的组、把恢复能力换成带宽。
int fec_target_for_loss(double loss) {
    if (loss < 0.005) return 20;    // <0.5%：5% 开销
    if (loss < 0.02)  return 12;    // <2%  ：8%  开销
    if (loss < 0.05)  return 8;     // <5%  ：12% 开销
    if (loss < 0.10)  return 5;     // <10% ：20% 开销
    return 4;                       // 更差：25% 开销，优先保住画面连续
}

// ---- 自测钩子用的小工具（正常运行时环境变量不存在，零开销）----
std::wstring get_env_w(const wchar_t* name) {
    wchar_t buf[512]{};
    const DWORD n = ::GetEnvironmentVariableW(name, buf, 512);
    return n ? std::wstring(buf, n) : std::wstring();
}
bool file_exists_w(const std::wstring& path) {
    return !path.empty() && ::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

} // namespace

// -----------------------------------------------------------------------------
//  Impl
// -----------------------------------------------------------------------------
struct HostService::Impl {
    HostConfig cfg;

    std::atomic<bool> running{false};
    std::atomic<bool> stopReq{false};

    std::thread thAccept, thStream, thInput;

    // ---- 双线（主线 + 备用线路）--------------------------------------------
    // 备用线路是第二条 TCP 连接：客户端从「备份端口」（cfg.backup_port）连进来，
    // 只传键鼠/控制，不传视频。主线（cfg.port）照旧传视频 + 控制。
    // 双线的好处：键鼠不再和视频帧排同一条 TCP 流，主线带宽全留给视频。
    net::TcpChannel controlBackup;              // 备用线路的 TCP 连接
    std::atomic<bool> backupActive{false};      // 备用线路是否已接入
    std::thread thAcceptBackup;                 // 备用线路的 accept 线程
    // accept_backup_channel / accept_backup_loop 的实现见后面（放在 accept_loop_body 之前）

    // ---- 生命周期同步 --------------------------------------------------------
    // stop() 会被多个地方调用：托盘"停止服务"、"退出"、main 里退出后的一刀，
    // 以及 ~HostService()。旧版本只用一个 running 标志，而且是在最后才置 false，
    // 于是两个 stop() 会同时进入、同时 join 同一个 std::thread —— MSVC 的
    // join() 在句柄已被别的线程 join 掉时会抛 std::system_error，
    // 线程函数里没人接就是 std::terminate -> abort。
    std::mutex stopMtx;           // 串行化 start()/stop()：第二个调用者排队，不并发
    bool       started = false;   // 受 stopMtx 保护
    std::mutex lifeMtx;           // 保护三个线程句柄

    // 句柄先"搬出锁外"再 join：谁搬走谁负责 join，天然幂等，
    // 也不可能出现两个线程同时 join 同一个 std::thread。
    static void join_thread(std::thread& t, std::mutex& m) {
        std::thread victim;
        {
            std::lock_guard<std::mutex> lk(m);
            if (!t.joinable()) return;
            // 自己 join 自己必然死锁，只能 detach
            if (t.get_id() == std::this_thread::get_id()) { t.detach(); return; }
            victim = std::move(t);
        }
        victim.join();
    }
    void join_session_threads() {
        join_thread(thInput, lifeMtx);
        join_thread(thStream, lifeMtx);
    }
    void close_udp() {
        if (udpSock >= 0) { ::closesocket(SOCKET(udpSock)); udpSock = -1; }
    }
    // 重新绑定 UDP 套接字到指定本机地址。
    // 多网卡机器（例如 192.168.1.10 虚拟网卡 + 192.168.1.11 物理网卡）上，
    // 如果绑定 0.0.0.0，回包的源 IP 由路由表决定，可能和客户端连入的地址不一致；
    // FRP 那种 connect() 过的 UDP 套接字只认它发过去的那个地址，源地址不对的
    // 回包会被直接丢弃 —— 表现就是"上行通、下行黑屏"。
    bool rebind_udp(const std::string& ip, std::string& err) {
        close_udp();
        const int s = net::udp_open(ip, u16(cfg.port), err);
        if (s < 0) return false;
        // 【必须】输入线程的收包循环是按非阻塞写的，套接字保持阻塞的话它会在
        // recvfrom 里睡死，会话结束时 join 一直等（实测卡 30 多秒才被 EINTR 打断），
        // 被控端就回不到监听状态。不能指望视频发送器初始化时再设（有先后竞态）。
        net::udp_set_nonblocking(s, true);
        udpSock = s;
        return true;
    }
    // 把 UDP 绑到「客户端实际连进来的那块本机 IP」；失败则退回 0.0.0.0
    void bind_udp_to_control_local_ip() {
        sockaddr_in la{};
        if (!control.local_addr(la)) return;
        const std::string lip = net::addr_str(la);
        if (lip.empty() || lip == "0.0.0.0") return;
        std::string berr;
        if (rebind_udp(lip, berr)) {
            MS_INFO("UDP 已绑定本机地址 %s:%u（保证回包源地址 = 客户端连入地址）",
                    lip.c_str(), unsigned(cfg.port));
        } else {
            MS_WARN("UDP 绑定本机地址 %s 失败: %s（退回 0.0.0.0，多网卡下可能黑屏）",
                    lip.c_str(), berr.c_str());
            std::string e2;
            rebind_udp("0.0.0.0", e2);
        }
    }
    void restore_udp_wildcard() {
        std::string err;
        if (!rebind_udp("0.0.0.0", err)) {
            MS_WARN("UDP 恢复 0.0.0.0 绑定失败: %s", err.c_str());
        }
    }
    void spawn_session_threads(const proto::HelloMsg& hello) {
        join_session_threads();   // 防御：上一轮的句柄必须先回收干净
        std::thread s([this, hello] { stream_loop(hello); });
        std::thread i([this] { input_loop(); });
        std::lock_guard<std::mutex> lk(lifeMtx);
        thStream = std::move(s);
        thInput  = std::move(i);
    }

    // --- 会话共享状态 ---
    std::atomic<u32>  sessionId{0};
    std::atomic<bool> sessionActive{false};
    std::atomic<bool> keyframePending{false};
    std::atomic<int>  targetBitrate{0};
    std::atomic<int>  keyframeIntervalSec{0};     // >0 表示有热调请求待应用
    // 重协商请求（分辨率/帧率变化时置位，stream 线程重建采集与编码器）
    std::atomic<int>  reconfigW{0};
    std::atomic<int>  reconfigH{0};
    std::atomic<int>  reconfigFps{0};
    std::atomic<int>  reconfigCodec{0};   // 0=不变，1=H.264，2=HEVC
    std::atomic<bool> reconfigPending{false};
    // TCP 视频回退：远控端 UDP 收不到视频时会请求改走控制通道（TCP）。
    // TCP 比 UDP 延迟略高，但不会丢包/被防火墙拦，打游戏时"能出画面"优先。
    // 0 = UDP（默认，低延迟）；1 = TCP（兜底）。
    std::atomic<int> videoTransport{0};
    // UDP 探测窗口结束时刻：远控端在 TCP 模式下会周期性请求探测，
    // 窗口内视频**同时**走 UDP，客户端统计到达率决定要不要切回 UDP。
    // 这就是"自愈"：UDP 只是抖了一下的话，几秒后自己就回到低延迟路径了。
    std::atomic<i64> udpProbeUntilUs{0};

    // ---- 自适应 FEC 组大小 ---------------------------------------------------
    // 远控端的 FeedbackMsg 带回「上次反馈以来的 framesOk / framesLost 增量」，
    // 这里累积成足够大的样本，算出丢帧率再映射成目标组大小。两个 Feedback 处理点
    // （UDP 在 input 线程、TCP 在 accept 线程）都会进来，所以状态必须加锁。
    // 真正的下发动作在 stream_loop 里做 —— VideoSender 是 stream 线程的局部
    // 对象，跨线程只能动这里的目标值。
    std::atomic<int> fecGroupTarget{int(proto::kFecDefaultGroup)};
    std::mutex fecMtx;
    std::atomic<int> lastRttMs{0};       // 客户端反馈的 RTT（ms），cwnd 用
    u32  fecAccOk = 0;             // 采样窗口内累积的「完整收到」帧数
    u32  fecAccLost = 0;           // 采样窗口内累积的「缺包丢弃」帧数
    int  fecStreakDir = 0;         // 连续同向的方向：+1 变大 / -1 变小
    int  fecStreakCount = 0;       // 该方向已连续出现几次

    void on_video_feedback(const proto::FeedbackMsg& fb) {
        // RTT 每 250ms 刷新一次（无论 UDP/TCP），给 cwnd 拥塞窗口用
        if (fb.rttMs > 0) lastRttMs.store(int(fb.rttMs), std::memory_order_relaxed);
        // TCP 传视频时没有真正意义上的 UDP 丢包可测：这份统计里混着 TCP 帧（不丢）
        // 和探测窗口里的 UDP 缺帧，算出来的丢帧率会在 0% 和 30%+ 之间来回跳。
        // 如果这时候还去调组大小，组大小就会在 20（5%）和 4（25%）之间来回抖，
        // 等到切回 UDP 的那一瞬间保护力度刚好是错的那一档。所以只在 UDP 传视频时自适应。
        if (videoTransport.load(std::memory_order_relaxed) != 0) return;
        int applied = 0;
        double loss = 0.0;
        {
            std::lock_guard<std::mutex> lk(fecMtx);
            // 反馈周期是 250ms，60fps 下每次只有 ~15 帧，达不到 kFecMinSamples。
            // 所以不能"单包直接判断"，而是攒够再算（约两次反馈 ≈ 500ms）：
            // 既压住小样本噪声，又不会像"丢弃不足 20 帧的样本"那样永远触发不了。
            // 丢帧率的统计区间 = 上次判断之后的所有反馈。
            fecAccOk += fb.framesOk;
            fecAccLost += fb.framesLost;
            const u32 sample = fecAccOk + fecAccLost;
            if (sample < kFecMinSamples) return;   // 样本还不够：保持当前组大小

            loss = double(fecAccLost) / double(sample);
            fecAccOk = 0;      // 这段样本已被消费，下一个判断区间从零开始
            fecAccLost = 0;
            const int desired = fec_target_for_loss(loss);
            const int cur = fecGroupTarget.load(std::memory_order_relaxed);
            // 死区：目标与当前相差不到 2 档就不动。相邻档（比如 4↔5、8↔12）
            // 的恢复能力差别很小，但校验开销差 5~8 个百分点，来回跳只会让码率抖。
            const int delta = desired - cur;
            if (delta == 0 || (delta > 0 ? delta : -delta) < kFecDeadband) {
                fecStreakDir = 0;
                fecStreakCount = 0;
                return;
            }

            const int dir = (desired > cur) ? 1 : -1;
            if (dir == fecStreakDir) {
                ++fecStreakCount;
            } else {
                fecStreakDir = dir;
                fecStreakCount = 1;
            }
            if (fecStreakCount < kFecDebounce) return;   // 还没连续两次同向，先不动

            fecGroupTarget.store(desired, std::memory_order_relaxed);
            fecStreakDir = 0;
            fecStreakCount = 0;
            applied = desired;
        }
        // 日志放在锁外面：log_write 自己有锁，不值得在持有 fecMtx 时再进一把。
        if (applied != 0) {
            MS_INFO("自适应 FEC：丢帧率 %.2f%%，组大小 %d（校验开销约 %d%%）",
                    loss * 100.0, applied, 100 / applied);
        }
    }

    // 新会话开始：自适应状态清零、组大小回到协议默认值。
    // 换会话后丢帧率要重新观察，别沿用上个会话的防抖方向。
    void reset_fec_adapt() {
        std::lock_guard<std::mutex> lk(fecMtx);
        fecAccOk = 0;
        fecAccLost = 0;
        fecStreakDir = 0;
        fecStreakCount = 0;
        fecGroupTarget.store(int(proto::kFecDefaultGroup), std::memory_order_relaxed);
    }

    // UDP
    int udpSock = -1;
    std::atomic<bool> udpPeerKnown{false};
    sockaddr_in udpPeer{};
    mutable std::mutex peerMtx;

    // TCP 控制通道（仅 accept 线程使用）
    net::TcpChannel control;
    std::string controlPeer;
    bool controlOpen = false;

    // 输入注入
    InputInjector injector;

    // ---- v2 可靠输入批次队列（accept 线程写 / 输入线程读）----
    // 单一注入点：UDP 输入包和 TCP InputBatch 都进同一个队列，由输入线程
    // 串行 apply —— 消灭"控制线程和 UDP 线程同时操作注入器"的竞态。
    struct InputBatchItem {
        u32 batchId = 0;
        int  fromChannel = 0;               // 回执来源连接：0=UDP(不回) 1=主线 2=备线
        u32 count = 0;
        proto::InputEvent* events = nullptr; // make_unique[] 转移所有权
    };
    std::mutex inBatchMtx;
    std::vector<InputBatchItem> inBatchQueue;

    // ---- NACK（缺片重传）队列 ------------------------------------------------
    // 输入线程负责收包（UDP 收包循环在那边），串流线程才是 VideoSender 的持有者，
    // 所以这里只做"收 → 入队"，真正重传在串流线程里做（不跨线程往同一个 socket 发）。
    struct NackReq {
        u32 frameId = 0;
        u32 count = 0;
        u16 indexes[32]{};
    };
    std::mutex nackMtx;
    std::vector<NackReq> nackQueue;
    u64 inputQueuedUs = 0;                  // 最近一次入队时刻（排障用）

    // 投递一个批次（accept 线程调用；events 所有权转移进来）。
    // 【P0 修复】fromChannel 记录回执要走的**来源连接**：0=UDP老路径(不回执)
    // 1=主线控制通道 2=备线TCP。旧版固定回主线，备线键鼠的回执全走错路。
    void queue_input_batch(u32 batchId, int fromChannel,
                           proto::InputEvent* events, u32 count) {
        std::lock_guard<std::mutex> lk(inBatchMtx);
        // 积压保护：队列超过 64 批说明链路严重卡顿，丢最旧批次并告警。
        // 丢批次会破坏按下/抬起配对，因此丢完立刻补一条 ReleaseAll。
        if (inBatchQueue.size() >= 64) {
            MS_WARN("输入批次积压（>64），丢弃最旧批次 batch=%u", inBatchQueue.front().batchId);
            if (inBatchQueue.front().events) delete[] inBatchQueue.front().events;
            inBatchQueue.erase(inBatchQueue.begin());
            proto::InputEvent* rel = new proto::InputEvent[1];
            rel[0].type = u8(proto::InputType::ReleaseAll);
            inBatchQueue.push_back(InputBatchItem{0, 0, 1, rel});
        }
        inBatchQueue.push_back(InputBatchItem{batchId, fromChannel, count, events});
        inputQueuedUs = now_us();
    }

    // 输入线程从队列取一批（返回 false = 空）
    bool pop_input_batch(InputBatchItem& out) {
        std::lock_guard<std::mutex> lk(inBatchMtx);
        if (inBatchQueue.empty()) return false;
        out = std::move(inBatchQueue.front());
        inBatchQueue.erase(inBatchQueue.begin());
        return true;
    }

    // 注入完成后发回执（输入线程调用）。
    // 【P0 修复】回执走**来源连接**：备线的批次回备线、主线的回主线。
    // 发送失败只记日志不重试 —— 注入线程不能被网络拖住，客户端靠
    // 「回执推进 + 心跳」健康判定兜底（无进展会触发线路切换）。
    void send_input_applied(int fromChannel, u32 batchId, u32 applied, u8 status) {
        const Bytes pl = proto::encode_input_applied(batchId, applied, status);
        bool ok = false;
        if (fromChannel == 2) {
            ok = controlBackup.send_frame(proto::MsgType::InputApplied, pl);
            if (!ok) MS_WARN("InputApplied 回备线失败（batch=%u），客户端将按无进展处理", batchId);
        } else {
            ok = control.send_frame(proto::MsgType::InputApplied, pl);
            if (!ok) MS_WARN("InputApplied 回主线失败（batch=%u）", batchId);
        }
    }

    // 状态
    mutable std::mutex statMtx;
    HostStatus status;

    // 监听失败信息的去重（只被 accept 线程读写，不需要锁）
    std::string lastListenError;

    StatusSink sink;

    // 供 stream 线程读的协商结果
    std::atomic<int> negotiatedW{0}, negotiatedH{0}, negotiatedFps{0}, negotiatedBitrate{0};

    void set_error(const std::string& e) {
        std::lock_guard<std::mutex> lk(statMtx);
        status.lastError = e;
        MS_ERR("%s", e.c_str());
    }

    void publish() {
        HostStatus copy;
        {
            std::lock_guard<std::mutex> lk(statMtx);
            copy = status;
        }
        if (sink) sink(copy);
    }

    void set_udp_peer(const sockaddr_in& a) {
        std::lock_guard<std::mutex> lk(peerMtx);
        udpPeer = a;
        udpPeerKnown = true;
    }

    bool get_udp_peer(sockaddr_in& out) {
        std::lock_guard<std::mutex> lk(peerMtx);
        if (!udpPeerKnown.load()) return false;
        out = udpPeer;
        return true;
    }

    // ---- TCP 握手 + 口令校验 ----
    // 任何一步失败都先给对端回一条带原因的 Disconnect 再断开。
    // 之前直接 close()，远控端只能看到"连接被关闭"，排障全靠猜。
    void reject(const std::string& reason) {
        proto::DisconnectMsg dm;
        dm.code = 1;
        dm.reason = reason;
        control.send_frame(proto::MsgType::Disconnect, dm.serialize());
    }

    bool handshake(u32& sidOut, proto::HelloMsg& helloOut, std::string& err) {
        proto::MsgType t;
        Bytes payload;

        int r = control.recv_frame(t, payload, 5000);
        if (r < 0) { err = "连接在握手前被对端关闭"; return false; }
        if (r == 0 || t != proto::MsgType::Hello) {
            err = "未收到 Hello 消息";
            reject("这不是 MiniStream 远控端？（未收到 Hello）");
            return false;
        }
        proto::HelloMsg hello;
        if (!proto::HelloMsg::parse(payload.data(), payload.size(), hello)) {
            err = "Hello 消息格式错误";
            reject("Hello 消息格式错误");
            return false;
        }
        if (hello.version != proto::kVersion) {
            err = "协议版本不匹配（对端 " + std::to_string(hello.version) +
                  "，本机 " + std::to_string(proto::kVersion) + "）";
            reject("版本不匹配：远控端 v" + std::to_string(hello.version) +
                   "，被控端 v" + std::to_string(proto::kVersion) + "，请更新其中一端");
            return false;
        }

        // 挑战-应答：不传明文密码
        proto::ChallengeMsg chal;
        crypt::random_bytes(chal.serverNonce, proto::kNonceLen);
        crypt::random_bytes(chal.salt, proto::kSaltLen);
        if (!control.send_frame(proto::MsgType::Challenge, chal.serialize())) {
            err = "发送 Challenge 失败";
            return false;
        }

        r = control.recv_frame(t, payload, 5000);
        if (r < 0) { err = "连接在握手期间被对端关闭"; return false; }
        if (r == 0 || t != proto::MsgType::Auth) {
            err = "未收到 Auth 消息";
            reject("未收到 Auth（验证码应答）");
            return false;
        }
        proto::AuthMsg auth;
        if (!proto::AuthMsg::parse(payload.data(), payload.size(), auth)) {
            err = "Auth 消息格式错误";
            reject("Auth 消息格式错误");
            return false;
        }

        u8 key[32];
        crypt::derive_key(cfg.password, key);
        u8 expect[proto::kProofLen];
        crypt::compute_proof(key, hello.nonce, chal.serverNonce, chal.salt, expect);

        proto::AuthResultMsg ar;
        const bool ok = crypt::constant_time_equal(expect, auth.proof, proto::kProofLen);
        if (ok) {
            u32 sid = 0;
            crypt::random_bytes(&sid, sizeof(sid));
            if (sid == 0) sid = 1;
            ar.ok = 1;
            ar.sessionId = sid;
            ar.message = "ok";
            sidOut = sid;
            helloOut = hello;
        } else {
            ar.ok = 0;
            ar.sessionId = 0;
            ar.message = u8"验证码错误";
        }
        control.send_frame(proto::MsgType::AuthResult, ar.serialize());

        if (!ok) {
            err = "验证码错误";
            return false;
        }
        return true;
    }

    // 线程函数一律不许把异常抛出去：线程里逃逸的异常**不会**走
    // std::set_terminate（MSVC 走 __std_terminate -> abort），
    // 进程会直接以 0xC0000409 消失，连日志都留不下。
    void input_loop() {
        // MMCSS：输入注入直接决定"鼠标卡不卡"，用 Pro Audio 拿最短调度周期
        const ms::PerfThreadScope perf(L"Pro Audio");
        try {
            input_loop_body();
        } catch (const std::exception& e) {
            MS_ERR("输入线程异常: %s（已抑制，服务继续）", e.what());
        } catch (...) {
            MS_ERR("输入线程未知异常（已抑制，服务继续）");
        }
    }

    // ---- 输入线程 ----
    // v2：UDP 输入包 + TCP InputBatch 都进 inBatchQueue，由本线程串行注入。
    // UDP 包在收包循环里解码后直接入队（保持到达顺序），这里统一消费。
    void input_loop_body() {
        std::vector<u8> buf(2048);
        u64 lastPeerCheck = 0;
        sockaddr_in altPeer{};   // 候选的新来源地址（漂移检测）
        int altStreak = 0;       // 同一新来源连续出现次数
        (void)lastPeerCheck;

        // ---- UDP 输入包整包去重 ----
        // 远控端在「视频走 TCP」时会把键鼠改走 UDP，并且每个包连发 3 份抗丢包。
        // 三份内容完全相同、UdpHeader.id 也相同，这里按 id 整包去重，
        // 否则滚轮会滚 3 格、相对鼠标位移会跳 3 倍（注入端只按事件 seq 去重，
        // 一个包里多个事件时挡不住第二、三份）。
        // 【协议约定】UdpHeader.id = 输入包序号：会话内单调递增、每个**包** +1，
        // 同一个包的 N 份副本共用同一个 id；不要用 0（0 表示对端还没填序号）。
        // 64 个包的窗口足够覆盖「三份之间的乱序/延迟」，又不占多少内存。
        static constexpr u32 kSeenCap = 64;
        u32 seenIds[kSeenCap] = {};
        u32 seenPos = 0;      // 环形写指针：满 64 个之后每次覆盖最旧的一个
        u32 seenCount = 0;    // 已记录的有效个数（<= kSeenCap，用来决定扫描范围）
        u64 dupDropped = 0;

        // 【必须检查 sessionActive！】
        // 之前这里只有 running/stopReq，于是会话结束后输入线程永远不退，
        // join_session_threads() 里 join(thInput) 永久阻塞 —— 被控端再也
        // 回不到监听状态（TCP 端口消失），远控端重连就只能看到黑屏/连接失败。
        // 只有点"停止服务"（stopReq）才会退，所以自测的启停全过、真机连一次就废。
        while (running.load() && !stopReq.load() && sessionActive.load()) {
            // ---- 1. 先消费 TCP 批次（可靠路径优先；单线程注入点）----
            InputBatchItem item;
            while (pop_input_batch(item)) {
                if (item.events && item.count > 0) {
                    u32 applied = 0;
                    u8 status = 0;
                    if (cfg.inputEnabled) {
                        injector.apply(item.events, item.count, &applied);
                        if (applied < item.count) status = 1;   // 部分提交（UIPI 等）
                    } else {
                        status = 2;   // 输入被禁用
                    }
                    if (item.fromChannel != 0) {
                        send_input_applied(item.fromChannel, item.batchId, applied, status);
                    }
                }
                if (item.events) delete[] item.events;
            }

            // ---- 2. UDP 收包（非阻塞）----
            sockaddr_in from{};
            const int n = net::udp_recv_from(udpSock, buf.data(), buf.size(), from);
            if (n < 0) {
                const int e = ::WSAGetLastError();
                if (e == WSAEWOULDBLOCK) { sleep_ms(1); continue; }
                MS_WARN("UDP 接收错误 %d", e);
                sleep_ms(5);
                continue;
            }
            if (n < int(sizeof(proto::UdpHeader))) continue;

            proto::UdpHeader h{};
            std::memcpy(&h, buf.data(), sizeof(h));
            if (h.sessionId != sessionId.load()) continue;
            if (size_t(n) < sizeof(h) + h.payloadSize) continue;

            const u8* pl = buf.data() + sizeof(h);

            // 学习/校验来源
            sockaddr_in known{};
            const bool known_ok = get_udp_peer(known);
            if (!known_ok) {
                set_udp_peer(from);
                MS_INFO("学到远控端 UDP 地址: %s:%u", net::addr_str(from).c_str(), net::addr_port(from));
            } else if (!net::addr_same(known, from)) {
                // NAT / FRP 漂移：**必须尽快跟随**。
                // 走 FRP 时 frpc 的本地 UDP 源端口会随会话重建而变；如果我们还往
                // 旧端口发视频，就是一整场黑屏。原来的策略是"连续 200 个包才切"，
                // 按 2 秒一个保活包算要 400 秒 —— 等于永远不切。现在同一新来源
                // 连续 4 个包就切（视频丢几包无所谓，切过去立刻请求关键帧）。
                if (altStreak == 0 || !net::addr_same(from, altPeer)) {
                    altPeer = from;
                    altStreak = 1;
                } else {
                    ++altStreak;
                }
                if (altStreak >= 4) {
                    set_udp_peer(from);
                    altStreak = 0;
                    MS_WARN("远控端 UDP 地址漂移 -> %s:%u（已跟随）",
                            net::addr_str(from).c_str(), net::addr_port(from));
                    keyframePending.store(true);   // 换了目标，立刻补一个 IDR
                }
                continue;
            }

            switch (proto::UdpType(h.type)) {
                case proto::UdpType::Input: {
                    if (!cfg.inputEnabled) break;
                    // 整包去重：同一 id 的冗余副本（远控端连发 3 份）只注入一次。
                    // u32 回绕无害：只比较最近 64 个包内的相等。
                    // h.id == 0 表示对端还没填包序号（老客户端）：不做整包去重，
                    // 退回原有的「事件级 seq 去重」，否则会把整条 UDP 输入误杀。
                    if (h.id != 0) {
                        bool dup = false;
                        for (u32 k = 0; k < seenCount; ++k) {
                            if (seenIds[k] == h.id) { dup = true; break; }
                        }
                        if (dup) {
                            // 每个重复包都打日志会把文件刷爆：每 200 次报一行
                            if ((++dupDropped % 200) == 1) {
                                MS_DEBUG("UDP 输入包去重：累计丢弃 %llu 份冗余副本（最近 id=%u）",
                                         (unsigned long long)dupDropped, h.id);
                            }
                            break;
                        }
                        // 环形写入：写指针循环前进；满 64 个后自然挤出最旧的一个。
                        // 【别用 seenCount 取模】seenCount 到 64 就不再增长，取模会让
                        // 写指针永远停在 0 —— 环退化成「1 个槽」，就失去窗口意义了。
                        seenIds[seenPos] = h.id;
                        seenPos = (seenPos + 1) % kSeenCap;
                        if (seenCount < kSeenCap) ++seenCount;
                    }
                    // 解码后入队（与 TCP 批次同队列串行注入，保证全局顺序）
                    auto evs = std::make_unique<proto::InputEvent[]>(
                        proto::kMaxInputBatchEvents);
                    size_t cnt = 0;
                    if (proto::decode_input_packet(pl, h.payloadSize, evs.get(),
                                                   proto::kMaxInputBatchEvents, cnt)) {
                        std::lock_guard<std::mutex> lk(inBatchMtx);
                        inBatchQueue.push_back(
                            InputBatchItem{0, 0, u32(cnt), evs.release()});
                    }
                    break;
                }
                case proto::UdpType::Feedback: {
                    proto::FeedbackMsg fb;
                    if (proto::FeedbackMsg::parse(pl, h.payloadSize, fb)) {
                        // UDP 是反馈的主路径（丢了也无所谓，下一包会补齐）。
                        // 这里只做「丢帧率 -> FEC 组大小」自适应，统计展示仍以
                        // TCP 那份为准（见 accept_loop_body 的 Feedback 分支）。
                        on_video_feedback(fb);
                    }
                    break;
                }
                case proto::UdpType::Nack: {
                    // 缺片重传请求（[u16 count][u16 idx...]）：
                    // 依据 UU远程/GameViewer 逆向报告，NACK 是丢包恢复的主力，
                    // FEC 只提供恒定打底。这里只解析入队，真正补发在串流线程。
                    if (h.payloadSize < 2 || size_t(h.payloadSize) > 2 + 64 * 2) break;
                    const u16 nackCnt = u16(pl[0]) | (u16(pl[1]) << 8);
                    if (nackCnt == 0 || size_t(2 + nackCnt * 2) > h.payloadSize) break;
                    NackReq req;
                    req.frameId = h.id;
                    for (u16 i = 0; i < nackCnt && req.count < 32; ++i) {
                        req.indexes[req.count++] =
                            u16(pl[2 + i * 2]) | (u16(pl[2 + i * 2 + 1]) << 8);
                    }
                    if (req.count == 0) break;
                    std::lock_guard<std::mutex> lk(nackMtx);
                    if (nackQueue.size() < 64) nackQueue.push_back(req);
                    break;
                }
                case proto::UdpType::Keepalive:
                default:
                    break;
            }
        }

        // 会话结束：清空批次队列并抬起所有按键，避免"卡键"
        {
            std::lock_guard<std::mutex> lk(inBatchMtx);
            for (auto& it : inBatchQueue) {
                if (it.events) delete[] it.events;
            }
            inBatchQueue.clear();
        }
        {
            // 会话结束：丢掉没来得及重传的 NACK（帧号下个会话会重头开始）
            std::lock_guard<std::mutex> lk(nackMtx);
            nackQueue.clear();
        }
        injector.release_all();
    }

    // ---- 采集/编码/发送线程 ----
    void stream_loop(const proto::HelloMsg& hello) {
        // MMCSS：采集+编码+发送是全链路最延迟敏感的循环，注册为 Games 类别
        // （参考 UU远程 streamer.dll 导入 AvSetMmThreadCharacteristics 的做法）。
        // 失败时退回普通调度，不影响功能。
        const ms::PerfThreadScope perf(L"Games");
        try {
            stream_loop_body(hello);
        } catch (const std::exception& e) {
            MS_ERR("串流线程异常: %s（已抑制，会话结束）", e.what());
        } catch (...) {
            MS_ERR("串流线程未知异常（已抑制，会话结束）");
        }
        sessionActive.store(false);
    }

    void stream_loop_body(const proto::HelloMsg& hello) {
        std::string err;

        const u32 sid = sessionId.load();

        // 1) UDP 发送器。注意这一步**不**等远控端的保活包：
        //    远控端要等收到 StreamReady 才会发保活包，如果我们在这里先等它，
        //    就会互相等成死锁（实测：被控端等满 1.5s 退化用 TCP 端口发视频，
        //    视频全丢进黑洞）。所以顺序必须是：先建好采集/编码 -> 发 StreamReady
        //    -> 再等保活包学地址（见下面第 5 步）。
        VideoSender sender;
        if (!sender.init(udpSock, sid, err)) { set_error("初始化视频发送失败: " + err); return; }
        // 初始 FEC 组大小 = 协议默认值（10% 校验开销）。远控端从第一帧起就有
        // 抗丢包保护，之后再按它反馈的丢帧率自适应（见 on_video_feedback）。
        // set_fec_group 内部只是写一个 atomic，这里和 Feedback 线程并发调用也安全。
        int appliedFecGroup = fecGroupTarget.load(std::memory_order_relaxed);
        sender.set_fec_group(u16(appliedFecGroup));
        MS_INFO("视频 FEC：组大小 %d（约 %d%% 校验开销），将按远控端丢帧率自适应",
                appliedFecGroup, 100 / (appliedFecGroup > 0 ? appliedFecGroup : 1));

        // ---------------------------------------------------------------------------
        // 可重入的「建管线」步骤：初次连接和运行期重协商（分辨率/帧率变化）
        // 都走这里。failResumed=true 表示沿用上一轮的 frameId 继续编号。
        // ---------------------------------------------------------------------------
        ScreenCapture cap;
        VideoEncoder  enc;
        EncoderConfig ecfg;
        int outW = 0, outH = 0, curFps = 0;
        u32  curBr = 0;
        // v2：被采集显示器的虚拟桌面原点 + 物理像素尺寸（StreamReady 发给
        // 远控端做绝对鼠标换算；重协商时也可能变，所以放在可重入段里刷新）
        int monOriginX = 0, monOriginY = 0;
        int monPhysW = 0, monPhysH = 0;
        // 当前生效的目标尺寸/帧率（重协商请求可能改它们）
        int wantW = negotiatedW.load();
        int wantH = negotiatedH.load();
        int wantFps = negotiatedFps.load();
        // 当前编码格式（1=H.264, 2=HEVC）。可在会话中被远控端热切换
        // （画质面板「编码」下拉 → Reconfigure(codec) → 重建编码器）。
        // auto = 自动协商：远控端 Hello 里声明支持 HEVC 时才选 H.265。
        const bool clientSupportsHevc =
            (hello.flags & proto::kHelloFlagHevcCapable) != 0;
        int wantCodec;
        if (cfg.codec == "hevc" || cfg.codec == "h265") {
            wantCodec = int(proto::VideoCodec::HEVC);
        } else if (cfg.codec == "h264") {
            wantCodec = int(proto::VideoCodec::H264);
        } else {
            // auto：双方都支持才用 H.265，否则退回 H.264
            wantCodec = clientSupportsHevc ? int(proto::VideoCodec::HEVC)
                                           : int(proto::VideoCodec::H264);
        }
        // TCP 视频兜底模式的拥塞控制状态（见下面 TCP 发送那段注释）：
        //   tcpBehindUs  > 0 表示发送管道已经落后，需要丢帧追赶
        //   tcpSendMaxUs    统计窗口内最慢的一次整帧发送耗时（日志用）
        i64 tcpBehindUs = 0;
        i64 tcpSendMaxUs = 0;

        auto build_pipeline = [&](bool announce) -> bool {
            // 采集（wantW/wantH 为 0 表示原生分辨率；非 0 时在 GPU 上缩放）
            if (!cap.init(cfg.monitor, true, wantW, wantH, err)) {
                set_error("屏幕采集初始化失败: " + err);
                return false;
            }
            outW = cap.width();
            outH = cap.height();
            // v2：刷新显示器原点与物理尺寸（Desktop Duplication 输出即物理像素，
            // 所以桌面物理尺寸用原生分辨率，而不是可能被缩放过的输出分辨率）
            for (const auto& m : ScreenCapture::enumerate_monitors()) {
                if (m.index == cfg.monitor) {
                    monOriginX = m.left;
                    monOriginY = m.top;
                    monPhysW = m.width;
                    monPhysH = m.height;
                    break;
                }
            }

            ecfg = EncoderConfig{};
            ecfg.preferred = cfg.encoder;
            ecfg.codec = (wantCodec == int(proto::VideoCodec::HEVC))
                             ? proto::VideoCodec::HEVC
                             : proto::VideoCodec::H264;
            ecfg.width = outW;
            ecfg.height = outH;
            ecfg.fps = wantFps;
            // 码率：重协商后沿用现有值（SetBitrate 可继续热调）
            ecfg.bitrateKbps = curBr > 0 ? int(curBr)
                               : (negotiatedBitrate.load() > 0 ? negotiatedBitrate.load()
                                 : auto_bitrate_kbps(outW, outH, wantFps));
            ecfg.keyframeIntervalSec = cfg.keyframeIntervalSec;
            ecfg.minQp = cfg.minQp;
            ecfg.maxQp = cfg.maxQp;
            ecfg.slices = cfg.slices;
            ecfg.vbvMultiplier = cfg.vbvMultiplier;

            if (!enc.init(ecfg, err)) {
                set_error("编码器初始化失败: " + err);
                return false;
            }
            curFps = wantFps;
            curBr = u32(ecfg.bitrateKbps);
            MS_INFO("编码器: %s (%s), %s, %dx%d @%dfps",
                    enc.encoder_name().c_str(), enc.is_hardware() ? "硬件" : "软件",
                    wantCodec == int(proto::VideoCodec::HEVC) ? "H.265/HEVC" : "H.264",
                    outW, outH, wantFps);

            {
                std::lock_guard<std::mutex> lk(statMtx);
                status.encoderName = enc.encoder_name();
                status.hardwareEncoder = enc.is_hardware();
                status.width = outW;
                status.height = outH;
                status.fps = wantFps;
                status.bitrateKbps = int(curBr);
            }

            // 首次连接发 StreamReady，重协商发 Reconfigured（远控端同样处理）
            if (announce) {
                proto::StreamReadyMsg sr;
                sr.codec = ecfg.codec;
                sr.width = u16(outW);
                sr.height = u16(outH);
                sr.fps = u16(wantFps);
                sr.bitrateKbps = curBr;
                sr.monitorName = "display" + std::to_string(cfg.monitor);
                // v2：绝对鼠标换算信息（显示器原点 + 物理尺寸）
                sr.monitorOriginX = i32(monOriginX);
                sr.monitorOriginY = i32(monOriginY);
                sr.desktopPhysW = u16(monPhysW);
                sr.desktopPhysH = u16(monPhysH);
                control.send_frame(proto::MsgType::Reconfigured, sr.serialize());
            }
            return true;
        };

        if (!build_pipeline(false)) return;
        MS_INFO("采集: 显示器 %d, 原生 %dx%d -> 输出 %dx%d @%dfps %ukbps",
                cfg.monitor, cap.native_width(), cap.native_height(),
                outW, outH, wantFps, curBr);

        // 首次连接要发的是 StreamReady（不是 Reconfigured）
        {
            proto::StreamReadyMsg sr;
            sr.codec = ecfg.codec;
            sr.width = u16(outW);
            sr.height = u16(outH);
            sr.fps = u16(wantFps);
            sr.bitrateKbps = curBr;
            sr.monitorName = "display" + std::to_string(cfg.monitor);
            // v2：绝对鼠标换算信息（显示器原点 + 物理尺寸）
            sr.monitorOriginX = i32(monOriginX);
            sr.monitorOriginY = i32(monOriginY);
            sr.desktopPhysW = u16(monPhysW);
            sr.desktopPhysH = u16(monPhysH);
            control.send_frame(proto::MsgType::StreamReady, sr.serialize());
        }

        // 5) 现在远控端已经拿到 StreamReady，会开始发 UDP 保活包。
        //    这里等它出现以学到 NAT/FRP 之后的真实地址；超时就先用 TCP 对端地址兜底，
        //    主循环里还会持续自愈（学到新地址就切换发送目标）。
        {
            const i64 t0 = now_us();
            sockaddr_in peer{};
            while (running.load() && !stopReq.load() && (now_us() - t0) / 1000 < kUdpPeerLearnMs) {
                if (get_udp_peer(peer)) break;
                sleep_ms(5);
            }
            bool learned = get_udp_peer(peer);
            if (!learned) {
                if (controlPeerIp.empty() ||
                    !net::resolve_ipv4(controlPeerIp, controlPeerPort, peer, err)) {
                    set_error("无法确定远控端 UDP 地址");
                    return;
                }
                MS_WARN("未收到 UDP 保活包，暂用 TCP 对端地址 %s:%u（学到后会自愈）",
                        net::addr_str(peer).c_str(), net::addr_port(peer));
            } else {
                MS_INFO("视频发送目标: %s:%u", net::addr_str(peer).c_str(), net::addr_port(peer));
            }
            sender.set_peer(peer);
        }

        // 6) 主循环
        i64 slotUs = i64(1000000) / (wantFps > 0 ? wantFps : 60);
        int kfIntervalSec = cfg.keyframeIntervalSec;
        i64 lastEncodeUs = 0;
        i64 nextKeyframeUs = now_us() + i64(kfIntervalSec) * 1000000;
        i64 lastIdrUs = 0;            // 上次真正出 IDR 的时刻（600ms 最小间隔用）
        i64 lastPublishUs = 0;

        Nv12Frame frame;              // 当前帧
        Nv12Frame cached;             // 静态画面时用于重发
        u32 frameId = 0;

        // 帧率节流用：按「时间累加器」发牌，保证长期平均帧率就是目标帧率，
        // 同时永远发最新的那一帧（不会为了凑数去发旧帧）。
        i64 slotAccUs = 0;
        i64 lastTickUs = now_us();
        sockaddr_in lastSenderPeer{};   // 已设置的发送目标（用于自愈比较）
        {
            std::lock_guard<std::mutex> lk(peerMtx);
            if (udpPeerKnown.load()) lastSenderPeer = udpPeer;
        }

        RateCounter capRate, encRate;

        // ---- 音频：被控端系统声音（WASAPI loopback -> Opus）----
        // 失败不致命：视频/键鼠照常，只是没声音。
        AudioSender audio;
        std::vector<AudioSender::Packet> audioPkts;
        if (cfg.audioEnabled) {
            std::string aerr;
            if (!audio.init(cfg.audioBitrateKbps, aerr)) {
                MS_WARN("音频初始化失败，本次会话没有声音: %s", aerr.c_str());
            }
        } else {
            MS_INFO("音频已在配置里关闭（[audio] enabled=false）");
        }

        // ---- 光标发送状态 ---------------------------------------------------
        // Desktop Duplication 的桌面纹理里没有光标，必须单独发位置/形状。
        // 位置走在 UDP 上（跟手，顺带充当"下行 UDP 探针"），
        // 形状走 TCP（几 KB，只在光标形状变化时发，可靠性优先）。
        u32  pointerSeq   = 0;
        u32  sentShapeId  = 0;
        int  lastPtrX = 0, lastPtrY = 0;
        bool lastPtrVisible = false;
        bool haveLastPtr = false;
        i64  lastPtrSentUs = 0;

        // 自测钩子（真机验证用；正常运行时环境变量不存在）：
        //   MINISTREAM_TEST_CURSOR_FLAG=路径   文件存在 -> 强制上报"光标不可见"
        //   MINISTREAM_TEST_DROP_UDP_FLAG=路径 文件存在 -> 不发 UDP 视频（模拟 UDP 不通）
        const std::wstring testCursorFlag  = get_env_w(L"MINISTREAM_TEST_CURSOR_FLAG");
        const std::wstring testDropUdpFlag = get_env_w(L"MINISTREAM_TEST_DROP_UDP_FLAG");

        auto send_pointer_if_needed = [&]() {
            const PointerInfo& p = cap.pointer();
            if (!p.available) return;
            const i64 now = now_us();

            // 自测钩子：强制上报"光标不可见"，验证远控端会不会跟着隐藏
            const bool forceHidden = file_exists_w(testCursorFlag);

            // 1) 形状变了才发（一次性 4~16KB，正常一场会话就几次）
            if (p.shapeId != 0 && p.shapeId != sentShapeId) {
                proto::PointerShapeMsg sm;
                sm.shapeId = p.shapeId;
                sm.width  = u16(p.width);
                sm.height = u16(p.height);
                sm.hotX   = i16(p.hotX);
                sm.hotY   = i16(p.hotY);
                sm.bgra   = p.bgra;
                if (control.send_frame(proto::MsgType::PointerShape, sm.serialize())) {
                    sentShapeId = p.shapeId;
                    MS_INFO("光标形状已发给远控端: %dx%d 热点(%d,%d) shapeId=%u",
                            p.width, p.height, p.hotX, p.hotY, p.shapeId);
                }
            }

            // 2) 位置/可见性
            const bool visNow = p.visible && !forceHidden;
            const bool moved = !haveLastPtr || p.x != lastPtrX || p.y != lastPtrY ||
                               visNow != lastPtrVisible;
            // 鼠标 1000Hz 回报也没必要全发：125Hz 上限足够跟手
            const bool due = (moved && now - lastPtrSentUs > 8000) ||
                             (now - lastPtrSentUs > 500000);
            if (!due) return;

            proto::PointerPosMsg pm;
            pm.seq     = ++pointerSeq;
            pm.x       = i16(p.x);
            pm.y       = i16(p.y);
            pm.visible = visNow ? 1 : 0;
            sender.send_pointer(pm);                   // UDP：低延迟 + 下行探针
            if (videoTransport.load(std::memory_order_relaxed) != 0) {
                // TCP 模式下 UDP 可能不通：再走控制通道兜一份，保证光标不卡死
                control.send_frame(proto::MsgType::PointerPos, pm.serialize());
            }
            lastPtrX = p.x;
            lastPtrY = p.y;
            lastPtrVisible = visNow;
            haveLastPtr = true;
            lastPtrSentUs = now;
        };

        while (running.load() && !stopReq.load()) {
            if (!sessionActive.load()) break;

            // ---- NACK 重传：把远控端缺的分片从发送历史里补发 ----
            // 放在每轮最前面：越早补上，远控端越可能在组帧超时前收齐，
            // 避免"整帧丢弃 → 等 IDR"的冻结。
            {
                std::vector<NackReq> reqs;
                {
                    std::lock_guard<std::mutex> lk(nackMtx);
                    if (!nackQueue.empty()) reqs.swap(nackQueue);
                }
                u64 retx = 0;
                for (const auto& r : reqs) {
                    retx += u64(sender.resend(r.frameId, r.indexes, r.count));
                }
                if (retx > 0) {
                    static u64 retxTotal = 0;
                    static i64 lastRetxLogUs = 0;
                    retxTotal += retx;
                    if (now_us() - lastRetxLogUs > 2000000) {
                        lastRetxLogUs = now_us();
                        MS_DEBUG("NACK 重传：本窗口 %llu 包（累计 %llu）",
                                 (unsigned long long)retx, (unsigned long long)retxTotal);
                    }
                }
            }

            // ---- 自适应 FEC 组大小下发 ----
            // 目标值由 Feedback 处理线程根据丢帧率算好（fecGroupTarget），这里
            // 每帧查一次原子就够（60Hz 的开销可以忽略）。放在帧率节流之前，
            // 保证丢包率一变，下一帧的校验包就换了组大小。
            {
                const int gt = fecGroupTarget.load(std::memory_order_relaxed);
                if (gt != appliedFecGroup) {
                    appliedFecGroup = gt;
                    sender.set_fec_group(u16(gt));
                }
            }

            // ---- 运行期重协商：分辨率/帧率/编码格式变了，重建采集与编码器 ----
            if (reconfigPending.exchange(false)) {
                const int nw = reconfigW.load();
                const int nh = reconfigH.load();
                const int nf = reconfigFps.load();
                const int nc = reconfigCodec.load();
                MS_INFO("重协商 -> %dx%d @%dfps %s（原 %dx%d @%dfps %s）",
                        nw, nh, nf,
                        nc == int(proto::VideoCodec::HEVC) ? "H.265" : "H.264",
                        outW, outH, wantFps,
                        wantCodec == int(proto::VideoCodec::HEVC) ? "H.265" : "H.264");
                enc.shutdown();
                cap.shutdown();
                wantW = nw;
                wantH = nh;
                wantFps = nf;
                if (nc == int(proto::VideoCodec::H264) ||
                    nc == int(proto::VideoCodec::HEVC)) {
                    wantCodec = nc;
                }
                if (!build_pipeline(true)) break;   // 失败已在内部 set_error
                slotUs = i64(1000000) / (wantFps > 0 ? wantFps : 60);
                slotAccUs = 0;
                lastEncodeUs = 0;
                keyframePending.store(true);
                nextKeyframeUs = now_us() + i64(kfIntervalSec) * 1000000;
                // 分辨率变了，缓存的旧帧尺寸不对，清掉
                cached.data.clear();
                frame.data.clear();
                continue;
            }

            // ---- 关键帧间隔热调（不用重建任何东西）----
            if (keyframeIntervalSec.load() > 0) {
                const int ns = keyframeIntervalSec.exchange(0);
                if (ns != kfIntervalSec) {
                    kfIntervalSec = ns;
                    MS_INFO("关键帧间隔生效: %d 秒", ns);
                }
            }

            const i64 now = now_us();

            int r = cap.acquire_frame(int(slotUs / 1000), err);
            bool haveFrame = false;

            if (r < 0) {
                MS_WARN("采集异常: %s，尝试重新初始化", err.c_str());
                cap.shutdown();
                if (!cap.init(cfg.monitor, true, wantW, wantH, err)) {
                    set_error("采集重初始化失败: " + err);
                    break;
                }
                continue;
            }

            // 光标：位置/可见性/形状。放在帧率节流之前——桌面完全静止时也可能
            // 只有鼠标在动（此时 AcquireNextFrame 给的是 LastPresentTime==0 的帧），
            // 如果不在这里发，静止画面里光标就不动了。
            send_pointer_if_needed();

            // ---- 音频：采集 + 编码 + 发送 ------------------------------------
            // 20ms 一帧，编出来立刻发。UDP 发两份抗丢包（远端按 seq 去重）；
            // 视频走 TCP 兜底时再走控制通道一份，保证"切 TCP 时声音不断"。
            if (audio.active() && !audio.failed()) {
                audioPkts.clear();
                audio.poll(audioPkts);
                if (!audioPkts.empty()) {
                    const bool tcpMode =
                        videoTransport.load(std::memory_order_relaxed) != 0;
                    for (const auto& ap : audioPkts) {
                        sender.send_audio(ap.seq, ap.ts, ap.data.data(), ap.data.size(), 2);
                        if (tcpMode) {
                            const Bytes body = proto::encode_audio_packet(
                                ap.seq, ap.ts, ap.data.data(), ap.data.size());
                            control.send_frame(proto::MsgType::AudioPacket, body);
                        }
                    }
                }
            }

            if (r == 1) {
                // 帧率节流：桌面刷新率（常见 76~144Hz）通常高于目标帧率。
                // 累加器到点才编码；否则丢掉这帧并立刻取下一帧。
                slotAccUs += (now - lastTickUs);
                lastTickUs = now;
                if (slotAccUs < slotUs) {
                    cap.release_frame();
                    continue;
                }
                slotAccUs -= slotUs;
                if (slotAccUs > slotUs) slotAccUs = 0;   // 长时间没帧后不要攒出一串补发
                if (cap.to_nv12(frame, err)) {
                    haveFrame = true;
                    capRate.add(1);
                    cached = frame;      // 缓存一份，供静态画面重发关键帧
                } else {
                    MS_WARN("BGRA->NV12 失败: %s", err.c_str());
                }
                cap.release_frame();
                lastEncodeUs = now_us();
            } else {
                // 画面无变化：只有在等待关键帧时才重编码缓存帧。
                // 注意这里**不能**清 keyframePending：要留给下面的 IDR 调度去消费，
                // 否则"静止画面里请求 IDR"会变成重发一个 P 帧，远控端一直等不到 IDR。
                const bool keyPending = keyframePending.load();
                if (keyPending && !cached.data.empty() &&
                    (now_us() - lastEncodeUs) > i64(kStaticKeyframeMs) * 1000) {
                    frame = cached;
                    haveFrame = true;
                }
            }

            // ---- IDR 调度：按需为主，周期只做兜底 ----
            // 依据（UU远程/GameViewer 逆向报告）：周期 GOP 会造成每秒一次的码率锯齿，
            // 是"规律性卡顿"的常见根因；它们长稳会话 24 万帧只有 7 个 I 帧。
            // 我们默认 kfIntervalSec=0（无限 GOP），只在远控端请求/换发送目标时出 IDR，
            // 并用 600ms 最小间隔防止请求风暴（报告实测 min_keyframe_send_interval_ms=600）。
            const i64 nowKf = now_us();
            const bool periodicDue = (kfIntervalSec > 0) && (nowKf >= nextKeyframeUs);
            const bool wantIdr = keyframePending.load() || periodicDue;
            bool doIdr = false;
            if (wantIdr) {
                const i64 minGapUs = 600000;   // 600ms 内不重复出 IDR
                if (lastIdrUs == 0 || nowKf - lastIdrUs >= minGapUs) {
                    doIdr = true;
                    keyframePending.store(false);
                }
                // 未满 600ms 的请求保留 pending，下一帧/下一轮再执行（不丢，只是延后）
            }
            if (doIdr) {
                lastIdrUs = nowKf;
                if (kfIntervalSec > 0) nextKeyframeUs = nowKf + i64(kfIntervalSec) * 1000000;
            }
            const bool forceIdr = doIdr;

            // ---- TCP 兜底模式的「丢帧追赶」----------------------------------
            // 上一帧的发送耗时超过一帧间隔时，管道里已经积压了数据。这时再按满帧率
            // 编码发送，只会让积压越来越多（延迟越拖越大、越拖越卡）。
            // 主动丢掉这一帧（不编码、不发送），把积压还回去 —— 和 RustDesk 在
            // 带宽不足时「跳帧保延迟」的策略一致：实时性优先，宁可掉帧。
            if (!forceIdr && tcpBehindUs > 0 &&
                videoTransport.load(std::memory_order_relaxed) != 0) {
                tcpBehindUs -= slotUs;
                if (tcpBehindUs < 0) tcpBehindUs = 0;
                static i64 lastSkipLogUs = 0;
                if (now_us() - lastSkipLogUs > 2000000) {
                    lastSkipLogUs = now_us();
                    MS_DEBUG("TCP 视频管道积压 -> 本轮跳帧追赶（剩余 %lld us）",
                             (long long)tcpBehindUs);
                }
                continue;
            }

            if (haveFrame) {
                // 【重要】发给远控端的时间戳必须是「采集时的墙钟时刻」，
                // 不能用编码器返回的 pkt.ptsUs —— 后者是从帧序号按 1/fps 推出来的
                // 播放时间轴（0, 16666, 33333...），拿它算端到端延迟会算出负数，
                // 远控端悬浮条上就会一直显示 0.0 ms。
                const i64 frameWallUs = now_us();
                frame.captureTsUs = frameWallUs;

                const i64 submitT0 = now_us();
                const int sr = enc.submit(frame.y(), frame.strideY, frame.uv(), frame.strideUV,
                                          frameWallUs, forceIdr);
                if (sr < 0) {
                    set_error("编码提交失败");
                    break;
                }
                if (sr == 1) {
                    EncodedPacket pkt;
                    int pr;
                    while ((pr = enc.poll(pkt, 0)) == 1) {
                        if (pkt.data.empty()) continue;
                        const u32 fid = ++frameId;
                        const bool tcpMode =
                            videoTransport.load(std::memory_order_relaxed) != 0;
                        // 探测窗口内：TCP 模式也同时走一遍 UDP。远控端统计到达率，
                        // 够好就从 TCP 切回 UDP（低延迟"自愈"）。
                        const bool probeUdp = tcpMode &&
                            now_us() < udpProbeUntilUs.load(std::memory_order_relaxed);
                        if (tcpMode) {
                            // TCP 视频回退：整帧塞进控制通道（发送端已加锁）
                            //
                            // 【为什么这里要限速 + 丢帧】
                            // FRP/中继链路的带宽和缓冲都是有限的，而 TCP 本身没有
                            // "丢包"信号：码率一旦超过管道容量，数据只会堆在发送/中继
                            // 缓冲区里，端到端延迟线性上涨 —— 用户看到的就是"画面越来
                            // 越滞后、鼠标点下去半天才反应"。
                            // 所以 TCP 模式下：① 把码率压到链路能吃下的量级；
                            //              ② 发送耗时超过一帧间隔就主动丢后续帧
                            //                 （实时性优先，宁可掉帧也不排队）。
                            const i64 tSend0 = now_us();
                            const Bytes vf = proto::encode_tcp_video_frame(
                                fid, pkt.keyframe,
                                u32(u64(frameWallUs) & 0xFFFFFFFFu),
                                pkt.data.data(), pkt.data.size());
                            if (control.send_frame(proto::MsgType::VideoFrame, vf)) {
                                sender.note_tcp_frame(pkt.data.size());
                            } else {
                                set_error("TCP 视频帧发送失败（控制通道已断）");
                                break;
                            }
                            const i64 sendUs = now_us() - tSend0;
                            tcpSendMaxUs = std::max(tcpSendMaxUs, sendUs);
                            // 发送耗时超过 1.5 帧间隔 = 管道已经堵了：记一笔"落后"，
                            // 后面的非关键帧直接丢弃，直到追平（关键帧必须发，否则
                            // 对端会一直等 IDR 没画面）。
                            if (sendUs > i64(1.5 * double(slotUs))) {
                                tcpBehindUs += sendUs;
                                MS_DEBUG("TCP 视频发送慢: 本帧 %lld us（阈值 %lld us）",
                                         (long long)sendUs, (long long)(slotUs * 3 / 2));
                            }
                        }
                        if ((!tcpMode || probeUdp) && !file_exists_w(testDropUdpFlag)) {
                            sender.send_frame(fid, pkt.data.data(), pkt.data.size(),
                                              pkt.keyframe, frameWallUs);
                        }
                        pkt.encodeStartUs = submitT0;
                        encRate.add(1);
                        {
                            std::lock_guard<std::mutex> lk(statMtx);
                            status.encodeLatencyUs = pkt.encodeLatencyUs();
                        }
                    }
                    if (pr < 0) {
                        set_error("编码取出失败");
                        break;
                    }
                }
            }

            // 视频发送目标自愈：如果 input 线程后来学到了新的 UDP 来源
            // （例如启动时没等到保活包、先用了 TCP 对端地址兜底），这里切过去。
            // 不这么做的话，一旦退化就只能等下次重连才会好。
            {
                sockaddr_in cur{};
                bool have = false;
                {
                    std::lock_guard<std::mutex> lk(peerMtx);
                    if (udpPeerKnown.load()) { cur = udpPeer; have = true; }
                }
                if (have && !net::addr_same(cur, lastSenderPeer)) {
                    lastSenderPeer = cur;
                    sender.set_peer(cur);
                    MS_INFO("视频发送目标更新为 %s:%u（自愈）",
                            net::addr_str(cur).c_str(), net::addr_port(cur));
                    keyframePending.store(true);   // 换了目标，重新发 IDR 让对端立刻有画面
                }
            }

            // 码率动态调整
            // 【TCP 兜底模式的码率上限】FRP/中继的带宽与缓冲远小于局域网直连，
            // 而 TCP 没有丢包信号：码率超了只会把数据堆进缓冲区，延迟越拖越大。
            // 所以 TCP 模式下把目标码率压到 12 Mbps（1080p60 这个量级已经够清楚，
            // 再高在中继上只会变成"越看越滞后"）。切回 UDP 后远控端会重新协商，
            // 上限自然放开。
            {
                constexpr int kTcpVideoCapKbps = 12000;
                int wantBr = targetBitrate.load();
                if (wantBr == 0) wantBr = int(curBr);   // 0 = 不变
                if (videoTransport.load(std::memory_order_relaxed) != 0 &&
                    wantBr > kTcpVideoCapKbps) {
                    if (int(curBr) > kTcpVideoCapKbps) {
                        MS_INFO("TCP 传视频：码率压到上限 %d kbps（原 %u）",
                                kTcpVideoCapKbps, curBr);
                    }
                    wantBr = kTcpVideoCapKbps;
                }
                if (wantBr > 0 && wantBr != int(curBr)) {
                    if (enc.set_bitrate(wantBr)) {
                        curBr = u32(wantBr);
                        std::lock_guard<std::mutex> lk(statMtx);
                        status.bitrateKbps = wantBr;
                    }
                    targetBitrate.store(0);
                }
            }

            // 发送整形：默认关闭（回环实测初版整形参数会把延迟拉爆，见 video_sender.cpp
            // 里 pace() 的注释）。需要时用 MINISTREAM_PACE_KBPS=8000 打开做对照测试。
            {
                static const int paceKbpsEnv = [] {
                    wchar_t v[32]{};
                    if (::GetEnvironmentVariableW(L"MINISTREAM_PACE_KBPS", v, 32) > 0)
                        return _wtoi(v);
                    return 0;
                }();
                if (paceKbpsEnv > 0) sender.set_pace_bitrate(paceKbpsEnv);
            }

            // cwnd：每轮按最新 RTT 刷新拥塞窗口（Parsec/SCTP 的"在途上限"思路）
            if (const int rtt = lastRttMs.load(std::memory_order_relaxed); rtt > 0) {
                sender.update_cwnd(int(curBr), rtt);
            }

            // 状态上报
            if (now_us() - lastPublishUs > 500000) {
                lastPublishUs = now_us();
                {
                    std::lock_guard<std::mutex> lk(statMtx);
                    status.captureFps = capRate.rate();
                    status.encodeFps = encRate.rate();
                    status.sendKbps = sender.sent_kbps();
                    status.framesSent = sender.frames_sent();
                    status.framesDropped = sender.frames_dropped();
                }
                publish();
            }
        }

        MS_INFO("串流线程结束");
        audio.shutdown();
        enc.shutdown();
        cap.shutdown();
        (void)hello;
    }

    // stream_loop 里退化路径需要 TCP 对端地址
    std::string controlPeerIp;
    u16 controlPeerPort = 0;

    // ---- accept/控制线程 ----
    void accept_loop() {
        try {
            accept_loop_body();
        } catch (const std::exception& e) {
            set_error(std::string("accept 线程异常: ") + e.what());
        } catch (...) {
            set_error("accept 线程未知异常");
        }
        // accept 线程负责给会话线程收尾；stop() 只 join 它一个，
        // 所以不会出现两边抢着 join 同一个 std::thread 的情况。
        join_session_threads();
    }

    // ---- 备用线路（双线：键鼠走备线，主线留给视频）------------------------
    // 备用线路是独立的 TCP 连接（客户端连到 backup_port），只做口令校验，
    // 不建会话，收 InputBatch / Ping / Pong。键鼠不再和视频帧排同一条流。
    bool accept_backup_channel(proto::HelloMsg& helloOut, std::string& err) {
        proto::MsgType t;
        Bytes payload;
        int r = controlBackup.recv_frame(t, payload, 5000);
        if (r < 0 || r == 0 || t != proto::MsgType::Hello) {
            err = "备用线路未收到 Hello"; return false;
        }
        if (!proto::HelloMsg::parse(payload.data(), payload.size(), helloOut)) {
            err = "备用线路 Hello 格式错误"; return false;
        }
        if (!(helloOut.flags & proto::kHelloFlagBackupLine)) {
            err = "这条连接没标备用线路标志，拒绝"; return false;
        }
        proto::ChallengeMsg chal;
        crypt::random_bytes(chal.serverNonce, proto::kNonceLen);
        crypt::random_bytes(chal.salt, proto::kSaltLen);
        if (!controlBackup.send_frame(proto::MsgType::Challenge, chal.serialize())) {
            err = "备用线路 Challenge 发送失败"; return false;
        }
        r = controlBackup.recv_frame(t, payload, 5000);
        if (r < 0 || r == 0 || t != proto::MsgType::Auth) {
            err = "备用线路未收到 Auth"; return false;
        }
        proto::AuthMsg auth;
        if (!proto::AuthMsg::parse(payload.data(), payload.size(), auth)) {
            err = "备用线路 Auth 格式错误"; return false;
        }
        u8 key[32];
        crypt::derive_key(cfg.password, key);
        u8 expect[proto::kProofLen];
        crypt::compute_proof(key, helloOut.nonce, chal.serverNonce, chal.salt, expect);
        proto::AuthResultMsg ar;
        const bool ok = crypt::constant_time_equal(expect, auth.proof, proto::kProofLen);
        ar.ok = ok ? 1 : 0;
        ar.sessionId = ok ? 1 : 0;
        ar.message = ok ? "backup line ok" : "验证码错误";
        controlBackup.send_frame(proto::MsgType::AuthResult, ar.serialize());
        if (!ok) { err = "备用线路验证码错误"; return false; }
        backupActive.store(true);
        MS_INFO("备用线路已接入（%s）", controlBackup.peer_str().c_str());
        return true;
    }

    // 备用线路的 accept 循环
    void accept_backup_loop() {
        if (cfg.backupPort == 0) return;
        while (running.load() && !stopReq.load()) {
            std::string err;
            if (!controlBackup.listen(cfg.bind, cfg.backupPort, err)) {
                sleep_ms(1000);
                continue;
            }
            MS_INFO("备用线路监听 %s:%u（只传键鼠/控制）", cfg.bind.c_str(), cfg.backupPort);
            while (running.load() && !stopReq.load()) {
                std::string aerr;
                if (controlBackup.accept(500, aerr)) break;
                if (aerr.find("超时") == std::string::npos &&
                    aerr.find("10038") == std::string::npos &&
                    aerr.find("尚未监听") == std::string::npos) {
                    MS_WARN("备用线路 accept: %s", aerr.c_str());
                    sleep_ms(200);
                    break;
                }
            }
            if (!controlBackup.is_open()) continue;

            proto::HelloMsg hello;
            if (!accept_backup_channel(hello, err)) {
                MS_WARN("备用线路接入失败（%s），断开", err.c_str());
                controlBackup.close();
                continue;
            }

            i64 lastRecv = now_us();
            while (running.load() && !stopReq.load() && backupActive.load()) {
                proto::MsgType t;
                Bytes payload;
                const int r = controlBackup.recv_frame(t, payload, 200);
                if (r < 0) { MS_INFO("备用线路断开"); backupActive.store(false); break; }
                if (r > 0) {
                    lastRecv = now_us();
                    if (t == proto::MsgType::InputBatch) {
                        proto::InputBatchView view;
                        auto evs = std::make_unique<proto::InputEvent[]>(proto::kMaxInputBatchEvents);
                        if (proto::parse_input_batch(payload.data(), payload.size(),
                                                     evs.get(), proto::kMaxInputBatchEvents, view)) {
                            queue_input_batch(view.batchId, 2, evs.release(), view.count);
                        }
                    } else if (t == proto::MsgType::Ping) {
                        proto::PingMsg pm;
                        if (proto::PingMsg::parse(payload.data(), payload.size(), pm)) {
                            proto::PongMsg po;
                            po.seq = pm.seq; po.t1 = pm.t1; po.t2 = now_us(); po.t3 = now_us();
                            controlBackup.send_frame(proto::MsgType::Pong, po.serialize());
                        }
                    }
                }
                // 【P0 修复】备线空闲不是错误："60 秒不点鼠标"是正常状态，
                // 8 秒无数据就踢掉会导致下一次点击时备线刚好断开（第一次点击丢失）。
                // 改为：空闲 >=5 秒由被控端主动发 Ping 探活；25 秒无任何接收才算死。
                if (now_us() - lastRecv >= 5000000) {
                    proto::PingMsg pm;
                    pm.seq = 0; pm.t1 = now_us();
                    if (!controlBackup.send_frame(proto::MsgType::Ping, pm.serialize())) {
                        MS_INFO("备线探活发送失败，判定断开");
                        backupActive.store(false);
                        break;
                    }
                    lastRecv = now_us() - 4500000;   // 5 秒后再探
                }
                if (now_us() - lastRecv > 25000000) {
                    MS_INFO("备线 25 秒无任何接收，判定断开");
                    backupActive.store(false);
                    break;
                }
            }
            controlBackup.close();
        }
    }

    void accept_loop_body() {
        while (running.load() && !stopReq.load()) {
            std::string err;

            if (!control.listen(cfg.bind, cfg.port, err)) {
                std::string msg = "监听失败: " + err;
                if (err.find("10048") != std::string::npos) {
                    msg += "（端口被占用：可能已经有一个被控端在运行，或端口被别的程序占着）";
                }
                // 只在上报内容变化时记一次，避免每秒刷屏
                if (msg != lastListenError) {
                    lastListenError = msg;
                    set_error(msg);
                }
                sleep_ms(1000);
                continue;
            }
            lastListenError.clear();
            {
                std::lock_guard<std::mutex> lk(statMtx);
                status.listening = true;
                status.clientConnected = false;
                status.clientAddr.clear();
            }
            publish();
            MS_INFO("正在监听 %s:%u (TCP+UDP)", cfg.bind.c_str(), cfg.port);

            // 等待客户端接入
            bool got = false;
            while (running.load() && !stopReq.load()) {
                std::string aerr;
                if (control.accept(500, aerr)) { got = true; break; }
                if (aerr.find("超时") == std::string::npos) {
                    // WSAENOTSOCK(10038)：监听套接字被 stop() 关掉了。
                    // 这是正常收尾路径，不是错误，日志里别吓人。
                    if (aerr.find("10038") != std::string::npos ||
                        aerr.find("尚未监听") != std::string::npos) {
                        MS_INFO("监听套接字已关闭（服务停止中）");
                    } else {
                        MS_WARN("accept: %s", aerr.c_str());
                    }
                    sleep_ms(200);
                    break;   // 重新 listen
                }
            }
            if (!got) continue;
            if (stopReq.load()) break;

            controlPeer = control.peer_str();
            controlPeerPort = control.peer_port();
            {
                const size_t colon = controlPeer.rfind(':');
                controlPeerIp = (colon == std::string::npos) ? controlPeer
                                                             : controlPeer.substr(0, colon);
            }
            MS_INFO("客户端已连接: %s", controlPeer.c_str());
            {
                std::lock_guard<std::mutex> lk(statMtx);
                status.clientAddr = controlPeer;
            }
            publish();

            // 握手 + 口令校验
            u32 sid = 0;
            proto::HelloMsg hello;
            if (!handshake(sid, hello, err)) {
                MS_WARN("握手失败（%s），断开 %s", err.c_str(), controlPeer.c_str());
                set_error("握手失败: " + err);
                control.close();
                continue;
            }
            MS_INFO("口令校验通过, session=0x%08X", sid);

            // 【多网卡关键修复】会话开始前把 UDP 绑到客户端连入的那块本机 IP，
            // 否则回包源地址由路由决定，FRP 的 connected UDP 套接字会丢弃视频包。
            bind_udp_to_control_local_ip();

            // 计算协商参数
            int fps = hello.wantFps ? hello.wantFps : cfg.fps;
            if (fps != 30 && fps != 60 && fps != 120 && fps != 144 && fps != 240) fps = cfg.fps;
            int outW = hello.wantWidth ? hello.wantWidth : cfg.width;
            int outH = hello.wantHeight ? hello.wantHeight : cfg.height;
            int br = hello.wantBitrateKbps ? int(hello.wantBitrateKbps) : cfg.bitrateKbps;
            if (br <= 0) br = auto_bitrate_kbps(outW ? outW : 1920, outH ? outH : 1080, fps);

            sessionId.store(sid);
            negotiatedW.store(outW);
            negotiatedH.store(outH);
            negotiatedFps.store(fps);
            negotiatedBitrate.store(br);
            targetBitrate.store(0);
            keyframePending.store(false);
            videoTransport.store(0);      // 每个会话默认走 UDP，远控端可以请求切 TCP
            udpProbeUntilUs.store(0);
            // FEC 自适应状态也要按会话重置：远控端的帧计数是从本会话重新累计的，
            // 沿用上个会话的基准会被判成"计数倒退"。
            reset_fec_adapt();
            {
                std::lock_guard<std::mutex> lk(peerMtx);
                udpPeerKnown.store(false);
            }
            injector.set_screen_size(outW ? outW : 1920, outH ? outH : 1080);
            // 绝对鼠标映射要用的显示器原点（多显示器时虚拟桌面坐标不是从 0 开始）
            {
                int ox = 0, oy = 0;
                const auto mons = ScreenCapture::enumerate_monitors();
                for (const auto& m : mons) {
                    if (m.index == cfg.monitor) { ox = m.left; oy = m.top; break; }
                }
                injector.set_monitor_origin(ox, oy);
                injector.set_relative_mode(true);   // 每个会话默认相对；远端发绝对坐标时按事件标志切换
            }

            sessionActive.store(true);
            {
                std::lock_guard<std::mutex> lk(statMtx);
                status.clientConnected = true;
                // 丢帧数是本会话的累加值（客户端每包只发增量），新会话从 0 起
                status.clientFramesLost = 0;
            }
            publish();

            // 起串流与输入线程
            spawn_session_threads(hello);

            // 控制循环：心跳/关键帧请求/码率调整/断线检测
            i64 lastRecv = now_us();
            while (running.load() && !stopReq.load() && sessionActive.load()) {
                proto::MsgType t;
                Bytes payload;
                const int r = control.recv_frame(t, payload, 200);
                if (r < 0) { MS_INFO("控制通道断开"); break; }
                if (r > 0) {
                    lastRecv = now_us();
                    switch (t) {
                        case proto::MsgType::Ping: {
                            proto::PingMsg pm;
                            if (proto::PingMsg::parse(payload.data(), payload.size(), pm)) {
                                proto::PongMsg po;
                                po.seq = pm.seq;
                                po.t1 = pm.t1;
                                po.t2 = now_us();          // 本机收到时刻
                                po.t3 = now_us();          // 本机发出时刻
                                // 【排障关键】Pong 发送失败必须打日志：
                                // 心跳超时的根因要么是 Pong 没发出去（这里），
                                // 要么是客户端没收到（网络）。两边日志一对就知道。
                                if (!control.send_frame(proto::MsgType::Pong, po.serialize())) {
                                    MS_WARN("Pong 发送失败（seq=%u），客户端会心跳超时", pm.seq);
                                }
                            }
                            break;
                        }
                        case proto::MsgType::KeyframeReq:
                            keyframePending.store(true);
                            break;
                        case proto::MsgType::Feedback: {
                            // 远控端的实时反馈（统计）。UDP 通道也会收一份，
                            // 这里收 TCP 版本是为了在 UDP 被 FRP 限流时仍有数据。
                            proto::FeedbackMsg fb;
                            if (proto::FeedbackMsg::parse(payload.data(), payload.size(), fb)) {
                                {
                                    std::lock_guard<std::mutex> lk(statMtx);
                                    status.clientRecvKbps = fb.recvKbps;
                                    // framesLost 现在是「上次反馈以来的增量」，累加成本
                                    // 会话总数 —— 否则 UI 上平时几乎总是 0，看不出丢包。
                                    status.clientFramesLost += fb.framesLost;
                                    status.clientLatencyUs = fb.latencyUs;
                                }
                                // 同一条自适应逻辑：UDP 反馈断了的时候，FEC 组大小
                                // 至少还能跟着 TCP 这条路走。
                                on_video_feedback(fb);
                            }
                            break;
                        }
                        case proto::MsgType::SetBitrate: {
                            proto::SetBitrateMsg sm;
                            if (proto::SetBitrateMsg::parse(payload.data(), payload.size(), sm))
                                targetBitrate.store(int(sm.kbps));
                            break;
                        }
                        case proto::MsgType::SetKeyframeInterval: {
                            proto::SetKeyframeIntervalMsg ki;
                            if (proto::SetKeyframeIntervalMsg::parse(payload.data(), payload.size(), ki)) {
                                int s = int(ki.seconds);
                                if (s < 0) s = 0;      // 0 = 按需（无限 GOP + 按需 IDR）
                                if (s > 10) s = 10;
                                keyframeIntervalSec.store(s);
                                if (s == 0) {
                                    MS_INFO("关键帧间隔热调为「按需」（不再周期性出 IDR）");
                                } else {
                                    MS_INFO("关键帧间隔热调为 %d 秒", s);
                                }
                            }
                            break;
                        }
                        case proto::MsgType::Reconfigure: {                            proto::ReconfigureMsg rc;
                            if (proto::ReconfigureMsg::parse(payload.data(), payload.size(), rc)) {
                                int fps = int(rc.fps);
                                if (fps != 30 && fps != 60 && fps != 120 && fps != 144 && fps != 240)
                                    fps = negotiatedFps.load();
                                reconfigW.store(int(rc.width));
                                reconfigH.store(int(rc.height));
                                reconfigFps.store(fps);
                                reconfigCodec.store(int(rc.codec));
                                reconfigPending.store(true);
                                MS_INFO("收到重协商请求: %dx%d @%dfps codec=%u",
                                        int(rc.width), int(rc.height), fps,
                                        unsigned(rc.codec));
                            }
                            break;
                        }
                        case proto::MsgType::VideoOverTcp: {
                            // 传输模式/探测请求。空 body 的老格式 = 切 TCP。
                            proto::TransportModeMsg tm;
                            if (!proto::TransportModeMsg::parse(payload.data(), payload.size(), tm)) {
                                tm = proto::TransportModeMsg{};
                                tm.mode = 1;
                            }
                            if (tm.probe) {
                                // 探测：接下来 1.2 秒视频同时走一遍 UDP，
                                // 客户端统计到达率，够好就自己切回 UDP。
                                // 窗口从 0.9 秒放宽到 1.2 秒：1080p60 下 0.9 秒
                                // 约 54 帧，扣掉抖动/重排后样本偏少，容易误判成
                                // "UDP 还是不行"。1.2 秒约 72 帧，判定稳一些。
                                // 【必须与远控端一致】客户端按同一个窗口统计。
                                udpProbeUntilUs.store(now_us() + 1200000);
                                MS_INFO("远控端请求 UDP 探测：1.2 秒窗口内同时用 UDP 发视频");
                            } else {
                                const int want = tm.mode ? 1 : 0;
                                const int old = videoTransport.exchange(want);
                                if (old != want) {
                                    // 换路径后客户端解码链要重新对齐，补一个 IDR
                                    keyframePending.store(true);
                                    if (want) {
                                        MS_WARN("远控端要求改用 TCP 传视频（UDP 不通），已切换");
                                    } else {
                                        MS_INFO("远控端确认 UDP 已恢复，切回 UDP 传视频");
                                    }
                                }
                            }
                            break;
                        }
                        case proto::MsgType::InputBatch: {
                            // ---- v2 可靠输入（TCP 控制通道）----
                            // 诊断方案第一阶段：点击/按键走可靠传输。
                            // 这里只解析 + 投递到注入器；注入本身在输入线程串行
                            // 执行（injector 的 apply 与 UDP 路径共用同一实例，
                            // 状态表/批处理不被两个线程并发打）。
                            if (!cfg.inputEnabled) {
                                // 明确拒绝：客户端据此区分"未送达"和"被禁用"
                                // batchId 在 payload 头 4 字节（小端）
                                u32 bid = 0;
                                if (payload.size() >= 4) {
                                    bid = u32(payload[0]) | (u32(payload[1]) << 8) |
                                          (u32(payload[2]) << 16) | (u32(payload[3]) << 24);
                                }
                                control.send_frame(proto::MsgType::InputApplied,
                                                   proto::encode_input_applied(bid, 0, 2));
                                break;
                            }
                            proto::InputBatchView view;
                            // 存储放堆上：批次最大 128 个事件 * 12B，栈也放得下，
                            // 但注入线程异步消费，必须转移所有权
                            auto evs = std::make_unique<proto::InputEvent[]>(
                                proto::kMaxInputBatchEvents);
                            if (proto::parse_input_batch(payload.data(), payload.size(),
                                                         evs.get(), proto::kMaxInputBatchEvents,
                                                         view)) {
                                // 回执在注入完成后由注入线程发（InputApplied 表示
                                // "已提交系统"，不是"已收到"）—— 见 input_done_cb
                                queue_input_batch(view.batchId, 1, evs.release(), view.count);
                            } else {
                                MS_WARN("InputBatch 解析失败（%u 字节）", unsigned(payload.size()));
                            }
                            break;
                        }
                        case proto::MsgType::Disconnect:
                            MS_INFO("客户端主动断开");
                            sessionActive.store(false);
                            break;
                        default:
                            break;
                    }
                }
                // 心跳超时：20 秒没收到任何控制消息才认为掉线。
                // 取值理由（实测）：远控端在部分机器上（显卡驱动/安全软件/输入法
                // 注入多）启动会话时会出现**整进程停顿 5~10 秒**（CPU 时间不动），
                // 期间一个 Ping 都发不出来。3 秒/8 秒都会把这种暂时卡顿误判成掉线，
                // 表现就是"连上黑屏几秒然后断开"。宁可晚点发现真掉线，也别误杀。
                if (now_us() - lastRecv > 20000000) {
                    MS_WARN("控制通道心跳超时，断开客户端");
                    // 告诉客户端为什么掉线（它在收到后会显示在屏幕上/写进日志）
                    proto::DisconnectMsg dm;
                    dm.code = 1;
                    dm.reason = "被控端 20 秒没收到你的消息（远控端进程卡死或网络中断）";
                    control.send_frame(proto::MsgType::Disconnect, dm.serialize());
                    break;
                }
                // 串流线程自己退出了也要收尾
                if (!sessionActive.load()) break;
            }

            // 收尾
            sessionActive.store(false);
            join_session_threads();

            // 会话结束：恢复通配绑定，下一轮客户端从哪块网卡连进来都能接
            restore_udp_wildcard();

            injector.release_all();
            control.close();
            {
                std::lock_guard<std::mutex> lk(statMtx);
                status.clientConnected = false;
                status.clientAddr.clear();
                status.sendKbps = 0;
                status.encodeFps = 0;
                status.captureFps = 0;
            }
            publish();
            MS_INFO("会话结束，回到监听状态");
        }

        // =====================================================================
        // 【真实故障现场，务必别改回去】
        // 这里原来把 publish() 写在了 statMtx 的作用域里面，而 publish()
        // 自己会再锁一次同一把 std::mutex。UCRT 的 mutex 是非递归的，
        // 同一线程重复加锁会被判定为"必然死锁"，std::mutex::lock() 于是抛
        // std::system_error(_RESOURCE_DEADLOCK_WOULD_OCCUR)；accept 线程里
        // 没人接这个异常 -> __std_terminate -> abort，进程以 0xC0000409 消失。
        //
        // 触发条件就是"accept 线程退出外层循环"，也就是**停止服务 / 退出程序**
        // 的时候：用户看到的现象是"连上之后黑屏一下，然后闪退"。
        // =====================================================================
        {
            std::lock_guard<std::mutex> lk(statMtx);
            status.listening = false;
            status.clientConnected = false;
            status.clientAddr.clear();
        }
        publish();   // 必须在锁外面调用
    }
};

// -----------------------------------------------------------------------------
//  HostService
// -----------------------------------------------------------------------------
HostService::HostService() : impl_(new Impl()) {}
HostService::~HostService() { stop(); }

bool HostService::start(const HostConfig& cfg, std::string& err) {
    if (!impl_) { err = "内部错误"; return false; }
    // 串行化：stop() 正在跑时这里会等它收尾完，不会撞进"半停止"状态
    std::lock_guard<std::mutex> serial(impl_->stopMtx);
    if (impl_->started) { err = "服务已在运行"; return false; }

    if (!cfg.validate(err)) return false;
    if (!net::init()) { err = "WSAStartup 失败"; return false; }

    impl_->cfg = cfg;
    impl_->stopReq.store(false);
    impl_->lastListenError.clear();

    // 自测钩子：禁用输入注入。真机 UI 自动化测试时会点击远端画面，
    // 不希望这些点击真的落到本机桌面上（MINISTREAM_TEST_DISABLE_INPUT=1）。
    {
        wchar_t v[8]{};
        if (::GetEnvironmentVariableW(L"MINISTREAM_TEST_DISABLE_INPUT", v, 8) > 0 && v[0] == L'1') {
            impl_->cfg.inputEnabled = false;
            MS_WARN("[自测] 已禁用输入注入（MINISTREAM_TEST_DISABLE_INPUT=1）");
        }
    }

    // UDP 与 TCP 同端口
    impl_->udpSock = net::udp_open(cfg.bind, cfg.port, err);
    if (impl_->udpSock < 0) {
        net::shutdown();
        return false;
    }
    // 收包循环按非阻塞写的，这里就把套接字设成非阻塞（别依赖视频发送器稍后再设）
    net::udp_set_nonblocking(impl_->udpSock, true);

    impl_->injector.release_all();
    impl_->running.store(true);
    impl_->started = true;
    Impl::join_thread(impl_->thAccept, impl_->lifeMtx);   // 防御：上一次必须已回收
    impl_->thAccept = std::thread([this] { impl_->accept_loop(); });

    // 备用线路（双线）：配了 backup_port 就起一个副 accept 线程
    if (impl_->cfg.backupPort != 0) {
        Impl::join_thread(impl_->thAcceptBackup, impl_->lifeMtx);
        impl_->thAcceptBackup = std::thread([this] { impl_->accept_backup_loop(); });
        MS_INFO("双线模式：备用线路端口 %u 已启用（键鼠/控制走备线，主线全留给视频）",
                impl_->cfg.backupPort);
    }
    return true;
}

void HostService::stop() {
    if (!impl_) return;
    // 【关键】用一把锁把 stop() 串行化。旧版本只判断 running，而且是在最后才
    // 置 false，于是两个 stop() 会同时进入并**同时 join 同一个 std::thread**：
    // MSVC 的 join() 遇到已被别人 join 掉的句柄会抛 std::system_error，
    // 线程函数里没人接就是 std::terminate -> abort（0xC0000409）。
    // 现在第二个调用者在这里排队，等第一个收尾完，然后 started==false 直接返回。
    std::lock_guard<std::mutex> serial(impl_->stopMtx);
    if (!impl_->started) return;

    impl_->started = false;
    impl_->stopReq.store(true);
    impl_->sessionActive.store(false);

    // 先关套接字把阻塞在 accept/recv 的线程唤醒，**不要**先动线程句柄：
    // accept 线程会以 WSAENOTSOCK(10038) 从 accept() 返回，然后自己走完收尾。
    impl_->control.close();
    impl_->close_udp();
    impl_->controlBackup.close();   // 备用线路也关掉

    // accept 线程内部负责 join 会话线程，所以这里 join 它一个就够；
    // 后面的 join_session_threads() 只是兜底（幂等：谁先搬走谁负责 join）。
    Impl::join_thread(impl_->thAccept, impl_->lifeMtx);
    Impl::join_thread(impl_->thAcceptBackup, impl_->lifeMtx);
    impl_->join_session_threads();

    impl_->injector.release_all();
    impl_->running.store(false);
    net::shutdown();
}

bool HostService::running() const { return impl_->running.load(); }

HostStatus HostService::status() const {
    std::lock_guard<std::mutex> lk(impl_->statMtx);
    return impl_->status;
}

void HostService::set_status_sink(StatusSink sink) { impl_->sink = std::move(sink); }
void HostService::force_keyframe() { impl_->keyframePending.store(true); }
void HostService::set_bitrate(int kbps) { impl_->targetBitrate.store(kbps); }

} // namespace ms::host
