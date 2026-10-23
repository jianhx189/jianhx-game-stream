// =============================================================================
//  tray.cpp - 被控端极简 UI：托盘图标 + 一个配置窗口
//
//  结构：
//    * 一个隐藏的「消息窗口」常驻，负责接收托盘回调与跨线程消息；
//    * 配置窗口按需创建，关闭时只 ShowWindow(SW_HIDE)，永不销毁（省事且快）；
//    * 所有控件都是手工 CreateWindowExW，不用 .rc 资源脚本（避免 rc 编译依赖）；
//    * set_status/set_running 可能从服务工作线程调用，因此一律 PostMessageW
//      把数据丢回 UI 线程，绝不跨线程碰控件。
// =============================================================================
// windows.h 先包含：tray.h 直接用 HINSTANCE，且 Win32 头要求在 winsock 之前
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <strsafe.h>

#include "tray.h"
#include "capture.h"
#include "../common/log.h"

#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

// 老 SDK 里 NIF_SHOWTIP / NOTIFYICON_VERSION_4 可能没定义，这里兜底
#ifndef NIF_SHOWTIP
#define NIF_SHOWTIP 0x00000080
#endif
#ifndef NOTIFYICON_VERSION_4
#define NOTIFYICON_VERSION_4 4
#endif

namespace ms::host {
namespace {

const wchar_t* const kMsgWndClass = L"MiniStreamHostMsgWnd";
const wchar_t* const kCfgWndClass = L"MiniStreamHostCfgWnd";
const wchar_t* const kCfgTitle    = L"jianhx 被控端设置";

// 自定义消息
constexpr UINT WM_TRAY_CALLBACK = WM_APP + 101;   // 托盘回调
constexpr UINT WM_APP_QUIT      = WM_APP + 102;   // 请求退出消息循环
constexpr UINT WM_APP_STATUS    = WM_APP + 103;   // lParam = new std::wstring(状态行)
constexpr UINT WM_APP_RUNNING   = WM_APP + 104;   // wParam = 服务是否在跑

// 托盘图标
constexpr UINT kTrayIconId = 1;

// 菜单项
constexpr UINT kMenuStatus = 40001;
constexpr UINT kMenuStart  = 40002;
constexpr UINT kMenuStop   = 40003;
constexpr UINT kMenuConfig = 40004;
constexpr UINT kMenuExit   = 40005;

// 控件 ID
constexpr int kIdPort    = 1001;
constexpr int kIdPwd     = 1002;
constexpr int kIdBitrate = 1003;
constexpr int kIdFps     = 1004;
constexpr int kIdMonitor = 1005;
constexpr int kIdInput   = 1006;
constexpr int kIdSave    = 1007;
constexpr int kIdStatus  = 1008;

// 托盘回调里可能出现的「选择」事件（老式回调才有，v4 下用鼠标消息）
constexpr UINT kNinSelect    = UINT(WM_USER) + 0;
constexpr UINT kNinKeySelect = UINT(WM_USER) + 1;

// 配置窗口（客户区）尺寸
constexpr int kClientW = 480;
constexpr int kClientH = 338;
constexpr int kLabelX  = 16;
constexpr int kLabelW  = 132;
constexpr int kFieldX  = 156;

// -----------------------------------------------------------------------------
// 字符串小工具（项目统一 UTF-8 存储，Win32 一律宽字符）
// -----------------------------------------------------------------------------
std::wstring widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int len = int(s.size());
    const int need = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), len, nullptr, 0);
    if (need <= 0) return std::wstring();
    std::wstring w(size_t(need), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), len, &w[0], need);
    return w;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return std::string();
    const int len = int(w.size());
    const int need = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), len, nullptr, 0, nullptr, nullptr);
    if (need <= 0) return std::string();
    std::string s(size_t(need), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), len, &s[0], need, nullptr, nullptr);
    return s;
}

std::wstring get_text(HWND h) {
    if (!h) return std::wstring();
    const int n = ::GetWindowTextLengthW(h);
    if (n <= 0) return std::wstring();
    std::wstring s(size_t(n) + 1, L'\0');
    const int got = ::GetWindowTextW(h, &s[0], n + 1);
    s.resize(got > 0 ? size_t(got) : 0);
    return s;
}

void set_text(HWND h, const std::wstring& s) {
    if (h) ::SetWindowTextW(h, s.c_str());
}

