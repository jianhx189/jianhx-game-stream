// =============================================================================
//  session_toolbar.cpp - 远控中的顶部工具栏（RustDesk 式）实现
//
//  命门：绝不能打断游戏。
//   * 工具栏与画质面板都是渲染窗口的 WS_CHILD 子窗口，扩展样式
//     WS_EX_NOACTIVATE | WS_EX_TOPMOST —— 点击不会激活、键盘焦点留在渲染窗口，
//     游戏键鼠捕获状态原样保留；再用 SetWindowPos(HWND_TOPMOST) 保证盖在视频上。
//   * WM_MOUSEACTIVATE 返回 MA_NOACTIVATE，点击永不产生激活。
//   * 按钮点击之后主动 ::SetFocus(::GetParent(wnd_)) 把焦点还给渲染窗口。
//   * 光标碰顶边滑出，3 秒无操作自动收回；展开画质面板期间不自动隐藏。
//   * 绝不使用 MessageBox / 任何弹窗 —— 一切反馈只进顶部状态字。
//
//  线程模型：工具栏子窗口与渲染父窗口在同一线程（同一线程创建、同一个
//  消息队列天然同线程），因此子窗口过程里直接调用 cmds_.on_xxx 回调没有任何
//  线程安全问题；这些回调本身就是设计成在父窗口线程被调用的。
//
//  窗口层级：
//    渲染窗口（视频）
//    ├─ MiniStreamToolbar（通栏 44px，贴顶，NOACTIVATE|TOPMOST，初始隐藏）
//    │   └─ 全部控件（STATIC/BUTTON/CHECKBOX）
//    └─ MiniStreamQualityPanel（工具栏正下方 220px 面板，NOACTIVATE，初始隐藏）
//        └─ 预设/分辨率/帧率/码率下拉、码率输入框、关键帧间隔下拉、「应用」
//
//  光标协同：父窗口的 InputCapture 捕获期间会 ShowCursor(FALSE)，
//  工具栏 show(true) 时发 WM_APP+0x520（wParam=1）让父窗口暂时显示光标，
//  show(false) 时发 wParam=0 交还。父窗口负责实际光标状态，这里只通知。
//
//  未实测部分（本模块无法独立运行 GUI）：
//   * 窗口层级遮挡关系（D3D11 全屏独占下子窗口可见性）依赖父窗口是窗口化
//     或 borderless 全屏；真独占全屏时子窗口不会被显示，需父窗口用
//     borderless 模式（本工具的常规形态）。
//   * 3 秒自动隐藏的手感、下拉在 NOACTIVATE 子窗口里的键盘操作体验未实测。
// =============================================================================
#include "session_toolbar.h"
#include "../common/log.h"

#include <windows.h>
#include <windowsx.h>   // GET_X_LPARAM / GET_Y_LPARAM

#include <cstdio>
#include <string>
#include <vector>

namespace ms::client {
namespace {

// ---- 自定义消息：通知父窗口显示/隐藏光标（wParam: 1=显示 0=交还）----
constexpr UINT WM_TB_CURSOR_OVERLAY = WM_APP + 0x520;
// 控制线程请求刷新工具栏状态字（见 session_toolbar.h 里的跨线程说明）
constexpr UINT WM_TB_SYNC           = WM_APP + 0x521;

constexpr wchar_t kToolbarClass[] = L"MiniStreamToolbar";
constexpr wchar_t kPanelClass[]   = L"MiniStreamQualityPanel";
constexpr wchar_t kDockClass[]    = L"MiniStreamDock";

// ---- 尺寸（客户区像素）----
constexpr int kToolbarH = 44;    // 通栏高度
constexpr int kPanelH    = 264;   // 展开的画质面板高度（6 行：预设/分辨率/帧率/关键帧/码率/锐化 + 应用）
constexpr int kPanelW    = 560;
constexpr int kEdge      = 12;   // 左右留白
constexpr int kCtrlH     = 24;   // 控件统一高
constexpr int kComboW    = 150;
constexpr int kEditW     = 80;

// ---- 顶部小悬浮按钮（点击展开工具栏）----
constexpr int kDockW = 64;
constexpr int kDockH = 22;
constexpr int kDockTopMargin = 5;
constexpr BYTE kDockAlpha = 160;        // 平时半透明，不挡画面
constexpr BYTE kDockAlphaHover = 235;   // 悬停时实一点，好点

// ---- 配色 ----
constexpr COLORREF kToolbarBg = RGB(28, 30, 34);
constexpr COLORREF kPanelBg   = RGB(38, 40, 46);
constexpr COLORREF kTextFg    = RGB(230, 232, 236);
constexpr COLORREF kDangerFg = RGB(232, 80, 80);
constexpr COLORREF kSepLine   = RGB(60, 63, 70);

// ---- 交互参数 ----
constexpr int64_t kAutoHideUs = 3 * 1000 * 1000;  // 3 秒无操作自动隐藏
constexpr int kTopEdgePx = 2;                     // 光标距顶边多少像素算"碰边"
constexpr int64_t kPanelAutoHideUs = 10 * 1000 * 1000;  // 画质面板 10 秒无操作自动收起

// keyframe 下拉显示文本与秒值。
// 0 = 按需（推荐）：只在远控端缺帧/换目标时请求 IDR。
// 依据（UU远程/GameViewer 逆向报告）：周期 IDR 是"规律性卡顿"的常见根因，
// 它们长稳会话 24 万帧只有 7 个 I 帧；改成按需后画质更稳、码率不锯齿。
struct KfOption { const wchar_t* label; int sec; };
const KfOption kKfOptions[] = {
    { L"按需（推荐）", 0 },
    { L"1 秒",  1 },
    { L"2 秒",  2 },
    { L"3 秒",  3 },
    { L"5 秒",  5 },
    { L"10 秒", 10 },
};
constexpr int kKfCount = int(sizeof(kKfOptions) / sizeof(kKfOptions[0]));

// 锐化档位（下拉显示文本 + 实际强度，强度要给 renderer::set_sharpen）
struct SharpenOption { const wchar_t* label; float strength; };
const SharpenOption kSharpenOptions[] = {
    { L"关闭", 0.0f },
    { L"低",   0.35f },
    { L"中",   0.70f },
    { L"高",   1.20f },
};
constexpr int kSharpenCount = int(sizeof(kSharpenOptions) / sizeof(kSharpenOptions[0]));

// -----------------------------------------------------------------------------
// 小工具
// -----------------------------------------------------------------------------
std::wstring widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int need = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    if (need <= 0) return std::wstring();
    std::wstring w(size_t(need), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), &w[0], need);
    return w;
}

std::wstring get_edit_text(HWND h) {
    if (!h) return std::wstring();
    const int n = ::GetWindowTextLengthW(h);
    if (n <= 0) return std::wstring();
    std::wstring s(size_t(n) + 1, L'\0');
    const int got = ::GetWindowTextW(h, &s[0], n + 1);
    s.resize(got > 0 ? size_t(got) : 0);
    return s;
}

int parse_int(const std::wstring& s, int def) {
    if (s.empty()) return def;
    wchar_t* end = nullptr;
    const long v = ::wcstol(s.c_str(), &end, 10);
    if (end == s.c_str()) return def;
    if (v < 0 || v > 2147483647L) return def;
    return int(v);
}

HFONT make_message_font() {
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);
    if (::SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
        return ::CreateFontIndirectW(&ncm.lfMessageFont);   // 系统消息字体，9pt 量级
    }
    return nullptr;
}

HFONT make_mono_font() {
    // 状态字用等宽字体，数字不跳动
    return ::CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                         CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
}

// 码率下拉项的显示文本（0 = 自适应）
std::wstring bitrate_label(int kbps) {
    if (kbps <= 0) return L"自适应";
    wchar_t buf[32]{};
    _snwprintf_s(buf, _TRUNCATE, L"%d Mbps", kbps / 1000);
    return buf;
}

void fill_combo(HWND cb, const std::vector<std::wstring>& items, int defSel) {
    ::SendMessageW(cb, CB_RESETCONTENT, 0, 0);
    for (const auto& s : items) {
        ::SendMessageW(cb, CB_ADDSTRING, 0, LPARAM(s.c_str()));
    }
    if (defSel >= 0) ::SendMessageW(cb, CB_SETCURSEL, WPARAM(defSel), 0);
}

// 关掉控件视觉主题：深色面板里的下拉/编辑框才能用 WM_CTLCOLOR* 上深色底
// （带主题时 combobox 由主题自己画白底，WM_CTLCOLOR 会被忽略，面板会很难看）
void disable_theme(HWND h) {
    using Fn = HRESULT(WINAPI*)(HWND, LPCWSTR, LPCWSTR);
    static Fn fn = []() -> Fn {
        HMODULE m = ::LoadLibraryW(L"uxtheme.dll");
        return m ? reinterpret_cast<Fn>(::GetProcAddress(m, "SetWindowTheme")) : nullptr;
    }();
    if (fn && h) fn(h, L"", L"");
}

