// =============================================================================
//  client_session.cpp
// =============================================================================
#include "client_session.h"

#include "decoder.h"
#include "renderer.h"
#include "audio_player.h"
#include "video_receiver.h"
#include "input_capture.h"
#include "input_pipeline.h"
#include "session_toolbar.h"

#include "../common/clock.h"
#include "../common/crypt.h"
#include "../common/fsutil.h"
#include "../common/log.h"
#include "../common/net.h"
#include "../common/perf.h"
#include "../common/stats.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
}

// 由 main.cpp 定义：窗口内是否藏起本机系统光标（改画远端光标）
extern std::atomic<bool> g_hide_local_cursor;

namespace ms::client {
namespace {

// 把被控端 32 位时间戳还原成 64 位，并对齐到本机时间轴
i64 rebuild_remote_ts(const ClockSync& clock, u32 ts32, i64 localNowUs) {
    const i64 remoteNow = clock.local_to_remote(localNowUs);
    i64 full = (remoteNow & ~i64(0xFFFFFFFF)) | i64(ts32);
    const i64 half = i64(1) << 31;
    if (full - remoteNow > half)       full -= (i64(1) << 32);
    else if (remoteNow - full > half)  full += (i64(1) << 32);
    return full;
}

} // namespace

struct ClientSession::Impl {
    std::atomic<bool> stopReq{false};
    std::string serverHost;
    u16 serverPort = 0;
    sockaddr_in serverAddr{};

    ControlClient control;
    net::TcpChannel backupCtrl;    // 备用线路（双线：键鼠走备线，主线留给视频）
    std::atomic<bool> backupActive{false};
    VideoReceiver receiver;
    VideoDecoder  decoder;
    Renderer      renderer;
    InputCapture  input;
    InputPipeline inputPipe;         // 可靠输入发送线程（TCP 主路径，v2）
    SessionToolbar toolbar;          // 顶部工具栏（RustDesk 式，不抢焦点）

    SessionOptions opts;
    ConnectParams  params;
    StreamParams   stream;

    std::thread thControl;
    std::thread thBackupKeep;   // 备线保活/重连线程（P0：备线完整生命周期）

    mutable std::mutex statMtx;
    ClientStatus status;

    LatencyStats decodeStats;
    LatencyStats presentStats;
    LatencyStats latencyStats;

    // 近期整帧丢失率（3 秒半衰 EMA）：UI 显示与自适应码率都用它，
    // 不用会话累计值（累计值在开局抖一阵之后就永远回不去了）。
    double lossEma = 0.0;

    // 网络排队延迟：端到端延迟相对"近期最好值"的增量（ms）
    // RustDesk 用它做拥塞判定（>150ms 视为拥塞），比丢包更早、更平滑。
    i64 latencyBestUs = 0;
    i64 latencyBestAtUs = 0;
    double queueDelayMs = 0.0;

    // ---- 端到端延迟分位数 + jank 分级（照抄 UU远程/GameViewer 的验收口径）----
    // 报告 §8.3：小 jank = 迟到 1 帧（33~100ms）、中 jank = 100~1000ms、大 jank >1000ms；
    // §8.4 的"连接不抖"判据：jank 时间占比 <0.5%、中+大 jank <1 次/分钟、p99 <150ms。
    // 有了这几个数，用户/我们能一眼判断"到底是链路抖还是程序卡"。
    static constexpr int kLatRing = 512;      // ≈8.5 秒@60fps 的窗口
    i32  latRing[kLatRing] = {};
    int  latPos = 0;
    int  latCount = 0;
    u32  jankSmall = 0;
    u32  jankMedium = 0;
    u32  jankBig = 0;

    void sample_latency(i64 us) {
        const i32 ms = i32(us > 0 ? (us + 500) / 1000 : 0);
        latRing[latPos] = ms;
        latPos = (latPos + 1) % kLatRing;
        if (latCount < kLatRing) ++latCount;
        if (ms > 1000)      ++jankBig;
        else if (ms > 100)  ++jankMedium;
        else if (ms > 33)   ++jankSmall;
    }

    // p50/p90/p99（毫秒）；窗口里没有样本时返回 0
    void latency_percentiles(int& p50, int& p90, int& p99) const {
        p50 = p90 = p99 = 0;
        if (latCount <= 0) return;
        i32 tmp[kLatRing];
        std::memcpy(tmp, latRing, sizeof(i32) * size_t(latCount));
        std::sort(tmp, tmp + latCount);
        auto pick = [&](double q) {
            int idx = int(q * double(latCount - 1) + 0.5);
            if (idx < 0) idx = 0;
            if (idx >= latCount) idx = latCount - 1;
            return int(tmp[idx]);
        };
        p50 = pick(0.50);
        p90 = pick(0.90);
        p99 = pick(0.99);
    }

    AudioPlayer audio;               // 被控端声音播放（失败不致命，静音继续）
    bool audioEnabled = true;
    int  audioRecvLogUs = 0;

    u32  inputSeq = 0;               // UDP 回退路径的包序号（TCP 主路径用 batchId）
    // 按钮归属（拖拽语义）：bit0..4 = 鼠标按钮 1..5 的按下已转发远端。
    // 会话层用它过滤本机 UI 上方的鼠标事件：远端拖拽进行中，抬起必须照发，
    // 否则光标移到工具栏上松开会把远端按钮卡在按下状态（拖拽不结束）。
    u32  remoteButtonsMask = 0;
    u64  framesDecoded = 0;
    int  curKeyframeIntervalSec = 2;
    int  curCodec = 1;               // 1=H.264 2=HEVC（会话内可切，见 on_codec_change）
    int  sharpenLevel = 1;           // 0=关 1=低 2=中 3=高（会话内向工具栏同步）
    bool needDecoderReset = false;   // 重协商后要等新 IDR 再喂解码器
    std::string endReason;           // 掉线/退出原因，收尾时显示给用户

    // ---- 远端光标 ----------------------------------------------------------
    bool   havePointer = false;
    proto::PointerPosMsg lastPointer{};

    // ---- 鼠标模式（解决"光标乱飘/没映射到本机鼠标"）------------------------
    // 被控端光标**可见**（桌面/游戏菜单）时走绝对映射：把本机鼠标位置换算成
    // 远端像素坐标发过去，被控端直接 SetCursorPos —— 远端光标和本机鼠标 1:1
    // 重合，不经过系统鼠标加速，所以不会飘。
    // 被控端光标**隐藏**（游戏内场）时走相对模式：藏起本机光标，发原始位移，
    // 游戏自己读 Raw Input（这才是 FPS 瞄准需要的）。
    int   mouseEffective = 2;            // 当前生效：1=绝对 2=相对
    bool  autoVis = false;               // 自动模式："被控端光标可见"状态
    i64   autoVisChangeUs = 0;           // 该状态最近一次变化（切换防抖用）
    i64   lastAbsSendUs = 0;
    int   lastAbsX = -1, lastAbsY = -1;
    u32   absSeq = 0;
    bool  absPhysical_ = true;        // v2 能力协商后置位：绝对坐标走物理桌面约定
    HWND  sessionHwnd = nullptr;

    void set_mouse_effective(int mode) {
        if (mouseEffective == mode) return;
        mouseEffective = mode;
        // 切换鼠标模式时远端按键状态语义会变（相对位移残留、拖拽中断），
        // 让被控端把按下的键/按钮全部抬起，避免拖拽悬挂或卡键
        inputPipe.request_release_all();
        remoteButtonsMask = 0;
        if (mode == 1) {
            input.set_mouse_relative(false);          // 本机光标恢复可见、解除限制
            renderer.set_cursor_draw_enabled(false);  // 不再画远端光标（本机光标就是它）
            // 切过去的瞬间把本机光标摆到远端光标当前位置，视觉上不跳
            if (havePointer && sessionHwnd) {
                POINT cli{};
                if (renderer.video_to_client(lastPointer.x, lastPointer.y, cli)) {
                    POINT scr = cli;
                    if (::ClientToScreen(sessionHwnd, &scr)) {
                        ::SetCursorPos(scr.x, scr.y);
                        lastAbsX = lastPointer.x;
                        lastAbsY = lastPointer.y;
                    }
                }
            }
            MS_INFO("鼠标模式 -> 绝对映射（被控端光标可见：桌面/游戏菜单，"
                    "远端光标与你本机鼠标 1:1）");
        } else {
            input.set_mouse_relative(true);           // 藏光标 + 限制 + 钉中心
            renderer.set_cursor_draw_enabled(true);
            lastAbsX = lastAbsY = -1;
            MS_INFO("鼠标模式 -> 相对模式（被控端光标隐藏：游戏内场，发原始位移）");
        }
    }

    void update_mouse_mode(i64 now) {
        if (opts.mouseMode == 1) { set_mouse_effective(1); return; }
        if (opts.mouseMode == 2) { set_mouse_effective(2); return; }
        // 自动模式
        const bool vis = havePointer && lastPointer.visible != 0;
        if (vis != autoVis) {
            autoVis = vis;
            autoVisChangeUs = now;
        }
        if (vis) {
            // 可见后稳定 400ms 再切绝对：游戏进出菜单时光标可见性会抖，避免来回切
            if (mouseEffective != 1 && now - autoVisChangeUs > 400000) {
                set_mouse_effective(1);
            }
        } else if (mouseEffective != 2) {
            // 进游戏（光标一被藏起来）立刻切相对，保证瞄准手感
            set_mouse_effective(2);
        }
    }

    // 发一个绝对坐标的鼠标移动（a/b = 远端像素坐标）。
    // v2：默认带 kInputFlagAbsPhysical —— a/b 是虚拟桌面物理像素坐标，
    // 被控端 SetCursorPos 直接使用；缩放串流/多显示器/负坐标的换算在
    // video_to_physical_desktop() 里一次做完，注入端不再二次换算。
    void send_abs_mouse(int vx, int vy) {
        proto::InputEvent e;
        e.type  = u8(proto::InputType::MouseMove);
        e.flags = proto::kInputFlagAbsMove;
        if (absPhysical_) e.flags |= proto::kInputFlagAbsPhysical;
        e.seq   = u16(++absSeq);
        int px = vx, py = vy;
        if (absPhysical_) {
            int sx = 0, sy = 0;
            if (renderer.video_to_physical_desktop(vx, vy, sx, sy)) {
                px = sx;
                py = sy;
            } else {
                return;   // 目标点在画面外（黑边），不注入
            }
        }
        e.a = px;
        e.b = py;
        inputPipe.enqueue(e);
        // 首次下发留一条 INFO（排障"鼠标不跟手"时能确认绝对映射真的在工作），
        // 之后只在 Debug 级别抽样记录，避免刷日志。
        static int n = 0;
        if (n == 0) {
            MS_INFO("[鼠标] 绝对映射已生效，首个坐标 (%d,%d)%s", px, py,
                    absPhysical_ ? "（物理桌面坐标）" : "");
        } else if ((n % 200) == 0) {
            MS_DEBUG("[鼠标] 绝对坐标 (%d,%d) 第 %d 次", px, py, n + 1);
        }
        ++n;
    }

