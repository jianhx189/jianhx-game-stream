// Integration tests for the mslink tunnel robustness work:
//   - non-blocking host handshake with several slow / invalid peers
//   - authenticated TCP + UDP echo while a slow unauthenticated peer is pending
//   - wrong password, oversized handshake, single authenticated client
//   - fast stop with pending handshakes
//   - client rejects oversized frames during handshake and data phase
// Compile this standalone test instead of relay.cpp (the implementation is included).
#include "../src/relay.cpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>
#include <set>
#include <string>
#include <thread>

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::printf("[FAIL] %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        ++failures; \
    } \
} while (0)

static uint64_t test_now_ms() {
    return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

static void test_log(const char* line, void*) {
    std::fprintf(stderr, "[relay] %s\n", line);
}

static std::set<uint16_t> g_usedPorts;

static uint16_t probe_port(int type, uint16_t wanted) {
    const int proto = type == SOCK_STREAM ? IPPROTO_TCP : IPPROTO_UDP;
    SOCKET s = ::socket(AF_INET, type, proto);
    if (!mslink::sock_valid(s)) return 0;
    int one = 1;
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(wanted);
    uint16_t got = 0;
    if (::bind(s, (sockaddr*)&a, sizeof(a)) == 0) {
        int len = sizeof(a);
        if (::getsockname(s, (sockaddr*)&a, &len) == 0) got = ntohs(a.sin_port);
    }
    ::closesocket(s);
    return got;
}

static uint16_t free_port(int type, uint16_t lo = 0, uint16_t hi = 0) {
    const uint32_t count = lo ? uint32_t(hi - lo + 1) : 1;
    for (uint32_t attempt = 0; attempt < 300; ++attempt) {
        const uint16_t want = lo ? uint16_t(lo + attempt % count) : 0;
        const uint16_t got = probe_port(type, want);
        if (got != 0 && g_usedPorts.insert(got).second) return got;
    }
    return 0;
}

static void set_blocking_socket(SOCKET s) {
    u_long nb = 0;
    ::ioctlsocket(s, FIONBIO, &nb);
}

static SOCKET connect_tcp(uint16_t port, int timeoutMs = 5000) {
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (!mslink::sock_valid(s)) return INVALID_SOCKET;
    DWORD tmo = DWORD(timeoutMs);
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof(tmo));
    ::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tmo, sizeof(tmo));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(s, (sockaddr*)&a, sizeof(a)) != 0) {
        ::closesocket(s);
        return INVALID_SOCKET;
    }
    return s;
}

static bool recv_exact(SOCKET s, void* data, size_t len) {
    uint8_t* p = static_cast<uint8_t*>(data);
    size_t got = 0;
    while (got < len) {
        int n = ::recv(s, (char*)p + got, int(len - got), 0);
        if (n <= 0) return false;
        got += size_t(n);
    }
    return true;
}

static bool recv_frame(SOCKET s, uint16_t& type, uint16_t& sid,
                       std::vector<uint8_t>& payload, int timeoutMs) {
    DWORD tmo = DWORD(timeoutMs);
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof(tmo));
    uint8_t hdr[8];
    if (!recv_exact(s, hdr, sizeof(hdr))) return false;
    const uint32_t len = uint32_t(hdr[0]) | (uint32_t(hdr[1]) << 8) |
                         (uint32_t(hdr[2]) << 16) | (uint32_t(hdr[3]) << 24);
    if (len > mslink::kMaxTcpFrame) return false;
    type = uint16_t(hdr[4] | (hdr[5] << 8));
    sid = uint16_t(hdr[6] | (hdr[7] << 8));
    payload.assign(len, 0);
    return len == 0 || recv_exact(s, payload.data(), len);
}

