#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把崩溃线程栈上所有落在「代码区」的地址列出来，并用 map 精确命名。
判断依据：RVА 落在 .text 范围内（< 导入表起始）即视为可能的返回地址。"""
import sys, struct, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from minidump_info import Dump, MapSymbols, SYMBOL_INFO, IMAGEHLP_LINE64, u64, CTX_RIP, CTX_RSP

dump = sys.argv[1]
mapf = sys.argv[2]
span = int(sys.argv[3]) if len(sys.argv) > 3 else 4096

d = Dump(dump)
ms = MapSymbols(mapf)
exc = d.exception()
print("异常码 0x%08X  fastfail=%d" % (exc["code"], exc["params"][0] if exc["params"] else -1))

# 找到崩溃线程
th = None
for t in d.threads():
    if t["tid"] == exc["tid"]:
        th = t
        break
ctx = exc["ctx"]
rip = u64(ctx, CTX_RIP)
rsp = u64(ctx, CTX_RSP)
m = d.module_of(rip)
mod_base = m["base"] if m else None
print("崩溃线程 %d  RIP=0x%X  RSP=0x%X" % (exc["tid"], rip, rsp))

# map 里所有符号，构造「代码函数」判定：RVA 从小到大，取前 N 个在 .text 内的
code = [(rva, n) for rva, n in ms.syms if rva < 0x27190]
code.sort()
code_rvas = [r for r, _ in code]

import bisect
def name_rva(rva):
    i = bisect.bisect_right(code_rvas, rva) - 1
    if i < 0:
        return None
    r, n = code[i]
    # 只在偏移合理时才认为是同一个函数
    if rva - r > 0x4000:
        return None
    return "%s+0x%x" % (n[:110], rva - r)

mem = d.read(rsp, span)
print("=" * 78)
print("栈上落在我方模块内的所有地址（RSP+偏移 -> 绝对地址 -> RVA -> 符号）：")
if not mem:
    print("无栈内存")
for off in range(0, len(mem) - 7, 8):
    v = u64(mem, off)
    mm = d.module_of(v)
    if not mm:
        continue
    rva = v - mm["base"]
    if rva >= 0x27190:      # 数据/IAT 区域
        tag = "  (数据)"
    else:
        s = name_rva(rva)
        tag = "  %s" % s if s else ""
        if mm["base"] != mod_base:
            tag += "  [%s]" % os.path.basename(mm["name"])
    mark = "   <== RIP" if v == rip else ""
    print("  +0x%04X  0x%016X  rva 0x%06X%s%s" % (off, v, rva, tag, mark))