    // ---- 传输自愈（UDP <-> TCP）-------------------------------------------
    // 【设计目标：不来回切】切换的代价是「解码链断开 + 等一个 IDR」，一秒钟切三次
    // 比一直轻微丢包更难受（用户实测就是「tcp/udp 一直切换 + 鼠标卡顿 + 花屏」）。
    // 所以改成「滞回 + 冻结期」的保守状态机：
    //
    //   UDP -> TCP：只有 UDP 真的不能用才切
    //       a) 连续 2 秒一个 UDP 包都收不到（被控端每 500ms 至少发一个光标包，
    //          所以这等于「UDP 被防火墙/隧道拦了」）；
    //       b) 或者整帧丢失率 >30% 且持续 2 秒（真的没法玩了）。
    //       轻微丢包（1~10%）坚决不切：FEC + 自适应码率会把它压下去，
    //       切到 TCP 反而把延迟拉高一个数量级。
    //   冷却：切完 15 秒内不再切；切回 UDP 后 20 秒观察期内不再切走。
    //
    //   TCP -> UDP：每 5 秒探测一次（1.2 秒窗口），连续 2 次到达率 >=95% 才切回。
    //       每次抖动把观察期翻倍（20s→40s→80s…最多 5 分钟），
    //       稳定 2 分钟后惩罚归零 —— 反复抖的网络最终会"钉"在更稳的那条路上。
    int  linkMode = 0;                 // 0=UDP（低延迟）1=TCP（零丢包兜底）
    i64  sessionStartUs = 0;           // 会话开始时刻（判断 UDP 迟迟不来）
    i64  lastModeChangeUs = 0;         // 上次切换时刻（冷却期用）
    i64  lastUdpBadUs = 0;             // UDP 从什么时候开始"不能用"（0=正常）
    i64  udpQuarantineUntilUs = 0;     // 切回 UDP 后的观察期：期间不再切走
    i64  tcpSettleUntilUs = 0;         // 刚切到 TCP 的稳定期：期间不探测
    i64  nextProbeUs = 0;              // 下一次探测时刻（TCP 模式下）
    i64  probeEndUs = 0;               // 当前探测窗口结束时刻（0 = 没在探测）
    int  probeOkStreak = 0;            // 连续几次探测成功才切回（防抖）
    i64  probeIntervalUs = 5000000;    // 探测间隔
    i64  flapPenaltyUs = 20000000;     // 抖动惩罚：每次抖动翻倍
    std::atomic<u32> tcpFramesRecv{0}; // 探测窗口内的 TCP 帧数（做分母，控制线程写）
    i64  lastTransportTickUs = 0;
    u64  lastLostFrames = 0;
    u64  lastOkFrames = 0;
    u64  lastUdpPackets = 0;
    bool forceTcp = false;             // 排障：强制 TCP，不探测回来
    bool testTcpStart = false;         // 自测钩子：开局故意切 TCP，验证能自愈回 UDP
    bool testTcpStarted = false;
    int  toastMs = 3200;               // 顶部小提示条显示时长（自测时可通过环境变量拉长）

    // ---- 解码链连续性（抗花屏的核心状态）-----------------------------------
    // 缺一帧之后，后面的 P 帧全都参考错帧 -> 花屏；所以宁可丢帧等 IDR，
    // 也绝不把参考链已断的帧喂给解码器。
    bool waitingIdr = true;            // 只在关键帧到达后才解除
    i64  waitingSinceUs = 0;           // 什么时候开始等 IDR（0=没在等）
    bool haveFedFrame = false;
    u32  lastFedFrameId = 0;
    i64  lastIdrReqUs = 0;
    bool pendingIdrRequest = false;    // 丢了帧但还没发 IDR 请求（等下一个完整帧再发）
    u64  framesSinceIdr_ = 0;          // 距上次 IDR 过了多少帧（兜底强制请求用）

    void request_idr(int copies, i64 now) {
        // 节流：等 IDR 期间每 100ms 至少求一次就够，别把控制通道刷爆
        if (now - lastIdrReqUs < 100000 && waitingIdr) return;
        lastIdrReqUs = now;
        control.request_keyframe(copies);
    }

    void switch_to_tcp(const char* why) {
        if (linkMode == 1) return;
        linkMode = 1;
        if (!control.set_transport_mode(1, false)) return;
        const i64 now = now_us();
        lastModeChangeUs = now;
        tcpSettleUntilUs = now + 8000000;   // 8 秒内不探测，先让 TCP 画面稳住
        nextProbeUs = now + 5000000;
        probeOkStreak = 0;
        probeEndUs = 0;
        lastUdpBadUs = 0;
        // 换了传输路径：解码链必须重新对齐，等一个 IDR（否则立刻花屏）
        waitingIdr = true;
        waitingSinceUs = (waitingSinceUs == 0) ? now : waitingSinceUs;
        request_idr(2, now);
        // 【P0 修复】视频状态机不再无条件改输入路由：
        // 双线模式键鼠走独立备线 TCP，与视频走哪条路无关；只有单线（无备线）时
        // 视频切 TCP 说明 UDP 基本不通，输入才跟着走 TCP 控制通道。
        if (!backupActive.load()) {
            inputPipe.set_use_tcp(true);
        }
        MS_WARN("%s，切到 TCP 传视频（不丢帧）。会持续探测 UDP，一恢复就自动切回", why);
        renderer.show_toast(L"UDP 不稳定，已切 TCP 保画面（会自动切回）", toastMs);
    }

    void switch_to_udp(const char* why) {
        if (linkMode == 0) return;
        linkMode = 0;
        control.set_transport_mode(0, false);
        const i64 now = now_us();
        lastModeChangeUs = now;
        // 观察期：刚切回 UDP 就算再抖也先忍着（配合自适应码率），
        // 否则就会出现用户看到的「一直切换」。抖动一次翻倍惩罚。
        udpQuarantineUntilUs = now + flapPenaltyUs;
        flapPenaltyUs = std::min<i64>(flapPenaltyUs * 2, 300000000);
        nextProbeUs = 0;
        probeEndUs = 0;
        probeOkStreak = 0;
        lastUdpBadUs = 0;
        receiver.set_probe_mode(false);
        tcpFramesRecv.store(0);
        waitingIdr = true;
        waitingSinceUs = (waitingSinceUs == 0) ? now : waitingSinceUs;
        request_idr(2, now);
        // 【P0 修复】同上：视频切回 UDP 不代表输入也切回 UDP。
        // 双线模式键鼠留在备线 TCP（独立连接，不受视频影响）；
        // 单线模式输入跟视频走 UDP（3 份冗余，避免被 TCP 视频帧队头阻塞）。
        if (!backupActive.load()) {
            inputPipe.set_use_tcp(false);
        }
        MS_INFO("%s，切回 UDP 传视频（低延迟）。观察期 %lld 秒内不再切走",
                why, (long long)(flapPenaltyUs / 2000000));
        renderer.show_toast(L"UDP 已恢复，切回低延迟模式", toastMs);
    }

    void begin_probe() {
        if (!control.request_udp_probe()) {
            nextProbeUs = now_us() + probeIntervalUs;
            return;
        }
        probeEndUs = now_us() + 1200000;   // 窗口长度必须和被控端一致
        receiver.set_probe_mode(true);
        receiver.take_probe_frames();      // 清掉旧计数
        tcpFramesRecv.store(0);
        MS_DEBUG("开始 UDP 探测窗口（1.2 秒）");
    }

    void finish_probe() {
        probeEndUs = 0;
        receiver.set_probe_mode(false);
        const u32 udpFrames = receiver.take_probe_frames();
        const u32 tcpFrames = tcpFramesRecv.exchange(0);
        if (tcpFrames == 0) {
            // 窗口内根本没有视频帧（画面静止/游戏暂停）：这次探测没有意义，
            // 不能当成"UDP 坏"（否则静止桌面会把会话永远钉在 TCP 上）。
            MS_DEBUG("UDP 探测：窗口内没有视频帧，稍后重试");
            probeOkStreak = 0;
            nextProbeUs = now_us() + probeIntervalUs;
            return;
        }
        const double ratio = double(udpFrames) / double(tcpFrames);
        MS_INFO("UDP 探测结果: UDP %u 帧 / TCP %u 帧，到达率 %.0f%%",
                udpFrames, tcpFrames, ratio * 100.0);
        if (ratio >= 0.95 && tcpFrames >= 8) {
            ++probeOkStreak;
            if (probeOkStreak >= 2) {
                switch_to_udp("UDP 探测连续两次 >=95%");
            } else {
                nextProbeUs = now_us() + 1500000;
            }
        } else {
            probeOkStreak = 0;
            nextProbeUs = now_us() + probeIntervalUs;
        }
    }

    // 每 150ms 跑一次的传输自愈状态机
    void tick_transport(i64 now) {
        if (now - lastTransportTickUs < 150000) return;
        lastTransportTickUs = now;

        const auto rs = receiver.stats();
        const u64 lostDelta = rs.framesLost - lastLostFrames;
        const u64 okDelta   = rs.framesOk - lastOkFrames;
        lastLostFrames = rs.framesLost;
        lastOkFrames   = rs.framesOk;
        lastUdpPackets = rs.packets;
        const double lossRate = (lostDelta + okDelta) > 0
            ? double(lostDelta) * 100.0 / double(lostDelta + okDelta) : 0.0;

        if (linkMode == 0) {
            // 用「任意 UDP 包」判活而不是「视频包」：被控端每 500ms 必发一个
            // 光标包（静止桌面也发），所以一个包都没有 = UDP 路径真的断了；
            // 而屏幕静止时没有视频帧是正常的，不能当成断流。
            const i64 lastAnyUs = receiver.last_packet_us();
            const i64 silentUs = lastAnyUs == 0 ? (now - sessionStartUs) : (now - lastAnyUs);
            const bool dead = silentUs > ((lastAnyUs == 0) ? 3000000 : 2000000);
            // 「烂到没法玩」：整帧丢失率 >30% 且确实有帧在流动。
            // 轻微丢包交给 FEC + 自适应码率，不切 TCP（切了延迟更高、更卡）。
            const bool hopeless = lossRate > 30.0 && (okDelta + lostDelta) >= 3;

            if (dead || hopeless) {
                if (lastUdpBadUs == 0) lastUdpBadUs = now;
            } else {
                lastUdpBadUs = 0;
            }
            const i64 badForUs = lastUdpBadUs == 0 ? 0 : (now - lastUdpBadUs);
            // 断流 2 秒就切（UDP 被拦/不通）；丢包型的判定要更久（3.5 秒），
            // 因为丢包通常能靠 FEC + 降码率救回来，贸然切 TCP 只会把延迟拉高。
            // 开局 6 秒内不切：首帧 IDR 很大，丢几包是正常的（误判会导致一上来就 TCP）。
            const i64 needBadUs = dead ? 2000000 : 3500000;
            const bool warmed = (now - sessionStartUs) > 6000000;

            // 【第三种判据：解码链卡死】
            // 丢包重到连 IDR（它是最大的一帧，几千个分片）都拼不出来时，画面会一直
            // 停在"等 IDR"，而帧级丢包率可能只有 25~30%（够不到上面的 30% 阈值）。
            // 这种情况对于游戏就是"彻底玩不了"，必须切 TCP：至少能持续出画面。
            const bool stalled = waitingIdr && waitingSinceUs != 0 &&
                                 (now - waitingSinceUs) > 2500000;

            if (warmed && stalled && now > udpQuarantineUntilUs &&
                now - lastModeChangeUs > 15000000) {
                switch_to_tcp("连续 2.5 秒等不到 IDR（丢包把关键帧也丢了）");
            } else if (warmed && badForUs > needBadUs && now > udpQuarantineUntilUs &&
                       now - lastModeChangeUs > 15000000) {
                switch_to_tcp(dead ? "UDP 连续 2 秒没有任何包（被拦/断流）"
                                   : "UDP 整帧丢失率 >30% 持续 3.5 秒");
            }
        } else {
            if (forceTcp) return;   // 排障模式：钉死在 TCP
            if (now < tcpSettleUntilUs) return;
            if (probeEndUs == 0 && now >= nextProbeUs) begin_probe();
            else if (probeEndUs != 0 && now >= probeEndUs) finish_probe();
        }
    }