static bool wait_until(const std::function<bool()>& pred, int timeoutMs) {
    const uint64_t deadline = test_now_ms() + uint64_t(timeoutMs);
    while (test_now_ms() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return pred();
}

// ---------------------------------------------------------------------------
// Local echo targets used by the authenticated host.
// ---------------------------------------------------------------------------
class TcpEcho {
public:
    TcpEcho() {
        listen_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (!mslink::sock_valid(listen_)) return;
        int one = 1;
        ::setsockopt(listen_, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(listen_, (sockaddr*)&a, sizeof(a)) != 0 || ::listen(listen_, 8) != 0) {
            ::closesocket(listen_);
            listen_ = INVALID_SOCKET;
            return;
        }
        int len = sizeof(a);
        ::getsockname(listen_, (sockaddr*)&a, &len);
        port_ = ntohs(a.sin_port);
        mslink::set_nonblocking(listen_);
        thread_ = std::thread([this] { run(); });
    }
    ~TcpEcho() { stop(); }
    uint16_t port() const { return port_; }
    bool valid() const { return port_ != 0; }

    void stop() {
        stop_.store(true);
        if (thread_.joinable()) thread_.join();
        mslink::sock_close(listen_);
    }

private:
    void run() {
        while (!stop_.load()) {
            sockaddr_in from{};
            int fl = sizeof(from);
            SOCKET c = ::accept(listen_, (sockaddr*)&from, &fl);
            if (!mslink::sock_valid(c)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            set_blocking_socket(c);
            DWORD tmo = 200;
            ::setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof(tmo));
            char buf[4096];
            for (;;) {
                if (stop_.load()) break;
                int n = ::recv(c, buf, sizeof(buf), 0);
                if (n <= 0) {
                    if (n < 0 && ::WSAGetLastError() == WSAETIMEDOUT) continue;
                    break;
                }
                if (!mslink::send_all(c, buf, size_t(n))) break;
            }
            ::closesocket(c);
        }
    }

    SOCKET listen_ = INVALID_SOCKET;
    uint16_t port_ = 0;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

class UdpEcho {
public:
    explicit UdpEcho(uint16_t port) : port_(port) {
        sock_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (!mslink::sock_valid(sock_)) return;
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = htons(port_);
        if (::bind(sock_, (sockaddr*)&a, sizeof(a)) != 0) {
            ::closesocket(sock_);
            sock_ = INVALID_SOCKET;
            return;
        }
        DWORD tmo = 100;
        ::setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof(tmo));
        thread_ = std::thread([this] { run(); });
    }
    ~UdpEcho() { stop(); }
    bool valid() const { return mslink::sock_valid(sock_); }
    uint16_t port() const { return port_; }

    void stop() {
        stop_.store(true);
        if (thread_.joinable()) thread_.join();
        mslink::sock_close(sock_);
    }

private:
    void run() {
        uint8_t buf[2048];
        while (!stop_.load()) {
            sockaddr_in from{};
            int fl = sizeof(from);
            int n = ::recvfrom(sock_, (char*)buf, sizeof(buf), 0, (sockaddr*)&from, &fl);
            if (n <= 0) continue;
            ::sendto(sock_, (const char*)buf, n, 0, (sockaddr*)&from, fl);
        }
    }

    SOCKET sock_ = INVALID_SOCKET;
    uint16_t port_ = 0;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

// ---------------------------------------------------------------------------
// Raw peer scenarios against the real host.
// ---------------------------------------------------------------------------
static void test_wrong_password(uint16_t hostTcp) {
    std::printf("[TEST] wrong password is rejected\n");
    SOCKET s = connect_tcp(hostTcp);
    CHECK(mslink::sock_valid(s));
    if (!mslink::sock_valid(s)) return;

    uint16_t type = 0, sid = 0;
    std::vector<uint8_t> pl;
    CHECK(recv_frame(s, type, sid, pl, 3000));
    CHECK(type == mslink::kTcpHello && pl.size() == 4);
    if (pl.size() < 4) { ::closesocket(s); return; }

    const uint32_t nonce = uint32_t(pl[0]) | (uint32_t(pl[1]) << 8) |
                           (uint32_t(pl[2]) << 16) | (uint32_t(pl[3]) << 24);
    uint8_t token[16];
    mslink::make_token("definitely-wrong", nonce, token);
    CHECK(mslink::send_tcp_frame(s, mslink::kTcpHelloAck, 0, token, sizeof(token)));

    bool ackOk = recv_frame(s, type, sid, pl, 3000);
    CHECK(ackOk && type == mslink::kTcpHelloAck && pl.size() == 1 && pl[0] == 0);
    char extra = 0;
    CHECK(::recv(s, &extra, 1, 0) <= 0);
    ::closesocket(s);
}

static void test_oversized_handshake(uint16_t hostTcp) {
    std::printf("[TEST] oversized handshake frame closes the pending connection\n");
    SOCKET s = connect_tcp(hostTcp);
    CHECK(mslink::sock_valid(s));
    if (!mslink::sock_valid(s)) return;

    uint16_t type = 0, sid = 0;
    std::vector<uint8_t> pl;
    CHECK(recv_frame(s, type, sid, pl, 3000));
    CHECK(type == mslink::kTcpHello);

    const uint32_t tooBig = uint32_t(mslink::kMaxTcpFrame) + 1;
    uint8_t hdr[8] = {
        uint8_t(tooBig & 0xFF), uint8_t((tooBig >> 8) & 0xFF),
        uint8_t((tooBig >> 16) & 0xFF), uint8_t((tooBig >> 24) & 0xFF),
        uint8_t(mslink::kTcpHelloAck & 0xFF), uint8_t((mslink::kTcpHelloAck >> 8) & 0xFF),
        0, 0
    };
    DWORD tmo = 3000;
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof(tmo));
    CHECK(::send(s, (const char*)hdr, sizeof(hdr), 0) == int(sizeof(hdr)));
    char b = 0;
    CHECK(::recv(s, &b, 1, 0) <= 0);
    ::closesocket(s);
}

static SOCKET open_slow_handshake(uint16_t hostTcp) {
    SOCKET s = connect_tcp(hostTcp);
    if (!mslink::sock_valid(s)) return INVALID_SOCKET;
    uint16_t type = 0, sid = 0;
    std::vector<uint8_t> pl;
    if (!recv_frame(s, type, sid, pl, 3000) || type != mslink::kTcpHello) {
        ::closesocket(s);
        return INVALID_SOCKET;
    }
    // Deliberately send no token: the host must keep the event loop free.
    return s;
}

static bool tcp_echo_roundtrip(uint16_t clientTcpPort) {
    SOCKET s = connect_tcp(clientTcpPort);
    if (!mslink::sock_valid(s)) return false;
    const std::string message = "mslink-tcp-echo-42";
    bool ok = mslink::send_all(s, message.data(), message.size());
    std::vector<char> echoed(message.size());
    if (ok) ok = recv_exact(s, echoed.data(), echoed.size());
    ::closesocket(s);
    return ok && std::string(echoed.begin(), echoed.end()) == message;
}

static bool udp_echo_roundtrip(uint16_t clientUdpPort) {
    SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (!mslink::sock_valid(s)) return false;
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::bind(s, (sockaddr*)&local, sizeof(local));
    sockaddr_in target{};
    target.sin_family = AF_INET;
    target.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    target.sin_port = htons(clientUdpPort);

    const std::string message = "mslink-udp-echo-42";
    DWORD tmo = 200;
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof(tmo));
    bool ok = false;
    for (int attempt = 0; attempt < 20 && !ok; ++attempt) {
        if (::sendto(s, message.data(), int(message.size()), 0,
                     (sockaddr*)&target, sizeof(target)) != int(message.size())) break;
        char buf[256];
        int n = ::recv(s, buf, sizeof(buf), 0);
        if (n == int(message.size()) && std::string(buf, size_t(n)) == message) ok = true;
    }
    ::closesocket(s);
    return ok;
}

