// =============================================================================
//  main.cpp - 被控端入口
//
//  两种运行方式：
//    * 正常模式：托盘图标 + 配置窗口（双击 exe 就用这个）
//    * --headless：无界面纯服务模式，日志打到控制台，便于自动化和实测
// =============================================================================
#include "host_service.h"
#include "tray.h"

#include "../common/config.h"
#include "../common/crash.h"
#include "../common/log.h"
#include "../common/clock.h"
#include "../common/types.h"

#include <windows.h>
#include <shellapi.h>
#include <cstdio>
#include <cstring>
#include <string>

using namespace ms;
using namespace ms::host;

// 双显卡笔记本/台式机上告诉驱动「用独显跑」——UU远程/GameViewer 也这么做
// （逆向第 2 轮 §8：导出 NvOptimusEnablement / AmdPowerXpressRequestHighPerformance）。
// 采集走 DXGI，如果系统给我们分到核显而画面在独显上，Desktop Duplication 根本
// 收不到那台显示器的内容，或者要经过跨 GPU 拷贝。
extern "C" {
__declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
__declspec(dllexport) DWORD AmdPowerXpressRequestHighPerformance = 0x00000001;
}

namespace {

struct Args {
    std::string config;
    std::string password;
    std::string encoder;
    int  port = -1;
    int  fps = -1;
    int  bitrate = -1;
    int  monitor = -1;
    int  logLevel = -1;
    int  seconds = 0;        // >0: headless 模式跑这么久后退出
    bool headless = false;
    bool console = false;
};

std::string narrow(const wchar_t* w) {
    if (!w) return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n > 0 ? n - 1 : 0), '\0');
    if (n > 1) ::WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
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
        else if (k == "--password") a.password = next(i);
        else if (k == "--encoder")  a.encoder  = next(i);
        else if (k == "--port")     a.port     = std::atoi(next(i).c_str());
        else if (k == "--fps")      a.fps      = std::atoi(next(i).c_str());
        else if (k == "--bitrate")  a.bitrate  = std::atoi(next(i).c_str());
        else if (k == "--monitor")  a.monitor  = std::atoi(next(i).c_str());
        else if (k == "--log-level")a.logLevel = std::atoi(next(i).c_str());
        else if (k == "--seconds")  a.seconds  = std::atoi(next(i).c_str());
        else if (k == "--headless") a.headless = true;
        else if (k == "--console")  a.console  = true;
    }
    ::LocalFree(argv);
    return a;
}

void attach_console() {
    if (!::AttachConsole(ATTACH_PARENT_PROCESS)) {
        ::AllocConsole();
    }
    FILE* f = nullptr;
    freopen_s(&f, "CONOUT$", "w", stdout);
    freopen_s(&f, "CONOUT$", "w", stderr);
    freopen_s(&f, "CONIN$", "r", stdin);
    ::SetConsoleOutputCP(CP_UTF8);
}

BOOL WINAPI ctrl_handler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        MS_INFO("收到中断信号，准备退出");
        ::PostQuitMessage(0);
        return TRUE;
    }
    return FALSE;
}

int run_headless(HostConfig& cfg, int seconds) {
    HostService svc;
    std::string err;

    svc.set_status_sink([](const HostStatus& s) {
        if (s.clientConnected) {
            MS_INFO("[状态] 客户端 %s | %s(%s) %dx%d@%d %dkbps | 采集 %.1ffps 编码 %.1ffps 发送 %.0fkbps | 编码延迟 %.1fms | 已发 %llu 丢 %llu | 客户端反馈 延迟 %.1fms 丢 %u 帧",
                    s.clientAddr.c_str(), s.encoderName.c_str(), s.hardwareEncoder ? "硬编" : "软编",
                    s.width, s.height, s.fps, s.bitrateKbps,
                    s.captureFps, s.encodeFps, s.sendKbps,
                    double(s.encodeLatencyUs) / 1000.0,
                    (unsigned long long)s.framesSent, (unsigned long long)s.framesDropped,
                    double(s.clientLatencyUs) / 1000.0, s.clientFramesLost);
        } else if (!s.lastError.empty()) {
            MS_WARN("[状态] %s", s.lastError.c_str());
        }
    });

    if (!svc.start(cfg, err)) {
        MS_ERR("启动失败: %s", err.c_str());
        return 2;
    }
    MS_INFO("被控端已启动 (headless)，监听 %s:%d (TCP+UDP)", cfg.bind.c_str(), cfg.port);
    MS_INFO("按 Ctrl+C 退出");

    ::SetConsoleCtrlHandler(ctrl_handler, TRUE);

    MSG msg;
    const i64 t0 = now_us();
    while (true) {
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                svc.stop();
                MS_INFO("已停止");
                return 0;
            }
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
        if (seconds > 0 && (now_us() - t0) > i64(seconds) * 1000000) {
            MS_INFO("达到 --seconds %d，退出", seconds);
            svc.stop();
            return 0;
        }
        sleep_ms(50);
    }
}

} // namespace

