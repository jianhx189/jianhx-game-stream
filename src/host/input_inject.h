// =============================================================================
//  input_inject.h - 键鼠注入（SendInput）
//
//  游戏能否识别，取决于用「扫描码 + KEYEVENTF_SCANCODE」而不是直接设 vk，
//  这样 DirectInput / Raw Input 的游戏也能收到。
//  断线时必须 release_all()，否则会出现「按键卡住」——这是自用工具最容易
//  踩的坑（切出去鼠标还在转、W 键一直按着）。
// =============================================================================
#pragma once

#include "../common/types.h"
#include "../common/protocol.h"

namespace ms::host {

class InputInjector {
public:
    InputInjector() = default;

    // 归一化输入坐标用：被控端实际分辨率
    void set_screen_size(int w, int h) { width_ = w; height_ = h; }
    // 被采集显示器在虚拟桌面里的原点（多显示器时绝对坐标要加上它）
    void set_monitor_origin(int x, int y) { originX_ = x; originY_ = y; }

    // 处理一批事件。
    // appliedOut 非空时回填「实际提交到系统的事件数」（SendInput 短交时 < count，
    // 调用方可据此把 InputApplied.status 报成"部分提交"）。
    void apply(const proto::InputEvent* ev, size_t count, u32* appliedOut = nullptr);

    // 抬起所有可能按下的键/鼠标按钮（断线、退出捕获时调用）
    void release_all();

    // 相对鼠标累积发送开关（有些游戏需要绝对定位，暂只做相对）
    bool relative_mode() const { return relative_; }
    void set_relative_mode(bool r) { relative_ = r; }

private:
    void do_mouse_move(i32 dx, i32 dy);
    void do_mouse_button(i32 button, bool down);
    void do_mouse_wheel(i32 vert, i32 horiz);
    void do_key(u16 scancode, bool extended, bool up);

    void remember_key(u16 scancode, bool down);
    void remember_button(int button, bool down);

    int width_ = 1920, height_ = 1080;
    int originX_ = 0, originY_ = 0;   // 被采集显示器在虚拟桌面里的原点
    bool relative_ = true;
    // 记录按下状态用于 release_all
    bool keysDown_[256] = {};
    // 每个 scancode 的扩展（E0）标记：release_all 时必须原样带回去，
    // 否则主 Enter(1C) 会被当成小键盘 Enter(E0 1C) 抬起，游戏里就卡键了
    bool keysExt_[256] = {};
    bool buttonsDown_[8] = {};

    // 远控端对按键/按钮连发 3 份（seq 相同）抗丢包；这里丢弃相邻的同 seq 重复份，
    // 保证游戏只看到一次按下/抬起。被控端网络层若已按 seq 去重，这里就是幂等的。
    u16  lastKeySeq_ = 0;
    bool hasKeySeq_  = false;
    u16  lastBtnSeq_ = 0;
    bool hasBtnSeq_  = false;

    // 绝对移动连续失败计数（SetCursorPos 与 SendInput 都失败时），用于限流日志
    // 与"已恢复"提示。见 input_inject.cpp 里绝对映射那段注释。
    int  absFailStreak_ = 0;
};

} // namespace ms::host