int parse_int(const std::wstring& s, int def) {
    if (s.empty()) return def;
    wchar_t* end = nullptr;
    const long v = ::wcstol(s.c_str(), &end, 10);
    if (end == s.c_str()) return def;
    if (v < -2147483647L || v > 2147483647L) return def;
    return int(v);
}

HFONT create_ui_font() {
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);
    if (::SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
        // 用系统消息字体，中文才不会糊
        return ::CreateFontIndirectW(&ncm.lfMessageFont);
    }
    return nullptr;
}

} // namespace

// =============================================================================
//  Impl
// =============================================================================
struct HostUi::Impl {
    HostUi*   owner = nullptr;
    HINSTANCE hinst = nullptr;
    HWND      msgWnd = nullptr;
    HWND      cfgWnd = nullptr;
    HFONT     font = nullptr;
    bool      fontOwned = false;
    DWORD     uiThreadId = 0;
    UINT      taskbarCreatedMsg = 0;
    bool      destroying = false;

    bool         running = false;
    std::wstring status  = L"未启动";

    NOTIFYICONDATAW nid{};
    bool            iconAdded = false;
    // 托盘数据既被 UI 线程改，也可能被工作线程（notify）改；用递归锁是因为
    // Shell_NotifyIconW 在个别情况下会同步回调窗口过程。
    std::recursive_mutex nidMtx;

    // 配置窗口控件
    HWND edPort = nullptr, edPwd = nullptr, edBitrate = nullptr;
    HWND cbFps = nullptr, cbMonitor = nullptr;
    HWND chkInput = nullptr, btnSave = nullptr, stStatus = nullptr;
    bool statusIsError = false;

    std::vector<MonitorInfo> monitors;

