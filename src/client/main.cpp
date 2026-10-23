// =============================================================================
//  main.cpp - 远控端入口
//
//  流程：连接界面 -> 建渲染窗口 -> 全屏/窗口化接收画面 + 捕获键鼠
//  支持 --auto 直接连接（用于自动化和实测，跳过界面）。
// =============================================================================
#include "client_session.h"
#include "connect_ui.h"
#include "firewall.h"

#include "../common/config.h"
#include "../common/crash.h"
#include "../common/log.h"
#include "../common/types.h"

#include <windows.h>
#include <shellapi.h>   // CommandLineToArgvW
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

using namespace ms;
using namespace ms::client;

// 是否隐藏本机系统光标（正在画远端光标时）。client_session.cpp 会写它。
// 为什么要藏本机的：远端光标是画在视频上的，本机光标再亮一个就会"两个鼠标"。
std::atomic<bool> g_hide_local_cursor{false};

// 双显卡机器上告诉驱动「用独显跑」——解码器是 D3D11，核显解码会让 DXVA
// 落到慢路径；参考 UU远程/GameViewer 的同名导出（逆向第 2 轮 §8）。
extern "C" {
__declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
__declspec(dllexport) DWORD AmdPowerXpressRequestHighPerformance = 0x00000001;
}

namespace {

struct Args {
    std::string config;
    std::string host;
    std::string password;
    int  port = -1;
    int  fps = -1;
    int  bitrate = -1;
    int  logLevel = -1;
    int  fullscreen = -1;
    int  overlay = -1;
    int  seconds = 0;
    bool autoConnect = false;
    bool fwAllow = false;      // --fw-allow：UAC 提权子进程只做防火墙放行
    bool console = false;
    // 命令行是否明确给了这三项（区别于"从 ini 里读到的旧值"）：
    // 只有命令行给的才允许跳过连接面板自动连
    bool hostGiven = false;
    bool portGiven = false;
    bool passwordGiven = false;
};

std::string narrow(const wchar_t* w) {
    if (!w) return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n > 0 ? n - 1 : 0), '\0');
    if (n > 1) ::WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(size_t(n > 0 ? n - 1 : 0), L'\0');
    if (n > 1) ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}

std::string exe_dir() {
    wchar_t buf[MAX_PATH] = {0};
    ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring w(buf);
    const size_t p = w.find_last_of(L"\\/");
    if (p != std::wstring::npos) w.resize(p);
    return narrow(w.c_str());
}

Args parse_args() {
    Args a;
    int argc = 0;
    LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
    if (!argv) return a;
    auto next = [&](int& i) -> std::string {
        if (i + 1 < argc) return narrow(argv[++i]);
        return {};
    };
    for (int i = 1; i < argc; ++i) {
        const std::string k = narrow(argv[i]);
        if (k == "--config")        a.config   = next(i);
        else if (k == "--host")    { a.host     = next(i); a.hostGiven = true; }
        else if (k == "--password"){ a.password = next(i); a.passwordGiven = true; }
        else if (k == "--port")    { a.port     = std::atoi(next(i).c_str()); a.portGiven = true; }
        else if (k == "--fps")      a.fps      = std::atoi(next(i).c_str());
        else if (k == "--bitrate")  a.bitrate  = std::atoi(next(i).c_str());
        else if (k == "--log-level")a.logLevel = std::atoi(next(i).c_str());
        else if (k == "--fullscreen") a.fullscreen = std::atoi(next(i).c_str());
        else if (k == "--overlay")  a.overlay  = std::atoi(next(i).c_str());
        else if (k == "--seconds")  a.seconds  = std::atoi(next(i).c_str());
        else if (k == "--auto")     a.autoConnect = true;
        else if (k == "--fw-allow") a.fwAllow = true;
        else if (k == "--console")  a.console  = true;
    }
    ::LocalFree(argv);
    return a;
}

