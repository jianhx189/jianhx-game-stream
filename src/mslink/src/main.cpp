// =============================================================================
//  mslink/main.cpp —— 2 端口复用通道 CLI
//
//  用法：
//    被控端（游戏主机，与 Sunshine 同机）：
//      mslink.exe host --tcp 14738 --udp 14739 --password xxx
//        可选：--target 127.0.0.1 --map-tcp 47989=47989,48010=48010
//              --map-udp 47998=47998
//    远控端（与 Moonlight 同机）：
//      mslink.exe client --server 1.2.3.4 --tcp 14738 --udp 14739 --password xxx
//        Moonlight 里添加主机时填 127.0.0.1 即可（本程序已在 127.0.0.1 监听标准端口）
//
//    自检（不依赖任何外部程序，验证隧道链路）：
//      mslink.exe selftest
// =============================================================================
#include "mslink/relay.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

using namespace mslink;

static uint64_t nowMs() { return uint64_t(::GetTickCount64()); }

static void sleep_ms(int ms) { ::Sleep(DWORD(ms)); }

#define STEP(...) do { fprintf(stderr, "[STEP] " __VA_ARGS__); fprintf(stderr, "\n"); fflush(stderr); } while (0)

static void split_kv(const std::string& s, std::map<uint16_t, uint16_t>& out) {
    // "47989=47989,48010=48051"
    size_t pos = 0;
    while (pos < s.size()) {
        size_t comma = s.find(',', pos);
        std::string item = s.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        size_t eq = item.find('=');
        if (eq != std::string::npos) {
            int a = atoi(item.substr(0, eq).c_str());
            int b = atoi(item.substr(eq + 1).c_str());
            if (a > 0 && a < 65536 && b > 0 && b < 65536)
                out[uint16_t(a)] = uint16_t(b);
        }
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
}

static std::vector<uint16_t> split_ports(const std::string& s) {
    std::vector<uint16_t> v;
    size_t pos = 0;
    while (pos < s.size()) {
        size_t comma = s.find(',', pos);
        int p = atoi(s.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos).c_str());
        if (p > 0 && p < 65536) v.push_back(uint16_t(p));
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return v;
}

static void usage() {
    printf(
        "mslink —— 把 Moonlight/Sunshine 的 6 条流压进 1 TCP + 1 UDP\n\n"
        "  mslink host   --tcp <端口> --udp <端口> --password <口令> [--target <Sunshine地址>]\n"
        "                [--map-tcp a=b,c=d] [--map-udp a=b,c=d] [--bind 0.0.0.0]\n"
        "  mslink client --server <地址> --tcp <端口> --udp <端口> --password <口令>\n"
        "                [--listen 127.0.0.1] [--tcp-ports 47989,47984,48010]\n"
        "                [--udp-ports 47998,47999,48000]\n"
        "  mslink selftest\n");
}

// =============================================================================
//  自检：本机起一个假的 Sunshine（TCP/UDP 回显），跑通 客户端 -> 隧道 -> 被控端 -> 回显
// =============================================================================
namespace selftest {

static std::atomic<bool> gStop{false};

// 假 Sunshine：TCP 回显
static void echo_tcp_server(uint16_t port, std::atomic<int>& okCount) {
    SOCKET ls = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ls == INVALID_SOCKET) return;
    int yes = 1;
    ::setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (::bind(ls, (sockaddr*)&a, sizeof(a)) != 0 || ::listen(ls, 4) != 0) { ::closesocket(ls); return; }
    while (!gStop.load()) {
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(ls, &rf);
        timeval tv{ 0, 200000 };
        if (::select(0, &rf, nullptr, nullptr, &tv) <= 0) continue;
        SOCKET c = ::accept(ls, nullptr, nullptr);
        if (c == INVALID_SOCKET) continue;
        char buf[4096];
        int n = ::recv(c, buf, sizeof(buf), 0);
        if (n > 0) {
            ::send(c, buf, n, 0);
            ++okCount;
        }
        ::closesocket(c);
    }
    ::closesocket(ls);
}

// 假 Sunshine：UDP 回显
static void echo_udp_server(uint16_t port, std::atomic<int>& okCount) {
    SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (::bind(s, (sockaddr*)&a, sizeof(a)) != 0) { ::closesocket(s); return; }
    while (!gStop.load()) {
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(s, &rf);
        timeval tv{ 0, 200000 };
        if (::select(0, &rf, nullptr, nullptr, &tv) <= 0) continue;
        char buf[4096];
        sockaddr_in from{};
        int fl = sizeof(from);
        int n = ::recvfrom(s, buf, sizeof(buf), 0, (sockaddr*)&from, &fl);
        if (n > 0) {
            ::sendto(s, buf, n, 0, (sockaddr*)&from, fl);
            ++okCount;
        }
    }
    ::closesocket(s);
}

static bool wait_true(const std::function<bool()>& fn, int timeoutMs) {
    const uint64_t deadline = nowMs() + uint64_t(timeoutMs);
    while (nowMs() < deadline) {
        if (fn()) return true;
        sleep_ms(20);
    }
    return false;
}

int run() {
    printf("=== mslink 自检：本机回环验证 2 端口隧道 ===\n");

    // 端口规划（避开真实 Sunshine 的 4798x，避免已在跑 Sunshine 的机器上冲突）
    const uint16_t kFakeTcp   = 19001;   // 假 Sunshine TCP 回显
    const uint16_t kFakeUdp   = 19002;   // 假 Sunshine UDP 回显
    const uint16_t kTunnelTcp = 19010;   // 隧道 TCP（模拟 FRP 映射端口）
    const uint16_t kTunnelUdp = 19011;   // 隧道 UDP
    const uint16_t kLocalTcp  = 19020;   // 客户端本机监听（Moonlight 会连这个）
    const uint16_t kLocalUdp  = 19021;

    WSADATA wsa{};
    if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { printf("WSAStartup 失败\n"); return 1; }

    std::atomic<int> tcpEchoOk{0}, udpEchoOk{0};
    std::thread tcpSrv(echo_tcp_server, kFakeTcp, std::ref(tcpEchoOk));
    std::thread udpSrv(echo_udp_server, kFakeUdp, std::ref(udpEchoOk));
    sleep_ms(150);

    // 被控端复用器
    Relay hostRelay;
    HostConfig hc;
    hc.listenTcpPort = kTunnelTcp;
    hc.listenUdpPort = kTunnelUdp;
    hc.password = "selftest-pw";
    hc.targetHost = "127.0.0.1";
    hc.tcpPortMap[kLocalTcp] = kFakeTcp;
    hc.udpPortMap[kLocalUdp] = kFakeUdp;
    std::string err;
    STEP("host relay starting");
    if (!hostRelay.start_host(hc, err)) {
        printf("[FAIL] 被控端复用器启动失败: %s\n", err.c_str());
        gStop.store(true);
        tcpSrv.join(); udpSrv.join();
        return 1;
    }

    // 客户端复用器
    Relay clientRelay;
    ClientConfig cc;
    cc.serverHost = "127.0.0.1";
    cc.serverTcpPort = kTunnelTcp;
    cc.serverUdpPort = kTunnelUdp;
    cc.password = "selftest-pw";
    cc.listenAddr = "127.0.0.1";
    cc.tcpListenPorts = { kLocalTcp };
    cc.udpListenPorts = { kLocalUdp };
    STEP("client relay starting");
    if (!clientRelay.start_client(cc, err)) {
        printf("[FAIL] 客户端复用器启动失败: %s\n", err.c_str());
        hostRelay.stop();
        gStop.store(true);
        tcpSrv.join(); udpSrv.join();
        return 1;
    }

    STEP("waiting tunnel");
    const bool tunnelUp = wait_true([&] {
        return clientRelay.stats().tunnelUp && hostRelay.stats().tunnelUp;
    }, 5000);
    printf("%s 隧道建立：client=%d host=%d\n", tunnelUp ? "[ OK ]" : "[FAIL]",
           int(clientRelay.stats().tunnelUp), int(hostRelay.stats().tunnelUp));

    STEP("tcp roundtrip");
    // ---- TCP 往返 ----
    bool tcpPass = false;
    {
        SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(kLocalTcp);
        ::inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
        DWORD tmo = 3000;
        ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof(tmo));
        if (::connect(s, (sockaddr*)&a, sizeof(a)) == 0) {
            const char* msg = "PING-TCP-THROUGH-TUNNEL";
            ::send(s, msg, int(strlen(msg)), 0);
            char buf[128]{};
            int n = ::recv(s, buf, sizeof(buf), 0);
            tcpPass = (n == int(strlen(msg))) && (memcmp(buf, msg, strlen(msg)) == 0);
            printf("%s TCP 往返（%d 字节）：%s\n", tcpPass ? "[ OK ]" : "[FAIL]",
                   int(strlen(msg)), tcpPass ? "回显一致" : "回显不一致/超时");
        } else {
            printf("[FAIL] TCP 连接本机监听端口失败\n");
        }
        ::closesocket(s);
    }

    STEP("udp roundtrip");
    // ---- 并发 TCP：模拟 Moonlight 同时开 HTTP/HTTPS/RTSP 三条连接 ----
    {
        const int kConns = 3;
        std::vector<SOCKET> socks;
        bool allOk = true;
        for (int i = 0; i < kConns; ++i) {
            SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            sockaddr_in a{};
            a.sin_family = AF_INET;
            a.sin_port = htons(kLocalTcp);
            ::inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
            DWORD tmo = 3000;
            ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof(tmo));
            if (::connect(s, (sockaddr*)&a, sizeof(a)) != 0) { allOk = false; ::closesocket(s); continue; }
            char msg[64];
            int mlen = snprintf(msg, sizeof(msg), "CONCURRENT-STREAM-%d", i);
            ::send(s, msg, mlen, 0);
            socks.push_back(s);
        }
        for (size_t i = 0; i < socks.size(); ++i) {
            char msg[64];
            int mlen = snprintf(msg, sizeof(msg), "CONCURRENT-STREAM-%zu", i);
            char buf[128]{};
            int n = ::recv(socks[i], buf, sizeof(buf), 0);
            if (n != mlen || memcmp(buf, msg, size_t(mlen)) != 0) allOk = false;
            ::closesocket(socks[i]);
        }
        printf("%s 并发 TCP %d 条连接：%s\n", allOk ? "[ OK ]" : "[FAIL]",
               int(socks.size()), allOk ? "各自回显正确、无串流" : "有连接回显错乱");
        if (!allOk) tcpPass = false;
    }

    // ---- UDP 往返 ----
    bool udpPass = false;
    {
        SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        DWORD tmo = 3000;
        ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof(tmo));
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(kLocalUdp);
        ::inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
        const char* msg = "PING-UDP-THROUGH-TUNNEL";
        ::sendto(s, msg, int(strlen(msg)), 0, (sockaddr*)&a, sizeof(a));
        char buf[128]{};
        int n = ::recv(s, buf, sizeof(buf), 0);
        udpPass = (n == int(strlen(msg))) && (memcmp(buf, msg, strlen(msg)) == 0);
        printf("%s UDP 往返（%d 字节）：%s\n", udpPass ? "[ OK ]" : "[FAIL]",
               int(strlen(msg)), udpPass ? "回显一致" : "回显不一致/超时");
        ::closesocket(s);
    }

    // ---- 统计 ----
    sleep_ms(200);
    const RelayStats cs = clientRelay.stats();
    const RelayStats hs = hostRelay.stats();
    printf("\n统计  客户端: tcp流 %llu | tcp上行 %llu B 下行 %llu B | udp 上 %llu 下 %llu | 丢 %llu\n",
           (unsigned long long)cs.tcpStreamsOpened,
           (unsigned long long)cs.tcpBytesUp, (unsigned long long)cs.tcpBytesDown,
           (unsigned long long)cs.udpPacketsUp, (unsigned long long)cs.udpPacketsDown,
           (unsigned long long)cs.udpDropped);
    printf("      被控端: tcp上行 %llu B 下行 %llu B | udp 上 %llu 下 %llu\n",
           (unsigned long long)hs.tcpBytesUp, (unsigned long long)hs.tcpBytesDown,
           (unsigned long long)hs.udpPacketsUp, (unsigned long long)hs.udpPacketsDown);

    STEP("stopping client relay");
    clientRelay.stop();
    STEP("stopping host relay");
    hostRelay.stop();
    STEP("relays stopped");
    gStop.store(true);
    tcpSrv.join();
    udpSrv.join();
    ::WSACleanup();

    const bool ok = tunnelUp && tcpPass && udpPass;
    printf("\n=== 自检结果：%s ===\n", ok ? "全部通过" : "存在失败项");
    return ok ? 0 : 2;
}

} // namespace selftest

