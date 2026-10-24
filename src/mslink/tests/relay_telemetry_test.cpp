// White-box/integration checks for the in-memory telemetry history and events.
//   - history keeps at most kTelemetryHistoryMax samples and samples at most once per second
//   - events are bounded, throttled, and cover tunnel/stream/auth/media transitions
//   - telemetry JSON stays small, contains no history, and never leaks passwords or peers
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

struct mslink::RelayTestPeer {
    static mslink::Relay::Impl* impl(mslink::Relay& r) { return r.impl_.get(); }
};

static void test_log(const char* line, void*) {
    std::fprintf(stderr, "[relay] %s\n", line);
}

static uint64_t tick_ms() {
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

static bool has_event(const mslink::Relay::Impl& impl, const char* category,
                      const std::string& text) {
    for (const auto& e : impl.events)
        if (e.category == category && e.message.find(text) != std::string::npos) return true;
    return false;
}

static void test_history_bounds() {
    mslink::Relay relay;
    auto* impl = mslink::RelayTestPeer::impl(relay);
    const uint64_t baseTick = tick_ms();
    const uint64_t baseUnix = 1700000000000ULL;

    for (int i = 0; i < 350; ++i)
        impl->sample_history(baseTick + uint64_t(i) * 1000, baseUnix + uint64_t(i) * 1000);

    CHECK(impl->history.size() == mslink::kTelemetryHistoryMax);
    CHECK(impl->history.front().timeUnixMs == baseUnix + 50 * 1000);
    CHECK(impl->history.back().timeUnixMs == baseUnix + 349 * 1000);
    CHECK(impl->history.back().timeUnixMs - impl->history.front().timeUnixMs ==
          (mslink::kTelemetryHistoryMax - 1) * 1000);
    bool increasing = true;
    for (size_t i = 1; i < impl->history.size(); ++i)
        if (impl->history[i].timeUnixMs <= impl->history[i - 1].timeUnixMs) increasing = false;
    CHECK(increasing);
    for (const auto& s : impl->history) {
        CHECK(!s.tunnelUp);
        CHECK(s.tcpBytesUp == 0);
        CHECK(s.activeTcpStreams == 0);
        CHECK(s.queuedBytes == 0);
    }

    // 同一秒内的额外调用不再新增采样点。
    const size_t before = impl->history.size();
    const uint64_t lastUnix = impl->history.back().timeUnixMs;
    impl->sample_history(baseTick + 349 * 1000 + 400, lastUnix + 400);
    CHECK(impl->history.size() == before);
    CHECK(impl->history.back().timeUnixMs == lastUnix);

    // 一次长时间跳跃只补一个采样点，不会补发一串。
    impl->sample_history(baseTick + 400 * 1000, lastUnix + 51000);
    CHECK(impl->history.size() == before);
    CHECK(impl->history.back().timeUnixMs == lastUnix + 51000);

    mslink::Relay relay2;
    auto* impl2 = mslink::RelayTestPeer::impl(relay2);
    impl2->sample_history(baseTick, baseUnix);
    impl2->sample_history(baseTick + 200, baseUnix + 200);
    CHECK(impl2->history.size() == 1);
    impl2->sample_history(baseTick + 1000, baseUnix + 1000);
    CHECK(impl2->history.size() == 2);
}

static void test_event_bounds_and_throttle() {
    mslink::Relay relay;
    auto* impl = mslink::RelayTestPeer::impl(relay);

    for (int i = 0; i < 150; ++i)
        impl->record_event("test", "info", "事件 " + std::to_string(i));
    CHECK(impl->events.size() == mslink::kTelemetryEventMax);
    CHECK(impl->events.back().message == "事件 149");

    const size_t before = impl->events.size();
    impl->record_event("test", "info", "重复事件");
    const size_t afterFirst = impl->events.size();
    impl->record_event("test", "info", "重复事件");
    impl->record_event("test", "info", "重复事件");
    CHECK(afterFirst == before);             // 已到上限，最旧的被淘汰
    CHECK(impl->events.size() == mslink::kTelemetryEventMax);

    int repeats = 0;
    for (const auto& e : impl->events)
        if (e.message == "重复事件") ++repeats;
    CHECK(repeats == 1);                     // 1 秒内同 category+message 只记一次

    impl->record_event("other", "warn", "重复事件");
    repeats = 0;
    for (const auto& e : impl->events)
        if (e.message == "重复事件") ++repeats;
    CHECK(repeats == 2);                     // category 不同则独立去重

    impl->record_event("test", "warn", std::string(500, 'x'));
    CHECK(impl->events.back().message.size() <= 163);

    impl->record_event("test", "warn", std::string("控制字符\n\t 保留中文"));
    CHECK(impl->events.back().message.find('\n') == std::string::npos);
}

static void test_json_contract() {
    mslink::Relay relay;
    auto* impl = mslink::RelayTestPeer::impl(relay);
    impl->record_event("tunnel", "info", "隧道已建立并通过认证");
    impl->sample_history(tick_ms(), 1700000001000ULL);

    const std::string hist = impl->history_json();
    CHECK(hist.find("\"schema\":1") != std::string::npos);
    CHECK(hist.find("\"events\":[") != std::string::npos);
    CHECK(hist.find("\"history\":[") != std::string::npos);
    CHECK(hist.find("\"device_name\"") == std::string::npos);
    CHECK(hist.find("peer_report") == std::string::npos);
    CHECK(hist.find("\"time_unix_ms\":1700000001000") != std::string::npos);

    const std::string telemetry = impl->telemetry_json();
    CHECK(telemetry.find("\"events\":[") != std::string::npos);
    CHECK(telemetry.find("\"history\"") == std::string::npos);
    CHECK(telemetry.size() < 64 * 1024);
    CHECK(impl->telemetry_json(false).find("\"events\"") == std::string::npos);

    for (int i = 0; i < 200; ++i)
        impl->record_event("bulk", "warn", std::to_string(i) + std::string(300, 'z'));
    CHECK(impl->events.size() == mslink::kTelemetryEventMax);
    CHECK(impl->telemetry_json().size() < 64 * 1024);
    CHECK(impl->history_json().size() < 128 * 1024);

    mslink::publish_media_metrics("{\"frames\":1}");
    impl->sample_history(tick_ms() + 2000, 1700000005000ULL);
    CHECK(has_event(*impl, "media", "媒体会话已开始"));
    impl->sample_history(tick_ms() + 9000, 1700000012000ULL);
    CHECK(has_event(*impl, "media", "媒体会话已结束"));
}

static void test_integration_events_and_secrets() {
    const uint16_t hostTcp = free_port(SOCK_STREAM);
    const uint16_t hostUdp = free_port(SOCK_DGRAM);
    const uint16_t clientTcp = free_port(SOCK_STREAM);
    const uint16_t clientUdp = free_port(SOCK_DGRAM);
    const uint16_t deadPort = free_port(SOCK_STREAM);
    CHECK(hostTcp && hostUdp && clientTcp && clientUdp && deadPort);

    const std::string password = "telemetry-secret-pw-9f3a";
    const std::string deviceName = "TelemetryDeviceNameXYZ";

    mslink::HostConfig hc;
    hc.listenTcpPort = hostTcp;
    hc.listenUdpPort = hostUdp;
    hc.bindAddr = "127.0.0.1";
    hc.targetHost = "127.0.0.1";
    hc.password = password;
    hc.tcpPortMap[clientTcp] = deadPort;

    mslink::ClientConfig cc;
    cc.deviceName = deviceName;
    cc.serverHost = "127.0.0.1";
    cc.serverTcpPort = hostTcp;
    cc.serverUdpPort = hostUdp;
    cc.password = password;
    cc.listenAddr = "127.0.0.1";
    cc.tcpListenPorts = {clientTcp};
    cc.udpListenPorts = {clientUdp};

    mslink::Relay host, client;
    host.set_log_sink(&test_log, nullptr);
    client.set_log_sink(&test_log, nullptr);

    std::string err;
    CHECK(host.start_host(hc, err));
    CHECK(client.start_client(cc, err));
    auto* hostImpl = mslink::RelayTestPeer::impl(host);
    auto* clientImpl = mslink::RelayTestPeer::impl(client);
    CHECK(hostImpl && clientImpl);

    bool up = false;
    for (int i = 0; i < 200 && !up; ++i) {
        up = client.stats().tunnelUp && host.stats().tunnelUp;
        if (!up) sleep_ms(25);
    }
    CHECK(up);
    if (!up) {
        client.stop();
        host.stop();
        return;
    }

    CHECK(has_event(*clientImpl, "tunnel", "隧道已建立"));
    CHECK(has_event(*hostImpl, "tunnel", "隧道已建立"));

    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    CHECK(mslink::sock_valid(s));
    if (mslink::sock_valid(s)) {
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = htons(clientTcp);
        CHECK(::connect(s, (sockaddr*)&a, sizeof(a)) == 0);
        for (int i = 0; i < 80 && !has_event(*hostImpl, "stream", "上游流打开失败"); ++i)
            sleep_ms(25);
        CHECK(has_event(*hostImpl, "stream", "上游流打开失败"));
        ::closesocket(s);
    }

    // 认证拒绝计数变化 -> 下一个 1 秒采样点产生一条聚合事件，而不是每包一条。
    clientImpl->udpAuthRejected.fetch_add(4);
    for (int i = 0; i < 100 && !has_event(*clientImpl, "auth", "UDP 认证拒绝"); ++i)
        sleep_ms(25);
    CHECK(has_event(*clientImpl, "auth", "UDP 认证拒绝"));

    for (int i = 0; i < 80 && clientImpl->history.empty(); ++i) sleep_ms(25);
    CHECK(!clientImpl->history.empty());
    CHECK(clientImpl->history.size() <= mslink::kTelemetryHistoryMax);
    CHECK(clientImpl->events.size() <= mslink::kTelemetryEventMax);
    CHECK(hostImpl->events.size() <= mslink::kTelemetryEventMax);

    const uint64_t nowUnix = mslink::unix_ms();
    if (!clientImpl->history.empty()) {
        CHECK(clientImpl->history.back().timeUnixMs >= nowUnix - 5000);
        CHECK(clientImpl->history.back().timeUnixMs <= nowUnix + 1000);
    }

    const std::string clientHist = client.history_json();
    const std::string hostHist = host.history_json();
    const std::string clientTelemetry = client.telemetry_json();
    for (const std::string* doc : {&clientHist, &hostHist, &clientTelemetry}) {
        CHECK(doc->find(password) == std::string::npos);
        CHECK(doc->find("dpapi:v1") == std::string::npos);
    }
    CHECK(clientHist.find(deviceName) == std::string::npos);
    CHECK(hostHist.find(deviceName) == std::string::npos);
    CHECK(clientHist.find("127.0.0.1") == std::string::npos);
    CHECK(hostHist.find("127.0.0.1") == std::string::npos);
    CHECK(clientTelemetry.size() < 64 * 1024);

    for (const auto& e : clientImpl->events) {
        CHECK(e.message.find(password) == std::string::npos);
        CHECK(e.message.find("127.0.0.1") == std::string::npos);
    }
    for (const auto& e : hostImpl->events) {
        CHECK(e.message.find(password) == std::string::npos);
        CHECK(e.message.find("127.0.0.1") == std::string::npos);
    }

    client.stop();
    host.stop();
    CHECK(has_event(*clientImpl, "tunnel", "隧道已断开"));
}

int main() {
    WSADATA wsa{};
    if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::printf("[FAIL] WSAStartup\n");
        return 1;
    }

    test_history_bounds();
    test_event_bounds_and_throttle();
    test_json_contract();
    test_integration_events_and_secrets();

    if (failures) {
        std::printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("\ntelemetry history/event checks passed\n");
    return 0;
}
