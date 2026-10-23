// =============================================================================
//  types.h - 全局基础类型与常量
// =============================================================================
#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <memory>
#include <atomic>

namespace ms {

using u8  = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using i8  = int8_t;
using i16 = int16_t;
using i32 = int32_t;
using i64 = int64_t;

using Bytes = std::vector<u8>;

// 视频/音频/显示相关的公共默认值
namespace defaults {
constexpr u16 kPort = 20000;        // 默认端口（要求 > 10000）
constexpr u16 kMinPort = 10001;     // 端口下限（含）
constexpr int kFps = 60;
constexpr int kBitrateAuto = 0;     // 0 = 按分辨率自适应
} // namespace defaults

// 端口合法性：> 10000 且 <= 65535
inline bool is_valid_port(int p) {
    return p >= defaults::kMinPort && p <= 65535;
}

// 按分辨率估算自适应码率（kbps）。
//
// 【为什么基准从 20 Mbps 降到 8 Mbps】（2026-10 实测 + 对照 RustDesk 源码）
// 老版本 1080p60 默认给 20 Mbps（4K 甚至 80 Mbps）。局域网直连没问题，但只要
// 中间有 FRP/中继，这个量级会瞬间把管道灌满：TCP 没有丢包信号，数据只会堆在
// 发送/中继缓冲里，端到端延迟一路涨 —— 用户看到的就是"越用越滞后、鼠标点下去
// 半天才动"。同一个 FRP 服务器上 RustDesk 流畅，是因为它 1080p 的基准码率是
// 2073 kbps（Balanced 系数 0.67，硬件 H.264 ×2 ≈ 2.8 Mbps，
// 见 libs/scrap/src/common/codec.rs:929-959 与 hwcodec.rs:258-283）。
//
// 我们要的是游戏画面，所以取 RustDesk 的约 3 倍（帧率 60 vs 30、画质更好），
// 即 1080p60 ≈ 8 Mbps，再按像素数和帧率线性缩放。用户可以在画质面板里手动调高，
// 这条只决定「没指定时的起点」。
inline int auto_bitrate_kbps(int w, int h, int fps) {
    if (w <= 0 || h <= 0) return 8000;
    const double pixels = double(w) * double(h);
    // 1080p60 -> 8 Mbps 为基准
    double base = 8000.0 * (pixels / (1920.0 * 1080.0)) * (double(fps) / 60.0);
    if (base < 2500.0)  base = 2500.0;
    if (base > 30000.0) base = 30000.0;
    return int(base / 1000.0) * 1000;
}

// 作用域退出时执行（用于 socket / handle 清理）
template <typename F>
struct ScopeGuard {
    F f;
    explicit ScopeGuard(F fn) : f(std::move(fn)) {}
    ~ScopeGuard() { f(); }
    ScopeGuard(const ScopeGuard&) = delete;
    ScopeGuard& operator=(const ScopeGuard&) = delete;
};

template <typename F>
ScopeGuard<F> make_scope_guard(F fn) { return ScopeGuard<F>(std::move(fn)); }

} // namespace ms