// 深色扁平按钮（owner-draw）：和工具栏/面板配色一致，按下去有反馈
void draw_owner_button(const DRAWITEMSTRUCT* dis, HFONT font, bool danger) {
    if (!dis) return;
    HDC dc = dis->hDC;
    RECT rc = dis->rcItem;
    const bool pressed  = (dis->itemState & ODS_SELECTED) != 0;
    const bool disabled = (dis->itemState & ODS_DISABLED) != 0;
    COLORREF bg = pressed ? RGB(56, 62, 74) : RGB(46, 50, 58);
    COLORREF bd = pressed ? RGB(96, 150, 240) : RGB(76, 82, 94);
    COLORREF fg = disabled ? RGB(140, 144, 152) : RGB(232, 236, 242);
    if (danger) {
        bg = pressed ? RGB(96, 40, 40) : RGB(58, 34, 34);
        bd = pressed ? RGB(230, 110, 110) : RGB(120, 60, 60);
        fg = disabled ? RGB(150, 110, 110) : kDangerFg;
    }
    HBRUSH b = ::CreateSolidBrush(bg);
    HPEN   p = ::CreatePen(PS_SOLID, 1, bd);
    HGDIOBJ ob = ::SelectObject(dc, b);
    HGDIOBJ op = ::SelectObject(dc, p);
    ::RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, 6, 6);
    ::SelectObject(dc, op);
    ::SelectObject(dc, ob);
    ::DeleteObject(p);
    ::DeleteObject(b);
    wchar_t label[128]{};
    ::GetWindowTextW(dis->hwndItem, label, 128);
    ::SetBkMode(dc, TRANSPARENT);
    ::SetTextColor(dc, fg);
    HGDIOBJ of = font ? ::SelectObject(dc, font) : nullptr;
    ::DrawTextW(dc, label, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    if (of) ::SelectObject(dc, of);
}

// 面板里的行标签
HWND make_label(HWND parent, const wchar_t* text, HFONT font) {
    HWND h = ::CreateWindowExW(0, L"STATIC", text,
                               WS_CHILD | WS_VISIBLE | SS_NOPREFIX,
                               0, 0, 10, 10, parent, nullptr,
                               ::GetModuleHandleW(nullptr), nullptr);
    if (h && font) ::SendMessageW(h, WM_SETFONT, WPARAM(font), TRUE);
    return h;
}

} // namespace

// 静态实例指针（进程内一个工具栏）
SessionToolbar* SessionToolbar::s_this_ = nullptr;