void attach_console() {
    if (!::AttachConsole(ATTACH_PARENT_PROCESS)) ::AllocConsole();
    FILE* f = nullptr;
    freopen_s(&f, "CONOUT$", "w", stdout);
    freopen_s(&f, "CONOUT$", "w", stderr);
    freopen_s(&f, "CONIN$", "r", stdin);
    ::SetConsoleOutputCP(CP_UTF8);
}

ClientSession* g_session = nullptr;
bool          g_quit = false;
bool          g_fullscreen = false;
RECT          g_windowedRect{};

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CLOSE:
            g_quit = true;
            if (g_session) g_session->request_stop();
            return 0;

        case WM_DESTROY:
            ::PostQuitMessage(0);
            return 0;

        case WM_SIZE:
            if (g_session) {
                // 渲染器自己在 present 时读取 backbuffer 大小，这里只记录
            }
            return 0;

        case WM_ERASEBKGND:
            return 1;   // 不闪白

        case WM_SETCURSOR:
            // 正在画远端光标时，本机光标不显示（否则视频上会叠两个鼠标）。
            // 只在客户区生效：拖边框/工具栏按钮时仍然用系统光标。
            if (LOWORD(lp) == HTCLIENT &&
                g_hide_local_cursor.load(std::memory_order_relaxed)) {
                ::SetCursor(nullptr);
                return TRUE;
            }
            break;

        case WM_SYSCOMMAND:
            // 全屏时屏蔽屏保/待机的快捷键干扰
            if ((wp & 0xFFF0) == SC_SCREENSAVE || (wp & 0xFFF0) == SC_MONITORPOWER) return 0;
            break;

        default:
            break;
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

HWND create_render_window(HINSTANCE hinst, bool fullscreen, const wchar_t* title, std::string& err) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = hinst;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"MiniStreamRenderWnd";
    if (!::RegisterClassExW(&wc)) {
        // 已注册过（重连场景）不算错误
        const DWORD e = ::GetLastError();
        if (e != ERROR_CLASS_ALREADY_EXISTS) {
            err = "注册窗口类失败（错误码 " + std::to_string(e) + "）";
            MS_ERR("%s", err.c_str());
            return nullptr;
        }
    }

    int w = 1280, h = 720;
    // 【关键】WS_CLIPCHILDREN：渲染窗口有子窗口（顶部工具栏）。
    // D3D11 swapchain 每帧 Present 会重写整个客户区，如果父窗口不裁剪子窗口，
    // 工具栏会被视频帧完全盖住，表现为"show(true) 调用成功但看不到"。
    DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT;

    if (fullscreen) {
        style = WS_POPUP | WS_CLIPCHILDREN;
        HMONITOR mon = ::MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        ::GetMonitorInfoW(mon, &mi);
        x = mi.rcMonitor.left;
        y = mi.rcMonitor.top;
        w = mi.rcMonitor.right - mi.rcMonitor.left;
        h = mi.rcMonitor.bottom - mi.rcMonitor.top;
    }

    HWND hwnd = ::CreateWindowExW(
        fullscreen ? WS_EX_TOPMOST : 0, L"MiniStreamRenderWnd", title, style,
        x, y, w, h, nullptr, nullptr, hinst, nullptr);
    if (!hwnd) {
        const DWORD e = ::GetLastError();
        err = "创建渲染窗口失败（错误码 " + std::to_string(e) + "）";
        MS_ERR("%s", err.c_str());
        return nullptr;
    }

    g_windowedRect.left = x; g_windowedRect.top = y;
    g_windowedRect.right = x + w; g_windowedRect.bottom = y + h;

    ::ShowWindow(hwnd, SW_SHOW);
    ::UpdateWindow(hwnd);
    ::SetFocus(hwnd);
    return hwnd;
}

// 会话结束后把窗口恢复成普通窗口，避免用户看到全屏黑屏
void restore_window(HWND hwnd) {
    if (g_fullscreen) {
        ::SetWindowLongPtrW(hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW);
        ::SetWindowPos(hwnd, HWND_NOTOPMOST, 100, 100, 1280, 720, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    }
}

} // namespace