    // ---- 备线保活/重连（成员函数，thBackupKeep 线程运行）----
    // 备线握手参数（connect_backup_line 内部使用）
    bool connect_backup_line() {
        std::string berr;
        if (!backupCtrl.connect(params.backupHost, params.backupPort, 5000, berr))
            return false;
        proto::HelloMsg bh;
        bh.version = proto::kVersion;
        bh.flags   = proto::kHelloFlagBackupLine;
        crypt::random_bytes(bh.nonce, proto::kNonceLen);
        bh.wantFps = stream.fps;
        bh.clientName = params.clientName + "-backup";
        if (!backupCtrl.send_frame(proto::MsgType::Hello, bh.serialize())) return false;
        proto::MsgType bt;
        Bytes bpayload;
        if (backupCtrl.recv_frame(bt, bpayload, 5000) <= 0 || bt != proto::MsgType::Challenge)
            return false;
        proto::ChallengeMsg chal;
        if (!proto::ChallengeMsg::parse(bpayload.data(), bpayload.size(), chal)) return false;
        u8 key[32];
        crypt::derive_key(params.password, key);
        proto::AuthMsg auth;
        crypt::compute_proof(key, bh.nonce, chal.serverNonce, chal.salt, auth.proof);
        if (!backupCtrl.send_frame(proto::MsgType::Auth, auth.serialize())) return false;
        if (backupCtrl.recv_frame(bt, bpayload, 5000) <= 0 || bt != proto::MsgType::AuthResult)
            return false;
        proto::AuthResultMsg ar;
        if (!proto::AuthResultMsg::parse(bpayload.data(), bpayload.size(), ar) || !ar.ok)
            return false;
        return true;
    }

    void backup_keep_loop() {
        i64 lastPingUs = 0;
        i64 lastProgressUs = now_us();   // 最近一次「收到任何包」时刻
        u32 lastAcked = 0;              // 最近一次 InputApplied 批号
        i64 retryAtUs = 0;              // 下次备线重连时刻
        i64 retryDelayUs = 3000000;    // 重连退避（3s 起，最大 10s）
        while (!stopReq.load()) {
            const i64 now = now_us();

            if (!backupActive.load()) {
                // ---- 未连接：按退避重连 ----
                if (now < retryAtUs) { sleep_ms(100); continue; }
                backupCtrl.close();
                if (connect_backup_line()) {
                    backupActive.store(true);
                    lastProgressUs = now;
                    lastAcked = inputPipe.batches_acked();
                    retryDelayUs = 3000000;
                    // 重连成功：先让远端清一遍旧状态（ReleaseAll 屏障），
                    // 再把键鼠迁回备线。旧事件不跨连接重发 —— 队列里的
                    // 事件继续走当前主线通道，不会因切换而重复执行。
                    inputPipe.request_release_all();
                    inputPipe.set_use_tcp(true);
                    inputPipe.set_use_backup(true);
                    MS_INFO("备线重连成功，键鼠迁回备线（已先发 ReleaseAll 清理旧状态）");
                    toolbar.set_backup_line_status(true);
                } else {
                    retryAtUs = now + retryDelayUs;
                    retryDelayUs = std::min<i64>(retryDelayUs * 2, 10000000);
                    backupCtrl.close();
                }
                continue;
            }

            // ---- 已连接：心跳 + 收包 + 健康判定 ----
            if (now - lastPingUs >= 500000) {
                lastPingUs = now;
                proto::PingMsg pm;
                pm.seq = 0; pm.t1 = now;
                if (!backupCtrl.send_frame(proto::MsgType::Ping, pm.serialize())) {
                    MS_WARN("备线心跳发送失败，判定断开，切回主线并准备重连");
                    backupActive.store(false);
                    inputPipe.set_use_backup(false);
                    // 回主线：视频当前走 TCP（UDP 不通）时输入也走 TCP 控制通道，
                    // 否则键鼠包会从死掉的 UDP 路径凭空消失。
                    inputPipe.set_use_tcp(linkMode == 1);
                    toolbar.set_backup_line_status(false);
                    retryAtUs = now + 1000000;   // 1 秒后开始重连
                    continue;
                }
            }
            proto::MsgType bt;
            Bytes bpayload;
            const int r = backupCtrl.recv_frame(bt, bpayload, 100);
            if (r > 0) {
                lastProgressUs = now;
                if (bt == proto::MsgType::InputApplied) {
                    proto::InputAppliedMsg m;
                    if (m.parse(bpayload.data(), bpayload.size()))
                        inputPipe.on_input_applied(m.batchId, m.applied, m.status);
                }
                // Pong 等消息：收到即算健康进展
            } else if (r < 0) {
                MS_WARN("备线接收失败（对端关闭），切回主线并准备重连");
                backupActive.store(false);
                inputPipe.set_use_backup(false);
                inputPipe.set_use_tcp(linkMode == 1);
                toolbar.set_backup_line_status(false);
                retryAtUs = now + 1000000;
                continue;
            }
            // 健康判定：2 秒内既没有包收到、回执也没推进 -> 异常
            const u32 ackNow = inputPipe.batches_acked();
            if (ackNow != lastAcked) {
                lastAcked = ackNow;
                lastProgressUs = now;
            }
            if (now - lastProgressUs > 2000000) {
                MS_WARN("备线 2 秒无进展（无 Pong/回执），判定异常，切回主线并重连");
                backupActive.store(false);
                backupCtrl.close();
                inputPipe.set_use_backup(false);
                inputPipe.set_use_tcp(linkMode == 1);
                toolbar.set_backup_line_status(false);
                retryAtUs = now + 1000000;
            }
        }
    }

