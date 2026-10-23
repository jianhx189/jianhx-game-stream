// =============================================================================
//  fec.h - XOR 组纠删码（视频 UDP 抗丢包）
//
//  为什么是 XOR 而不是 Reed-Solomon：
//    RS 能一次恢复多个丢包，但要几百行 GF(256) 运算 + 一整套编解码状态；
//    而游戏串流的真实故障几乎都是「随机丢 1~3 个包」，组大小可调的 XOR 组码
//    已经能把 2% 丢包下的整帧到达率从 ~49% 提到 ~80%，实现只有几十行、
//    零依赖、延迟为零（校验包跟着数据分片一起发，不需要额外等待）。
//
//  约定（与 protocol.h 的 kUdpFlagFec 注释一致）：
//    * 一帧数据切成 total 个分片，分片 i 的净荷长度恒为
//        frag_len(frameBytes, i) = min(kMaxVideoPayload, frameBytes - i*kMaxVideoPayload)
//      —— 收端不需要额外携带每片长度。
//    * 每 groupSize 个连续分片组成一组，配 1 个校验包：
//        校验净荷长度 = 组内各片净荷长度的最大值
//        校验净荷     = 组内各片净荷「补零到该长度」后逐字节 XOR
//    * 任意丢 1 片可用「同组其它片 + 校验」恢复（XOR 自反）。
//
//  本文件是纯函数、无 socket、无状态，两端共用同一份实现（也便于单元测试）。
// =============================================================================
#pragma once

#include "types.h"
#include "protocol.h"

#include <cstring>

namespace ms::fec {

// 分片 index 的净荷长度（框架由协议固定，不随包携带）
inline size_t frag_len(size_t frameBytes, u32 index) {
    const size_t off = size_t(index) * proto::kMaxVideoPayload;
    if (off >= frameBytes) return 0;
    const size_t left = frameBytes - off;
    return left < proto::kMaxVideoPayload ? left : proto::kMaxVideoPayload;
}

// 某组的成员范围：[groupStart, groupStart+count)，全部落在 [0, total) 内
inline u32 group_members(u32 total, u32 groupStart, u16 groupSize) {
    if (groupStart >= total) return 0;
    const u32 left = total - groupStart;
    return left < u32(groupSize) ? left : u32(groupSize);
}

// 校验净荷长度 = 组内各片长度的最大值
inline size_t parity_len(size_t frameBytes, u32 groupStart, u32 members) {
    size_t n = 0;
    for (u32 i = 0; i < members; ++i) {
        const size_t l = frag_len(frameBytes, groupStart + i);
        if (l > n) n = l;
    }
    return n;
}

// 逐字节 XOR（每个分片补零到 parityLen 后参与）。dst 必须已经清零。
inline void xor_into(u8* dst, size_t parityLen, const u8* part, size_t partLen) {
    if (dst == nullptr || part == nullptr) return;
    if (partLen > parityLen) partLen = parityLen;
    for (size_t i = 0; i < partLen; ++i) dst[i] ^= part[i];
}

// 恢复一个缺失分片：把「同组其它已收到的分片」和「校验」一起 XOR 回去。
//   out        : 输出缓冲（长度至少 outLen；实现只会写 outLen 字节）
//   outLen     : 该缺失分片的原始净荷长度 frag_len(frameBytes, missingIndex)
//   parts/lens : 同组其它已收到的分片（不含缺失那片，也不含校验）
//   parity     : 校验净荷；parityLen = parity_len(...)
// 返回值：true = 恢复成功（XOR 可逆，只要只缺 1 片就必然成功）
inline bool xor_recover(u8* out, size_t outLen,
                        const u8* const* parts, const size_t* lens, size_t n,
                        const u8* parity, size_t parityLen) {
    if (out == nullptr || parity == nullptr) return false;
    if (outLen > parityLen) return false;    // 校验比要恢复的分片还短：数据不一致
    std::memset(out, 0, outLen);
    // out = parity XOR 其它分片（都截断到 outLen）
    for (size_t i = 0; i < outLen; ++i) out[i] = parity[i];
    for (size_t k = 0; k < n; ++k) {
        const u8* p = parts[k];
        if (p == nullptr) continue;
        size_t l = lens[k];
        if (l > outLen) l = outLen;
        for (size_t i = 0; i < l; ++i) out[i] ^= p[i];
    }
    return true;
}

} // namespace ms::fec
