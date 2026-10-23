// =============================================================================
//  input_capture.cpp - 远控端键鼠捕获（Raw Input 鼠标 + 低层键盘钩子）
//
//  这个文件的取舍（都写在这里，避免后来的人踩坑）：
//
//  1) 键盘事件只在一个地方产生：低层钩子。
//     WH_KEYBOARD_LL 是全局的，即使键被我们返回 1 吞掉，钩子本身照样能看到它。
//     而 Raw Input 的键盘分支只有在键「没被吞掉」时才会收到。两处都发的话，
//     一个键会变成两条事件（吞掉系统键时反而只有钩子收到，状态就更乱）。
//     所以：钩子负责 push_key（并按需吞键），Raw Input 的键盘分支只看一眼
//     VK_NONE 就忽略。
//
//  2) 按键/按钮不再连发多份（v2 第一阶段改动）：
//     输入主路径已改走 TCP 控制通道（InputBatch，可靠有序），多份副本反而
//     是"丢一个包同时丢三份点击"的帮凶。传输可靠性由 InputPipeline 保证；
//     旧版服务端（UDP 老路径）按相邻 seq 去重，单份照常工作。
//
//  3) 位移每次回调立即产出一条事件，不做时间攒批。
//     FPS 瞄准吃的就是相位，任何攒批窗口都等于人为加延迟（几毫秒就能感觉出来）；
//     带宽不用靠攒批省——发送线程一次 poll_events 本来就能把多条事件打进同一个
//     UDP 包。accDx_/accDy_ 只当「亚像素残量」：灵敏度 <1 时单次位移可能不足
//     1 像素，留到下一次一起发，既不延迟也不丢精度。
//
//  4) 环形缓冲满时丢弃「当前」事件，而不是覆盖最旧。
//     覆盖最旧会把已经入队的按下/抬起顺序打乱（比如丢掉 keydown 只剩 keyup），
//     被控端状态机就错了；丢当前最多丢一次动作。丢弃计数在 poll_events 里
//     惰性打日志（钩子里绝对不能做 IO）。
//
//  注意：本文件必须先包含 <windows.h>，因为 input_capture.h 用到了 HWND/UINT/
//        WPARAM/LPARAM（保持头文件原样，不额外改它的包含关系）。
// =============================================================================

#include <windows.h>

#include "input_capture.h"
#include "../common/log.h"

