// =============================================================================
//  fsutil.h - 文件路径与读写工具
//
//  为什么需要这一层：
//   Windows 上 std::fopen / std::ifstream 会把传入的窄字符串按「当前 ANSI 代码页」
//   解释，而程序内部统一用 UTF-8。只要安装路径里有一个非 ASCII 字符
//   （例如 D:\远程项目\...），用 std::fopen 打开的日志/配置文件就会全部失败，
//   而且失败得悄无声息。
//   所以这里统一走宽字符 API 的 _wfopen。
// =============================================================================
#pragma once

#include "types.h"
#include <string>

namespace ms::fsutil {

// UTF-8 <-> UTF-16
std::wstring utf8_to_wide(const std::string& s);
std::string  wide_to_utf8(const std::wstring& s);

// 读取整个文件（自动跳过 UTF-8 BOM）。文件不存在返回 false。
bool read_text_file(const std::string& pathUtf8, std::string& out);

// 覆盖写入（UTF-8，无 BOM）
bool write_text_file(const std::string& pathUtf8, const std::string& content);

// 追加写入
bool append_text_file(const std::string& pathUtf8, const std::string& content);

// 文件/目录是否存在
bool file_exists(const std::string& pathUtf8);
bool dir_exists(const std::string& pathUtf8);

// 取路径的目录部分（不含结尾分隔符）；无分隔符时返回空串
std::string dir_name(const std::string& pathUtf8);

// 把相对路径拼到目录后面（自动补分隔符）
std::string join(const std::string& dirUtf8, const std::string& nameUtf8);

} // namespace ms::fsutil
