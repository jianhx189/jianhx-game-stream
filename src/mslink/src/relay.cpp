// =============================================================================
//  mslink/relay.cpp —— 2 端口复用通道实现
//
//  【P1 修复记录】
//  - P1-1: 拆分 send_raw_queued / send_tcp_frame_queued，本地程序与 Sunshine 两侧原样转发
//  - P1-2: FrameReader 保留剩余数据，修复 TCP 粘包握手失败
//  - P1-3: 隧道 socket 握手后显式设非阻塞，所有转发统一走对应发送队列
//  - P1-4: UDP 认证密钥明文传输 + 防重放（序号窗口）
// =============================================================================
#include "mslink/relay.h"
#include "mslink/sequence_window.h"
#include <sstream>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <windows.h>
#include <bcrypt.h>

#include <atomic>
#include <cstdarg>
#include <cstring>
#include <cstdio>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "bcrypt.lib")

namespace mslink {

// ---------------------------------------------------------------------------
// 基础设施
// ---------------------------------------------------------------------------
static void (*g_logSink)(const char*, void*) = nullptr;
static void* g_logCtx = nullptr;

static std::mutex g_logMtx;

static void logf(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> lk(g_logMtx);
    if (g_logSink) g_logSink(buf, g_logCtx);
    else { fprintf(stderr, "[mslink] %s\n", buf); fflush(stderr); }
}

static std::atomic<int> g_connSeq{0};

static uint64_t now_ms() {
    return uint64_t(::GetTickCount64());
}

static std::mutex g_mediaMutex;
static std::string g_mediaJson;
static uint64_t g_mediaAt = 0;
void publish_media_metrics(const std::string& json) {
    if(json.size()>4096)return;
    std::lock_guard<std::mutex> lock(g_mediaMutex);g_mediaJson=json;g_mediaAt=now_ms();
}
static std::string json_quote(const std::string& s) {
    std::string out="\"";
    for(unsigned char c:s){if(c=='"'||c=='\\'){out+='\\';out+=c;}else if(c<32){char b[7];snprintf(b,sizeof(b),"\\u%04x",c);out+=b;}else out+=c;}
    return out+'"';
}
static uint64_t unix_ms(){FILETIME ft;GetSystemTimeAsFileTime(&ft);ULARGE_INTEGER n;n.LowPart=ft.dwLowDateTime;n.HighPart=ft.dwHighDateTime;return n.QuadPart/10000-11644473600000ULL;}
static std::string device_name(){wchar_t name[256];DWORD size=256;if(!GetComputerNameW(name,&size))return "Windows client";int count=WideCharToMultiByte(CP_UTF8,0,name,size,nullptr,0,nullptr,nullptr);std::string out(count,0);WideCharToMultiByte(CP_UTF8,0,name,size,&out[0],count,nullptr,nullptr);return out;}

static bool sock_valid(SOCKET s) { return s != INVALID_SOCKET; }
static void sock_close(SOCKET& s) { if (sock_valid(s)) { ::closesocket(s); s = INVALID_SOCKET; } }

static bool set_nonblocking(SOCKET s) {
    u_long nb = 1;
    return ::ioctlsocket(s, FIONBIO, &nb) == 0;
}

// ---------------------------------------------------------------------------
//  放大套接字缓冲区
// ---------------------------------------------------------------------------
constexpr int kSockBufBytes = 4 * 1024 * 1024;

static void tune_buffers(SOCKET s, const char* tag, int bytes = kSockBufBytes) {
    int v = bytes;
    if (::setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char*)&v, sizeof(v)) != 0) {
        logf("警告：%s SO_RCVBUF 设置失败（WSA=%d）", tag, ::WSAGetLastError());
    }
    if (::setsockopt(s, SOL_SOCKET, SO_SNDBUF, (const char*)&v, sizeof(v)) != 0) {
        logf("警告：%s SO_SNDBUF 设置失败（WSA=%d）", tag, ::WSAGetLastError());
    }
    int rcvGot = 0, sndGot = 0;
    socklen_t gl = sizeof(int);
    ::getsockopt(s, SOL_SOCKET, SO_RCVBUF, (char*)&rcvGot, &gl);
    gl = sizeof(int);
    ::getsockopt(s, SOL_SOCKET, SO_SNDBUF, (char*)&sndGot, &gl);
    if (rcvGot > 0 && rcvGot < bytes / 2) {
        logf("提示：%s 实际接收缓冲 %d KB（请求 %d KB），大码率下仍可能丢包",
             tag, rcvGot / 1024, bytes / 1024);
    }
}

static bool resolve_v4(const std::string& host, uint16_t port, sockaddr_in& out) {
    std::memset(&out, 0, sizeof(out));
    out.sin_family = AF_INET;
    out.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &out.sin_addr) == 1) return true;
    addrinfo hints{};
    hints.ai_family = AF_INET;
    addrinfo* res = nullptr;
    if (::getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) return false;
    out.sin_addr = reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr;
    ::freeaddrinfo(res);
    return true;
}