namespace ms::client {

namespace {

// 我们自己注入的事件标记（dwExtraInfo），钩子看到就放行，避免自我回环
constexpr ULONG_PTR kInjectedMagic = static_cast<ULONG_PTR>(0x4D535452u);  // 'MSTR'

// 按键/按钮连发份数（v2 起为 1：可靠传输由 TCP 主路径保证）
constexpr size_t kRepeatCount = 1;

// ShowCursor 的循环上限：正常一两次就到位，加个护栏防止极端情况下死循环
constexpr int kCursorLoopMax = 64;

// 修饰键 VK -> MOD_* 位（注意是 VK_MENU 不是 VK_ALT）
u32 vk_to_mod_bit(u32 vk) {
    switch (vk) {
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL: return MOD_CONTROL;
    case VK_MENU:    case VK_LMENU:    case VK_RMENU:    return MOD_ALT;
    case VK_SHIFT:   case VK_LSHIFT:   case VK_RSHIFT:   return MOD_SHIFT;
    case VK_LWIN:    case VK_RWIN:                       return MOD_WIN;
    default: return 0;
    }
}

// 钩子吞键后系统不会再更新按键状态，修饰键只能自己跟踪。
// 局限：同时按住左右两个 Ctrl、只松开其中一个时会提前清位（自用工具可接受）。
u32 update_mods_mask(u32 mods, u32 vk, bool up) {
    const u32 bit = vk_to_mod_bit(vk);
    if (bit == 0) return mods;
    if (up) return mods & ~bit;
    return mods | bit;
}

// 释放快捷键文本解析："CTRL+ALT+SHIFT+Q" / "ctrl-alt-q"，大小写不敏感，
// 分隔符支持 '+' 和 '-'，空格忽略。
bool parse_hotkey(const std::string& text, u32& mods, u32& vk, std::string& err) {
    mods = 0;
    vk = 0;
    err.clear();

    std::string s;
    s.reserve(text.size());
    for (char c : text) {
        if (c == ' ' || c == '\t') continue;                 // 忽略空格
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        s.push_back(c);
    }
    if (s.empty()) {
        err = "释放快捷键为空（示例：CTRL+ALT+Q）";
        return false;
    }

    bool haveKey = false;
    size_t pos = 0;
    for (;;) {
        const size_t sep = s.find_first_of("+-", pos);
        const std::string tok = s.substr(pos, (sep == std::string::npos ? s.size() : sep) - pos);
        if (tok.empty()) {
            err = "释放快捷键格式错误（有空的分段）: " + text;
            return false;
        }

        u32 modBit = 0;
        if (tok == "CTRL" || tok == "CONTROL")      modBit = MOD_CONTROL;
        else if (tok == "ALT")                      modBit = MOD_ALT;
        else if (tok == "SHIFT")                    modBit = MOD_SHIFT;
        else if (tok == "WIN" || tok == "WINDOWS")  modBit = MOD_WIN;

        if (modBit != 0) {
            mods |= modBit;
        } else {
            u32 key = 0;
            if (tok.size() == 1) {
                const char c = tok[0];
                if (c >= 'A' && c <= 'Z')      key = static_cast<u32>(c);         // A-Z -> 'A'..'Z'
                else if (c >= '0' && c <= '9') key = static_cast<u32>(c);         // 0-9 -> '0'..'9'
            } else if (tok.size() >= 2 && tok.size() <= 3 && tok[0] == 'F') {
                int n = 0;
                bool digits = true;
                for (size_t i = 1; i < tok.size(); ++i) {
                    if (tok[i] < '0' || tok[i] > '9') { digits = false; break; }
                    n = n * 10 + (tok[i] - '0');
                }
                if (digits && n >= 1 && n <= 12) key = static_cast<u32>(VK_F1 + n - 1);
            } else if (tok == "SPACE") {
                key = VK_SPACE;
            } else if (tok == "ESC" || tok == "ESCAPE") {
                key = VK_ESCAPE;
            } else if (tok == "TAB") {
                key = VK_TAB;
            }

            if (key == 0) {
                err = "释放快捷键里有无法识别的按键: " + tok + "（原文 " + text + "）";
                return false;
            }
            if (haveKey) {
                err = "释放快捷键只能有一个主键: " + text;
                return false;
            }
            vk = key;
            haveKey = true;
        }

        if (sep == std::string::npos) break;
        pos = sep + 1;
    }

    if (!haveKey) {
        err = "释放快捷键缺少主键（只有修饰键）: " + text;
        return false;
    }
    return true;
}

// 注销 Raw Input（RIDEV_REMOVE 要求 hwndTarget 为 NULL）
void unregister_raw_input() {
    RAWINPUTDEVICE rid[2] = {};
    rid[0].usUsagePage = 0x01;
    rid[0].usUsage = 0x02;
    rid[0].dwFlags = RIDEV_REMOVE;
    rid[0].hwndTarget = nullptr;
    rid[1].usUsagePage = 0x01;
    rid[1].usUsage = 0x06;
    rid[1].dwFlags = RIDEV_REMOVE;
    rid[1].hwndTarget = nullptr;
    ::RegisterRawInputDevices(rid, 2, sizeof(RAWINPUTDEVICE));
}

} // namespace

InputCapture* InputCapture::s_instance_ = nullptr;

InputCapture::~InputCapture() {
    shutdown();
}

// -----------------------------------------------------------------------------
// 初始化：注册 Raw Input + 装低层键盘钩子
// -----------------------------------------------------------------------------
bool InputCapture::init(HWND hwnd, const std::string& releaseHotkey, std::string& err) {
    err.clear();
    if (hwnd == nullptr) {
        err = "输入捕获: 窗口句柄为空";
        return false;
    }

    u32 mods = 0;
    u32 vk = 0;
    if (!parse_hotkey(releaseHotkey, mods, vk, err)) {
        return false;
    }

    hwnd_ = hwnd;
    hotkeyMods_ = mods;
    hotkeyVk_ = vk;
    hotkeyText_ = releaseHotkey;
    // blockSystemKeys_ 默认 true（见头文件成员初值）；调用方若在 init 前用
    // set_block_system_keys() 改过，这里刻意不覆盖。

    // Raw Input：鼠标 + 键盘都用 RIDEV_INPUTSINK，窗口没焦点也收得到。
    // 键盘刻意不加 RIDEV_NOLEGACY：那会把整个系统键盘屏蔽掉，连释放快捷键都收不到，
    // 系统键交给下面的低层钩子逐个处理。
    RAWINPUTDEVICE rid[2] = {};
    rid[0].usUsagePage = 0x01;
    rid[0].usUsage = 0x02;   // 鼠标
    rid[0].dwFlags = RIDEV_INPUTSINK;
    rid[0].hwndTarget = hwnd;
    rid[1].usUsagePage = 0x01;
    rid[1].usUsage = 0x06;   // 键盘
    rid[1].dwFlags = RIDEV_INPUTSINK;
    rid[1].hwndTarget = hwnd;

    if (!::RegisterRawInputDevices(rid, 2, sizeof(RAWINPUTDEVICE))) {
        err = "注册 Raw Input 设备失败，错误码 " + std::to_string(::GetLastError());
        hwnd_ = nullptr;
        return false;
    }

    // 低层键盘钩子：dwThreadId = 0 是全局钩子；WH_KEYBOARD_LL 不需要 DLL，
    // 回调在安装钩子的这个线程（消息循环线程）里被调用，所以下面的成员访问
    // 天然是单线程的，不需要锁。
    s_instance_ = this;
    kbHook_ = static_cast<void*>(
        ::SetWindowsHookExW(WH_KEYBOARD_LL, kb_hook_proc, ::GetModuleHandleW(nullptr), 0));
    if (kbHook_ == nullptr) {
        err = "安装低层键盘钩子失败，错误码 " + std::to_string(::GetLastError());
        s_instance_ = nullptr;
        unregister_raw_input();
        hwnd_ = nullptr;
        return false;
    }

    captured_ = false;
    initialized_ = true;
    return true;
}

// -----------------------------------------------------------------------------
// 关闭：卸钩子 + 恢复光标
// -----------------------------------------------------------------------------
void InputCapture::shutdown() {
    if (captured_) {
        captured_ = false;
        ::ClipCursor(nullptr);
        int guard = 0;
        while (::ShowCursor(TRUE) < 0 && ++guard < kCursorLoopMax) {}
    }

    if (kbHook_ != nullptr) {
        ::UnhookWindowsHookEx(static_cast<HHOOK>(kbHook_));
        kbHook_ = nullptr;
    }
    if (s_instance_ == this) {
        s_instance_ = nullptr;
    }
    if (initialized_) {
        unregister_raw_input();
    }

    initialized_ = false;
    captured_ = false;
    hwnd_ = nullptr;
    releaseRequested_ = false;
    modsDown_ = 0;
    hasLastAbs_ = false;
    accDx_ = 0;
    accDy_ = 0;
    wheelAccV_ = 0;
    wheelAccH_ = 0;
}

// -----------------------------------------------------------------------------
// 消息转发：WM_INPUT / WM_SETFOCUS
// -----------------------------------------------------------------------------
bool InputCapture::handle_message(UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_INPUT:
        if (!initialized_) return false;
        on_raw_input(lparam);
        // WM_INPUT 处理完必须交给 DefWindowProc，否则系统不会认为这次输入已被消费
        ::DefWindowProcW(hwnd_, msg, wparam, lparam);
        return true;

    case WM_SETFOCUS:
        // 重新拿到焦点时把物理光标钉回窗口中心（相对模式的前提）
        if (captured_) recenter_cursor();
        return false;   // 其余默认处理照旧

    default:
        return false;
    }
}

