// =============================================================================
//  quality.h - 画质档位（RustDesk 式「画质/流畅」预设）
//
//  一组预设把「分辨率 / 帧率 / 码率 / 关键帧间隔」打包成三个游戏场景档位，
//  远控端一键切换；同时每一项都可以自定义。预设只影响协商参数与运行期
//  可热调的部分（码率、关键帧间隔、帧率上限），分辨率切换需要重连。
// =============================================================================
#pragma once

#include "types.h"
#include <string>
#include <vector>

namespace ms::quality {

// 预设档位（在 UI 里显示的顺序）
enum class Preset : int {
    Balanced  = 0,   // 均衡（默认）：大多数游戏
    Sharp     = 1,   // 画质优先：画面细节多、节奏慢一点的
    Smooth    = 2,   // 流畅优先：竞技 FPS，延迟和帧率压倒一切
    Custom    = 3,   // 自定义
};

constexpr int kPresetCount = 4;

inline const char* preset_name(Preset p) {
    switch (p) {
        case Preset::Balanced: return "均衡";
        case Preset::Sharp:    return "画质优先";
        case Preset::Smooth:   return "流畅优先";
        case Preset::Custom:   return "自定义";
    }
    return "?";
}

// 一个档位的全部参数
struct QualityProfile {
    Preset preset = Preset::Balanced;
    int width  = 0;              // 0 = 跟随被控端原生
    int height = 0;
    int fps    = 60;
    int bitrateKbps = 0;         // 0 = 按分辨率自适应
    int keyframeIntervalSec = 0; // 0=按需 IDR；>0=周期兜底
    int codec  = 1;              // 1=H.264，2=HEVC(H.265)；0=不改变
};

// 编码格式下拉（显示文本 + 值）
struct CodecOption { const wchar_t* label; int value; };
inline const std::vector<CodecOption>& codec_options() {
    static const std::vector<CodecOption> opts = {
        { L"H.264（兼容优先）",  1 },
        { L"H.265（同画质省 35% 码率）", 2 },
    };
    return opts;
}
inline const wchar_t* codec_label(int v) {
    return v == 2 ? L"H.265" : L"H.264";
}

// 三档预设的缺省值（Custom 由 UI 里的当前编辑值充当）
inline QualityProfile default_profile(Preset p) {
    QualityProfile q;
    q.preset = p;
    switch (p) {
        case Preset::Balanced:               // 1080p60 自适应码率
            q.width = 0;  q.height = 0;  q.fps = 60;
            q.bitrateKbps = 0;  q.keyframeIntervalSec = 0;
            break;
        case Preset::Sharp:                  // 原生分辨率 + 高码率
            q.width = 0;  q.height = 0;  q.fps = 60;
            q.bitrateKbps = 50000; q.keyframeIntervalSec = 0;
            break;
        case Preset::Smooth:                 // 帧率优先 + 短 GOP 快恢复
            q.width = 0;  q.height = 0;  q.fps = 120;
            q.bitrateKbps = 30000; q.keyframeIntervalSec = 0;
            break;
        case Preset::Custom:
        default:
            break;
    }
    return q;
}

// 连接面板里的常用分辨率选项（显示文本 -> w x h；0x0 = 原生）
struct ResOption { const wchar_t* label; int w; int h; };
inline const std::vector<ResOption>& resolution_options() {
    static const std::vector<ResOption> opts = {
        { L"跟随被控端",       0,    0 },
        { L"1920 x 1080 (1080p)", 1920, 1080 },
        { L"2560 x 1440 (1440p)", 2560, 1440 },
        { L"3840 x 2160 (4K)",    3840, 2160 },
        { L"1600 x 900",       1600,  900 },
        { L"1280 x 720 (720p)",   1280,  720 },
    };
    return opts;
}

inline const std::vector<int>& fps_options() {
    static const std::vector<int> fps = {30, 60, 120, 144, 240};
    return fps;
}

// 常用码率选项（kbps，0 = 自适应）
inline const std::vector<int>& bitrate_options() {
    static const std::vector<int> br = {0, 8000, 15000, 20000, 30000, 50000, 80000};
    return br;
}

// 自定义值合法性
inline bool valid_bitrate(int kbps) { return kbps >= 0 && kbps <= 200000; }
inline bool valid_dimensions(int w, int h) {
    return w >= 0 && h >= 0 && w <= 7680 && h <= 4320 && ((w == 0) == (h == 0));
}

} // namespace ms::quality