    // 控制线程：心跳 + 时钟同步
    void control_loop() {
        i64 lastFeedbackUs = 0;
        while (!stopReq.load()) {
            const int r = control.poll(100);
            if (r < 0) {
                if (!stopReq.load()) {
                    std::lock_guard<std::mutex> lk(statMtx);
                    status.lastError = "控制通道已断开";
                }
                break;
            }
            const i64 now = now_us();
            if (now - lastFeedbackUs > 250000) {
                lastFeedbackUs = now;
                proto::FeedbackMsg fb;
                // 帧统计发**增量**：被控端据此算整帧丢失率，自适应调 FEC 组大小
                // 与码率（FEC 开销 = 1/组大小，丢包越重组要越小）。
                static u64 fbOk = 0, fbLost = 0;
                {
                    const auto rs = receiver.stats();
                    fb.framesOk   = u32(rs.framesOk   - fbOk);
                    fb.framesLost = u32(rs.framesLost - fbLost);
                    fbOk   = rs.framesOk;
                    fbLost = rs.framesLost;
                }
                {
                    std::lock_guard<std::mutex> lk(statMtx);
                    fb.lastFrameId = 0;
                    fb.recvKbps = u32(status.recvKbps);
                    fb.decodeUs = u32(decodeStats.avg());
                    fb.presentUs = u32(presentStats.avg());
                    fb.latencyUs = u32(latencyStats.avg());
                    fb.queueDepth = u16(receiver.queue_depth());
                    fb.rttMs = u16(control.rtt_us() / 1000);
                }
                control.send_feedback(fb);
            }
            sleep_ms(2);
        }
    }
};

ClientSession::~ClientSession() {
    request_stop();
    if (impl_) {
        if (impl_->thControl.joinable()) impl_->thControl.join();
        delete impl_;
        impl_ = nullptr;
    }
}

void ClientSession::request_stop() {
    if (impl_) impl_->stopReq.store(true);
}

void ClientSession::request_keyframe() {
    if (impl_) impl_->control.request_keyframe(3);
}

ClientStatus ClientSession::status() const {
    if (!impl_) return ClientStatus{};
    std::lock_guard<std::mutex> lk(impl_->statMtx);
    ClientStatus s = impl_->status;
    s.queueDepth = int(impl_->receiver.queue_depth());
    return s;
}

int ClientSession::sharpen_level() const {
    if (!impl_) return -1;
    return impl_->sharpenLevel;
}

bool ClientSession::run(HWND hwnd, const ConnectParams& params,
                        const SessionOptions& opts, std::string& err) {
    // MMCSS：主循环身兼「解码 + 渲染 + 消息泵」，注册为 Games 类别；
    // 计时器分辨率同步被拉到 0.5ms，MsgWaitForMultipleObjects(...,2,...) 的
    // 2ms 唤醒才真正精确（默认 15.6ms 分辨率下实际会睡到 15ms，帧间隔抖动很大）。
    // 参考 UU远程 streamer.dll 的做法（逆向第 2 轮，§8 运行时开关）。
    const ms::PerfThreadScope perfScope(L"Games");

    delete impl_;
    impl_ = new Impl();
    impl_->params = params;
    impl_->opts = opts;
    impl_->sessionHwnd = hwnd;
    impl_->serverHost = params.host;
    impl_->serverPort = params.port;

    auto fail = [&](const std::string& e) {
        err = e;
        std::lock_guard<std::mutex> lk(impl_->statMtx);
        impl_->status.lastError = e;
        return false;
    };

    if (!net::init()) return fail("WSAStartup 失败");

    // ---- 1. TCP 握手 + 口令校验 ----
    if (!impl_->control.connect_and_auth(params, impl_->stream, err, 8000)) {
        net::shutdown();
        return fail("连接失败: " + err);
    }

    // v2 能力协商：绝对坐标走物理桌面约定（旧服务端忽略未知 flag 位，仍按 v1 解析）。
    impl_->absPhysical_ = true;
    impl_->inputPipe.set_server_supports_batch(true);
    // 【输入主路径：默认 UDP，不走 TCP】
    // 依据（UU远程/GameViewer 逆向报告 P0）：TCP 的队头阻塞 + 最小 RTO 200ms，
    // 一次丢包就让鼠标"粘住" 200ms 以上；而鼠标移动是可丢弃的状态量。
    // 它们实测输入延迟 18~22ms 且 p90=p99（无长尾），前提就是输入不排在任何
    // 可靠有序流后面。我们改成 UDP + 每包 3 份冗余（被控端按包序号去重）。
    // 只有视频被迫切到 TCP（说明 UDP 基本不通）时才回落到 TCP 控制通道。
    // 输入初始路由策略改由 §2.8 start(Config) 显式传入，这里不再预置。

    {
        std::lock_guard<std::mutex> lk(impl_->statMtx);
        impl_->status.connected = true;
        impl_->status.serverAddr = impl_->serverHost + ":" + std::to_string(impl_->serverPort);
        impl_->status.width = impl_->stream.width;
        impl_->status.height = impl_->stream.height;
        impl_->status.fps = impl_->stream.fps;
    }
    MS_INFO("已连接 %s，协商参数 %dx%d @%dfps %ukbps codec=%d",
            (impl_->serverHost + ":" + std::to_string(impl_->serverPort)).c_str(),
            impl_->stream.width, impl_->stream.height,
            impl_->stream.fps, impl_->stream.bitrateKbps, int(impl_->stream.codec));

    // v2 绝对鼠标换算信息：被采集显示器在虚拟桌面里的原点 + 物理尺寸
    impl_->renderer.set_desktop_origin(impl_->stream.monitorOriginX,
                                       impl_->stream.monitorOriginY);
    if (impl_->stream.desktopPhysW > 0 && impl_->stream.desktopPhysH > 0) {
        impl_->renderer.set_desktop_pixels(impl_->stream.desktopPhysW,
                                           impl_->stream.desktopPhysH);
    }

    // ---- 2. UDP 接收端 ----
    // 绑定临时端口（0），不占用被控端的端口号：
    // 视频是「被控端回发到我们的源地址」，所以只需要一个稳定的本地源端口，
    // 由保活包让被控端学到。这样两端跑在同一台机器上（回环测试）也不会抢端口。
    if (!impl_->receiver.init(0, err)) {
        impl_->control.send_disconnect("client udp init failed");
        net::shutdown();
        return fail("UDP 初始化失败: " + err);
    }
    impl_->receiver.set_session(impl_->control.session_id());

    // 解析并记住被控端 UDP 地址
    if (!net::resolve_ipv4(params.host, params.port, impl_->serverAddr, err)) {
        net::shutdown();
        return fail("无法解析服务端地址: " + err);
    }
    impl_->receiver.set_server(impl_->serverAddr);

    // 立刻发一个保活包，让被控端学到我们 NAT 后的真实 UDP 地址。
    // 早发几个，避免被控端在 1.5s 学习窗口内还没建立 UDP 映射。
    {
        std::vector<u8> pkt(sizeof(proto::UdpHeader));
        proto::UdpHeader h{};
        h.type = u8(proto::UdpType::Keepalive);
        h.sessionId = impl_->control.session_id();
        std::memcpy(pkt.data(), &h, sizeof(h));
        for (int i = 0; i < 3; ++i) {
            net::udp_send_to(impl_->receiver.udp_socket(), pkt.data(), pkt.size(), impl_->serverAddr);
            sleep_ms(10);
        }
    }
    impl_->receiver.start();
    MS_INFO("UDP 接收线程已启动，开始初始化解码器…");

    // ---- 2.5 备用线路（双线：键鼠走备线，主线全留给视频）----
    // 备线是独立的 TCP 连接（客户端连到 backup_host:backup_port），只传
    // InputBatch / Ping / Pong，不传视频。键鼠包不再和视频帧排同一条流，
    // 即使主线 UDP 视频不稳定，键鼠也依然跟手。
    if (!params.backupHost.empty() && params.backupPort != 0) {
        std::string berr;
        if (impl_->backupCtrl.connect(params.backupHost, params.backupPort, 5000, berr)) {
            // 简化握手：Hello 带 backup 标志，被控端只做口令校验，不建会话
            proto::HelloMsg bh;
            bh.version = proto::kVersion;
            bh.flags   = proto::kHelloFlagBackupLine;
            crypt::random_bytes(bh.nonce, proto::kNonceLen);
            bh.wantFps = impl_->stream.fps;
            bh.clientName = impl_->params.clientName + "-backup";
            if (impl_->backupCtrl.send_frame(proto::MsgType::Hello, bh.serialize())) {
                proto::MsgType bt;
                Bytes bpayload;
                if (impl_->backupCtrl.recv_frame(bt, bpayload, 5000) > 0 &&
                    bt == proto::MsgType::Challenge) {
                    proto::ChallengeMsg chal;
                    if (proto::ChallengeMsg::parse(bpayload.data(), bpayload.size(), chal)) {
                        u8 key[32];
                        crypt::derive_key(params.password, key);
                        proto::AuthMsg auth;
                        crypt::compute_proof(key, bh.nonce, chal.serverNonce, chal.salt, auth.proof);
                        if (impl_->backupCtrl.send_frame(proto::MsgType::Auth, auth.serialize())) {
                            if (impl_->backupCtrl.recv_frame(bt, bpayload, 5000) > 0 &&
                                bt == proto::MsgType::AuthResult) {
                                proto::AuthResultMsg ar;
                                if (proto::AuthResultMsg::parse(bpayload.data(), bpayload.size(), ar) && ar.ok) {
                                    impl_->backupActive.store(true);
                                    // 键鼠立刻切到备线 TCP（InputPipeline 的主/备通道
                                    // 是原子指针，发送线程下一把就生效，不卡半天）
                                    impl_->inputPipe.set_backup_channel(&impl_->backupCtrl);
                                    impl_->inputPipe.set_backup_active(true);
                                    impl_->inputPipe.set_use_backup(true);
                                    // 初始路由 useTcp 由 §2.8 的 Config 显式传入
                                    MS_INFO("备用线路已就绪：%s:%u（键鼠已迁移到备线，主线全留给视频）",
                                            params.backupHost.c_str(), params.backupPort);
                                } else {
                                    MS_WARN("备用线路验证码错误，本次不用备线");
                                }
                            }
                        }
                    }
                }
            }
            if (!impl_->backupActive.load()) {
                MS_WARN("备用线路握手未完成，回退到主线传键鼠");
                impl_->backupCtrl.close();
            }
        } else {
            MS_WARN("备用线路连接失败（%s），回退到主线传键鼠", berr.c_str());
        }
    }

    // ---- 2.8 输入发送线程（P0 修复：初始化顺序）----
    // 必须在「会话认证 → 地址解析 → UDP 创建 → 备线握手」全部完成后启动：
    // 之前的版本在这里之前就启动了发送线程，它保存的还是无效 socket
    // (-1) 和未解析地址（sin_family==0），期间捕获到的事件全部静默丢弃。
    // 现在路由策略通过不可变的 Config 一次性显式传入，start() 不再覆盖。
    impl_->inputPipe.set_main_channel(&impl_->control.ch());
    {
        InputPipeline::Config ipCfg;
        ipCfg.control = &impl_->control.ch();
        ipCfg.sessionId = impl_->control.session_id();
        ipCfg.udpSock = impl_->receiver.udp_socket();
        ipCfg.serverAddr = impl_->serverAddr;
        // 初始路由：双线（备线就绪）= TCP 备线；单线 = UDP 主路径。
        ipCfg.useTcp = impl_->backupActive.load();
        impl_->inputPipe.start(ipCfg);
    }
    impl_->inputPipe.set_forwarding(false);

    // ---- 2.9 备线保活/重连线程（P0：备线要有完整生命周期）----
    // 职责：
    //   1) 备线已连接时：每 500ms 收包（Pong 等）+ 发输入心跳 Ping；
    //      连续 2 秒无进展（无 Pong 且无 InputApplied 推进）判定异常，
    //      关闭备线并把键鼠切回主线，随后自动重连备线。
    //   2) 备线未连接但已配置：每 3 秒尝试重连（指数退避，上限 10s）。
    //   3) 重连成功后提升输入 epoch（走主线的 ReleaseAll 屏障），
    //      再把键鼠迁回备线 —— 不跨连接重发旧事件，避免重复点击。
    if (!params.backupHost.empty() && params.backupPort != 0) {
        impl_->thBackupKeep = std::thread([impl = impl_] {
            const ms::PerfThreadScope perfScope(L"Pro Audio");
            try { impl->backup_keep_loop(); }
            catch (const std::exception& e) { MS_ERR("备线保活线程异常: %s", e.what()); }
            catch (...) { MS_ERR("备线保活线程未知异常"); }
        });
    }

    // ---- 3. 解码器 ----
    // 排障开关：MINISTREAM_FORCE_SW_DECODE=1 强制软解（用来判断卡死是否与
    // D3D11VA 硬解/显卡驱动有关；某些老显卡或虚拟显示器机器上硬解会卡住驱动）
    bool preferHw = true;
    {
        wchar_t v[8]{};
        if (::GetEnvironmentVariableW(L"MINISTREAM_FORCE_SW_DECODE", v, 8) > 0 &&
            v[0] != L'0' && v[0] != 0) {
            preferHw = false;
            MS_WARN("[排障] 已按环境变量强制软解（MINISTREAM_FORCE_SW_DECODE=1）");
        }
    }
    if (!impl_->decoder.init(impl_->stream.codec, impl_->stream.width, impl_->stream.height,
                             preferHw, err)) {
        impl_->control.send_disconnect("decoder init failed");
        net::shutdown();
        return fail("解码器初始化失败: " + err);
    }
    {
        std::lock_guard<std::mutex> lk(impl_->statMtx);
        impl_->status.decoderName = impl_->decoder.decoder_name();
        impl_->status.hardwareDecode = impl_->decoder.is_hardware();
    }
    MS_INFO("解码器就绪: %s (%s)", impl_->decoder.decoder_name().c_str(),
            impl_->decoder.is_hardware() ? "硬解" : "软解");

    // ---- 4. 渲染器（硬解时共享解码器的 D3D11 设备）----
    if (!impl_->renderer.init(hwnd, impl_->decoder.d3d11_device(), opts.vsync, err)) {
        impl_->control.send_disconnect("renderer init failed");
        net::shutdown();
        return fail("渲染器初始化失败: " + err);
    }
    impl_->renderer.set_overlay_enabled(opts.overlayEnabled);
    impl_->renderer.set_overlay_corner(static_cast<OverlayCorner>(
        opts.overlayCorner < 0 ? 0 : (opts.overlayCorner > 3 ? 3 : opts.overlayCorner)));
    // 锐化（游戏画面细节）：0=关 1=低 2=中 3=高
    {
        static const float kStrength[4] = { 0.0f, 0.35f, 0.70f, 1.20f };
        const int lv = opts.sharpen < 0 ? 0 : (opts.sharpen > 3 ? 3 : opts.sharpen);
        impl_->sharpenLevel = lv;
        impl_->renderer.set_sharpen(kStrength[lv]);
    }
    MS_INFO("渲染器就绪");

    // ---- 音频播放（被控端系统声音）----
    // 失败不致命：视频/键鼠照常，只是没声音（比如远控端机器没声卡）。
    impl_->audioEnabled = opts.audioEnabled;
    if (opts.audioEnabled) {
        std::string aerr;
        if (!impl_->audio.init(aerr)) {
            MS_WARN("音频播放初始化失败，本次会话没有声音: %s", aerr.c_str());
        }
    } else {
        MS_INFO("声音已在配置里关闭（[audio] enabled=false）");
    }

    // ---- 5. 键鼠捕获 ----
    // 排障开关：MINISTREAM_NO_INPUT=1 完全跳过键鼠捕获初始化（Raw Input /
    // ClipCursor / 钩子）。用来判断卡死是否与输入子系统或输入法注入有关。
    bool inputDisabled = false;
    {
        wchar_t v[8]{};
        if (::GetEnvironmentVariableW(L"MINISTREAM_NO_INPUT", v, 8) > 0 &&
            v[0] == L'1') {
            inputDisabled = true;
        }
    }
    if (inputDisabled) {
        MS_WARN("[诊断] 已按环境变量禁用键鼠捕获（MINISTREAM_NO_INPUT=1）");
    } else if (!impl_->input.init(hwnd, opts.releaseHotkey, err)) {
        MS_WARN("键鼠捕获初始化失败（仍可观看画面）: %s", err.c_str());
    } else if (opts.captureOnConnect) {
        impl_->input.set_captured(true);
        MS_INFO("键鼠捕获: 已开启");
    }

    // ---- 5.5 顶部工具栏（RustDesk 式：光标碰顶边滑出，不抢焦点）----
    if (impl_->toolbar.init(hwnd, err)) {
        // 【重要】nativeW/nativeH 要传真实采集分辨率，不能传 0：画质面板的
        // needReconnect 判定拿它和「跟随被控端」比较，传 0 会让"什么都没改"
        // 也判定为重连，点一次「应用」就黑一下、还会把面板收掉。
        impl_->toolbar.set_stream_params(impl_->stream.width, impl_->stream.height,
                                         impl_->stream.width, impl_->stream.height,
                                         impl_->stream.fps, int(impl_->stream.bitrateKbps));
        impl_->toolbar.set_sharpen_level(impl_->sharpenLevel);
        // 双线状态进工具栏：备线接入时显示「双线·键鼠走备线」
        impl_->toolbar.set_backup_line_status(impl_->backupActive.load());
        // 编码格式下拉跟随被控端实际使用的格式（host.ini 的 codec= 或上次切换）
        impl_->curCodec = (impl_->stream.codec == proto::VideoCodec::HEVC) ? 2 : 1;
        impl_->toolbar.set_codec_value(impl_->curCodec);
        SessionToolbar::Commands cmds;
        cmds.on_disconnect = [this] { impl_->stopReq.store(true); };
        cmds.on_toggle_fullscreen = [this, hwnd](bool fs) {
            // 全屏 <-> 窗口切换：改样式并调整位置
            if (fs) {
                HMONITOR mon = ::MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
                MONITORINFO mi{};
                mi.cbSize = sizeof(mi);
                ::GetMonitorInfoW(mon, &mi);
                ::SetWindowLongPtrW(hwnd, GWL_STYLE, WS_POPUP);
                ::SetWindowPos(hwnd, HWND_TOPMOST,
                               mi.rcMonitor.left, mi.rcMonitor.top,
                               mi.rcMonitor.right - mi.rcMonitor.left,
                               mi.rcMonitor.bottom - mi.rcMonitor.top,
                               SWP_FRAMECHANGED | SWP_SHOWWINDOW);
            } else {
                ::SetWindowLongPtrW(hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW);
                ::SetWindowPos(hwnd, HWND_NOTOPMOST, 100, 100, 1280, 720,
                               SWP_FRAMECHANGED | SWP_SHOWWINDOW);
            }
        };
        cmds.on_toggle_overlay = [this](bool on) {
            impl_->renderer.set_overlay_enabled(on);
        };
        // 声音开关：只是本机静音/取消静音，被控端一直发流，切回来立刻有声
        cmds.on_toggle_audio = [this](bool on) {
            impl_->audio.set_muted(!on);
        };
        // 显示缩放模式：智能缩放 / 拉伸填充 / 原始大小 1:1（工具栏按钮循环）
        cmds.on_scale_mode = [this](int mode) {
            if (mode < 0 || mode > 2) mode = 0;
            impl_->renderer.set_scale_mode(static_cast<ScaleMode>(mode));
            MS_INFO("显示模式 -> %s",
                    mode == 0 ? "智能缩放(黑边)" : (mode == 1 ? "拉伸填充" : "原始大小1:1"));
        };
        // 画质改动：码率可热调；分辨率/帧率需要重协商
        cmds.on_quality_change = [this](const quality::QualityProfile& q, bool needReconnect) {
            if (needReconnect) {
                MS_INFO("画质面板 -> 重协商 %dx%d @%dfps（码率 %d）",
                        q.width, q.height, q.fps, q.bitrateKbps);
                if (q.bitrateKbps > 0) impl_->control.send_set_bitrate(u32(q.bitrateKbps));
                impl_->control.send_reconfigure(q.width, q.height, q.fps);
                // 等服务端回 Reconfigured 后由 on_reconfigured 应用新参数
            } else {
                if (q.bitrateKbps > 0) {
                    MS_INFO("画质面板 -> 热调码率 %d kbps", q.bitrateKbps);
                    impl_->control.send_set_bitrate(u32(q.bitrateKbps));
                }
                if (q.keyframeIntervalSec != impl_->curKeyframeIntervalSec) {
                    impl_->curKeyframeIntervalSec = q.keyframeIntervalSec;
                    impl_->control.send_set_keyframe_interval(q.keyframeIntervalSec);
                }
            }
        };
        cmds.on_bitrate_change = [this](int kbps) {
            MS_INFO("工具栏滑条 -> 码率 %d kbps", kbps);
            impl_->control.send_set_bitrate(u32(kbps));
        };
        cmds.on_keyframe_interval_change = [this](int sec) {
            impl_->curKeyframeIntervalSec = sec;
            impl_->control.send_set_keyframe_interval(sec);
        };
        // 锐化：着色器 uniform 热切换，和编码/连接无关，改了立刻生效
        cmds.on_sharpen_change = [this](int level) {
            static const float kStrength[4] = { 0.0f, 0.35f, 0.70f, 1.20f };
            const int lv = level < 0 ? 0 : (level > 3 ? 3 : level);
            impl_->sharpenLevel = lv;
            impl_->renderer.set_sharpen(kStrength[lv]);
        };
        // 编码格式切换（H.264 <-> H.265）：被控端重建编码器，回 Reconfigured，
        // 我们这边 needDecoderReset 会等下一个 IDR 再重建解码器 —— 不用重连。
        cmds.on_codec_change = [this](int codec) {
            const int c = (codec == 2) ? 2 : 1;
            if (c == impl_->curCodec) return;
            MS_INFO("切换编码格式 -> %s（被控端重建编码器，等下一个 IDR）",
                    c == 2 ? "H.265/HEVC" : "H.264");
            impl_->curCodec = c;
            impl_->control.send_reconfigure(impl_->stream.width, impl_->stream.height,
                                            impl_->stream.fps, c);
        };
        impl_->toolbar.set_audio_enabled(impl_->audioEnabled &&
                                         impl_->audio.active() && !impl_->audio.failed());
        impl_->toolbar.set_commands(cmds);
        MS_INFO("顶部工具栏就绪");
    } else {
        MS_WARN("顶部工具栏初始化失败（不影响串流）: %s", err.c_str());
    }
    impl_->curKeyframeIntervalSec = 0;

    // 连接建立后：先显示「等待被控端发首帧」，收到第一帧切到正常悬浮条。
    // 状态推进用 firstFrameSeen 布尔，由主循环根据"是否已解码第一帧"驱动。
    impl_->status.connected = true;
    {
        std::lock_guard<std::mutex> lk(impl_->statMtx);
        impl_->status.width  = impl_->stream.width;
        impl_->status.height = impl_->stream.height;
        impl_->status.fps    = impl_->stream.fps;
    }
    impl_->renderer.draw_centered_message(
        L"已连接，等待被控端发首帧…\n\n"
        L"如果一直停在这里，说明 UDP 视频流被防火墙拦住了\n"
        L"（回到连接面板，点「防火墙已放行？点此放行」即可）");
    impl_->renderer.clear(0.05f, 0.05f, 0.07f);

    // 重协商完成回调：更新 stream 参数与工具栏状态显示
    // 掉线原因：控制通道断开时把被控端给的原因记下来（屏幕显示 + 日志）
    impl_->control.on_disconnected = [this](const std::string& why) {
        if (impl_->endReason.empty()) {
            impl_->endReason = why;
            MS_WARN("掉线: %s", why.c_str());
        }
        // 断开前让远端抬起所有按键/按钮（管线在连接关闭前尽力发一条 ReleaseAll）
        impl_->inputPipe.request_release_all();
    };

    impl_->control.on_reconfigured = [this](const StreamParams& sp) {
        MS_INFO("重协商完成: %dx%d @%dfps %ukbps codec=%d",
                sp.width, sp.height, sp.fps, sp.bitrateKbps, int(sp.codec));
        impl_->stream = sp;
        // 编码格式可能被切换了（H.264 <-> H.265）：同步下拉显示
        impl_->curCodec = (sp.codec == proto::VideoCodec::HEVC) ? 2 : 1;
        impl_->toolbar.set_codec_value(impl_->curCodec);
        // 换算基准可能变了（显示器调整/重协商到别的分辨率）
        impl_->renderer.set_desktop_origin(sp.monitorOriginX, sp.monitorOriginY);
        if (sp.desktopPhysW > 0 && sp.desktopPhysH > 0) {
            impl_->renderer.set_desktop_pixels(sp.desktopPhysW, sp.desktopPhysH);
        }
        impl_->toolbar.set_stream_params(sp.width, sp.height, sp.width, sp.height,
                                         sp.fps, int(sp.bitrateKbps));
        // 分辨率变了要重置解码器，等下一个 IDR
        impl_->needDecoderReset = true;
        // 重协商期间输入语义可能变化（分辨率换算基准变了），让远端清一遍状态
        impl_->remoteButtonsMask = 0;
        impl_->inputPipe.request_release_all();
    };

    // TCP 视频回退：控制线程收到整帧 -> 还原时间戳 -> 直接入播放队列。
    // 这条路径不丢片、不受防火墙影响，UDP 不可用时保证还有画面。
    impl_->control.on_video_frame = [this](u32 fid, bool kf, u32 ts32,
                                           const u8* data, size_t len) {
        const i64 sendRemote = rebuild_remote_ts(impl_->control.clock(), ts32, now_us());
        Bytes buf(data, data + len);
        impl_->receiver.push_tcp_frame(fid, kf, sendRemote, buf);
        // 探测窗口的分母：TCP 帧在窗口内一定收得到（TCP 不丢）
        impl_->tcpFramesRecv.fetch_add(1, std::memory_order_relaxed);
    };

    // 可靠输入回执（v2）：第一个回执说明服务端认识 InputBatch。
    // 旧版服务端不回执 -> batchPipe 超时自动回退 UDP 老路径（见主循环的检查）。
    impl_->control.on_input_applied = [this](u32 bid, u32 applied, u8 status) {
        if (!impl_->inputPipe.server_acked()) {
            MS_INFO("服务端支持可靠输入（首条回执 batch=%u applied=%u status=%u）",
                    bid, applied, unsigned(status));
        }
        impl_->inputPipe.on_input_applied(bid, applied, status);
    };

    MS_INFO("初始化完毕，进入主循环");

    // ---- 6. 控制线程 ----
    // 线程函数不许把异常抛出去：MSVC 下线程里逃逸的异常不会走 std::set_terminate，
    // 而是 __std_terminate -> abort，进程直接以 0xC0000409 消失。
    impl_->thControl = std::thread([this] {
        try {
            impl_->control_loop();
        } catch (const std::exception& e) {
            MS_ERR("控制线程异常: %s（已抑制）", e.what());
        } catch (...) {
            MS_ERR("控制线程未知异常（已抑制）");
        }
    });

    // ---- 7. 主循环：消息 + 解码 + 渲染 ----
    bool firstKeyframe = false;
    // 【都别用 0 当哨兵】时钟一旦返回非正数，所有 "now - x > N" 判断都会失效；
    // 用真实时刻初始化，语义也更准（统计从进入主循环开始算）。
    i64 lastStatsUs = now_us();
    i64 lastFrameRecvUs = now_us();
    i64 lastKeepaliveUs = now_us();
    impl_->sessionStartUs = now_us();
    impl_->lastTransportTickUs = impl_->sessionStartUs;
    // 开局 3 秒内不判「UDP 不通」：首帧本来就是完整 IDR，要 1~2 秒才到。
    impl_->udpQuarantineUntilUs = impl_->sessionStartUs + 3000000;
    {
        wchar_t v[8]{};
        if (::GetEnvironmentVariableW(L"MINISTREAM_FORCE_TCP_VIDEO", v, 8) > 0 && v[0] == L'1') {
            impl_->forceTcp = true;
            MS_WARN("[排障] 已强制 TCP 传视频（MINISTREAM_FORCE_TCP_VIDEO=1）");
        }
        if (::GetEnvironmentVariableW(L"MINISTREAM_TEST_TCP_START", v, 8) > 0 && v[0] == L'1') {
            impl_->testTcpStart = true;
            MS_WARN("[自测] 开局将强制切 TCP，用来验证 UDP 自愈能否切回来");
        }
        // 自测用：把顶部提示条拉长（截图脚本启动慢，默认 3.2 秒容易截不到）
        if (::GetEnvironmentVariableW(L"MINISTREAM_TEST_TOAST_MS", v, 8) > 0) {
            const int ms = _wtoi(v);
            if (ms > 0) impl_->toastMs = ms;
        }
    }
    int statTicks = 0;
    RateCounter recvRate;
    // 可靠输入握手窗口：连接后 2 秒内没收到任何 InputApplied 就回退 UDP 老路径
    // （旧版被控端不认识 InputBatch 消息，静默丢弃）。回退只做一次。
    const i64 batchProbeUntilUs = now_us() + 2000000;
    bool inputFallbackDone = false;

    while (!impl_->stopReq.load()) {
        // 等消息或最多 2ms（收到键鼠消息会立刻醒来 -> 输入延迟 ~0）
        ::MsgWaitForMultipleObjects(0, nullptr, FALSE, 2, QS_ALLINPUT);

        MSG msg;
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { impl_->stopReq.store(true); break; }

            // 键鼠相关消息优先交给捕获模块（Raw Input 必须被消费，否则系统光标会动）
            bool consumed = false;
            switch (msg.message) {
                case WM_INPUT:
                case WM_KEYDOWN:
                case WM_SYSKEYDOWN:
                case WM_KEYUP:
                case WM_SYSKEYUP:
                case WM_SETFOCUS:
                    consumed = impl_->input.handle_message(msg.message, msg.wParam, msg.lParam);
                    break;
                default:
                    break;
            }
            if (consumed) continue;

            // 工具栏显示时发 WM_APP+0x520：wParam=1 暂时显示本机光标，0 交还
            if (msg.message == WM_APP + 0x520) {
                impl_->input.temporary_show_cursor(msg.wParam == 1);
                continue;
            }

            // 点画面 = 收起画质面板（点工具栏/面板自身时消息进的是它们的窗口，
            // 不会走到这里，所以这是"点外面关面板"的准确判定）。
            if ((msg.message == WM_LBUTTONDOWN || msg.message == WM_LBUTTONDBLCLK) &&
                msg.hwnd == hwnd) {
                impl_->toolbar.collapse_panel();
            }

            // 未捕获时按 ESC 退出会话
            if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE && !impl_->input.captured()) {
                impl_->stopReq.store(true);
                break;
            }
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
        if (!impl_->control.is_open()) break;

        // 输入通道握手窗口：默认已经走 UDP（3 份冗余），这里只做一次兼容性探测：
        // 被控端是否会回 InputApplied。会回执说明它支持 TCP InputBatch，
        // 万一以后 UDP 完全不可用（视频切 TCP 时）可以无缝回落到控制通道。
        if (!inputFallbackDone && now_us() > batchProbeUntilUs) {
            inputFallbackDone = true;
            if (!impl_->inputPipe.server_acked()) {
                impl_->inputPipe.set_server_supports_batch(false);
                MS_INFO("被控端未回执 InputBatch（旧版本）：输入保持 UDP + 3 份冗余路径"
                        "（本来就是主路径，不影响使用）");
            } else {
                MS_INFO("被控端支持 TCP InputBatch（仅作为 UDP 完全不通时的回落路径）");
            }
        }
        // 捕获状态 -> 管线转发开关（未捕获时不转发普通输入，ReleaseAll 照发）
        impl_->inputPipe.set_forwarding(impl_->input.captured());

        // 释放快捷键：切换捕获状态
        if (impl_->input.take_release_request()) {
            const bool nowCaptured = !impl_->input.captured();
            impl_->input.set_captured(nowCaptured);
            MS_INFO("键鼠捕获 -> %s", nowCaptured ? "开启" : "关闭");
            // 退出捕获：无论传输路径，立刻让远端抬起全部按键/按钮。
            // ReleaseAll 由输入管线独立发送（不受捕获开关影响），
            // 避免"松键通知卡在队列里、远端一直按键"的老问题。
            if (!nowCaptured) {
                impl_->remoteButtonsMask = 0;
                impl_->inputPipe.request_release_all();
                // 【RustDesk 式联动】释放键鼠 = 唤出顶部工具栏；重新捕获 = 收回。
                // 捕获状态下用户按释放键就是为了操作面板/退出，直接亮出来最顺手；
                // 也解决了「捕获中 ClipCursor 钉住光标，根本到不了顶边」的死结。
                impl_->toolbar.summon();
            }
        }

        // ---- 顶部工具栏：光标碰顶边滑出 / 离开 3 秒自动隐藏 ----
        // 捕获状态下光标被 ClipCursor 限制在客户区内，"碰顶边"就是客户区顶边：
        // 判定用客户区坐标（GetClientRect + ClientToScreen），而不是窗口边框。
        // 窗口模式下用户把光标推到客户区最顶端；全屏无边框模式下客户区顶边就是屏幕顶边。
        {
            POINT pt{};
            if (::GetCursorPos(&pt)) {
                RECT cr{};
                ::GetClientRect(hwnd, &cr);
                POINT tl{cr.left, cr.top};
                ::ClientToScreen(hwnd, &tl);
                RECT wr{};
                ::GetWindowRect(hwnd, &wr);
                // 触发区域放宽到 60px：用户把光标推到客户区顶部区域即可唤出，
                // 8px 太窄容易摸不到（尤其窗口模式下标题栏占了上面一段）。
                const bool atTop = (pt.y <= tl.y + 60) &&
                                   (pt.x >= tl.x && pt.x <= tl.x + (cr.right - cr.left));
                if (atTop) {
                    impl_->toolbar.on_mouse_move(pt);
                }
            }
            impl_->toolbar.poll_auto_hide();
        }

        // ---- 键鼠输入（v2）：入队 -> 专用发送线程经 TCP 控制通道可靠送达 ----
        // 主循环只做事件过滤，不再自己组 UDP 包；发送、合并、批号都由
        // InputPipeline 负责（诊断方案第一阶段：点击/按键走可靠传输）。
        if (impl_->input.captured()) {
            proto::InputEvent evs[128];
            const size_t n = impl_->input.poll_events(evs, 128);
            // 光标停在本机界面（工具栏/画质面板/小悬浮按钮）上时，不要把鼠标
            // 事件发给远端：否则点一下"全屏/画质"，游戏里也会跟着点一下
            // （FPS 游戏里等于误开火），键盘照常转发。
            // 例外：按钮归属在远端的「抬起」必须照发 —— 远端拖拽进行中，
            // 光标移到工具栏上松开，抬起被过滤会让远端按钮卡在按下状态。
            size_t outN = 0;
            const bool overUi = (n > 0) && impl_->toolbar.cursor_over_ui();
            for (size_t i = 0; i < n; ++i) {
                const auto t = proto::InputType(evs[i].type);
                if (overUi && (t == proto::InputType::MouseMove ||
                               t == proto::InputType::MouseButton ||
                               t == proto::InputType::MouseWheel)) {
                    if (t == proto::InputType::MouseButton && evs[i].b == 0) {
                        const int btn = int(evs[i].a);
                        if (btn >= 1 && btn <= 5 &&
                            (impl_->remoteButtonsMask & (1u << (btn - 1))) != 0) {
                            // 远端开始的拖拽：抬起放行
                            impl_->remoteButtonsMask &= ~(1u << (btn - 1));
                            evs[outN++] = evs[i];
                        }
                    }
                    continue;
                }
                // 登记按钮归属：按下 = 归远端；抬起 = 清掉
                if (t == proto::InputType::MouseButton && !overUi) {
                    const int btn = int(evs[i].a);
                    if (btn >= 1 && btn <= 5) {
                        if (evs[i].b != 0) impl_->remoteButtonsMask |= (1u << (btn - 1));
                        else               impl_->remoteButtonsMask &= ~(1u << (btn - 1));
                    }
                }
                evs[outN++] = evs[i];
            }
            for (size_t i = 0; i < outN; ++i) {
                impl_->inputPipe.enqueue(evs[i]);
            }
        }

        // 定期发 UDP 保活：维持 NAT / FRP 的 UDP 映射不被回收
        if (now_us() - lastKeepaliveUs > 2000000) {
            lastKeepaliveUs = now_us();
            std::vector<u8> pkt(sizeof(proto::UdpHeader));
            proto::UdpHeader h{};
            h.type = u8(proto::UdpType::Keepalive);
            h.sessionId = impl_->control.session_id();
            std::memcpy(pkt.data(), &h, sizeof(h));
            net::udp_send_to(impl_->receiver.udp_socket(), pkt.data(), pkt.size(),
                             impl_->serverAddr);
        }

        // ---- 远端光标：UDP 优先（低延迟），TCP 兜底（UDP 不通时也能看到鼠标）----
        // 被控端把「系统光标是否可见」如实发过来：游戏菜单里可见、进内场游戏
        // 自己把光标藏起来 -> 这里跟着不画。完全复刻本机玩游戏的体验。
        {
            proto::PointerPosMsg pm;
            bool got = false;
            if (impl_->receiver.take_pointer(pm)) got = true;
            proto::PointerPosMsg pmTcp;
            if (impl_->control.take_pointer_pos(pmTcp)) {
                // 两路都到了：按 seq 取更新的那份（UDP 快，TCP 只是兜底）
                if (!got || i32(u32(pmTcp.seq - pm.seq)) > 0) { pm = pmTcp; got = true; }
            }
            if (got) {
                if (!impl_->havePointer) {
                    MS_INFO("收到远端光标位置: (%d,%d) visible=%d", int(pm.x), int(pm.y),
                            int(pm.visible));
                } else if (impl_->lastPointer.visible != pm.visible) {
                    MS_INFO("远端光标可见性 -> %s", pm.visible
                            ? "显示（桌面/游戏菜单）"
                            : "隐藏（游戏内场或全屏应用把系统光标藏起来了）");
                }
                impl_->havePointer = true;
                impl_->lastPointer = pm;
                impl_->renderer.set_cursor_pos(pm.x, pm.y, pm.visible != 0);
            }
            proto::PointerShapeMsg sm;
            if (impl_->control.take_pointer_shape(sm)) {
                impl_->renderer.set_cursor_shape(int(sm.width), int(sm.height),
                                                 int(sm.hotX), int(sm.hotY), sm.bgra);
                MS_INFO("收到远端光标形状: %dx%d 热点(%d,%d) %u 字节",
                        int(sm.width), int(sm.height), int(sm.hotX), int(sm.hotY),
                        unsigned(sm.bgra.size()));
            }
            // ---- 鼠标模式：被控端光标可见 = 绝对映射（1:1），隐藏 = 相对（游戏内场）----
            impl_->update_mouse_mode(now_us());

            if (impl_->mouseEffective == 1 && impl_->input.captured() &&
                !impl_->toolbar.cursor_over_ui()) {
                // 绝对映射：把本机鼠标位置换算成远端像素坐标发过去（被控端 SetCursorPos）。
                // 8ms 节流（125Hz）足够跟手，也不会把输入通道打满。
                // （光标停在本机工具栏/面板上时不发：那是在操作本机界面，别把
                //   远端光标也拽到屏幕顶上去。）
                const i64 nowAbs = now_us();
                if (nowAbs - impl_->lastAbsSendUs > 8000) {
                    POINT cp{};
                    if (::GetCursorPos(&cp)) {
                        POINT cli = cp;
                        if (::ScreenToClient(hwnd, &cli)) {
                            int vx = 0, vy = 0;
                            bool inside = false;
                            if (impl_->renderer.client_to_video(cli, vx, vy, inside) && inside &&
                                (vx != impl_->lastAbsX || vy != impl_->lastAbsY)) {
                                impl_->send_abs_mouse(vx, vy);
                                impl_->lastAbsX = vx;
                                impl_->lastAbsY = vy;
                                impl_->lastAbsSendUs = nowAbs;
                            }
                        }
                    }
                }
            }

            // 本机光标：相对模式下藏掉本机的（我们画远端光标）；
            // 绝对模式下本机光标就是那个光标，必须可见。
            const bool hideLocal = impl_->mouseEffective == 2 &&
                                   impl_->renderer.cursor_ready() &&
                                   !impl_->toolbar.visible();
            g_hide_local_cursor.store(hideLocal, std::memory_order_relaxed);
        }

        // ---- 声音：UDP 优先（低延迟），TCP 兜底（视频切 TCP 时也不断）----
        if (impl_->audio.active() && !impl_->audio.failed()) {
            AudioChunk ac;
            int guard = 0;
            while (++guard < 128 && impl_->receiver.take_audio(ac)) {
                impl_->audio.push(ac);
            }
            proto::AudioPacket ap;
            guard = 0;
            while (++guard < 128 && impl_->control.take_audio(ap)) {
                impl_->audio.push(ap);
            }
            impl_->audio.tick();
        }

        // ---- 传输自愈：UDP 不通/烂到没法玩 -> TCP；TCP 下周期性探测 -> 能回就回 ----
        {
            if (impl_->forceTcp && impl_->linkMode == 0) {
                impl_->switch_to_tcp("排障模式强制 TCP");
            } else if (impl_->testTcpStart && !impl_->testTcpStarted &&
                       now_us() - impl_->sessionStartUs > 800000) {
                impl_->testTcpStarted = true;
                MS_WARN("[自测] 开局强制切 TCP，验证 UDP 自愈能不能自己切回来");
                impl_->switch_to_tcp("自测钩子");
            }
            impl_->tick_transport(now_us());
        }

        // 取出重组好的完整帧。
        //
        // 【抗花屏的核心状态机】H.264 的 P 帧参考前一帧：只要中间丢了一帧，
        // 后面所有帧都会解错 —— 玩家看到的就是「画面局部花屏」，一直烂到下一个 IDR。
        // 所以这里照抄 Moonlight 的做法（VideoDepacketizer 的 waitingForIdrFrame）：
        //   帧号不连续 / 接收端报告丢帧 / 解码报错  -> 进入 WAIT_IDR
        //   WAIT_IDR 期间：所有非关键帧一律丢弃（绝不喂解码器），并节流请求 IDR
        //   收到关键帧 -> 解除等待，解码链重新对齐
        // 代价是丢包后画面冻结一小会儿（等 IDR，通常 1~2 帧），换来的是**永不花屏**。
        //
        // 另外：IDR 请求不在丢包瞬间发，而是等「丢包后第一个完整帧」到了再发。
        // 这是 Moonlight 踩过坑的地方 —— 丢包瞬间求 IDR 会造成
        // 「IDR 尖峰 → 更多丢包」的拥塞崩溃，反而越救越卡。
        ReceivedFrame rf;
        while (impl_->receiver.pop(rf, 0)) {
            recvRate.add(rf.data.size());
            lastFrameRecvUs = now_us();
            ++impl_->framesSinceIdr_;

            // 重协商刚完成：编码器换了新参数，必须丢到下一个 IDR 才能继续解码，
            // 否则旧解码器拿新分辨率的流去解必花屏。
            if (impl_->needDecoderReset) {
                if (!rf.keyframe) {
                    impl_->waitingIdr = true;
                    impl_->control.request_keyframe(2);
                    continue;
                }
                impl_->decoder.flush();
                impl_->decoder.shutdown();
                std::string derr;
                if (!impl_->decoder.init(impl_->stream.codec, impl_->stream.width,
                                         impl_->stream.height, true, derr)) {
                    MS_WARN("重协商后解码器重建失败: %s", derr.c_str());
                } else {
                    impl_->needDecoderReset = false;
                    impl_->haveFedFrame = false;
                    MS_INFO("解码器已按新参数重建 (%dx%d)",
                            impl_->stream.width, impl_->stream.height);
                }
            }

            // ---- 帧号连续性检查 ---------------------------------------------
            if (!impl_->waitingIdr && impl_->haveFedFrame) {
                if (rf.frameId != impl_->lastFedFrameId + 1u) {
                    // 中间有帧不见了（FEC 也救不回来 / 队列溢出 / 换传输的空档）。
                    // 这时后续 P 帧的参考链已经断了，继续解 = 花屏。
                    impl_->waitingIdr = true;
                    if (impl_->waitingSinceUs == 0) impl_->waitingSinceUs = now_us();
                    static i64 lastGapLogUs = 0;
                    if (now_us() - lastGapLogUs > 2000000) {   // 丢包风暴时别刷屏
                        lastGapLogUs = now_us();
                        MS_INFO("帧号不连续（上一帧 %u，本帧 %u，跳了 %d），"
                                "已暂停解码并等 IDR —— 宁冻结不花屏",
                                impl_->lastFedFrameId, rf.frameId,
                                int(i32(rf.frameId - impl_->lastFedFrameId)) - 1);
                    }
                }
            }

            if (impl_->waitingIdr) {
                if (!rf.keyframe) {
                    // 还在等 IDR：这一帧丢掉，但把"该求 IDR"记下来，
                    // 由下面「收到完整帧之后再求」的统一逻辑节流发送。
                    impl_->pendingIdrRequest = true;
                    continue;
                }
                impl_->waitingIdr = false;
                impl_->waitingSinceUs = 0;
                impl_->haveFedFrame = false;
                impl_->framesSinceIdr_ = 0;
                if (!firstKeyframe) {
                    firstKeyframe = true;
                    impl_->renderer.clear_message();   // 收到首帧 -> 切回悬浮条
                }
            }

            // 估算端到端：采集 -> 即将呈现
            const i64 sendRemote = rebuild_remote_ts(impl_->control.clock(),
                                                     u32(rf.sendTsUs), now_us());
            const i64 latencyUs = now_us() - impl_->control.clock().remote_to_local(sendRemote);

            const i64 t0 = now_us();
            const int sr = impl_->decoder.send(rf.data.data(), rf.data.size(), sendRemote);
            if (sr < 0) {
                // 解码器报错：位流坏了/参考帧丢了。清掉解码器状态 + 等 IDR。
                impl_->waitingIdr = true;
                if (impl_->waitingSinceUs == 0) impl_->waitingSinceUs = now_us();
                impl_->haveFedFrame = false;
                impl_->pendingIdrRequest = true;
                impl_->decoder.flush();
            } else {
                impl_->haveFedFrame = true;
                impl_->lastFedFrameId = rf.frameId;
                AVFrame* af = nullptr;
                int rr;
                i64 presentTotal = 0;
                while ((rr = impl_->decoder.receive(af, 0)) == 1) {
                    i64 presentUs = 0;
                    impl_->renderer.present(af, &presentUs);
                    impl_->decoder.release(af);
                    ++impl_->framesDecoded;
                    impl_->presentStats.add(presentUs);
                    presentTotal += presentUs;
                }
                // 解码耗时 = 本轮总耗时 - 呈现耗时（比减平均值准确，也不会算出负数）
                const i64 total = now_us() - t0;
                impl_->decodeStats.add(total > presentTotal ? total - presentTotal : 0);
                impl_->latencyStats.add(latencyUs);

                // ---- 网络排队延迟（RustDesk QoS 的核心信号）----
                // 端到端延迟里扣掉"近期最好值"就是链路上堆积的排队量：
                // 它比丢包更早发现拥塞（bufferbloat 时延迟先涨、丢包后到），
                // 让我们能在开始花屏之前就把码率降下来。
                // 基线每 30 秒没被刷新就缓慢上抬（链路可能永久变差了）。
                {
                    const i64 nowLoop = now_us();
                    if (impl_->latencyBestUs == 0 || latencyUs < impl_->latencyBestUs) {
                        impl_->latencyBestUs = latencyUs;
                        impl_->latencyBestAtUs = nowLoop;
                    } else if (nowLoop - impl_->latencyBestAtUs > 30000000) {
                        impl_->latencyBestUs += (latencyUs - impl_->latencyBestUs) / 4;
                        impl_->latencyBestAtUs = nowLoop;
                    }
                    const i64 excess = latencyUs - impl_->latencyBestUs;
                    const double instQueueMs = excess > 0 ? double(excess) / 1000.0 : 0.0;
                    impl_->queueDelayMs = impl_->queueDelayMs * 0.8 + instQueueMs * 0.2;
                    impl_->sample_latency(latencyUs);
                }
                if (rr < 0) {
                    impl_->waitingIdr = true;
                    impl_->haveFedFrame = false;
                    impl_->pendingIdrRequest = true;
                    impl_->renderer.draw_centered_message(
                        L"已收到视频流，但解码失败\n\n"
                        L"把日志级别调到 1（--log-level 1）重连一次\n"
                        L"把 client.log 最后 30 行发给开发者排查");
                }
            }
        }

        // ---- IDR 请求节流（丢包后等第一个完整帧再发，避免拥塞崩溃）--------
        // 120 帧（60fps≈2 秒）还没等到 IDR 就强制求一次，避免请求本身丢在网络上。
        if (impl_->pendingIdrRequest && impl_->framesSinceIdr_ > 0 &&
            now_us() - impl_->lastIdrReqUs > 100000) {
            impl_->pendingIdrRequest = false;
            impl_->lastIdrReqUs = now_us();
            impl_->control.request_keyframe(2);
        }
        if (impl_->waitingIdr && impl_->framesSinceIdr_ > 120 &&
            now_us() - impl_->lastIdrReqUs > 500000) {
            impl_->lastIdrReqUs = now_us();
            impl_->control.request_keyframe(2);
        }

        // 接收端自己丢了帧（缺片超时/队列溢出）-> 同样要等 IDR，否则必花屏。
        // 但 TCP 模式下的丢帧来自"UDP 探测窗口"，和画面无关，不能因此停解码。
        if (impl_->receiver.take_keyframe_request()) {
            if (impl_->linkMode == 0) {
                impl_->waitingIdr = true;
                if (impl_->waitingSinceUs == 0) impl_->waitingSinceUs = now_us();
                impl_->pendingIdrRequest = true;
            }
        }

        // 画面长时间没动静也要刷新悬浮条
        if (now_us() - lastStatsUs > 500000) {
            lastStatsUs = now_us();
            const auto rs = impl_->receiver.stats();

            // 【近期丢帧率】rs.lossPercent 是整个会话的累计值，开头上网差一阵就会
            // 一直显示很高、自适应码率也永远回不去。这里用 3 秒半衰的 EMA
            // 表示"现在这条链路怎么样"，UI 和自适应都用它。
            static u64 emaOk = 0, emaLost = 0;
            {
                const u64 okD   = rs.framesOk   - emaOk;
                const u64 lostD = rs.framesLost - emaLost;
                emaOk   = rs.framesOk;
                emaLost = rs.framesLost;
                const double inst = (okD + lostD) > 0
                    ? double(lostD) * 100.0 / double(okD + lostD) : 0.0;
                impl_->lossEma = impl_->lossEma * 0.7 + inst * 0.3;
            }
            const double lossNow = impl_->lossEma;

            // 【注意】丢帧不再直接切 TCP：
            // 现在有 FEC 兜底 + 自适应码率 + 「缺帧等 IDR」，偶发丢包影响很小；
            // 为一点抖动切到 TCP 反而把延迟拉高一个数量级。只有"真的不通"
            // 才切，判断在 tick_transport() 里（滞回 + 冻结期，切了也不会来回抖）。

            // ---- 自适应码率：AIMD，按「每秒」而不是「每 RTT」----
            // 依据（UU远程/GameViewer 逆向报告，22,244 个逐秒样本实测）：
            //   无丢包且无排队增长 ×1.08/秒；排队时延上升 ×0.90/秒；
            //   丢包 4~13% ×0.94/秒；丢包 ≥15% ×0.80~0.86/秒；下降后 2 秒内不准升。
            // 报告特别强调：写成"每 RTT +5%"是错的（RTT 40ms 时一秒 25 个 RTT，会翻 3 倍）。
            // 预算取协商码率的 85%，剩下 15% 留给 FEC 校验包（典型 5%~25% 开销）。
            {
                static i64 lastAdaptUs = 0;
                static double curBr = 0.0;
                static u32 lastSentBr = 0;
                static i64 noRaiseUntilUs = 0;
                const double cap = double(impl_->stream.bitrateKbps > 0
                                              ? impl_->stream.bitrateKbps : 20000) * 0.85;
                // 起步不要一上来就顶满：先给 55%，让它自己 ×1.08/秒 爬上去
                // （约 6 秒到顶），避免开局就把链路塞满丢一串包。
                if (curBr <= 0.0) curBr = cap * 0.55;
                if (now_us() - lastAdaptUs > 1000000) {
                    static double lastQ = 0.0;
                    lastAdaptUs = now_us();
                    const double q = impl_->queueDelayMs;
                    const bool qRising = q > lastQ + 10.0;
                    lastQ = q;
                    const double before = curBr;
                    // 控制律对齐 RustDesk video_qos.rs:227-248 / :775-795：
                    //   * 排队延迟是主信号（TCP/UDP 都有效），分档砍：500ms→0.5、
                    //     300ms→0.7、150ms→0.9、80ms 且在涨→0.93；
                    //   * 丢包只作为补充（我们还有 FEC/NACK，不能一丢就砍穿）；
                    //   * 增长 ×1.10/秒，且每秒最多 +450 kbps（RustDesk 是 +150/3s 步长，
                    //     我们 60fps 游戏画面略放宽），避免"慢慢爬"变成"暴涨"。
                    if (lossNow >= 15.0) {
                        curBr *= 0.70;                       // 重丢包
                    } else if (lossNow >= 4.0) {
                        curBr *= 0.90;
                    } else if (q >= 500.0) {
                        curBr *= 0.50;                       // 严重排队
                    } else if (q >= 300.0) {
                        curBr *= 0.70;
                    } else if (q >= 150.0) {
                        curBr *= 0.90;
                    } else if (q >= 80.0 && qRising) {
                        curBr *= 0.93;                       // 开始积压，先小步退
                    } else if (lossNow < 1.5 && q < 60.0 && now_us() > noRaiseUntilUs) {
                        double grown = curBr * 1.10;
                        if (grown > curBr + 450.0) grown = curBr + 450.0;
                        curBr = grown;                       // 健康：缓慢爬升
                    }
                    if (curBr > cap) curBr = cap;
                    if (curBr < 2500.0) curBr = 2500.0;      // 地板：再低 1080p 没法看
                    if (curBr < before) noRaiseUntilUs = now_us() + 2000000;  // 降额冷却 2s
                    const u32 want = u32(curBr);
                    // 变化超过 4% 才下发，减少重配置次数（报告实测 median 间隔 1.75s）
                    if (lastSentBr == 0 || want > lastSentBr * 104 / 100 ||
                        want * 100 < lastSentBr * 96) {
                        lastSentBr = want;
                        impl_->control.send_set_bitrate(want);
                        MS_INFO("自适应码率 -> %u kbps（近期丢帧 %.1f%%，排队延迟 %.0f ms）",
                                want, lossNow, impl_->queueDelayMs);
                    }
                }
            }

            // 「已连接但一个 UDP 包都没进来」= 防火墙拦了入站 UDP。
            // 这是内网"已连接但黑屏"的头号原因，必须把原因打在屏幕上。
            // （已经切到 TCP 兜底时不再提示：那时画面是好的，提示只会误导）
            if (impl_->linkMode == 0 && impl_->framesDecoded == 0 && rs.packets == 0 &&
                impl_->receiver.packets_total() == 0) {
                static i64 fwHintAt = now_us();
                if (now_us() - fwHintAt > 3000000) {
                    fwHintAt = now_us();
                    MS_WARN("[诊断] 已连接 3 秒但一个 UDP 包都没收到："
                            "防火墙很可能拦了入站 UDP（内网黑屏的头号原因）。"
                            "在连接面板点「防火墙已放行？点此放行」，或以管理员运行一次客户端。");
                }
                impl_->renderer.draw_centered_message(
                    L"已连接，但收不到视频流（UDP 被防火墙拦住了）\n\n"
                    L"解决：回到连接面板，点「防火墙已放行？点此放行」\n"
                    L"（需要管理员权限；或右键以管理员运行一次本程序，它会自动放行）\n\n"
                    L"原因：被控端发视频用的 UDP，Windows 默认会拦");
            } else if (impl_->linkMode == 0 && impl_->framesDecoded == 0 && rs.packets > 0) {
                // 有包但没解码出帧：要么是首帧 IDR 还在路上，要么是来源/session 过滤丢包
                impl_->renderer.draw_centered_message(
                    L"已连接，收到视频包但还在等第一帧…\n\n"
                    L"（首帧是完整的关键帧，可能要 1~2 秒）");
            } else if (impl_->linkMode == 1 && impl_->framesDecoded == 0) {
                impl_->renderer.draw_centered_message(
                    L"UDP 不通，已改用 TCP 传视频，正在等首帧…\n\n"
                    L"（TCP 通道已建立，画面马上出来）");
            }
            // framesDecoded>0 时由 clear_message() 切回悬浮条，这里不用动
            const double recvFps = rs.fps > 0 ? rs.fps : recvRate.rate();
            const double recvKbps = rs.kbps > 0 ? rs.kbps : recvRate.kbps();
            const double latencyMs = double(impl_->latencyStats.avg()) / 1000.0;
            const double decodeMs = double(impl_->decodeStats.avg()) / 1000.0;
            const double presentMs = double(impl_->presentStats.avg()) / 1000.0;
            const int rttMs = int(impl_->control.rtt_us() / 1000);
            const int qd = int(impl_->receiver.queue_depth());
            static u64 lastOvSample = 0;
            const u64 ovDelta = rs.framesOverwritten - lastOvSample;
            lastOvSample = rs.framesOverwritten;
            {
                std::lock_guard<std::mutex> lk(impl_->statMtx);
                impl_->status.recvFps = recvFps;
                impl_->status.recvKbps = recvKbps;
                impl_->status.lossPercent = lossNow;
                impl_->status.latencyMs = latencyMs;
                impl_->status.decodeMs = decodeMs;
                impl_->status.rttMs = rttMs;
                impl_->status.framesDecoded = impl_->framesDecoded;
                impl_->status.framesDropped = rs.framesLost;
                impl_->status.queueDepth = qd;
            }
            OverlayStats os;
            os.fps = recvFps;
            os.kbps = recvKbps;
            os.lossPercent = lossNow;
            os.latencyMs = latencyMs;
            os.queueDepth = qd;
            os.rttMs = rttMs;
            os.decodeMs = decodeMs;
            os.hardwareDecode = impl_->decoder.is_hardware();
            os.extra = impl_->decoder.decoder_name();
            impl_->renderer.draw_overlay(os);

            // 每 2 秒往日志写一行汇总，方便事后核对（也是自动化实测的观测点）
            if (++statTicks % 4 == 0) {
                // 分片级丢包率：直接反映链路质量（帧级丢包率被 FEC 拉低过，不好判断）
                const double pktLoss = rs.dataPktsNeeded > 0
                    ? 100.0 * double(rs.dataPktsNeeded - rs.dataPktsRecv) /
                      double(rs.dataPktsNeeded) : 0.0;
                MS_INFO("[串流] %dx%d | %s | 接收 %.1f fps %.0f kbps 丢帧 %.1f%% 分片丢 %.1f%% | "
                        "FEC 校验包 %llu 恢复 %llu 片 | NACK %llu 救回 %llu 帧 | 队列溢出 +%llu | "
                        "解码 %s %.2fms 呈现 %.2fms | 端到端 %.1f ms 排队 %.0f ms | "
                        "RTT %.2f ms | 队列 %d | 已解码 %llu 帧",
                        impl_->stream.width, impl_->stream.height,
                        impl_->linkMode == 1 ? "TCP兜底" : "UDP",
                        recvFps, recvKbps, lossNow, pktLoss,
                        (unsigned long long)rs.fecReceived,
                        (unsigned long long)rs.fecRecovered,
                        (unsigned long long)rs.nackSent,
                        (unsigned long long)rs.nackRecovered,
                        (unsigned long long)ovDelta,
                        impl_->decoder.is_hardware() ? "硬解" : "软解", decodeMs, presentMs,
                        latencyMs, impl_->queueDelayMs,
                        double(rttMs) == 0 ? double(impl_->control.rtt_us()) / 1000.0
                                                      : double(rttMs),
                        qd, (unsigned long long)impl_->framesDecoded);

                // 延迟分位数 + jank 分级（验收口径见 UU远程/GameViewer 报告 §8.3/8.4）
                int p50 = 0, p90 = 0, p99 = 0;
                impl_->latency_percentiles(p50, p90, p99);
                MS_INFO("[延迟] p50 %d ms · p90 %d ms · p99 %d ms | jank 小 %u / 中 %u / 大 %u",
                        p50, p90, p99,
                        impl_->jankSmall, impl_->jankMedium, impl_->jankBig);
            }

            // 工具栏状态行只补"实时指标"——分辨率/帧率/码率由 set_stream_params 负责，
            // 两边都写会把同一串信息重复显示（曾经的实际 bug）。
            char line[160];
            std::snprintf(line, sizeof(line), "延迟 %.1f ms · 丢帧 %.1f%% · 队列 %d · RTT %.1f ms%s%s",
                          latencyMs, lossNow, qd,
                          double(impl_->control.rtt_us()) / 1000.0,
                          impl_->linkMode == 1 ? " · TCP兜底" : "",
                          impl_->backupActive.load() ? " · 双线" : "");
            impl_->toolbar.set_connection_status(line);

            // 自动化验证钩子：MINISTREAM_TEST_QUALITY=1 时，连接 8 秒后
            // 先热调码率、再请求一次分辨率/帧率重协商，用来验证画质面板的两条路径
            // （无需人工点 UI）。默认关闭，正常使用零开销。
            {
                static bool qualityTested = false;
                static const bool qualityTest =
                    ::GetEnvironmentVariableW(L"MINISTREAM_TEST_QUALITY", nullptr, 0) > 0;
                if (qualityTest && !qualityTested && statTicks >= 16) {
                    qualityTested = true;
                    MS_INFO("[测试] 热调码率 -> 8000 kbps");
                    impl_->control.send_set_bitrate(8000);
                    MS_INFO("[测试] 关键帧间隔 -> 1 秒");
                    impl_->control.send_set_keyframe_interval(1);
                    MS_INFO("[测试] 请求重协商 -> 1280x720 @120fps");
                    impl_->control.send_reconfigure(1280, 720, 120);
                }
            }
        }

        // 500ms 没有任何帧，就主动要一次关键帧。
        // 注意：**首帧之前也要要**（原来用 lastFrameRecvUs==0 跳过，结果一旦
        // 第一帧没收到就再也不会主动拉 IDR，画面永远停在"等待首帧"）。
        if (now_us() - lastFrameRecvUs > 500000) {
            lastFrameRecvUs = now_us();
            impl_->control.request_keyframe(1);
        }
    }