// ---------------------------------------------------------------------------
// Fake host used to check the client's own oversized-frame handling.
// ---------------------------------------------------------------------------
class FakeHost {
public:
    enum class Mode { BadHandshake, BadData };

    FakeHost(Mode mode, const std::string& password)
        : mode_(mode), password_(password) {
        listen_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (!mslink::sock_valid(listen_)) return;
        int one = 1;
        ::setsockopt(listen_, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(listen_, (sockaddr*)&a, sizeof(a)) != 0 || ::listen(listen_, 4) != 0) {
            ::closesocket(listen_);
            listen_ = INVALID_SOCKET;
            return;
        }
        int len = sizeof(a);
        ::getsockname(listen_, (sockaddr*)&a, &len);
        port_ = ntohs(a.sin_port);
        mslink::set_nonblocking(listen_);
        thread_ = std::thread([this] { run(); });
    }
    ~FakeHost() {
        stop_.store(true);
        if (thread_.joinable()) thread_.join();
        mslink::sock_close(listen_);
    }

    uint16_t port() const { return port_; }
    bool valid() const { return port_ != 0; }
    bool sawClose() const { return sawClose_.load(); }
    uint64_t closeLatencyMs() const { return closeLatencyMs_.load(); }

private:
    void run() {
        SOCKET c = INVALID_SOCKET;
        const uint64_t acceptDeadline = test_now_ms() + 5000;
        while (!stop_.load() && test_now_ms() < acceptDeadline) {
            sockaddr_in from{};
            int fl = sizeof(from);
            c = ::accept(listen_, (sockaddr*)&from, &fl);
            if (mslink::sock_valid(c)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!mslink::sock_valid(c)) return;

        // accept() inherits the non-blocking listener mode on Windows; this
        // test peer deliberately uses blocking recv with timeouts.
        set_blocking_socket(c);
        DWORD tmo = 5000;
        ::setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof(tmo));
        ::setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tmo, sizeof(tmo));