// =============================================================================
//  init / shutdown
// =============================================================================
bool SessionToolbar::init(HWND parentWnd, std::string& err) {
    if (wnd_) { err = "toolbar already initialized"; return false; }   // ASCII err
    if (!parentWnd || !::IsWindow(parentWnd)) {
        err = "invalid parent window";
        return false;
    }
    s_this_ = this;
    parent_ = parentWnd;
    suppressComboEvents_ = true;   // 建控件期间吞掉一切 CBN_SELCHANGE

    const HINSTANCE hi = ::GetModuleHandleW(nullptr);

    // ---- 字体 ----
    font_ = make_message_font();
    fontMono_ = make_mono_font();
    brushToolbar_ = ::CreateSolidBrush(kToolbarBg);
    brushPanel_   = ::CreateSolidBrush(kPanelBg);

    // ---- 注册窗口类（幂等）----
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.hInstance = hi;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = ::CreateSolidBrush(kToolbarBg);

    if (!::GetClassInfoExW(hi, kToolbarClass, &wc) || wc.lpfnWndProc != toolbar_proc) {
        wc.lpfnWndProc = toolbar_proc;
        wc.lpszClassName = kToolbarClass;
        ::RegisterClassExW(&wc);
    }
    if (!::GetClassInfoExW(hi, kPanelClass, &wc) || wc.lpfnWndProc != panel_proc) {
        wc.lpfnWndProc = panel_proc;
        wc.lpszClassName = kPanelClass;
        wc.hbrBackground = ::CreateSolidBrush(kPanelBg);
        ::RegisterClassExW(&wc);
    }
    if (!::GetClassInfoExW(hi, kDockClass, &wc) || wc.lpfnWndProc != dock_proc) {
        wc.lpfnWndProc = dock_proc;
        wc.lpszClassName = kDockClass;
        wc.hbrBackground = ::CreateSolidBrush(kToolbarBg);
        wc.hCursor = ::LoadCursorW(nullptr, IDC_HAND);
        ::RegisterClassExW(&wc);
    }

    // ---- 父窗口客户区尺寸（通栏宽度跟随）----
    RECT rc{};
    ::GetClientRect(parent_, &rc);
    const int parentW = int(rc.right), parentH = int(rc.bottom);

    // ---- 工具栏容器条：WS_CHILD + NOACTIVATE + TOPMOST，贴顶 44px ----
    // 说明：子窗口的 WS_EX_TOPMOST 本身只影响顶层窗口间的 Z 序，对子窗口无意义，
    // 这里保留该扩展样式位与任务描述一致；真正保证盖在视频上的是子窗口天然
    // 的 Z 序（后创建/置顶的子窗口在上）+ SetWindowPos(HWND_TOPMOST)。
        RECT wrc{};
    ::GetWindowRect(parent_, &wrc);
    wnd_ = ::CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                              kToolbarClass, L"", WS_POPUP | WS_BORDER | WS_CLIPCHILDREN,
                              wrc.left, wrc.top, parentW, kToolbarH,
                              parent_, nullptr, hi, this);
    if (!wnd_) { err = "CreateWindow(toolbar) failed"; shutdown(); return false; }

    // ---- 控件 ----
    // 状态字（STATIC，透明底浅字，WM_CTLCOLORSTATIC 处理）
    stStatus_ = ::CreateWindowExW(0, L"STATIC", L"",
                                  WS_CHILD | WS_VISIBLE | SS_NOPREFIX | SS_ENDELLIPSIS,
                                  0, 0, 10, 10, wnd_, nullptr, hi, nullptr);
    // 分隔竖线（1px 宽 STATIC，WM_CTLCOLORSTATIC 上色）
    // ---- btnChevron_：「画质 ▾」----
    btnChevron_ = ::CreateWindowExW(0, L"BUTTON", L"画质 ▾",
                                    WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_OWNERDRAW,
                                    0, 0, 10, 10, wnd_, nullptr, hi, nullptr);
    btnFullscreen_ = ::CreateWindowExW(0, L"BUTTON", L"全屏",
                                       WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_CENTER | BS_OWNERDRAW,
                                       0, 0, 10, 10, wnd_, nullptr, hi, nullptr);
    // 缩放模式：点一下循环 智能缩放 -> 拉伸填充 -> 原始大小(1:1)
    btnScale_ = ::CreateWindowExW(0, L"BUTTON", L"缩放:智能",
                                  WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_CENTER | BS_OWNERDRAW,
                                  0, 0, 10, 10, wnd_, nullptr, hi, nullptr);
    chkOverlay_ = ::CreateWindowExW(0, L"BUTTON", L"悬浮条",
                                    WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                    0, 0, 10, 10, wnd_, nullptr, hi, nullptr);
    // 声音开关：只控制本机播放（被控端一直发，切回来立刻有声）
    chkAudio_ = ::CreateWindowExW(0, L"BUTTON", L"声音",
                                  WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                  0, 0, 10, 10, wnd_, nullptr, hi, nullptr);
    // 断开按钮：BS_PUSHBUTTON + owner-draw 文字上红色（不弹窗、不抢焦点）
    btnDisconnect_ = ::CreateWindowExW(0, L"BUTTON", L"断开连接",
                                       WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_OWNERDRAW,
                                       0, 0, 10, 10, wnd_, nullptr, hi, nullptr);
    if (!stStatus_ || !btnChevron_ || !btnFullscreen_ || !chkOverlay_ || !btnDisconnect_ ||
        !btnScale_ || !chkAudio_) {
        err = "CreateWindow(controls) failed";
        shutdown();
        return false;
    }
    ::SendMessageW(chkOverlay_, BM_SETCHECK, BST_CHECKED, 0);
    ::SendMessageW(chkAudio_, BM_SETCHECK, BST_CHECKED, 0);

    // ---- 画质面板：工具栏正下方，贴左，同样 NOACTIVATE ----
        pnlQuality_ = ::CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                                    kPanelClass, L"", WS_POPUP | WS_BORDER | WS_CLIPCHILDREN,
                                    wrc.left, wrc.top + kToolbarH, kPanelW, kPanelH,
                                    parent_, nullptr, hi, this);
    if (!pnlQuality_) { err = "CreateWindow(quality panel) failed"; shutdown(); return false; }

    // ---- 面板控件 ----
    cbPreset_ = ::CreateWindowExW(0, L"COMBOBOX", L"",
                                  WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS,
                                  0, 0, 10, 10, pnlQuality_, nullptr, hi, nullptr);
    cbRes_ = ::CreateWindowExW(0, L"COMBOBOX", L"",
                               WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS,
                               0, 0, 10, 10, pnlQuality_, nullptr, hi, nullptr);
    cbFps_ = ::CreateWindowExW(0, L"COMBOBOX", L"",
                              WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS,
                              0, 0, 10, 10, pnlQuality_, nullptr, hi, nullptr);
    cbBitrate_ = ::CreateWindowExW(0, L"COMBOBOX", L"",
                                  WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS,
                                  0, 0, 10, 10, pnlQuality_, nullptr, hi, nullptr);
    edBitrate_ = ::CreateWindowExW(WS_EX_STATICEDGE, L"EDIT", L"",
                                   WS_CHILD | WS_VISIBLE | ES_NUMBER | ES_AUTOHSCROLL,
                                   0, 0, 10, 10, pnlQuality_, nullptr, hi, nullptr);
    btnApply_ = ::CreateWindowExW(0, L"BUTTON", L"应用",
                                  WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_OWNERDRAW,
                                  0, 0, 10, 10, pnlQuality_, nullptr, hi, nullptr);
    if (!cbPreset_ || !cbRes_ || !cbFps_ || !cbBitrate_ || !edBitrate_ || !btnApply_) {
        err = "CreateWindow(panel controls) failed";
        shutdown();
        return false;
    }

    // ---- 字号 ----
    if (font_) {
        for (HWND h : { stStatus_, btnChevron_, btnFullscreen_, btnScale_, chkOverlay_, chkAudio_,
                       btnDisconnect_, cbPreset_, cbRes_, cbFps_, cbBitrate_,
                       edBitrate_, btnApply_, cbKeyframe_, cbSharpen_, btnPanelClose_ }) {
            ::SendMessageW(h, WM_SETFONT, WPARAM(font_), TRUE);
        }
    }
    if (fontMono_) ::SendMessageW(stStatus_, WM_SETFONT, WPARAM(fontMono_), TRUE);

    // ---- 下拉内容 ----
    std::vector<std::wstring> presets;
    for (int i = 0; i < quality::kPresetCount; ++i)
        presets.push_back(widen(quality::preset_name(quality::Preset(i))));
    fill_combo(cbPreset_, presets, int(quality::Preset::Balanced));
    comboPresetCustomIdx_ = int(quality::Preset::Custom);

    std::vector<std::wstring> resLabels;
    for (const auto& r : quality::resolution_options()) resLabels.push_back(r.label);
    fill_combo(cbRes_, resLabels, 0);   // 默认「跟随被控端」

    std::vector<std::wstring> fpsLabels;
    for (int f : quality::fps_options()) { wchar_t b[16]{}; _snwprintf_s(b, _TRUNCATE, L"%d fps", f); fpsLabels.push_back(b); }
    fill_combo(cbFps_, fpsLabels, 1);   // 默认 60

    std::vector<std::wstring> brLabels;
    for (int b : quality::bitrate_options()) brLabels.push_back(bitrate_label(b));
    fill_combo(cbBitrate_, brLabels, 0);  // 默认自适应

    // 关键帧间隔下拉（与 cbFps_ 同行右侧）
    cbKeyframe_ = ::CreateWindowExW(0, L"COMBOBOX", L"",
                                    WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS,
                                    0, 0, 10, 10, pnlQuality_, nullptr, hi, nullptr);
    if (!cbKeyframe_) { err = "CreateWindow(keyframe combo) failed"; shutdown(); return false; }
    if (font_) ::SendMessageW(cbKeyframe_, WM_SETFONT, WPARAM(font_), TRUE);
    std::vector<std::wstring> kfLabels;
    for (int i = 0; i < kKfCount; ++i) kfLabels.push_back(kKfOptions[i].label);
    fill_combo(cbKeyframe_, kfLabels, 0);   // 默认「按需」（0 = 无限 GOP + 按需 IDR）

    // 锐化档位下拉（游戏画面细节；编码糊掉的字/远处目标靠它拉回来）
    cbSharpen_ = ::CreateWindowExW(0, L"COMBOBOX", L"",
                                   WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS,
                                   0, 0, 10, 10, pnlQuality_, nullptr, hi, nullptr);
    if (!cbSharpen_) { err = "CreateWindow(sharpen combo) failed"; shutdown(); return false; }
    if (font_) ::SendMessageW(cbSharpen_, WM_SETFONT, WPARAM(font_), TRUE);
    {
        std::vector<std::wstring> shLabels;
        for (int i = 0; i < kSharpenCount; ++i) shLabels.push_back(kSharpenOptions[i].label);
        fill_combo(cbSharpen_, shLabels,
                   sharpenLevel_ >= 0 && sharpenLevel_ < kSharpenCount ? sharpenLevel_ : 1);
    }

    // 编码格式下拉（H.264 / H.265）——和锐化同一行右侧，不额外占面板高度
    cbCodec_ = ::CreateWindowExW(0, L"COMBOBOX", L"",
                                 WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS,
                                 0, 0, 10, 10, pnlQuality_, nullptr, hi, nullptr);
    if (!cbCodec_) { err = "CreateWindow(codec combo) failed"; shutdown(); return false; }
    if (font_) ::SendMessageW(cbCodec_, WM_SETFONT, WPARAM(font_), TRUE);
    {
        std::vector<std::wstring> cLabels;
        for (const auto& c : quality::codec_options()) cLabels.push_back(c.label);
        fill_combo(cbCodec_, cLabels, codecValue_ == 2 ? 1 : 0);
    }

    // 面板右上角「收起」：画质按钮改成"只开不关"，双击也不会闪一下就没了；
    // 想主动关面板点这个（或者直接点一下画面）。
    btnPanelClose_ = ::CreateWindowExW(0, L"BUTTON", L"收起",
                                       WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_OWNERDRAW,
                                       0, 0, 10, 10, pnlQuality_, nullptr, hi, nullptr);
    if (!btnPanelClose_) { err = "CreateWindow(panel close) failed"; shutdown(); return false; }
    if (font_) ::SendMessageW(btnPanelClose_, WM_SETFONT, WPARAM(font_), TRUE);

    // ---- 面板行标签（否则用户看不出哪个下拉是什么）----
    stLblPreset_  = make_label(pnlQuality_, L"画质预设", font_);
    stLblRes_     = make_label(pnlQuality_, L"分辨率",   font_);
    stLblFps_     = make_label(pnlQuality_, L"帧率",     font_);
    stLblKf_      = make_label(pnlQuality_, L"关键帧",   font_);
    stLblBitrate_ = make_label(pnlQuality_, L"码率",     font_);
    stLblSharpen_ = make_label(pnlQuality_, L"锐化",     font_);
    stLblCodec_   = make_label(pnlQuality_, L"编码",     font_);
    if (!stLblPreset_ || !stLblRes_ || !stLblFps_ || !stLblKf_ || !stLblBitrate_ ||
        !stLblSharpen_ || !stLblCodec_) {
        err = "CreateWindow(panel labels) failed";
        shutdown();
        return false;
    }
    // 深色主题：关掉视觉主题后下拉框/输入框显示区才走 WM_CTLCOLOR*，
    // 才能和面板深色背景一致（否则是一排刺眼的白盒子）
    for (HWND h : { cbPreset_, cbRes_, cbFps_, cbBitrate_, cbKeyframe_, cbSharpen_,
                    cbCodec_, edBitrate_ }) {
        disable_theme(h);
    }

    // ---- 顶部小悬浮按钮（≡）：平时一直在，点一下展开工具栏 ----
    // 用分层窗口做半透明，既提示"这里有菜单"，又不至于挡游戏画面。
    dock_ = ::CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
                              kDockClass, L"", WS_POPUP,
                              wrc.left, wrc.top + kDockTopMargin, kDockW, kDockH,
                              parent_, nullptr, hi, this);
    if (!dock_) { err = "CreateWindow(dock) failed"; shutdown(); return false; }
    ::SetLayeredWindowAttributes(dock_, 0, kDockAlpha, LWA_ALPHA);

    // ---- 布局 ----
    rebuild_layout();

    // 初始：工具栏/面板隐藏，小悬浮按钮常驻
    ::ShowWindow(pnlQuality_, SW_HIDE);
    ::ShowWindow(wnd_, SW_HIDE);
    ::ShowWindow(dock_, SW_SHOWNA);
    place_windows();
    shown_ = false;
    expanded_ = false;

    // 调试开关：MINISTREAM_TOOLBAR_ALWAYS=1 时初始就显示，
    // 用来验证「显示本身是否正常」（与交互唤出的逻辑分开测）。
    if (::GetEnvironmentVariableW(L"MINISTREAM_TOOLBAR_ALWAYS", nullptr, 0) > 0) {
        show(true);
        MS_INFO("工具栏调试：初始强制显示");
        // 再顺带展开画质面板，方便一眼确认面板布局
        if (::GetEnvironmentVariableW(L"MINISTREAM_TOOLBAR_EXPAND", nullptr, 0) > 0) {
            set_expanded(true);
        }
    }

    suppressComboEvents_ = false;
    lastActivityUs_ = 0;
    refresh_status_text();
    return true;
}

