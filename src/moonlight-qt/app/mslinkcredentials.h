// =============================================================================
//  mslinkcredentials.h —— mslink.ini 口令字段的 Windows DPAPI 保护
//
//  用当前 Windows 用户的 DPAPI（CryptProtectData / CryptUnprotectData，
//  CRYPTPROTECT_UI_FORBIDDEN）把口令加密成 "dpapi:v1:<base64>" 存进 ini，
//  读取时再解开。旧版本写下的明文口令仍然读得回来，由调用方负责迁移。
//
//  边界（不要过度宣称）：
//   * 不是端到端加密。密文只能在本机、同一 Windows 用户下解开；它防的是
//     ini 被随手拷走或被其他 Windows 用户直接读到，不防同一用户下的恶意进程，
//     也不防内存转储。
//   * 程序运行期间口令仍以明文 QString 存在（QML 需要回填输入框），本模块
//     只对 DPAPI 进出的临时 QByteArray 尽量清零，不能保证进程内存安全。
//   * 非 Windows 平台没有 DPAPI：加密直接失败，密文也解不开；旧明文照常可读，
//     调用方据此决定是否只提示、不阻断启动。
// =============================================================================
#pragma once

#include <QByteArray>
#include <QString>

namespace MslinkCredentials {

// ini 中密文的固定前缀（版本化，便于以后换算法或作用域）
inline constexpr char kDpapiPrefix[] = "dpapi:v1:";

// plain 按 UTF-8 编码后加密。成功时 encoded = "dpapi:v1:<base64>"；
// 失败返回 false 并写 err（调用方不得因此覆盖磁盘上已有的配置文件）。
bool encodeSecret(const QString& plain, QString* encoded, QString* err);

// 只接受 "dpapi:v1:" 前缀的密文；失败返回 false 并写 err。
bool decodeSecret(const QString& encoded, QString* plain, QString* err);

// 读取 ini 里的一个口令值：
//  * 带 "dpapi:v1:" 前缀 → 解密；
//  * 否则按旧版明文原样返回，并置 wasPlaintext = true（调用方据此迁移）；
//  * 空串 → 返回 true、plain 为空（未配置，不是错误）。
// 只有「密文解不开」才返回 false 并写 err。
bool decodeStored(const QString& stored, QString* plain, bool* wasPlaintext, QString* err);

// 尽量把缓冲区清零（用 volatile 写避免被优化掉；不保证副本/寄存器里的残留）
void wipe(QByteArray& buf);

} // namespace MslinkCredentials
