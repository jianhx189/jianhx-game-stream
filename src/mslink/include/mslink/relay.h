// =============================================================================
//  mslink/relay.h —— 2 端口复用通道（1 TCP + 1 UDP 承载 Moonlight 的 6 条流）
//
//  背景：
//    Moonlight/Sunshine 需要 6 条独立的流：
//      TCP  47989 HTTP（首次 serverinfo）
//      TCP  47984 HTTPS（配对 / 应用列表 / launch）
//      TCP  48010 RTSP（串流信令）
//      UDP  47998 视频 RTP
//      UDP  47999 控制 ENet（键鼠，无 TCP 回退）
//      UDP  48000 音频 RTP
//    而目标环境（买的 FRP）只提供 1 个 TCP + 1 个 UDP。
//
//  做法（两端各一层复用器，Sunshine/Moonlight 协议一律不动）：
//
//    客户端：在 127.0.0.1 上**原样监听**那 6 个标准端口。
//            Moonlight 连 127.0.0.1 → 命中本地监听 → 全部塞进 1 TCP + 1 UDP 隧道。
//            因为 Sunshine 广告的端口号本来就是这 6 个标准端口，
//            **不需要改写任何 RTSP/serverinfo 响应**，TLS 证书 pinning 也不受影响。
//    被控端：监听 2 个端口，按「目标端口」把流拆回 127.0.0.1 上的 Sunshine。
//
//  为什么 UDP 侧不用包头嗅探：
//    Moonlight 为视频/控制/音频各开一个独立的 UDP socket（源端口不同），
//    所以按 (客户端地址, 源端口) 建一张流表就够了，比解析 RTP/ENet 头稳得多。
//
//  协议：
//    TCP 帧： [u32 payloadLen][u16 type][u16 streamId][payload...]
//    UDP 包： [u8 magic='M'][u8 type][u16 flowId][u32 seq][payload...]
// =============================================================================
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include <memory>

namespace mslink {

// ---- 协议常量 ---------------------------------------------------------------
constexpr uint8_t  kUdpMagic        = 0x4D;   // 'M'
constexpr uint32_t kTunnelVersion   = 1;
constexpr size_t   kMaxTcpFrame     = 64 * 1024;   // 单帧上限（TLS 握手/RTSP 响应都远小于此）
// 单包上限。约束来自最外层 UDP 载荷不能超过 1472B（1500 MTU - 20 IP - 8 UDP），
// 否则会被 IP 分片，任一分片丢失就等于整包丢失，视频链路上不可接受。
// 我们的封装头是 8 字节，所以内层最多 1472 - 8 = 1464。
// Sunshine 默认 packetSize=1024，但它是可在被控端配置里调大的，这里留足余量。
constexpr size_t   kMaxUdpPayload   = 1472 - 8;
constexpr int      kUdpFlowIdleMs   = 120000;      // UDP 流表空闲回收（2 分钟）

enum TcpFrameType : uint16_t {
    kTcpHello      = 1,   // client -> host：版本 + 认证 token
    kTcpHelloAck   = 2,   // host -> client：认证结果
    kTcpStreamOpen = 3,   // client -> host：要连的目标端口
    kTcpStreamData = 4,   // 双向：字节流
    kTcpStreamClose= 5,   // 双向：关流
    kTcpPing       = 6,
    kTcpPong       = 7,
    // 兜底通道：有些穿透/映射服务只让 UDP 去程通过、回程被丢（实测遇到过）。
    // 这种情况下把 UDP 流改走 TCP 隧道，牺牲一点实时性换取"能用"。
    kTcpUdpUp      = 8,   // client -> host：streamId=flowId，payload=[u16 目标端口][UDP 数据]
    kTcpUdpDown    = 9,   // host -> client：streamId=flowId，payload=[UDP 数据]
    // P1-4：TCP 认证成功后，host -> client 下发 32 字节会话密钥（UDP 包校验用）
    kTcpSessionKey = 10,
};

enum UdpPacketType : uint8_t {
    kUdpHello = 1,   // client -> host：认证 + 声明本端地址（建立回程映射）
    kUdpData  = 2,   // 双向：一个完整 UDP 数据报
    kUdpClose = 3,   // 双向：关流
    kUdpHelloAck = 4, // host -> client：HELLO 应答（客户端据此判断"UDP 回程通不通"）
};

// ---- 配置 -------------------------------------------------------------------
struct Endpoint {
    std::string host = "127.0.0.1";
    uint16_t    port = 0;
};

// 客户端（远控端）配置
struct ClientConfig {
    std::string serverHost;          // FRP 的 TCP/UDP 目标地址（可同一主机）
    uint16_t    serverTcpPort = 0;   // FRP 映射的 TCP 端口
    uint16_t    serverUdpPort = 0;   // FRP 映射的 UDP 端口
    std::string password;            // 与被控端一致的共享口令