// ---------------------------------------------------------------------------
//  P1-4: 会话密钥派生（TCP 认证后生成，UDP 包校验用）
// ---------------------------------------------------------------------------
static bool gen_random(void* buf, size_t len) {
    return BCryptGenRandom(nullptr, (PUCHAR)buf, (ULONG)len,
                           BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
}

static void derive_session_key(const std::string& password, uint32_t nonce,
                               uint8_t out[32]) {
    uint64_t h1 = 0xcbf29ce484222325ull;
    uint64_t h2 = 0x9e3779b97f4a7c15ull;
    uint64_t h3 = 0x2545F4914F6CDD1Dull;
    uint64_t h4 = 0x9E3779B97F4A7C15ull;
    auto mix = [](uint64_t& h, uint8_t b) {
        h ^= b; h *= 0x100000001b3ull; h ^= (h >> 29);
    };
    for (unsigned char c : password) {
        mix(h1, c); mix(h2, uint8_t(c ^ 0x5A));
        mix(h3, uint8_t(c ^ 0xA5)); mix(h4, uint8_t(c ^ 0x3C));
    }
    for (int i = 0; i < 4; ++i) {
        mix(h1, uint8_t((nonce >> (8 * i)) & 0xFF));
        mix(h2, uint8_t(((nonce >> (8 * i)) & 0xFF) ^ 0xA5));
        mix(h3, uint8_t(((nonce >> (8 * i)) & 0xFF) ^ 0x5A));
        mix(h4, uint8_t(((nonce >> (8 * i)) & 0xFF) ^ 0x3C));
    }
    for (int i = 0; i < 8; ++i) {
        out[i]      = uint8_t((h1 >> (8 * i)) & 0xFF);
        out[8 + i]  = uint8_t((h2 >> (8 * i)) & 0xFF);
        out[16 + i] = uint8_t((h3 >> (8 * i)) & 0xFF);
        out[24 + i] = uint8_t((h4 >> (8 * i)) & 0xFF);
    }
}

// 用会话密钥计算 UDP 包认证 tag（8 字节）
static void udp_auth_tag(const uint8_t key[32], uint8_t ptype, uint16_t flowId,
                         uint32_t seq, const void* payload, size_t payloadLen, uint8_t out[8]) {
    uint64_t h1 = 0xcbf29ce484222325ull;
    uint64_t h2 = 0x9e3779b97f4a7c15ull;
    auto mix = [](uint64_t& h, uint8_t b) {
        h ^= b; h *= 0x100000001b3ull; h ^= (h >> 29);
    };
    for (int i = 0; i < 32; ++i) { mix(h1, key[i]); mix(h2, uint8_t(key[i] ^ 0x5A)); }
    mix(h1, ptype); mix(h2, uint8_t(ptype ^ 0x5A));
    mix(h1, uint8_t(flowId & 0xFF)); mix(h1, uint8_t((flowId >> 8) & 0xFF));
    mix(h2, uint8_t((flowId & 0xFF) ^ 0x5A)); mix(h2, uint8_t(((flowId >> 8) & 0xFF) ^ 0x5A));
    for (int i = 0; i < 4; ++i) {
        mix(h1, uint8_t((seq >> (8 * i)) & 0xFF));
        mix(h2, uint8_t(((seq >> (8 * i)) & 0xFF) ^ 0xA5));
    }
    const uint8_t* p = static_cast<const uint8_t*>(payload);
    for (size_t i = 0; i < payloadLen; ++i) { mix(h1, p[i]); mix(h2, uint8_t(p[i] ^ 0xA5)); }
    for (int i = 0; i < 8; ++i) {
        out[i] = uint8_t((h1 >> (8 * i)) & 0xFF) ^ uint8_t((h2 >> (8 * i)) & 0xFF);
    }
}

// P0 轻量 keyed 校验
static void make_token(const std::string& pw, uint32_t nonce, uint8_t out[16]) {
    uint64_t h1 = 0xcbf29ce484222325ull;
    uint64_t h2 = 0x9e3779b97f4a7c15ull;
    auto mix = [](uint64_t& h, uint8_t b) { h ^= b; h *= 0x100000001b3ull; h ^= (h >> 29); };
    for (unsigned char c : pw) { mix(h1, c); mix(h2, uint8_t(c ^ 0x5A)); }
    for (int i = 0; i < 4; ++i) {
        mix(h1, uint8_t((nonce >> (8 * i)) & 0xFF));
        mix(h2, uint8_t(((nonce >> (8 * i)) & 0xFF) ^ 0xA5));
    }
    for (int i = 0; i < 8; ++i) {
        out[i]     = uint8_t((h1 >> (8 * i)) & 0xFF);
        out[8 + i] = uint8_t((h2 >> (8 * i)) & 0xFF);
    }
}

// ---- TCP 帧 ---------------------------------------------------------------
// [u32 payloadLen][u16 type][u16 streamId][payload...]

// P1-3: 非阻塞发送队列。每个 socket 一个队列，WSAEWOULDBLOCK 时入队，POLLOUT 时恢复。
struct OutQueue {
    struct Item {
        std::vector<uint8_t> data;
        size_t offset = 0;
    };
    std::deque<Item> items;
    size_t totalBytes = 0;
    static constexpr size_t kMaxQueueBytes = 4 * 1024 * 1024;  // 4MB 上限

    bool empty() const { return items.empty(); }

    // 容量预留和入队是一个原子动作：预留失败时队列保持原样。
    // offset 表示调用方已经直接发出、无需再入队的前缀。
    bool push(const void* data, size_t len, size_t offset = 0) {
        if (offset > len) return false;
        const size_t pending = len - offset;
        if (pending > kMaxQueueBytes || totalBytes > kMaxQueueBytes - pending) return false;
        Item it;
        const uint8_t* p = static_cast<const uint8_t*>(data);
        it.data.assign(p + offset, p + len);
        it.offset = 0;
        items.push_back(std::move(it));
        totalBytes += pending;
        return true;
    }

    // 尝试发送队列头部数据。返回：0=全部发完，1=部分发送（需等可写），-1=错误关闭。
    int flush(SOCKET s) {
        while (!items.empty()) {
            Item& front = items.front();
            const char* p = reinterpret_cast<const char*>(front.data.data()) + front.offset;
            size_t remaining = front.data.size() - front.offset;
            if (remaining == 0) {
                totalBytes -= front.data.size();
                items.pop_front();
                continue;
            }
            int n = ::send(s, p, int(remaining), 0);
            if (n > 0) {
                front.offset += size_t(n);
                if (front.offset >= front.data.size()) {
                    totalBytes -= front.data.size();
                    items.pop_front();
                }
            } else if (n == 0) {
                return -1;  // 对端关闭
            } else {
                int err = ::WSAGetLastError();
                if (err == WSAEWOULDBLOCK) {
                    return 1;  // 等可写
                }
                return -1;  // 真正错误
            }
        }
        return 0;
    }
};

// P1-1: 发送一段连续字节。能直接发完就直接发；遇到 WSAEWOULDBLOCK
// 时把尚未发送的剩余部分作为一个整体入队。返回 false 表示连接错误或
// 队列容量不足，调用方必须关闭对应流/隧道；不会静默丢弃数据。
static bool send_blob_queued(SOCKET s, OutQueue& oq, const uint8_t* data, size_t len) {
    if (!sock_valid(s)) return false;
    if (len == 0) return true;

    // 先尝试冲刷已有队列
    if (!oq.empty()) {
        int rc = oq.flush(s);
        if (rc < 0) return false;
        if (rc == 1) {
            // 队列还没发完，新数据整体入队；失败时队列保持原样。
            return oq.push(data, len);
        }
    }

    // 队列为空，尝试直接发送
    size_t sent = 0;
    while (sent < len) {
        int n = ::send(s, reinterpret_cast<const char*>(data) + sent, int(len - sent), 0);
        if (n > 0) { sent += size_t(n); continue; }
        if (n == 0) return false;
        int err = ::WSAGetLastError();
        if (err == WSAEWOULDBLOCK) {
            // 只把未发送的部分挂回队列，offset 指向已发送前缀。
            return oq.push(data, len, sent);
        }
        return false;
    }
    return true;
}

// P1-1: 发送原始字节（不加帧头）。用于本地程序与 Sunshine 之间的直接转发。
static bool send_raw_queued(SOCKET s, OutQueue& oq, const void* data, size_t len) {
    return send_blob_queued(s, oq, static_cast<const uint8_t*>(data), len);
}

// P1-1: 发送 TCP 帧（加 8 字节帧头）。仅用于两个 mslink 之间的隧道通信。
// 整帧先拼成一个连续缓冲区再交给 send_blob_queued，保证队列里不会出现
// 「只有帧头没有 payload」的半帧；超过单帧上限直接拒绝。
static bool send_tcp_frame_queued(SOCKET s, OutQueue& oq, uint16_t type, uint16_t streamId,
                                  const void* payload, size_t payloadLen) {
    if (!sock_valid(s)) return false;
    if (payloadLen > kMaxTcpFrame) return false;

    uint8_t hdr[8];
    uint32_t len = uint32_t(payloadLen);
    hdr[0] = uint8_t(len & 0xFF); hdr[1] = uint8_t((len >> 8) & 0xFF);
    hdr[2] = uint8_t((len >> 16) & 0xFF); hdr[3] = uint8_t((len >> 24) & 0xFF);
    hdr[4] = uint8_t(type & 0xFF); hdr[5] = uint8_t((type >> 8) & 0xFF);
    hdr[6] = uint8_t(streamId & 0xFF); hdr[7] = uint8_t((streamId >> 8) & 0xFF);

    std::vector<uint8_t> frame;
    frame.reserve(sizeof(hdr) + payloadLen);
    frame.insert(frame.end(), hdr, hdr + sizeof(hdr));
    if (payloadLen) {
        const uint8_t* p = static_cast<const uint8_t*>(payload);
        frame.insert(frame.end(), p, p + payloadLen);
    }
    return send_blob_queued(s, oq, frame.data(), frame.size());
}

// 旧接口保留（同步阻塞版本，仅用于握手阶段）
static bool send_all(SOCKET s, const void* data, size_t len) {
    const char* p = static_cast<const char*>(data);
    while (len > 0) {
        int n = ::send(s, p, int(len), 0);
        if (n <= 0) return false;
        p += n; len -= size_t(n);
    }
    return true;
}

static bool send_tcp_frame(SOCKET s, uint16_t type, uint16_t streamId,
                           const void* payload, size_t payloadLen) {
    if (!sock_valid(s) || payloadLen > kMaxTcpFrame) return false;
    uint8_t hdr[8];
    uint32_t len = uint32_t(payloadLen);
    hdr[0] = uint8_t(len & 0xFF); hdr[1] = uint8_t((len >> 8) & 0xFF);
    hdr[2] = uint8_t((len >> 16) & 0xFF); hdr[3] = uint8_t((len >> 24) & 0xFF);
    hdr[4] = uint8_t(type & 0xFF); hdr[5] = uint8_t((type >> 8) & 0xFF);
    hdr[6] = uint8_t(streamId & 0xFF); hdr[7] = uint8_t((streamId >> 8) & 0xFF);
    if (!send_all(s, hdr, sizeof(hdr))) return false;
    if (payloadLen && !send_all(s, payload, payloadLen)) return false;
    return true;
}

// P1-2: FrameReader 保留剩余数据，修复 TCP 粘包；同时把单帧长度限制在
// kMaxTcpFrame，收到 8 字节头即可拒绝超长帧，缓冲区保持有界。
struct FrameReader {
    std::vector<uint8_t> buf;
    size_t offset = 0;       // 当前解析位置
    bool badFrame = false;   // 见到过非法帧头或缓冲越界，连接必须关闭

    static constexpr size_t kMaxBuffered = 8 + kMaxTcpFrame + 16 * 1024;

    size_t available() const { return buf.size() - offset; }
    bool bad() const { return badFrame; }

    uint32_t header_len() const {
        return uint32_t(buf[offset]) | (uint32_t(buf[offset + 1]) << 8) |
               (uint32_t(buf[offset + 2]) << 16) | (uint32_t(buf[offset + 3]) << 24);
    }

    bool complete() const {
        if (badFrame || available() < 8) return false;
        const uint32_t len = header_len();
        if (len > kMaxTcpFrame) return false;
        return available() >= 8 + size_t(len);
    }
    size_t want() const {
        if (badFrame || available() < 8) return badFrame ? 0 : 8 - available();
        const uint32_t len = header_len();
        if (len > kMaxTcpFrame) return 0;
        const size_t total = 8 + size_t(len);
        return total > available() ? total - available() : 0;
    }
    // 追加收到的字节。返回 false 表示数据非法，调用方必须关闭连接。
    bool feed(const void* data, size_t len) {
        if (badFrame) return false;
        if (len == 0) return true;
        if (available() >= 8 && header_len() > kMaxTcpFrame) {
            badFrame = true;
            buf.clear();
            offset = 0;
            return false;
        }
        // 丢弃已经消费的前缀，保证缓冲区大小只和未处理数据有关。
        if (offset > 0) {
            buf.erase(buf.begin(), buf.begin() + offset);
            offset = 0;
        }
        if (len > kMaxBuffered || available() > kMaxBuffered - len) {
            badFrame = true;
            buf.clear();
            offset = 0;
            return false;
        }
        const uint8_t* p = static_cast<const uint8_t*>(data);
        buf.insert(buf.end(), p, p + len);
        if (available() >= 8 && header_len() > kMaxTcpFrame) {
            badFrame = true;
            buf.clear();
            offset = 0;
            return false;
        }
        return true;
    }
    bool pop(uint16_t& type, uint16_t& streamId, std::vector<uint8_t>& payload) {
        if (!complete()) return false;
        uint32_t len = header_len();
        type = uint16_t(buf[offset+4] | (buf[offset+5] << 8));
        streamId = uint16_t(buf[offset+6] | (buf[offset+7] << 8));
        payload.assign(buf.begin() + offset + 8, buf.begin() + offset + 8 + len);
        offset += 8 + len;
        // 如果全部消费完，清空 buffer 重置 offset
        if (offset >= buf.size()) {
            buf.clear();
            offset = 0;
        } else if (available() >= 8 && header_len() > kMaxTcpFrame) {
            // 同一批数据里紧跟合法帧的超长头也必须立刻拒绝，不能等下一次 recv。
            badFrame = true;
            buf.clear();
            offset = 0;
        }
        return true;
    }
    // 新连接时重置
    void reset() {
        buf.clear();
        offset = 0;
        badFrame = false;
    }
};

// ---- UDP 包 ---------------------------------------------------------------
// P1-4 新格式：[u8 magic][u8 type][u16 flowId][u32 seq][u32 sessionId][u8 tag[8]][payload...]
static constexpr size_t kUdpHdr = 20;  // 1+1+2+4+4+8 = 20

// A wildcard UDP socket must reply from the address that received the packet.
// sendto() alone can select another local address on multi-homed Windows hosts,
// which connected UDP forwarders then discard as an unexpected source endpoint.
struct UdpReplyRoute {
    IN_PKTINFO info{};
    bool valid = false;
};

struct UdpPacketIo {
    LPFN_WSARECVMSG receive = nullptr;
    LPFN_WSASENDMSG send = nullptr;

    bool enable(SOCKET socket) {
        GUID receiveId = WSAID_WSARECVMSG, sendId = WSAID_WSASENDMSG;
        DWORD size = 0, enabled = 1;
        return WSAIoctl(socket, SIO_GET_EXTENSION_FUNCTION_POINTER,
                        &receiveId, sizeof(receiveId), &receive, sizeof(receive),
                        &size, nullptr, nullptr) == 0 &&
               WSAIoctl(socket, SIO_GET_EXTENSION_FUNCTION_POINTER,
                        &sendId, sizeof(sendId), &send, sizeof(send),
                        &size, nullptr, nullptr) == 0 &&
               setsockopt(socket, IPPROTO_IP, IP_PKTINFO,
                          reinterpret_cast<const char*>(&enabled), sizeof(enabled)) == 0;
    }

    int recv(SOCKET socket, uint8_t* buffer, size_t capacity,
             sockaddr_in& from, UdpReplyRoute& route) const {
        WSABUF data{ULONG(capacity), reinterpret_cast<char*>(buffer)};
        alignas(WSACMSGHDR) char control[WSA_CMSG_SPACE(sizeof(IN_PKTINFO))]{};
        WSAMSG message{};
        message.name = reinterpret_cast<sockaddr*>(&from);
        message.namelen = sizeof(from);
        message.lpBuffers = &data;
        message.dwBufferCount = 1;
        message.Control = {ULONG(sizeof(control)), control};
        DWORD received = 0;
        route = {};
        if (receive(socket, &message, &received, nullptr, nullptr) != 0) return -1;
        if (!(message.dwFlags & MSG_CTRUNC)) {
            for (auto header = WSA_CMSG_FIRSTHDR(&message); header;
                 header = WSA_CMSG_NXTHDR(&message, header)) {
                if (header->cmsg_level == IPPROTO_IP && header->cmsg_type == IP_PKTINFO &&
                    header->cmsg_len >= WSA_CMSG_LEN(sizeof(IN_PKTINFO))) {
                    std::memcpy(&route.info, WSA_CMSG_DATA(header), sizeof(route.info));
                    route.valid = route.info.ipi_addr.s_addr != INADDR_ANY;
                    break;
                }
            }
        }
        return int(received);
    }
};

static bool send_udp_packet(SOCKET s, const sockaddr_in& to, uint8_t type,
                            uint16_t flowId, uint32_t seq,
                            const uint8_t sessionKey[32],
                            const void* payload, size_t payloadLen,
                            const UdpReplyRoute* route = nullptr,
                            LPFN_WSASENDMSG sendMessage = nullptr) {
    if (!sock_valid(s) || payloadLen > kMaxUdpPayload) return false;
    uint8_t pkt[kUdpHdr + kMaxUdpPayload];
    pkt[0] = kUdpMagic;
    pkt[1] = type;
    pkt[2] = uint8_t(flowId & 0xFF);
    pkt[3] = uint8_t((flowId >> 8) & 0xFF);
    pkt[4] = uint8_t(seq & 0xFF);
    pkt[5] = uint8_t((seq >> 8) & 0xFF);
    pkt[6] = uint8_t((seq >> 16) & 0xFF);
    pkt[7] = uint8_t((seq >> 24) & 0xFF);
    // sessionId = 会话密钥前 4 字节
    pkt[8] = sessionKey[0]; pkt[9] = sessionKey[1];
    pkt[10] = sessionKey[2]; pkt[11] = sessionKey[3];
    // tag = 认证标签（覆盖 type + flowId + seq + payload）
    udp_auth_tag(sessionKey, type, flowId, seq, payload, payloadLen, pkt + 12);
    if (payloadLen) std::memcpy(pkt + kUdpHdr, payload, payloadLen);
    if (route) {
        // Never silently fall back to a route-dependent source address.
        if (!route->valid || !sendMessage) { WSASetLastError(WSAEADDRNOTAVAIL); return false; }
        WSABUF data{ULONG(kUdpHdr + payloadLen), reinterpret_cast<char*>(pkt)};
        alignas(WSACMSGHDR) char control[WSA_CMSG_SPACE(sizeof(IN_PKTINFO))]{};
        WSAMSG message{};
        message.name = reinterpret_cast<sockaddr*>(const_cast<sockaddr_in*>(&to));
        message.namelen = sizeof(to);
        message.lpBuffers = &data;
        message.dwBufferCount = 1;
        message.Control = {ULONG(sizeof(control)), control};
        auto header = WSA_CMSG_FIRSTHDR(&message);
        header->cmsg_level = IPPROTO_IP;
        header->cmsg_type = IP_PKTINFO;
        header->cmsg_len = WSA_CMSG_LEN(sizeof(IN_PKTINFO));
        std::memcpy(WSA_CMSG_DATA(header), &route->info, sizeof(route->info));
        DWORD sent = 0;
        return sendMessage(s, &message, 0, &sent, nullptr, nullptr) == 0 && sent == data.len;
    }
    return ::sendto(s, reinterpret_cast<const char*>(pkt), int(kUdpHdr + payloadLen), 0,
                    reinterpret_cast<const sockaddr*>(&to), sizeof(to)) > 0;
}

static uint32_t flow_key(const sockaddr_in& a) {
    return uint32_t(a.sin_addr.S_un.S_addr) ^ (uint32_t(ntohs(a.sin_port)) << 16);
}

// =============================================================================
//  Impl
// =============================================================================
struct Relay::Impl {
    enum class Mode { None, Client, Host };
    Mode mode = Mode::None;

    ClientConfig ccfg;
    HostConfig   hcfg;

    std::thread th;
    std::thread statsTh;
    std::atomic<bool> stopReq{false};
    std::atomic<bool> runningFlag{false};
    mutable std::mutex statMtx;
    RelayStats st;

    std::atomic<uint64_t> udpBytesUp{0};
    std::atomic<uint64_t> udpBytesDown{0};

    std::atomic<uint64_t> pingSentMs{0};
    std::atomic<uint32_t> rttMs{0};
    std::atomic<uint64_t> lastPongMs{0}, connectedAtMs{0};
    std::atomic<uint32_t> jitterMs{0};
    std::atomic<uint64_t> udpProbeSent{0},udpProbeAck{0},udpAuthRejected{0},udpDataReceived{0};
    std::atomic<uint64_t> udpHelloReceived{0},udpHelloAckSent{0},udpSessionRejected{0},udpReplyErrors{0};
    std::atomic<bool> udpReplyRoutePinned{false};
    std::atomic<int> lastUdpSendError{0};
    std::atomic<uint64_t> lastAuthenticatedUdpMs{0};
    std::atomic<bool> hostFallback{false};
    std::string peerReport;
    uint64_t peerReportAt=0;
    std::string thisDevice=device_name();

    std::atomic<bool> tunnelUp{false};

    // ---- 客户端 ----
    SOCKET tunnelTcp = INVALID_SOCKET;
    SOCKET udpTunnel = INVALID_SOCKET;
    sockaddr_in serverUdpAddr{};
    std::vector<SOCKET> tcpListeners;
    std::vector<SOCKET> udpListeners;
    std::vector<uint16_t> tcpListenPorts;
    std::vector<uint16_t> udpListenPorts;
    FrameReader tcpIn;
    uint32_t udpSeq = 0;
    uint16_t nextStreamId = 1;
    uint16_t nextFlowId = 1;

    struct ClientStream {
        SOCKET sock = INVALID_SOCKET;
        uint16_t id = 0;
        uint16_t targetPort = 0;
        OutQueue outQ;
    };
    std::map<uint16_t, ClientStream> clientStreams;

    struct UdpFlow {
        uint16_t id = 0;
        sockaddr_in clientAddr{};
        SOCKET localSock = INVALID_SOCKET;
        uint64_t lastMs = 0;
    };
    std::map<uint32_t, UdpFlow> clientFlows;
    std::map<uint16_t, uint32_t> clientFlowById;

    OutQueue tunnelOutQ;

    // ---- 被控端 ----
    SOCKET hTcpListen = INVALID_SOCKET;
    SOCKET hUdpListen = INVALID_SOCKET;
    SOCKET hTunnel = INVALID_SOCKET;
    FrameReader hTcpIn;
    sockaddr_in clientUdpAddr{};
    bool clientUdpKnown = false;
    UdpPacketIo hostUdpIo;
    UdpReplyRoute clientUdpRoute;

    OutQueue hTunnelOutQ;

    // ---- UDP 回程探测 / TCP 兜底 ----
    std::atomic<bool> udpDirectOk{false};
    std::atomic<bool> udpViaTcp{false};
    bool     udpModeDecided   = false;
    uint64_t udpProbeStartMs  = 0;
    uint64_t nextUdpHelloMs   = 0;
    uint64_t lastUdpAckMs     = 0;   // 最近一次收到 HELLO_ACK
    int      udpRecoverHits   = 0;   // 从 TCP 兜底切回 UDP 的连续确认次数
    int      udpProbeFails    = 0;

    struct HostStream {
        SOCKET up = INVALID_SOCKET;
        uint16_t targetPort = 0;
        OutQueue outQ;
    };
    std::map<uint16_t, HostStream> hostStreams;

    struct HostFlow { SOCKET up = INVALID_SOCKET; uint64_t lastMs = 0; bool viaTcp = false; uint16_t targetPort = 0; };
    std::map<uint16_t, HostFlow> hostFlows;

    // 未完成握手的连接。host_loop 用 WSAPoll 同时驱动它们，任何一条
    // 慢连接都不会阻塞已经认证的会话，也不会阻塞 accept。
    static constexpr size_t kMaxPendingHandshakes = 8;
    static constexpr uint64_t kHandshakeTimeoutMs = 5000;
    struct PendingHandshake {
        enum class State { WaitToken, AwaitFlush, Closing };
        SOCKET sock = INVALID_SOCKET;
        FrameReader in;
        OutQueue outQ;
        sockaddr_in from{};
        uint32_t nonce = 0;
        uint64_t deadlineMs = 0;
        State state = State::WaitToken;
        uint8_t key[32] = {};
    };
    std::map<SOCKET, PendingHandshake> pendingHandshakes;
    std::mutex pendingMtx;

    // P1-4: 会话密钥
    uint8_t sessionKey[32] = {};
    bool sessionValid = false;

    // P1-4: UDP 序号防重放窗口（每个 flowId 记录最近收到的 seq）
    std::map<uint16_t, SequenceWindow> udpLastSeq;

    // ---- 抗丢包：1+1 冗余发送 ----------------------------------------------
    // 副本与原包使用同一个 (flowId, seq)：接收端现有的 64 包防重放窗口本来就会丢弃
    // 重复 seq，所以不需要任何协议变更，也不需要旧版本对端配合。
    // 只有通过认证、已经发出的数据报才会进入队列，未认证流量无法被放大。
    static constexpr size_t kMaxPendingDups = 256;   // 上限保护（约 300 KB 内存）
    struct PendingUdpDup {
        uint64_t dueMs = 0;
        SOCKET sock = INVALID_SOCKET;
        sockaddr_in to{};
        UdpReplyRoute route{};
        bool hasRoute = false;
        bool useSendMsg = false;
        uint8_t type = kUdpData;
        uint16_t flowId = 0;
        uint32_t seq = 0;
        std::vector<uint8_t> payload;
    };
    std::deque<PendingUdpDup> dupQueue;

    UdpRedundancy redundancy_mode() const {
        return mode == Mode::Host ? hcfg.udpRedundancy : ccfg.udpRedundancy;
    }
    uint16_t redundancy_gap_ms() const {
        const uint16_t g = mode == Mode::Host ? hcfg.udpRedundancyGapMs : ccfg.udpRedundancyGapMs;
        return g > 50 ? 50 : g;
    }
    // Control 模式只保护非视频流：视频占绝对多数带宽，复制它等于上行翻倍。
    bool redundancy_for(uint16_t targetPort) const {
        switch (redundancy_mode()) {
            case UdpRedundancy::All:     return true;
            case UdpRedundancy::Control: return targetPort != kVideoTargetPort;
            default:                     return false;
        }
    }
    void queue_udp_duplicate(SOCKET sock, const sockaddr_in& to, const UdpReplyRoute* route,
                             bool useSendMsg, uint8_t ptype, uint16_t flowId, uint32_t seq,
                             const void* payload, size_t len, uint16_t targetPort) {
        if (!redundancy_for(targetPort)) return;
        if (dupQueue.size() >= kMaxPendingDups) dupQueue.pop_front();
        PendingUdpDup d;
        d.dueMs = now_ms() + redundancy_gap_ms();
        d.sock = sock;
        d.to = to;
        d.hasRoute = route != nullptr;
        if (route) d.route = *route;
        d.useSendMsg = useSendMsg;
        d.type = ptype;
        d.flowId = flowId;
        d.seq = seq;
        const uint8_t* src = static_cast<const uint8_t*>(payload);
        d.payload.assign(src, src + len);
        dupQueue.push_back(std::move(d));
    }
    void pump_udp_duplicates(uint64_t now) {
        while (!dupQueue.empty() && dupQueue.front().dueMs <= now) {
            PendingUdpDup& d = dupQueue.front();
            if (send_udp_packet(d.sock, d.to, d.type, d.flowId, d.seq, sessionKey,
                                d.payload.data(), d.payload.size(),
                                d.hasRoute ? &d.route : nullptr,
                                d.useSendMsg ? hostUdpIo.send : nullptr)) {
                stat([](RelayStats& s) { s.udpRedundantSent++; });
            }
            dupQueue.pop_front();
        }
    }
    // 有待发副本时把 poll 超时收紧到副本到期时刻，保证副本间隔（默认 2 ms）足够精确。
    int poll_timeout_ms() const {
        if (dupQueue.empty()) return 20;
        const uint64_t now = now_ms();
        const uint64_t due = dupQueue.front().dueMs;
        if (due <= now) return 0;
        const uint64_t wait = due - now;
        return wait > 20 ? 20 : int(wait);
    }

    // ---- 统计 ----
    template <typename F> void stat(F fn) {
        std::lock_guard<std::mutex> lk(statMtx);
        fn(st);
    }
    RelayStats snapshot() const { std::lock_guard<std::mutex> lk(statMtx); return st; }
    void set_tunnel_up(bool up) {
        tunnelUp.store(up);
        if(up)connectedAtMs.store(now_ms());else {connectedAtMs.store(0);lastPongMs.store(0);stat([&](RelayStats&){peerReport.clear();peerReportAt=0;});}
        stat([&](RelayStats& s) { s.tunnelUp = up; });
    }

    bool start_client(const ClientConfig& cfg, std::string& err);
    bool start_host(const HostConfig& cfg, std::string& err);
    void stop_impl();

    void client_loop();
    void host_loop();
    void stats_loop();
    std::string telemetry_json() const;
    void record_pong(){const auto sent=pingSentMs.load(),n=now_ms();if(sent && n>=sent){auto sample=uint32_t(n-sent),old=rttMs.exchange(sample);auto diff=sample>old?sample-old:old-sample;jitterMs.store((jitterMs.load()*3+diff)/4);lastPongMs.store(n);}}

    bool client_connect_tunnel();
    void client_close_streams();
    bool client_handle_frame(uint16_t type, uint16_t sid, const std::vector<uint8_t>& pl);
    bool process_client_frames();
    void client_drop_tunnel(const char* why);

    void host_accept_tunnel();
    bool host_handle_frame(uint16_t type, uint16_t sid, const std::vector<uint8_t>& pl);
    bool process_host_frames();
    void host_drop_tunnel(const char* why);
    bool host_process_pending_locked(std::map<SOCKET, PendingHandshake>::iterator it, short revents);
    void host_promote_pending_locked(std::map<SOCKET, PendingHandshake>::iterator it);
    void host_close_pending_locked();
    void host_close_all();
};

// =============================================================================
//  客户端
// =============================================================================
bool Relay::Impl::start_client(const ClientConfig& cfg, std::string& err) {
    mode = Mode::Client;
    ccfg = cfg;
    if(!ccfg.deviceName.empty())thisDevice=ccfg.deviceName.substr(0,128);
    if (ccfg.tcpListenPorts.empty()) ccfg.tcpListenPorts = {47989, 47984, 48010, 47990};
    if (ccfg.udpListenPorts.empty()) ccfg.udpListenPorts = {47998, 47999, 48000};

    WSADATA wsa{};
    if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { err = "WSAStartup 失败"; return false; }

    for (uint16_t port : ccfg.tcpListenPorts) {
        SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (!sock_valid(s)) { err = "创建 TCP 监听失败"; return false; }
        int yes = 1;
        ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        ::inet_pton(AF_INET, ccfg.listenAddr.c_str(), &a.sin_addr);
        if (::bind(s, (sockaddr*)&a, sizeof(a)) != 0 || ::listen(s, 16) != 0) {
            err = "本机 TCP 端口监听失败: " + std::to_string(port) + "（被占用？）";
            sock_close(s);
            return false;
        }
        set_nonblocking(s);
        tcpListeners.push_back(s);
        tcpListenPorts.push_back(port);
        logf("客户端：本机 TCP 监听 %s:%u", ccfg.listenAddr.c_str(), port);
    }
    for (uint16_t port : ccfg.udpListenPorts) {
        SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (!sock_valid(s)) { err = "创建 UDP 监听失败"; return false; }
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        ::inet_pton(AF_INET, ccfg.listenAddr.c_str(), &a.sin_addr);
        if (::bind(s, (sockaddr*)&a, sizeof(a)) != 0) {
            err = "本机 UDP 端口绑定失败: " + std::to_string(port);
            sock_close(s);
            return false;
        }
        set_nonblocking(s);
        tune_buffers(s, "客户端 UDP 监听");
        udpListeners.push_back(s);
        udpListenPorts.push_back(port);
        logf("客户端：本机 UDP 监听 %s:%u", ccfg.listenAddr.c_str(), port);
    }

    udpTunnel = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (!sock_valid(udpTunnel)) { err = "创建隧道 UDP socket 失败"; return false; }
    set_nonblocking(udpTunnel);
    tune_buffers(udpTunnel, "隧道 UDP");
    if (!resolve_v4(ccfg.serverHost, ccfg.serverUdpPort, serverUdpAddr)) {
        err = "解析服务器 UDP 地址失败"; return false;
    }

    stopReq.store(false);
    runningFlag.store(true);
    th = std::thread([this] {
        try { client_loop(); } catch (...) { logf("客户端事件循环异常退出"); }
    });
    statsTh = std::thread([this] { stats_loop(); });
    return true;
}

bool Relay::Impl::client_connect_tunnel() {
    udpProbeSent=0;udpProbeAck=0;udpAuthRejected=0;udpDataReceived=0;lastAuthenticatedUdpMs=0;lastUdpSendError=0;
    udpLastSeq.clear();
    const int conn = ++g_connSeq;
    sock_close(tunnelTcp);
    tunnelOutQ = OutQueue{};
    sessionValid = false;

    sockaddr_in addr{};
    if (!resolve_v4(ccfg.serverHost, ccfg.serverTcpPort, addr)) return false;

    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (!sock_valid(s)) return false;
    DWORD tmo = 8000;
    ::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tmo, sizeof(tmo));
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof(tmo));
    tune_buffers(s, "隧道 TCP");
    if (::connect(s, (sockaddr*)&addr, sizeof(addr)) != 0) {
        logf("客户端[#%d]：隧道 TCP 连接失败（WSA=%d）", conn, ::WSAGetLastError());
        sock_close(s);
        return false;
    }
    tunnelTcp = s;

    // P1-2: 新连接时重置 FrameReader
    tcpIn.reset();

    uint16_t type = 0, sid = 0;
    std::vector<uint8_t> pl;

    // P1-2: read_frame 不再清空 buffer，保留剩余数据给下一帧
    auto read_frame = [&](int timeoutMs) -> bool {
        const uint64_t deadline = now_ms() + uint64_t(timeoutMs);
        while (now_ms() < deadline) {
            if (tcpIn.bad()) return false;
            if (tcpIn.complete()) {
                if (!tcpIn.pop(type, sid, pl)) return false;
                return !tcpIn.bad();
            }
            uint8_t tmp[1024];
            int n = ::recv(tunnelTcp, (char*)tmp, sizeof(tmp), 0);
            if (n <= 0) return false;
            if (!tcpIn.feed(tmp, size_t(n))) return false;
        }
        return false;
    };

    if (!read_frame(8000) || type != kTcpHello || pl.size() < 4) {
        logf("客户端[#%d]：等待 nonce 失败（type=%u size=%u WSA=%d）", conn, unsigned(type), unsigned(pl.size()), ::WSAGetLastError());
        sock_close(tunnelTcp);
        return false;
    }
    uint32_t nonce = uint32_t(pl[0]) | (uint32_t(pl[1]) << 8) | (uint32_t(pl[2]) << 16) | (uint32_t(pl[3]) << 24);
    uint8_t token[16];
    make_token(ccfg.password, nonce, token);
    if (!send_tcp_frame(tunnelTcp, kTcpHelloAck, 0, token, sizeof(token))) {
        logf("客户端[#%d]：发送 token 失败（WSA=%d）", conn, ::WSAGetLastError());
        sock_close(tunnelTcp);
        return false;
    }
    if (!read_frame(8000) || type != kTcpHelloAck || pl.size() < 1 || pl[0] != 1) {
        logf("客户端[#%d]：认证阶段失败（type=%u size=%u result=%d）", conn, unsigned(type), unsigned(pl.size()),
             pl.empty() ? -1 : int(pl[0]));
        sock_close(tunnelTcp);
        return false;
    }

    // P1-4: 接收会话密钥（被控端在认证成功后发送）
    if (!read_frame(5000) || type != kTcpSessionKey || pl.size() != 32) {
        logf("客户端[#%d]：等待会话密钥失败（type=%u size=%u）", conn, unsigned(type), unsigned(pl.size()));
        sock_close(tunnelTcp);
        return false;
    }
    std::memcpy(sessionKey, pl.data(), 32);
    sessionValid = true;
    logf("客户端[#%d]：会话密钥已接收", conn);

    // P1-3: 握手完成后，把隧道 socket 设为非阻塞（事件循环用）
    if (!set_nonblocking(tunnelTcp)) {
        logf("客户端[#%d]：设置隧道非阻塞失败（WSA=%d）", conn, ::WSAGetLastError());
        sock_close(tunnelTcp);
        return false;
    }
    // 清除超时设置（非阻塞 socket 不需要）
    DWORD zero = 0;
    ::setsockopt(tunnelTcp, SOL_SOCKET, SO_SNDTIMEO, (const char*)&zero, sizeof(zero));
    ::setsockopt(tunnelTcp, SOL_SOCKET, SO_RCVTIMEO, (const char*)&zero, sizeof(zero));

    tcpIn.reset();   // 握手用的缓冲不留给数据阶段
    set_tunnel_up(true);
    logf("客户端[#%d]：隧道已连接并认证 %s:%u", conn, ccfg.serverHost.c_str(), ccfg.serverTcpPort);

    // 告知被控端我们的 UDP 来源地址，并开始探测 UDP 回程是否可用
    udpDirectOk.store(false);
    udpViaTcp = false;
    udpModeDecided = false;
    udpProbeStartMs = now_ms();
    nextUdpHelloMs = now_ms();
    lastUdpAckMs = 0;
    udpRecoverHits = 0;
    udpProbeFails = 0;
    if (::getenv("MSLINK_FORCE_TCP_UDP") != nullptr) {
        udpModeDecided = true;
        udpViaTcp = true;
        logf("客户端：MSLINK_FORCE_TCP_UDP 已设置，强制使用 TCP 兜底通道");
    }
    // P1-4: HELLO 不再携带口令，只发空 payload（认证已由 TCP 完成）
    udpProbeSent.fetch_add(1);
    if (!send_udp_packet(udpTunnel, serverUdpAddr, kUdpHello, 0, ++udpSeq,
                         sessionKey, nullptr, 0)) lastUdpSendError.store(WSAGetLastError());
    return true;
}

