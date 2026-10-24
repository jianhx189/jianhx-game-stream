// Startup failure-path tests for the mslink relay:
//   - a failed start_client / start_host closes every socket it had already created
//     (so the caller can immediately retry on the same ports, e.g. the 47990 fallback)
//   - an invalid listen / bind address is rejected instead of binding 0.0.0.0
//   - start_client on the same ports succeeds again after the failure is removed
//   - stop() during the client handshake does not wait for the socket timeouts
// The implementation is included (white box), like the other relay tests.
#include "../src/relay.cpp"

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::printf("[FAIL] %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        ++failures; \
    } \
} while (0)

static void test_log(const char* line, void*) {
    std::fprintf(stderr, "[relay] %s\n", line);
}

static uint64_t tnow_ms() {
    return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

static void sleep_ms(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// 取一个当前空闲的端口（bind 0 后立即释放）。
static uint16_t free_port(int type) {
    const int proto = type == SOCK_STREAM ? IPPROTO_TCP : IPPROTO_UDP;
    SOCKET s = ::socket(AF_INET, type, proto);
    if (!mslink::sock_valid(s)) return 0;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    uint16_t port = 0;
    if (::bind(s, (sockaddr*)&a, sizeof(a)) == 0) {
        int len = sizeof(a);
        if (::getsockname(s, (sockaddr*)&a, &len) == 0) port = ntohs(a.sin_port);
    }
    ::closesocket(s);
    return port;
}

// 占住一个端口，用来制造 bind 失败。
static SOCKET occupy(int type, uint16_t port) {
    const int proto = type == SOCK_STREAM ? IPPROTO_TCP : IPPROTO_UDP;
    SOCKET s = ::socket(AF_INET, type, proto);
    if (!mslink::sock_valid(s)) return INVALID_SOCKET;
    // SO_EXCLUSIVEADDRUSE：明确禁止别人（即使带 SO_REUSEADDR）再绑定同一端口，
    // 否则 Windows 会允许「后来者带 REUSEADDR 抢绑」，测试会假通过。
    BOOL exclusive = TRUE;
    ::setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&exclusive, sizeof(exclusive));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(port);
    if (::bind(s, (sockaddr*)&a, sizeof(a)) != 0 ||
        (type == SOCK_STREAM && ::listen(s, 1) != 0)) {
        ::closesocket(s);
        return INVALID_SOCKET;
    }
    return s;
}

// 关键判定：不带 SO_REUSEADDR 能否绑上。
// Windows 上已存在的监听口即使带 SO_REUSEADDR 绑定，也会阻止后来者无 REUSEADDR 绑定；
// 反过来说这里能绑上，就说明失败路径确实把 socket 关掉了。
static bool can_bind(int type, uint16_t port) {
    SOCKET s = occupy(type, port);
    if (!mslink::sock_valid(s)) return false;
    ::closesocket(s);
    return true;
}

static mslink::ClientConfig client_cfg(const std::vector<uint16_t>& tcpPorts,
                                       const std::vector<uint16_t>& udpPorts) {
    mslink::ClientConfig cc;
    cc.serverHost = "127.0.0.1";
    cc.serverTcpPort = 2;          // 未监听：握手必然失败，但启动阶段不依赖它
    cc.serverUdpPort = 3;
    cc.password = "startup-test-password";
    cc.listenAddr = "127.0.0.1";
    cc.tcpListenPorts = tcpPorts;
    cc.udpListenPorts = udpPorts;
    return cc;
}

static mslink::HostConfig host_cfg(uint16_t tcpPort, uint16_t udpPort) {
    mslink::HostConfig hc;
    hc.listenTcpPort = tcpPort;
    hc.listenUdpPort = udpPort;
    hc.bindAddr = "127.0.0.1";
    hc.targetHost = "127.0.0.1";
    hc.password = "startup-test-password";
    return hc;
}

// 客户端：UDP 绑定失败后，之前已建好的 TCP 监听必须已经关掉；
// 端口释放后同一组端口能再次启动成功。
static void client_udp_failure_cleans_up() {
    const uint16_t t1 = free_port(SOCK_STREAM);
    const uint16_t t2 = free_port(SOCK_STREAM);
    const uint16_t u1 = free_port(SOCK_DGRAM);
    CHECK(t1 && t2 && u1);

    SOCKET blocker = occupy(SOCK_DGRAM, u1);
    CHECK(mslink::sock_valid(blocker));

    mslink::Relay relay;
    std::string err;
    CHECK(!relay.start_client(client_cfg({t1, t2}, {u1}), err));
    CHECK(err.find("UDP") != std::string::npos);
    CHECK(!relay.running());
    CHECK(can_bind(SOCK_STREAM, t1));
    CHECK(can_bind(SOCK_STREAM, t2));

    if (mslink::sock_valid(blocker)) ::closesocket(blocker);

    mslink::Relay retry;
    std::string err2;
    CHECK(retry.start_client(client_cfg({t1, t2}, {u1}), err2));
    CHECK(retry.running());
    sleep_ms(300);
    CHECK(retry.running());
    retry.stop();
    CHECK(can_bind(SOCK_STREAM, t1));
    CHECK(can_bind(SOCK_STREAM, t2));
    CHECK(can_bind(SOCK_DGRAM, u1));
}

// 主控端 47990 回退路径的根因：第一次带某个被占端口启动失败后，
// 必须能在同一进程里用剩下的端口立刻重试成功（否则降级也只是再失败一次）。
static void client_retry_after_port_taken() {
    const uint16_t a = free_port(SOCK_STREAM);
    const uint16_t b = free_port(SOCK_STREAM);
    const uint16_t taken = free_port(SOCK_STREAM);
    const uint16_t u = free_port(SOCK_DGRAM);
    CHECK(a && b && taken && u);

    SOCKET blocker = occupy(SOCK_STREAM, taken);
    CHECK(mslink::sock_valid(blocker));

    mslink::Relay first;
    std::string err;
    CHECK(!first.start_client(client_cfg({a, b, taken}, {u}), err));
    CHECK(err.find("TCP") != std::string::npos);
    CHECK(can_bind(SOCK_STREAM, a));
    CHECK(can_bind(SOCK_STREAM, b));
    if (mslink::sock_valid(blocker)) ::closesocket(blocker);

    // 这正是 MslinkBridge::startFromIni() 的降级动作：去掉被占端口再启动一次。
    mslink::Relay fallback;
    std::string err2;
    CHECK(fallback.start_client(client_cfg({a, b}, {u}), err2));
    CHECK(fallback.running());
    fallback.stop();
    CHECK(can_bind(SOCK_STREAM, a));
    CHECK(can_bind(SOCK_STREAM, b));
}

static void client_rejects_invalid_listen_addr() {
    const uint16_t t = free_port(SOCK_STREAM);
    const uint16_t u = free_port(SOCK_DGRAM);

    mslink::Relay relay;
    std::string err;
    mslink::ClientConfig cc = client_cfg({t}, {u});
    cc.listenAddr = "127.0.0.999";
    CHECK(!relay.start_client(cc, err));
    CHECK(err.find("不合法") != std::string::npos);
    CHECK(!relay.running());

    // 非法地址不应该留下任何监听口（旧实现会静默绑到 0.0.0.0）。
    CHECK(can_bind(SOCK_STREAM, t));
    CHECK(can_bind(SOCK_DGRAM, u));

    mslink::Relay retry;
    std::string err2;
    CHECK(retry.start_client(client_cfg({t}, {u}), err2));
    retry.stop();
}

static void host_udp_failure_cleans_up() {
    const uint16_t t = free_port(SOCK_STREAM);
    const uint16_t u = free_port(SOCK_DGRAM);
    CHECK(t && u);

    SOCKET blocker = occupy(SOCK_DGRAM, u);
    CHECK(mslink::sock_valid(blocker));

    mslink::Relay relay;
    std::string err;
    CHECK(!relay.start_host(host_cfg(t, u), err));
    CHECK(err.find("UDP") != std::string::npos);
    CHECK(!relay.running());
    CHECK(can_bind(SOCK_STREAM, t));

    if (mslink::sock_valid(blocker)) ::closesocket(blocker);

    mslink::Relay retry;
    std::string err2;
    CHECK(retry.start_host(host_cfg(t, u), err2));
    CHECK(retry.running());
    sleep_ms(200);
    CHECK(retry.running());
    retry.stop();
    CHECK(can_bind(SOCK_STREAM, t));
    CHECK(can_bind(SOCK_DGRAM, u));
}

static void host_rejects_invalid_bind_addr() {
    const uint16_t t = free_port(SOCK_STREAM);
    const uint16_t u = free_port(SOCK_DGRAM);

    mslink::HostConfig hc = host_cfg(t, u);
    hc.bindAddr = "not-an-ip";
    mslink::Relay relay;
    std::string err;
    CHECK(!relay.start_host(hc, err));
    CHECK(err.find("不合法") != std::string::npos);
    CHECK(!relay.running());
    CHECK(can_bind(SOCK_STREAM, t));
    CHECK(can_bind(SOCK_DGRAM, u));
}

// stop() 发生在客户端等待被控端 nonce 的阶段：不应该被 8 秒的 recv 超时拖住。
static void client_stop_during_handshake_is_fast() {
    const uint16_t t = free_port(SOCK_STREAM);
    const uint16_t u = free_port(SOCK_DGRAM);

    // 监听但不 accept：connect 能成功（进 backlog），之后客户端就一直等 nonce。
    SOCKET silent = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    CHECK(mslink::sock_valid(silent));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    CHECK(::bind(silent, (sockaddr*)&a, sizeof(a)) == 0);
    CHECK(::listen(silent, 4) == 0);
    int len = sizeof(a);
    CHECK(::getsockname(silent, (sockaddr*)&a, &len) == 0);
    const uint16_t silentPort = ntohs(a.sin_port);

    mslink::ClientConfig cc = client_cfg({t}, {u});
    cc.serverTcpPort = silentPort;
    mslink::Relay relay;
    std::string err;
    CHECK(relay.start_client(cc, err));
    sleep_ms(300);

    const uint64_t t0 = tnow_ms();
    relay.stop();
    const uint64_t dt = tnow_ms() - t0;
    std::printf("client stop during handshake: %llu ms\n", (unsigned long long)dt);
    CHECK(dt < 2000);
    CHECK(!relay.running());
    ::closesocket(silent);
}

int main() {
    WSADATA wsa{};
    CHECK(::WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
    {
        mslink::Relay logger;
        logger.set_log_sink(&test_log, nullptr);
    }
    client_udp_failure_cleans_up();
    client_retry_after_port_taken();
    client_rejects_invalid_listen_addr();
    host_udp_failure_cleans_up();
    host_rejects_invalid_bind_addr();
    client_stop_during_handshake_is_fast();

    if (failures) {
        std::printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("\nall startup checks passed\n");
    return 0;
}
