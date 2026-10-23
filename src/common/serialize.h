// =============================================================================
//  serialize.h - 显式字节序序列化（头文件实现）
//
//  为什么不用 memcpy 结构体：MSVC / GCC 的 struct packing 与对齐规则不同，
//  网络协议必须字节级确定。这里统一小端。
// =============================================================================
#pragma once

#include "types.h"
#include <cstring>
#include <string>

namespace ms {

class ByteWriter {
public:
    explicit ByteWriter(Bytes& out) : out_(out) {}

    void u8v(u8 v) { out_.push_back(v); }
    void u16v(u16 v) { out_.push_back(u8(v & 0xFF)); out_.push_back(u8((v >> 8) & 0xFF)); }
    void u32v(u32 v) {
        out_.push_back(u8(v & 0xFF));         out_.push_back(u8((v >> 8) & 0xFF));
        out_.push_back(u8((v >> 16) & 0xFF)); out_.push_back(u8((v >> 24) & 0xFF));
    }
    void i32v(i32 v) { u32v(u32(v)); }
    void i64v(i64 v) { u32v(u32(u64(v) & 0xFFFFFFFFu)); u32v(u32((u64(v) >> 32) & 0xFFFFFFFFu)); }
    void raw(const void* p, size_t n) {
        const u8* b = static_cast<const u8*>(p);
        out_.insert(out_.end(), b, b + n);
    }
    void fill(u8 v, size_t n) { out_.insert(out_.end(), n, v); }
    // 短字符串：u8 长度 + 字节
    void str8(const std::string& s) {
        const size_t n = s.size() > 255 ? 255 : s.size();
        u8v(u8(n));
        raw(s.data(), n);
    }

private:
    Bytes& out_;
};

class ByteReader {
public:
    ByteReader(const u8* p, size_t n) : p_(p), n_(n) {}
    explicit ByteReader(const Bytes& b) : p_(b.data()), n_(b.size()) {}

    bool u8v(u8& v) {
        if (left() < 1) return false;
        v = *p_++; --n_; return true;
    }
    bool u16v(u16& v) {
        if (left() < 2) return false;
        v = u16(p_[0] | (p_[1] << 8)); p_ += 2; n_ -= 2; return true;
    }
    bool u32v(u32& v) {
        if (left() < 4) return false;
        v = u32(p_[0]) | (u32(p_[1]) << 8) | (u32(p_[2]) << 16) | (u32(p_[3]) << 24);
        p_ += 4; n_ -= 4; return true;
    }
    bool i32v(i32& v) { u32 t; if (!u32v(t)) return false; v = i32(t); return true; }
    bool i64v(i64& v) {
        u32 lo, hi;
        if (!u32v(lo) || !u32v(hi)) return false;
        v = i64((u64(hi) << 32) | u64(lo)); return true;
    }
    bool raw(void* dst, size_t n) {
        if (left() < n) return false;
        std::memcpy(dst, p_, n); p_ += n; n_ -= n; return true;
    }
    bool skip(size_t n) {
        if (left() < n) return false;
        p_ += n; n_ -= n; return true;
    }
    bool str8(std::string& s) {
        u8 n;
        if (!u8v(n)) return false;
        if (left() < n) return false;
        s.assign(reinterpret_cast<const char*>(p_), n);
        p_ += n; n_ -= n; return true;
    }
    // 把剩余全部当作字符串（用于容错解析）
    std::string rest_string() {
        std::string s(reinterpret_cast<const char*>(p_), left());
        p_ += left(); n_ = 0;
        return s;
    }

    size_t left() const { return n_; }
    const u8* cur() const { return p_; }
    bool ok_all() const { return n_ == 0; }

private:
    const u8* p_;
    size_t n_;
};

} // namespace ms