void Relay::Impl::client_close_streams() {
    for (auto& kv : clientStreams) sock_close(kv.second.sock);
    clientStreams.clear();
}

void Relay::Impl::client_drop_tunnel(const char* why) {
    if (sock_valid(tunnelTcp)) logf("客户端：%s", why);
    sock_close(tunnelTcp);
    tcpIn.reset();
    tunnelOutQ = OutQueue{};
    client_close_streams();
    set_tunnel_up(false);
}

bool Relay::Impl::process_client_frames() {
    uint16_t t = 0, sid = 0;
    std::vector<uint8_t> pl;
    while (tcpIn.complete()) {
        if (!tcpIn.pop(t, sid, pl)) return false;
        if (!client_handle_frame(t, sid, pl)) return false;
    }
    return !tcpIn.bad();
}

bool Relay::Impl::client_handle_frame(uint16_t type, uint16_t sid,
                                      const std::vector<uint8_t>& pl) {
    switch (type) {
        case kTcpStreamData: {
            auto it = clientStreams.find(sid);
            if (it == clientStreams.end()) break;
            // P1-1: 用原始字节发送（不加帧头），给本地 Moonlight
            if (!pl.empty()) {
                if (!send_raw_queued(it->second.sock, it->second.outQ, pl.data(), pl.size())) {
                    // 本地流溢出/断开：关闭对应流，并尽力通知对端。通知失败视为隧道故障。
                    const uint16_t id = it->second.id;
                    sock_close(it->second.sock);
                    clientStreams.erase(it);
                    if (!send_tcp_frame_queued(tunnelTcp, tunnelOutQ, kTcpStreamClose, id, nullptr, 0))
                        return false;
                    break;
                }
                stat([&](RelayStats& s) { s.tcpBytesDown += pl.size(); });
            }
            break;
        }
        case kTcpStreamClose: {
            auto it = clientStreams.find(sid);
            if (it != clientStreams.end()) { sock_close(it->second.sock); clientStreams.erase(it); }
            break;
        }
        case kTcpUdpDown: {
            auto byId = clientFlowById.find(sid);
            if (byId == clientFlowById.end()) break;
            auto fit = clientFlows.find(byId->second);
            if (fit == clientFlows.end()) break;
            if (!pl.empty()) {
                ::sendto(fit->second.localSock, (const char*)pl.data(), int(pl.size()), 0,
                         (sockaddr*)&fit->second.clientAddr, sizeof(fit->second.clientAddr));
                udpBytesDown.fetch_add(uint64_t(pl.size()));
                stat([](RelayStats& s) { s.udpPacketsDown++; });
            }
            break;
        }
        case kTcpPing:
            return send_tcp_frame_queued(tunnelTcp, tunnelOutQ, kTcpPong, 0, nullptr, 0);
        case kTcpPong:
            record_pong();
            break;
        default:
            break;
    }
    return true;
}