        const uint32_t nonce = 0x13572468u;
        uint8_t nb[4] = { uint8_t(nonce & 0xFF), uint8_t((nonce >> 8) & 0xFF),
                          uint8_t((nonce >> 16) & 0xFF), uint8_t((nonce >> 24) & 0xFF) };
        if (!mslink::send_tcp_frame(c, mslink::kTcpHello, 0, nb, sizeof(nb))) {
            ::closesocket(c);
            return;
        }

        uint16_t type = 0, sid = 0;
        std::vector<uint8_t> pl;
        if (!recv_frame(c, type, sid, pl, 5000) ||
            type != mslink::kTcpHelloAck || pl.size() < 16) {
            ::closesocket(c);
            return;
        }

        if (mode_ == Mode::BadHandshake) {
            send_bad_header(c);
        } else {
            uint8_t good = 1;
            uint8_t key[32];
            mslink::derive_session_key(password_, nonce, key);
            if (!mslink::send_tcp_frame(c, mslink::kTcpHelloAck, 0, &good, 1) ||
                !mslink::send_tcp_frame(c, mslink::kTcpSessionKey, 0, key, 32)) {
                ::closesocket(c);
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            send_bad_header(c);
        }

        const uint64_t start = test_now_ms();
        DWORD readTmo = 2000;
        ::setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char*)&readTmo, sizeof(readTmo));
        char b = 0;
        const int n = ::recv(c, &b, 1, 0);
        if (n <= 0) {
            closeLatencyMs_.store(test_now_ms() - start);
            sawClose_.store(true);
        }
        ::closesocket(c);
    }

    static void send_bad_header(SOCKET c) {
        const uint32_t tooBig = uint32_t(mslink::kMaxTcpFrame) + 1;
        uint8_t hdr[8] = {
            uint8_t(tooBig & 0xFF), uint8_t((tooBig >> 8) & 0xFF),
            uint8_t((tooBig >> 16) & 0xFF), uint8_t((tooBig >> 24) & 0xFF),
            uint8_t(mslink::kTcpStreamData & 0xFF),
            uint8_t((mslink::kTcpStreamData >> 8) & 0xFF), 0, 0
        };
        ::send(c, (const char*)hdr, sizeof(hdr), 0);
    }

    Mode mode_;
    std::string password_;
    SOCKET listen_ = INVALID_SOCKET;
    uint16_t port_ = 0;
    std::atomic<bool> stop_{false};
    std::atomic<bool> sawClose_{false};
    std::atomic<uint64_t> closeLatencyMs_{0};
    std::thread thread_;
};