    // --- 静态窗口过程：转发到 Impl（GWLP_USERDATA 里存的是 this）-------------
    static LRESULT CALLBACK msg_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
        Impl* self = reinterpret_cast<Impl*>(::GetWindowLongPtrW(h, GWLP_USERDATA));
        if (m == WM_NCCREATE) {
            const CREATESTRUCTW* cs = reinterpret_cast<const CREATESTRUCTW*>(l);
            self = static_cast<Impl*>(cs->lpCreateParams);
            ::SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            if (self) self->msgWnd = h;
        }
        if (self) return self->on_msg(h, m, w, l);
        return ::DefWindowProcW(h, m, w, l);
    }

    static LRESULT CALLBACK cfg_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
        Impl* self = reinterpret_cast<Impl*>(::GetWindowLongPtrW(h, GWLP_USERDATA));
        if (m == WM_NCCREATE) {
            const CREATESTRUCTW* cs = reinterpret_cast<const CREATESTRUCTW*>(l);
            self = static_cast<Impl*>(cs->lpCreateParams);
            ::SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            if (self) self->cfgWnd = h;
        }
        if (self) return self->on_cfg(h, m, w, l);
        return ::DefWindowProcW(h, m, w, l);
    }

    // --- 创建 ---------------------------------------------------------------
    bool create_windows(std::string& err) {
        WNDCLASSEXW wc{};
        wc.cbSize        = sizeof(wc);
        wc.style         = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc   = msg_proc;
        wc.hInstance     = hinst;
        wc.hCursor       = ::LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = kMsgWndClass;
        wc.hIcon         = ::LoadIconW(nullptr, IDI_APPLICATION);
        wc.hIconSm       = wc.hIcon;
        if (!::RegisterClassExW(&wc) && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            err = "注册消息窗口类失败";
            return false;
        }

        wc.lpfnWndProc   = cfg_proc;
        wc.hbrBackground = ::GetSysColorBrush(COLOR_BTNFACE);
        wc.lpszClassName = kCfgWndClass;
        if (!::RegisterClassExW(&wc) && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            err = "注册配置窗口类失败";
            return false;
        }

        // 隐藏消息窗口：唯一常驻窗口，托盘回调都发到这里
        msgWnd = ::CreateWindowExW(0, kMsgWndClass, L"MiniStreamHost", 0,
                                   0, 0, 0, 0, nullptr, nullptr, hinst, this);
        if (!msgWnd) {
            err = "创建消息窗口失败";
            return false;
        }
        uiThreadId = ::GetCurrentThreadId();
        // 资源管理器重启后要重新加图标
        taskbarCreatedMsg = ::RegisterWindowMessageW(L"TaskbarCreated");
        return true;
    }

    // --- 托盘 ---------------------------------------------------------------
    void add_icon() {
        std::lock_guard<std::recursive_mutex> lk(nidMtx);
        nid = NOTIFYICONDATAW{};
        nid.cbSize           = sizeof(NOTIFYICONDATAW);
        nid.hWnd             = msgWnd;
        nid.uID              = kTrayIconId;
        nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        nid.uCallbackMessage = WM_TRAY_CALLBACK;
        nid.hIcon            = ::LoadIconW(nullptr, IDI_APPLICATION);
        ::StringCchCopyW(nid.szTip, _countof(nid.szTip), L"jianhx 被控端");
        if (!::Shell_NotifyIconW(NIM_ADD, &nid)) return;
        iconAdded = true;
        // 用 v4 回调格式：lParam 低位是事件，wParam 里带坐标
        NOTIFYICONDATAW ver = nid;
        ver.uVersion = NOTIFYICON_VERSION_4;
        ::Shell_NotifyIconW(NIM_SETVERSION, &ver);
        refresh_tip_locked();
    }

    void remove_icon() {
        std::lock_guard<std::recursive_mutex> lk(nidMtx);
        if (!iconAdded) return;
        ::Shell_NotifyIconW(NIM_DELETE, &nid);
        iconAdded = false;
    }

    void refresh_tip_locked() {
        if (!iconAdded) return;
        std::wstring tip = L"jianhx 被控端 - " + status;
        if (tip.size() > 120) tip.resize(120);   // szTip 只有 128 个 wchar
        ::StringCchCopyW(nid.szTip, _countof(nid.szTip), tip.c_str());
        nid.uFlags = NIF_TIP;
        ::Shell_NotifyIconW(NIM_MODIFY, &nid);
    }

    void balloon(const std::wstring& title, const std::wstring& msg) {
        std::lock_guard<std::recursive_mutex> lk(nidMtx);
        if (!iconAdded) return;
        nid.uFlags      = NIF_INFO;
        nid.dwInfoFlags = NIIF_INFO;
        ::StringCchCopyW(nid.szInfoTitle, _countof(nid.szInfoTitle), title.c_str());
        ::StringCchCopyW(nid.szInfo, _countof(nid.szInfo), msg.c_str());
        ::Shell_NotifyIconW(NIM_MODIFY, &nid);
    }

    // --- 状态 ---------------------------------------------------------------
    // 只刷新配置窗口里的状态行（UI 线程）
    void set_status_line(const std::wstring& text, bool isError) {
        statusIsError = isError;
        if (cfgWnd && stStatus) {
            ::SetWindowTextW(stStatus, text.c_str());
            ::InvalidateRect(stStatus, nullptr, TRUE);
        }
    }

    // 状态行 + 托盘 tooltip（UI 线程）
    void apply_status(const std::wstring& text, bool isError) {
        status = text;
        set_status_line(text, isError);
        std::lock_guard<std::recursive_mutex> lk(nidMtx);
        refresh_tip_locked();
    }

    // 回收还没处理的 WM_APP_STATUS（销毁窗口前调用，避免泄漏）
    void drain_status_messages() {
        if (!msgWnd) return;
        MSG m;
        while (::PeekMessageW(&m, msgWnd, WM_APP_STATUS, WM_APP_STATUS, PM_REMOVE)) {
            delete reinterpret_cast<std::wstring*>(m.lParam);
        }
    }

    // --- 配置窗口 -----------------------------------------------------------
    HWND mk(const wchar_t* cls, const wchar_t* text, DWORD style,
            int x, int y, int w, int h, int id) {
        HWND c = ::CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
                                   x, y, w, h, cfgWnd,
                                   reinterpret_cast<HMENU>(INT_PTR(id)), hinst, nullptr);
        if (c && font) ::SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        return c;
    }

    void fill_fps_combo() {
        if (!cbFps) return;
        static const int kFps[] = {30, 60, 120, 144};
        for (int v : kFps) {
            wchar_t buf[16] = {0};
            ::StringCchPrintfW(buf, _countof(buf), L"%d", v);
            const int idx = int(::SendMessageW(cbFps, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(buf)));
            if (idx >= 0) ::SendMessageW(cbFps, CB_SETITEMDATA, WPARAM(idx), LPARAM(v));
        }
    }

    void fill_monitor_combo() {
        if (!cbMonitor) return;
        ::SendMessageW(cbMonitor, CB_RESETCONTENT, 0, 0);
        monitors = ScreenCapture::enumerate_monitors();
        if (monitors.empty()) {
            // 枚举失败（比如采集未初始化）时给个兜底项，避免下拉框是空的
            const int idx = int(::SendMessageW(cbMonitor, CB_ADDSTRING, 0,
                                               reinterpret_cast<LPARAM>(L"默认显示器")));
            if (idx >= 0) ::SendMessageW(cbMonitor, CB_SETITEMDATA, WPARAM(idx), LPARAM(0));
            return;
        }
        for (size_t i = 0; i < monitors.size(); ++i) {
            const MonitorInfo& mi = monitors[i];
            std::wstring label = widen(mi.friendlyName);
            if (label.empty()) label = widen(mi.deviceName);
            if (label.empty()) label = L"显示器 " + std::to_wstring(mi.index);
            if (mi.width > 0 && mi.height > 0) {
                label += L" (" + std::to_wstring(mi.width) + L"x" + std::to_wstring(mi.height) + L")";
            }
            if (mi.isPrimary) label += L" [主]";
            const int idx = int(::SendMessageW(cbMonitor, CB_ADDSTRING, 0,
                                               reinterpret_cast<LPARAM>(label.c_str())));
            if (idx >= 0) {
                ::SendMessageW(cbMonitor, CB_SETITEMDATA, WPARAM(idx), LPARAM(INT_PTR(i)));
            }
        }
    }

    void create_config_window() {
        if (cfgWnd) return;

        const DWORD style   = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
        const DWORD exStyle = WS_EX_CONTROLPARENT;   // 让 IsDialogMessage 能处理 Tab
        RECT rc{0, 0, kClientW, kClientH};
        ::AdjustWindowRectEx(&rc, style, FALSE, exStyle);
        const int w = rc.right - rc.left;
        const int h = rc.bottom - rc.top;
        const int x = (::GetSystemMetrics(SM_CXSCREEN) - w) / 2;
        const int y = (::GetSystemMetrics(SM_CYSCREEN) - h) / 2;

        cfgWnd = ::CreateWindowExW(exStyle, kCfgWndClass, kCfgTitle, style,
                                   x, y, w, h, nullptr, nullptr, hinst, this);
        if (!cfgWnd) {
            MS_ERR("创建配置窗口失败, 错误码 %lu", ::GetLastError());
            return;
        }

        font = create_ui_font();
        if (font) {
            fontOwned = true;
        } else {
            font = reinterpret_cast<HFONT>(::GetStockObject(DEFAULT_GUI_FONT));
        }

        // 行 1：端口
        mk(L"STATIC", L"端口 (>10000):", 0, kLabelX, 20, kLabelW, 20, -1);
        edPort = mk(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL | ES_NUMBER,
                    kFieldX, 18, 90, 22, kIdPort);
        // 行 2：密码（明文可读，自用工具不做遮挡）
        mk(L"STATIC", L"访问密码:", 0, kLabelX, 54, kLabelW, 20, -1);
        edPwd = mk(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, kFieldX, 52, 220, 22, kIdPwd);
        // 行 3：码率
        mk(L"STATIC", L"码率 kbps (0=自适应):", 0, kLabelX, 88, kLabelW, 20, -1);
        edBitrate = mk(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL | ES_NUMBER,
                       kFieldX, 86, 90, 22, kIdBitrate);
        // 行 4：帧率
        mk(L"STATIC", L"帧率:", 0, kLabelX, 122, kLabelW, 20, -1);
        cbFps = mk(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                   kFieldX, 120, 100, 200, kIdFps);
        // 行 5：显示器
        mk(L"STATIC", L"显示器:", 0, kLabelX, 156, kLabelW, 20, -1);
        cbMonitor = mk(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                       kFieldX, 154, 300, 240, kIdMonitor);
        // 行 6：输入注入
        chkInput = mk(L"BUTTON", L"启用输入注入（鼠标 / 键盘）",
                      WS_TABSTOP | BS_AUTOCHECKBOX, kLabelX, 192, 320, 22, kIdInput);
        // 行 7：状态行
        stStatus = mk(L"STATIC", L"", SS_LEFT, kLabelX, 226, kClientW - 2 * kLabelX, 60, kIdStatus);
        // 行 8：保存
        btnSave = mk(L"BUTTON", L"保存", WS_TABSTOP | BS_DEFPUSHBUTTON,
                     kFieldX, 296, 110, 26, kIdSave);

        fill_fps_combo();
        fill_monitor_combo();
        load_fields();
    }

    void load_fields() {
        if (!cfgWnd || !owner || !owner->cfg_) return;
        const HostConfig& c = *owner->cfg_;

        set_text(edPort, std::to_wstring(int(c.port)));
        set_text(edPwd, widen(c.password));
        set_text(edBitrate, std::to_wstring(c.bitrateKbps));

        int fpsSel = 1;   // 默认 60
        const int fpsCount = int(::SendMessageW(cbFps, CB_GETCOUNT, 0, 0));
        for (int i = 0; i < fpsCount; ++i) {
            if (int(::SendMessageW(cbFps, CB_GETITEMDATA, WPARAM(i), 0)) == c.fps) { fpsSel = i; break; }
        }
        ::SendMessageW(cbFps, CB_SETCURSEL, WPARAM(fpsSel), 0);

        int monSel = 0;
        const int monCount = int(::SendMessageW(cbMonitor, CB_GETCOUNT, 0, 0));
        for (int i = 0; i < monCount; ++i) {
            if (int(::SendMessageW(cbMonitor, CB_GETITEMDATA, WPARAM(i), 0)) == c.monitor) {
                monSel = i;
                break;
            }
        }
        ::SendMessageW(cbMonitor, CB_SETCURSEL, WPARAM(monSel), 0);

        ::SendMessageW(chkInput, BM_SETCHECK,
                       c.inputEnabled ? WPARAM(BST_CHECKED) : WPARAM(BST_UNCHECKED), 0);
        // 只刷新窗口里的状态行：托盘 tooltip 留给服务状态（启动/停止）用
        set_status_line(L"当前配置: " + std::to_wstring(int(c.port)) + L" 端口 / " +
                            std::to_wstring(c.fps) + L" fps",
                        false);
    }

    // 「保存」按钮
    void do_save() {
        if (!owner || !owner->cfg_) return;

        const int port = parse_int(get_text(edPort), -1);
        if (port < int(defaults::kMinPort) || port > 65535) {
            apply_status(L"保存失败：端口必须在 10001-65535 之间", true);
            return;
        }
        const std::wstring pwd = get_text(edPwd);
        if (pwd.empty()) {
            apply_status(L"保存失败：访问密码不能为空", true);
            return;
        }
        const int bitrate = parse_int(get_text(edBitrate), -1);
        if (bitrate < 0) {
            apply_status(L"保存失败：码率必须是不小于 0 的整数（0 = 自适应）", true);
            return;
        }

        int fpsSel = int(::SendMessageW(cbFps, CB_GETCURSEL, 0, 0));
        if (fpsSel < 0) fpsSel = 1;
        int fps = int(::SendMessageW(cbFps, CB_GETITEMDATA, WPARAM(fpsSel), 0));
        if (fps <= 0) fps = 60;

        int monSel = int(::SendMessageW(cbMonitor, CB_GETCURSEL, 0, 0));
        if (monSel < 0) monSel = 0;
        const int monitor = int(::SendMessageW(cbMonitor, CB_GETITEMDATA, WPARAM(monSel), 0));

        HostConfig c = *owner->cfg_;
        c.port         = u16(port);
        c.password     = narrow(pwd);
        c.bitrateKbps  = bitrate;
        c.fps          = fps;
        c.monitor      = monitor;
        c.inputEnabled = (::SendMessageW(chkInput, BM_GETCHECK, 0, 0) == BST_CHECKED);

        std::string err;
        if (!c.validate(err)) {
            apply_status(L"保存失败：" + widen(err), true);
            return;
        }
        // ini 路径以运行时记录为准（配置对象里可能是空的）
        if (c.configPath.empty()) c.configPath = owner->configPath_;
        *owner->cfg_ = c;

        if (!owner->cfg_->save(err)) {
            apply_status(L"保存失败：" + widen(err), true);
            return;
        }
        if (owner->onSaved_) {
            // 回调里通常会重建服务端监听，可能耗时，但自用工具可以接受同步调用
            owner->onSaved_(*owner->cfg_);
        }
        apply_status(L"已保存到 " + widen(owner->cfg_->configPath) +
                         L"（端口 " + std::to_wstring(port) + L"）",
                     false);
        balloon(L"jianhx", L"配置已保存");
    }

    void show_config() {
        if (!cfgWnd) create_config_window();
        if (!cfgWnd) return;
        load_fields();
        ::ShowWindow(cfgWnd, SW_SHOW);
        ::SetForegroundWindow(cfgWnd);
    }

    // --- 托盘右键菜单 -------------------------------------------------------
    void show_menu() {
        POINT pt{};
        ::GetCursorPos(&pt);

        HMENU m = ::CreatePopupMenu();
        if (!m) return;
        ::AppendMenuW(m, MF_STRING | MF_GRAYED, kMenuStatus,
                      running ? L"状态: 正在提供服务" : L"状态: 未启动");
        ::AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        ::AppendMenuW(m, MF_STRING | (running ? MF_GRAYED : 0u), kMenuStart, L"启动服务");
        ::AppendMenuW(m, MF_STRING | (running ? 0u : MF_GRAYED), kMenuStop, L"停止服务");
        ::AppendMenuW(m, MF_STRING, kMenuConfig, L"打开配置(&C)...");
        ::AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        ::AppendMenuW(m, MF_STRING, kMenuExit, L"退出(&X)");

        // 托盘菜单的经典要求：先抢前台，菜单结束再补一条空消息，否则点别处菜单不收
        ::SetForegroundWindow(msgWnd);
        const int cmd = int(::TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                                             pt.x, pt.y, 0, msgWnd, nullptr));
        ::DestroyMenu(m);
        ::PostMessageW(msgWnd, WM_NULL, 0, 0);

        switch (UINT(cmd)) {
        case kMenuStart:
            if (owner->onStart_) owner->onStart_();
            break;
        case kMenuStop:
            if (owner->onStop_) owner->onStop_();
            break;
        case kMenuConfig:
            show_config();
            break;
        case kMenuExit:
            if (owner->onQuit_) owner->onQuit_();
            owner->post_quit();
            break;
        default:
            break;
        }
    }

    // --- 消息窗口过程 -------------------------------------------------------
    LRESULT on_msg(HWND h, UINT m, WPARAM w, LPARAM l) {
        if (m == taskbarCreatedMsg && taskbarCreatedMsg != 0) {
            // 资源管理器重启：图标没了，重新加回来
            iconAdded = false;
            add_icon();
            return 0;
        }
        switch (m) {
        case WM_TRAY_CALLBACK: {
            const UINT ev = UINT(LOWORD(l));
            // v4 回调：lParam 低位是事件。选择/左键 -> 打开配置，右键 -> 菜单
            if (ev == UINT(WM_LBUTTONUP) || ev == UINT(WM_LBUTTONDBLCLK) ||
                ev == kNinSelect || ev == kNinKeySelect) {
                show_config();
            } else if (ev == UINT(WM_RBUTTONUP) || ev == UINT(WM_CONTEXTMENU)) {
                show_menu();
            }
            return 0;
        }
        case WM_APP_STATUS: {
            std::unique_ptr<std::wstring> s(reinterpret_cast<std::wstring*>(l));
            if (s) apply_status(*s, false);
            return 0;
        }
        case WM_APP_RUNNING:
            running = (w != 0);
            apply_status(running ? L"正在提供服务" : L"服务已停止", false);
            return 0;
        case WM_APP_QUIT:
            ::PostQuitMessage(0);
            return 0;
        case WM_CLOSE:
            ::DestroyWindow(h);
            return 0;
        case WM_ENDSESSION:
            remove_icon();   // 注销/关机：把图标摘掉，免得留个幽灵
            break;
        case WM_DESTROY:
            msgWnd = nullptr;
            if (!destroying) ::PostQuitMessage(0);
            return 0;
        default:
            break;
        }
        return ::DefWindowProcW(h, m, w, l);
    }

    // --- 配置窗口过程 -------------------------------------------------------
    LRESULT on_cfg(HWND h, UINT m, WPARAM w, LPARAM l) {
        switch (m) {
        case WM_COMMAND:
            if (LOWORD(w) == UINT(kIdSave) && HIWORD(w) == BN_CLICKED) {
                do_save();
                return 0;
            }
            return 0;
        case WM_CLOSE:
            // 关闭 = 隐藏，窗口复用，下次打开更快
            ::ShowWindow(h, SW_HIDE);
            return 0;
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(w);
            ::SetBkMode(dc, TRANSPARENT);
            if (reinterpret_cast<HWND>(l) == stStatus && statusIsError) {
                ::SetTextColor(dc, RGB(192, 0, 0));
            }
            return reinterpret_cast<LRESULT>(::GetSysColorBrush(COLOR_BTNFACE));
        }
        case WM_DESTROY:
            cfgWnd = nullptr;
            stStatus = nullptr;
            return 0;
        default:
            break;
        }
        return ::DefWindowProcW(h, m, w, l);
    }
};

