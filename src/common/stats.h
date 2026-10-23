// =============================================================================
//  stats.h - 码率/帧率/延迟统计
//
//  渲染悬浮条要靠这些数字；同时被控端也用码率反馈做自适应（后续）。
// =============================================================================
#pragma once

#include "types.h"
#include "clock.h"
#include <deque>
#include <mutex>
#include <vector>
#include <algorithm>

namespace ms {

// 滑动窗口吞吐统计
class RateCounter {
public:
    void add(size_t bytes, i64 tsUs = 0) {
        const i64 t = tsUs ? tsUs : now_us();
        std::lock_guard<std::mutex> lk(m_);
        q_.push_back({t, bytes});
        trim(t);
    }

    // 单位：次/秒
    double rate(double windowSec = 1.0) const {
        const i64 t = now_us();
        std::lock_guard<std::mutex> lk(m_);
        trim(t);
        if (q_.size() < 2) return 0.0;
        const i64 span = q_.back().t - q_.front().t;
        if (span <= 0) return 0.0;
        const double win = double(windowSec) * 1e6;
        const i64 eff = span < i64(win) ? span : i64(win);
        if (eff <= 0) return 0.0;
        size_t cnt = 0;
        for (auto it = q_.rbegin(); it != q_.rend(); ++it) {
            if (q_.back().t - it->t <= eff) ++cnt; else break;
        }
        return double(cnt) * 1e6 / double(eff);
    }

    // 单位：kbps
    double kbps(double windowSec = 1.0) const {
        const i64 t = now_us();
        std::lock_guard<std::mutex> lk(m_);
        trim(t);
        if (q_.empty()) return 0.0;
        const double win = double(windowSec) * 1e6;
        size_t bytes = 0;
        for (auto it = q_.rbegin(); it != q_.rend(); ++it) {
            if (q_.back().t - it->t <= i64(win)) bytes += it->b; else break;
        }
        const i64 span = (q_.size() < 2) ? i64(win)
                                         : std::min<i64>(q_.back().t - q_.front().t, i64(win));
        if (span <= 0) return 0.0;
        return double(bytes) * 8.0 * 1e6 / double(span) / 1000.0;
    }

    void reset() { std::lock_guard<std::mutex> lk(m_); q_.clear(); }

private:
    struct Sample { i64 t; size_t b; };
    void trim(i64 now) const {
        const i64 keep = 8 * 1000000LL;   // 保留 8 秒窗口
        while (!q_.empty() && now - q_.front().t > keep) q_.pop_front();
        while (q_.size() > 200000) q_.pop_front();
    }
    mutable std::deque<Sample> q_;
    mutable std::mutex m_;
};

// 延迟统计（微秒）
class LatencyStats {
public:
    void add(i64 us) {
        if (us < 0) return;
        std::lock_guard<std::mutex> lk(m_);
        if (n_ == 0) { min_ = max_ = us; }
        min_ = std::min(min_, us);
        max_ = std::max(max_, us);
        sum_ += us;
        ++n_;
        samples_.push_back(us);
        if (samples_.size() > 900) samples_.erase(samples_.begin(),
                                                  samples_.begin() + 300);
    }
    i64 avg() const { std::lock_guard<std::mutex> lk(m_); return n_ ? sum_ / n_ : 0; }
    i64 max() const { std::lock_guard<std::mutex> lk(m_); return max_; }
    i64 min() const { std::lock_guard<std::mutex> lk(m_); return min_; }
    i64 p95() const {
        std::lock_guard<std::mutex> lk(m_);
        if (samples_.empty()) return 0;
        std::vector<i64> v = samples_;
        const size_t idx = size_t(double(v.size() - 1) * 0.95);
        std::nth_element(v.begin(), v.begin() + idx, v.end());
        return v[idx];
    }
    void reset() {
        std::lock_guard<std::mutex> lk(m_);
        n_ = 0; sum_ = 0; min_ = INT64_MAX; max_ = 0; samples_.clear();
    }

private:
    mutable std::mutex m_;
    i64 n_ = 0, sum_ = 0, min_ = INT64_MAX, max_ = 0;
    std::vector<i64> samples_;
};

} // namespace ms