void Relay::Impl::client_loop() {
    enum Kind { kTunnel = 0, kTcpListen, kUdpListen, kStream, kUdpTunnel };
    struct Entry { SOCKET s; Kind kind; size_t index; };

    std::vector<WSAPOLLFD> fds;
    std::vector<Entry> entries;
    uint64_t lastReconnectMs = 0;
    uint64_t lastPingMs = now_ms();
    uint64_t lastSweepMs = now_ms();

    while (!stopReq.load()) {
        pump_udp_duplicates(now_ms());
        if (!sock_valid(tunnelTcp)) {
            set_tunnel_up(false);
            client_close_streams();
            const uint64_t n = now_ms();
            if (n - lastReconnectMs < 1000) { ::Sleep(50); continue; }
            lastReconnectMs = n;
            logf("客户端：正在连接隧道 %s:%u …", ccfg.serverHost.c_str(), ccfg.serverTcpPort);
            if (!client_connect_tunnel()) {
                stat([](RelayStats& s) { s.tunnelReconnects++; });
                ::Sleep(1000);
                continue;
            }
            lastPingMs = now_ms();
        }

        fds.clear();
        entries.clear();
        short tunnelEvents = POLLRDNORM;
        if (!tunnelOutQ.empty()) tunnelEvents |= POLLWRNORM;
        fds.push_back({ tunnelTcp, tunnelEvents, 0 });
        entries.push_back({ tunnelTcp, kTunnel, 0 });

        for (size_t i = 0; i < tcpListeners.size(); ++i) {
            fds.push_back({ tcpListeners[i], POLLRDNORM, 0 });
            entries.push_back({ tcpListeners[i], kTcpListen, i });
        }
        for (size_t i = 0; i < udpListeners.size(); ++i) {
            fds.push_back({ udpListeners[i], POLLRDNORM, 0 });
            entries.push_back({ udpListeners[i], kUdpListen, i });
        }
        for (auto& kv : clientStreams) {
            if (!sock_valid(kv.second.sock)) continue;
            short ev = POLLRDNORM;
            if (!kv.second.outQ.empty()) ev |= POLLWRNORM;
            fds.push_back({ kv.second.sock, ev, 0 });
            entries.push_back({ kv.second.sock, kStream, kv.second.id });
        }
        if (sock_valid(udpTunnel)) {
            fds.push_back({ udpTunnel, POLLRDNORM, 0 });
            entries.push_back({ udpTunnel, kUdpTunnel, 0 });
        }

        if (fds.empty()) break;
        int rc = ::WSAPoll(fds.data(), ULONG(fds.size()), poll_timeout_ms());
        if (rc < 0) { ::Sleep(10); continue; }

        for (size_t i = 0; i < entries.size(); ++i) {
            if (!(fds[i].revents & (POLLRDNORM | POLLHUP | POLLERR | POLLWRNORM))) continue;
            const Entry& e = entries[i];

            if (e.kind == kTunnel) {
                if (fds[i].revents & POLLWRNORM) {
                    int rc2 = tunnelOutQ.flush(tunnelTcp);
                    if (rc2 < 0) {
                        client_drop_tunnel("隧道发送队列错误，断开重连");
                        break;
                    }
                }
                if (!(fds[i].revents & (POLLRDNORM | POLLHUP | POLLERR))) continue;

                uint8_t tmp[8192];
                int n = ::recv(tunnelTcp, (char*)tmp, sizeof(tmp), 0);
                if (n <= 0) {
                    client_drop_tunnel("隧道断开，1 秒后重连");
                    break;
                }
                if (!tcpIn.feed(tmp, size_t(n))) {
                    client_drop_tunnel("收到非法或超长 TCP 帧，断开重连");
                    break;
                }
                if (!process_client_frames()) {
                    client_drop_tunnel("隧道帧处理失败，断开重连");
                    break;
                }
            } else if (e.kind == kTcpListen) {
                sockaddr_in from{};
                int fl = sizeof(from);
                SOCKET c = ::accept(tcpListeners[e.index], (sockaddr*)&from, &fl);
                if (!sock_valid(c)) continue;
                set_nonblocking(c);
                const uint16_t port = tcpListenPorts[e.index];
                uint16_t sid2 = nextStreamId++;
                if (nextStreamId == 0) nextStreamId = 1;
                uint8_t pb[2] = { uint8_t(port & 0xFF), uint8_t((port >> 8) & 0xFF) };
                if (!send_tcp_frame_queued(tunnelTcp, tunnelOutQ, kTcpStreamOpen, sid2, pb, sizeof(pb))) {
                    sock_close(c);
                    client_drop_tunnel("隧道发送队列溢出/错误，断开重连");
                    break;
                }
                ClientStream cs;
                cs.sock = c; cs.id = sid2; cs.targetPort = port;
                clientStreams[sid2] = cs;
                stat([](RelayStats& s) { s.tcpStreamsOpened++; });
                logf("客户端：Moonlight 连入本机 %u -> 隧道流 #%u", port, sid2);
            } else if (e.kind == kUdpListen) {
                bool tunnelOk = true;
                for (;;) {
                    uint8_t buf[kMaxUdpPayload + 64];
                    sockaddr_in from{};
                    int fl = sizeof(from);
                    int n = ::recvfrom(udpListeners[e.index], (char*)buf, sizeof(buf), 0,
                                       (sockaddr*)&from, &fl);
                    if (n <= 0) break;
                    const uint32_t key = flow_key(from);
                    auto it = clientFlows.find(key);
                    if (it == clientFlows.end()) {
                        uint16_t fid = nextFlowId++;
                        if (nextFlowId == 0) nextFlowId = 1;
                        UdpFlow f;
                        f.id = fid; f.clientAddr = from;
                        f.localSock = udpListeners[e.index];
                        f.lastMs = now_ms();
                        clientFlows[key] = f;
                        clientFlowById[fid] = key;
                        it = clientFlows.find(key);
                        logf("客户端：新 UDP 流 #%u（本地端口 %u）", fid, udpListenPorts[e.index]);
                    }
                    it->second.lastMs = now_ms();
                    uint8_t pkt[kMaxUdpPayload];
                    if (size_t(n) + 2 > sizeof(pkt)) {
                        stat([](RelayStats& s) { s.udpDropped++; });
                        continue;
                    }
                    pkt[0] = uint8_t(udpListenPorts[e.index] & 0xFF);
                    pkt[1] = uint8_t((udpListenPorts[e.index] >> 8) & 0xFF);
                    std::memcpy(pkt + 2, buf, size_t(n));
                    if (udpViaTcp) {
                        if (send_tcp_frame_queued(tunnelTcp, tunnelOutQ, kTcpUdpUp, it->second.id, pkt, size_t(n) + 2)) {
                            udpBytesUp.fetch_add(uint64_t(n) + 2);
                            stat([](RelayStats& s) { s.udpPacketsUp++; });
                        } else {
                            stat([](RelayStats& s) { s.udpDropped++; });
                            tunnelOk = false;
                            break;
                        }
                    } else {
                        const uint32_t seq = ++udpSeq;
                        const uint16_t logicalPort = udpListenPorts[e.index];
                        if (send_udp_packet(udpTunnel, serverUdpAddr, kUdpData, it->second.id,
                                            seq, sessionKey, pkt, size_t(n) + 2)) {
                            udpBytesUp.fetch_add(uint64_t(n) + 2);
                            stat([](RelayStats& s) { s.udpPacketsUp++; });
                            // 抗丢包：同一 (flowId, seq) 再发一份副本；接收端按防重放窗口去重
                            queue_udp_duplicate(udpTunnel, serverUdpAddr, nullptr, false,
                                                kUdpData, it->second.id, seq,
                                                pkt, size_t(n) + 2, logicalPort);
                        } else {
                            stat([](RelayStats& s) { s.udpDropped++; });
                        }
                    }
                }
                if (!tunnelOk) {
                    client_drop_tunnel("隧道发送队列溢出/错误，断开重连");
                    break;
                }
            } else if (e.kind == kStream) {
                auto it = clientStreams.find(uint16_t(e.index));
                if (it == clientStreams.end()) continue;

                if (fds[i].revents & POLLWRNORM) {
                    int rc2 = it->second.outQ.flush(it->second.sock);
                    if (rc2 < 0) {
                        const uint16_t id = it->second.id;
                        sock_close(it->second.sock);
                        clientStreams.erase(it);
                        if (!send_tcp_frame_queued(tunnelTcp, tunnelOutQ, kTcpStreamClose, id, nullptr, 0)) {
                            client_drop_tunnel("隧道发送队列溢出/错误，断开重连");
                            break;
                        }
                        continue;
                    }
                }
                if (!(fds[i].revents & (POLLRDNORM | POLLHUP | POLLERR))) continue;

                uint8_t buf[8192];
                int n = ::recv(it->second.sock, (char*)buf, sizeof(buf), 0);
                if (n <= 0) {
                    const uint16_t id = it->second.id;
                    sock_close(it->second.sock);
                    clientStreams.erase(it);
                    if (!send_tcp_frame_queued(tunnelTcp, tunnelOutQ, kTcpStreamClose, id, nullptr, 0)) {
                        client_drop_tunnel("隧道发送队列溢出/错误，断开重连");
                        break;
                    }
                } else if (send_tcp_frame_queued(tunnelTcp, tunnelOutQ, kTcpStreamData, it->second.id, buf, size_t(n))) {
                    stat([&](RelayStats& s) { s.tcpBytesUp += size_t(n); });
                } else {
                    client_drop_tunnel("隧道发送队列溢出/错误，断开重连");
                    break;
                }
            } else if (e.kind == kUdpTunnel) {
                for (;;) {
                    uint8_t buf[kUdpHdr + kMaxUdpPayload + 64];
                    sockaddr_in from{};
                    int fl = sizeof(from);
                    int n = ::recvfrom(udpTunnel, (char*)buf, sizeof(buf), 0, (sockaddr*)&from, &fl);
                    if (n <= 0) break;
                    if (n < int(kUdpHdr) || buf[0] != kUdpMagic) continue;
                    if (buf[1] != kUdpData && buf[1] != kUdpHelloAck) continue;

                    if (!sessionValid) {
                        stat([](RelayStats& s) { s.udpDropped++; });
                        continue;
                    }
                    if (buf[8] != sessionKey[0] || buf[9] != sessionKey[1] ||
                        buf[10] != sessionKey[2] || buf[11] != sessionKey[3]) {
                        stat([](RelayStats& s) { s.udpDropped++; });
                        continue;
                    }
                    const uint32_t seq = uint32_t(buf[4]) | (uint32_t(buf[5]) << 8) |
                                         (uint32_t(buf[6]) << 16) | (uint32_t(buf[7]) << 24);
                    const uint16_t fid = uint16_t(buf[2] | (buf[3] << 8));

                    const size_t payloadLen = size_t(n) - kUdpHdr;
                    uint8_t expectTag[8];
                    udp_auth_tag(sessionKey, buf[1], fid, seq, buf + kUdpHdr, payloadLen, expectTag);
                    if (std::memcmp(buf + 12, expectTag, 8) != 0) {
                        udpAuthRejected.fetch_add(1);
                        stat([](RelayStats& s) { s.udpDropped++; });
                        continue;
                    }

                    if(buf[1]==kUdpHelloAck){
                        udpProbeAck.fetch_add(1);lastAuthenticatedUdpMs.store(now_ms());lastUdpAckMs=now_ms();udpDirectOk.store(true);
                        if(udpViaTcp.load() && ++udpRecoverHits>=3){udpViaTcp.store(false);udpRecoverHits=0;logf("UDP recovered after three authenticated ACKs");}
                        continue;
                    }
                    if(udpLastSeq[fid].duplicate(seq)){stat([](RelayStats& s){s.udpDuplicates++;});continue;}
                    if(!udpLastSeq[fid].accept(seq)){stat([](RelayStats& s){s.udpDropped++;});continue;}

                    udpDataReceived.fetch_add(1);lastAuthenticatedUdpMs.store(now_ms());lastUdpAckMs=now_ms();udpDirectOk.store(true);
                    auto byId = clientFlowById.find(fid);
                    if (byId == clientFlowById.end()) continue;
                    auto fit = clientFlows.find(byId->second);
                    if (fit == clientFlows.end()) continue;
                    ::sendto(fit->second.localSock, (const char*)(buf + kUdpHdr), int(payloadLen), 0,
                             (sockaddr*)&fit->second.clientAddr, sizeof(fit->second.clientAddr));
                    udpBytesDown.fetch_add(uint64_t(n));
                    stat([](RelayStats& s) { s.udpPacketsDown++; });
                }
            }
        }

        // ---- UDP 传输模式：持续探测 + 带滞后的恢复 ----
        //
        // 之前是「1.5 秒内收不到 HELLO_ACK 就永久回退 TCP」。
        // 问题：FRP/防火墙的 UDP 映射有时要好几秒才建立，1.5 秒完全不够，
        // 结果是一次误判 → 整个会话都跑 TCP，UI 上也一直显示「UDP 不通」。
        //
        // 现在改成：
        //   · 探测包一直发（未确认期间 500ms 一次，确认后 3s 一次保活）
        //   · 收到 ACK 立刻置 udpDirectOk，不需要等某个窗口结束
        //   · 转 TCP 只是「临时兜底」，之后仍持续探测；连续几次 ACK 成功就切回 UDP
        //   · 切换带滞后，避免在临界状态反复横跳
        if (sock_valid(tunnelTcp)) {
            const uint64_t tnow = now_ms();

            // 定期发 HELLO（未确认时更频繁）
            const uint64_t helloGap = udpDirectOk.load() ? 3000 : 500;
            if (tnow >= nextUdpHelloMs) {
                nextUdpHelloMs = tnow + helloGap;
                udpProbeSent.fetch_add(1);
                if(!send_udp_packet(udpTunnel, serverUdpAddr, kUdpHello, 0, ++udpSeq,sessionKey,nullptr,0))lastUdpSendError.store(WSAGetLastError());
                else lastUdpSendError.store(0);
            }

            if (udpDirectOk.load()) {
                // 已确认可走 UDP；但长时间收不到 ACK 说明链路变了，退回去重新评估
                if (lastUdpAckMs != 0 && tnow - lastUdpAckMs > 8000) {
                    udpDirectOk.store(false);
                    udpProbeFails = 0;
                    udpRecoverHits = 0;
                    udpProbeStartMs = tnow;
                    logf("提示：UDP 回程应答中断，重新探测中（期间自动用 TCP 兜底）");
                } else {
                    udpProbeFails = 0;
                    // Recovery count is updated only by authenticated HELLO_ACK packets.

                }
            } else {
                udpRecoverHits = 0;
                // 还没确认通：第一次超时后先用 TCP 兜底保证能用
                if (!udpViaTcp && tnow - udpProbeStartMs > 2500) {
                    udpViaTcp = true;
                    logf("提示：UDP 回程暂时没有应答，先走 TCP 兜底（仍在持续探测，通了会自动切回）");
                }
            }
            udpModeDecided = true;
        }

        if (now_ms() - lastPingMs > 1000) {
            lastPingMs = now_ms();
            pingSentMs.store(now_ms());
            if (!send_tcp_frame_queued(tunnelTcp, tunnelOutQ, kTcpPing, 0, nullptr, 0)) {
                client_drop_tunnel("控制帧发送失败，断开重连");
            } else {
                const auto report = telemetry_json();
                if (!send_tcp_frame_queued(tunnelTcp, tunnelOutQ, kTcpTelemetry, 0, report.data(), report.size())) {
                    client_drop_tunnel("控制帧发送失败，断开重连");
                }
            }
        }
        stat([&](RelayStats& s){s.activeTcpStreams=uint32_t(clientStreams.size());s.activeUdpFlows=uint32_t(clientFlows.size());s.queuedBytes=tunnelOutQ.totalBytes;});
        if (now_ms() - lastSweepMs > 30000) {
            lastSweepMs = now_ms();
            const uint64_t cutoff = now_ms() - uint64_t(kUdpFlowIdleMs);
            for (auto it = clientFlows.begin(); it != clientFlows.end();) {
                if (it->second.lastMs < cutoff) {
                    clientFlowById.erase(it->second.id);
                    it = clientFlows.erase(it);
                } else ++it;
            }
        }
    }

    client_close_streams();
    sock_close(tunnelTcp);
    sock_close(udpTunnel);
    for (SOCKET s : tcpListeners) sock_close(s);
    for (SOCKET s : udpListeners) sock_close(s);
    tcpListeners.clear();
    udpListeners.clear();
    set_tunnel_up(false);
}

