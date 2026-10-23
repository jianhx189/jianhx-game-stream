#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""在被控端 14738 上跑 UDP echo，用来验证 FRP 的 UDP 映射是否双向通。
用法: python tools/udp_echo.py [端口]
"""
import socket
import sys

port = int(sys.argv[1]) if len(sys.argv) > 1 else 14738
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(("0.0.0.0", port))
print("UDP echo listening on 0.0.0.0:%d" % port, flush=True)
n = 0
while True:
    data, addr = s.recvfrom(4096)
    n += 1
    print("[%d] from %s:%d len=%d data=%r" % (n, addr[0], addr[1], len(data), data[:32]), flush=True)
    s.sendto(b"ECHO:" + data, addr)
