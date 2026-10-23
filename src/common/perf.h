// =============================================================================
//  perf.h - 线程调度性能辅助（MMCSS / 计时器分辨率）
//
//  依据：UU远程/GameViewer 逆向第 2 轮发现 streamer.dll 显式导入 AVRT.dll 的
//  AvSetMmThreadCharacteristics —— 即「多媒体类调度」。
//  单纯 SetThreadPriority 只是改线程优先级；MMCSS 还会让调度器给这个线程
//  更长的连续时间片、抢占掉普通后台任务，并把系统计时器分辨率拉到 0.5ms。
//  对采集/编码/收包/渲染这种 60Hz 节奏敏感的线程，收益是实打实的：
//  帧间隔抖动（jank）变小，偶发的 10ms+ 卡顿减少。
// =============================================================================
#pragma once

#include "types.h"
#include "log.h"
#include <windows.h>
#include <avrt.h>

#pragma comment(lib, "avrt.lib")

namespace ms {

// 把当前线程注册为 MMCSS 多媒体线程。
//   task 取值（Windows 内置，不用自己注册）：
//     "Pro Audio"      —— 最低延迟，时间片最短，适合音频/收包这种硬实时循环
//     "Games"          —— 次之，适合采集/编码/渲染（每帧要做点活，不能饿死别人）
//     "Playback"       —— 普通多媒体播放
// 返回 MMCSS 句柄，失败返回 nullptr（例如老系统没有 avrt.dll —— 那就当无事发生，
// 调用方不需要做任何分支处理）。线程退出前用 perf_thread_exit 注销。
inline HANDLE perf_thread_enter(const wchar_t* task) {
    DWORD idx = 0;
    HANDLE h = ::AvSetMmThreadCharacteristicsW(task, &idx);
    if (h) {
        MS_DEBUG("MMCSS: 线程已注册为 %ls（task index %lu）", task, idx);
    } else {
        // 不报错：老系统/服务会话可能没有 MMCSS，调度行为退回普通优先级
        MS_DEBUG("MMCSS 注册失败（%lu），使用默认调度", ::GetLastError());
    }
    return h;
}

inline void perf_thread_exit(HANDLE h) {
    if (h) ::AvRevertMmThreadCharacteristics(h);
}

// RAII 版：线程函数开头声明一个，析构自动注销。
struct PerfThreadScope {
    HANDLE h = nullptr;
    explicit PerfThreadScope(const wchar_t* task) : h(perf_thread_enter(task)) {}
    ~PerfThreadScope() { perf_thread_exit(h); }
    PerfThreadScope(const PerfThreadScope&) = delete;
    PerfThreadScope& operator=(const PerfThreadScope&) = delete;
};

} // namespace ms