// =============================================================================
//  被控端
// =============================================================================
bool Relay::Impl::start_host(const HostConfig& cfg, std::string& err) {
    mode = Mode::Host;
    hcfg = cfg;

    WSADATA wsa{};
    if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { err = "WSAStartup 失败"; return false; }

    sockaddr_in a{};
    a.sin_family = AF_INET;
    ::inet_pton(AF_INET, hcfg.bindAddr.c_str(), &a.sin_addr);

    hTcpListen = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (!sock_valid(hTcpListen)) { err = "创建监听失败"; return false; }
    int yes = 1;
    ::setsockopt(hTcpListen, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));
    a.sin_port = htons(hcfg.listenTcpPort);
    if (::bind(hTcpListen, (sockaddr*)&a, sizeof(a)) != 0 || ::listen(hTcpListen, 8) != 0) {
        err = "被控端 TCP 监听失败: " + std::to_string(hcfg.listenTcpPort);
        sock_close(hTcpListen);
        return false;
    }
    set_nonblocking(hTcpListen);

    hUdpListen = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (!sock_valid(hUdpListen)) { err = "创建 UDP 监听失败"; return false; }
    a.sin_port = htons(hcfg.listenUdpPort);
    if (::bind(hUdpListen, (sockaddr*)&a, sizeof(a)) != 0) {
        err = "被控端 UDP 监听失败: " + std::to_string(hcfg.listenUdpPort);
        sock_close(hUdpListen);
        return false;
    }
    if (!hostUdpIo.enable(hUdpListen)) {
        err = "初始化 UDP 回包源地址控制失败 (WSA=" + std::to_string(WSAGetLastError()) + ")";
        sock_close(hUdpListen);
        sock_close(hTcpListen);
        return false;
    }
    set_nonblocking(hUdpListen);
    tune_buffers(hUdpListen, "被控端 UDP 监听");

    logf("被控端：监听 TCP %u / UDP %u -> 转发目标 %s",
         hcfg.listenTcpPort, hcfg.listenUdpPort, hcfg.targetHost.c_str());

    stopReq.store(false);
    runningFlag.store(true);
    th = std::thread([this] {
        try { host_loop(); } catch (...) { logf("被控端事件循环异常退出"); }
    });
    statsTh = std::thread([this] { stats_loop(); });
    return true;
}