// -----------------------------------------------------------------------------
// Raw Input 解析（鼠标；键盘见文件头的「去重取舍」）
// -----------------------------------------------------------------------------
void InputCapture::on_raw_input(LPARAM lparam) {
    RAWINPUT raw{};
    UINT size = sizeof(raw);
    const UINT got = ::GetRawInputData(reinterpret_cast<HRAWINPUT>(lparam), RID_INPUT,
                                       &raw, &size, sizeof(RAWINPUTHEADER));
    if (got == static_cast<UINT>(-1) || got < sizeof(RAWINPUTHEADER)) return;

    if (raw.header.dwType != RIM_TYPEMOUSE) {
        // RIM_TYPEKEYBOARD：刻意忽略，键盘事件统一由低层钩子产生（见文件头说明）
        return;
    }
    if (!captured_) return;   // 没在捕获态就不碰鼠标

    const RAWMOUSE& m = raw.data.mouse;

    // --- 位移 ---
    i32 dx = 0;
    i32 dy = 0;
    if ((m.usFlags & MOUSE_MOVE_ABSOLUTE) != 0) {
        // 远程桌面 / 某些触摸设备给的是绝对坐标：自己做差分
        if (hasLastAbs_) {
            dx = m.lLastX - lastAbsX_;
            dy = m.lLastY - lastAbsY_;
        }
        lastAbsX_ = m.lLastX;
        lastAbsY_ = m.lLastY;
        hasLastAbs_ = true;
    } else {
        // 通常走这里：lLastX/lLastY 本身就是相对位移
        dx = m.lLastX;
        dy = m.lLastY;
    }
    if (dx != 0 || dy != 0) {
        // 绝对模式下不发明相对位移：会话层会按本机光标位置发绝对坐标，
        // 两种位移同时发会让被控端光标"抖 + 飘"。
        if (mouse_relative_) push_move(dx, dy);
    }

    // --- 按键 ---
    const USHORT bf = m.usButtonFlags;
    if (bf & RI_MOUSE_LEFT_BUTTON_DOWN)   push_button(1, true);
    if (bf & RI_MOUSE_LEFT_BUTTON_UP)     push_button(1, false);
    if (bf & RI_MOUSE_RIGHT_BUTTON_DOWN)  push_button(2, true);
    if (bf & RI_MOUSE_RIGHT_BUTTON_UP)    push_button(2, false);
    if (bf & RI_MOUSE_MIDDLE_BUTTON_DOWN) push_button(3, true);
    if (bf & RI_MOUSE_MIDDLE_BUTTON_UP)   push_button(3, false);
    if (bf & RI_MOUSE_BUTTON_4_DOWN)      push_button(4, true);
    if (bf & RI_MOUSE_BUTTON_4_UP)        push_button(4, false);
    if (bf & RI_MOUSE_BUTTON_5_DOWN)      push_button(5, true);
    if (bf & RI_MOUSE_BUTTON_5_UP)        push_button(5, false);

    // --- 滚轮（usButtonData 是有符号的原始增量，正常就是 ±120 的倍数）---
    if (bf & RI_MOUSE_WHEEL)  push_wheel(static_cast<SHORT>(m.usButtonData), 0);
    if (bf & RI_MOUSE_HWHEEL) push_wheel(0, static_cast<SHORT>(m.usButtonData));
}

