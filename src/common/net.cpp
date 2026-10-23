// =============================================================================
//  net.cpp - 套接字封装实现
// =============================================================================
#include "net.h"
#include "log.h"
#include "clock.h"

#include <mstcpip.h>
#include <cstring>

// mstcpip.h 里这个 ioctl 码在某些 SDK/宏组合下不会被声明（它受 NTDDI_VERSION 保护），
// 而它是关闭 UDP ICMP 重置上报的唯一手段，所以这里按官方定义兜底。
// _WSAIOW(IOC_VENDOR, 12) 是稳定的 ABI 值。
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

namespace ms::net {
namespace {

bool g_wsaInit = false;
int  g_wsaRef  = 0;

// 等待可读；返回 true 可读、false 超时/出错
bool wait_readable(SOCKET s, int timeoutMs) {
    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(s, &rd);
    timeval tv{};
    timeval* ptv = nullptr;
    if (timeoutMs >= 0) {
        tv.tv_sec  = timeoutMs / 1000;
        tv.tv_usec = (timeoutMs % 1000) * 1000;
        ptv = &tv;
    }
    const int r = ::select(0, &rd, nullptr, nullptr, ptv);
    return r > 0;
}

bool wait_writable(SOCKET s, int timeoutMs) {
    fd_set wr;
    FD_ZERO(&wr);
    FD_SET(s, &wr);
    timeval tv{};
    timeval* ptv = nullptr;
    if (timeoutMs >= 0) {
        tv.tv_sec  = timeoutMs / 1000;
        tv.tv_usec = (timeoutMs % 1000) * 1000;
        ptv = &tv;
    }
    const int r = ::select(0, nullptr, &wr, nullptr, ptv);
    return r > 0;
}

} // namespace

bool init() {
    if (g_wsaRef++ > 0) { g_wsaInit = true; return true; }
    WSADATA wsa{};
    const int r = ::WSAStartup(MAKEWORD(2, 2), &wsa);
    if (r != 0) {
        MS_ERR("WSAStartup 失败: %d", r);
        --g_wsaRef;
        return false;
    }
    g_wsaInit = true;
    return true;
}

void shutdown() {
    if (--g_wsaRef <= 0) {
        g_wsaRef = 0;
        if (g_wsaInit) { ::WSACleanup(); g_wsaInit = false; }
    }
}

bool resolve_ipv4(const std::string& host, u16 port, sockaddr_in& out, std::string& err) {
    std::memset(&out, 0, sizeof(out));
    out.sin_family = AF_INET;
    out.sin_port = htons(port);

    // 先按字面量解析
    if (::inet_pton(AF_INET, host.c_str(), &out.sin_addr) == 1) return true;

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    const int rc = ::getaddrinfo(host.c_str(), nullptr, &hints, &res);
    if (rc != 0 || !res) {
        err = "无法解析地址: " + host;
        if (res) ::freeaddrinfo(res);
        return false;
    }
    out.sin_addr = reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr;
    ::freeaddrinfo(res);
    return true;
}

std::string addr_str(const sockaddr_in& a) {
    char buf[INET_ADDRSTRLEN] = {0};
    ::inet_ntop(AF_INET, &a.sin_addr, buf, sizeof(buf));
    return std::string(buf);
}

u16 addr_port(const sockaddr_in& a) { return ntohs(a.sin_port); }

bool addr_same(const sockaddr_in& a, const sockaddr_in& b) {
    return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
}

// -----------------------------------------------------------------------------
// UDP
// -----------------------------------------------------------------------------
int udp_open(const std::string& bindAddr, u16 port, std::string& err) {
    SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) { err = "创建 UDP 套接字失败"; return -1; }

    BOOL yes = TRUE;
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));

    // 关闭 ICMP 触发的连接重置上报（Windows 默认开启）。
    // 不关的话：UDP 是无连接的，只要之前发给某个地址的包换来一个 ICMP
    // "port unreachable"（对端进程退出 / NAT 回收映射时很常见），下一次 recvfrom
    // 就会返回 WSAECONNRESET(10054)，而且会持续返回，把接收循环刷成死循环刷屏。
    {
        BOOL disable = FALSE;
        DWORD returned = 0;
        ::WSAIoctl(s, SIO_UDP_CONNRESET, &disable, sizeof(disable),
                   nullptr, 0, &returned, nullptr, nullptr);
    }

    // 帧率越高，缓冲需求越小；给 4MB 降低突发丢包
    int bufsz = 4 * 1024 * 1024;
    ::setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&bufsz), sizeof(bufsz));
    ::setsockopt(s, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&bufsz), sizeof(bufsz));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (bindAddr.empty() || bindAddr == "0.0.0.0" || bindAddr == "*") {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else if (::inet_pton(AF_INET, bindAddr.c_str(), &addr.sin_addr) != 1) {
        err = "无效的绑定地址: " + bindAddr;
        ::closesocket(s);
        return -1;
    }

    if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        err = "UDP 绑定 " + bindAddr + ":" + std::to_string(port) + " 失败，错误码 " +
              std::to_string(::WSAGetLastError());
        ::closesocket(s);
        return -1;
    }
    return int(s);
}

