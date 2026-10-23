# 放到客户端机器上跑：完整 jianhx 握手 + 15 秒心跳，测真实公网映射路径
import socket, struct, time, sys, hashlib, os, hmac

TARGET = (os.environ.get("HB_HOST", "your-server.example.com"), int(os.environ.get("HB_PORT", "14738")))
HB_PASSWORD = os.environ.get("HB_PW", "123")
# 严格对齐 src/common/protocol.h 的 MsgType 枚举：
# Hello=1 Challenge=2 Auth=3 AuthResult=4 StreamReady=5 Ping=6 Pong=7
# KeyframeReq=8 SetBitrate=9 Feedback=10 Disconnect=11
MT_HELLO, MT_CHALLENGE, MT_AUTH, MT_AUTHRESULT, MT_STREAMREADY, MT_PING, MT_PONG = 1, 2, 3, 4, 5, 6, 7

def frame(t, p=b""):
    return struct.pack("<I", 4 + len(p)) + struct.pack("<HH", t, 0) + p

def parse(buf):
    # 帧格式：[u32 length][u16 type][u16 reserved][payload...]
    # length = 4 + payload_len，所以 payload 从第 8 字节开始（不是 6！）
    if len(buf) < 8:
        return None, buf
    ln = struct.unpack_from("<I", buf, 0)[0]
    tot = 4 + ln
    if len(buf) < tot:
        return None, buf
    t = struct.unpack_from("<H", buf, 4)[0]
    return (t, buf[8:tot]), buf[tot:]

def recv_frame(sock, timeout=10):
    sock.settimeout(timeout)
    buf = b""
    while True:
        r, buf = parse(buf)
        if r:
            return r
        try:
            buf += sock.recv(8192)
        except socket.timeout:
            return None
        except Exception:
            return None

print("connecting to %s:%d ..." % TARGET)
s = socket.create_connection(TARGET, timeout=10)
print("connected")

nonce = os.urandom(16)
# HelloMsg::serialize 的布局（逐字节对齐 protocol.cpp）：
#   u16 version, u16 flags, nonce[16], u16 wantW, u16 wantH, u16 wantFps,
#   u32 wantBitrate, u8 displayIndex, str8 clientName
# str8 = u8 长度前缀 + 字节内容 —— 之前漏了长度字节，被控端判"Hello 消息格式错误"
client_name = b"diag-client"
hello = (struct.pack("<HH", 1, 0) + nonce
         + struct.pack("<HHHI", 0, 0, 60, 0)
         + b"\x00"
         + struct.pack("<B", len(client_name)) + client_name)
s.sendall(frame(MT_HELLO, hello))
r = recv_frame(s, 10)
print("after Hello: msgtype=%s" % (r[0] if r else "TIMEOUT"))
if not r or r[0] != MT_CHALLENGE:
    print("FAIL: expected Challenge")
    sys.exit(1)

chal = r[1]
server_nonce = chal[:16]
salt = chal[16:32]

key = hashlib.sha256(b"MiniStream/v1" + HB_PASSWORD.encode()).digest()
proof = hmac.new(key, nonce + server_nonce + salt, hashlib.sha256).digest()
if os.environ.get("HB_DEBUG"):
    print("DBG pw=%r key=%s cnonce=%s snonce=%s salt=%s proof=%s"
          % (HB_PASSWORD, key.hex(), nonce.hex(), server_nonce.hex(), salt.hex(), proof.hex()))
s.sendall(frame(MT_AUTH, proof))
r = recv_frame(s, 10)
print("after Auth: msgtype=%s" % (r[0] if r else "TIMEOUT"))
if not r or r[0] != MT_AUTHRESULT:
    print("FAIL: expected AuthResult")
    sys.exit(1)

ar = r[1]
ok = ar[0]
print("auth ok: %d" % ok)
if not ok:
    sys.exit(1)

r = recv_frame(s, 10)
print("after AuthResult: msgtype=%s" % (r[0] if r else "TIMEOUT"))
if r and r[0] == MT_STREAMREADY:
    sr = r[1]
    codec = sr[0]
    w = struct.unpack_from("<H", sr, 1)[0]
    h = struct.unpack_from("<H", sr, 3)[0]
    fps = struct.unpack_from("<H", sr, 5)[0]
    br = struct.unpack_from("<I", sr, 7)[0]
    print("StreamReady: %dx%d @%dfps %dkbps codec=%d" % (w, h, fps, br, codec))

print("heartbeating 20s ...")
t0 = time.time()
pings = 0
pongs = 0
last_pong = time.time()
max_gap = 0.0
rtts = []
recvbuf = b""
s.settimeout(0.2)
next_ping = t0
while time.time() - t0 < 20:
    now = time.time()
    if now >= next_ping:
        seq = pings
        t1 = int(now * 1e6)
        ping = struct.pack("<I", seq) + struct.pack("<q", t1)
        try:
            s.sendall(frame(MT_PING, ping))
            pings += 1
        except Exception as e:
            print("send error: %s" % e)
            break
        next_ping = now + 1.0
    try:
        chunk = s.recv(8192)
        if not chunk:
            print("server closed TCP at t=%.1fs" % (time.time() - t0))
            break
        recvbuf += chunk
    except socket.timeout:
        continue
    except Exception as e:
        print("recv error: %s" % e)
        break
    while True:
        r, recvbuf = parse(recvbuf)
        if not r:
            break
        ty, pl = r
        if ty == MT_PONG and len(pl) >= 28:
            _seq, _t1, _t2, _t3 = struct.unpack_from("<Iqqq", pl, 0)
            pongs += 1
            gap = time.time() - last_pong
            if gap > max_gap:
                max_gap = gap
            last_pong = time.time()
            if _t3 >= _t1:
                rtts.append((_t3 - _t1) / 1000.0)
        else:
            print("  t=%.1fs got unexpected msgtype=%d len=%d" % (time.time() - t0, ty, len(pl)))

print("RESULT: pings=%d pongs=%d max_pong_gap=%.2fs" % (pings, pongs, max_gap))
if rtts:
    rtts.sort()
    print("RTT ms: min=%.1f avg=%.1f p50=%.1f p95=%.1f max=%.1f"
          % (rtts[0], sum(rtts) / len(rtts), rtts[len(rtts)//2],
             rtts[min(len(rtts)-1, int(len(rtts)*0.95))], rtts[-1]))
s.close()