int WINAPI wWinMain(HINSTANCE hinst, HINSTANCE, LPWSTR, int) {
    const Args args = parse_args();
    if (args.headless || args.console) attach_console();

    const std::string dir = exe_dir();
    std::string cfgPath = args.config.empty() ? (dir + "\\host.ini") : args.config;

    HostConfig cfg;
    std::string err;
    if (!HostConfig::load(cfgPath, cfg, err)) {
        MS_WARN("%s", err.c_str());
        // 首次运行：写一份默认配置，方便用户直接改
        cfg.configPath = cfgPath;
        std::string serr;
        if (cfg.save(serr)) MS_INFO("已生成默认配置: %s", cfgPath.c_str());
    }
    cfg.configPath = cfgPath;

    // 命令行覆盖
    if (args.port > 0)      cfg.port = u16(args.port);
    if (!args.password.empty()) cfg.password = args.password;
    if (args.fps > 0)       cfg.fps = args.fps;
    if (args.bitrate >= 0)  cfg.bitrateKbps = args.bitrate;
    if (args.monitor >= 0)  cfg.monitor = args.monitor;
    if (!args.encoder.empty()) cfg.encoder = args.encoder;
    if (args.logLevel >= 0) cfg.logLevel = args.logLevel;

    log_set_level(static_cast<LogLevel>(cfg.logLevel < 0 ? 2 : (cfg.logLevel > 4 ? 4 : cfg.logLevel)));
    log_set_file(dir + "\\host.log");

    // 崩溃自诊断：未捕获异常/访问越界都会写 crash.log + minidump，
    // 而不是只弹一个"程序已停止工作"然后消失。
    crash::install("host");
    MS_INFO("崩溃自诊断已就绪：崩溃时写 %s 与 crash_host_<pid>.dmp", crash::log_path());

    // -------------------------------------------------------------------------
    // 单实例守卫。
    // 两个被控端同时跑会：抢同一端口（后启动的 bind 失败刷屏）、两个进程同时
    // 写同一个 host.log（日志互相覆盖成乱码），并且连接会被其中任意一个吃掉，
    // 表现成"连上了却永远收不到 Challenge"——历史上最难排查的故障。
    // 同一台机器只需要一个被控端，这里直接拦住第二个。
    // -------------------------------------------------------------------------
    ::SetLastError(0);
    HANDLE instanceMutex = ::CreateMutexW(nullptr, TRUE, L"Local\\MiniStreamHostSingleton");
    if (instanceMutex && ::GetLastError() == ERROR_ALREADY_EXISTS) {
        MS_WARN("已经有一个被控端在运行，本次启动退出（同一台机器只需要一个）");
        if (!args.headless) {
            ::MessageBoxW(nullptr,
                          L"已经有一个 jianhx 被控端在运行了。\n\n"
                          L"同一台机器只需要一个：请直接使用右下角托盘里的那个图标。\n"
                          L"（如果托盘里没有它，说明上一个进程已经卡住，"
                          L"请在任务管理器里结束 ministream-host.exe 后重试。）",
                          L"jianhx 被控端", MB_ICONINFORMATION | MB_OK);
        }
        return 6;
    }

    if (!cfg.validate(err)) {
        MS_ERR("配置不合法: %s", err.c_str());
        if (args.headless) return 3;
        ::MessageBoxW(nullptr, std::wstring(err.begin(), err.end()).c_str(),
                      L"jianhx 被控端 - 配置错误", MB_ICONERROR);
        return 3;
    }

    MS_INFO("jianhx 被控端启动, 配置 %s", cfgPath.c_str());

    if (args.headless) {
        return run_headless(cfg, args.seconds);
    }

    // ---- 正常模式：托盘 + 配置窗口 ----
    HostUi ui;
    if (!ui.create(hinst, &cfg, cfgPath, err)) {
        MS_ERR("创建托盘界面失败: %s", err.c_str());
        return 4;
    }

    HostService svc;

    // 当前"已生效"的配置：保存时只有真的改了才重启监听，避免手滑点保存
    // 就掐断正在进行的会话。
    HostConfig activeCfg = cfg;
    auto effective_changed = [](const HostConfig& a, const HostConfig& b) {
        return a.port != b.port || a.bind != b.bind || a.password != b.password ||
               a.fps != b.fps || a.bitrateKbps != b.bitrateKbps ||
               a.monitor != b.monitor || a.encoder != b.encoder ||
               a.inputEnabled != b.inputEnabled ||
               a.keyframeIntervalSec != b.keyframeIntervalSec;
    };

    svc.set_status_sink([&ui, &cfg](const HostStatus& s) {
        char buf[512];
        if (s.clientConnected) {
            std::snprintf(buf, sizeof(buf),
                          "已连接 %s | %s | %dx%d@%d %dkbps | %.1f fps | %.0f kbps | 延迟 %.1f ms",
                          s.clientAddr.c_str(), s.encoderName.c_str(),
                          s.width, s.height, s.fps, s.bitrateKbps,
                          s.encodeFps, s.sendKbps, double(s.encodeLatencyUs) / 1000.0);
        } else if (s.listening) {
            std::snprintf(buf, sizeof(buf), "等待连接 | 监听 %d (TCP+UDP)", cfg.port);
        } else {
            std::snprintf(buf, sizeof(buf), "已停止");
        }
        ui.set_status(buf);
    });

    ui.set_on_service_start([&] {
        std::string e;
        if (!svc.start(cfg, e)) {
            MS_ERR("启动服务失败: %s", e.c_str());
            ui.notify("启动失败", e);
            ui.set_running(false);
        } else {
            activeCfg = cfg;
            ui.set_running(true);
        }
    });
    ui.set_on_service_stop([&] {
        svc.stop();
        ui.set_running(false);
        ui.set_status("已停止");
    });
    ui.set_on_quit([&] {
        svc.stop();
        log_close();
    });
    ui.set_on_config_saved([&](const HostConfig& c) {
        // 保存后**立刻**按新配置重新监听。
        // 之前的版本只提示"重启服务后生效"：但 HostService 里持有的是启动时的
        // 配置快照，于是用户改了端口/密码之后，服务仍在用旧端口旧口令工作——
        // 客户端连新端口会超时、填新密码会说"验证码错误"，很容易被误判成程序坏了。
        //
        // 但只有"真的改了"才重启：否则手滑点一下保存就会掐断正在进行的会话。
        if (!svc.running()) { activeCfg = c; return; }
        if (!effective_changed(activeCfg, c)) {
            MS_INFO("配置内容没有变化，保持当前监听（不打断会话）");
            ui.notify("配置已保存", "内容没有变化，监听保持不变");
            return;
        }
        svc.stop();
        std::string e;
        if (!svc.start(c, e)) {
            MS_ERR("按新配置重启监听失败: %s", e.c_str());
            ui.notify("配置已保存，但重启监听失败", e);
            ui.set_running(false);
        } else {
            activeCfg = c;
            MS_INFO("配置已保存，已按新配置重新监听 %s:%u", c.bind.c_str(), unsigned(c.port));
            ui.set_running(true);
            ui.notify("配置已保存并生效",
                      "已在 " + std::to_string(unsigned(c.port)) + " 端口重新监听");
        }
    });

    // 启动即开始监听
    {
        std::string e;
        if (!svc.start(cfg, e)) {
            MS_ERR("启动服务失败: %s", e.c_str());
            ui.notify("启动失败", e);
        } else {
            activeCfg = cfg;
            ui.set_running(true);
        }
    }

    const int rc = ui.message_loop();
    svc.stop();
    ui.destroy();
    log_close();
    return rc;
}
