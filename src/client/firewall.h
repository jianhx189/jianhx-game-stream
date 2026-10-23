// =============================================================================
//  firewall.h - Windows 防火墙一键放行（按程序）
//
//  为什么按程序而不是按端口：
//    客户端的 UDP 监听端口是**每次随机分配**的（listenPort=0），根本没法
//    写死一条"放行某端口"的规则。按程序放行（netsh advfirewall add rule
//    program=...）会自动覆盖该程序所有端口/所有协议，一次配置永久生效。
//
//  典型故障（实测抓到过）：
//    内网连接、密码全过、被控端开始发视频，但客户端一个 UDP 包都收不到，
//    表现是"已连接但黑屏，几秒后被被控端当掉线踢掉"。根因就是 Windows
//    Defender 防火墙拦了 ministream-client.exe 的 UDP 入站。
// =============================================================================
#pragma once

#include <string>

namespace ms::fw {

// 检查 net 防火墙里是否已存在对 ministream-client.exe 的放行规则。
// 有规则但 enable=no 的也算"已放行"（netsh 里 enable 语义是启用/禁用规则本身）。
bool is_exempt();

// 添放入站放行规则（程序级，UDP+TCP 入站）。
// 返回是否成功；失败时 reason 里给出 netsh 的标准错误文本（多为"权限不足"）。
bool add_exemption(std::string& reason);

// 仅用于连接面板的提示行：已放行 / 未放行 / 检测失败。
// 文本里不带路径等敏感信息。
std::string status_text();

// 当前进程是否以管理员身份运行（UAC 提升过）。
bool running_as_admin();

} // namespace ms::fw