void Relay::Impl::host_accept_tunnel() {
    for (;;) {
        sockaddr_in from{};
        int fl = sizeof(from);
        SOCKET c = ::accept(hTcpListen, (sockaddr*)&from, &fl);
        if (!sock_valid(c)) return;

        // 单客户端：已有认证会话时直接拒绝新连接，本轮不做接管。
        if (sock_valid(hTunnel)) {
            logf("被控端：已有认证会话，拒绝新连接 %s:%u", inet_ntoa(from.sin_addr), ntohs(from.sin_port));
            sock_close(c);
            continue;
        }

        {
            std::lock_guard<std::mutex> lk(pendingMtx);
            if (pendingHandshakes.size() >= kMaxPendingHandshakes) {
                logf("被控端：未认证握手已达上限 %u，拒绝新连接",
                     unsigned(kMaxPendingHandshakes));
                sock_close(c);
                continue;
            }
            // Windows 上 accept() 出来的 socket 会继承监听口的非阻塞模式，
            // 这里保持非阻塞，握手完全由 WSAPoll 驱动，不再阻塞事件循环。
            set_nonblocking(c);
            tune_buffers(c, "被控端隧道 TCP");

            PendingHandshake p;
            p.sock = c;
            p.from = from;
            p.nonce = uint32_t(now_ms()) ^ uint32_t(uintptr_t(&from));
            if (p.nonce == 0) p.nonce = 1;
            p.deadlineMs = now_ms() + kHandshakeTimeoutMs;

            uint8_t nb[4] = { uint8_t(p.nonce & 0xFF), uint8_t((p.nonce >> 8) & 0xFF),
                              uint8_t((p.nonce >> 16) & 0xFF), uint8_t((p.nonce >> 24) & 0xFF) };
            if (!send_tcp_frame_queued(p.sock, p.outQ, kTcpHello, 0, nb, sizeof(nb))) {
                logf("被控端：发送非阻塞握手 nonce 失败（WSA=%d）", ::WSAGetLastError());
                sock_close(p.sock);
                continue;
            }
            pendingHandshakes.emplace(c, std::move(p));
        }
        logf("被控端：新连接 %s:%u 已发送 nonce（非阻塞握手）",
             inet_ntoa(from.sin_addr), ntohs(from.sin_port));
    }
}

void Relay::Impl::host_close_pending_locked() {
    for (auto& kv : pendingHandshakes) sock_close(kv.second.sock);
    pendingHandshakes.clear();
}

// 返回 true 表示该 pending 仍保留；false 表示已关闭或已提升为会话（均已从 map 移除）。
bool Relay::Impl::host_process_pending_locked(
        std::map<SOCKET, PendingHandshake>::iterator it, short revents) {
    PendingHandshake& p = it->second;
    auto drop = [&](const char* why) -> bool {
        if (why) logf("被控端：%s", why);
        sock_close(p.sock);
        pendingHandshakes.erase(it);
        return false;
    };

    if (now_ms() > p.deadlineMs) return drop("未认证握手超时（5 秒），关闭连接");

    if (!p.outQ.empty()) {
        int rc = p.outQ.flush(p.sock);
        if (rc < 0) return drop("未认证握手发送失败（连接已断开）");
    }

    if (p.state == PendingHandshake::State::WaitToken) {
        if (!(revents & (POLLRDNORM | POLLHUP | POLLERR))) return true;

        uint8_t tmp[512];
        int n = ::recv(p.sock, (char*)tmp, sizeof(tmp), 0);
        if (n <= 0) return drop("未认证握手读取失败");
        if (!p.in.feed(tmp, size_t(n)))
            return drop("未认证握手帧非法或超长");

        if (!p.in.complete()) return true;

        uint16_t type = 0, sid = 0;
        std::vector<uint8_t> pl;
        if (!p.in.pop(type, sid, pl) || p.in.bad() ||
            type != kTcpHelloAck || pl.size() < 16)
            return drop("握手帧非法（类型、长度或后续帧头不符）");

        uint8_t expect[16];
        make_token(hcfg.password, p.nonce, expect);
        uint8_t diff = 0;
        for (int i = 0; i < 16; ++i) diff |= uint8_t(pl[i] ^ expect[i]);
        const uint8_t good = (diff == 0) ? 1 : 0;

        if (!send_tcp_frame_queued(p.sock, p.outQ, kTcpHelloAck, 0, &good, 1))
            return drop("发送认证结果失败");

        if (!good) {
            logf("被控端：隧道认证失败（密码不一致）");
            p.state = PendingHandshake::State::Closing;
            int rc = p.outQ.flush(p.sock);
            if (rc < 0 || p.outQ.empty()) {
                sock_close(p.sock);
                pendingHandshakes.erase(it);
                return false;
            }
            return true;
        }

        derive_session_key(hcfg.password, p.nonce, p.key);
        if (!send_tcp_frame_queued(p.sock, p.outQ, kTcpSessionKey, 0, p.key, 32))
            return drop("发送会话密钥失败");
        p.state = PendingHandshake::State::AwaitFlush;

        int rc = p.outQ.flush(p.sock);
        if (rc < 0) return drop("发送会话密钥失败（连接已断开）");
        if (p.outQ.empty()) {
            host_promote_pending_locked(it);
            return false;
        }
        return true;
    }

    // AwaitFlush / Closing：必须先把手里的 HelloAck/SessionKey 全部发完，
    // 才能把 socket 交给数据阶段或关闭，避免半帧/缺密钥。
    if (!p.outQ.empty()) return true;

    if (p.state == PendingHandshake::State::Closing) {
        sock_close(p.sock);
        pendingHandshakes.erase(it);
        return false;
    }

    host_promote_pending_locked(it);
    return false;
}

void Relay::Impl::host_promote_pending_locked(
        std::map<SOCKET, PendingHandshake>::iterator it) {
    PendingHandshake& p = it->second;

    hTunnel = p.sock;
    p.sock = INVALID_SOCKET;
    hTcpIn = std::move(p.in);
    hTunnelOutQ = OutQueue{};
    std::memcpy(sessionKey, p.key, 32);
    sessionValid = true;
    set_tunnel_up(true);
    logf("被控端：隧道已建立并认证（%s:%u）",
         inet_ntoa(p.from.sin_addr), ntohs(p.from.sin_port));

    // 单认证客户端：其余还在握手的连接全部拒绝。
    for (auto o = pendingHandshakes.begin(); o != pendingHandshakes.end();) {
        if (o == it) { ++o; continue; }
        logf("被控端：已有会话建立，拒绝另一个未认证连接");
        sock_close(o->second.sock);
        o = pendingHandshakes.erase(o);
    }
    pendingHandshakes.erase(it);
}

void Relay::Impl::host_drop_tunnel(const char* why) {
    if (sock_valid(hTunnel)) logf("被控端：%s", why);
    sock_close(hTunnel);
    hTcpIn.reset();
    hTunnelOutQ = OutQueue{};
    host_close_all();
    set_tunnel_up(false);
}

bool Relay::Impl::process_host_frames() {
    uint16_t t = 0, sid = 0;
    std::vector<uint8_t> pl;
    while (hTcpIn.complete()) {
        if (!hTcpIn.pop(t, sid, pl)) return false;
        if (!host_handle_frame(t, sid, pl)) return false;
    }
    return !hTcpIn.bad();
}

void Relay::Impl::host_close_all() {
    for (auto& kv : hostStreams) sock_close(kv.second.up);
    hostStreams.clear();
    for (auto& kv : hostFlows) sock_close(kv.second.up);
    hostFlows.clear();
    clientUdpKnown = false;
    clientUdpRoute = {};
    udpReplyRoutePinned.store(false);
    sessionValid = false;
    udpLastSeq.clear();
}

