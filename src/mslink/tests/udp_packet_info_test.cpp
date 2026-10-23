// Compile this standalone test instead of relay.cpp (the implementation is included).
// Optional arguments are pairs: <destination-local-ip> <client-local-ip>.
// Cross two different local NIC addresses to reproduce connected-UDP source filtering.
#include "../src/relay.cpp"

struct TestSockets {
    SOCKET host = INVALID_SOCKET, client = INVALID_SOCKET;
    ~TestSockets() { mslink::sock_close(host); mslink::sock_close(client); }
};

static bool check_reply_source(const char* destination, const char* source) {
    printf("[TEST] destination=%s client_source=%s\n", destination, source);
    TestSockets sockets;
    sockets.host = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockets.client = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (!mslink::sock_valid(sockets.host) || !mslink::sock_valid(sockets.client)) return false;
    DWORD timeout = 400;
    setsockopt(sockets.client, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<char*>(&timeout), sizeof(timeout));
    setsockopt(sockets.host, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<char*>(&timeout), sizeof(timeout));
    sockaddr_in host{}, client{}, target{};
    host.sin_family = client.sin_family = target.sin_family = AF_INET;
    if (inet_pton(AF_INET, destination, &target.sin_addr) != 1 ||
        inet_pton(AF_INET, source, &client.sin_addr) != 1 ||
        bind(sockets.host, reinterpret_cast<sockaddr*>(&host), sizeof(host)) != 0 ||
        bind(sockets.client, reinterpret_cast<sockaddr*>(&client), sizeof(client)) != 0) return false;
    int length = sizeof(host);
    if (getsockname(sockets.host, reinterpret_cast<sockaddr*>(&host), &length) != 0) return false;
    target.sin_port = host.sin_port;
    if (connect(sockets.client, reinterpret_cast<sockaddr*>(&target), sizeof(target)) != 0) return false;
    mslink::UdpPacketIo io;
    if (!io.enable(sockets.host)) return false;
    uint8_t key[32]{}, buffer[256];
    int fixedReplies = 0, legacyReplies = 0;
    for (int i = 0; i < 20; ++i) {
        const char payload = char(i);
        if (send(sockets.client, &payload, 1, 0) != 1) return false;
        sockaddr_in from{};
        mslink::UdpReplyRoute route;
        const int incoming = io.recv(sockets.host, buffer, sizeof(buffer), from, route);
        if (incoming != 1 || !route.valid || route.info.ipi_addr.s_addr != target.sin_addr.s_addr) {
            printf("[FAIL] receive/route: bytes=%d valid=%d interface=%lu local=%s WSA=%d\n",
                   incoming, int(route.valid), route.info.ipi_ifindex, inet_ntoa(route.info.ipi_addr), WSAGetLastError());
            return false;
        }
        if (i == 0) {
            // Contrast the old wildcard-sendto path. Some route tables choose the
            // expected address, others reproduce the dropped ACK; either is valid.
            if (!mslink::send_udp_packet(sockets.host, from, mslink::kUdpHelloAck,
                                         0, 0, key, nullptr, 0)) return false;
            if (recv(sockets.client, reinterpret_cast<char*>(buffer), sizeof(buffer), 0) == 20) ++legacyReplies;
        }
        if (!mslink::send_udp_packet(sockets.host, from, mslink::kUdpData, 1,
                                     uint32_t(i+1), key, &payload, 1, &route, io.send)) {
            printf("[FAIL] pinned send WSA=%d\n", WSAGetLastError()); return false;
        }
        const int received = recv(sockets.client, reinterpret_cast<char*>(buffer), sizeof(buffer), 0);
        if (received != 21 || buffer[1] != mslink::kUdpData || buffer[20] != uint8_t(payload)) {
            printf("[FAIL] pinned receive bytes=%d WSA=%d\n", received, WSAGetLastError()); return false;
        }
        ++fixedReplies;
    }
    mslink::UdpReplyRoute missingRoute;
    if (mslink::send_udp_packet(sockets.host, target, mslink::kUdpHelloAck,
                                0, 0, key, nullptr, 0, &missingRoute, io.send)) return false;
    printf("[ OK ] connected UDP source preserved: %d/20; old sendto accepted %d/1; missing route rejected\n",
           fixedReplies, legacyReplies);
    return fixedReplies == 20;
}

int main(int argc, char** argv) {
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) return 1;
    bool passed = argc == 1 ? check_reply_source("127.0.0.1", "127.0.0.1") : argc % 2 == 1;
    for (int i=1; passed && i+1<argc; i+=2) passed=check_reply_source(argv[i],argv[i+1]);
    if (!passed) printf("[FAIL] UDP packet-info source preservation (WSA=%d)\n", WSAGetLastError());
    WSACleanup();
    return passed ? 0 : 1;
}