// =============================================================================
//  HostUi
// =============================================================================
HostUi::~HostUi() {
    destroy();
}

bool HostUi::create(HINSTANCE hinst, HostConfig* cfg, const std::string& configPath,
                    std::string& err) {
    if (impl_) return true;
    if (!cfg) { err = "配置对象为空"; return false; }

    hinst_      = hinst ? hinst : ::GetModuleHandleW(nullptr);
    cfg_        = cfg;
    configPath_ = configPath;

    std::unique_ptr<Impl> im(new Impl());
    im->owner = this;
    im->hinst = hinst_;

    if (!im->create_windows(err)) return false;

    impl_ = im.release();
    impl_->add_icon();
    MS_INFO("被控端托盘已创建, 配置: %s", configPath_.c_str());
    return true;
}

void HostUi::destroy() {
    Impl* im = impl_;
    if (!im) return;

    if (im->uiThreadId != 0 && ::GetCurrentThreadId() != im->uiThreadId) {
        // 非 UI 线程：Win32 不允许跨线程 DestroyWindow，只能请求退出，
        // 真正的资源回收交给消息循环所在的线程（析构应在该线程调用）
        post_quit();
        return;
    }

    im->drain_status_messages();
    im->remove_icon();
    im->destroying = true;
    if (im->cfgWnd) { ::DestroyWindow(im->cfgWnd); im->cfgWnd = nullptr; }
    if (im->msgWnd) { ::DestroyWindow(im->msgWnd); im->msgWnd = nullptr; }
    if (im->fontOwned && im->font) ::DeleteObject(im->font);
    im->font = nullptr;

    delete im;
    impl_ = nullptr;
    cfg_ = nullptr;
}