void SessionToolbar::shutdown() {
    // 销毁面板与工具栏（控件作为各自子窗口随父销毁），释放字体。可重复调用。
    if (dock_)       { ::DestroyWindow(dock_);       dock_ = nullptr; }
    if (pnlQuality_) { ::DestroyWindow(pnlQuality_); pnlQuality_ = nullptr; }
    if (wnd_)        { ::DestroyWindow(wnd_);        wnd_ = nullptr; }
    if (brushToolbar_) { ::DeleteObject(brushToolbar_); brushToolbar_ = nullptr; }
    if (brushPanel_)   { ::DeleteObject(brushPanel_);   brushPanel_ = nullptr; }
    if (fontMono_)   { ::DeleteObject(fontMono_);    fontMono_ = nullptr; }
    if (font_)       { ::DeleteObject(font_);        font_ = nullptr; }
    if (s_this_ == this) s_this_ = nullptr;
    // 【注意】这里要真的把成员置空：原来写的是 `for (HWND h : nulls) h = nullptr;`
    // ——那只是把循环变量的副本置空，成员仍然是野句柄，二次 shutdown 会去
    // DestroyWindow 已销毁的窗口（跨会话时表现为工具栏怪怪的/不显示）。
    cbPreset_ = cbRes_ = cbFps_ = cbBitrate_ = cbKeyframe_ = edBitrate_ = nullptr;
    chkAudio_ = nullptr;
    chkOverlay_ = btnDisconnect_ = btnFullscreen_ = btnChevron_ = btnScale_ = nullptr;
    stStatus_ = btnApply_ = nullptr;
    stLblPreset_ = stLblRes_ = stLblFps_ = stLblKf_ = stLblBitrate_ = nullptr;
    cbSharpen_ = nullptr;
    btnPanelClose_ = nullptr;
    stLblSharpen_ = nullptr;
    shown_ = false;
    expanded_ = false;
    dockHovered_ = false;
    sepX_ = 516;
}

// 窗口过程里找回 this：优先用 GWLP_USERDATA（避免静态指针在"上一会话的窗口
// 还没销毁 / 新实例已创建"时指错对象——这类问题会让工具栏第一次能用、
// 后面就完全没反应）。
SessionToolbar* SessionToolbar::instance(HWND wnd) {
    if (wnd) {
        auto* p = reinterpret_cast<SessionToolbar*>(::GetWindowLongPtrW(wnd, GWLP_USERDATA));
        if (p) return p;
    }
    return s_this_;
}