bool udp_set_nonblocking(int s, bool nb) {
    u_long mode = nb ? 1 : 0;
    return ::ioctlsocket(SOCKET(s), FIONBIO, &mode) == 0;
}

bool udp_set_buffers(int s, int recvBytes, int sendBytes) {
    bool ok = true;
    if (recvBytes > 0)
        ok &= ::setsockopt(SOCKET(s), SOL_SOCKET, SO_RCVBUF,
                           reinterpret_cast<const char*>(&recvBytes), sizeof(recvBytes)) == 0;
    if (sendBytes > 0)
        ok &= ::setsockopt(SOCKET(s), SOL_SOCKET, SO_SNDBUF,
                           reinterpret_cast<const char*>(&sendBytes), sizeof(sendBytes)) == 0;
    return ok;
}

int udp_recv_from(int s, void* buf, size_t len, sockaddr_in& from) {
    int fromLen = sizeof(from);
    const int r = ::recvfrom(SOCKET(s), static_cast<char*>(buf), int(len), 0,
                             reinterpret_cast<sockaddr*>(&from), &fromLen);
    return r;
}

int udp_send_to(int s, const void* buf, size_t len, const sockaddr_in& to) {
    return ::sendto(SOCKET(s), static_cast<const char*>(buf), int(len), 0,
                    reinterpret_cast<const sockaddr*>(&to), sizeof(to));
}

// -----------------------------------------------------------------------------
// TcpChannel
// -----------------------------------------------------------------------------
TcpChannel::~TcpChannel() { close(); }

bool TcpChannel::listen(const std::string& bindAddr, u16 port, std::string& err) {
    close();
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) { err = "创建 TCP 套接字失败"; return false; }

    // 【故意不设 SO_REUSEADDR】Windows 上它允许两个进程绑同一端口抢连接：
    // 被控端被起两个实例时，新实例照常"监听成功"，但连接会被旧实例吃掉，
    // 现象是"连上了却永远没有 Challenge"——正是最难排查的那种故障。
    // 不设它，第二个实例会在 bind 时直接报"端口被占用"，一眼定位。

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (bindAddr.empty() || bindAddr == "0.0.0.0" || bindAddr == "*")
        addr.sin_addr.s_addr = INADDR_ANY;
    else if (::inet_pton(AF_INET, bindAddr.c_str(), &addr.sin_addr) != 1) {
        err = "无效的绑定地址: " + bindAddr;
        ::closesocket(s);
        return false;
    }

    if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        err = "TCP 绑定 " + bindAddr + ":" + std::to_string(port) + " 失败，错误码 " +
              std::to_string(::WSAGetLastError());
        ::closesocket(s);
        return false;
    }
    if (::listen(s, 1) == SOCKET_ERROR) {
        err = "TCP listen 失败，错误码 " + std::to_string(::WSAGetLastError());
        ::closesocket(s);
        return false;
    }
    sock_ = s;
    return true;
}

bool TcpChannel::accept(int timeoutMs, std::string& err) {
    if (sock_ == INVALID_SOCKET) { err = "尚未监听"; return false; }
    if (!wait_readable(sock_, timeoutMs)) { err = "等待连接超时"; return false; }

    sockaddr_in peer{};
    int plen = sizeof(peer);
    SOCKET c = ::accept(sock_, reinterpret_cast<sockaddr*>(&peer), &plen);
    if (c == INVALID_SOCKET) {
        err = "accept 失败，错误码 " + std::to_string(::WSAGetLastError());
        return false;
    }
    // 监听套接字用完就关，避免重复 accept
    ::closesocket(sock_);
    sock_ = c;
    peerIp_ = addr_str(peer);
    peerPort_ = addr_port(peer);
    inbuf_.clear();

    BOOL nodelay = TRUE;
    ::setsockopt(sock_, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));
    return true;
}

