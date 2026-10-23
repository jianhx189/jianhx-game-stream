// =============================================================================
//  log.h - 极简日志
//
//  需求里明确「无日志上传」，所以日志只写本地文件/调试输出。
// =============================================================================
#pragma once

#include "types.h"

namespace ms {

enum class LogLevel : int {
    Trace = 0,
    Debug = 1,
    Info  = 2,
    Warn  = 3,
    Error = 4,
    Off   = 5,
};

void log_set_level(LogLevel lv);
LogLevel log_get_level();
// 打开日志文件（追加）。path 为空则只输出到 stderr / OutputDebugString。
bool log_set_file(const std::string& path);
void log_close();

// 【重要】控制台/调试输出开关，默认关闭。
// 踩过的坑：日志里每行都调 OutputDebugStringA，在客户端那台机器上（装了输入法/
// 远程控制/虚拟显示器等会挂调试输出监听的软件），调用会**阻塞好几秒**，
// 而日志写是在全局锁里做的 —— 结果是整个进程所有线程一起冻死（实测 CPU 时间
// 一动不动 7.5 秒），被控端等不到心跳就把人踢了。所以正式运行只写文件，
// 只有命令行 --auto 这类脚本场景才打开控制台输出。
void log_set_console(bool on);
bool log_get_console();

void log_write(LogLevel lv, const char* fmt, ...);

// 未设置 level 时默认 Info；Debug/Trace 需要显式打开
#define MS_LOG(lv, ...) ::ms::log_write(lv, __VA_ARGS__)
#define MS_TRACE(...) MS_LOG(::ms::LogLevel::Trace, __VA_ARGS__)
#define MS_DEBUG(...) MS_LOG(::ms::LogLevel::Debug, __VA_ARGS__)
#define MS_INFO(...)  MS_LOG(::ms::LogLevel::Info,  __VA_ARGS__)
#define MS_WARN(...)  MS_LOG(::ms::LogLevel::Warn,  __VA_ARGS__)
#define MS_ERR(...)   MS_LOG(::ms::LogLevel::Error, __VA_ARGS__)

} // namespace ms