// =============================================================================
//  布局
// =============================================================================
void SessionToolbar::place_windows() {
    if (!wnd_ || !parent_ || !::IsWindow(parent_)) return;

    // 以父窗口**客户区**左上角为基准（窗口化模式不会盖住标题栏；
    // 无边框全屏时就是屏幕左上角）。
    POINT tl{0, 0};
    if (!::ClientToScreen(parent_, &tl)) return;
    RECT cr{};
    ::GetClientRect(parent_, &cr);
    const int cw = int(cr.right) > 0 ? int(cr.right) : 1;

    ::SetWindowPos(wnd_, HWND_TOPMOST, tl.x, tl.y, cw, kToolbarH,
                   SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    ::SetWindowPos(pnlQuality_, HWND_TOPMOST, tl.x, tl.y + kToolbarH, kPanelW, kPanelH,
                   SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    if (dock_) {
        // 顶部居中：不挡状态条（右上）也不挡画面中心
        const int dx = tl.x + (cw - kDockW) / 2;
        ::SetWindowPos(dock_, HWND_TOPMOST, dx, tl.y + kDockTopMargin, kDockW, kDockH,
                       SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    }
}

void SessionToolbar::rebuild_layout() {
    if (!wnd_ || !pnlQuality_) return;

    place_windows();
    RECT rc{};
    ::GetClientRect(parent_, &rc);
    const int parentW = int(rc.right);

    // 控件横向排布：状态字 | 分隔线 | 画质▾ | 全屏 | 缩放 | 声音 | 悬浮条 | …… | 断开
    int x = kEdge;
    const int y = (kToolbarH - kCtrlH) / 2;

    // 右侧按钮区固定宽度，状态字吃掉剩下的（屏幕宽时不再被省略号截断）
    const int btnW = 92, btnGap = 8, btnAudioW = 80;
    const int rightW = btnW + btnGap + btnW + btnGap + 112 + btnGap + btnAudioW + btnGap +
                       btnW + btnGap + 104 + kEdge * 2 + 26;
    int statusW = parentW - rightW - kEdge * 2;
    if (statusW < 240) statusW = 240;
    ::SetWindowPos(stStatus_, nullptr, x, y, statusW, kCtrlH, SWP_NOZORDER | SWP_NOACTIVATE);
    x += statusW + kEdge;
    sepX_ = x;
    x += 1 + kEdge;

    ::SetWindowPos(btnChevron_, nullptr, x, y, btnW, kCtrlH, SWP_NOZORDER | SWP_NOACTIVATE);
    x += btnW + btnGap;
    ::SetWindowPos(btnFullscreen_, nullptr, x, y, btnW, kCtrlH, SWP_NOZORDER | SWP_NOACTIVATE);
    x += btnW + btnGap;
    ::SetWindowPos(btnScale_, nullptr, x, y, 112, kCtrlH, SWP_NOZORDER | SWP_NOACTIVATE);
    x += 112 + btnGap;
    ::SetWindowPos(chkAudio_, nullptr, x, y, btnAudioW, kCtrlH, SWP_NOZORDER | SWP_NOACTIVATE);
    x += btnAudioW + btnGap;
    ::SetWindowPos(chkOverlay_, nullptr, x, y, btnW, kCtrlH, SWP_NOZORDER | SWP_NOACTIVATE);
    // 断开按钮放最右
    const int btnDiscW = 104;
    ::SetWindowPos(btnDisconnect_, nullptr,
                   parentW - btnDiscW - kEdge, y, btnDiscW, kCtrlH,
                   SWP_NOZORDER | SWP_NOACTIVATE);

    // 面板位置已由 place_windows() 统一摆好（贴父窗口客户区左上、工具栏下方）

    // ---- 画质面板：左列标签 + 右列控件（原来没标签，根本看不出哪行是什么）----
    const int px = 16;          // 外边距
    const int lblW = 64;        // 标签列宽
    const int cx = px + lblW + 8;
    const int cbH = kCtrlH * 8; // 下拉展开高度
    const int rowY[6] = { 12, 50, 88, 126, 164, 202 };

    ::SetWindowPos(stLblPreset_, nullptr, px, rowY[0] + 4, lblW, 20, SWP_NOZORDER | SWP_NOACTIVATE);
    ::SetWindowPos(cbPreset_, nullptr, cx, rowY[0], 260, cbH, SWP_NOZORDER | SWP_NOACTIVATE);

    ::SetWindowPos(stLblRes_, nullptr, px, rowY[1] + 4, lblW, 20, SWP_NOZORDER | SWP_NOACTIVATE);
    ::SetWindowPos(cbRes_, nullptr, cx, rowY[1], 260, cbH, SWP_NOZORDER | SWP_NOACTIVATE);

    ::SetWindowPos(stLblFps_, nullptr, px, rowY[2] + 4, lblW, 20, SWP_NOZORDER | SWP_NOACTIVATE);
    ::SetWindowPos(cbFps_, nullptr, cx, rowY[2], 100, cbH, SWP_NOZORDER | SWP_NOACTIVATE);
    ::SetWindowPos(stLblKf_, nullptr, cx + 118, rowY[2] + 4, 52, 20, SWP_NOZORDER | SWP_NOACTIVATE);
    ::SetWindowPos(cbKeyframe_, nullptr, cx + 172, rowY[2], 100, cbH, SWP_NOZORDER | SWP_NOACTIVATE);

    ::SetWindowPos(stLblBitrate_, nullptr, px, rowY[3] + 4, lblW, 20, SWP_NOZORDER | SWP_NOACTIVATE);
    ::SetWindowPos(cbBitrate_, nullptr, cx, rowY[3], 160, cbH, SWP_NOZORDER | SWP_NOACTIVATE);
    ::SetWindowPos(edBitrate_, nullptr, cx + 168, rowY[3], 92, kCtrlH, SWP_NOZORDER | SWP_NOACTIVATE);

    // 锐化：只有 4 档（关/低/中/高），放一行的左侧
    ::SetWindowPos(stLblSharpen_, nullptr, px, rowY[4] + 4, lblW, 20, SWP_NOZORDER | SWP_NOACTIVATE);
    ::SetWindowPos(cbSharpen_, nullptr, cx, rowY[4], 100, cbH, SWP_NOZORDER | SWP_NOACTIVATE);
    // 编码格式（H.264/H.265）放同一行右侧，不额外占面板高度
    ::SetWindowPos(stLblCodec_, nullptr, cx + 118, rowY[4] + 4, 40, 20, SWP_NOZORDER | SWP_NOACTIVATE);
    ::SetWindowPos(cbCodec_, nullptr, cx + 160, rowY[4], 200, cbH, SWP_NOZORDER | SWP_NOACTIVATE);

    ::SetWindowPos(btnApply_, nullptr, cx, rowY[5], 96, 30, SWP_NOZORDER | SWP_NOACTIVATE);
    // 面板右上角「收起」
    ::SetWindowPos(btnPanelClose_, nullptr, kPanelW - 68, 10, 52, 22,
                   SWP_NOZORDER | SWP_NOACTIVATE);
}

// =============================================================================
//  显隐与自动隐藏
// =============================================================================
void SessionToolbar::show(bool on) {
    if (!wnd_) return;

    if (!on) {
        if (!shown_ && !expanded_) return;
        if (expanded_) set_expanded(false);   // 收工具栏时一并收面板
        ::ShowWindow(wnd_, SW_HIDE);
        shown_ = false;
        // 工具栏收起 -> 小悬浮按钮回来
        if (dock_) {
            ::SetLayeredWindowAttributes(dock_, 0, kDockAlpha, LWA_ALPHA);
            ::ShowWindow(dock_, SW_SHOWNA);
        }
        MS_INFO("工具栏隐藏（小悬浮按钮已恢复）");
        notify_cursor_overlay(false);
        return;
    }

    // 【修复】以前这里写 `if (on == shown_) return;`，一旦窗口被别的置顶窗口
    // 压住、或被系统隐藏而 shown_ 仍为 true，就再也弹不出来（用户看到的就是
    // "第一次能用，后面工具栏完全不出来"）。现在每次唤出都无条件重摆 + 重绘。
    lastActivityUs_ = now_us();
    shown_ = true;
    ::ShowWindow(wnd_, SW_SHOWNA);
    rebuild_layout();   // 内部 place_windows() 会 SetWindowPos 到父窗口客户区顶边
    {
        // 【诊断】工具栏是否真的在屏幕可见位置（全屏/多显示器/DPI 下最容易出错）
        RECT tr{};
        ::GetWindowRect(wnd_, &tr);
        MS_DEBUG("工具栏状态: visible=%d rect=(%d,%d)-(%d,%d) 屏幕=%dx%d",
                 ::IsWindowVisible(wnd_) ? 1 : 0,
                 int(tr.left), int(tr.top), int(tr.right), int(tr.bottom),
                 ::GetSystemMetrics(SM_CXSCREEN), ::GetSystemMetrics(SM_CYSCREEN));
    }
    ::RedrawWindow(wnd_, nullptr, nullptr,
                   RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW | RDW_ERASE);
    if (dock_) ::ShowWindow(dock_, SW_HIDE);
    // 显示期间本机光标必须可见（捕获态会隐藏光标），否则用户没法点按钮
    notify_cursor_overlay(true);
    MS_INFO("工具栏显示（展开状态=%d）", expanded_ ? 1 : 0);
}

void SessionToolbar::set_expanded(bool on) {
    if (!pnlQuality_ || on == expanded_) return;
    ::ShowWindow(pnlQuality_, on ? SW_SHOWNA : SW_HIDE);
    if (on) {
        place_windows();
        ::RedrawWindow(pnlQuality_, nullptr, nullptr,
                       RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW | RDW_ERASE);
    }
    expanded_ = on;
    lastActivityUs_ = now_us();
    MS_INFO("画质面板 %s", on ? "展开" : "收起");
    // 【诊断】面板"展开了但用户看不见"的排查：把真实窗口状态打出来。
    // 只要出现 rect 明显不对 / visible=0 / topmost=0 / z 序不在父窗口之上，就能一眼定位。
    if (on) {
        RECT pr{};
        ::GetWindowRect(pnlQuality_, &pr);
        const LONG_PTR ex = ::GetWindowLongPtrW(pnlQuality_, GWL_EXSTYLE);
        RECT wr{};
        ::GetWindowRect(wnd_, &wr);
        MS_DEBUG("面板状态: visible=%d rect=(%d,%d)-(%d,%d) topmost=%d toolwin=%d "
                 "toolbar=(%d,%d)-(%d,%d) toolbar_visible=%d expanded_=%d",
                 ::IsWindowVisible(pnlQuality_) ? 1 : 0,
                 int(pr.left), int(pr.top), int(pr.right), int(pr.bottom),
                 (ex & WS_EX_TOPMOST) ? 1 : 0, (ex & WS_EX_TOOLWINDOW) ? 1 : 0,
                 int(wr.left), int(wr.top), int(wr.right), int(wr.bottom),
                 ::IsWindowVisible(wnd_) ? 1 : 0, expanded_ ? 1 : 0);
    }
}

void SessionToolbar::collapse_panel() {
    if (expanded_) set_expanded(false);
}

// 按钮防抖：同一次"双击"里的第二下不再触发动作。
// 典型场景：用户按 Windows 习惯双击「画质 ▾」→ 第一下开面板、第二下又关掉，
// 视觉上就是"面板闪一下就没了 / 按钮点不开"。窗口取系统双击时间（上限 800ms）。
bool SessionToolbar::click_debounced(int slot) {
    if (slot < 0 || slot >= int(sizeof(lastClickUs_) / sizeof(lastClickUs_[0]))) return false;
    const i64 now = now_us();
    int dblMs = int(::GetDoubleClickTime());
    if (dblMs < 250) dblMs = 250;
    if (dblMs > 800) dblMs = 800;
    const i64 windowUs = i64(dblMs) * 1000;
    const i64 prev = lastClickUs_[slot];
    lastClickUs_[slot] = now;
    if (prev != 0 && now - prev < windowUs) {
        MS_DEBUG("[工具栏按钮] 忽略连击（%lld us 内第二次，防双击误触）",
                 (long long)(now - prev));
        return true;
    }
    return false;
}

// 光标是否停在本机界面上（工具栏 / 画质面板 / 顶部小悬浮按钮）。
// 用途：此时不能再把鼠标事件转发给远端，否则点“全屏/画质”会顺手在游戏里点一下。
bool SessionToolbar::cursor_over_ui() const {
    POINT pt{};
    if (!::GetCursorPos(&pt)) return false;
    RECT r{};
    if (wnd_ && ::IsWindowVisible(wnd_) && ::GetWindowRect(wnd_, &r) && ::PtInRect(&r, pt))
        return true;
    if (expanded_ && pnlQuality_ && ::IsWindowVisible(pnlQuality_) &&
        ::GetWindowRect(pnlQuality_, &r) && ::PtInRect(&r, pt))
        return true;
    if (dock_ && ::IsWindowVisible(dock_) && ::GetWindowRect(dock_, &r) && ::PtInRect(&r, pt))
        return true;
    return false;
}

void SessionToolbar::set_audio_enabled(bool on) {
    if (chkAudio_) {
        ::SendMessageW(chkAudio_, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
    }
}

void SessionToolbar::set_sharpen_level(int level) {
    if (level < 0) level = 0;
    if (level >= kSharpenCount) level = kSharpenCount - 1;
    sharpenLevel_ = level;
    if (cbSharpen_) {
        const bool old = suppressComboEvents_;
        suppressComboEvents_ = true;
        ::SendMessageW(cbSharpen_, CB_SETCURSEL, WPARAM(level), 0);
        suppressComboEvents_ = old;
    }
}

void SessionToolbar::set_codec_value(int codec) {
    codecValue_ = (codec == 2) ? 2 : 1;
    if (cbCodec_) {
        const bool old = suppressComboEvents_;
        suppressComboEvents_ = true;
        ::SendMessageW(cbCodec_, CB_SETCURSEL, WPARAM(codecValue_ == 2 ? 1 : 0), 0);
        suppressComboEvents_ = old;
    }
}

void SessionToolbar::on_mouse_move(POINT screenPos) {
    // 光标进入父窗口顶边 2px 内 -> 唤出工具栏
    if (!wnd_) return;
    RECT rc{};
    ::GetWindowRect(parent_, &rc);
    if (screenPos.y >= rc.top && screenPos.y <= rc.top + kTopEdgePx &&
        screenPos.x >= rc.left && screenPos.x <= rc.right) {
        if (shown_) {
            lastActivityUs_ = now_us();   // 已经显示：只续命，别每 2ms 重摆窗口
        } else {
            show(true);
        }
    }
}

void SessionToolbar::summon() {
    // 主动唤出（例如按了释放键鼠快捷键）：不依赖光标位置判定，直接显示。
    // 光标这时通常不在顶边，所以同时刷新活动时间，保证 3 秒内不会自动收回。
    lastActivityUs_ = now_us();
    MS_INFO("工具栏被唤出（summon）");
    show(true);
}

void SessionToolbar::poll_auto_hide() {
    if (!shown_ && !expanded_) return;   // 都隐藏时无事可做（每 2ms 调一次，别白跑）
    const i64 now = now_us();

    // 光标停在工具栏/画质面板/小悬浮按钮上时不算"空闲"。
    // 按钮控件自己会吃掉 WM_MOUSEMOVE（不转发给父窗口），只靠消息刷新活动时间
    // 会出现"鼠标明明停在按钮上，3 秒后工具栏自己消失"。
    POINT pt{};
    if (::GetCursorPos(&pt)) {
        RECT r{};
        bool over = false;
        if (wnd_ && ::GetWindowRect(wnd_, &r) && ::PtInRect(&r, pt)) over = true;
        if (!over && expanded_ && pnlQuality_ && ::GetWindowRect(pnlQuality_, &r) &&
            ::PtInRect(&r, pt)) over = true;
        if (!over && dock_ && ::IsWindowVisible(dock_) && ::GetWindowRect(dock_, &r) &&
            ::PtInRect(&r, pt)) over = true;
        if (over) lastActivityUs_ = now;
    }

    // 画质面板开着但 10 秒没人动：自动收起，别一直挡着画面左边
    if (expanded_ && lastActivityUs_ != 0 && now - lastActivityUs_ > kPanelAutoHideUs) {
        set_expanded(false);
    }
    if (!shown_ || expanded_) return;   // 展开画质面板时不自动隐藏（用户正在改参数）
    // 调试：MINISTREAM_TOOLBAR_PIN=1 时永不自动隐藏（用于截图/人工确认外观）
    if (::GetEnvironmentVariableW(L"MINISTREAM_TOOLBAR_PIN", nullptr, 0) > 0) return;
    if (lastActivityUs_ == 0) { lastActivityUs_ = now; return; }
    if (now - lastActivityUs_ > kAutoHideUs) show(false);
}

// =============================================================================
//  会话状态同步
// =============================================================================
void SessionToolbar::set_stream_params(int nativeW, int nativeH,
                                        int curW, int curH, int curFps, int curKbps) {
    {
        std::lock_guard<std::mutex> lk(syncMtx_);
        nativeW_ = nativeW; nativeH_ = nativeH;
        pendingW_ = curW;   pendingH_ = curH;
        pendingFps_ = curFps; pendingKbps_ = curKbps;
    }
    post_sync();
}

void SessionToolbar::set_connection_status(const std::string& text) {
    {
        std::lock_guard<std::mutex> lk(syncMtx_);
        pendingConn_ = widen(text);
        pendingConnDirty_ = true;
    }
    post_sync();
}

// 把刷新请求投递回 UI 线程（从 UI 线程自己调用也安全：只是排队到下一次消息泵）
void SessionToolbar::post_sync() {
    if (!wnd_) return;
    ::PostMessageW(wnd_, WM_TB_SYNC, 0, 0);
}

// 只在 UI 线程执行：把缓存参数落到成员上并刷新控件
void SessionToolbar::apply_sync() {
    if (!wnd_ || !stStatus_) return;
    {
        std::lock_guard<std::mutex> lk(syncMtx_);
        curW_ = pendingW_;
        curH_ = pendingH_;
        curFps_ = pendingFps_;
        curKbps_ = pendingKbps_;
        if (pendingConnDirty_) {
            connStatus_ = pendingConn_;
            pendingConnDirty_ = false;
        }
    }
    refresh_status_text();
}

void SessionToolbar::refresh_status_text() {
    if (!stStatus_) return;
    wchar_t buf[256]{};
    // 双线状态：备线已接入时显示「双线·键鼠走备线」，否则「单线」
    const wchar_t* lineTag = connStatus_.find(L"双线") != std::wstring::npos
                                 ? L"双线" : L"单线";
    _snwprintf_s(buf, _TRUNCATE, L"%d x %d · %d fps · %s · %s · %s",
                 curW_, curH_, curFps_, bitrate_label(curKbps_).c_str(),
                 connStatus_.empty() ? L"已连接" : connStatus_.c_str(),
                 lineTag);
    statusText_ = buf;
    ::SetWindowTextW(stStatus_, buf);
    ::InvalidateRect(wnd_, nullptr, TRUE);
}

// 双线状态同步：备线接入/断开时更新显示（「双线·键鼠走备线」/「单线」）
void SessionToolbar::set_backup_line_status(bool active, const wchar_t* rttText) {
    connStatus_ = active ? L"双线·键鼠走备线" : L"单线";
    if (active && rttText && rttText[0]) {
        connStatus_ += L" ";
        connStatus_ += rttText;
    }
    refresh_status_text();
}

// =============================================================================
//  画质面板联动
// =============================================================================
void SessionToolbar::apply_preset_selection() {
    // 用户改预设 -> 三个下拉跳到 default_profile 的值（此函数只在非 Custom 时调）
    const int sel = int(::SendMessageW(cbPreset_, CB_GETCURSEL, 0, 0));
    if (sel < 0 || sel >= int(quality::Preset::Custom)) return;
    const quality::QualityProfile q =
        quality::default_profile(quality::Preset(sel));

    suppressComboEvents_ = true;

    // 分辨率：default_profile 全是 0x0（跟随被控端）-> 选「跟随被控端」项
    ::SendMessageW(cbRes_, CB_SETCURSEL, 0, 0);

    // 帧率
    const auto& fpsList = quality::fps_options();
    for (size_t i = 0; i < fpsList.size(); ++i) {
        if (fpsList[i] == q.fps) { ::SendMessageW(cbFps_, CB_SETCURSEL, WPARAM(i), 0); break; }
    }

    // 码率：0=自适应在列表首位
    const auto& brList = quality::bitrate_options();
    for (size_t i = 0; i < brList.size(); ++i) {
        if (brList[i] == q.bitrateKbps) { ::SendMessageW(cbBitrate_, CB_SETCURSEL, WPARAM(i), 0); break; }
    }
    // 自定义码率输入框同步清成空（跟随预设）
    ::SetWindowTextW(edBitrate_, L"");

    // 关键帧间隔
    for (int i = 0; i < kKfCount; ++i) {
        if (kKfOptions[i].sec == q.keyframeIntervalSec) {
            ::SendMessageW(cbKeyframe_, CB_SETCURSEL, WPARAM(i), 0);
            break;
        }
    }

    suppressComboEvents_ = false;
}

void SessionToolbar::mark_manual_edit() {
    // 手动改任何下拉/输入 -> 预设显示「自定义」
    if (suppressComboEvents_) return;
    if (int(::SendMessageW(cbPreset_, CB_GETCURSEL, 0, 0)) != comboPresetCustomIdx_) {
        suppressComboEvents_ = true;
        ::SendMessageW(cbPreset_, CB_SETCURSEL, WPARAM(comboPresetCustomIdx_), 0);
        suppressComboEvents_ = false;
    }
}

quality::QualityProfile SessionToolbar::collect_profile(bool* needReconnect) const {
    quality::QualityProfile q;

    // 预设
    const int presetSel = int(::SendMessageW(cbPreset_, CB_GETCURSEL, 0, 0));
    q.preset = (presetSel >= 0 && presetSel < quality::kPresetCount)
                   ? quality::Preset(presetSel)
                   : quality::Preset::Custom;

    // 分辨率（下拉 itemdata：ResOption 里没有存 HWND 侧索引的通道，用 CurSel 对表）
    const auto& resList = quality::resolution_options();
    const int resSel = int(::SendMessageW(cbRes_, CB_GETCURSEL, 0, 0));
    if (resSel >= 0 && resSel < int(resList.size())) {
        q.width = resList[size_t(resSel)].w;
        q.height = resList[size_t(resSel)].h;
    }

    // 帧率
    const auto& fpsList = quality::fps_options();
    const int fpsSel = int(::SendMessageW(cbFps_, CB_GETCURSEL, 0, 0));
    if (fpsSel >= 0 && fpsSel < int(fpsList.size())) q.fps = fpsList[size_t(fpsSel)];
    else q.fps = curFps_;

    // 码率：优先自定义输入框，空则用下拉
    int kbps = -1;
    const std::wstring edTxt = get_edit_text(edBitrate_);
    if (!edTxt.empty()) {
        const int v = parse_int(edTxt, -1);
        if (v >= 0 && quality::valid_bitrate(v)) kbps = v;
    }
    if (kbps < 0) {
        const auto& brList = quality::bitrate_options();
        const int brSel = int(::SendMessageW(cbBitrate_, CB_GETCURSEL, 0, 0));
        if (brSel >= 0 && brSel < int(brList.size())) kbps = brList[size_t(brSel)];
    }
    if (kbps < 0) kbps = curKbps_;
    q.bitrateKbps = kbps;

    // 关键帧间隔
    const int kfSel = int(::SendMessageW(cbKeyframe_, CB_GETCURSEL, 0, 0));
    if (kfSel >= 0 && kfSel < kKfCount) q.keyframeIntervalSec = kKfOptions[kfSel].sec;

    // 编码格式（H.264 / H.265）
    q.codec = codecValue_;

    // needReconnect：分辨率或帧率与当前会话值不同（分辨率/fps 改变要重建
    // 采集与编码器）；只码率变化走热调不用重连。
    // 「跟随被控端」(0x0) 比较时落到 native 分辨率，否则选"跟随"永远判重连。
    if (needReconnect) {
        const int effW = q.width  > 0 ? q.width  : nativeW_;
        const int effH = q.height > 0 ? q.height : nativeH_;
        const bool resChanged = (effW != curW_ || effH != curH_);
        const bool fpsChanged = (q.fps != curFps_);
        *needReconnect = resChanged || fpsChanged;
    }
    return q;
}

void SessionToolbar::notify_cursor_overlay(bool on) {
    if (!parent_) return;
    // wParam=1 显示 / 0 隐藏交还；父窗口的 InputCapture 捕获时会隐藏光标，
    // 工具栏可见期间由父窗口暂时 ShowCursor(TRUE)。
    ::PostMessageW(parent_, WM_TB_CURSOR_OVERLAY, on ? 1 : 0, 0);
}

// =============================================================================
//  窗口过程（工具栏容器）
// =============================================================================
LRESULT CALLBACK SessionToolbar::toolbar_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    SessionToolbar* self = instance(wnd);
    switch (msg) {
        case WM_NCCREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            ::SetWindowLongPtrW(wnd, GWLP_USERDATA,
                                reinterpret_cast<LONG_PTR>(cs ? cs->lpCreateParams : nullptr));
            break;
        }
        case WM_TB_SYNC:
            // 来自任意线程的"状态变了"通知：真正碰控件的一定是 UI 线程
            if (self) self->apply_sync();
            return 0;
        case WM_MOUSEACTIVATE:
            // 命门：点击工具栏绝不激活它（父窗口也不因此失焦）
            return MA_NOACTIVATE;
        case WM_LBUTTONDOWN: {
            // 【诊断】点没点到工具栏：低于 3 秒的连续两次点击会把面板开了又关，
            // 用户看起来就是"按钮点不开"，这里把每次点击的坐标记下来。
            MS_DEBUG("[工具栏点击] x=%d y=%d", GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            break;
        }
        case WM_MOUSEMOVE:
        case WM_NCMOUSEMOVE: {
            if (self) self->lastActivityUs_ = now_us();
            // TrackMouseEvent 拿 WM_MOUSELEAVE
            TRACKMOUSEEVENT tme{};
            tme.cbSize = sizeof(tme);
            tme.hwndTrack = wnd;
            tme.dwFlags = TME_LEAVE;
            ::TrackMouseEvent(&tme);
            // 把 hover 传给面板区域判定（面板在工具栏下方，需要保持可见）
            break;
        }
        case WM_MOUSELEAVE:
            if (self) self->lastActivityUs_ = now_us();
            break;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = ::BeginPaint(wnd, &ps);
            if (dc) {
                RECT rc{};
                ::GetClientRect(wnd, &rc);
                // 背景由类刷上色，这里画一条 1px 分隔竖线（状态字与按钮之间）
                const HGDIOBJ old = ::SelectObject(dc, ::GetStockObject(DC_PEN));
                ::SetDCPenColor(dc, kSepLine);
                const int sepX = self ? self->sepX_ : 516;   // 与布局保持一致
                ::MoveToEx(dc, sepX, 8, nullptr);
                ::LineTo(dc, sepX, rc.bottom - 8);
                ::SelectObject(dc, old);
            }
            ::EndPaint(wnd, &ps);
            return 0;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN: {
            // STATIC 透明底浅字（背景刷 init 时建好，长生命周期不泄漏）
            HDC dc = HDC(wp);
            ::SetBkMode(dc, TRANSPARENT);
            ::SetTextColor(dc, kTextFg);
            return LRESULT(self ? self->brushToolbar_ : nullptr);
        }
        case WM_DRAWITEM: {
            const DRAWITEMSTRUCT* dis = reinterpret_cast<const DRAWITEMSTRUCT*>(lp);
            if (dis && dis->CtlType == ODT_BUTTON && self) {
                draw_owner_button(dis, self->font_, dis->hwndItem == self->btnDisconnect_);
                return TRUE;
            }
            break;
        }
        case WM_COMMAND: {
            if (!self) break;
            const int code = HIWORD(wp);
            const int id   = LOWORD(wp);
            (void)id;
            HWND ctrl = HWND(lp);
            if (code == BN_CLICKED) {
                self->lastActivityUs_ = now_us();
                // 【连击保护】双击时第二下直接吞掉：以前"画质"是纯 toggle，
                // 用户双击就会开了立刻关，看起来像"面板闪一下就消失/按钮点不开"。
                int slot = -1;
                if (ctrl == self->btnChevron_)            slot = 0;
                else if (ctrl == self->btnFullscreen_)    slot = 1;
                else if (ctrl == self->btnScale_)         slot = 2;
                else if (ctrl == self->chkOverlay_)       slot = 3;
                else if (ctrl == self->btnDisconnect_)    slot = 4;
                else if (ctrl == self->chkAudio_)         slot = 5;
                if (self->click_debounced(slot)) break;
                MS_DEBUG("[工具栏按钮] %s（点击后 expanded_=%d）",
                         ctrl == self->btnChevron_ ? "画质" :
                         (ctrl == self->btnFullscreen_ ? "全屏" :
                          (ctrl == self->btnScale_ ? "缩放" :
                           (ctrl == self->chkOverlay_ ? "悬浮条" :
                            (ctrl == self->btnDisconnect_ ? "断开" : "未知")))),
                         self->expanded_ ? 1 : 0);
                // 命令转发：子窗口与父窗口共用一个线程（同一线程创建、同一
                // 消息队列），这里直接调用 cmds_.on_xxx 没有任何线程问题。
                if (ctrl == self->btnChevron_) {
                    // 【只开不关】画质按钮只负责展开；收起用面板右上角「收起」，
                    // 或者直接点一下画面。这样无论单击/双击都不会出现"闪一下"。
                    if (!self->expanded_) {
                        self->set_expanded(true);
                    } else {
                        // 已经展开时再点：把面板提到最前 + 续命（不是关掉）
                        self->place_windows();
                        self->lastActivityUs_ = now_us();
                        MS_DEBUG("[工具栏按钮] 画质面板已展开，保持显示");
                    }
                } else if (ctrl == self->btnFullscreen_) {
                    self->fullscreenState_ = !self->fullscreenState_;
                    ::SetWindowTextW(self->btnFullscreen_,
                                     self->fullscreenState_ ? L"窗口" : L"全屏");
                    if (self->cmds_.on_toggle_fullscreen)
                        self->cmds_.on_toggle_fullscreen(self->fullscreenState_);
                } else if (ctrl == self->btnScale_) {
                    // 显示缩放模式循环：智能缩放 -> 拉伸填充 -> 原始大小(1:1)
                    self->scaleMode_ = (self->scaleMode_ + 1) % 3;
                    static const wchar_t* kScaleNames[3] = {
                        L"缩放:智能", L"缩放:拉伸", L"缩放:1:1"
                    };
                    ::SetWindowTextW(self->btnScale_, kScaleNames[self->scaleMode_]);
                    if (self->cmds_.on_scale_mode)
                        self->cmds_.on_scale_mode(self->scaleMode_);
                } else if (ctrl == self->chkOverlay_) {
                    const bool on = ::SendMessageW(self->chkOverlay_, BM_GETCHECK, 0, 0) == BST_CHECKED;
                    if (self->cmds_.on_toggle_overlay)
                        self->cmds_.on_toggle_overlay(on);
                } else if (ctrl == self->chkAudio_) {
                    // 声音开关：只停/开本机播放（被控端照常发流，切回来立刻有声）
                    const bool on = ::SendMessageW(self->chkAudio_, BM_GETCHECK, 0, 0) == BST_CHECKED;
                    if (self->cmds_.on_toggle_audio)
                        self->cmds_.on_toggle_audio(on);
                } else if (ctrl == self->btnDisconnect_) {
                    if (self->cmds_.on_disconnect) self->cmds_.on_disconnect();
                }
                // 注：btnApply_ 是画质面板的子控件，其命令走 panel_proc。
                // 任何按钮点击后把焦点还给父窗口（渲染窗口）
                if (self->wnd_) ::SetFocus(::GetParent(self->wnd_));
            }
            break;
        }
        case WM_DESTROY:
            return 0;
        default:
            break;
    }
    return ::DefWindowProcW(wnd, msg, wp, lp);
}

// =============================================================================
//  顶部小悬浮按钮（≡）
//  平时半透明常驻，点一下展开工具栏；工具栏显示期间它自动隐藏，收起后再回来。
//  这样不需要"把光标精准推到屏幕最顶边"也能打开菜单（原来那是唯一入口，
//  在游戏里很难命中，也是"工具栏显示不出来"体感的一部分）。
// =============================================================================
void SessionToolbar::paint_dock() {
    if (!dock_) return;
    PAINTSTRUCT ps{};
    HDC dc = ::BeginPaint(dock_, &ps);
    if (!dc) return;
    RECT rc{};
    ::GetClientRect(dock_, &rc);
    const int w = int(rc.right - rc.left), h = int(rc.bottom - rc.top);
    if (w <= 2 || h <= 2) { ::EndPaint(dock_, &ps); return; }

    const COLORREF bgCol  = dockHovered_ ? RGB(52, 96, 172) : RGB(26, 28, 33);
    const COLORREF bdCol  = dockHovered_ ? RGB(128, 178, 255) : RGB(66, 70, 80);
    const COLORREF fgCol  = dockHovered_ ? RGB(255, 255, 255) : RGB(210, 216, 226);

    HBRUSH bg = ::CreateSolidBrush(bgCol);
    HPEN   bd = ::CreatePen(PS_SOLID, 1, bdCol);
    HGDIOBJ oldB = ::SelectObject(dc, bg);
    HGDIOBJ oldP = ::SelectObject(dc, bd);
    ::RoundRect(dc, 0, 0, w, h, h, h);
    ::SelectObject(dc, oldP);
    ::SelectObject(dc, oldB);
    ::DeleteObject(bd);
    ::DeleteObject(bg);

    // 「≡」三条横线
    HPEN lp = ::CreatePen(PS_SOLID, 2, fgCol);
    HGDIOBJ oldL = ::SelectObject(dc, lp);
    const int lx = w / 2 - 8, rx = w / 2 + 8;
    const int ys[3] = { h / 2 - 5, h / 2, h / 2 + 5 };
    for (int i = 0; i < 3; ++i) {
        ::MoveToEx(dc, lx, ys[i], nullptr);
        ::LineTo(dc, rx, ys[i]);
    }
    ::SelectObject(dc, oldL);
    ::DeleteObject(lp);
    ::EndPaint(dock_, &ps);
}

LRESULT CALLBACK SessionToolbar::dock_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    SessionToolbar* self = instance(wnd);
    switch (msg) {
        case WM_NCCREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            ::SetWindowLongPtrW(wnd, GWLP_USERDATA,
                                reinterpret_cast<LONG_PTR>(cs ? cs->lpCreateParams : nullptr));
            break;
        }
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;   // 点击不抢焦点，游戏捕获不被打断
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                ::SetCursor(::LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
            break;
        case WM_MOUSEMOVE: {
            if (self) {
                self->lastActivityUs_ = now_us();
                if (!self->dockHovered_) {
                    self->dockHovered_ = true;
                    ::SetLayeredWindowAttributes(wnd, 0, kDockAlphaHover, LWA_ALPHA);
                    ::InvalidateRect(wnd, nullptr, FALSE);
                }
                TRACKMOUSEEVENT tme{};
                tme.cbSize = sizeof(tme);
                tme.dwFlags = TME_LEAVE;
                tme.hwndTrack = wnd;
                ::TrackMouseEvent(&tme);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            if (self) {
                self->dockHovered_ = false;
                ::SetLayeredWindowAttributes(wnd, 0, kDockAlpha, LWA_ALPHA);
                ::InvalidateRect(wnd, nullptr, FALSE);
            }
            return 0;
        case WM_LBUTTONDOWN:
            // 点一下展开工具栏（工具栏收起后这个小按钮会再回来）
            MS_INFO("小悬浮按钮被点击 -> 展开工具栏");
            if (self) self->summon();
            return 0;
        case WM_PAINT:
            if (self) self->paint_dock();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_DESTROY:
            return 0;
        default:
            break;
    }
    return ::DefWindowProcW(wnd, msg, wp, lp);
}

// =============================================================================
//  窗口过程（画质面板）
// =============================================================================
LRESULT CALLBACK SessionToolbar::panel_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    SessionToolbar* self = instance(wnd);
    switch (msg) {
        case WM_NCCREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            ::SetWindowLongPtrW(wnd, GWLP_USERDATA,
                                reinterpret_cast<LONG_PTR>(cs ? cs->lpCreateParams : nullptr));
            break;
        }
        case WM_DRAWITEM: {
            const DRAWITEMSTRUCT* dis = reinterpret_cast<const DRAWITEMSTRUCT*>(lp);
            if (!dis) break;
            if (dis->CtlType == ODT_BUTTON && self) {
                draw_owner_button(dis, self->font_, false);
                return TRUE;
            }
            if (dis->CtlType == ODT_COMBOBOX && self) {
                // 自绘下拉：深色底 + 浅色字，和整体面板一致
                HDC dc = dis->hDC;
                RECT rc = dis->rcItem;
                const bool sel = (dis->itemState & ODS_SELECTED) != 0;
                HBRUSH br = ::CreateSolidBrush(sel ? RGB(52, 64, 86) : RGB(40, 43, 50));
                ::FillRect(dc, &rc, br);
                ::DeleteObject(br);
                wchar_t text[256]{};
                if (dis->itemID != UINT(-1)) {
                    ::SendMessageW(dis->hwndItem, CB_GETLBTEXT, WPARAM(dis->itemID),
                                   LPARAM(text));
                }
                ::SetBkMode(dc, TRANSPARENT);
                ::SetTextColor(dc, RGB(228, 232, 240));
                HGDIOBJ of = self->font_ ? ::SelectObject(dc, self->font_) : nullptr;
                RECT tr = rc;
                tr.left += 6;
                ::DrawTextW(dc, text, -1, &tr,
                            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                if (of) ::SelectObject(dc, of);
                return TRUE;
            }
            break;
        }
        case WM_MEASUREITEM: {
            auto* mis = reinterpret_cast<MEASUREITEMSTRUCT*>(lp);
            if (mis && mis->CtlType == ODT_COMBOBOX) {
                mis->itemHeight = 20;
                return TRUE;
            }
            break;
        }
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_LBUTTONDOWN:
            // 【诊断】面板里的点击（下拉/应用）——确认事件真的到了面板窗口
            MS_DEBUG("[面板点击] x=%d y=%d", GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            break;
        case WM_MOUSEMOVE:
        case WM_NCMOUSEMOVE: {
            if (self) self->lastActivityUs_ = now_us();
            TRACKMOUSEEVENT tme{};
            tme.cbSize = sizeof(tme);
            tme.hwndTrack = wnd;
            tme.dwFlags = TME_LEAVE;
            ::TrackMouseEvent(&tme);
            break;
        }
        case WM_MOUSELEAVE:
            if (self) self->lastActivityUs_ = now_us();
            break;
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORLISTBOX:
        case WM_CTLCOLOREDIT: {
            // 下拉弹出的列表框、码率输入框也统一深色处理（默认白底）
            HDC dc = HDC(wp);
            ::SetBkColor(dc, kPanelBg);
            ::SetBkMode(dc, OPAQUE);
            ::SetTextColor(dc, kTextFg);
            return LRESULT(self ? self->brushPanel_ : nullptr);
        }
        case WM_COMMAND: {
            if (!self) break;
            const int code = HIWORD(wp);
            HWND ctrl = HWND(lp);
            self->lastActivityUs_ = now_us();

            if (code == CBN_SELCHANGE && !self->suppressComboEvents_) {
                if (ctrl == self->cbPreset_) {
                    // 用户选了具体预设 -> 三个下拉跳到 default_profile 值；
                    // 选「自定义」不动作（等用户手动改其它下拉）
                    const int sel = int(::SendMessageW(self->cbPreset_, CB_GETCURSEL, 0, 0));
                    if (sel >= 0 && sel < int(quality::Preset::Custom)) {
                        self->apply_preset_selection();
                    }
                } else if (ctrl == self->cbRes_ || ctrl == self->cbFps_ ||
                           ctrl == self->cbBitrate_) {
                    // 手动改参数 -> 预设显示「自定义」
                    self->mark_manual_edit();
                } else if (ctrl == self->cbKeyframe_) {
                    // 关键帧间隔是运行期热调，改动即生效
                    self->mark_manual_edit();
                    const int sel = int(::SendMessageW(self->cbKeyframe_, CB_GETCURSEL, 0, 0));
                    if (sel >= 0 && sel < kKfCount) {
                        self->curKeyframeSec_ = kKfOptions[sel].sec;
                        if (self->cmds_.on_keyframe_interval_change)
                            self->cmds_.on_keyframe_interval_change(kKfOptions[sel].sec);
                    }
                } else if (ctrl == self->cbSharpen_) {
                    // 锐化：改一下立刻生效（着色器 uniform，不重连、不重编码）
                    const int sel = int(::SendMessageW(self->cbSharpen_, CB_GETCURSEL, 0, 0));
                    if (sel >= 0 && sel < kSharpenCount) {
                        self->sharpenLevel_ = sel;
                        if (self->cmds_.on_sharpen_change)
                            self->cmds_.on_sharpen_change(sel);
                    }
                } else if (ctrl == self->cbCodec_) {
                    // 编码格式：选了立刻生效（被控端重建编码器 -> 远控端重建解码器）
                    const int sel = int(::SendMessageW(self->cbCodec_, CB_GETCURSEL, 0, 0));
                    const auto& opts = quality::codec_options();
                    if (sel >= 0 && sel < int(opts.size())) {
                        self->codecValue_ = opts[size_t(sel)].value;
                        MS_INFO("画质面板 -> 编码格式 H.%s",
                                self->codecValue_ == 2 ? "265" : "264");
                        if (self->cmds_.on_codec_change)
                            self->cmds_.on_codec_change(self->codecValue_);
                    }
                }
            } else if (code == EN_CHANGE && !self->suppressComboEvents_) {
                if (ctrl == self->edBitrate_) self->mark_manual_edit();
            } else if (code == BN_CLICKED && ctrl == self->btnPanelClose_) {
                // 面板右上角「收起」：唯一"主动关面板"的按钮
                self->set_expanded(false);
                ::SetFocus(::GetParent(self->wnd_));
            } else if (code == BN_CLICKED && ctrl == self->btnApply_) {
                // 「应用」：收集全部参数 -> on_quality_change(profile, needReconnect)。
                // needReconnect=false（只码率变）时按任务约定额外走 on_bitrate_change
                // 热调通道，父窗口可以立刻 send_set_bitrate 不重连。
                if (self->click_debounced(6)) break;   // 「应用」也防双击
                bool needReconnect = false;
                const quality::QualityProfile q = self->collect_profile(&needReconnect);
                if (self->cmds_.on_quality_change)
                    self->cmds_.on_quality_change(q, needReconnect);
                if (!needReconnect && self->cmds_.on_bitrate_change &&
                    q.bitrateKbps != self->curKbps_) {
                    self->cmds_.on_bitrate_change(q.bitrateKbps);
                }
                if (needReconnect) {
                    self->curFps_ = q.fps;
                    if (q.width > 0 && q.height > 0) { self->curW_ = q.width; self->curH_ = q.height; }
                }
                self->curKbps_ = q.bitrateKbps;
                self->refresh_status_text();
                // 【不再自动关面板】以前"应用"后面板会自己消失，用户以为点坏了。
                // 现在面板留着（能继续调），想关就点右上角「收起」或点一下画面。
                self->lastActivityUs_ = now_us();
                ::SetFocus(::GetParent(self->wnd_));
            }
            break;
        }
        case WM_DESTROY:
            return 0;
        default:
            break;
    }
    return ::DefWindowProcW(wnd, msg, wp, lp);
}

} // namespace ms::client
