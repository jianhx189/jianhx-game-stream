// =============================================================================
//  log.cpp
// =============================================================================
#include "log.h"
#include "clock.h"
#include "fsutil.h"

#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <mutex>
#include <string>

namespace ms {
namespace {

LogLevel   g_level = LogLevel::Info;
FILE*      g_file  = nullptr;
bool       g_console = false;
std::mutex g_mtx;

const char* level_tag(LogLevel lv) {
    switch (lv) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO ";
        case LogLevel::Warn:  return "WARN ";
        case LogLevel::Error: return "ERROR";
        default:              return "?????";
    }
}

} // namespace

void log_set_level(LogLevel lv) { g_level = lv; }
LogLevel log_get_level() { return g_level; }

void log_set_console(bool on) { g_console = on; }
bool log_get_console() { return g_console; }

bool log_set_file(const std::string& path) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_file) { std::fclose(g_file); g_file = nullptr; }
    if (path.empty()) return true;
    // 用 _wfopen：路径内部是 UTF-8，std::fopen 会按 ANSI 解释，
    // 安装路径含中文时会导致日志文件静默创建失败。
    const std::wstring wpath = fsutil::utf8_to_wide(path);
    if (wpath.empty()) return false;

    // 新建/空文件先写 UTF-8 BOM：记事本、VS Code、PowerShell 的 Get-Content
    // 都会按 BOM 判断编码；否则中文日志会被当成本地 ANSI 读成乱码。
    bool needBom = false;
    {
        FILE* probe = _wfopen(wpath.c_str(), L"rb");
        if (probe) {
            needBom = (std::fgetc(probe) == EOF);
            std::fclose(probe);
        } else {
            needBom = true;
        }
    }

    g_file = _wfopen(wpath.c_str(), needBom ? L"wb" : L"ab");
    if (g_file && needBom) {
        static const unsigned char kBom[3] = {0xEF, 0xBB, 0xBF};
        std::fwrite(kBom, 1, 3, g_file);
    }
    return g_file != nullptr;
}

void log_close() {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_file) { std::fclose(g_file); g_file = nullptr; }
}

void log_write(LogLevel lv, const char* fmt, ...) {
    if (lv < g_level) return;

    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    SYSTEMTIME st{};
    ::GetLocalTime(&st);
    char line[2200];
    std::snprintf(line, sizeof(line), "[%02d:%02d:%02d.%03d][%s] %s\n",
                  st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                  level_tag(lv), msg);

    std::lock_guard<std::mutex> lk(g_mtx);
    // 【绝不能无条件调 OutputDebugStringA】
    // 客户端机器上常驻的调试输出监听器（输入法 / 远程控制 / 虚拟显示器工具）
    // 会让这个调用阻塞数秒；因为这里是持锁写的，一阻塞整个进程所有线程全冻住
    // （实测 CPU 时间 7.5 秒不动），被控端心跳超时直接踢人。
    // 只在**真有调试器**时才发调试串，正常使用零风险。
    if (::IsDebuggerPresent()) ::OutputDebugStringA(line);
    if (g_console) {
        // stderr 同样可能被控制台/管道阻塞，默认不写；脚本模式才开
        std::fputs(line, stderr);
    }
    if (g_file) {
        std::fputs(line, g_file);
        std::fflush(g_file);   // 崩溃时也能留下日志
    }
}

} // namespace ms