// -----------------------------------------------------------------------------
// 事件生产
// -----------------------------------------------------------------------------
void InputCapture::push_move(i32 dx, i32 dy) {
    // 每次回调立即产出（理由见文件头第 3 条）；accDx_/accDy_ 是亚像素残量
    accDx_ += dx;
    accDy_ += dy;

    // 灵敏度被误设成 0 或负数时按 1.0 处理，否则鼠标会「完全不动」且累加器无限增长
    const double sens = (sensitivity_ > 0.0) ? sensitivity_ : 1.0;

    // 截断而不是四舍五入：灵敏度 <1 时残量留在累加器里，下一次补齐，长期不偏
    const i32 sx = static_cast<i32>(static_cast<double>(accDx_) * sens);
    const i32 sy = static_cast<i32>(static_cast<double>(accDy_) * sens);
    if (sx == 0 && sy == 0) return;   // 不足 1 像素，攒着，这次不发

    if (sx != 0) accDx_ = 0;          // 只清真正发出去的那个轴，另一个轴继续攒
    if (sy != 0) accDy_ = 0;

    proto::InputEvent e;
    e.type = static_cast<u8>(proto::InputType::MouseMove);
    e.flags = 0;
    e.seq = seq_++;
    e.a = sx;
    e.b = sy;
    push_event(e);
}

