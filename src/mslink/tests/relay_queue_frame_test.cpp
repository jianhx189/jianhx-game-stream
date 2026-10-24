// White-box tests for the bounded send queue and the TCP frame reader.
// Compile this standalone test instead of relay.cpp (the implementation is included).
#include "../src/relay.cpp"

#include <algorithm>
#include <cstdio>
#include <vector>

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::printf("[FAIL] %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        ++failures; \
    } \
} while (0)

static std::vector<uint8_t> make_frame(uint16_t type, uint16_t streamId,
                                       const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> frame(8 + payload.size());
    const uint32_t len = uint32_t(payload.size());
    frame[0] = uint8_t(len & 0xFF);
    frame[1] = uint8_t((len >> 8) & 0xFF);
    frame[2] = uint8_t((len >> 16) & 0xFF);
    frame[3] = uint8_t((len >> 24) & 0xFF);
    frame[4] = uint8_t(type & 0xFF);
    frame[5] = uint8_t((type >> 8) & 0xFF);
    frame[6] = uint8_t(streamId & 0xFF);
    frame[7] = uint8_t((streamId >> 8) & 0xFF);
    std::copy(payload.begin(), payload.end(), frame.begin() + 8);
    return frame;
}

static void append(std::vector<uint8_t>& dst, const std::vector<uint8_t>& src) {
    dst.insert(dst.end(), src.begin(), src.end());
}

static void test_out_queue_capacity() {
    std::printf("[TEST] OutQueue capacity reservation / atomic overflow\n");
    mslink::OutQueue q;
    std::vector<uint8_t> block(1024 * 1024, 0x5A);

    for (int i = 0; i < 4; ++i) CHECK(q.push(block.data(), block.size()));
    CHECK(q.totalBytes == mslink::OutQueue::kMaxQueueBytes);
    CHECK(q.items.size() == 4);

    const size_t itemsBefore = q.items.size();
    const uint8_t one = 1;
    CHECK(!q.push(&one, 1));
    CHECK(q.totalBytes == mslink::OutQueue::kMaxQueueBytes);
    CHECK(q.items.size() == itemsBefore);

    std::vector<uint8_t> oversized(mslink::OutQueue::kMaxQueueBytes + 1, 0);
    CHECK(!q.push(oversized.data(), oversized.size()));
    CHECK(q.totalBytes == mslink::OutQueue::kMaxQueueBytes);
    CHECK(q.items.size() == itemsBefore);

    // A partially sent buffer only reserves/copies the unsent tail.
    mslink::OutQueue tail;
    std::vector<uint8_t> buffer(100, 0x11);
    CHECK(tail.push(buffer.data(), buffer.size(), 40));
    CHECK(tail.totalBytes == 60);
    CHECK(tail.items.front().data.size() == 60);
    CHECK(tail.items.front().data[0] == 0x11);
    CHECK(!tail.push(buffer.data(), buffer.size(), buffer.size() + 1));
    CHECK(tail.totalBytes == 60);
}

static void test_frame_reader_sticky_and_half() {
    std::printf("[TEST] FrameReader sticky frames / half frame\n");
    mslink::FrameReader r;
    const auto ping = make_frame(mslink::kTcpPing, 0, {});
    const std::vector<uint8_t> dataPayload(100, 0xAB);
    const auto data = make_frame(mslink::kTcpStreamData, 7, dataPayload);
    const std::vector<uint8_t> lastPayload = {1, 2, 3, 4, 5, 6};
    const auto last = make_frame(mslink::kTcpStreamData, 8, lastPayload);

    std::vector<uint8_t> firstChunk;
    append(firstChunk, ping);
    append(firstChunk, data);
    const size_t half = last.size() / 2;
    firstChunk.insert(firstChunk.end(), last.begin(), last.begin() + half);

    CHECK(r.feed(firstChunk.data(), firstChunk.size()));

    uint16_t type = 0, sid = 0;
    std::vector<uint8_t> payload;
    CHECK(r.complete());
    CHECK(r.pop(type, sid, payload));
    CHECK(type == mslink::kTcpPing && sid == 0 && payload.empty());

    CHECK(r.complete());
    CHECK(r.pop(type, sid, payload));
    CHECK(type == mslink::kTcpStreamData && sid == 7);
    CHECK(payload == dataPayload);

    CHECK(!r.complete());
    CHECK(!r.bad());
    // Only `half` bytes of the next 8-byte header are available.
    CHECK(r.want() == 8 - half);

    CHECK(r.feed(last.data() + half, last.size() - half));
    CHECK(r.complete());
    CHECK(r.pop(type, sid, payload));
    CHECK(type == mslink::kTcpStreamData && sid == 8);
    CHECK(payload == lastPayload);
    CHECK(!r.bad());
}

static void test_frame_reader_limits() {
    std::printf("[TEST] FrameReader rejects oversized header / accepts exact max\n");
    std::vector<uint8_t> badHeader(8, 0);
    const uint32_t tooBig = uint32_t(mslink::kMaxTcpFrame) + 1;
    badHeader[0] = uint8_t(tooBig & 0xFF);
    badHeader[1] = uint8_t((tooBig >> 8) & 0xFF);
    badHeader[2] = uint8_t((tooBig >> 16) & 0xFF);
    badHeader[3] = uint8_t((tooBig >> 24) & 0xFF);
    badHeader[4] = uint8_t(mslink::kTcpStreamData & 0xFF);
    badHeader[5] = uint8_t((mslink::kTcpStreamData >> 8) & 0xFF);

    mslink::FrameReader bad;
    CHECK(!bad.feed(badHeader.data(), badHeader.size()));
    CHECK(bad.bad());
    CHECK(!bad.complete());
    uint16_t type = 0, sid = 0;
    std::vector<uint8_t> payload;
    CHECK(!bad.pop(type, sid, payload));

    // A valid frame followed by an oversized header in the same recv batch is
    // rejected immediately after the valid frame is consumed.
    mslink::FrameReader trailing;
    std::vector<uint8_t> batch = make_frame(mslink::kTcpPing, 0, {});
    batch.insert(batch.end(), badHeader.begin(), badHeader.end());
    CHECK(trailing.feed(batch.data(), batch.size()));
    CHECK(trailing.complete());
    CHECK(trailing.pop(type, sid, payload));
    CHECK(type == mslink::kTcpPing);
    CHECK(trailing.bad());
    CHECK(!trailing.complete());

    // Exactly kMaxTcpFrame is legal; only the header is available at first.
    std::vector<uint8_t> maxHeader(8, 0);
    const uint32_t maxLen = uint32_t(mslink::kMaxTcpFrame);
    maxHeader[0] = uint8_t(maxLen & 0xFF);
    maxHeader[1] = uint8_t((maxLen >> 8) & 0xFF);
    maxHeader[2] = uint8_t((maxLen >> 16) & 0xFF);
    maxHeader[3] = uint8_t((maxLen >> 24) & 0xFF);
    maxHeader[4] = uint8_t(mslink::kTcpStreamData & 0xFF);

    mslink::FrameReader maxr;
    CHECK(maxr.feed(maxHeader.data(), maxHeader.size()));
    CHECK(!maxr.bad());
    CHECK(!maxr.complete());
    CHECK(maxr.want() == mslink::kMaxTcpFrame);

    std::vector<uint8_t> half(mslink::kMaxTcpFrame / 2, 0x42);
    CHECK(maxr.feed(half.data(), half.size()));
    CHECK(!maxr.complete());
    std::vector<uint8_t> rest(mslink::kMaxTcpFrame - half.size(), 0x43);
    CHECK(maxr.feed(rest.data(), rest.size()));
    CHECK(maxr.complete());
    CHECK(maxr.pop(type, sid, payload));
    CHECK(payload.size() == mslink::kMaxTcpFrame);
    CHECK(payload.front() == 0x42 && payload.back() == 0x43);
    CHECK(!maxr.bad());
}

static void test_frame_send_atomicity() {
    std::printf("[TEST] tunnel frame queue overflow keeps whole frames only\n");
    SOCKET listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    CHECK(mslink::sock_valid(listener));
    if (!mslink::sock_valid(listener)) return;
    int one = 1;
    ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(::bind(listener, (sockaddr*)&a, sizeof(a)) == 0);
    CHECK(::listen(listener, 1) == 0);
    int len = sizeof(a);
    CHECK(::getsockname(listener, (sockaddr*)&a, &len) == 0);

    SOCKET sender = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    CHECK(mslink::sock_valid(sender));
    CHECK(::connect(sender, (sockaddr*)&a, sizeof(a)) == 0);
    sockaddr_in peer{};
    int peerLen = sizeof(peer);
    SOCKET receiver = ::accept(listener, (sockaddr*)&peer, &peerLen);
    CHECK(mslink::sock_valid(receiver));
    if (!mslink::sock_valid(sender) || !mslink::sock_valid(receiver)) {
        mslink::sock_close(sender);
        mslink::sock_close(receiver);
        mslink::sock_close(listener);
        return;
    }

    int small = 4096;
    ::setsockopt(sender, SOL_SOCKET, SO_SNDBUF, (const char*)&small, sizeof(small));
    ::setsockopt(receiver, SOL_SOCKET, SO_RCVBUF, (const char*)&small, sizeof(small));
    CHECK(mslink::set_nonblocking(sender));

    // The receiver never reads, so the kernel send buffer fills and frames
    // start piling up in OutQueue until the 4 MiB cap is reached.
    mslink::OutQueue q;
    std::vector<uint8_t> payload(mslink::kMaxTcpFrame, 0x7E);
    bool overflowed = false;
    for (int i = 0; i < 200; ++i) {
        if (!mslink::send_tcp_frame_queued(sender, q, mslink::kTcpStreamData, 1,
                                           payload.data(), payload.size())) {
            overflowed = true;
            break;
        }
    }
    CHECK(overflowed);
    CHECK(q.totalBytes <= mslink::OutQueue::kMaxQueueBytes);
    for (const auto& item : q.items) {
        CHECK(item.data.size() == 8 + mslink::kMaxTcpFrame);
        const uint32_t frameLen = uint32_t(item.data[0]) | (uint32_t(item.data[1]) << 8) |
                                  (uint32_t(item.data[2]) << 16) | (uint32_t(item.data[3]) << 24);
        CHECK(frameLen == mslink::kMaxTcpFrame);
        CHECK(item.data[4] == uint8_t(mslink::kTcpStreamData & 0xFF));
        CHECK(item.data[5] == uint8_t((mslink::kTcpStreamData >> 8) & 0xFF));
        CHECK(item.data[6] == 1);
        CHECK(item.data[7] == 0);
    }

    mslink::sock_close(sender);
    mslink::sock_close(receiver);
    mslink::sock_close(listener);
}

static void test_sequence_duplicate_latest() {
    std::printf("[TEST] replay window classifies latest-sequence copies as duplicates\n");
    mslink::SequenceWindow w;
    CHECK(!w.duplicate(100));          // not initialized yet
    CHECK(w.accept(100));
    CHECK(w.duplicate(100));           // copy of the newest sequence
    CHECK(!w.duplicate(101));          // newer is not a duplicate
    CHECK(w.accept(101));
    CHECK(w.duplicate(100));           // within the 64-slot window
    CHECK(!w.accept(100));             // still rejected by the accept path
    CHECK(!w.duplicate(37));           // outside the window
    CHECK(!w.duplicate(102));
}

int main() {
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 1;

    test_out_queue_capacity();
    test_frame_reader_sticky_and_half();
    test_frame_reader_limits();
    test_frame_send_atomicity();
    test_sequence_duplicate_latest();

    WSACleanup();
    if (failures == 0) std::printf("[ OK ] queue/frame robustness tests passed\n");
    return failures == 0 ? 0 : 1;
}