bool Relay::Impl::host_handle_frame(uint16_t type, uint16_t sid,
                                    const std::vector<uint8_t>& pl) {
    switch (type) {
        case kTcpStreamOpen: {
            if (pl.size() < 2) break;
            const uint16_t logical = uint16_t(pl[0] | (pl[1] << 8));
            const uint16_t port = hcfg.resolve_tcp(logical);
            sockaddr_in a{};
            if (!resolve_v4(hcfg.targetHost, port, a)) break;
            SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (!sock_valid(s)) break;
            if (::connect(s, (sockaddr*)&a, sizeof(a)) != 0) { sock_close(s); break; }
            set_nonblocking(s);
            HostStream hs; hs.up = s; hs.targetPort = port;
            hostStreams[sid] = hs;
            logf("被控端：流 #%u -> %s:%u（逻辑 %u）", sid, hcfg.targetHost.c_str(), port, logical);
            break;
        }
        case kTcpStreamData: {
            auto it = hostStreams.find(sid);
            if (it == hostStreams.end()) break;
            // P1-1: 用原始字节发送（不加帧头），给 Sunshine
            if (!pl.empty()) {
                if (!send_raw_queued(it->second.up, it->second.outQ, pl.data(), pl.size())) {
                    // 目标流溢出/断开：关闭该流并通知对端；通知失败视为隧道故障。
                    sock_close(it->second.up);
                    hostStreams.erase(it);
                    if (!send_tcp_frame_queued(hTunnel, hTunnelOutQ, kTcpStreamClose, sid, nullptr, 0))
                        return false;
                    break;
                }
                stat([&](RelayStats& s) { s.tcpBytesUp += pl.size(); });
            }
            break;
        }
        case kTcpStreamClose: {
            auto it = hostStreams.find(sid);
            if (it != hostStreams.end()) { sock_close(it->second.up); hostStreams.erase(it); }
            break;
        }
        case kTcpUdpUp: {
            if (pl.size() < 2) break;
            const uint16_t targetPort = hcfg.resolve_udp(uint16_t(pl[0] | (pl[1] << 8)));
            auto it = hostFlows.find(sid);
            if (it == hostFlows.end()) {
                sockaddr_in ta{};
                if (!resolve_v4(hcfg.targetHost, targetPort, ta)) break;
                SOCKET us = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
                if (!sock_valid(us)) break;
                if (::connect(us, (sockaddr*)&ta, sizeof(ta)) != 0) { sock_close(us); break; }
                set_nonblocking(us);
                tune_buffers(us, "被控端上游 UDP（TCP 兜底）");
                HostFlow hf; hf.up = us; hf.lastMs = now_ms(); hf.viaTcp = true; hf.targetPort = targetPort;
                hostFlows[sid] = hf;
                it = hostFlows.find(sid);
                logf("被控端：UDP 流 #%u -> %s:%u（经 TCP 兜底通道）", sid, hcfg.targetHost.c_str(), targetPort);
            } else {
                it->second.viaTcp = true;
            }
            it->second.lastMs = now_ms();
            ::send(it->second.up, (const char*)(pl.data() + 2), int(pl.size() - 2), 0);
            udpBytesUp.fetch_add(uint64_t(pl.size() - 2));
            stat([](RelayStats& s) { s.udpPacketsUp++; });
            break;
        }
        case kTcpTelemetry:
            if(pl.size()<=8192)stat([&](RelayStats&){peerReport.assign(pl.begin(),pl.end());peerReportAt=now_ms();});
            break;
        case kTcpPong: record_pong(); break;
        case kTcpPing:
            return send_tcp_frame_queued(hTunnel, hTunnelOutQ, kTcpPong, 0, nullptr, 0);
        default:
            break;
    }
    return true;
}

void Relay::Impl::host_loop() {
    enum Kind { kHTcpListen = 0, kHUdpListen, kHTunnel, kHStream, kHUdpFlow, kHPending };
    struct Entry { SOCKET s; Kind kind; size_t index; };

    std::vector<WSAPOLLFD> fds;
    std::vector<Entry> entries;
    uint64_t lastSweepMs = now_ms();
    uint64_t lastPingMs = now_ms();

    while (!stopReq.load()) {
        pump_udp_duplicates(now_ms());

        // 每轮清理超时的未认证握手；它们不占用事件循环，也不影响已有会话。
        {
            std::lock_guard<std::mutex> lk(pendingMtx);
            const uint64_t n = now_ms();
            for (auto it = pendingHandshakes.begin(); it != pendingHandshakes.end();) {
                if (n > it->second.deadlineMs) {
                    logf("被控端：未认证握手超时（5 秒），关闭连接");
                    sock_close(it->second.sock);
                    it = pendingHandshakes.erase(it);
                } else {
                    ++it;
                }
            }
        }

        fds.clear();
        entries.clear();
        fds.push_back({ hTcpListen, POLLRDNORM, 0 });
        entries.push_back({ hTcpListen, kHTcpListen, 0 });
        fds.push_back({ hUdpListen, POLLRDNORM, 0 });
        entries.push_back({ hUdpListen, kHUdpListen, 0 });
        if (sock_valid(hTunnel)) {
            short ev = POLLRDNORM;
            if (!hTunnelOutQ.empty()) ev |= POLLWRNORM;
            fds.push_back({ hTunnel, ev, 0 });
            entries.push_back({ hTunnel, kHTunnel, 0 });
        }
        {
            std::lock_guard<std::mutex> lk(pendingMtx);
            for (auto& kv : pendingHandshakes) {
                short ev = POLLRDNORM;
                if (!kv.second.outQ.empty()) ev |= POLLWRNORM;
                fds.push_back({ kv.first, ev, 0 });
                entries.push_back({ kv.first, kHPending, 0 });
            }
        }
        for (auto& kv : hostStreams) {
            if (!sock_valid(kv.second.up)) continue;
            short ev = POLLRDNORM;
            if (!kv.second.outQ.empty()) ev |= POLLWRNORM;
            fds.push_back({ kv.second.up, ev, 0 });
            entries.push_back({ kv.second.up, kHStream, kv.first });
        }
        for (auto& kv : hostFlows) {
            if (!sock_valid(kv.second.up)) continue;
            fds.push_back({ kv.second.up, POLLRDNORM, 0 });
            entries.push_back({ kv.second.up, kHUdpFlow, kv.first });
        }

        int rc = ::WSAPoll(fds.data(), ULONG(fds.size()), poll_timeout_ms());
        if (rc < 0) { ::Sleep(10); continue; }

        for (size_t i = 0; i < entries.size(); ++i) {
            if (!(fds[i].revents & (POLLRDNORM | POLLHUP | POLLERR | POLLWRNORM))) continue;
            const Entry& e = entries[i];

            if (e.kind == kHTcpListen) {
                host_accept_tunnel();
            } else if (e.kind == kHPending) {
                std::lock_guard<std::mutex> lk(pendingMtx);
                auto it = pendingHandshakes.find(e.s);
                if (it != pendingHandshakes.end())
                    host_process_pending_locked(it, fds[i].revents);
                continue;
            } else if (e.kind == kHUdpListen) {
                for (;;) {
                    uint8_t buf[kUdpHdr + kMaxUdpPayload + 64];
                    sockaddr_in from{};
                    UdpReplyRoute receivedRoute;
                    int n = hostUdpIo.recv(hUdpListen, buf, sizeof(buf), from, receivedRoute);
                    if (n <= 0) break;
                    if (n < int(kUdpHdr) || buf[0] != kUdpMagic) continue;
                    const uint8_t ptype = buf[1];
                    const uint16_t fid = uint16_t(buf[2] | (buf[3] << 8));

                    if (!sessionValid) {
                        stat([](RelayStats& s) { s.udpDropped++; });
                        continue;
                    }
                    if (buf[8] != sessionKey[0] || buf[9] != sessionKey[1] ||
                        buf[10] != sessionKey[2] || buf[11] != sessionKey[3]) {
                        udpSessionRejected.fetch_add(1);
                        stat([](RelayStats& s) { s.udpDropped++; });
                        continue;
                    }
                    const uint32_t seq = uint32_t(buf[4]) | (uint32_t(buf[5]) << 8) |
                                         (uint32_t(buf[6]) << 16) | (uint32_t(buf[7]) << 24);

                    const size_t payloadLen = size_t(n) - kUdpHdr;
                    uint8_t expectTag[8];
                    udp_auth_tag(sessionKey, ptype, fid, seq, buf + kUdpHdr, payloadLen, expectTag);
                    if (std::memcmp(buf + 12, expectTag, 8) != 0) {
                        udpAuthRejected.fetch_add(1);
                        stat([](RelayStats& s) { s.udpDropped++; });
                        continue;
                    }

                    if(udpLastSeq[fid].duplicate(seq)){stat([](RelayStats& s){s.udpDuplicates++;});continue;}
                    if(!udpLastSeq[fid].accept(seq)){stat([](RelayStats& s){s.udpDropped++;});continue;}

                    if (!receivedRoute.valid) {
                        udpReplyErrors.fetch_add(1);
                        lastUdpSendError.store(WSAEADDRNOTAVAIL);
                        continue;
                    }

                    clientUdpAddr = from;
                    clientUdpRoute = receivedRoute;
                    clientUdpKnown = true;
                    udpReplyRoutePinned.store(true);

                    if (ptype == kUdpHello) {
                        udpHelloReceived.fetch_add(1);
                        if (send_udp_packet(hUdpListen, from, kUdpHelloAck, 0, 0,
                                            sessionKey, nullptr, 0, &receivedRoute, hostUdpIo.send)) {
                            udpHelloAckSent.fetch_add(1);
                            lastUdpSendError.store(0);
                            lastAuthenticatedUdpMs.store(now_ms());
                        } else {
                            udpReplyErrors.fetch_add(1);
                            lastUdpSendError.store(WSAGetLastError());
                        }
                        continue;
                    }
                    if (ptype != kUdpData) continue;
                    udpDataReceived.fetch_add(1);
                    lastAuthenticatedUdpMs.store(now_ms());

                    if (payloadLen < 2) continue;
                    const uint8_t* pl = buf + kUdpHdr;
                    const uint16_t targetPort = hcfg.resolve_udp(uint16_t(pl[0] | (pl[1] << 8)));

                    // P1-4: 目标端口白名单（允许配置的标准端口 + 自检映射端口）
                    bool allowed = false;
                    // 标准 Moonlight 端口
                    if (targetPort == 47998 || targetPort == 47999 || targetPort == 48000 ||
                        targetPort == 47989 || targetPort == 47984 || targetPort == 48010 ||
                        targetPort == 47990) {
                        allowed = true;
                    }
                    // 自检映射端口（19000-19099 范围，用于测试）
                    if (targetPort >= 19000 && targetPort <= 19099) {
                        allowed = true;
                    }
                    if (!allowed) {
                        logf("被控端：UDP 流 #%u 目标端口 %u 不在白名单，丢弃", fid, targetPort);
                        stat([](RelayStats& s) { s.udpDropped++; });
                        continue;
                    }

                    auto it = hostFlows.find(fid);
                    if (it == hostFlows.end()) {
                        sockaddr_in ta{};
                        if (!resolve_v4(hcfg.targetHost, targetPort, ta)) continue;
                        SOCKET us = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
                        if (!sock_valid(us)) continue;
                        if (::connect(us, (sockaddr*)&ta, sizeof(ta)) != 0) { sock_close(us); continue; }
                        set_nonblocking(us);
                        tune_buffers(us, "被控端上游 UDP");
                        HostFlow hf; hf.up = us; hf.lastMs = now_ms(); hf.targetPort = targetPort;
                        hostFlows[fid] = hf;
                        it = hostFlows.find(fid);
                        logf("被控端：UDP 流 #%u -> %s:%u", fid, hcfg.targetHost.c_str(), targetPort);
                    }
                    it->second.viaTcp=false;
                    it->second.lastMs = now_ms();
                    ::send(it->second.up, (const char*)(pl + 2), int(payloadLen - 2), 0);
                    udpBytesUp.fetch_add(uint64_t(payloadLen - 2));
                    stat([](RelayStats& s) { s.udpPacketsUp++; });
                }
            } else if (e.kind == kHTunnel) {
                bool tunnelOk = true;
                if (fds[i].revents & POLLWRNORM) {
                    int rc2 = hTunnelOutQ.flush(hTunnel);
                    if (rc2 < 0) tunnelOk = false;
                }
                if (tunnelOk && (fds[i].revents & (POLLRDNORM | POLLHUP | POLLERR))) {
                    uint8_t tmp[8192];
                    int n = ::recv(hTunnel, (char*)tmp, sizeof(tmp), 0);
                    if (n <= 0) {
                        tunnelOk = false;
                    } else if (!hTcpIn.feed(tmp, size_t(n))) {
                        logf("被控端：收到非法或超长 TCP 帧，断开隧道");
                        tunnelOk = false;
                    }
                }
                if (!tunnelOk) {
                    host_drop_tunnel("隧道断开或发送队列错误，等待客户端重连");
                    break;
                }
            } else if (e.kind == kHStream) {
                const uint16_t sid = uint16_t(e.index);
                auto it = hostStreams.find(sid);
                if (it == hostStreams.end()) continue;

                if (fds[i].revents & POLLWRNORM) {
                    int rc2 = it->second.outQ.flush(it->second.up);
                    if (rc2 < 0) {
                        sock_close(it->second.up);
                        hostStreams.erase(it);
                        if (!send_tcp_frame_queued(hTunnel, hTunnelOutQ, kTcpStreamClose, sid, nullptr, 0)) {
                            host_drop_tunnel("隧道发送队列溢出/错误，断开");
                            break;
                        }
                        continue;
                    }
                }
                if (!(fds[i].revents & (POLLRDNORM | POLLHUP | POLLERR))) continue;

                uint8_t buf[8192];
                int n = ::recv(it->second.up, (char*)buf, sizeof(buf), 0);
                if (n <= 0) {
                    sock_close(it->second.up);
                    hostStreams.erase(it);
                    if (!send_tcp_frame_queued(hTunnel, hTunnelOutQ, kTcpStreamClose, sid, nullptr, 0)) {
                        host_drop_tunnel("隧道发送队列溢出/错误，断开");
                        break;
                    }
                } else if (send_tcp_frame_queued(hTunnel, hTunnelOutQ, kTcpStreamData, sid, buf, size_t(n))) {
                    stat([&](RelayStats& s) { s.tcpBytesDown += size_t(n); });
                } else {
                    host_drop_tunnel("隧道发送队列溢出/错误，断开");
                    break;
                }
            } else if (e.kind == kHUdpFlow) {
                const uint16_t fid = uint16_t(e.index);
                auto it = hostFlows.find(fid);
                if (it == hostFlows.end()) continue;

                bool tunnelOk = true;
                for (;;) {
                    uint8_t buf[kMaxUdpPayload + 16];
                    int n = ::recv(it->second.up, (char*)buf, sizeof(buf), 0);
                    if (n <= 0) break;
                    if (size_t(n) > kMaxUdpPayload) {
                        // 超过单包上限会触发 IP 分片（任一分片丢失=整包丢失），宁可不发。
                        stat([](RelayStats& s) { s.udpDropped++; });
                        continue;
                    }
                    if (it->second.viaTcp) {
                        if (!sock_valid(hTunnel)) { tunnelOk = false; break; }
                        if (!send_tcp_frame_queued(hTunnel, hTunnelOutQ, kTcpUdpDown, fid,
                                                   buf, size_t(n))) {
                            tunnelOk = false;
                            break;
                        }
                    } else if (clientUdpKnown) {
                        const uint32_t seq = ++udpSeq;
                        if (!send_udp_packet(hUdpListen, clientUdpAddr, kUdpData, fid,
                                        seq, sessionKey,
                                        buf, size_t(n), &clientUdpRoute, hostUdpIo.send)) {
                            udpReplyErrors.fetch_add(1);
                            lastUdpSendError.store(WSAGetLastError());
                            continue;
                        }
                        // 抗丢包：下行副本（视频是丢包的主要受害者）。被控端可开关。
                        queue_udp_duplicate(hUdpListen, clientUdpAddr, &clientUdpRoute, true,
                                            kUdpData, fid, seq, buf, size_t(n),
                                            it->second.targetPort);
                    } else {
                        continue;
                    }
                    udpBytesDown.fetch_add(uint64_t(n));
                    stat([](RelayStats& s) { s.udpPacketsDown++; });
                }
                if (!tunnelOk) {
                    host_drop_tunnel("隧道发送队列溢出/错误，断开");
                    break;
                }
            }
        }

        // 处理本轮及上一轮缓冲区里已经完整到达的帧（含刚提升为会话的连接）。
        if (sock_valid(hTunnel) && !process_host_frames()) {
            host_drop_tunnel("隧道帧处理失败，断开");
        }

        if (now_ms() - lastPingMs > 1000) {
            lastPingMs = now_ms();
            if (sock_valid(hTunnel)) {
                pingSentMs.store(now_ms());
                if (!send_tcp_frame_queued(hTunnel, hTunnelOutQ, kTcpPing, 0, nullptr, 0))
                    host_drop_tunnel("控制帧发送失败，断开");
            }
        }
        bool anyFallback=false;for(const auto& flow:hostFlows)anyFallback|=flow.second.viaTcp;hostFallback.store(anyFallback);
        stat([&](RelayStats& s){s.activeTcpStreams=uint32_t(hostStreams.size());s.activeUdpFlows=uint32_t(hostFlows.size());s.queuedBytes=hTunnelOutQ.totalBytes;});
        if (now_ms() - lastSweepMs > 30000) {
            lastSweepMs = now_ms();
            const uint64_t cutoff = now_ms() - uint64_t(kUdpFlowIdleMs);
            for (auto it = hostFlows.begin(); it != hostFlows.end();) {
                if (it->second.lastMs < cutoff) { sock_close(it->second.up); it = hostFlows.erase(it); }
                else ++it;
            }
        }
    }

    host_close_all();
    sock_close(hTunnel);
    sock_close(hTcpListen);
    sock_close(hUdpListen);
    {
        std::lock_guard<std::mutex> lk(pendingMtx);
        host_close_pending_locked();
    }
    set_tunnel_up(false);
}