void InputCapture::push_button(int button, bool down) {
    proto::InputEvent e;
    e.type = static_cast<u8>(proto::InputType::MouseButton);
    e.flags = 0;
    e.seq = seq_++;
    e.a = button;
    e.b = down ? 1 : 0;
    // 按下/抬起各发一份，可靠与顺序由 TCP 主路径（InputBatch）保证
    push_event(e);
}

void InputCapture::push_wheel(i32 vert, i32 horiz) {
    wheelAccV_ += vert;
    wheelAccH_ += horiz;

    // 协议里 MouseWheel.a/b 是「刻度数」（1 = WHEEL_DELTA）；不够一格就攒着，
    // 高精度滚轮（自由滚轮/触控板）那些零头也不会被丢掉。
    const i32 nv = wheelAccV_ / WHEEL_DELTA;
    const i32 nh = wheelAccH_ / WHEEL_DELTA;
    if (nv == 0 && nh == 0) return;
    wheelAccV_ -= nv * WHEEL_DELTA;
    wheelAccH_ -= nh * WHEEL_DELTA;

    proto::InputEvent e;
    e.type = static_cast<u8>(proto::InputType::MouseWheel);
    e.flags = 0;
    e.seq = seq_++;
    e.a = nv;
    e.b = nh;
    push_event(e);   // 滚轮不连发
}

void InputCapture::push_key(u32 vkey, bool up) {
    // vk -> scancode：被控端直接拿扫描码注入，DirectInput / Raw Input 游戏才认
    bool ext = false;
    const u16 sc = proto::make_scancode(vkey, ext);
    if (sc == 0) return;   // 没有扫描码的键（VK_NONE 之类）直接丢

    proto::InputEvent e;
    e.type = static_cast<u8>(proto::InputType::Key);
    const u8 flags = static_cast<u8>((up ? proto::kInputFlagKeyUp : 0) |
                                    (ext ? proto::kInputFlagKeyExt : 0));
    e.flags = flags;
    e.seq = seq_++;
    e.a = static_cast<i32>(sc);
    e.b = 0;
    push_event(e);
}

void InputCapture::push_event_repeat(const proto::InputEvent& e, size_t repeat) {
    if (repeat == 0) return;

    const size_t used = (head_ + kBufSize - tail_) % kBufSize;
    const size_t freeSlots = kBufSize - 1 - used;   // 留一格区分空/满
    if (freeSlots < repeat) {
        // 丢当前、不覆盖最旧：覆盖最旧会打乱已入队的按下/抬起顺序（可能只剩 keyup），
        // 被控端就会卡键。丢当前只是丢一次动作，状态机不会错乱。
        dropped_ += static_cast<u32>(repeat);
        return;
    }
    for (size_t i = 0; i < repeat; ++i) {
        buf_[head_] = e;
        head_ = (head_ + 1) % kBufSize;
    }
}

