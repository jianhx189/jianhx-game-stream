// =============================================================================
//  input_capture.h - 远控端键鼠捕获（Raw Input + 低层键盘钩子）
//
//  关键需求：游戏过程中不弹窗、不显示系统光标、不被系统提示打断。
//    * Raw Input 拿相对位移（鼠标不移动本机光标）
//    * 低层键盘钩子吞掉 Win/Alt+Tab 等系统键，避免切出；
//      只放行「释放快捷键」让用户能退出捕获
//    * 捕获期间 ShowCursor(FALSE) + ClipCursor 把光标钉在窗口中心
// =============================================================================
#pragma once

#include "../common/types.h"
#include "../common/protocol.h"
#include <string>

namespace ms::client {

class InputCapture {
public:
    InputCapture() = default;
    ~InputCapture();

    // hwnd 用于接收 WM_INPUT；releaseHotkey 形如 "CTRL+ALT+SHIFT+Q"
    bool init(HWND hwnd, const std::string& releaseHotkey, std::string& err);
    void shutdown();

    // 必须在窗口过程里转发消息。返回 true 表示已被消费。
    bool handle_message(UINT msg, WPARAM wparam, LPARAM lparam);

    // 开启/关闭捕获。开启时会隐藏光标、限制光标、吃掉系统键。
    void set_captured(bool on);
    bool captured() const { return captured_; }

    // 鼠标工作模式（捕获态下有效）：
    //   true（默认）= 相对模式：藏本机光标 + ClipCursor + Raw Input 位移，
    //                 适合游戏内场（游戏自己读原始输入，光标由游戏画）。
    //   false = 绝对模式：本机光标正常显示、不限制，位移改由会话层按
    //                 "本机光标位置 -> 远端像素坐标" 绝对映射发送。
    //                 适合桌面/游戏菜单，光标和本机鼠标 1:1 重合，不吃系统加速。
    // 键盘钩子/按键在两种模式下都照常工作。
    void set_mouse_relative(bool on);
    bool mouse_relative() const { return mouse_relative_; }

    // 取出累积事件（发送线程调用）
    size_t poll_events(proto::InputEvent* out, size_t maxOut);

    // 是否触发过释放快捷键（读取后清除）
    bool take_release_request();

    // 鼠标灵敏度倍率（默认 1.0）
    void set_sensitivity(double s) { sensitivity_ = s; }
    double sensitivity() const { return sensitivity_; }

    // 捕获期间是否吞掉系统键（默认 true）
    void set_block_system_keys(bool on) { blockSystemKeys_ = on; }

    // 顶部工具栏临时唤出光标（不影响捕获状态本身）。
    // 场景：捕获仍开着，用户把光标移到顶边要看工具栏——这时需要
    // 临时恢复系统光标与 ClipCursor 让用户能点按钮，工具栏隐藏后恢复。
    void temporary_show_cursor(bool on);

private:
    void on_raw_input(LPARAM lparam);
    void push_move(i32 dx, i32 dy);
    void push_button(int button, bool down);
    void push_wheel(i32 vert, i32 horiz);
    void push_key(u32 vkey, bool up);
    void push_event(const proto::InputEvent& e);
    // 同一个事件写 repeat 份（seq 相同），空间不足时整体丢弃
    void push_event_repeat(const proto::InputEvent& e, size_t repeat);
    bool hotkey_match(u32 vkey) const;
    void recenter_cursor();
    void reset_modifiers();

    HWND hwnd_ = nullptr;
    bool captured_ = false;
    bool initialized_ = false;
    bool blockSystemKeys_ = true;   // 默认吞掉 Win/Alt+Tab 等系统键
    bool mouse_relative_ = true;    // 捕获态下鼠标走相对模式（游戏内场）
    double sensitivity_ = 1.0;

    // 释放快捷键
    u32 hotkeyMods_ = 0;   // MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_WIN
    u32 hotkeyVk_ = 0;
    std::string hotkeyText_;

    // 事件缓存（环形，容量固定避免临界区里分配）
    static constexpr size_t kBufSize = 4096;
    proto::InputEvent buf_[kBufSize];
    size_t head_ = 0, tail_ = 0;
    u16 seq_ = 0;
    bool releaseRequested_ = false;

    // 鼠标累积（把高频移动合并再发送，减少包数同时不增加延迟）
    i32 accDx_ = 0, accDy_ = 0;
    // 滚轮残量：Raw Input 给的是 120 的倍数，高精度滚轮可能给零头，攒够一格再发
    i32 wheelAccV_ = 0, wheelAccH_ = 0;
    // MOUSE_MOVE_ABSOLUTE 设备（远程桌面/部分触摸板）需要自己做差分
    i32  lastAbsX_ = 0, lastAbsY_ = 0;
    bool hasLastAbs_ = false;
    // 钩子里手动跟踪的修饰键（吞键后 GetAsyncKeyState 不可信，只能自己记）
    u32 modsDown_ = 0;
    // 环形缓冲满导致的丢弃计数（钩子里不写日志，留给 poll_events 惰性输出）
    u32 dropped_ = 0;

    // 低层钩子
    void* kbHook_ = nullptr;
    static InputCapture* s_instance_;
    static long long __stdcall kb_hook_proc(int code, unsigned long long wparam, long long lparam);
};

} // namespace ms::client