static void test_host_rejects_bad_data_frame() {
    std::printf("[TEST] host rejects oversized data frame and closes the tunnel\n");
    const uint16_t p1 = free_port(SOCK_STREAM);
    const uint16_t p2 = free_port(SOCK_DGRAM);
    CHECK(p1 && p2);
    if (!p1 || !p2) return;

    mslink::HostConfig hc;
    hc.listenTcpPort = p1;
    hc.listenUdpPort = p2;
    hc.bindAddr = "127.0.0.1";
    hc.targetHost = "127.0.0.1";
    hc.password = "bad-data-pw";
    mslink::Relay host;
    host.set_log_sink(test_log, nullptr);
    std::string err;
    CHECK(host.start_host(hc, err));
    if (!host.running()) return;

    SOCKET s = connect_tcp(p1);
    CHECK(mslink::sock_valid(s));
    if (mslink::sock_valid(s)) {
        uint16_t type = 0, sid = 0;
        std::vector<uint8_t> pl;
        CHECK(recv_frame(s, type, sid, pl, 3000));
        if (type == mslink::kTcpHello && pl.size() == 4) {
            const uint32_t nonce = uint32_t(pl[0]) | (uint32_t(pl[1]) << 8) |
                                   (uint32_t(pl[2]) << 16) | (uint32_t(pl[3]) << 24);
            uint8_t token[16];
            mslink::make_token("bad-data-pw", nonce, token);
            CHECK(mslink::send_tcp_frame(s, mslink::kTcpHelloAck, 0, token, sizeof(token)));
            CHECK(recv_frame(s, type, sid, pl, 3000));
            CHECK(type == mslink::kTcpHelloAck && pl.size() == 1 && pl[0] == 1);
            CHECK(recv_frame(s, type, sid, pl, 3000));
            CHECK(type == mslink::kTcpSessionKey && pl.size() == 32);

            const uint32_t tooBig = uint32_t(mslink::kMaxTcpFrame) + 1;
            uint8_t hdr[8] = {
                uint8_t(tooBig & 0xFF), uint8_t((tooBig >> 8) & 0xFF),
                uint8_t((tooBig >> 16) & 0xFF), uint8_t((tooBig >> 24) & 0xFF),
                uint8_t(mslink::kTcpStreamData & 0xFF),
                uint8_t((mslink::kTcpStreamData >> 8) & 0xFF), 1, 0
            };
            CHECK(mslink::send_all(s, hdr, sizeof(hdr)));
            DWORD tmo = 3000;
            ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof(tmo));
            char b = 0;
            CHECK(::recv(s, &b, 1, 0) <= 0);
        }
        ::closesocket(s);
    }
    host.stop();
}

static void test_client_rejects_bad_frame(FakeHost::Mode mode, const char* label) {
    std::printf("[TEST] client rejects oversized frame during %s\n", label);
    FakeHost fake(mode, "integration-pw");
    CHECK(fake.valid());
    if (!fake.valid()) return;

    const uint16_t listenTcp = free_port(SOCK_STREAM);
    const uint16_t listenUdp = free_port(SOCK_DGRAM);
    const uint16_t fakeUdp = free_port(SOCK_DGRAM);
    CHECK(listenTcp && listenUdp && fakeUdp);
    if (!listenTcp || !listenUdp || !fakeUdp) return;

    mslink::Relay client;
    client.set_log_sink(test_log, nullptr);
    mslink::ClientConfig cc;
    cc.serverHost = "127.0.0.1";
    cc.serverTcpPort = fake.port();
    cc.serverUdpPort = fakeUdp;
    cc.password = "integration-pw";
    cc.listenAddr = "127.0.0.1";
    cc.tcpListenPorts = { listenTcp };
    cc.udpListenPorts = { listenUdp };
    std::string err;
    CHECK(client.start_client(cc, err));
    if (!err.empty()) std::printf("[info] client start: %s\n", err.c_str());

    const bool closed = wait_until([&] { return fake.sawClose(); }, 5000);
    CHECK(closed);
    if (closed) {
        CHECK(fake.closeLatencyMs() < 2000);
        std::printf("[info] %s close latency=%llu ms\n", label,
                    (unsigned long long)fake.closeLatencyMs());
    }
    client.stop();
}