void InputCapture::push_event(const proto::InputEvent& e) {
    push_event_repeat(e, 1);
}

size_t InputCapture::poll_events(proto::InputEvent* out, size_t maxOut) {
    if (out == nullptr || maxOut == 0) return 0;

    size_t n = 0;
    while (n < maxOut && tail_ != head_) {
        out[n++] = buf_[tail_];
        tail_ = (tail_ + 1) % kBufSize;
    }

    if (dropped_ != 0) {
        // 钩子路径不能写日志，丢弃统计攒到这里惰性输出一次
        MS_WARN("input_capture: 事件缓冲满，丢弃 %u 条（发送线程跟不上）", dropped_);
        dropped_ = 0;
    }
    return n;
}

bool InputCapture::take_release_request() {
    const bool r = releaseRequested_;
    releaseRequested_ = false;
    return r;
}

// -----------------------------------------------------------------------------
// 捕获开关
// -----------------------------------------------------------------------------
void InputCapture::set_captured(bool on) {
    if (on == captured_) return;

    if (on) {
        captured_ = true;

        // 只有相对模式才藏光标/限制光标/钉中心；绝对模式下本机光标要正常可见
        if (mouse_relative_) {
            // 隐藏本机光标：内部显示计数可能 >0，一直减到负数才真的看不见
            int guard = 0;
            while (::ShowCursor(FALSE) >= 0 && ++guard < kCursorLoopMax) {}

            // 把光标限制在窗口客户区（GetClientRect 是客户区坐标，要转屏幕坐标）
            RECT rc{};
            if (::GetClientRect(hwnd_, &rc)) {
                POINT tl{rc.left, rc.top};
                POINT br{rc.right, rc.bottom};
                if (::ClientToScreen(hwnd_, &tl) && ::ClientToScreen(hwnd_, &br)) {
                    RECT clip{tl.x, tl.y, br.x, br.y};
                    ::ClipCursor(&clip);
                }
            }
            recenter_cursor();
        }

        // 捕获开始前就已经按下的修饰键不会再产生钩子事件：按系统状态播种一次，
        // 否则「先按住 Ctrl 再进捕获」时快捷键组合会被误判。
        modsDown_ = 0;
        if (::GetAsyncKeyState(VK_CONTROL) & 0x8000) modsDown_ |= MOD_CONTROL;
        if (::GetAsyncKeyState(VK_MENU)    & 0x8000) modsDown_ |= MOD_ALT;
        if (::GetAsyncKeyState(VK_SHIFT)   & 0x8000) modsDown_ |= MOD_SHIFT;
        if ((::GetAsyncKeyState(VK_LWIN) | ::GetAsyncKeyState(VK_RWIN)) & 0x8000) modsDown_ |= MOD_WIN;

        hasLastAbs_ = false;
        accDx_ = 0;
        accDy_ = 0;
        wheelAccV_ = 0;
        wheelAccH_ = 0;

        // 让被控端从干净状态开始（上一轮会话可能残留按下状态）
        proto::InputEvent e;
        e.type = static_cast<u8>(proto::InputType::ReleaseAll);
        e.flags = 0;
        e.seq = seq_++;
        e.a = 0;
        e.b = 0;
        push_event(e);
    } else {
        captured_ = false;   // 先置 false：钩子才会放行下面补发的本地按键

        ::ClipCursor(nullptr);
        int guard = 0;
        while (::ShowCursor(TRUE) < 0 && ++guard < kCursorLoopMax) {}

        // 捕获期间被吞掉的「抬起」没进系统，这里补一次，避免退出后修饰键卡住
        reset_modifiers();

        accDx_ = 0;
        accDy_ = 0;
        wheelAccV_ = 0;
        wheelAccH_ = 0;
        hasLastAbs_ = false;

        // 通知被控端把按下的键都抬起来，避免远端卡键
        proto::InputEvent e;
        e.type = static_cast<u8>(proto::InputType::ReleaseAll);
        e.flags = 0;
        e.seq = seq_++;
        e.a = 0;
        e.b = 0;
        push_event(e);
    }
}