std::string Relay::Impl::telemetry_json() const {
    auto s=snapshot();auto n=now_ms();auto lastUdp=lastAuthenticatedUdpMs.load();auto connected=connectedAtMs.load();bool host=mode==Mode::Host;
    std::string peer;uint64_t peerAt;{std::lock_guard<std::mutex> lock(statMtx);peer=peerReport;peerAt=peerReportAt;}
    std::string media;uint64_t mediaAt;{std::lock_guard<std::mutex> lock(g_mediaMutex);media=g_mediaJson;mediaAt=g_mediaAt;}
    std::ostringstream o;o<<"{\"schema\":1,\"updated_unix_ms\":"<<unix_ms()<<",\"pid\":"<<GetCurrentProcessId()
      <<",\"role\":"<<json_quote(host?"host":"client")<<",\"device_name\":"<<json_quote(thisDevice)
      <<",\"tunnel_up\":"<<(s.tunnelUp?"true":"false")<<",\"connected_seconds\":"<<(connected?(n-connected)/1000:0)
      <<",\"rtt_ms\":"<<(lastPongMs.load() && n-lastPongMs.load()<8000?std::to_string(rttMs.load()):"null")
      <<",\"rtt_jitter_ms\":"<<jitterMs.load()<<",\"transport\":"<<json_quote((host?hostFallback.load():udpViaTcp.load())?"tcp_fallback":(host?(udpReplyRoutePinned.load()&&lastUdp&&n>=lastUdp&&n-lastUdp<8000):udpDirectOk.load())?"udp":"probing")
      <<",\"tcp_bytes_up\":"<<s.tcpBytesUp<<",\"tcp_bytes_down\":"<<s.tcpBytesDown
      <<",\"udp_bytes_up\":"<<udpBytesUp.load()<<",\"udp_bytes_down\":"<<udpBytesDown.load()
      <<",\"udp_local_dropped\":"<<s.udpDropped<<",\"reconnects\":"<<s.tunnelReconnects
      <<",\"active_tcp_streams\":"<<s.activeTcpStreams<<",\"active_udp_flows\":"<<s.activeUdpFlows
      <<",\"queued_bytes\":"<<s.queuedBytes
      <<",\"udp_last_success_age_ms\":"<<(lastUdp?std::to_string(n>=lastUdp?n-lastUdp:0):"null")
      <<",\"udp_probe_sent\":"<<udpProbeSent.load()<<",\"udp_probe_ack\":"<<udpProbeAck.load()
      <<",\"udp_auth_rejected\":"<<udpAuthRejected.load()<<",\"udp_data_received\":"<<udpDataReceived.load()<<",\"udp_last_send_error\":"<<lastUdpSendError.load()
      <<",\"udp_hello_received\":"<<udpHelloReceived.load()<<",\"udp_hello_ack_sent\":"<<udpHelloAckSent.load()
      <<",\"udp_session_rejected\":"<<udpSessionRejected.load()<<",\"udp_reply_errors\":"<<udpReplyErrors.load()
      <<",\"udp_reply_route_pinned\":"<<(udpReplyRoutePinned.load()?"true":"false")
      <<",\"udp_redundancy\":"<<json_quote(udp_redundancy_name(redundancy_mode()))
      <<",\"udp_redundancy_gap_ms\":"<<redundancy_gap_ms()
      <<",\"udp_redundant_sent\":"<<s.udpRedundantSent<<",\"udp_duplicate_received\":"<<s.udpDuplicates
      <<",\"peer_report_age_ms\":"<<(peerAt?n-peerAt:UINT64_MAX)<<",\"peer_report_json\":"<<json_quote(peer)
      <<",\"media_age_ms\":"<<(mediaAt?n-mediaAt:UINT64_MAX)<<",\"media\":"<<(!host&&!media.empty()&&n-mediaAt<5000?media:"null")<<"}";
    return o.str();
}

// =============================================================================
//  周期统计（验收用）：每 5 秒一行，机器可解析
// =============================================================================
void Relay::Impl::stats_loop() {
    uint64_t prevMs = now_ms();
    RelayStats prev = snapshot();
    uint64_t prevUdpUp = udpBytesUp.load();
    uint64_t prevUdpDown = udpBytesDown.load();

    while (!stopReq.load()) {
        for (int i = 0; i < 50 && !stopReq.load(); ++i) ::Sleep(100);
        if (stopReq.load()) break;

        const uint64_t n = now_ms();
        const double dt = double(n - prevMs) / 1000.0;
        if (dt <= 0.0) continue;

        const RelayStats cur = snapshot();
        const uint64_t uUp = udpBytesUp.load();
        const uint64_t uDown = udpBytesDown.load();
        auto mbps = [&](uint64_t a, uint64_t b) {
            return double(b - a) * 8.0 / dt / 1000000.0;
        };

        SYSTEMTIME stt;
        ::GetLocalTime(&stt);
        logf("[stats] epoch_ms=%llu t=%02u:%02u:%02u.%03u up=%d rtt_ms=%u "
             "tcp_up_B=%llu tcp_down_B=%llu udp_up_B=%llu udp_down_B=%llu "
             "tcp_up_Mbps=%.3f tcp_down_Mbps=%.3f udp_up_Mbps=%.3f udp_down_Mbps=%.3f "
             "drop=%llu streams=%llu reconn=%llu",
             (unsigned long long)n,
             unsigned(stt.wHour), unsigned(stt.wMinute), unsigned(stt.wSecond),
             unsigned(stt.wMilliseconds),
             cur.tunnelUp ? 1 : 0, rttMs.load(),
             (unsigned long long)cur.tcpBytesUp, (unsigned long long)cur.tcpBytesDown,
             (unsigned long long)uUp, (unsigned long long)uDown,
             mbps(prev.tcpBytesUp, cur.tcpBytesUp),
             mbps(prev.tcpBytesDown, cur.tcpBytesDown),
             mbps(prevUdpUp, uUp), mbps(prevUdpDown, uDown),
             (unsigned long long)cur.udpDropped,
             (unsigned long long)cur.tcpStreamsOpened,
             (unsigned long long)cur.tunnelReconnects);

        prev = cur;
        prevMs = n;
        prevUdpUp = uUp;
        prevUdpDown = uDown;
    }
}

// =============================================================================
//  公共接口
// =============================================================================
Relay::Relay() : impl_(new Impl()) {}
Relay::~Relay() { stop(); }

bool Relay::start_client(const ClientConfig& cfg, std::string& err) { return impl_->start_client(cfg, err); }
bool Relay::start_host(const HostConfig& cfg, std::string& err) { return impl_->start_host(cfg, err); }

void Relay::Impl::stop_impl() {
    if (!runningFlag.load()) return;
    stopReq.store(true);
    // P1-3: 取消阻塞：shutdown 所有 socket 唤醒 WSAPoll
    if (sock_valid(tunnelTcp)) ::shutdown(tunnelTcp, SD_BOTH);
    if (sock_valid(hTunnel)) ::shutdown(hTunnel, SD_BOTH);
    {
        std::lock_guard<std::mutex> lk(pendingMtx);
        for (auto& kv : pendingHandshakes)
            if (sock_valid(kv.second.sock)) ::shutdown(kv.second.sock, SD_BOTH);
    }
    if (sock_valid(udpTunnel)) ::shutdown(udpTunnel, SD_BOTH);
    if (th.joinable()) th.join();
    if (statsTh.joinable()) statsTh.join();
    runningFlag.store(false);
    ::WSACleanup();
}

void Relay::stop() { impl_->stop_impl(); }
bool Relay::running() const { return impl_->runningFlag.load(); }
std::string Relay::telemetry_json() const {return impl_->telemetry_json();}

RelayStats Relay::stats() const {
    RelayStats s = impl_->snapshot();
    s.tunnelUp = impl_->tunnelUp.load();
    return s;
}

// P2-5: 查询 UDP 传输模式
bool Relay::udpDirectOk() const {
    return impl_->udpDirectOk.load();
}
bool Relay::udpViaTcp() const {
    return impl_->udpViaTcp.load();
}

void Relay::set_log_sink(void (*sink)(const char*, void*), void* ctx) {
    g_logSink = sink;
    g_logCtx = ctx;
}

} // namespace mslink
