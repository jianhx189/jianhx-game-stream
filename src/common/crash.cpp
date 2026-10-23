// =============================================================================
//  crash.cpp
// =============================================================================
#include "crash.h"

#include <windows.h>
#include <dbghelp.h>

#include <crtdbg.h>
#include <cstdlib>
#include <exception>
#include <new>
#include <stdexcept>
#include <system_error>
#include <typeinfo>

namespace ms::crash {
namespace {

char    g_tag[32]   = "app";
wchar_t g_tagW[32]  = L"app";
wchar_t g_dirW[MAX_PATH] = {0};
char    g_logPath[MAX_PATH * 2] = {0};

void init_paths() {
    if (g_dirW[0]) return;
    ::GetModuleFileNameW(nullptr, g_dirW, MAX_PATH);
    wchar_t* p = ::wcsrchr(g_dirW, L'\\');
    if (p) *p = L'\0';
}

void to_ascii(const wchar_t* w, char* out, int n) {
    out[0] = '\0';
    if (!w || n <= 1) return;
    ::WideCharToMultiByte(CP_UTF8, 0, w, -1, out, n - 1, nullptr, nullptr);
    out[n - 1] = '\0';
}

// 崩溃路径专用的写日志：CreateFile + WriteFile，不碰 CRT。
void raw_write(const char* s) {
    init_paths();
    ::OutputDebugStringA(s);
    ::OutputDebugStringA("\n");

    wchar_t path[MAX_PATH + 32];
    ::wsprintfW(path, L"%s\\crash.log", g_dirW);
    HANDLE h = ::CreateFileW(path, FILE_APPEND_DATA,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;

    SYSTEMTIME st{};
    ::GetLocalTime(&st);
    char line[1400];
    const int n = ::wsprintfA(line, "[%02d:%02d:%02d.%03d][%s] %s\r\n",
                              st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                              g_tag, s);
    if (n > 0) {
        DWORD written = 0;
        ::WriteFile(h, line, DWORD(n < int(sizeof(line)) ? n : int(sizeof(line)) - 1),
                    &written, nullptr);
    }
    ::CloseHandle(h);
}

void write_dump(EXCEPTION_POINTERS* ep) {
    init_paths();
    wchar_t path[MAX_PATH + 64];
    ::wsprintfW(path, L"%s\\crash_%s_%lu.dmp", g_dirW, g_tagW,
                (unsigned long)::GetCurrentProcessId());

    HANDLE h = ::CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { raw_write("写崩溃转储失败：无法创建文件"); return; }

    MINIDUMP_EXCEPTION_INFORMATION info{};
    info.ThreadId          = ::GetCurrentThreadId();
    info.ExceptionPointers = ep;
    info.ClientPointers    = FALSE;

    const MINIDUMP_TYPE type = static_cast<MINIDUMP_TYPE>(
        MiniDumpNormal | MiniDumpWithThreadInfo |
        MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithDataSegs);

    const BOOL ok = ::MiniDumpWriteDump(::GetCurrentProcess(), ::GetCurrentProcessId(),
                                        h, type, ep ? &info : nullptr, nullptr, nullptr);
    ::CloseHandle(h);

    char a[MAX_PATH * 2];
    to_ascii(path, a, int(sizeof(a)));
    if (ok) { char b[1400]; ::wsprintfA(b, "崩溃转储已写出: %s", a); raw_write(b); }
    else    { raw_write("写崩溃转储失败（MiniDumpWriteDump 返回 FALSE）"); }
}

// 把调用栈按「模块+偏移」打进日志：没有符号也能看出崩在哪个 DLL 的哪里。
void log_stack() {
    void* frames[48] = {0};
    const USHORT n = ::CaptureStackBackTrace(0, 48, frames, nullptr);
    raw_write("---- 调用栈（模块+偏移）----");
    for (USHORT i = 0; i < n; ++i) {
        HMODULE mod = nullptr;
        char name[MAX_PATH] = "?";
        if (frames[i] && ::GetModuleHandleExA(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCSTR>(frames[i]), &mod)) {
            ::GetModuleFileNameA(mod, name, MAX_PATH);
        }
        const char* slash = strrchr(name, '\\');
        const char* base  = slash ? slash + 1 : name;
        const unsigned long long off = mod
            ? (unsigned long long)(reinterpret_cast<char*>(frames[i]) -
                                   reinterpret_cast<char*>(mod))
            : 0ull;
        char line[400];
        // 注意：user32 的 wsprintf 不支持 %llX（会打印成 "lX"），必须用 %I64X
        ::wsprintfA(line, "  #%02u %s+0x%I64X", (unsigned)i, base, off);
        raw_write(line);
    }
}

// ---- 1) 未捕获的 C++ 异常 ----
void on_terminate() {
    raw_write("===== 未捕获的 C++ 异常（std::terminate）=====");
    const std::exception_ptr ep = std::current_exception();
    if (ep) {
        try {
            std::rethrow_exception(ep);
        } catch (const std::system_error& e) {
            char b[1200];
            ::wsprintfA(b, "类型: std::system_error  what(): %s  code: %d (%s)",
                        e.what(), e.code().value(), e.code().message().c_str());
            raw_write(b);
        } catch (const std::exception& e) {
            char b[1200];
            ::wsprintfA(b, "类型: %s  what(): %s", typeid(e).name(), e.what());
            raw_write(b);
        } catch (...) {
            raw_write("类型: 非 std::exception 派生（自定义类型或非 C++ 异常）");
        }
    } else {
        raw_write("current_exception() 为空。常见原因："
                  "std::thread 析构时仍然 joinable，或在 noexcept 函数里抛出。");
    }
    log_stack();
    write_dump(nullptr);
    // 不用 abort()：日志和转储已经落盘，abort 只会再弹一个系统错误框。
    ::TerminateProcess(::GetCurrentProcess(), 3);
}

// ---- 2) 结构化异常（访问越界、除零、栈溢出等）----
LONG WINAPI on_seh(EXCEPTION_POINTERS* ep) {
    const DWORD code = (ep && ep->ExceptionRecord) ? ep->ExceptionRecord->ExceptionCode : 0;
    // 0xE06D7363 是 C++ 的 throw：正常传播路径，交给 C++ 运行时/terminate 处理
    if (code == 0xE06D7363) return EXCEPTION_CONTINUE_SEARCH;

    char b[400];
    ::wsprintfA(b, "===== 未处理的结构化异常 0x%08lX =====", (unsigned long)code);
    raw_write(b);

    if (ep && ep->ExceptionRecord) {
        void* addr = reinterpret_cast<void*>(ep->ExceptionRecord->ExceptionAddress);
        HMODULE mod = nullptr;
        char name[MAX_PATH] = "?";
        if (addr && ::GetModuleHandleExA(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCSTR>(addr), &mod)) {
            ::GetModuleFileNameA(mod, name, MAX_PATH);
        }
        const char* slash = strrchr(name, '\\');
        const char* base  = slash ? slash + 1 : name;
        const unsigned long long off = mod
            ? (unsigned long long)(reinterpret_cast<char*>(addr) -
                                   reinterpret_cast<char*>(mod))
            : 0ull;
        char b2[800];
        ::wsprintfA(b2, "异常地址: 0x%p (%s+0x%I64X)", addr, base, off);
        raw_write(b2);
        if (code == 0xC0000005 && ep->ExceptionRecord->NumberParameters >= 2) {
            const ULONG_PTR rw = ep->ExceptionRecord->ExceptionInformation[0];
            char b3[200];
            ::wsprintfA(b3, "  访问类型: %s  地址: 0x%p",
                        rw == 0 ? "读取" : (rw == 1 ? "写入" : "执行"),
                        (void*)ep->ExceptionRecord->ExceptionInformation[1]);
            raw_write(b3);
        }
    }
    log_stack();
    write_dump(ep);
    ::TerminateProcess(::GetCurrentProcess(), 4);
    return EXCEPTION_EXECUTE_HANDLER;
}

// ---- 3) CRT 非法参数 ----
// UCRT 默认会 __fastfail(FAST_FAIL_INVALID_ARG) 直接杀进程（异常码 0xC0000409），
// 这里改成先落日志，仍然退出——行为可预期，且以后能查到原因。
void on_invalid_param(const wchar_t* expr, const wchar_t* func, const wchar_t* file,
                      unsigned int line, uintptr_t) {
    char b[1200];
    char a1[256] = "?", a2[256] = "?", a3[256] = "?";
    to_ascii(expr, a1, int(sizeof(a1)));
    to_ascii(func, a2, int(sizeof(a2)));
    to_ascii(file, a3, int(sizeof(a3)));
    ::wsprintfA(b, "===== CRT 非法参数: %s  函数 %s  位置 %s:%u =====", a1, a2, a3, line);
    raw_write(b);
    log_stack();
    write_dump(nullptr);
    ::TerminateProcess(::GetCurrentProcess(), 5);
}

} // namespace

void install(const char* appTag) {
    init_paths();
    if (appTag) {
        ::lstrcpynA(g_tag, appTag, int(sizeof(g_tag)));
        ::MultiByteToWideChar(CP_UTF8, 0, appTag, -1, g_tagW,
                              int(sizeof(g_tagW) / sizeof(wchar_t)));
    }

    std::set_terminate(on_terminate);
    ::SetUnhandledExceptionFilter(on_seh);
    ::_set_invalid_parameter_handler(on_invalid_param);
    // 不弹"程序已停止工作"框，也不让 abort() 再触发一次 WER
    ::_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    ::SetErrorMode(::SetErrorMode(0) | SEM_NOGPFAULTERRORBOX);

    // 刻意不在这里写 crash.log：那份文件必须只在真的崩溃时出现，
    // 否则每次启动都多一行"已安装"，很容易被当成崩溃记录。
}

const char* log_path() {
    init_paths();
    if (!g_logPath[0]) {
        char a[MAX_PATH * 2];
        to_ascii(g_dirW, a, int(sizeof(a)));
        ::wsprintfA(g_logPath, "%s\\crash.log", a);
    }
    return g_logPath;
}

} // namespace ms::crash
