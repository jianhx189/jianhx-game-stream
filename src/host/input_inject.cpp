// =============================================================================
//  input_inject.cpp - 键鼠注入（被控端，SendInput）
//
//  为什么这么写（游戏能不能「当成真实输入」的关键）：
//   1) 键盘一律走「扫描码」注入：wVk = 0 + wScan = scancode + KEYEVENTF_SCANCODE。
//      只设 wVk 的注入在 DirectInput / Raw Input 游戏里会被过滤掉（很多游戏
//      忽略 LLKHF_INJECTED 之外的 vk 注入，但对扫描码是照单全收）。
//   2) 整批事件合成一次 SendInput 调用：一次 UDP 包里的十几个事件只花一次
//      系统调用开销，注入线程不会被 syscall 拖到错过下一帧的输入。
//   3) 每个事件都带 dwExtraInfo 标记（'MSTR'），方便远控端钩子放行自己注入的
//      按键，也方便以后定位「输入是谁发的」。
//   4) 维护 keysDown_/buttonsDown_ 状态，断线/退出捕获时 release_all() 兜底，
//      否则会出现「W 一直按着、鼠标一直转」这种自用工具最经典的翻车现场。
// =============================================================================

#include <windows.h>

#include "input_inject.h"
#include "../common/log.h"