    // ---- 8. 收尾：把掉线原因大字显示在屏幕上（用户最烦"闪退没提示"）----
    if (impl_->endReason.empty()) {
        // 主循环正常走到这里（没被踢）：用户主动退出 / ESC / --seconds 到点
        impl_->endReason = "已断开（用户主动退出）";
    }
    {
        std::wstring wreason = fsutil::utf8_to_wide(impl_->endReason);
        impl_->renderer.draw_centered_message(
            L"连接已断开\n\n" + wreason +
            L"\n\n（原因已写进 client.log；按 ESC 关闭本窗口）");
        // 渲染几帧让消息显示出来（用户能看到原因，而不是窗口瞬间消失）
        for (int i = 0; i < 120 && !impl_->stopReq.load(); ++i) {
            impl_->renderer.clear(0.05f, 0.05f, 0.07f);
            ::MsgWaitForMultipleObjects(0, nullptr, FALSE, 16, QS_ALLINPUT);
            MSG msg;
            while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) { impl_->stopReq.store(true); break; }
                ::TranslateMessage(&msg);
                ::DispatchMessageW(&msg);
            }
            if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) break;
        }
    }
    MS_INFO("会话结束，开始清理");
    impl_->audio.shutdown();
    impl_->stopReq.store(true);
    g_hide_local_cursor.store(false, std::memory_order_relaxed);
    impl_->receiver.set_probe_mode(false);
    impl_->toolbar.shutdown();
    impl_->input.set_captured(false);
    // 断开前让被控端抬起所有按键/按钮（趁控制通道还开着，独立于捕获开关发送）
    impl_->remoteButtonsMask = 0;
    impl_->inputPipe.request_release_all();
    sleep_ms(30);   // 给发送线程一点时间把 ReleaseAll 推出去
    impl_->inputPipe.stop();
    impl_->input.shutdown();
    if (impl_->thControl.joinable()) impl_->thControl.join();
    if (impl_->thBackupKeep.joinable()) impl_->thBackupKeep.join();
    impl_->receiver.stop();
    impl_->receiver.shutdown();
    impl_->control.send_disconnect("client quit");
    impl_->control.close();
    impl_->backupCtrl.close();   // 备用线路一并关
    impl_->backupActive.store(false);
    impl_->renderer.shutdown();
    impl_->decoder.shutdown();
    net::shutdown();

    {
        std::lock_guard<std::mutex> lk(impl_->statMtx);
        impl_->status.connected = false;
    }
    return true;
}

} // namespace ms::client
