#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""抓真客户端发出的 Hello / Auth 原始字节，用 Python 独立算出 proof 做对照。

用法：
  1) python tools/capture_hello.py            # 监听 127.0.0.1:14739
  2) ministream-client.exe --host 127.0.0.1 --port 14739 --password 123
只做诊断，不改任何被测代码。
"""
import hashlib
import hmac
import socket
import struct
import sys

PORT = 14739
PASSWORD = "123"


def recv_frame(conn):
    hdr = b""
    while len(hdr) < 8:
        c = conn.recv(8 - len(hdr))
        if not c:
            return None, b""
        hdr += c
    ln = struct.unpack_from("<I", hdr, 0)[0]
    ty = struct.unpack_from("<H", hdr, 4)[0]
    body = b""
    need = ln - 4
    while len(body) < need:
        c = conn.recv(need - len(body))
        if not c:
            break
        body += c
    return ty, body


def send_frame(conn, ty, body):
    conn.sendall(struct.pack("<I", 4 + len(body)) + struct.pack("<HH", ty, 0) + body)


def main():
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", PORT))
    srv.listen(1)
    print("listening on 127.0.0.1:%d ..." % PORT)
    sys.stdout.flush()

    conn, addr = srv.accept()
    print("client from %s" % (addr,))
    ty, hello = recv_frame(conn)
    print("frame1 type=%d len=%d hex=%s" % (ty, len(hello), hello.hex()))
    # Hello: u16 version, u16 flags, nonce[16], ...
    ver, flags = struct.unpack_from("<HH", hello, 0)
    client_nonce = hello[4:20]
    print("  version=%d flags=%d clientNonce=%s" % (ver, flags, client_nonce.hex()))

    server_nonce = bytes([0x11]) * 16
    salt = bytes([0x22]) * 16
    send_frame(conn, 2, server_nonce + salt)
    print("  sent Challenge (nonce=11.. salt=22..)")

    ty, auth = recv_frame(conn)
    print("frame2 type=%d len=%d proof=%s" % (ty, len(auth), auth.hex()))

    key = hashlib.sha256(b"MiniStream/v1" + PASSWORD.encode()).digest()
    expect = hmac.new(key, client_nonce + server_nonce + salt, hashlib.sha256).digest()
    print("python expect proof=%s" % expect.hex())
    print("MATCH" if expect == auth else "MISMATCH")
    conn.close()
    srv.close()


if __name__ == "__main__":
    main()