int WINAPI wWinMain(HINSTANCE hinst, HINSTANCE, LPWSTR, int) {
    // 【先设 DPI 感知，再干别的】否则在开了 125%/150% 缩放的机器上，
    // Windows 把我们当成不感知 DPI 的进程做位图放大：窗口坐标是逻辑像素，
    // 而 DXGI/D3D11 拿的是物理像素，两套坐标一错位，鼠标点就偏到左上角，
    // 画面也像是"只看到远端桌面的左上角一块"。
    // Per Monitor V2 是 Windows 10 1703+ 的标准做法，失败再退 Per Monitor V1。
    {
        HMODULE user32 = ::GetModuleHandleW(L"user32.dll");
        if (user32) {
            typedef BOOL (WINAPI *SetProcDpiCtx_t)(DWORD);
            typedef BOOL (WINAPI *SetProcDpi_t)(void*);
            // PROCESS_PER_MONITOR_DPI_AWARE_V2 = (DPI_AWARENESS_CONTEXT)-4
            if (auto fn = reinterpret_cast<SetProcDpiCtx_t>(
                    ::GetProcAddress(user32, "SetProcessDpiAwarenessContext"))) {
                fn(DWORD(-4));  // PROCESS_PER_MONITOR_DPI_AWARE_V2
            } else if (auto fn2 = reinterpret_cast<SetProcDpi_t>(
                           ::GetProcAddress(user32, "SetProcessDpiAwareness"))) {
                // PROCESS_PER_MONITOR_DPI_AWARE = 2
                HMODULE shcore = ::GetModuleHandleW(L"Shcore.dll");
                if (shcore) {
                    typedef HRESULT (WINAPI *SetProcDpiAwareness_t)(int);
                    if (auto fn3 = reinterpret_cast<SetProcDpiAwareness_t>(
                            ::GetProcAddress(shcore, "SetProcessDpiAwareness"))) {
                        fn3(2);  // PROCESS_PER_MONITOR_DPI_AWARE
                    }
                }
            }
        }
    }

    const Args args = parse_args();

    // ---- 快捷路径：--fw-allow 只做防火墙放行（用于 UAC 提权的子进程）----
    if (args.fwAllow) {
        // 无窗口、不建控制台；加完规则就退。必须够快，用户能看到 UAC 框一闪而过。
        std::string why;
        const bool ok = fw::add_exemption(why);
        if (ok) {
            ::MessageBoxW(nullptr,
                          L"jianhx 客户端已放行防火墙入站 UDP/TCP。\n\n"
                          L"现在可以正常连接了。",
                          L"jianhx", MB_ICONINFORMATION | MB_OK);
            return 0;
        } else {
            std::wstring w = widen(why);
            ::MessageBoxW(nullptr, (L"放行失败：" + w + L"\n\n请尝试右键以管理员身份运行本程序。").c_str(),
                          L"jianhx", MB_ICONERROR | MB_OK);
            return 1;
        }
    }

    if (args.console) attach_console();

    const std::string dir = exe_dir();
    const std::string cfgPath = args.config.empty() ? (dir + "\\client.ini") : args.config;

    ClientConfig cfg;
    std::string err;
    if (!ClientConfig::load(cfgPath, cfg, err)) {
        MS_WARN("%s", err.c_str());
        cfg.configPath = cfgPath;
        std::string serr;
        if (cfg.save(serr)) MS_INFO("已生成默认配置: %s", cfgPath.c_str());
    }
    cfg.configPath = cfgPath;

    if (!args.host.empty())     cfg.host = args.host;
    if (args.port > 0)          cfg.port = u16(args.port);
    if (!args.password.empty()) cfg.password = args.password;
    if (args.fps > 0)           cfg.fps = args.fps;
    if (args.bitrate >= 0)      cfg.bitrateKbps = args.bitrate;
    if (args.fullscreen >= 0)   cfg.fullscreen = (args.fullscreen != 0);
    if (args.overlay >= 0)      cfg.overlayEnabled = (args.overlay != 0);
    if (args.logLevel >= 0)     cfg.logLevel = args.logLevel;

    // 排障/自测用：MINISTREAM_MOUSE_MODE=auto|absolute|relative（覆盖 ini）
    {
        wchar_t v[16]{};
        if (::GetEnvironmentVariableW(L"MINISTREAM_MOUSE_MODE", v, 16) > 0) {
            if (v[0] == L'1' || v[0] == L'a' || v[0] == L'A') cfg.mouseMode = 1;
            else if (v[0] == L'2' || v[0] == L'r' || v[0] == L'R') cfg.mouseMode = 2;
            else cfg.mouseMode = 0;
            MS_INFO("鼠标模式（环境变量覆盖）-> %d", cfg.mouseMode);
        }
    }
    // 自测/排障用：MINISTREAM_AUDIO=0 关掉声音传输与播放
    {
        wchar_t v[8]{};
        if (::GetEnvironmentVariableW(L"MINISTREAM_AUDIO", v, 8) > 0) {
            cfg.audioEnabled = (v[0] != L'0');
            MS_INFO("声音（环境变量覆盖）-> %s", cfg.audioEnabled ? "开" : "关");
        }
    }
    // 自测用：MINISTREAM_SHARPEN=0..3 覆盖锐化档位（0=关 1=低 2=中 3=高）
    {
        wchar_t v[8]{};
        if (::GetEnvironmentVariableW(L"MINISTREAM_SHARPEN", v, 8) > 0) {
            const int lv = _wtoi(v);
            cfg.sharpen = lv < 0 ? 0 : (lv > 3 ? 3 : lv);
            MS_INFO("锐化档位（环境变量覆盖）-> %d", cfg.sharpen);
        }
    }

    log_set_level(static_cast<LogLevel>(cfg.logLevel < 0 ? 2 : (cfg.logLevel > 4 ? 4 : cfg.logLevel)));
    log_set_file(dir + "\\client.log");
    // 只有脚本模式才回显到控制台：GUI 模式下 stderr / OutputDebugString 都可能
    // 被外部工具卡住（详见 log.cpp 里的说明），那会冻死整个进程。
    log_set_console(args.autoConnect);

    // 崩溃自诊断：远控端闪退时也会留下 crash.log + minidump 调用栈
    crash::install("client");
    MS_INFO("崩溃自诊断已就绪：崩溃时写 %s 与 crash_client_<pid>.dmp", crash::log_path());

    // 【关键】不管后续 UI 是否成功，先写一条"启动"，这样客户端有没有活过一秒都有据可查
    MS_INFO("jianhx 客户端启动, 配置 %s, 可写日志 %s", cfgPath.c_str(), cfgPath.c_str());
    MS_INFO("环境: 调试器=%d 控制台日志=%d 管理员=%d",
            int(::IsDebuggerPresent()), int(log_get_console()), int(fw::running_as_admin()));

    // 防火墙一键放行（用户明确说"我会用管理员权限运行"）：
    // 客户端收视频用随机临时 UDP 端口，必须按程序放行，否则内网"已连接但黑屏"。
    if (!args.autoConnect) {
        if (fw::running_as_admin()) {
            if (fw::is_exempt()) {
                MS_INFO("防火墙：已放行（jianhx 客户端放行规则存在）");
            } else {
                std::string why;
                if (fw::add_exemption(why)) {
                    MS_INFO("防火墙：检测到管理员权限，已自动放行 UDP/TCP 入站");
                } else {
                    MS_WARN("防火墙：自动放行失败: %s（可在连接面板点「防火墙已放行？点此放行」重试）",
                            why.c_str());
                }
            }
        } else {
            // 未提权：不替用户做决定，但把状态说清楚
            MS_WARN("防火墙：当前不是管理员。内网连接若黑屏，请在连接面板点"
                    "「防火墙已放行？点此放行」，或右键以管理员运行一次本程序。");
        }
    }

    ConnectParams params;
    params.host = cfg.host;
    params.port = cfg.port;
    params.password = cfg.password;
    params.fps = cfg.fps;
    params.bitrateKbps = cfg.bitrateKbps;
    params.width = cfg.width;
    params.height = cfg.height;
    params.clientName = "jianhx-Client";
    // 双线：备用线路（键鼠走备线，主线留给视频）
    params.backupHost = cfg.backupHost;
    params.backupPort = cfg.backupPort;

    // ---- 连接循环：失败或断开 -> 回连接面板，错误就地显示 ----------------
    // 【修复的 bug】以前：会话一断就退出整个程序（"闪退"）。现在：回到连接面板，
    // 把断开原因红字显示在面板上（客户端那边能看到），用户可以改参数或重连。
    std::string connectError;
    bool fullscreen = cfg.fullscreen;

    if (!args.autoConnect) {
        // 只有**命令行**明确给了 --host/--port/--password 才自动连一次
        // （自动化测试用）。双击启动时一律先显示连接面板：
        // 之前这里误用了"配置文件里存的地址"来判断，结果用户双击打开
        // 就直接开始连 ini 里残留的旧地址，连改参数的机会都没有。
        const bool autoOnce = args.hostGiven && args.portGiven && args.passwordGiven;
        bool autoTried = false;   // 只自动连一次，失败就停在面板上
        // ---- 双线容灾（主线路 + 备用 FRP）----
        // 会话失败（连不上/中途掉线）时，自动换另一条线路重试，最多 3 次，
        // 全部失败才回到连接面板显示原因。cfg.auto_reconnect=0 可关掉。
        int  autoRetriesLeft = cfg.autoReconnect > 0 ? 3 : 0;
        bool retryAuto = false;   // 下一轮直接重连（不弹面板）
        while (true) {
            ConnectDialogResult res;
            if (retryAuto && autoRetriesLeft >= 0) {
                retryAuto = false;
                res.ok = true;
                res.params = params;
                res.fullscreen = fullscreen;
                res.overlayEnabled = cfg.overlayEnabled;
                res.rememberPassword = true;
            } else if (autoOnce && !autoTried) {
                autoTried = true;
                res.ok = true;
                res.params = params;
                res.fullscreen = fullscreen;
                res.overlayEnabled = cfg.overlayEnabled;
                res.rememberPassword = true;
            } else {
                if (!ConnectUi::show(hinst, cfg, res, connectError)) {
                    MS_INFO("用户取消连接，退出");
                    return 0;
                }
            }
            params = res.params;
            fullscreen = res.fullscreen;
            if (res.overlayEnabled != cfg.overlayEnabled) {
                cfg.overlayEnabled = res.overlayEnabled;
            }
            cfg.host = params.host;
            cfg.port = params.port;
            cfg.fullscreen = fullscreen;
            cfg.preset = int(res.profile.preset);
            if (res.rememberPassword) cfg.password = params.password;
            else                      cfg.password.clear();
            cfg.rememberPassword = res.rememberPassword;
            std::string serr;
            cfg.save(serr);

            g_fullscreen = fullscreen;
            HWND sessHwnd = create_render_window(hinst, fullscreen, L"jianhx 远控端", err);
            if (!sessHwnd) {
                connectError = "创建渲染窗口失败: " + err;
                MS_ERR("%s", connectError.c_str());
                // 把错误显示在连接面板上（红字），而不是静默继续
                continue;
            }

            SessionOptions opts;
            opts.fullscreen = fullscreen;
            opts.overlayEnabled = cfg.overlayEnabled;
            opts.overlayCorner = cfg.overlayCorner;
            opts.releaseHotkey = cfg.releaseHotkey;
            opts.captureOnConnect = cfg.captureOnConnect;
            opts.mouseMode = cfg.mouseMode;
            opts.sharpen = cfg.sharpen;
            opts.audioEnabled = cfg.audioEnabled;

            ClientSession session;
            g_session = &session;
            MS_INFO("开始串流会话 -> %s:%u", params.host.c_str(), params.port);
            std::string serr2;
            const bool ok = session.run(sessHwnd, params, opts, serr2);
            g_session = nullptr;
            // 会话里改过锐化档位 -> 记住，下次连上还是这个手感
            {
                const int slv = session.sharpen_level();
                if (slv >= 0 && slv != cfg.sharpen) {
                    cfg.sharpen = slv;
                    std::string sErr;
                    if (cfg.save(sErr)) MS_INFO("已记住锐化档位 %d", slv);
                }
            }

            restore_window(sessHwnd);
            ::DestroyWindow(sessHwnd);

            if (!ok) {
                MS_ERR("会话失败: %s", serr2.c_str());
                // ---- 自动重连（P0 修复）----
                // 备用端口（backup_port）只接受**输入连接**，不能承载完整
                // 串流会话（视频/音频/协商都在主线路端口上）。之前的版本把
                // 完整会话重连目标改到 backupPort，必然握手失败/黑屏。
                // 没有「第二个完整会话端点」之前，只重试原会话地址。
                if (autoRetriesLeft > 0 && cfg.autoReconnect > 0) {
                    --autoRetriesLeft;
                    MS_WARN("自动重连 -> %s:%u（剩余重试 %d 次）",
                            params.host.c_str(), params.port, autoRetriesLeft);
                    ::Sleep(1200);   // 给对端/FRP 一点恢复时间
                    retryAuto = true;
                    continue;
                }
                connectError = serr2;
                continue;               // 回连接面板显示错误
            }

            // 正常结束：用户主动退出/ESC。把断开原因也显示一下。
            // 拿到断开原因（掉线时客户端自己存了）
            const std::string why = session.status().disconnectReason.empty()
                                          ? session.status().lastError
                                          : session.status().disconnectReason;
            connectError = why.empty()
                ? "连接已断开（用户主动退出）"
                : why;                  // 显示掉线原因（比如"被控端 8 秒没收到消息"）
            MS_INFO("会话结束: %s", connectError.c_str());
            // 不退出程序：回到连接面板，显示原因，用户可以直接再连
        }
        log_close();
        return 0;
    }

    // ---- --auto 自动模式（自动化/脚本）：单次连接，失败直接退出 ----
    if (params.host.empty() || params.password.empty()) {
        MS_ERR("--auto 模式需要 --host 与 --password");
        return 2;
    }
    if (!cfg.validate(err)) {
        MS_ERR("配置不合法: %s", err.c_str());
        return 3;
    }

    g_fullscreen = fullscreen;
    HWND hwnd = create_render_window(hinst, fullscreen, L"jianhx 远控端", err);
    if (!hwnd) {
        MS_ERR("%s", err.c_str());
        return 4;
    }

    SessionOptions opts;
    opts.fullscreen = fullscreen;
    opts.overlayEnabled = cfg.overlayEnabled;
    opts.overlayCorner = cfg.overlayCorner;
    opts.releaseHotkey = cfg.releaseHotkey;
    opts.captureOnConnect = cfg.captureOnConnect && args.autoConnect;
    opts.mouseMode = cfg.mouseMode;
    opts.sharpen = cfg.sharpen;
    opts.audioEnabled = cfg.audioEnabled;

    ClientSession session;
    g_session = &session;

    // --seconds N：到点自动结束会话（自动化实测用）
    std::thread autoStop;
    if (args.seconds > 0) {
        autoStop = std::thread([&session, n = args.seconds] {
            sleep_ms(n * 1000);
            MS_INFO("达到 --seconds %d，结束会话", n);
            session.request_stop();
        });
    }

    MS_INFO("开始串流会话 -> %s:%u", params.host.c_str(), params.port);
    std::string serr;
    const bool ok = session.run(hwnd, params, opts, serr);
    g_session = nullptr;
    if (autoStop.joinable()) autoStop.join();

    if (!ok) {
        MS_ERR("会话失败: %s", serr.c_str());
        ::DestroyWindow(hwnd);
        log_close();
        return 5;
    }

    restore_window(hwnd);
    ::DestroyWindow(hwnd);
    log_close();

    // --seconds 到点自动退出（自动化实测用）
    if (args.seconds > 0) return 0;
    return ok ? 0 : 5;
}