// -----------------------------------------------------------------------------
// 鼠标模式切换（捕获态下有效；见 input_capture.h 的说明）
// -----------------------------------------------------------------------------
void InputCapture::set_mouse_relative(bool on) {
    if (on == mouse_relative_) return;
    mouse_relative_ = on;
    accDx_ = 0;
    accDy_ = 0;
    hasLastAbs_ = false;
    wheelAccV_ = 0;
    wheelAccH_ = 0;
    if (!captured_) return;   // 非捕获态两种模式没区别（本机光标本来就可见）

    if (on) {
        // -> 相对模式：藏光标、限制在客户区、钉到中心（游戏自己读原始位移）
        int guard = 0;
        while (::ShowCursor(FALSE) >= 0 && ++guard < kCursorLoopMax) {}
        RECT rc{};
        if (::GetClientRect(hwnd_, &rc)) {
            POINT tl{rc.left, rc.top};
            POINT br{rc.right, rc.bottom};
            if (::ClientToScreen(hwnd_, &tl) && ::ClientToScreen(hwnd_, &br)) {
                RECT clip{tl.x, tl.y, br.x, br.y};
                ::ClipCursor(&clip);
            }
        }
        recenter_cursor();
    } else {
        // -> 绝对模式：恢复系统光标、解除限制（位置由会话层按远端光标摆）
        ::ClipCursor(nullptr);
        int guard = 0;
        while (::ShowCursor(TRUE) < 0 && ++guard < kCursorLoopMax) {}
    }
}

// -----------------------------------------------------------------------------
// 临时唤出本机光标（顶部工具栏点面板用）。
// 不改捕获状态本身：Raw Input 捕获和钩子照样工作，只是松开 ClipCursor +
// 显示系统光标，让用户能点到工具栏的按钮。工具栏隐藏后调用方传 false 恢复。
// -----------------------------------------------------------------------------
void InputCapture::temporary_show_cursor(bool on) {
    if (on) {
        ::ClipCursor(nullptr);
        int guard = 0;
        while (::ShowCursor(TRUE) < 0 && ++guard < kCursorLoopMax) {}
    } else {
        // 【绝对鼠标模式】本机光标就是"那个光标"，必须一直可见、不限制；
        // 这里如果照旧隐藏+ClipCursor+钉中心，关掉工具栏后用户光标会直接消失。
        if (!captured_ || !mouse_relative_) return;
        int guard = 0;
        while (::ShowCursor(FALSE) >= 0 && ++guard < kCursorLoopMax) {}
        // 恢复限制在客户区
        RECT rc{};
        if (::GetClientRect(hwnd_, &rc)) {
            POINT tl{rc.left, rc.top};
            POINT br{rc.right, rc.bottom};
            if (::ClientToScreen(hwnd_, &tl) && ::ClientToScreen(hwnd_, &br)) {
                RECT clip{tl.x, tl.y, br.x, br.y};
                ::ClipCursor(&clip);
            }
        }
        recenter_cursor();
    }
}

void InputCapture::recenter_cursor() {
    if (hwnd_ == nullptr) return;
    RECT rc{};
    if (!::GetClientRect(hwnd_, &rc)) return;
    POINT pt{(rc.left + rc.right) / 2, (rc.top + rc.bottom) / 2};
    if (!::ClientToScreen(hwnd_, &pt)) return;
    ::SetCursorPos(pt.x, pt.y);
}

