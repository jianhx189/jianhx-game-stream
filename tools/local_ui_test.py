#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""本机窗口化跑一套 被控端+远控端，自动点一遍工具栏，截图验证 UI：

  1. 小悬浮按钮（≡）常驻
  2. 点它展开工具栏（工具栏要能反复出来 —— 之前"第一次之后就不出来"的 bug）
  3. 展开画质面板
  4. 3 秒自动收起后小按钮回来
"""
import ctypes
import ctypes.wintypes as wt
import os
import subprocess
import sys
import time

from PIL import ImageGrab

u32 = ctypes.windll.user32
BIN = r"D:\远程项目\build\msvc\bin"
OUT = r"D:\远程项目\build\ui_test"
os.makedirs(OUT, exist_ok=True)

u32.FindWindowW.restype = wt.HWND
u32.FindWindowW.argtypes = [wt.LPCWSTR, wt.LPCWSTR]


class RECT(ctypes.Structure):
    _fields_ = [("left", ctypes.c_long), ("top", ctypes.c_long),
                ("right", ctypes.c_long), ("bottom", ctypes.c_long)]


def rect_of(hwnd):
    r = RECT()
    u32.GetWindowRect(hwnd, ctypes.byref(r))
    return r


def click(x, y):
    u32.SetCursorPos(int(x), int(y))
    time.sleep(0.10)
    p = wt.POINT()
    u32.GetCursorPos(ctypes.byref(p))
    if abs(p.x - x) > 3 or abs(p.y - y) > 3:
        print("[warn] 光标没到位: want (%d,%d) got (%d,%d)" % (x, y, p.x, p.y))
    u32.mouse_event(0x0002, 0, 0, 0, 0)  # LEFTDOWN
    time.sleep(0.05)
    u32.mouse_event(0x0004, 0, 0, 0, 0)  # LEFTUP
    time.sleep(0.05)


def toolbar_visible():
    h = u32.FindWindowW("MiniStreamToolbar", None)
    return bool(h) and bool(u32.IsWindowVisible(h))


def shot(name):
    img = ImageGrab.grab()
    p = os.path.join(OUT, name)
    img.save(p)
    print("[shot]", p, img.size)
    return p


def main():
    for p in ("ministream-host", "ministream-client"):
        subprocess.run(["taskkill", "/f", "/im", p + ".exe"], capture_output=True)
    time.sleep(0.5)

    host = subprocess.Popen([os.path.join(BIN, "ministream-host.exe")], cwd=BIN)
    print("[host] pid", host.pid)
    time.sleep(2.0)
    # 同一台机器上做 UI 测试必须强制绝对鼠标模式：否则客户端捕获时会把
    # 系统光标藏起来，被控端（也是这台机器）会认为"光标不可见"，自动切相对模式，
    # 光标被 ClipCursor 钉住，模拟点击就点不到小悬浮按钮了。
    cenv = dict(os.environ)
    cenv["MINISTREAM_MOUSE_MODE"] = "absolute"
    client = subprocess.Popen([
        os.path.join(BIN, "ministream-client.exe"),
        "--auto", "--host", "127.0.0.1", "--port", "14738", "--password", "123",
        "--fullscreen", "0", "--seconds", "90", "--overlay", "1",
    ], cwd=BIN, env=cenv)
    print("[client] pid", client.pid)

    # 等窗口 + 连接
    hwnd = None
    for _ in range(60):
        time.sleep(0.5)
        hwnd = u32.FindWindowW("MiniStreamRenderWnd", None)
        if hwnd:
            break
    if not hwnd:
        print("FATAL: 找不到渲染窗口")
        return 2
    print("[hwnd]", hwnd)
    time.sleep(6.0)   # 等首帧、工具栏初始化、鼠标模式切换

    r = rect_of(hwnd)
    cw, ch = r.right - r.left, r.bottom - r.top
    print("[client rect] %d,%d %dx%d" % (r.left, r.top, cw, ch))

    dock_x = r.left + cw // 2
    dock_y = r.top + 5 + 11
    shot("1_dock.png")

    # 点小悬浮按钮 -> 工具栏
    click(dock_x, dock_y)
    time.sleep(0.7)
    print("[check] 第 1 次点小按钮 -> 工具栏可见:", toolbar_visible())
    shot("2_toolbar.png")

    # 点「画质 ▾」（布局里它在 x≈517, 宽 92，工具栏高 44）
    chev_x = r.left + 517 + 46
    chev_y = r.top + 22
    click(chev_x, chev_y)
    time.sleep(0.6)
    panel = u32.FindWindowW("MiniStreamQualityPanel", None)
    print("[check] 点画质 -> 面板可见:", bool(panel) and bool(u32.IsWindowVisible(panel)))
    shot("3_panel.png")
    click(chev_x, chev_y)   # 收起面板
    time.sleep(0.4)

    # 鼠标移开，等自动收起
    u32.SetCursorPos(r.left + cw // 2, r.top + ch // 2)
    time.sleep(4.5)
    print("[check] 自动收起后 工具栏可见:", toolbar_visible())
    shot("4_after_autohide.png")

    # 再点一次（这是原来会失败的地方）
    click(dock_x, dock_y)
    time.sleep(0.7)
    print("[check] 第 2 次点小按钮 -> 工具栏可见:", toolbar_visible())
    shot("5_toolbar_again.png")
    u32.SetCursorPos(r.left + cw // 2, r.top + ch // 2)
    time.sleep(4.5)

    # 第三次
    click(dock_x, dock_y)
    time.sleep(0.7)
    print("[check] 第 3 次点小按钮 -> 工具栏可见:", toolbar_visible())
    shot("6_toolbar_third.png")

    try:
        client.terminate()
    except Exception:
        pass
    try:
        host.terminate()
    except Exception:
        pass
    time.sleep(0.5)
    for p in ("ministream-host", "ministream-client"):
        subprocess.run(["taskkill", "/f", "/im", p + ".exe"], capture_output=True)
    print("DONE")
    return 0


if __name__ == "__main__":
    sys.exit(main())