bool TcpChannel::connect(const std::string& host, u16 port, int timeoutMs, std::string& err) {
    close();
    sockaddr_in addr{};
    if (!resolve_ipv4(host, port, addr, err)) return false;

    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) { err = "创建 TCP 套接字失败"; return false; }

    u_long nb = 1;
    ::ioctlsocket(s, FIONBIO, &nb);

    const int r = ::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (r == SOCKET_ERROR) {
        const int e = ::WSAGetLastError();
        if (e != WSAEWOULDBLOCK && e != WSAEINPROGRESS) {
            err = "连接 " + host + ":" + std::to_string(port) + " 失败，错误码 " + std::to_string(e);
            ::closesocket(s);
            return false;
        }
        if (!wait_writable(s, timeoutMs)) {
            err = "连接 " + host + ":" + std::to_string(port) + " 超时";
            ::closesocket(s);
            return false;
        }
        int soerr = 0;
        int slen = sizeof(soerr);
        ::getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soerr), &slen);
        if (soerr != 0) {
            err = "连接 " + host + ":" + std::to_string(port) + " 被拒绝，错误码 " + std::to_string(soerr);
            ::closesocket(s);
            return false;
        }
    }

    nb = 0;
    ::ioctlsocket(s, FIONBIO, &nb);
    BOOL nodelay = TRUE;
    ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));

    sock_ = s;
    peerIp_ = host;
    peerPort_ = port;
    inbuf_.clear();
    return true;
}

void TcpChannel::close() {
    if (sock_ != INVALID_SOCKET) {
        ::shutdown(sock_, SD_BOTH);
        ::closesocket(sock_);
        sock_ = INVALID_SOCKET;
    }
    inbuf_.clear();
}

bool TcpChannel::send_frame(proto::MsgType t, const Bytes& payload) {
    if (sock_ == INVALID_SOCKET) return false;
    const Bytes frame = proto::encode_tcp_frame(t, payload);
    // 串行化发送：控制循环回 Pong 与串流线程发 TCP 视频帧可能并发
    std::lock_guard<std::mutex> lk(sendMtx_);
    size_t sent = 0;
    while (sent < frame.size()) {
        const int n = ::send(sock_, reinterpret_cast<const char*>(frame.data() + sent),
                             int(frame.size() - sent), 0);
        if (n == SOCKET_ERROR) {
            const int e = ::WSAGetLastError();
            if (e == WSAEWOULDBLOCK) {
                if (!wait_writable(sock_, 2000)) return false;
                continue;
            }
            MS_WARN("TCP send 失败, 错误码 %d", e);
            return false;
        }
        sent += size_t(n);
    }
    return true;
}

int TcpChannel::recv_frame(proto::MsgType& t, Bytes& payload, int timeoutMs) {
    const i64 deadline = timeoutMs < 0 ? INT64_MAX : (now_us() + i64(timeoutMs) * 1000);

    for (;;) {
        // 缓冲区里已经有完整帧？
        if (inbuf_.size() >= proto::kTcpHeaderSize) {
            proto::TcpHeader h;
            if (!proto::parse_tcp_header(inbuf_.data(), inbuf_.size(), h)) {
                MS_WARN("收到非法 TCP 帧头，断开");
                return -1;
            }
            const size_t total = 4 + h.length;
            if (inbuf_.size() >= total) {
                const size_t plen = h.length - 4;
                payload.assign(inbuf_.begin() + proto::kTcpHeaderSize,
                               inbuf_.begin() + proto::kTcpHeaderSize + plen);
                inbuf_.erase(inbuf_.begin(), inbuf_.begin() + total);
                t = h.type;
                return 1;
            }
        }

        // 需要再读
        int waitMs;
        if (timeoutMs < 0) {
            waitMs = -1;
        } else {
            const i64 left = deadline - now_us();
            if (left <= 0) return 0;
            waitMs = int((left + 999) / 1000);
        }
        if (!wait_readable(sock_, waitMs)) {
            return (timeoutMs < 0) ? -1 : 0;
        }

        char buf[8192];
        const int n = ::recv(sock_, buf, sizeof(buf), 0);
        if (n == 0) { MS_INFO("TCP 对端关闭连接"); return -1; }
        if (n == SOCKET_ERROR) {
            const int e = ::WSAGetLastError();
            if (e == WSAEWOULDBLOCK) continue;
            MS_WARN("TCP recv 失败, 错误码 %d", e);
            return -1;
        }
        inbuf_.insert(inbuf_.end(), buf, buf + n);
        if (inbuf_.size() > proto::kMaxTcpPayload + 1024) {
            MS_WARN("TCP 接收缓冲异常增长，断开");
            return -1;
        }
    }
}

std::string TcpChannel::peer_str() const {
    if (peerIp_.empty()) return "?";
    return peerIp_ + ":" + std::to_string(peerPort_);
}

bool TcpChannel::local_addr(sockaddr_in& out) const {
    if (sock_ == INVALID_SOCKET) return false;
    sockaddr_in a{};
    int len = sizeof(a);
    if (::getsockname(sock_, reinterpret_cast<sockaddr*>(&a), &len) != 0) return false;
    out = a;
    return true;
}

} // namespace ms::net
