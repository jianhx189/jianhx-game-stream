#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""hang_threads.py - 分析"卡死"转储：对每个线程扫描栈，挑出落在被测 exe 里的返回地址
并用 .map 精确符号化，输出「像是卡在哪个函数」的紧凑视图。

用法:
  python hang_threads.py <dump> <exe> <map> [stack_words]
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from minidump_info import Dump, MapSymbols, u64, CTX_RIP, CTX_RSP


def main():
    dumpf, exef = sys.argv[1], sys.argv[2]
    mapf = sys.argv[3] if len(sys.argv) > 3 else None
    words = int(sys.argv[4]) if len(sys.argv) > 4 else 512

    d = Dump(dumpf)
    ms = MapSymbols(mapf) if mapf else None
    exe_base = os.path.basename(exef).lower()

    mod = None
    for m in d.modules:
        if os.path.basename(m["name"]).lower() == exe_base:
            mod = m
            break
    if not mod:
        print("被测 exe 没被加载？模块名列表:")
        for m in d.modules:
            print("  ", os.path.basename(m["name"]), hex(m["base"]))
        return
    print("exe 模块: %s base=0x%X size=0x%X" % (os.path.basename(mod["name"]),
                                               mod["base"], mod["size"]))
    print("线程数: %d" % len(d.threads()))
    print("=" * 90)

    for t in d.threads():
        ctx = t["ctx"]
        if not ctx:
            continue
        rip = u64(ctx, CTX_RIP)
        rsp = u64(ctx, CTX_RSP)
        frames = []
        mem = d.read(rsp, words * 8)
        if mem:
            for off in range(0, len(mem) - 8 + 1, 8):
                v = u64(mem, off)
                if mod["base"] <= v < mod["base"] + mod["size"]:
                    rva = v - mod["base"]
                    s = ms.resolve(rva) if ms else None
                    if s and s not in frames:
                        frames.append(s)
                if len(frames) >= 8:
                    break
        in_exe = mod["base"] <= rip < mod["base"] + mod["size"]
        rip_s = None
        if in_exe and ms:
            rip_s = ms.resolve(rip - mod["base"])
        print("TID %-6d %s%s" % (t["tid"],
                                 ("RIP=" + (rip_s or hex(rip))) if in_exe else "",
                                 "   <-- 卡在本程序代码里" if in_exe else ""))
        for s in frames:
            print("     栈: %s" % s)
        print("-" * 90)

    d.close() if hasattr(d, "close") else None


if __name__ == "__main__":
    main()
