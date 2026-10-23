// =============================================================================
//  firewall.cpp - 实现：netsh advfirewall（无需 admin API COM，netsh 自带
//  提权提示会替我们处理 UAC 同意框；没提权时静默失败并返回可读错误）。
// =============================================================================
#include "firewall.h"

#include "../common/fsutil.h"

#include <windows.h>
#include <shellapi.h>   // ShellExecuteExW

#include <array>
#include <cstdio>
#include <string>
#include <vector>

namespace ms::fw {
namespace {

std::wstring exe_path_w() {
    wchar_t buf[MAX_PATH * 2] = {0};
    ::GetModuleFileNameW(nullptr, buf, MAX_PATH * 2 - 1);
    return std::wstring(buf);
}

// 同步跑一条命令，捕获 stdout+stderr（最多 64KB）。
// 不依赖 CRT 的文件句柄转接，避免乱码/句柄泄漏。
struct CmdResult {
    DWORD exitCode = 0xFFFFFFFF;
    std::string out;
    bool ok = false;
};

CmdResult run_cmd(const std::wstring& cmdline) {
    CmdResult r;
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE outRd = nullptr, outWr = nullptr;
    if (!::CreatePipe(&outRd, &outWr, &sa, 64 * 1024)) return r;
    // 关键：子进程要能继承写端，读端我们自己用，别让它被子进程继承
    ::SetHandleInformation(outRd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = outWr;
    si.hStdError = outWr;
    si.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};
    std::wstring mutable_cmd = cmdline;   // CreateProcessW 要求可写缓冲
    const BOOL created = ::CreateProcessW(
        nullptr, &mutable_cmd[0], nullptr, nullptr,
        /*bInheritHandles=*/TRUE,
        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
        nullptr, nullptr, &si, &pi);
    ::CloseHandle(outWr);   // 父进程这边关掉写端，读端才能读到 EOF
    if (!created) {
        ::CloseHandle(outRd);
        return r;
    }

    std::string acc;
    std::array<char, 4096> chunk{};
    for (;;) {
        DWORD got = 0;
        if (!::ReadFile(outRd, chunk.data(), DWORD(chunk.size()), &got, nullptr) || got == 0) break;
        acc.append(chunk.data(), got);
        if (acc.size() > 256 * 1024) break;   // 防御性上限
    }
    ::WaitForSingleObject(pi.hProcess, INFINITE);
    ::GetExitCodeProcess(pi.hProcess, &r.exitCode);
    ::CloseHandle(pi.hProcess);
    ::CloseHandle(pi.hThread);
    ::CloseHandle(outRd);
    r.out = std::move(acc);
    r.ok = true;
    return r;
}

std::string w2u8(const std::wstring& w) {
    return fsutil::wide_to_utf8(w);
}

// 已存在的放行规则按名字找：jianhx-Client-Allow。
// netsh advfirewall firewall show rule name=... 有规则时 exitCode=0 且输出含 Rule Name。
const wchar_t* kRuleName = L"jianhx-Client-Allow";

bool rule_exists() {
    const std::wstring cmd =
        L"netsh advfirewall firewall show rule name=\"" + std::wstring(kRuleName) + L"\"";
    const CmdResult r = run_cmd(cmd);
    if (!r.ok) return false;
    return r.exitCode == 0;
}

} // namespace

bool running_as_admin() {
    HANDLE tok = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    TOKEN_ELEVATION elev{};
    DWORD sz = sizeof(elev);
    const BOOL ok = ::GetTokenInformation(tok, TokenElevation, &elev, sizeof(elev), &sz);
    ::CloseHandle(tok);
    return ok && elev.TokenIsElevated != 0;
}

bool is_exempt() {
    return rule_exists();
}

bool add_exemption(std::string& reason) {
    reason.clear();

    // 非管理员时：netsh 会自己弹 UAC 授权框（ShellExecute runas），点"是"就行。
    // 先删掉可能存在的旧规则（程序路径写不进去的残留），再加新的。
    if (!running_as_admin()) {
        std::wstring exe = exe_path_w();
        // 用 ShellExecute 提权运行自身，让它去跑 add_exemption 的 netsh 部分。
        // 传一个特殊参数告诉它"只做防火墙放行"。
        SHELLEXECUTEINFOW sei{};
        sei.cbSize = sizeof(sei);
        sei.lpVerb = L"runas";                     // 触发 UAC
        sei.lpFile = exe.c_str();                  // 自己
        sei.lpParameters = L"--fw-allow";
        sei.nShow = SW_HIDE;
        if (!::ShellExecuteExW(&sei)) {
            const DWORD e = ::GetLastError();
            reason = "UAC 授权被拒绝（错误码 " + std::to_string(e) + "）";
            return false;
        }
        // 提权后的子进程会独立运行并退出，这里不用等；规则在子进程里加。
        reason = "已弹出 UAC 授权框，请在弹出的窗口里点「是」";
        return false;   // 返回 false 让 UI 显示提示文本
    }

    // 已是管理员：直接跑 netsh
    run_cmd(L"netsh advfirewall firewall delete rule name=\"" +
            std::wstring(kRuleName) + L"\"");
    std::wstring exe = exe_path_w();
    wchar_t shortPath[MAX_PATH * 2] = {0};
    DWORD n = ::GetShortPathNameW(exe.c_str(), shortPath, DWORD(std::size(shortPath)));
    std::wstring prog = (n > 0 && n < DWORD(std::size(shortPath))) ? shortPath : exe;

    const std::wstring add =
        L"netsh advfirewall firewall add rule name=\"" + std::wstring(kRuleName) +
        L"\" dir=in action=allow protocol=UDP program=\"" + prog + L"\" enable=yes";
    const CmdResult r = run_cmd(add);
    if (!r.ok) {
        reason = "无法启动 netsh（系统异常）";
        return false;
    }
    if (r.exitCode != 0) {
        reason = r.out;
        const size_t cut = reason.find('\n');
        if (cut != std::string::npos) reason.resize(cut);
        while (!reason.empty() && (reason.back() == '\r' || reason.back() == ' ')) reason.pop_back();
        if (reason.size() > 120) reason.resize(120);
        if (reason.empty()) reason = "netsh 退出码 " + std::to_string(r.exitCode);
        return false;
    }
    return true;
}

std::string status_text() {
    const std::wstring cmd =
        L"netsh advfirewall firewall show rule name=\"" + std::wstring(kRuleName) + L"\"";
    const CmdResult r = run_cmd(cmd);
    if (!r.ok) return "无法检测防火墙状态";
    if (r.exitCode != 0) return "防火墙尚未放行（内网可能黑屏）";
    return "防火墙已放行 ✔";
}

} // namespace ms::fw