    // 本机监听（Moonlight 连这些地址）。留空 = 用默认的 6 个标准端口。
    std::vector<uint16_t> tcpListenPorts;   // 默认 {47989, 47984, 48010}
    std::vector<uint16_t> udpListenPorts;   // 默认 {47998, 47999, 48000}
    std::string listenAddr = "127.0.0.1";
};

// 被控端配置
struct HostConfig {
    uint16_t    listenTcpPort = 0;   // 监听 FRP 转发进来的 TCP
    uint16_t    listenUdpPort = 0;   // 监听 FRP 转发进来的 UDP
    std::string password;
    std::string bindAddr = "0.0.0.0";

    // 拆回本机 Sunshine 的目标主机（默认本机）
    std::string targetHost = "127.0.0.1";

    // 逻辑端口 -> 实际目标端口。留空 = 同号直转（Sunshine 用标准端口时即如此）。
    // 若 Sunshine 改过 base port，或做本机自测（逻辑端口与目标端口在同一台机器会冲突），
    // 用这里做映射。
    std::map<uint16_t, uint16_t> tcpPortMap;
    std::map<uint16_t, uint16_t> udpPortMap;

    uint16_t resolve_tcp(uint16_t logical) const {
        auto it = tcpPortMap.find(logical);
        return it == tcpPortMap.end() ? logical : it->second;
    }
    uint16_t resolve_udp(uint16_t logical) const {
        auto it = udpPortMap.find(logical);
        return it == udpPortMap.end() ? logical : it->second;
    }
};

// ---- 运行统计（验收/诊断用）--------------------------------------------------
struct RelayStats {
    uint64_t tcpStreamsOpened = 0;
    uint64_t tcpBytesUp = 0;         // 本机 -> 对端
    uint64_t tcpBytesDown = 0;       // 对端 -> 本机
    uint64_t udpPacketsUp = 0;
    uint64_t udpPacketsDown = 0;
    uint64_t udpDropped = 0;         // 隧道未就绪/流表满
    uint64_t tunnelReconnects = 0;
    bool     tunnelUp = false;
};

// ---- 运行主体 ---------------------------------------------------------------
class Relay {
public:
    Relay();
    ~Relay();

    // 启动后立即返回；内部线程跑事件循环。失败返回 false 并填 err。
    bool start_client(const ClientConfig& cfg, std::string& err);
    bool start_host(const HostConfig& cfg, std::string& err);
    void stop();
    bool running() const;
    RelayStats stats() const;

    // 传入非空回调即可收到运行日志（默认打到 stderr）
    void set_log_sink(void (*sink)(const char* line, void* ctx), void* ctx);

    // P2-5: 查询 UDP 传输模式（仅供 UI 显示真实状态）
    bool udpDirectOk() const;   // UDP 回程探测是否成功
    bool udpViaTcp() const;     // 是否已回退到 TCP 兼容模式

    struct Impl;
private:
    std::unique_ptr<Impl> impl_;
};

} // namespace mslink
