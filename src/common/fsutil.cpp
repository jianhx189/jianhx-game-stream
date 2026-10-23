// =============================================================================
//  fsutil.cpp
// =============================================================================
#include "fsutil.h"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <vector>

namespace ms::fsutil {

std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(size_t(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), &w[0], n);
    return w;
}

std::string wide_to_utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()),
                                        nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(size_t(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), &s[0], n, nullptr, nullptr);
    return s;
}

bool read_text_file(const std::string& pathUtf8, std::string& out) {
    out.clear();
    const std::wstring wpath = utf8_to_wide(pathUtf8);
    if (wpath.empty()) return false;

    FILE* f = _wfopen(wpath.c_str(), L"rb");
    if (!f) return false;

    char buf[8192];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        out.append(buf, n);
    }
    std::fclose(f);

    // 跳过 UTF-8 BOM
    if (out.size() >= 3 && (unsigned char)out[0] == 0xEF &&
        (unsigned char)out[1] == 0xBB && (unsigned char)out[2] == 0xBF) {
        out.erase(0, 3);
    }
    return true;
}

bool write_text_file(const std::string& pathUtf8, const std::string& content) {
    const std::wstring wpath = utf8_to_wide(pathUtf8);
    if (wpath.empty()) return false;
    FILE* f = _wfopen(wpath.c_str(), L"wb");
    if (!f) return false;
    const size_t wrote = content.empty() ? 0 : std::fwrite(content.data(), 1, content.size(), f);
    std::fclose(f);
    return wrote == content.size();
}

bool append_text_file(const std::string& pathUtf8, const std::string& content) {
    const std::wstring wpath = utf8_to_wide(pathUtf8);
    if (wpath.empty()) return false;
    FILE* f = _wfopen(wpath.c_str(), L"ab");
    if (!f) return false;
    const size_t wrote = content.empty() ? 0 : std::fwrite(content.data(), 1, content.size(), f);
    std::fclose(f);
    return wrote == content.size();
}

bool file_exists(const std::string& pathUtf8) {
    const std::wstring w = utf8_to_wide(pathUtf8);
    if (w.empty()) return false;
    const DWORD a = ::GetFileAttributesW(w.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool dir_exists(const std::string& pathUtf8) {
    const std::wstring w = utf8_to_wide(pathUtf8);
    if (w.empty()) return false;
    const DWORD a = ::GetFileAttributesW(w.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

std::string dir_name(const std::string& pathUtf8) {
    const size_t p = pathUtf8.find_last_of("\\/");
    if (p == std::string::npos) return {};
    return pathUtf8.substr(0, p);
}

std::string join(const std::string& dirUtf8, const std::string& nameUtf8) {
    if (dirUtf8.empty()) return nameUtf8;
    const char last = dirUtf8.back();
    if (last == '\\' || last == '/') return dirUtf8 + nameUtf8;
    return dirUtf8 + "\\" + nameUtf8;
}

} // namespace ms::fsutil
