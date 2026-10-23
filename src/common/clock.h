// =============================================================================
//  clock.h - 高精度时钟与睡眠
//
//  延迟统计全部走这里。用 QueryPerformanceCounter，微秒精度。
// =============================================================================
#pragma once

#include "types.h"
#include <windows.h>

namespace ms {

// 把 QPC 计数换算成微秒。
// 【血泪教训】不能直接写 (ticks * 1000000) / freq：ticks 是开机以来的计数，
// freq=10MHz 时开机超过约 10.7 天，ticks*1e6 就超过 int64 上限**溢出成负数**，
// now_us() 于是返回负值 —— 所有"距上次 xx 是否超过 N 毫秒"的判断都永远不成立：
// 心跳 Ping 发不出去、统计日志不打印、判死计时失效。真机症状就是"连上之后
// 卡在等首帧，几秒后被控端超时断开"，而且只有开机超过 10.7 天的机器才会犯。
// 先除后乘、余数单独换算，全程不溢出。
inline i64 us_from_ticks(i64 ticks, i64 freq) {
    if (freq <= 0) freq = 1;
    const i64 secs = ticks / freq;
    const i64 rem  = ticks % freq;                    // 0 <= rem < freq
    // rem*1e6 <= freq*1e6，对任何实际 QPF（<=1e9）都不会溢出 int64
    return secs * 1000000LL + (rem * 1000000LL) / freq;
}

// 单调微秒时钟
inline i64 now_us() {
    static const i64 freq = [] {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return f.QuadPart ? f.QuadPart : 1;
    }();
    LARGE_INTEGER c{};
    QueryPerformanceCounter(&c);
    return us_from_ticks(c.QuadPart, freq);
}

inline i64 now_ms() { return now_us() / 1000; }

inline void sleep_ms(int ms) {
    if (ms <= 0) return;
    ::Sleep(ms);
}

// 让出 CPU，用于短自旋
inline void yield_cpu() { ::SwitchToThread(); }

// 计算两次采样之间的微秒差
inline i64 since_us(i64 t0) { return now_us() - t0; }

// -----------------------------------------------------------------------------
// 时钟偏移估计器（迷你 NTP）
//
//  用途：远控端要算出「端到端延迟」，就必须把被控端的时间戳换算到本地时间轴。
//  做法：控制通道(TCP)上周期性 Ping/Pong，取最小 RTT 的那次作为最优样本。
//
//     T1 --Ping-->  T2
//     T4 <--Pong--  T3
//
//     rtt    = (T4 - T1) - (T3 - T2)
//     offset = ((T2 - T1) + (T3 - T4)) / 2      // 远端时钟 - 本地时钟
// -----------------------------------------------------------------------------
class ClockSync {
public:
    // 一次完整的往返样本；t1/t4 本地时间轴，t2/t3 远端时间轴
    void update(i64 t1, i64 t2, i64 t3, i64 t4) {
        const i64 rtt = (t4 - t1) - (t3 - t2);
        const i64 off = ((t2 - t1) + (t3 - t4)) / 2;
        last_rtt_us_ = rtt;
        // 网络抖动会污染 offset，只信 RTT 最小的那批样本
        if (!valid_ || rtt * kFilterNum < best_rtt_us_ * kFilterDen) {
            best_rtt_us_ = rtt;
            offset_us_   = off;
            valid_       = true;
        }
        // 慢速跟随，防止时钟漂移导致长期偏差
        offset_us_ += (off - offset_us_) / 64;
    }

    // 把远端时间戳换算成本地时间轴。
    // offset_us_ 的定义是「远端 - 本地」（见上面的 update），所以：
    //   本地 = 远端 - offset，远端 = 本地 + offset
    // 【以前的符号是反的】内网/环回时 offset≈0 看不出来，一跨公网延迟就显示 0.0ms。
    i64 remote_to_local(i64 remote_us) const { return remote_us - offset_us_; }
    // 反过来
    i64 local_to_remote(i64 local_us) const { return local_us + offset_us_; }

    i64 offset_us()   const { return offset_us_; }
    i64 last_rtt_us() const { return last_rtt_us_; }
    i64 best_rtt_us() const { return valid_ ? best_rtt_us_ : 0; }
    bool valid()      const { return valid_; }

private:
    // 新样本的 RTT 必须小于「最优 RTT 的 2 倍」才允许替换基准，避免异常样本
    static constexpr i64 kFilterNum = 2;
    static constexpr i64 kFilterDen = 1;

    i64  offset_us_   = 0;
    i64  last_rtt_us_ = 0;
    i64  best_rtt_us_ = 0;
    bool valid_       = false;
};

} // namespace ms