// =============================================================================
//  main
// =============================================================================
int main(int argc, char** argv) {
    if (argc < 2) { usage(); return 1; }
    const std::string mode = argv[1];

    if (mode == "selftest") return selftest::run();
    if (mode == "-h" || mode == "--help") { usage(); return 0; }

    std::map<std::string, std::string> opt;
    for (int i = 2; i < argc; ++i) {
        std::string k = argv[i];
        if (k.size() > 2 && k[0] == '-' && k[1] == '-') {
            k = k.substr(2);
            std::string v;
            if (i + 1 < argc && argv[i + 1][0] != '-') v = argv[++i];
            opt[k] = v;
        }
    }
    auto get = [&](const char* k, const char* def = "") -> std::string {
        auto it = opt.find(k);
        return it == opt.end() ? std::string(def) : it->second;
    };
    auto geti = [&](const char* k) -> int { return atoi(get(k).c_str()); };

    const std::string password = get("password");
    if (password.empty()) {
        printf("必须提供 --password（与被控端一致）\n\n");
        usage();
        return 1;
    }

    Relay relay;
    std::string err;

    if (mode == "host") {
        HostConfig hc;
        hc.listenTcpPort = uint16_t(geti("tcp"));
        hc.listenUdpPort = uint16_t(geti("udp"));
        hc.password = password;
        hc.bindAddr = get("bind", "0.0.0.0");
        hc.targetHost = get("target", "127.0.0.1");
        split_kv(get("map-tcp"), hc.tcpPortMap);
        split_kv(get("map-udp"), hc.udpPortMap);
        if (!hc.listenTcpPort || !hc.listenUdpPort) {
            printf("必须提供 --tcp 和 --udp 端口\n");
            return 1;
        }
        if (!relay.start_host(hc, err)) {
            printf("启动失败: %s\n", err.c_str());
            return 1;
        }
        printf("被控端复用器运行中（Ctrl+C 退出）\n");
    } else if (mode == "client") {
        ClientConfig cc;
        cc.serverHost = get("server");
        cc.serverTcpPort = uint16_t(geti("tcp"));
        cc.serverUdpPort = uint16_t(geti("udp"));
        cc.password = password;
        cc.listenAddr = get("listen", "127.0.0.1");
        const std::string tp = get("tcp-ports");
        const std::string up = get("udp-ports");
        if (!tp.empty()) cc.tcpListenPorts = split_ports(tp);
        if (!up.empty()) cc.udpListenPorts = split_ports(up);
        if (cc.serverHost.empty() || !cc.serverTcpPort || !cc.serverUdpPort) {
            printf("必须提供 --server / --tcp / --udp\n");
            return 1;
        }
        if (!relay.start_client(cc, err)) {
            printf("启动失败: %s\n", err.c_str());
            return 1;
        }
        printf("客户端复用器运行中：Moonlight 里添加主机填 %s（Ctrl+C 退出）\n",
               cc.listenAddr.c_str());
    } else {
        printf("未知模式: %s\n\n", mode.c_str());
        usage();
        return 1;
    }

    // 常驻：每 10 秒打一次统计
    while (relay.running()) {
        sleep_ms(10000);
        const RelayStats s = relay.stats();
        printf("[统计] 隧道 %s | tcp流 %llu | tcp↑%llu ↓%llu B | udp↑%llu ↓%llu | 丢 %llu | 重连 %llu\n",
               s.tunnelUp ? "UP" : "DOWN",
               (unsigned long long)s.tcpStreamsOpened,
               (unsigned long long)s.tcpBytesUp, (unsigned long long)s.tcpBytesDown,
               (unsigned long long)s.udpPacketsUp, (unsigned long long)s.udpPacketsDown,
               (unsigned long long)s.udpDropped,
               (unsigned long long)s.tunnelReconnects);
        fflush(stdout);
    }
    relay.stop();
    return 0;
}