int HostUi::message_loop() {
    if (!impl_ || !impl_->msgWnd) return 0;

    MSG msg;
    while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
        const bool cfgAlive = impl_->cfgWnd && ::IsWindowVisible(impl_->cfgWnd);
        // 配置窗口是普通窗口，靠 IsDialogMessage 提供 Tab 切换焦点
        if (cfgAlive && ::IsDialogMessageW(impl_->cfgWnd, &msg)) continue;

        // IsDialogMessage 对普通窗口不认默认按钮，回车自己兜一下
        if (cfgAlive && msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN) {
            HWND focus = ::GetFocus();
            if (focus && ::GetAncestor(focus, GA_ROOT) == impl_->cfgWnd) {
                ::SendMessageW(impl_->cfgWnd, WM_COMMAND,
                               MAKEWPARAM(kIdSave, BN_CLICKED),
                               reinterpret_cast<LPARAM>(impl_->btnSave));
                continue;
            }
        }
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }
    return int(msg.wParam);
}

void HostUi::post_quit() {
    if (!impl_ || !impl_->msgWnd) return;
    ::PostMessageW(impl_->msgWnd, WM_APP_QUIT, 0, 0);
}

void HostUi::set_status(const std::string& text) {
    if (!impl_ || !impl_->msgWnd) return;
    // 跨线程：堆上分配一份，由 UI 线程在 WM_APP_STATUS 里 delete
    std::unique_ptr<std::wstring> payload(new std::wstring(widen(text)));
    if (!::PostMessageW(impl_->msgWnd, WM_APP_STATUS, 0,
                        reinterpret_cast<LPARAM>(payload.get()))) {
        return;   // 投递失败，unique_ptr 自动回收
    }
    payload.release();
}

void HostUi::set_running(bool r) {
    if (!impl_ || !impl_->msgWnd) return;
    ::PostMessageW(impl_->msgWnd, WM_APP_RUNNING, r ? 1 : 0, 0);
}

void HostUi::notify(const std::string& title, const std::string& msg) {
    if (!impl_) return;
    impl_->balloon(widen(title), widen(msg));
}

} // namespace ms::host
