// =============================================================================
//  crypt.h - 自带的 SHA-256 / HMAC-SHA256 / 安全随机数
//
//  需求要求「不做复杂加密体系」，但也要求密码校验不能形同虚设。
//  这里用挑战-应答（HMAC）代替明文传输密码：
//      key  = SHA256("MiniStream/v1" || password)
//      proof= HMAC_SHA256(key, clientNonce || serverNonce || salt)
//  不引入 OpenSSL，保持零第三方依赖。
// =============================================================================
#pragma once

#include "types.h"
#include <string>

namespace ms::crypt {

constexpr size_t kSha256Len = 32;
constexpr size_t kProofLen  = 32;

void sha256(const void* data, size_t len, u8 out[kSha256Len]);
void sha256(const Bytes& data, u8 out[kSha256Len]);
std::string sha256_hex(const std::string& s);

void hmac_sha256(const u8* key, size_t keyLen,
                 const u8* msg, size_t msgLen,
                 u8 out[kSha256Len]);

// 由口令派生会话密钥
void derive_key(const std::string& password, u8 out[kSha256Len]);

// 计算认证凭据：HMAC(key, clientNonce || serverNonce || salt)
void compute_proof(const u8 key[kSha256Len],
                   const u8 clientNonce[16],
                   const u8 serverNonce[16],
                   const u8 salt[16],
                   u8 out[kProofLen]);

// 安全随机数（BCryptGenRandom）
void random_bytes(void* buf, size_t n);

// 常量时间比较，避免时序侧信道
bool constant_time_equal(const u8* a, const u8* b, size_t n);

} // namespace ms::crypt
