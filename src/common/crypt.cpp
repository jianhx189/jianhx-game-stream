// =============================================================================
//  crypt.cpp - SHA-256 (FIPS 180-4) + HMAC (RFC 2104)
// =============================================================================
#include "crypt.h"

#include <windows.h>
#include <bcrypt.h>
#include <cstring>
#include <cstdio>

namespace ms::crypt {
namespace {

// ---- SHA-256 常量 ----
constexpr u32 K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

inline u32 rotr(u32 x, u32 n) { return (x >> n) | (x << (32 - n)); }

struct Sha256Ctx {
    u32 h[8];
    u64 total = 0;
    u8  buf[64];
    size_t buflen = 0;

    Sha256Ctx() {
        h[0]=0x6a09e667; h[1]=0xbb67ae85; h[2]=0x3c6ef372; h[3]=0xa54ff53a;
        h[4]=0x510e527f; h[5]=0x9b05688c; h[6]=0x1f83d9ab; h[7]=0x5be0cd19;
    }

    void block(const u8* p) {
        u32 w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = (u32(p[i*4]) << 24) | (u32(p[i*4+1]) << 16) | (u32(p[i*4+2]) << 8) | u32(p[i*4+3]);
        }
        for (int i = 16; i < 64; ++i) {
            const u32 s0 = rotr(w[i-15],7) ^ rotr(w[i-15],18) ^ (w[i-15] >> 3);
            const u32 s1 = rotr(w[i-2],17) ^ rotr(w[i-2],19)  ^ (w[i-2] >> 10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        u32 a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
        for (int i = 0; i < 64; ++i) {
            const u32 S1 = rotr(e,6) ^ rotr(e,11) ^ rotr(e,25);
            const u32 ch = (e & f) ^ ((~e) & g);
            const u32 t1 = hh + S1 + ch + K[i] + w[i];
            const u32 S0 = rotr(a,2) ^ rotr(a,13) ^ rotr(a,22);
            const u32 mj = (a & b) ^ (a & c) ^ (b & c);
            const u32 t2 = S0 + mj;
            hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
    }

    void update(const u8* p, size_t n) {
        total += n;
        while (n > 0) {
            const size_t take = (n < 64 - buflen) ? n : (64 - buflen);
            std::memcpy(buf + buflen, p, take);
            buflen += take; p += take; n -= take;
            if (buflen == 64) { block(buf); buflen = 0; }
        }
    }

    void final(u8 out[32]) {
        const u64 bits = total * 8;
        const u8 pad = 0x80;
        update(&pad, 1);
        const u8 zero = 0;
        while (buflen != 56) update(&zero, 1);
        u8 len[8];
        for (int i = 0; i < 8; ++i) len[i] = u8((bits >> (56 - i*8)) & 0xFF);
        update(len, 8);
        for (int i = 0; i < 8; ++i) {
            out[i*4]   = u8((h[i] >> 24) & 0xFF);
            out[i*4+1] = u8((h[i] >> 16) & 0xFF);
            out[i*4+2] = u8((h[i] >> 8) & 0xFF);
            out[i*4+3] = u8(h[i] & 0xFF);
        }
    }
};

} // namespace

void sha256(const void* data, size_t len, u8 out[kSha256Len]) {
    Sha256Ctx c;
    c.update(static_cast<const u8*>(data), len);
    c.final(out);
}

void sha256(const Bytes& data, u8 out[kSha256Len]) {
    sha256(data.data(), data.size(), out);
}

std::string sha256_hex(const std::string& s) {
    u8 d[kSha256Len];
    sha256(s.data(), s.size(), d);
    static const char* hexd = "0123456789abcdef";
    std::string r;
    r.reserve(64);
    for (u8 b : d) { r.push_back(hexd[b >> 4]); r.push_back(hexd[b & 0xF]); }
    return r;
}

void hmac_sha256(const u8* key, size_t keyLen, const u8* msg, size_t msgLen, u8 out[kSha256Len]) {
    u8 k[64];
    std::memset(k, 0, sizeof(k));
    if (keyLen > 64) {
        sha256(key, keyLen, k);        // 长密钥先哈希，其余补 0
    } else {
        std::memcpy(k, key, keyLen);
    }

    u8 ipad[64], opad[64];
    for (int i = 0; i < 64; ++i) { ipad[i] = u8(k[i] ^ 0x36); opad[i] = u8(k[i] ^ 0x5C); }

    Sha256Ctx inner;
    inner.update(ipad, 64);
    inner.update(msg, msgLen);
    u8 ih[32];
    inner.final(ih);

    Sha256Ctx outer;
    outer.update(opad, 64);
    outer.update(ih, 32);
    outer.final(out);
}

void derive_key(const std::string& password, u8 out[kSha256Len]) {
    static const char kSaltPrefix[] = "MiniStream/v1";
    Bytes b;
    b.insert(b.end(), kSaltPrefix, kSaltPrefix + sizeof(kSaltPrefix) - 1);
    b.insert(b.end(), password.begin(), password.end());
    sha256(b, out);
}

void compute_proof(const u8 key[kSha256Len],
                   const u8 clientNonce[16],
                   const u8 serverNonce[16],
                   const u8 salt[16],
                   u8 out[kProofLen]) {
    Bytes msg;
    msg.reserve(48);
    msg.insert(msg.end(), clientNonce, clientNonce + 16);
    msg.insert(msg.end(), serverNonce, serverNonce + 16);
    msg.insert(msg.end(), salt, salt + 16);
    hmac_sha256(key, kSha256Len, msg.data(), msg.size(), out);
}

void random_bytes(void* buf, size_t n) {
    if (BCryptGenRandom(nullptr, static_cast<PUCHAR>(buf), static_cast<ULONG>(n),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        // 理论上不会失败；退化到时间+计数器，保证不会返回全 0
        static volatile u64 ctr = 0;
        u8* p = static_cast<u8*>(buf);
        const u64 t = u64(GetTickCount64()) ^ (++ctr * 0x9E3779B97F4A7C15ull);
        for (size_t i = 0; i < n; ++i) p[i] = u8((t >> ((i % 8) * 8)) & 0xFF);
    }
}

bool constant_time_equal(const u8* a, const u8* b, size_t n) {
    u8 diff = 0;
    for (size_t i = 0; i < n; ++i) diff |= u8(a[i] ^ b[i]);
    return diff == 0;
}

} // namespace ms::crypt
