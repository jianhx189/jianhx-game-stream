// =============================================================================
//  connect_ui.cpp - 远控端连接面板（RustDesk 风格重写）
//
//  纯 Win32 手工建窗（不用 .rc），阻塞式模态：内部跑一个 GetMessage 循环。
//  布局：顶部标识区（MiniStream + 副标题）→「连接」分组（大输入框一键搞定
//  地址+端口）→「画质」分组（预设联动三个下拉）→「选项」分组 → 底部通栏
//  大按钮。所有校验失败就地红字提示，绝不弹 MessageBox。
//
//  为什么不用 DialogBoxParam：要走资源模板，项目刻意避免 rc 依赖；
//  自己建窗 + IsDialogMessage 一样能拿到 Tab 切焦点和回车触发默认按钮。
// =============================================================================
#include "connect_ui.h"     // 经由 control_client.h -> net.h 已经带入 windows.h
#include "firewall.h"

#include "../common/log.h"

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>    // Shell_NotifyIcon：关闭窗口最小化到托盘
#include <strsafe.h>

#include <string>

namespace ms::client {
namespace {

const wchar_t* const kDlgClass = L"MiniStreamConnectWnd";
const wchar_t* const kDlgTitle = L"jianhx - 连接到被控端";

// -----------------------------------------------------------------------------
// 布局常量表（客户区坐标，全部集中在此，别处不许出现魔法数）
// -----------------------------------------------------------------------------
constexpr int kClientW     = 560;   // 客户区固定宽
constexpr int kMarginX     = 24;    // 左右统一页边距
constexpr int kContentW    = kClientW - kMarginX * 2;  // 内容通栏宽 512

// 顶部标识区
constexpr int kBrandY      = 22;
constexpr int kBrandH      = 44;    // 28pt 大字行
constexpr int kSubY        = 66;
constexpr int kSubH        = 20;    // 副标题行
constexpr int kFwY         = 90;    // 防火墙放行状态行
constexpr int kFwH         = 20;

// 「连接」分组
constexpr int kGrpConnY    = 116;
constexpr int kGrpConnH    = 178;
constexpr int kGrpPadX     = 16;    // 分组框内左右内边距
constexpr int kGrpPadTop   = 18;    // 分组框标题文字下方起始偏移
constexpr int kConnInW     = kContentW - kGrpPadX * 2;  // 组内通栏宽

// 「画质」分组
constexpr int kGrpQualY    = 291;   // = 100 + 178 + 13
constexpr int kGrpQualH    = 118;

// 「选项」分组
constexpr int kGrpOptY     = 425;   // = 291 + 118 + 16
constexpr int kGrpOptH     = 136;   // 加了双线复选框 + 备线地址输入框

// 底部
constexpr int kErrY        = 541;
constexpr int kErrH        = 18;
constexpr int kBtnY        = 593;   // = 425 + 136 + 32（选项组变高了，按钮往下挪）
constexpr int kBtnH        = 44;    // 通栏大按钮
constexpr int kClientH     = kBtnY + kBtnH + 16;

// 组内行尺寸
constexpr int kRowH        = 30;    // 常规控件行高（密码框等）
constexpr int kRowGap      = 10;    // 行间距
constexpr int kBigEditH    = 34;    // 大输入框行高
constexpr int kCheckH      = 22;    // 复选框行高
constexpr int kComboH      = 24;    // 下拉框行高（编辑部分）
constexpr int kComboListH  = 220;   // 下拉展开列表高度
constexpr int kLabelW      = 64;    // 下拉框前置标签宽

// 码率合法性上限（自适应 = 0）
constexpr int kMaxBitrateKbps = 200000;

// 控件 ID
constexpr int IDC_ADDR     = 2001;
constexpr int IDC_PWD      = 2002;
constexpr int IDC_REMEMBER = 2003;
constexpr int IDC_HOTKEY   = 2004;
constexpr int IDC_LAST     = 2005;
constexpr int IDC_PRESET   = 2006;
constexpr int IDC_RES      = 2007;
constexpr int IDC_FPS      = 2008;
constexpr int IDC_BITRATE  = 2009;
constexpr int IDC_FULLSCREEN = 2010;
constexpr int IDC_OVERLAY  = 2011;
constexpr int IDC_ERR      = 2012;
constexpr int IDC_CONNECT  = IDOK;
constexpr int IDC_FWBTN    = 2013;   // 防火墙放行按钮
constexpr int IDC_FWSTAT   = 2014;   // 防火墙状态文本
constexpr int IDC_DUALLINE = 2015;   // 双线复选框
constexpr int IDC_BACKUP   = 2016;   // 备线地址输入框

struct DlgCtx {
    HINSTANCE hinst = nullptr;
    HWND      wnd = nullptr;

    // 连接组
    HWND edAddr = nullptr, edPwd = nullptr, chkRemember = nullptr;
    HWND stLast = nullptr;
    // 画质组
    HWND cbPreset = nullptr, cbRes = nullptr, cbFps = nullptr, cbBitrate = nullptr;
    // 选项组
    HWND chkFullscreen = nullptr, chkOverlay = nullptr, stHotkey = nullptr;
    // 双线（备线：键鼠走备线，主线留给视频）
    HWND chkDualLine = nullptr, edBackupAddr = nullptr;
    // 防火墙
    HWND btnFw = nullptr, stFw = nullptr;
    // 公共
    HWND btnOk = nullptr, stErr = nullptr;

