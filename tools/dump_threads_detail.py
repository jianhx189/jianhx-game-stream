#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""dump_threads_detail.py - 全线程 RIP + 栈上返回地址（模块+偏移），卡死分析用。"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from minidump_info import Dump, MapSymbols, u64, CTX_RIP, CTX_RSP


def short(name):
    n = os.path.basename(name)
    return n[:14]


def main():
    d = Dump(sys.argv[1])
    exe_name = os.path.basename(sys.argv[2]).lower() if len(sys.argv) > 2 else None
    mapf = sys.argv[3] if len(sys.argv) > 3 else None
    words = int(sys.argv[4]) if len(sys.argv) > 4 else 256
    ms = MapSymbols(mapf) if mapf else None

    exe_mod = None
    for m in d.modules:
        if exe_name and os.path.basename(m["name"]).lower() == exe_name:
            exe_mod = m
            break

    def label(v):
        m = d.module_of(v)
        if not m:
            return None
        off = v - m["base"]
        if exe_mod and m["base"] == exe_mod["base"] and ms:
            s = ms.resolve(off)
            if s:
                return "%s!%s" % (short(m["name"]), s)
            return "%s+0x%X" % (short(m["name"]), off)
        return "%s+0x%X" % (short(m["name"]), off)

    print("threads=%d  exe_base=%s" % (len(d.threads()),
                                       hex(exe_mod["base"]) if exe_mod else "?"))
    for t in d.threads():
        ctx = t["ctx"]
        if not ctx:
            continue
        rip = u64(ctx, CTX_RIP)
        rsp = u64(ctx, CTX_RSP)
        print("---- TID %d  RIP=%s" % (t["tid"], label(rip) or hex(rip)))
        mem = d.read(rsp, words * 8)
        if not mem:
            print("     (无栈内存)")
            continue
        shown = 0
        for off in range(0, len(mem) - 8 + 1, 8):
            v = u64(mem, off)
            if v < 0x10000:
                continue
            lb = label(v)
            if not lb:
                continue
            print("     [rsp+0x%03X] %s" % (off, lb))
            shown += 1
            if shown >= 10:
                break


if __name__ == "__main__":
    main()
