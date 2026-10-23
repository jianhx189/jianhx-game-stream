// =============================================================================
//  net.h - 套接字封装（TCP 控制通道 / UDP 视频与输入通道）
// =============================================================================
#pragma once

#include "types.h"
#include "protocol.h"
#include <string>
#include <mutex>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

namespace ms::net {

// 进程级初始化（WSAStartup）
bool init();
void shutdown();

// --- 地址 ---------------------------------------------------------------
bool resolve_ipv4(const std::string& host, u16 port, sockaddr_in& out, std::string& err);
std::string addr_str(const sockaddr_in& a);
u16 addr_port(const sockaddr_in& a);
bool addr_same(const sockaddr_in& a, const sockaddr_in& b);

// --- UDP ----------------------------------------------------------------
// 打开一个 UDP 套接字。bindAddr 为空表示绑定通配地址。
int udp_open(const std::string& bindAddr, u16 port, std::string& err);
bool udp_set_nonblocking(int s, bool nb);
bool udp_set_buffers(int s, int recvBytes, int sendBytes);
// 返回收到的字节数；<0 表示错误（含 WSAEWOULDBLOCK）
int udp_recv_from(int s, void* buf, size_t len, sockaddr_in& from);
int udp_send_to(int s, const void* buf, size_t len, const sockaddr_in& to);

// -----------------------------------------------------------------------------
//  TCP 消息通道：把「按字节的流」封装成「按帧的消息」。
//  单线程使用（控制通道只有心跳和少量控制消息）。
// -----------------------------------------------------------------------------
class TcpChannel {
public:
    TcpChannel() = default;
    ~TcpChannel();
    TcpChannel(const TcpChannel&) = delete;
    TcpChannel& operator=(const TcpChannel&) = delete;

    // 被控端：监听
    bool listen(const std::string& bindAddr, u16 port, std::string& err);
    // 被控端：等待客户端接入（timeoutMs < 0 表示阻塞）
    bool accept(int timeoutMs, std::string& err);
    // 远控端：主动连接
    bool connect(const std::string& host, u16 port, int timeoutMs, std::string& err);

    void close();
    bool is_open() const { return sock_ != INVALID_SOCKET; }

    bool send_frame(proto::MsgType t, const Bytes& payload);
    // 返回 >0: 收到一帧; 0: 超时; <0: 断开或错误
    int recv_frame(proto::MsgType& t, Bytes& payload, int timeoutMs);

    std::string peer_str() const;
    u16 peer_port() const { return peerPort_; }
    // 本端地址（getsockname）：被控端用它知道自己是被哪块本机 IP 连进来的。
    // 多网卡机器上这个信息很关键——回包源地址必须是客户端连入的那个地址，
    // 否则对端（尤其 FRP 的 connected UDP 套接字）会直接丢弃。
    bool local_addr(sockaddr_in& out) const;

private:
    SOCKET sock_ = INVALID_SOCKET;
    Bytes  inbuf_;
    // 【线程安全】被控端的控制循环（收/回 Pong）和串流线程（TCP 视频回退）
    // 会同时往同一个 socket 发消息，必须串行化，否则帧会交错损坏。
    std::mutex sendMtx_;
    std::string peerIp_;
    u16    peerPort_ = 0;
};

} // namespace ms::net