    const ClientConfig*  initial = nullptr;
    ConnectDialogResult* out = nullptr;

    // 禁止预设下拉自身联动触发「手动改动」判定（程序设值时置位）
    bool syncingCombos = false;

    // ---- 双线（备线）----
    // 用户在选项组勾「启用双线」后显示备线地址输入框
    bool dualLine = false;

    HFONT font      = nullptr;   // 常规 UI 字体
    HFONT fontBig   = nullptr;   // 14pt：大输入框 / 大按钮
    HFONT fontBrand = nullptr;   // 28pt 粗体：应用名
    bool  fontsOwned = false;

    // ---- 托盘常驻（用户要求：用完不能直接退出，要留在后台）----
    // 关闭窗口 = 最小化到托盘；托盘图标双击/左键 = 恢复；右键菜单 = 显示/退出。
    bool  minimizeToTray = true;   // 来自 client.ini 的 minimize_to_tray
    bool  trayAdded = false;
    bool  allowClose = false;      // 点托盘菜单「退出」时置位，放行真正的退出
    HICON trayIcon  = nullptr;
};

// -----------------------------------------------------------------------------
// 小工具
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

std::wstring trim(const std::wstring& s) {
    size_t a = 0, b = s.size();
    auto isSpace = [](wchar_t c) { return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n'; };
    while (a < b && isSpace(s[a])) ++a;
    while (b > a && isSpace(s[b - 1])) --b;
    return s.substr(a, b - a);
}

HFONT create_point_font(int pt, bool bold) {
    const HDC dc = ::GetDC(nullptr);
    if (!dc) return nullptr;
    const int px = -::MulDiv(pt, ::GetDeviceCaps(dc, LOGPIXELSY), 72);
    ::ReleaseDC(nullptr, dc);
    return ::CreateFontW(px, 0, 0, 0, bold ? FW_BOLD : FW_NORMAL,
                         FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                         OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                         CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                         L"Microsoft YaHei UI");
}

void show_error(DlgCtx* ctx, const wchar_t* text, HWND focus) {
    set_text(ctx->stErr, text);
    if (focus) ::SetFocus(focus);
}

// -----------------------------------------------------------------------------
// 画质下拉的数据读写
// -----------------------------------------------------------------------------
int combo_data(HWND cb, int defVal) {
    const int sel = int(::SendMessageW(cb, CB_GETCURSEL, 0, 0));
    if (sel < 0) return defVal;
    return int(::SendMessageW(cb, CB_GETITEMDATA, WPARAM(sel), 0));
}

// 按数据值找下拉项索引，找不到返回 -1
int combo_find_data(HWND cb, int value) {
    const int n = int(::SendMessageW(cb, CB_GETCOUNT, 0, 0));
    for (int i = 0; i < n; ++i) {
        if (int(::SendMessageW(cb, CB_GETITEMDATA, WPARAM(i), 0)) == value) return i;
    }
    return -1;
}

// 分辨率下拉当前选中项的 w/h
void res_selection(DlgCtx* ctx, int& w, int& h) {
    const int sel = int(::SendMessageW(ctx->cbRes, CB_GETCURSEL, 0, 0));
    const auto& opts = quality::resolution_options();
    w = 0; h = 0;
    if (sel >= 0 && sel < int(opts.size())) {
        w = opts[size_t(sel)].w;
        h = opts[size_t(sel)].h;
    }
}

// 三下拉 -> 当前生效的 profile（preset 由调用方填）
quality::QualityProfile profile_from_combos(DlgCtx* ctx) {
    quality::QualityProfile q;
    res_selection(ctx, q.width, q.height);
    q.fps = combo_data(ctx->cbFps, 60);
    q.bitrateKbps = combo_data(ctx->cbBitrate, 0);
    return q;
}

// 三下拉是否与某个预设的缺省值完全一致（Custom 除外）
bool combos_match_preset(DlgCtx* ctx, quality::Preset p) {
    const quality::QualityProfile def = quality::default_profile(p);
    const quality::QualityProfile cur = profile_from_combos(ctx);
    return cur.width == def.width && cur.height == def.height &&
           cur.fps == def.fps && cur.bitrateKbps == def.bitrateKbps;
}

// 依据三下拉当前值刷新预设下拉：恰好匹配某预设 -> 选它；否则选「自定义」
void sync_preset_combo(DlgCtx* ctx) {
    int sel = int(quality::Preset::Custom);
    for (int i = 0; i < quality::kPresetCount - 1; ++i) {
        if (combos_match_preset(ctx, quality::Preset(i))) {
            sel = i;
            break;
        }
    }
    ::SendMessageW(ctx->cbPreset, CB_SETCURSEL, WPARAM(sel), 0);
}

// 分辨率下拉按 w/h 找索引（0x0 原生档也是一项）
int combo_find_data_w_h(HWND cb, int w, int h) {
    const int n = int(::SendMessageW(cb, CB_GETCOUNT, 0, 0));
    const auto& opts = quality::resolution_options();
    for (int i = 0; i < n && i < int(opts.size()); ++i) {
        if (opts[size_t(i)].w == w && opts[size_t(i)].h == h) return i;
    }
    return -1;
}

// 把某预设的缺省值写进三个下拉（同时刷新预设下拉本身）
void apply_preset(DlgCtx* ctx, quality::Preset p) {
    const quality::QualityProfile q = quality::default_profile(p);
    ctx->syncingCombos = true;

    int sel = combo_find_data_w_h(ctx->cbRes, q.width, q.height);
    if (sel >= 0) ::SendMessageW(ctx->cbRes, CB_SETCURSEL, WPARAM(sel), 0);

    sel = combo_find_data(ctx->cbFps, q.fps);
    if (sel >= 0) ::SendMessageW(ctx->cbFps, CB_SETCURSEL, WPARAM(sel), 0);

    sel = combo_find_data(ctx->cbBitrate, q.bitrateKbps);
    if (sel >= 0) ::SendMessageW(ctx->cbBitrate, CB_SETCURSEL, WPARAM(sel), 0);

    ::SendMessageW(ctx->cbPreset, CB_SETCURSEL, WPARAM(int(p)), 0);
    ctx->syncingCombos = false;
}

// -----------------------------------------------------------------------------
// 地址解析：`IP[:端口]` / `域名[:端口]`，无端口则默认 20000
// -----------------------------------------------------------------------------
bool parse_address(const std::wstring& raw, std::wstring& host, int& port, std::wstring& err) {
    const std::wstring s = trim(raw);
    if (s.empty()) { err = L"请填写被控端地址（IP 或域名）"; return false; }

    const size_t colon = s.rfind(L':');
    if (colon != std::wstring::npos && s.find(L']') == std::wstring::npos) {
        // 形如 host:port（IPv6 字面量这种带冒号又无方括号的输入不支持，就地报错）
        host = trim(s.substr(0, colon));
        const std::wstring portStr = trim(s.substr(colon + 1));
        if (host.empty() || portStr.empty()) {
            err = L"地址格式不对，应形如 1.2.3.4 或 game.example.com:20000";
            return false;
        }
        wchar_t* end = nullptr;
        const long v = ::wcstol(portStr.c_str(), &end, 10);
        if (end == portStr.c_str() || *end != L'\0' || v < 0 || v > 65535) {
            err = L"端口必须是 1..65535 的整数";
            return false;
        }
        port = int(v);
    } else {
        host = s;
        port = int(defaults::kPort);
    }

    if (!ms::is_valid_port(port)) {
        err = L"端口必须大于 10000 且不超过 65535";
        return false;
    }
    return true;
}

// -----------------------------------------------------------------------------
// 「连接」按钮：本地校验通过才关窗
// -----------------------------------------------------------------------------
void on_ok(DlgCtx* ctx) {
    std::wstring host, parseErr;
    int port = 0;
    if (!parse_address(get_text(ctx->edAddr), host, port, parseErr)) {
        show_error(ctx, parseErr.c_str(), ctx->edAddr);
        return;
    }

    const std::wstring pwd = get_text(ctx->edPwd);
    if (pwd.empty()) {
        show_error(ctx, L"请填写验证码（被控端设置的访问密码）", ctx->edPwd);
        return;
    }

    quality::QualityProfile q = profile_from_combos(ctx);

    // 分辨率：档位值，valid_dimensions 兜底
    if (!quality::valid_dimensions(q.width, q.height)) {
        show_error(ctx, L"分辨率档位不合法（0x0 = 跟随被控端）", ctx->cbRes);
        return;
    }

    // 码率：0（自适应）或 1000..200000
    if (q.bitrateKbps != 0 &&
        (q.bitrateKbps < 1000 || q.bitrateKbps > kMaxBitrateKbps)) {
        show_error(ctx, L"码率必须是 0（自适应）或 1000..200000 kbps", ctx->cbBitrate);
        return;
    }

    // 预设档位：恰好匹配某预设则记该预设，否则 Custom
    q.preset = quality::Preset::Custom;
    for (int i = 0; i < quality::kPresetCount - 1; ++i) {
        if (combos_match_preset(ctx, quality::Preset(i))) {
            q.preset = quality::Preset(i);
            break;
        }
    }

    ConnectDialogResult* r = ctx->out;
    r->params.host        = narrow(host);
    r->params.port        = u16(port);
    r->params.password    = narrow(pwd);
    r->params.width       = q.width;
    r->params.height      = q.height;
    r->params.fps         = q.fps;
    r->params.bitrateKbps = q.bitrateKbps;
    r->fullscreen   = (::SendMessageW(ctx->chkFullscreen, BM_GETCHECK, 0, 0) == BST_CHECKED);
    r->overlayEnabled = (::SendMessageW(ctx->chkOverlay, BM_GETCHECK, 0, 0) == BST_CHECKED);
    r->rememberPassword = (::SendMessageW(ctx->chkRemember, BM_GETCHECK, 0, 0) == BST_CHECKED);
    r->profile = q;

    // ---- 双线（备线）：勾了「启用双线」且填了备线地址才生效 ----
    ctx->dualLine = (::SendMessageW(ctx->chkDualLine, BM_GETCHECK, 0, 0) == BST_CHECKED);
    if (ctx->dualLine) {
        std::wstring bHost, bErr;
        int bPort = 0;
        if (parse_address(get_text(ctx->edBackupAddr), bHost, bPort, bErr)) {
            r->params.backupHost = narrow(bHost);
            r->params.backupPort = u16(bPort);
        } else {
            show_error(ctx, L"备线地址格式不对（例如 192.0.2.11:14740）", ctx->edBackupAddr);
            return;
        }
    } else {
        r->params.backupHost.clear();
        r->params.backupPort = 0;
    }

    r->ok = true;

    ::DestroyWindow(ctx->wnd);
}

// -----------------------------------------------------------------------------
//  托盘常驻
//
//  以前的交互：关掉连接面板 = 整个程序退出，用户下次还得重新找 exe。
//  现在（client.ini 里 minimize_to_tray=1，默认开）：
//    * 关闭窗口 -> 隐藏到托盘，进程继续活着（可以随时双击托盘再打开）
//    * 托盘左键单击/双击 -> 恢复主界面
//    * 托盘右键 -> 「打开主界面 / 退出」
//    * 想彻底退出：托盘右键选「退出」，或者连接面板上按 ESC 两次（第二次真退）
// -----------------------------------------------------------------------------
constexpr UINT kTrayCallbackMsg = WM_APP + 0x71;
constexpr UINT kTrayId          = 1;
constexpr int  IDM_TRAY_SHOW    = 1001;
constexpr int  IDM_TRAY_EXIT    = 1002;

void tray_add(DlgCtx* ctx) {
    if (!ctx || !ctx->wnd || ctx->trayAdded) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd   = ctx->wnd;
    nid.uID    = kTrayId;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = kTrayCallbackMsg;
    nid.hIcon  = ::LoadIconW(nullptr, IDI_APPLICATION);
    ::StringCchCopyW(nid.szTip, _countof(nid.szTip),
                     L"jianhx 远控端（双击打开 / 右键退出）");
    ctx->trayAdded = ::Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
    MS_INFO("托盘图标已创建（关闭窗口 = 留在后台）");
}

void tray_remove(DlgCtx* ctx) {
    if (!ctx || !ctx->wnd || !ctx->trayAdded) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd   = ctx->wnd;
    nid.uID    = kTrayId;
    ::Shell_NotifyIconW(NIM_DELETE, &nid);
    ctx->trayAdded = false;
}

void tray_restore(DlgCtx* ctx) {
    if (!ctx || !ctx->wnd) return;
    ::ShowWindow(ctx->wnd, SW_SHOW);
    ::SetForegroundWindow(ctx->wnd);
    MS_DEBUG("从托盘恢复主界面");
}

void tray_show_menu(DlgCtx* ctx) {
    if (!ctx || !ctx->wnd) return;
    HMENU menu = ::CreatePopupMenu();
    if (!menu) return;
    ::AppendMenuW(menu, MF_STRING, IDM_TRAY_SHOW, L"打开主界面");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, IDM_TRAY_EXIT, L"退出 jianhx 远控端");
    POINT pt{};
    ::GetCursorPos(&pt);
    // 必须先 SetForegroundWindow，否则点菜单外面菜单不会消失（Win32 老坑）
    ::SetForegroundWindow(ctx->wnd);
    const int cmd = ::TrackPopupMenu(menu,
                                     TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                                     pt.x, pt.y, 0, ctx->wnd, nullptr);
    ::DestroyMenu(menu);
    ::PostMessageW(ctx->wnd, WM_NULL, 0, 0);   // 同上，收尾

    if (cmd == IDM_TRAY_SHOW) {
        tray_restore(ctx);
    } else if (cmd == IDM_TRAY_EXIT) {
        MS_INFO("用户从托盘菜单退出");
        ctx->allowClose = true;
        ::DestroyWindow(ctx->wnd);
    }
}

// -----------------------------------------------------------------------------
// 窗口过程
// -----------------------------------------------------------------------------
LRESULT CALLBACK connect_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    DlgCtx* ctx = reinterpret_cast<DlgCtx*>(::GetWindowLongPtrW(h, GWLP_USERDATA));
    if (m == WM_NCCREATE) {
        const CREATESTRUCTW* cs = reinterpret_cast<const CREATESTRUCTW*>(l);
        ctx = static_cast<DlgCtx*>(cs->lpCreateParams);
        ::SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));
        if (ctx) ctx->wnd = h;
    }
    if (!ctx) return ::DefWindowProcW(h, m, w, l);

    switch (m) {
    case WM_COMMAND:
        // 下拉框选择变化：预设联动
        if (HIWORD(w) == CBN_SELENDOK) {
            const int id = int(LOWORD(w));
            if (!ctx->syncingCombos) {
                if (id == IDC_PRESET) {
                    const int sel = int(::SendMessageW(ctx->cbPreset, CB_GETCURSEL, 0, 0));
                    if (sel >= 0 && sel < quality::kPresetCount - 1) {
                        apply_preset(ctx, quality::Preset(sel));
                    }
                    // 选了「自定义」本身：不动三下拉，保留当前编辑值
                } else if (id == IDC_RES || id == IDC_FPS || id == IDC_BITRATE) {
                    // 用户手动改任何一项 -> 预设自动变「自定义」（或最近匹配）
                    sync_preset_combo(ctx);
                }
            }
            return 0;
        }
        if (HIWORD(w) == BN_CLICKED) {
            const int id = int(LOWORD(w));
            if (id == IDC_CONNECT) {
                on_ok(ctx);
            } else if (id == IDCANCEL) {
                ::DestroyWindow(h);
            } else if (id == IDC_DUALLINE) {
                // 勾了「启用双线」才显示备线地址输入框
                const bool on = (::SendMessageW(ctx->chkDualLine, BM_GETCHECK, 0, 0) == BST_CHECKED);
                ::ShowWindow(ctx->edBackupAddr, on ? SW_SHOW : SW_HIDE);
                ctx->dualLine = on;
            } else if (id == IDC_LAST) {
                // 点「上次连接」静态文本 = 填入输入框并聚焦
                ::SetFocus(ctx->edAddr);
            } else if (id == IDC_FWBTN) {
                // 防火墙一键放行：需要管理员；netsh 会自己弹 UAC 授权框
                std::string why;
                if (fw::add_exemption(why)) {
                    set_text(ctx->stFw, widen(fw::status_text()));
                    set_text(ctx->btnFw, L"防火墙已放行 ✔");
                    // 同时在错误行提示一次，确认操作生效
                    set_text(ctx->stErr, L"✔ 防火墙已放行，可以重新连接了");
                } else {
                    std::wstring werr = widen(why);
                    if (werr.empty()) werr = L"放行失败（需要管理员权限：右键以管理员运行，或在弹出的 UAC 框里点「是」）";
                    set_text(ctx->stErr, L"放行失败: " + werr);
                }
            }
        }
        if (HIWORD(w) == STN_CLICKED && int(LOWORD(w)) == IDC_LAST) {
            ::SetFocus(ctx->edAddr);
        }
        return 0;
    case WM_CLOSE:
        // 关闭窗口 = 最小化到托盘（用户要求：用完不能直接退出，要留存在后台）。
        // 只有托盘菜单「退出」或 minimize_to_tray=0 时才真退。
        if (ctx->minimizeToTray && !ctx->allowClose) {
            ::ShowWindow(h, SW_HIDE);
            // 第一次隐藏给个气泡提示，否则用户会以为程序被关掉了
            static bool hinted = false;
            if (!hinted) {
                hinted = true;
                NOTIFYICONDATAW nid{};
                nid.cbSize = sizeof(nid);
                nid.hWnd   = h;
                nid.uID    = kTrayId;
                nid.uFlags = NIF_INFO;
                nid.dwInfoFlags = NIIF_INFO;
                ::StringCchCopyW(nid.szInfoTitle, _countof(nid.szInfoTitle),
                                 L"jianhx 远控端仍在后台运行");
                ::StringCchCopyW(nid.szInfo, _countof(nid.szInfo),
                                 L"双击托盘图标可重新打开；右键托盘图标可退出。");
                ::Shell_NotifyIconW(NIM_MODIFY, &nid);
            }
            MS_INFO("窗口已最小化到托盘（进程保持运行）");
            return 0;
        }
        ::DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        tray_remove(ctx);
        // 模态循环靠这条消息退出
        ::PostQuitMessage(0);
        return 0;
    case kTrayCallbackMsg:
        switch (LOWORD(l)) {
        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK:
            tray_restore(ctx);
            break;
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:
            tray_show_menu(ctx);
            break;
        default:
            break;
        }
        return 0;
    case WM_CTLCOLORSTATIC: {
        const HWND ctl = reinterpret_cast<HWND>(l);
        HDC dc = reinterpret_cast<HDC>(w);
        if (ctl == ctx->stErr) {
            ::SetBkMode(dc, TRANSPARENT);
            ::SetTextColor(dc, RGB(200, 40, 40));   // 就地红字错误
            return reinterpret_cast<LRESULT>(::GetSysColorBrush(COLOR_BTNFACE));
        }
        if (ctl == ctx->stLast) {
            ::SetBkMode(dc, TRANSPARENT);
            ::SetTextColor(dc, ::GetSysColor(COLOR_HOTLIGHT));  // 链接蓝
            return reinterpret_cast<LRESULT>(::GetSysColorBrush(COLOR_BTNFACE));
        }
        break;
    }
    default:
        break;
    }
    return ::DefWindowProcW(h, m, w, l);
}

} // namespace

