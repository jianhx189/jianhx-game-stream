// host-side kTcpStreamOpen failure test:
//   when the host cannot reach the upstream target (resolve / socket / connect),
//   it must answer with kTcpStreamClose so the client's local stream closes at once
//   instead of waiting for its own read timeout.
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

static SOCKET connect_tcp(uint16_t port) {
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (!mslink::sock_valid(s)) return INVALID_SOCKET;
    DWORD tmo = 5000;
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof(tmo));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(port);
    if (::connect(s, (sockaddr*)&a, sizeof(a)) != 0) {
        ::closesocket(s);
        return INVALID_SOCKET;
    }
    return s;
}

// 返回收到 EOF/错误的毫秒数；超时返回 -1。
static long wait_peer_close(SOCKET s, int timeoutMs) {
    const uint64_t start = tnow_ms();
    for (;;) {
        char buf[256];
        int n = ::recv(s, buf, sizeof(buf), 0);
        if (n <= 0) return long(tnow_ms() - start);
        if (tnow_ms() - start > uint64_t(timeoutMs)) return -1;
    }
}

int main() {
    WSADATA wsa{};
    CHECK(::WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
    {
        mslink::Relay logger;
        logger.set_log_sink(&test_log, nullptr);
    }

    const uint16_t hostTcp = free_port(SOCK_STREAM);
    const uint16_t hostUdp = free_port(SOCK_DGRAM);
    const uint16_t clientTcp = free_port(SOCK_STREAM);
    const uint16_t clientUdp = free_port(SOCK_DGRAM);
    const uint16_t deadPort = free_port(SOCK_STREAM);
    CHECK(hostTcp && hostUdp && clientTcp && clientUdp && deadPort);

    mslink::HostConfig hc;
    hc.listenTcpPort = hostTcp;
    hc.listenUdpPort = hostUdp;
    hc.bindAddr = "127.0.0.1";
    hc.targetHost = "127.0.0.1";
    hc.password = "stream-open-failure-pw";
    hc.tcpPortMap[clientTcp] = deadPort;   // 客户端本机端口 -> 一个没人监听的目标端口

    mslink::ClientConfig cc;
    cc.serverHost = "127.0.0.1";
    cc.serverTcpPort = hostTcp;
    cc.serverUdpPort = hostUdp;
    cc.password = hc.password;
    cc.listenAddr = "127.0.0.1";
    cc.tcpListenPorts = {clientTcp};
    cc.udpListenPorts = {clientUdp};

    mslink::Relay host, client;
    host.set_log_sink(&test_log, nullptr);
    client.set_log_sink(&test_log, nullptr);

    std::string err;
    CHECK(host.start_host(hc, err));
    CHECK(client.start_client(cc, err));

    bool up = false;
    for (int i = 0; i < 200 && !up; ++i) {
        up = client.stats().tunnelUp;
        if (!up) sleep_ms(25);
    }
    CHECK(up);
    if (up) {
        SOCKET s = connect_tcp(clientTcp);
        CHECK(mslink::sock_valid(s));
        if (mslink::sock_valid(s)) {
            const long dt = wait_peer_close(s, 5000);
            std::printf("client stream closed by host after %ld ms\n", dt);
            CHECK(dt >= 0);        // 有回包（负值 = 一直没关）
            // 被控端 connect 预算 1.5 秒；这里给 3 秒余量，但仍远小于客户端自己的 8 秒读超时。
            CHECK(dt < 3000);
            ::closesocket(s);
        }
        CHECK(client.stats().tcpStreamsOpened >= 1);
    }

    client.stop();
    host.stop();

    if (failures) {
        std::printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("\nstream-open failure checks passed\n");
    return 0;
}
