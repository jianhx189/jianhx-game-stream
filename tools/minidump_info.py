#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
minidump_info.py - 解析 Windows minidump（WER 崩溃转储），输出：
  * 异常记录（异常码 / 异常地址 / fast-fail 子码）
  * 崩溃线程的寄存器与栈回溯（用 dbghelp + 本机 PDB 解析到函数名+行号）
  * 所有线程的当前 RIP（看谁卡在哪）

只依赖标准库 + 系统 dbghelp.dll，不需要装调试器。

用法：
    python minidump_info.py <dump 文件> [--exe 被控端 exe 路径] [--stack-words 512]
"""
import argparse
import ctypes
import os
import struct
import sys
from ctypes import wintypes

# --------------------------------------------------------------------------
# minidump 常量
# --------------------------------------------------------------------------
STREAM_THREAD_LIST = 3
STREAM_MODULE_LIST = 4
STREAM_MEMORY_LIST = 5
STREAM_EXCEPTION = 6
STREAM_SYSTEM_INFO = 7
STREAM_MEMORY64_LIST = 9

FASTFAIL = {
    0: "LEGACY_GS_VIOLATION",
    1: "VTGUARD_CHECK_FAILURE",
    2: "STACK_COOKIE_CHECK_FAILURE(_invalid_parameter?)",
    3: "CORRUPT_LIST_ENTRY",
    4: "INCORRECT_STACK",
    5: "INVALID_ARG",
    6: "GS_COOKIE_INIT",
    7: "FATAL_APP_EXIT(abort/terminate)",
    8: "RANGE_CHECK_FAILURE",
    9: "RF_CORRUPT_STACK",
    10: "UNSAFE_REGISTRY_ACCESS",
    11: "INVALID_GS_HANDLER",
    12: "CALLED_LONG_JUMP",
    13: "INVALID_JUMP_BUFFER",
    14: "INVALID_CONTEXT",
    15: "INVALID_SET_OF_CONTEXT",
    16: "INVALID_MOP",
    17: "INVALID_EXCEPTION_CHAIN",
    18: "MOP_CORRUPT_STACK",
    19: "INVALID_FAST_FAIL_CODE",
}

# x64 CONTEXT 里的关键偏移
CTX_RIP = 0xF8
CTX_RSP = 0x98
CTX_RBP = 0xA0
CTX_SIZE = 0x4D0


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def u64(b, o):
    return struct.unpack_from("<Q", b, o)[0]


class Dump:
    def __init__(self, path):
        with open(path, "rb") as f:
            self.buf = f.read()
        if self.buf[:4] != b"MDMP":
            raise ValueError("不是 minidump 文件（缺 MDMP 魔数）")
        b = self.buf
        self.nstreams = u32(b, 8)
        self.dir_rva = u32(b, 12)
        self.streams = {}
        for i in range(self.nstreams):
            o = self.dir_rva + i * 12
            stype = u32(b, o)
            dsize = u32(b, o + 4)
            rva = u32(b, o + 8)
            # 同类型只留第一份（线程/模块列表都是唯一的）
            self.streams.setdefault(stype, (rva, dsize))
        self.mem = self._load_memory()
        self.modules = self._load_modules()

    # ---- 内存段（Memory64List 优先，退化为 MemoryList）----
    def _load_memory(self):
        ranges = []  # (start, size_bytes, file_offset)
        if STREAM_MEMORY64_LIST in self.streams:
            rva, _ = self.streams[STREAM_MEMORY64_LIST]
            n = u64(self.buf, rva)
            base_rva = u64(self.buf, rva + 8)
            cur = base_rva
            for i in range(n):
                o = rva + 16 + i * 16
                start = u64(self.buf, o)
                size = u64(self.buf, o + 8)
                ranges.append((start, size, cur))
                cur += size
        elif STREAM_MEMORY_LIST in self.streams:
            rva, _ = self.streams[STREAM_MEMORY_LIST]
            n = u32(self.buf, rva)
            for i in range(n):
                o = rva + 4 + i * 16
                start = u64(self.buf, o)
                size = u32(self.buf, o + 8)
                frva = u32(self.buf, o + 12)
                ranges.append((start, size, frva))
        ranges.sort()
        return ranges

    def read(self, addr, size):
        """从转储里读一段虚拟内存，读不到就返回 None。"""
        for start, msize, foff in self.mem:
            if start <= addr < start + msize:
                avail = start + msize - addr
                n = min(size, avail)
                return self.buf[foff + (addr - start): foff + (addr - start) + n]
            if start > addr:
                break
        return None

    # ---- 模块列表 ----
    def _load_modules(self):
        mods = []
        if STREAM_MODULE_LIST not in self.streams:
            return mods
        rva, _ = self.streams[STREAM_MODULE_LIST]
        n = u32(self.buf, rva)
        for i in range(n):
            o = rva + 4 + i * 108
            base = u64(self.buf, o)
            size = u32(self.buf, o + 8)
            name_rva = u32(self.buf, o + 20)
            nlen = u32(self.buf, name_rva)
            name = self.buf[name_rva + 4: name_rva + 4 + nlen].decode("utf-16-le", "replace")
            mods.append({"base": base, "size": size, "name": name,
                         "ts": u32(self.buf, o + 16)})
        return mods

    def module_of(self, addr):
        for m in self.modules:
            if m["base"] <= addr < m["base"] + m["size"]:
                return m
        return None

    def file_time(self):
        """头部 TimeDateStamp（某些实现放在偏移 20）"""
        return u32(self.buf, 20)

    # ---- 异常 ----
    def exception(self):
        if STREAM_EXCEPTION not in self.streams:
            return None
        rva, _ = self.streams[STREAM_EXCEPTION]
        tid = u32(self.buf, rva)
        code = u32(self.buf, rva + 8)
        flags = u32(self.buf, rva + 12)
        addr = u64(self.buf, rva + 24)
        nparams = u32(self.buf, rva + 32)
        params = [u64(self.buf, rva + 40 + i * 8) for i in range(min(nparams, 15))]
        ctx_rva = u32(self.buf, rva + 164)
        ctx = self.buf[ctx_rva: ctx_rva + CTX_SIZE] if ctx_rva else None
        return {"tid": tid, "code": code, "flags": flags, "addr": addr,
                "params": params, "ctx": ctx}

    # ---- 线程 ----
    def threads(self):
        out = []
        if STREAM_THREAD_LIST not in self.streams:
            return out
        rva, _ = self.streams[STREAM_THREAD_LIST]
        n = u32(self.buf, rva)
        for i in range(n):
            o = rva + 4 + i * 48
            tid = u32(self.buf, o)
            stack_start = u64(self.buf, o + 16)
            stack_size = u32(self.buf, o + 24)
            ctx_rva = u32(self.buf, o + 44)
            ctx = self.buf[ctx_rva: ctx_rva + CTX_SIZE] if ctx_rva else None
            out.append({"tid": tid, "stack_start": stack_start,
                        "stack_size": stack_size, "ctx": ctx})
        return out


# --------------------------------------------------------------------------
# dbghelp 符号解析
# --------------------------------------------------------------------------
class SYMBOL_INFO(ctypes.Structure):
    _fields_ = [
        ("SizeOfStruct", wintypes.ULONG),
        ("TypeIndex", wintypes.ULONG),
        ("Reserved", ctypes.c_ulonglong * 2),
        ("Index", wintypes.ULONG),
        ("Size", wintypes.ULONG),
        ("ModBase", ctypes.c_ulonglong),
        ("Flags", wintypes.ULONG),
        ("Value", ctypes.c_ulonglong),
        ("Address", ctypes.c_ulonglong),
        ("Register", wintypes.ULONG),
        ("Scope", wintypes.ULONG),
        ("Tag", wintypes.ULONG),
        ("NameLen", wintypes.ULONG),
        ("MaxNameLen", wintypes.ULONG),
        ("Name", ctypes.c_char * 2048),
    ]


class IMAGEHLP_LINE64(ctypes.Structure):
    _fields_ = [
        ("SizeOfStruct", wintypes.DWORD),
        ("Key", ctypes.c_void_p),
        ("LineNumber", wintypes.DWORD),
        ("FileName", ctypes.c_char_p),
        ("Address", ctypes.c_ulonglong),
    ]


class Symbols:
    def __init__(self, dump: Dump, exe_hint=None):
        self.dbg = ctypes.WinDLL("dbghelp.dll")
        self.dbg.SymSetOptions(0x00000002 | 0x00000004 | 0x00000010 | 0x00000001)  # UNDNAME|DEFERRED|LOAD_LINES|CASE_INSENSITIVE
        self.hproc = ctypes.c_void_p(0x1234)  # 伪句柄：每个 dump 一套符号
        if not self.dbg.SymInitialize(self.hproc, None, False):
            raise OSError("SymInitialize 失败")
        self.loaded = []
        # 先加载真实 exe（带 PDB），比在转储里找路径靠谱
        pairs = []
        if exe_hint:
            pairs.append((exe_hint, None))
        for m in dump.modules:
            name = m["name"]
            if name.lower().endswith(".exe") and not any(p[0].lower() == name.lower() for p in pairs):
                pairs.append((name, None))
        for path, _ in pairs:
            if not os.path.exists(path):
                continue
            base = None
            for m in dump.modules:
                if m["name"].lower() == path.lower():
                    base = m["base"]
                    size = m["size"]
                    break
            if base is None:
                # exe 路径和转储里的名字对不上时，按文件名匹配
                fn = os.path.basename(path).lower()
                for m in dump.modules:
                    if os.path.basename(m["name"]).lower() == fn:
                        base, size = m["base"], m["size"]
                        break
            if base is None:
                continue
            r = self.dbg.SymLoadModuleExW(self.hproc, None, str(path), None,
                                          ctypes.c_ulonglong(base), size, None, 0)
            if r:
                self.loaded.append((path, base, size))

    def resolve(self, addr):
        buf = SYMBOL_INFO()
        buf.SizeOfStruct = ctypes.sizeof(SYMBOL_INFO)
        buf.MaxNameLen = 2000
        disp = ctypes.c_ulonglong(0)
        if not self.dbg.SymFromAddr(self.hproc, ctypes.c_ulonglong(addr),
                                    ctypes.byref(disp), ctypes.byref(buf)):
            return None
        name = buf.Name.decode("utf-8", "replace")
        line = ""
        li = IMAGEHLP_LINE64()
        li.SizeOfStruct = ctypes.sizeof(IMAGEHLP_LINE64)
        ldisp = wintypes.DWORD(0)
        if self.dbg.SymGetLineFromAddr64(self.hproc, ctypes.c_ulonglong(addr),
                                         ctypes.byref(ldisp), ctypes.byref(li)):
            f = li.FileName.decode("utf-8", "replace") if li.FileName else "?"
            line = "  [%s:%d]" % (os.path.basename(f), li.LineNumber)
        return "%s+0x%x%s" % (name, disp.value, line)


class MapSymbols:
    """用 MSVC 的 .map 文件做符号解析。

    只有当「加了 /MAP 的构建」与崩溃的那个 exe 逐字节一致（或仅 PE 头时间戳不同）
    时才可信，否则函数 RVA 对不上。
    """

    def __init__(self, path):
        self.base = 0
        self.syms = []  # (rva, name)
        with open(path, "r", errors="replace") as f:
            for ln in f:
                if "Preferred load address is" in ln:
                    self.base = int(ln.strip().split()[-1], 16)
                    continue
                parts = ln.split()
                if len(parts) < 3:
                    continue
                # 形如: 0001:0005A120  ?func@@YAXXZ  000000014005B120  f:\...\foo.obj
                try:
                    if ":" not in parts[0]:
                        continue
                    rvabase = int(parts[2], 16)
                except ValueError:
                    continue
                if not parts[1] or parts[1][0] not in "?_$@":
                    continue
                self.syms.append((rvabase - self.base, parts[1]))
        self.syms.sort()

    def resolve(self, rva):
        import bisect
        i = bisect.bisect_right(self.syms, (rva, "\uffff")) - 1
        if i < 0:
            return None
        r, n = self.syms[i]
        return "%s+0x%x" % (n, rva - r) if rva - r else n


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dump")
    ap.add_argument("--exe", default=None)
    ap.add_argument("--map", dest="mapfile", default=None)
    ap.add_argument("--stack-words", type=int, default=768)
    a = ap.parse_args()

    d = Dump(a.dump)
    print("=" * 78)
    print("转储: %s  (%.1f MB, %d 个流, %d 个模块)"
          % (os.path.basename(a.dump), len(d.buf) / 1048576.0, d.nstreams, len(d.modules)))

    exc = d.exception()
    if exc:
        print("-" * 78)
        code = exc["code"]
        print("异常码      : 0x%08X" % code)
        if code == 0xC0000409:
            sub = exc["params"][0] if exc["params"] else -1
            print("  => STATUS_STACK_BUFFER_OVERRUN / __fastfail")
            print("  fast-fail 子码: %d (%s)" % (sub, FASTFAIL.get(sub, "?")))
            if sub == 7:
                print("  => 也就是 abort()：通常来自 std::terminate（有 C++ 异常没人接）")
        m = d.module_of(exc["addr"])
        if m:
            print("异常地址    : 0x%016X  (%s + 0x%X)"
                  % (exc["addr"], os.path.basename(m["name"]), exc["addr"] - m["base"]))
        else:
            print("异常地址    : 0x%016X" % exc["addr"])
        if exc["flags"] & 1:
            print("              （非致命异常，被处理器接管）")
        print("崩溃线程 TID: %d" % exc["tid"])

    # 符号
    sym = None
    try:
        sym = Symbols(d, a.exe)
        print("已加载符号  : %s" % (", ".join(p for p, _, _ in sym.loaded) if sym.loaded else "（无）"))
    except Exception as e:  # noqa
        print("符号加载失败: %s（只能显示模块+偏移）" % e)

    mapsym = None
    map_base = None
    if a.mapfile:
        try:
            mapsym = MapSymbols(a.mapfile)
            print("MAP 符号    : %s（image base 0x%X，%d 个符号）"
                  % (os.path.basename(a.mapfile), mapsym.base, len(mapsym.syms)))
            if a.exe:
                fn = os.path.basename(a.exe).lower()
                for m in d.modules:
                    if os.path.basename(m["name"]).lower() == fn:
                        map_base = m["base"]
                        break
        except Exception as e:  # noqa
            print("MAP 解析失败: %s" % e)

    def fmt(addr):
        m = d.module_of(addr)
        if not m:
            return None
        base = "%s+0x%X" % (os.path.basename(m["name"]), addr - m["base"])
        s = None
        if mapsym and map_base is not None and m["base"] == map_base:
            s = mapsym.resolve(addr - m["base"])
        if s is None and sym:
            s = sym.resolve(addr)
        if s:
            return "%s  %s" % (base, s)
        return base

    # 崩溃线程栈回溯
    if exc and exc["ctx"]:
        rip = u64(exc["ctx"], CTX_RIP)
        rsp = u64(exc["ctx"], CTX_RSP)
        rbp = u64(exc["ctx"], CTX_RBP)
        print("-" * 78)
        print("崩溃线程寄存器: RIP=0x%016X RSP=0x%016X RBP=0x%016X" % (rip, rsp, rbp))
        print("  RIP -> %s" % fmt(rip))
        print("  ---- 栈上的返回地址（从 RSP 向上扫描）----")
        n = a.stack_words * 8
        mem = d.read(rsp, n)
        if mem is None:
            print("  （转储里没有这段栈内存）")
        else:
            shown = 0
            for off in range(0, len(mem) - 8 + 1, 8):
                v = u64(mem, off)
                if v < 0x10000:
                    continue
                m = d.module_of(v)
                if not m:
                    continue
                s = fmt(v)
                if s:
                    mark = "  <== 当前 RIP" if v == rip else ""
                    print("  [rsp+0x%03X] 0x%016X  %s%s" % (off, v, s, mark))
                    shown += 1
                    if shown >= 40:
                        print("  ...（省略）")
                        break

    # 所有线程
    print("-" * 78)
    print("所有线程（共 %d 个）:" % len(d.threads()))
    for t in d.threads():
        if not t["ctx"]:
            continue
        rip = u64(t["ctx"], CTX_RIP)
        sp = u64(t["ctx"], CTX_RSP)
        tag = "  <== 崩溃线程" if exc and t["tid"] == exc["tid"] else ""
        print("  TID %-6d RIP=0x%016X RSP=0x%016X  %s%s"
              % (t["tid"], rip, sp, fmt(rip), tag))
    print("=" * 78)


if __name__ == "__main__":
    main()
