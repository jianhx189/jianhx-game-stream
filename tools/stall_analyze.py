#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""stall_analyze.py - 卡死转储全线程分析：
  * RIP 与栈帧同时用「本程序 .map」和「系统 DLL 导出表」符号化
  * 标出哪些线程是本程序的线程（栈上有本程序地址），并打印它们的完整链
用法: python stall_analyze.py <dump> <exe> <map> [栈槽数]
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from minidump_info import Dump, MapSymbols, u64, CTX_RIP, CTX_RSP
from pe_exports_from_dump import PeExports


def main():
    dumpf, exef = sys.argv[1], sys.argv[2]
    mapf = sys.argv[3] if len(sys.argv) > 3 else None
    slots = int(sys.argv[4]) if len(sys.argv) > 4 else 160

    d = Dump(dumpf)
    ms = MapSymbols(mapf) if mapf else None
    exe_name = os.path.basename(exef).lower()
    exe_mod = None
    for m in d.modules:
        if os.path.basename(m["name"]).lower() == exe_name:
            exe_mod = m
            break
    if not exe_mod:
        print("exe 未加载")
        return
    exports = {}

    def lab(v):
        m = d.module_of(v)
        if not m:
            return None
        base = m["base"]
        if base == exe_mod["base"] and ms:
            s = ms.resolve(v - base)
            return ("EXE!" + s) if s else ("EXE+0x%X" % (v - base))
        if base not in exports:
            exports[base] = PeExports(d, m)
        s = exports[base].resolve(v)
        return "%s!%s" % (os.path.basename(m["name"]), s) if s else \
               "%s+0x%X" % (os.path.basename(m["name"]), v - base)

    ours, others = [], []
    for t in d.threads():
        ctx = t["ctx"]
        if not ctx:
            continue
        rip = u64(ctx, CTX_RIP)
        rsp = u64(ctx, CTX_RSP)
        frames = []
        mem = d.read(rsp, slots * 8)
        if mem:
            for off in range(0, len(mem) - 8 + 1, 8):
                v = u64(mem, off)
                m = d.module_of(v)
                if not m:
                    continue
                s = lab(v)
                if not s:
                    continue
                frames.append((off, s))
                if len(frames) >= 14:
                    break
        is_ours = any("EXE!" in f[1] or f[1].startswith("EXE+") for f in frames) or \
                  (exe_mod["base"] <= rip < exe_mod["base"] + exe_mod["size"])
        (ours if is_ours else others).append((t["tid"], lab(rip), frames))

    print("=" * 100)
    print("本程序线程（栈上有本程序帧）: %d 个" % len(ours))
    for tid, rip, frames in ours:
        print("-" * 100)
        print("TID %d  RIP=%s" % (tid, rip))
        for off, s in frames:
            print("   [rsp+0x%04X] %s" % (off, s))
    print("=" * 100)
    print("其它线程 %d 个（只列 RIP）:" % len(others))
    for tid, rip, _ in others:
        print("  TID %-7d %s" % (tid, rip))


if __name__ == "__main__":
    main()