void InputCapture::reset_modifiers() {
    // 场景：捕获前按住 Ctrl -> 捕获中松开（被钩子吞掉）-> 退出捕获后系统仍认为
    // Ctrl 按着。这里对「系统仍认为按下」的修饰键补发一次本地 KEYUP（用真实 vk，
    // 此时 captured_ 已经是 false，钩子会放行），然后清空本地跟踪状态。
    const WORD vks[5] = {VK_CONTROL, VK_MENU, VK_SHIFT, VK_LWIN, VK_RWIN};
    INPUT in[5];
    UINT n = 0;
    for (int i = 0; i < 5; ++i) {
        if ((::GetAsyncKeyState(vks[i]) & 0x8000) == 0) continue;
        INPUT k{};
        k.type = INPUT_KEYBOARD;
        k.ki.wVk = vks[i];
        k.ki.dwFlags = KEYEVENTF_KEYUP;
        k.ki.dwExtraInfo = kInjectedMagic;
        in[n++] = k;
    }
    if (n > 0) {
        ::SendInput(n, in, sizeof(INPUT));
    }
    modsDown_ = 0;
}

bool InputCapture::hotkey_match(u32 vkey) const {
    if (hotkeyVk_ == 0 || vkey != hotkeyVk_) return false;
    // 只信钩子里手动跟踪的状态：吞键后系统状态可能是过期的，混着用会误触发
    return (modsDown_ & hotkeyMods_) == hotkeyMods_;
}

// -----------------------------------------------------------------------------
// 低层键盘钩子
//
//  跑在系统调用链上（安装钩子的线程上下文），必须极快：
//  不做分配、不做 IO、不打日志、不 SendInput。只做位运算 + 往预分配环形缓冲写。
// -----------------------------------------------------------------------------
long long __stdcall InputCapture::kb_hook_proc(int code, unsigned long long wparam, long long lparam) {
    InputCapture* self = s_instance_;
    if (code < 0 || self == nullptr || !self->captured_) {
        return ::CallNextHookEx(nullptr, code, wparam, lparam);
    }

    const bool down = (wparam == WM_KEYDOWN || wparam == WM_SYSKEYDOWN);
    const bool up   = (wparam == WM_KEYUP   || wparam == WM_SYSKEYUP);
    if (!down && !up) {
        return ::CallNextHookEx(nullptr, code, wparam, lparam);
    }

    const KBDLLHOOKSTRUCT* kb = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lparam);
    if (kb == nullptr) {
        return ::CallNextHookEx(nullptr, code, wparam, lparam);
    }

    const u32 vk = kb->vkCode;
    if (vk == 0 || vk == 0xFF) {   // VK_NONE / 无 vk 的「假键」
        return ::CallNextHookEx(nullptr, code, wparam, lparam);
    }
    if (kb->dwExtraInfo == kInjectedMagic) {   // 自己注入的（reset_modifiers）直接放行
        return ::CallNextHookEx(nullptr, code, wparam, lparam);
    }

    // 修饰键状态只能自己记：这个钩子把键吞了之后 GetAsyncKeyState 不会更新
    self->modsDown_ = update_mods_mask(self->modsDown_, vk, up);

    // 释放快捷键：置标志并放行该键（用户靠它退出捕获），不推给远端
    if (self->hotkey_match(vk)) {
        if (!up) self->releaseRequested_ = true;
        return ::CallNextHookEx(nullptr, code, wparam, lparam);
    }

    // 其余按键统一在这里产生事件（键盘事件只有这一处产生，见文件头第 1 条）
    self->push_key(vk, up);

    if (self->blockSystemKeys_) {
        return 1;   // 吞掉：阻断 Win / Alt+Tab / Alt+F4 / Ctrl+Esc / Alt+Esc 等
    }
    return ::CallNextHookEx(nullptr, code, wparam, lparam);
}

} // namespace ms::client