bool ConnectUi::show(HINSTANCE hinst, const ClientConfig& initial,
                     ConnectDialogResult& out, const std::string& priorError) {
    out = ConnectDialogResult{};
    out.fullscreen     = initial.fullscreen;
    out.overlayEnabled = initial.overlayEnabled;

    DlgCtx ctx;
    ctx.hinst   = hinst ? hinst : ::GetModuleHandleW(nullptr);
    ctx.initial = &initial;
    ctx.out     = &out;

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = connect_proc;
    wc.hInstance     = ctx.hinst;
    wc.hCursor       = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = ::GetSysColorBrush(COLOR_BTNFACE);
    wc.lpszClassName = kDlgClass;
    wc.hIcon         = ::LoadIconW(nullptr, IDI_APPLICATION);
    wc.hIconSm       = wc.hIcon;
    if (!::RegisterClassExW(&wc) && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    const DWORD style   = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    const DWORD exStyle = WS_EX_CONTROLPARENT;   // 让 IsDialogMessage 能处理 Tab
    RECT rc{0, 0, kClientW, kClientH};
    ::AdjustWindowRectEx(&rc, style, FALSE, exStyle);
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    // 手工居中（不依赖 CW_USEDEFAULT）
    const int x0 = (::GetSystemMetrics(SM_CXSCREEN) - w) / 2;
    const int y0 = (::GetSystemMetrics(SM_CYSCREEN) - h) / 2;

    ctx.wnd = ::CreateWindowExW(exStyle, kDlgClass, kDlgTitle, style,
                                x0, y0, w, h, nullptr, nullptr, ctx.hinst, &ctx);
    if (!ctx.wnd) return false;

    // --- 字体 -------------------------------------------------------------------------
    ctx.font      = create_point_font(9, false);   // 常规 UI
    ctx.fontBig   = create_point_font(14, false);  // 大输入框 / 大按钮（按钮用粗体另建）
    ctx.fontBrand = create_point_font(28, true);   // 应用名
    HFONT fontBtnBig = create_point_font(14, true);
    bool fontBtnBigOwned = (fontBtnBig != nullptr);
    if (!ctx.font)      ctx.font      = reinterpret_cast<HFONT>(::GetStockObject(DEFAULT_GUI_FONT));
    if (!ctx.fontBig)   ctx.fontBig   = ctx.font;
    if (!ctx.fontBrand) ctx.fontBrand = ctx.font;
    if (!fontBtnBig)    fontBtnBig    = ctx.fontBig;
    ctx.fontsOwned = true;

    auto mk = [&](const wchar_t* cls, const wchar_t* text, DWORD cstyle, DWORD exstyle,
                  HFONT f, int cx, int cy, int cw, int ch, int id) -> HWND {
        HWND c = ::CreateWindowExW(exstyle, cls, text, WS_CHILD | WS_VISIBLE | cstyle,
                                   cx, cy, cw, ch, ctx.wnd,
                                   reinterpret_cast<HMENU>(INT_PTR(id)), ctx.hinst, nullptr);
        if (c && f) ::SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(f), TRUE);
        return c;
    };
    auto mkc = [&](const wchar_t* cls, const wchar_t* text, DWORD cstyle, int cx, int cy,
                   int cw, int ch, int id) -> HWND {
        return mk(cls, text, cstyle, 0, ctx.font, cx, cy, cw, ch, id);
    };

    const int mx = kMarginX;             // 页左边距
    const int ix = mx + kGrpPadX;        // 组内左边距
    const int iw = kConnInW;             // 组内通栏宽

    // --- 顶部标识区 --------------------------------------------------------------------
    mk(L"STATIC", L"jianhx", SS_LEFT, 0, ctx.fontBrand, mx, kBrandY, kContentW, kBrandH, -1);
    mk(L"STATIC", L"低延迟游戏串流", SS_LEFT, 0, ctx.font, mx, kSubY, kContentW, kSubH, -1);

    // --- 「连接」分组 ------------------------------------------------------------------
    mkc(L"BUTTON", L"连接", BS_GROUPBOX, mx, kGrpConnY, kContentW, kGrpConnH, -1);

    int y = kGrpConnY + kGrpPadTop;

    // 大输入框：IP / IP:端口 / 域名 / 域名:端口 一框搞定
    ctx.edAddr = mk(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE,
                    ctx.fontBig, ix, y, iw, kBigEditH, IDC_ADDR);
    ::SendMessageW(ctx.edAddr, EM_SETCUEBANNER, TRUE,
                   reinterpret_cast<LPARAM>(L"输入被控端 IP 或域名，如 1.2.3.4 或 game.example.com:20000"));
    y += kBigEditH + kRowGap;

    // 密码框
    ctx.edPwd = mk(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL | ES_PASSWORD, WS_EX_CLIENTEDGE,
                   ctx.font, ix, y, iw, kRowH, IDC_PWD);
    ::SendMessageW(ctx.edPwd, EM_SETCUEBANNER, TRUE,
                   reinterpret_cast<LPARAM>(L"被控端设置的访问密码"));
    y += kRowH + kRowGap / 2;

    // 记住验证码 + 「上次连接」快捷
    ctx.chkRemember = mkc(L"BUTTON", L"记住验证码", WS_TABSTOP | BS_AUTOCHECKBOX,
                          ix, y, 140, kCheckH, IDC_REMEMBER);
    if (!initial.host.empty()) {
        const std::wstring last = L"上次: " + widen(initial.host) + L":" +
                                  std::to_wstring(int(initial.port));
        ctx.stLast = mkc(L"STATIC", last.c_str(), SS_NOTIFY | SS_LEFT,
                         ix + iw - 240, y, 240, kCheckH, IDC_LAST);
    }
    y += kCheckH + kRowGap;

    // 错误提示区（组内底部，红字就地显示）
    ctx.stErr = mk(L"STATIC", L"", SS_LEFT, 0, ctx.font,
                   ix, y, iw, kErrH, IDC_ERR);
    // 上一次连接失败的原因：打开面板就让它可见（失败->改参数->重连的循环）
    if (!priorError.empty()) {
        set_text(ctx.stErr, widen(priorError));
    }

    // --- 「画质」分组 ------------------------------------------------------------------
    mkc(L"BUTTON", L"画质", BS_GROUPBOX, mx, kGrpQualY, kContentW, kGrpQualH, -1);

    y = kGrpQualY + kGrpPadTop;
    // 第一行：预设 + 分辨率
    mkc(L"STATIC", L"预设:", SS_RIGHT, ix, y + 3, kLabelW, kComboH, -1);
    ctx.cbPreset = mkc(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                       ix + kLabelW + 6, y, 140, kComboListH, IDC_PRESET);
    mkc(L"STATIC", L"分辨率:", SS_RIGHT, ix + 240, y + 3, kLabelW + 12, kComboH, -1);
    ctx.cbRes = mkc(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                    ix + 240 + kLabelW + 18, y, 160, kComboListH, IDC_RES);
    y += kComboH + kRowGap;
    // 第二行：帧率 + 码率
    mkc(L"STATIC", L"帧率:", SS_RIGHT, ix, y + 3, kLabelW, kComboH, -1);
    ctx.cbFps = mkc(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                    ix + kLabelW + 6, y, 140, kComboListH, IDC_FPS);
    mkc(L"STATIC", L"码率:", SS_RIGHT, ix + 240, y + 3, kLabelW + 12, kComboH, -1);
    ctx.cbBitrate = mkc(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                        ix + 240 + kLabelW + 18, y, 160, kComboListH, IDC_BITRATE);

    // 预设下拉：前三个档 + 「自定义」（选项数据存 Preset 值）
    for (int i = 0; i < quality::kPresetCount; ++i) {
        const quality::Preset p = quality::Preset(i);
        const int idx = int(::SendMessageW(ctx.cbPreset, CB_ADDSTRING, 0,
                           reinterpret_cast<LPARAM>(widen(quality::preset_name(p)).c_str())));
        if (idx >= 0) ::SendMessageW(ctx.cbPreset, CB_SETITEMDATA, WPARAM(idx), LPARAM(i));
    }

    // 分辨率下拉（数据存行号，行号即 resolution_options 索引）
    {
        const auto& opts = quality::resolution_options();
        for (size_t i = 0; i < opts.size(); ++i) {
            const int idx = int(::SendMessageW(ctx.cbRes, CB_ADDSTRING, 0,
                                               reinterpret_cast<LPARAM>(opts[i].label)));
            if (idx >= 0) ::SendMessageW(ctx.cbRes, CB_SETITEMDATA, WPARAM(idx), LPARAM(int(i)));
        }
    }

    // 帧率下拉
    for (int v : quality::fps_options()) {
        wchar_t buf[16] = {0};
        ::StringCchPrintfW(buf, _countof(buf), L"%d", v);
        const int idx = int(::SendMessageW(ctx.cbFps, CB_ADDSTRING, 0,
                                           reinterpret_cast<LPARAM>(buf)));
        if (idx >= 0) ::SendMessageW(ctx.cbFps, CB_SETITEMDATA, WPARAM(idx), LPARAM(v));
    }

    // 码率下拉：0 显示「自适应」，其它显示 "8 Mbps" 样式
    for (int v : quality::bitrate_options()) {
        wchar_t buf[24] = {0};
        if (v == 0) ::StringCchCopyW(buf, _countof(buf), L"自适应");
        else        ::StringCchPrintfW(buf, _countof(buf), L"%d Mbps", v / 1000);
        const int idx = int(::SendMessageW(ctx.cbBitrate, CB_ADDSTRING, 0,
                                           reinterpret_cast<LPARAM>(buf)));
        if (idx >= 0) ::SendMessageW(ctx.cbBitrate, CB_SETITEMDATA, WPARAM(idx), LPARAM(v));
    }

    // --- 「选项」分组 ------------------------------------------------------------------
    mkc(L"BUTTON", L"选项", BS_GROUPBOX, mx, kGrpOptY, kContentW, kGrpOptH + 30, -1);

    y = kGrpOptY + kGrpPadTop;
    ctx.chkFullscreen = mkc(L"BUTTON", L"连接后全屏", WS_TABSTOP | BS_AUTOCHECKBOX,
                            ix, y, iw / 2 - 8, kCheckH, IDC_FULLSCREEN);
    ctx.chkOverlay = mkc(L"BUTTON", L"显示悬浮状态条", WS_TABSTOP | BS_AUTOCHECKBOX,
                         ix + iw / 2 + 8, y, iw / 2 - 8, kCheckH, IDC_OVERLAY);
    y += kCheckH + kRowGap;
    ctx.stHotkey = mkc(L"STATIC", L"", SS_LEFT, ix, y, iw, kCheckH, -1);
    set_text(ctx.stHotkey, L"释放键鼠快捷键 = " + widen(initial.releaseHotkey));
    y += kCheckH + kRowGap;

    // 双线：备线地址输入框（勾了复选框才显示）
    ctx.chkDualLine = mkc(L"BUTTON", L"启用双线（键鼠走备线，主线全留给视频）",
                          WS_TABSTOP | BS_AUTOCHECKBOX, ix, y, iw, kCheckH, IDC_DUALLINE);
    y += kCheckH + 4;
    ctx.edBackupAddr = mk(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE,
                          ctx.font, ix, y, iw, kRowH, IDC_BACKUP);
    ::SendMessageW(ctx.edBackupAddr, EM_SETCUEBANNER, TRUE,
                   reinterpret_cast<LPARAM>(L"备线地址（例如 192.0.2.11:14740），留空 = 不用双线"));
    // 默认隐藏，勾了才显示
    ::ShowWindow(ctx.edBackupAddr, SW_HIDE);
    y += kRowH + kRowGap / 2;

    // --- 防火墙放行（状态 + 按钮） ------------------------------------------------------
    // 客户端 UDP 收视频用随机临时端口，防火墙必须按「程序」放行一条规则，
    // 否则内网连接会"已连接但黑屏几秒后被踢"（UDP 入站全被拦）。
    ctx.stFw = mkc(L"STATIC", L"", SS_LEFT, mx, kFwY, iw - 262, kFwH, IDC_FWSTAT);
    set_text(ctx.stFw, widen(fw::status_text()));
    ctx.btnFw = mkc(L"BUTTON", L"防火墙已放行？点此放行", WS_TABSTOP | BS_PUSHBUTTON,
                    mx + kContentW - 250, kFwY - 1, 250, kFwH + 2, IDC_FWBTN);
    if (fw::is_exempt()) set_text(ctx.btnFw, L"防火墙已放行 ✔（点击可重新放行）");

    // --- 底部：错误提示（全局备用位）+ 通栏大按钮 ---------------------------------------
    // 错误文字放在「连接」组内，这里不再重复；全局面积留给按钮
    ctx.btnOk = mk(L"BUTTON", L"连  接", WS_TABSTOP | BS_DEFPUSHBUTTON, 0,
                   fontBtnBig, mx, kBtnY, kContentW, kBtnH, IDC_CONNECT);

    // --- 回填上次配置 ------------------------------------------------------------------
    // 地址：host 非空则拼 host:port（端口非默认时才带）
    if (!initial.host.empty()) {
        std::wstring addr = widen(initial.host);
        if (int(initial.port) != int(defaults::kPort)) {
            addr += L":" + std::to_wstring(int(initial.port));
        }
        set_text(ctx.edAddr, addr);
    }
    set_text(ctx.edPwd, widen(initial.password));
    ::SendMessageW(ctx.chkRemember, BM_SETCHECK,
                   initial.password.empty() ? WPARAM(BST_UNCHECKED) : WPARAM(BST_CHECKED), 0);
    ::SendMessageW(ctx.chkFullscreen, BM_SETCHECK,
                   initial.fullscreen ? WPARAM(BST_CHECKED) : WPARAM(BST_UNCHECKED), 0);
    ::SendMessageW(ctx.chkOverlay, BM_SETCHECK,
                   initial.overlayEnabled ? WPARAM(BST_CHECKED) : WPARAM(BST_UNCHECKED), 0);
    // 双线：ini 里有 backup_host 就勾上并显示备线输入框
    if (!initial.backupHost.empty() && initial.backupPort != 0) {
        ::SendMessageW(ctx.chkDualLine, BM_SETCHECK, WPARAM(BST_CHECKED), 0);
        ::ShowWindow(ctx.edBackupAddr, SW_SHOW);
        ctx.dualLine = true;
        std::wstring bAddr = widen(initial.backupHost);
        if (int(initial.backupPort) != int(defaults::kPort)) {
            bAddr += L":" + std::to_wstring(int(initial.backupPort));
        }
        set_text(ctx.edBackupAddr, bAddr);
    }

    // 画质：先用 ini 的值定位三个下拉；定位不了任何一项则退回均衡预设
    {
        int selRes = combo_find_data_w_h(ctx.cbRes, initial.width, initial.height);
        int selFps = combo_find_data(ctx.cbFps, initial.fps);
        int selBr  = combo_find_data(ctx.cbBitrate, initial.bitrateKbps);
        if (selRes < 0 || selFps < 0 || selBr < 0) {
            apply_preset(&ctx, quality::Preset::Balanced);
        } else {
            ctx.syncingCombos = true;
            ::SendMessageW(ctx.cbRes,     CB_SETCURSEL, WPARAM(selRes), 0);
            ::SendMessageW(ctx.cbFps,     CB_SETCURSEL, WPARAM(selFps), 0);
            ::SendMessageW(ctx.cbBitrate, CB_SETCURSEL, WPARAM(selBr),  0);
            ctx.syncingCombos = false;
            sync_preset_combo(&ctx);   // ini 值恰好匹配某预设就选它，否则「自定义」
        }
    }

    ::ShowWindow(ctx.wnd, SW_SHOW);
    ::UpdateWindow(ctx.wnd);
    // 托盘常驻：关闭窗口只隐藏，进程留在后台（client.ini: minimize_to_tray）
    ctx.minimizeToTray = initial.minimizeToTray;
    tray_add(&ctx);
    if (initial.host.empty())          ::SetFocus(ctx.edAddr);
    else if (initial.password.empty()) ::SetFocus(ctx.edPwd);
    else                               ::SetFocus(ctx.btnOk);

    // --- 模态循环 -----------------------------------------------------------
    MSG msg;
    while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
        // Tab / 方向键切焦点：普通窗口也能用，只要能提供 WS_EX_CONTROLPARENT
        if (::IsDialogMessageW(ctx.wnd, &msg)) continue;

        // 普通窗口没有对话框过程，IsDialogMessage 不一定认默认按钮，这里兜底
        if (msg.message == WM_KEYDOWN && (msg.wParam == VK_RETURN || msg.wParam == VK_ESCAPE)) {
            HWND focus = ::GetFocus();
            if (focus && ::GetAncestor(focus, GA_ROOT) == ctx.wnd) {
                if (msg.wParam == VK_RETURN) on_ok(&ctx);
                // ESC 也走 WM_CLOSE：开了托盘常驻时只是隐藏，不是退出
                else                         ::PostMessageW(ctx.wnd, WM_CLOSE, 0, 0);
                continue;
            }
        }
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }

    if (ctx.fontsOwned) {
        if (ctx.font)      ::DeleteObject(ctx.font);
        if (ctx.fontBig)   ::DeleteObject(ctx.fontBig);
        if (ctx.fontBrand) ::DeleteObject(ctx.fontBrand);
        if (fontBtnBigOwned && fontBtnBig) ::DeleteObject(fontBtnBig);
    }
    return out.ok;
}

} // namespace ms::client
