#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""pe_exports_from_dump.py - 不依赖符号服务器：直接解析 dump 里已加载 DLL 的
导出表，把 RIP/栈上地址映射到「最近的导出函数」。

卡死分析时，这能区分线程等的是 NtWaitForSingleObject / RtlpWaitForCriticalSection
（锁）还是 NtDeviceIoControlFile（网络）等关键系统调用。

用法:
  python pe_exports_from_dump.py <dump> <目标TID,...> [栈槽数]
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from minidump_info import Dump, u64, CTX_RIP, CTX_RSP


def u16(b, o):
    return struct.unpack_from("<H", b, o)[0]


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


class PeExports:
    def __init__(self, d, mod):
        self.base = mod["base"]
        self.syms = []
        try:
            hdr = d.read(self.base, 0x400)
            if not hdr or hdr[:2] != b"MZ":
                return
            pe = u32(hdr, 0x3C)
            full = d.read(self.base, pe + 0x110)
            if not full or full[pe:pe + 4] != b"PE\0\0":
                return
            opt = pe + 24
            magic = u16(full, opt)
            dd = opt + (0x70 if magic == 0x20B else 0x60)
            exp_rva = u32(full, dd + 0 * 8)
            if exp_rva == 0:
                return
            exp = d.read(self.base + exp_rva, 40)
            nnames = u32(exp, 24)
            funcs_rva = u32(exp, 28)
            names_rva = u32(exp, 32)
            ords_rva = u32(exp, 36)
            names_rvas = d.read(self.base + names_rva, 4 * nnames)
            ords = d.read(self.base + ords_rva, 2 * nnames)
            funcs = d.read(self.base + funcs_rva, 4 * max(nnames, 1) * 4)
            if not (names_rvas and ords and funcs):
                return
            for i in range(nnames):
                name_rva = u32(names_rvas, i * 4)
                nm = d.read(self.base + name_rva, 96)
                if not nm:
                    continue
                nm = nm.split(b"\0")[0].decode("ascii", "replace")
                idx = u16(ords, i * 2)
                frva = u32(funcs, idx * 4)
                self.syms.append((frva, nm))
            self.syms.sort()
        except Exception:
            pass

    def resolve(self, addr):
        rva = addr - self.base
        lo, hi = 0, len(self.syms) - 1
        best = None
        while lo <= hi:
            mid = (lo + hi) // 2
            if self.syms[mid][0] <= rva:
                best = self.syms[mid]
                lo = mid + 1
            else:
                hi = mid - 1
        if not best:
            return None
        return "%s+0x%X" % (best[1], rva - best[0])


def main():
    dumpf = sys.argv[1]
    tids = set(int(x) for x in sys.argv[2].split(",")) if len(sys.argv) > 2 else set()
    slots = int(sys.argv[3]) if len(sys.argv) > 3 else 24
    d = Dump(dumpf)
    cache = {}

    def name_addr(v):
        m = d.module_of(v)
        if not m:
            return None
        key = m["base"]
        if key not in cache:
            cache[key] = PeExports(d, m)
        base = os.path.basename(m["name"])
        s = cache[key].resolve(v)
        return "%s!%s" % (base, s) if s else "%s+0x%X" % (base, v - m["base"])

    for t in d.threads():
        if tids and t["tid"] not in tids:
            continue
        ctx = t["ctx"]
        if not ctx:
            continue
        rip = u64(ctx, CTX_RIP)
        rsp = u64(ctx, CTX_RSP)
        print("==== TID %d" % t["tid"])
        print("  RIP: %s" % (name_addr(rip) or hex(rip)))
        mem = d.read(rsp, slots * 8)
        if not mem:
            continue
        for off in range(0, len(mem) - 8 + 1, 8):
            v = u64(mem, off)
            s = name_addr(v)
            if s:
                print("  [rsp+0x%03X] %s" % (off, s))


if __name__ == "__main__":
    main()