int main() {
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 1;

    const uint16_t hostTcp = free_port(SOCK_STREAM);
    const uint16_t hostUdp = free_port(SOCK_DGRAM);
    const uint16_t clientTcp = free_port(SOCK_STREAM);
    const uint16_t clientUdp = free_port(SOCK_DGRAM);
    // The host's UDP target whitelist accepts 19000-19099 for self-tests.
    // Stay above the fixed ports used by `mslink selftest` (19030-19033) so
    // both tests can also run in parallel.
    const uint16_t echoUdpPort = free_port(SOCK_DGRAM, 19050, 19099);
    CHECK(hostTcp && hostUdp && clientTcp && clientUdp && echoUdpPort);
    if (!(hostTcp && hostUdp && clientTcp && clientUdp && echoUdpPort)) {
        WSACleanup();
        return 1;
    }

    TcpEcho tcpEcho;
    UdpEcho udpEcho(echoUdpPort);
    CHECK(tcpEcho.valid() && udpEcho.valid());
    if (!tcpEcho.valid() || !udpEcho.valid()) {
        WSACleanup();
        return 1;
    }

    mslink::HostConfig hc;
    hc.listenTcpPort = hostTcp;
    hc.listenUdpPort = hostUdp;
    hc.bindAddr = "127.0.0.1";
    hc.targetHost = "127.0.0.1";
    hc.password = "integration-pw";
    hc.tcpPortMap[clientTcp] = tcpEcho.port();
    hc.udpPortMap[clientUdp] = udpEcho.port();

    mslink::Relay host;
    host.set_log_sink(test_log, nullptr);
    std::string err;
    CHECK(host.start_host(hc, err));
    if (!err.empty()) std::printf("[info] host start: %s\n", err.c_str());
    if (!host.running()) {
        WSACleanup();
        return 1;
    }

    test_wrong_password(hostTcp);
    test_oversized_handshake(hostTcp);

    SOCKET slow = open_slow_handshake(hostTcp);
    CHECK(mslink::sock_valid(slow));

    // Authenticated client must connect and carry TCP+UDP while the slow peer
    // above is still sitting in the pending handshake set.
    mslink::Relay client;
    client.set_log_sink(test_log, nullptr);
    mslink::ClientConfig cc;
    cc.serverHost = "127.0.0.1";
    cc.serverTcpPort = hostTcp;
    cc.serverUdpPort = hostUdp;
    cc.password = "integration-pw";
    cc.listenAddr = "127.0.0.1";
    cc.tcpListenPorts = { clientTcp };
    cc.udpListenPorts = { clientUdp };
    err.clear();
    CHECK(client.start_client(cc, err));
    if (!err.empty()) std::printf("[info] client start: %s\n", err.c_str());

    const bool up = wait_until([&] { return client.stats().tunnelUp; }, 8000);
    CHECK(up);
    const bool udpOk = wait_until([&] { return client.udpDirectOk(); }, 5000);
    CHECK(udpOk);

    if (up && udpOk) {
        CHECK(tcp_echo_roundtrip(clientTcp));
        CHECK(udp_echo_roundtrip(clientUdp));
    }

    // Single authenticated client: a new TCP connection is closed immediately.
    SOCKET extra = connect_tcp(hostTcp);
    CHECK(mslink::sock_valid(extra));
    if (mslink::sock_valid(extra)) {
        DWORD tmo = 2000;
        ::setsockopt(extra, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof(tmo));
        char b = 0;
        CHECK(::recv(extra, &b, 1, 0) <= 0);
        ::closesocket(extra);
    }

    if (mslink::sock_valid(slow)) ::closesocket(slow);
    client.stop();
    host.stop();

    // Fast stop while a handshake is pending: no 5-second blocking wait.
    {
        std::printf("[TEST] stop() wakes and closes pending handshakes\n");
        const uint16_t p1 = free_port(SOCK_STREAM);
        const uint16_t p2 = free_port(SOCK_DGRAM);
        CHECK(p1 && p2);
        if (p1 && p2) {
            mslink::HostConfig hc2;
            hc2.listenTcpPort = p1;
            hc2.listenUdpPort = p2;
            hc2.bindAddr = "127.0.0.1";
            hc2.targetHost = "127.0.0.1";
            hc2.password = "stop-pw";
            mslink::Relay host2;
            host2.set_log_sink(test_log, nullptr);
            std::string e2;
            CHECK(host2.start_host(hc2, e2));
            if (host2.running()) {
                SOCKET pending = open_slow_handshake(p1);
                CHECK(mslink::sock_valid(pending));
                const uint64_t start = test_now_ms();
                host2.stop();
                const uint64_t elapsed = test_now_ms() - start;
                std::printf("[info] stop latency with pending handshake=%llu ms\n",
                            (unsigned long long)elapsed);
                CHECK(elapsed < 2000);
                if (mslink::sock_valid(pending)) {
                    DWORD tmo = 1000;
                    ::setsockopt(pending, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof(tmo));
                    char b = 0;
                    CHECK(::recv(pending, &b, 1, 0) <= 0);
                    ::closesocket(pending);
                }
            }
        }
    }

    test_host_rejects_bad_data_frame();
    test_client_rejects_bad_frame(FakeHost::Mode::BadHandshake, "handshake");
    test_client_rejects_bad_frame(FakeHost::Mode::BadData, "data phase");

    tcpEcho.stop();
    udpEcho.stop();
    WSACleanup();
    if (failures == 0) std::printf("[ OK ] handshake/transfer robustness tests passed\n");
    return failures == 0 ? 0 : 1;
}