namespace ms::host {
namespace {

// 一批最多塞多少个 INPUT：128 * sizeof(INPUT)(40B, x64) ≈ 5KB 栈，够用又不浪费
constexpr UINT kBatchMax = 128;

// 注入标记（dwExtraInfo），远控端低层钩子看到这个值直接放行
constexpr ULONG_PTR kInjectedMagic = static_cast<ULONG_PTR>(0x4D535452u);  // 'MSTR'

// 绝对坐标归一化到 0..65535（MOUSEEVENTF_ABSOLUTE + VIRTUALDESK 的约定）
LONG normalize_axis(i32 v, int extent) {
    if (extent <= 1) return 0;
    double t = static_cast<double>(v) / static_cast<double>(extent - 1);
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;
    return static_cast<LONG>(t * 65535.0 + 0.5);
}

// 相对位移单轴上限：异常/恶意的巨大 delta 打到系统会表现为"鼠标瞬移"，
// 甚至让某些游戏把视角甩飞。RustDesk 对同样的风险做了 ±10000 的钳制。
constexpr i32 kMaxRelativeDelta = 10000;

i32 clamp_delta(i32 v) {
    if (v > kMaxRelativeDelta) return kMaxRelativeDelta;
    if (v < -kMaxRelativeDelta) return -kMaxRelativeDelta;
    return v;
}

// 用 SendInput 做「绝对像素坐标」移动：归一化到虚拟桌面 0..65535。
// 作为 SetCursorPos 失败时的兜底（见绝对映射那段注释）：
//   * SetCursorPos 失败通常发生在独占全屏游戏、锁屏/安全桌面、或光标被裁剪时；
//   * SendInput 走输入队列，不依赖光标可写性，游戏用 RawInput 也能收到。
bool abs_move_sendinput(int x, int y) {
    const int vx = ::GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int vy = ::GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int vw = ::GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int vh = ::GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (vw <= 1 || vh <= 1) return false;
    int nx = int((double(x - vx) * 65535.0) / double(vw - 1));
    int ny = int((double(y - vy) * 65535.0) / double(vh - 1));
    if (nx < 0) nx = 0; if (nx > 65535) nx = 65535;
    if (ny < 0) ny = 0; if (ny > 65535) ny = 65535;
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dx = LONG(nx);
    in.mi.dy = LONG(ny);
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    in.mi.dwExtraInfo = kInjectedMagic;
    if (::SendInput(1, &in, sizeof(INPUT)) != 1) return false;
    // 再把系统光标对齐到同一像素（SendInput 的归一化会带来 ±1px 误差，
    // 绝对模式下"远端光标 = 本机鼠标位置"要的就是像素级一致）
    ::SetCursorPos(x, y);
    return true;
}

INPUT make_mouse_move(i32 dx, i32 dy, bool absolute, int width, int height) {
    INPUT in{};
    in.type = INPUT_MOUSE;
    if (absolute) {
        // 非相对模式：a/b 是桌面绝对像素坐标，归一化后配 ABSOLUTE|VIRTUALDESK
        in.mi.dx = normalize_axis(dx, width);
        in.mi.dy = normalize_axis(dy, height);
        in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    } else {
        // 相对模式：dx/dy 原样传（钳掉异常巨值），游戏从原始输入里读相对位移，本机光标不动
        in.mi.dx = static_cast<LONG>(clamp_delta(dx));
        in.mi.dy = static_cast<LONG>(clamp_delta(dy));
        in.mi.dwFlags = MOUSEEVENTF_MOVE;
    }
    in.mi.dwExtraInfo = kInjectedMagic;
    return in;
}

// 返回 false 表示按键编号不认识（1..5 之外）
bool make_mouse_button(int button, bool down, INPUT& out) {
    INPUT in{};
    in.type = INPUT_MOUSE;
    switch (button) {
    case 1: in.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN   : MOUSEEVENTF_LEFTUP;   break;
    case 2: in.mi.dwFlags = down ? MOUSEEVENTF_RIGHTDOWN  : MOUSEEVENTF_RIGHTUP;  break;
    case 3: in.mi.dwFlags = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP; break;
    case 4:
    case 5:
        // 侧键：XDOWN/XUP 靠 mouseData 区分 XBUTTON1 / XBUTTON2
        in.mi.dwFlags = down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
        in.mi.mouseData = (button == 4) ? XBUTTON1 : XBUTTON2;
        break;
    default:
        return false;
    }
    in.mi.dwExtraInfo = kInjectedMagic;
    out = in;
    return true;
}

// 协议里的滚轮增量约定是「刻度数」；同时兼容对端直接发 ±120 整数倍的原始增量
i32 wheel_to_mouse_data(i32 v) {
    if (v == 0) return 0;
    if (v % WHEEL_DELTA == 0) return v;    // 已经是原始增量（±120、±240…）
    return v * WHEEL_DELTA;                // ±1、±2… 刻度 -> ±120、±240…
}

// 一次滚动最多产出 2 个 INPUT（垂直 + 水平各一）
int make_mouse_wheel(i32 vert, i32 horiz, INPUT out[2]) {
    int n = 0;
    if (vert != 0) {
        INPUT in{};
        in.type = INPUT_MOUSE;
        in.mi.dwFlags = MOUSEEVENTF_WHEEL;
        in.mi.mouseData = static_cast<DWORD>(wheel_to_mouse_data(vert));
        in.mi.dwExtraInfo = kInjectedMagic;
        out[n++] = in;
    }
    if (horiz != 0) {
        INPUT in{};
        in.type = INPUT_MOUSE;
        in.mi.dwFlags = MOUSEEVENTF_HWHEEL;
        in.mi.mouseData = static_cast<DWORD>(wheel_to_mouse_data(horiz));
        in.mi.dwExtraInfo = kInjectedMagic;
        out[n++] = in;
    }
    return n;
}

INPUT make_key(u16 scancode, bool extended, bool up) {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = 0;                      // 关键：不给 vk，只给扫描码
    in.ki.wScan = static_cast<WORD>(scancode);
    in.ki.dwFlags = KEYEVENTF_SCANCODE;
    if (extended) in.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;   // E0 前缀（方向键/右 Ctrl/RWin…）
    if (up)       in.ki.dwFlags |= KEYEVENTF_KEYUP;
    in.ki.dwExtraInfo = kInjectedMagic;
    return in;
}

// 提交一批 INPUT，返回实际提交数（< n = 有事件被系统拒绝，如 UIPI）
UINT send_now(INPUT* items, UINT n) {
    if (n == 0) return 0;
    const UINT sent = ::SendInput(n, items, sizeof(INPUT));
    if (sent < n) {
        // UIPI / 输入桌面被占用 / 有更高权限窗口在前台时会短交，记一笔便于排查
        MS_WARN("input_inject: SendInput 只提交 %u/%u 个事件，GetLastError=%lu",
                static_cast<unsigned>(sent), static_cast<unsigned>(n),
                static_cast<unsigned long>(::GetLastError()));
    }
    return sent;
}

// 返回实际提交的事件数（一批输入事件可能展开成多个 INPUT，按事件计）
UINT flush_batch_counted(INPUT* batch, UINT& n) {
    if (n == 0) return 0;
    const UINT sent = send_now(batch, n);
    n = 0;
    return sent;
}

void flush_batch(INPUT* batch, UINT& n) {
    flush_batch_counted(batch, n);
}

} // namespace

// -----------------------------------------------------------------------------
// 批量应用
// -----------------------------------------------------------------------------
void InputInjector::apply(const proto::InputEvent* ev, size_t count, u32* appliedOut) {
    if (appliedOut) *appliedOut = 0;
    if (!ev || count == 0) return;
    u32 applied = 0;

    INPUT batch[kBatchMax];
    UINT n = 0;

    for (size_t i = 0; i < count; ++i) {
        const proto::InputEvent& e = ev[i];
        switch (static_cast<proto::InputType>(e.type)) {
        case proto::InputType::MouseMove: {
            if ((e.flags & proto::kInputFlagAbsMove) != 0) {
                // 绝对映射（桌面/游戏菜单）：直接把光标摆到远端像素坐标。
                // 用 SetCursorPos 而不是 SendInput+ABSOLUTE：后者要按虚拟桌面归一化，
                // 多显示器/DPI 下容易偏，而且会经过鼠标加速；SetCursorPos 就是屏幕像素，
                // "远端光标 = 你本机鼠标位置"是一比一的。
                flush_batch(batch, n);   // 先把之前的按键/相对位移提交，保持顺序
                int ax = 0, ay = 0;
                if ((e.flags & proto::kInputFlagAbsPhysical) != 0) {
                    // v2 约定：a/b 已是虚拟桌面物理像素（客户端换算好了显示器
                    // 原点/缩放串流比例），直接用 —— 修复"缩放串流时点击偏移"。
                    ax = int(e.a);
                    ay = int(e.b);
                } else {
                    // v1 约定：a/b 是采集显示器内坐标，加上显示器原点
                    ax = originX_ + int(e.a);
                    ay = originY_ + int(e.b);
                }
                if (!::SetCursorPos(ax, ay)) {
                    // 【断触根因之一】SetCursorPos 在某些状态会失败：游戏独占全屏、
                    // 桌面被锁屏/UAC 安全桌面、或光标被 ClipCursor 限制。
                    // 以前这里只打日志，鼠标就彻底不动了（真机日志里一次会话
                    // 失败 1210 次 = 用户说的"鼠标卡住/断触"）。
                    // 现在退一步改用 SendInput 绝对移动（按虚拟桌面归一化），
                    // 它走的是输入队列，游戏用 RawInput 也能收到。
                    if (!abs_move_sendinput(ax, ay)) {
                        ++absFailStreak_;
                        // 失败日志限流：连续失败每 200 次才打一条，避免刷爆日志
                        if (absFailStreak_ == 1 || (absFailStreak_ % 200) == 0) {
                            MS_WARN("input_inject: 绝对移动失败 %d 次（SetCursorPos 与 "
                                    "SendInput 都不行），最近坐标 (%d,%d)，err=%lu",
                                    absFailStreak_, ax, ay,
                                    static_cast<unsigned long>(::GetLastError()));
                        }
                    } else {
                        if (absFailStreak_ > 0) {
                            MS_INFO("input_inject: 绝对移动已恢复（之前连续失败 %d 次）",
                                    absFailStreak_);
                            absFailStreak_ = 0;
                        }
                        ++applied;
                    }
                } else {
                    ++applied;
                    static int cnt = 0;
                    if (cnt == 0) {
                        MS_INFO("[鼠标] 绝对映射注入已生效，首个坐标 (%d,%d)", ax, ay);
                    } else if ((cnt % 200) == 0) {
                        MS_DEBUG("[鼠标] 绝对注入 (%d,%d) 第 %d 次", ax, ay, cnt + 1);
                    }
                    ++cnt;
                }
                break;
            }
            batch[n++] = make_mouse_move(e.a, e.b, !relative_, width_, height_);
            ++applied;
            if (n >= kBatchMax) applied += flush_batch_counted(batch, n);
            break;
        }

        case proto::InputType::MouseButton: {
            // 远控端对按钮连发 3 份（seq 相同）抗丢包，这里丢弃相邻的重复份
            if (hasBtnSeq_ && lastBtnSeq_ == e.seq) break;
            lastBtnSeq_ = e.seq;
            hasBtnSeq_ = true;

            const bool down = (e.b != 0);
            INPUT in{};
            if (!make_mouse_button(e.a, down, in)) {
                MS_DEBUG("input_inject: 忽略未知鼠标按键 %d", static_cast<int>(e.a));
                break;
            }
            batch[n++] = in;
            if (n >= kBatchMax) applied += flush_batch_counted(batch, n);
            remember_button(e.a, down);
            break;
        }

        case proto::InputType::MouseWheel: {
            INPUT w[2];
            const int wn = make_mouse_wheel(e.a, e.b, w);
            for (int k = 0; k < wn; ++k) {
                batch[n++] = w[k];
                if (n >= kBatchMax) applied += flush_batch_counted(batch, n);
            }
            break;
        }

        case proto::InputType::Key: {
            if (hasKeySeq_ && lastKeySeq_ == e.seq) break;
            lastKeySeq_ = e.seq;
            hasKeySeq_ = true;

            const bool up  = (e.flags & proto::kInputFlagKeyUp) != 0;
            const bool ext = (e.flags & proto::kInputFlagKeyExt) != 0;
            const u16 sc = static_cast<u16>(e.a & 0xFFFF);
            if (sc == 0) break;              // VK_NONE 之类的「假键」，没有扫描码可发

            batch[n++] = make_key(sc, ext, up);
            ++applied;
            if (n >= kBatchMax) applied += flush_batch_counted(batch, n);
            remember_key(sc, !up);
            if (sc < 256) keysExt_[sc] = ext;   // release_all 要原样带回去
            break;
        }

        case proto::InputType::ReleaseAll:
            // 先把本批之前的输入提交掉，再做整体抬起，保证顺序不乱
            flush_batch(batch, n);
            release_all();
            break;

        default:
            MS_DEBUG("input_inject: 未知输入事件类型 %u", static_cast<unsigned>(e.type));
            break;
        }
    }

    applied += flush_batch_counted(batch, n);
    if (appliedOut) *appliedOut = applied;
}

// -----------------------------------------------------------------------------
// 兜底抬起（断线 / 退出捕获必须调用）
// -----------------------------------------------------------------------------
void InputInjector::release_all() {
    INPUT batch[kBatchMax];
    UINT n = 0;
    int released = 0;

    for (int sc = 0; sc < 256; ++sc) {
        if (!keysDown_[sc]) continue;
        keysDown_[sc] = false;
        const bool ext = keysExt_[sc];
        keysExt_[sc] = false;
        batch[n++] = make_key(static_cast<u16>(sc), ext, true);
        ++released;
        if (n >= kBatchMax) flush_batch(batch, n);
    }

    for (int b = 1; b < 8; ++b) {
        if (!buttonsDown_[b]) continue;
        buttonsDown_[b] = false;
        INPUT in{};
        if (!make_mouse_button(b, false, in)) continue;
        batch[n++] = in;
        ++released;
        if (n >= kBatchMax) flush_batch(batch, n);
    }

    flush_batch(batch, n);
    if (released > 0) {
        MS_INFO("input_inject: release_all 抬起 %d 个按键/按钮", released);
    }
}

// -----------------------------------------------------------------------------
// 单事件路径（供少量即时注入使用；批量路径见 apply()）
// -----------------------------------------------------------------------------
void InputInjector::do_mouse_move(i32 dx, i32 dy) {
    INPUT in = make_mouse_move(dx, dy, !relative_, width_, height_);
    send_now(&in, 1);
}

void InputInjector::do_mouse_button(i32 button, bool down) {
    INPUT in{};
    if (!make_mouse_button(static_cast<int>(button), down, in)) {
        MS_DEBUG("input_inject: 忽略未知鼠标按键 %d", static_cast<int>(button));
        return;
    }
    send_now(&in, 1);
}

void InputInjector::do_mouse_wheel(i32 vert, i32 horiz) {
    INPUT w[2];
    const int wn = make_mouse_wheel(vert, horiz, w);
    send_now(w, static_cast<UINT>(wn));
}

void InputInjector::do_key(u16 scancode, bool extended, bool up) {
    if (scancode == 0) return;
    INPUT in = make_key(scancode, extended, up);
    send_now(&in, 1);
}

// -----------------------------------------------------------------------------
// 按下状态跟踪
// -----------------------------------------------------------------------------
void InputInjector::remember_key(u16 scancode, bool down) {
    if (scancode < 256) keysDown_[scancode] = down;
}

void InputInjector::remember_button(int button, bool down) {
    if (button >= 0 && button < 8) buttonsDown_[button] = down;
}

} // namespace ms::host
