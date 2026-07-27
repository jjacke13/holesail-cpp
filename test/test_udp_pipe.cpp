// SPDX-License-Identifier: AGPL-3.0-or-later
// Part of holesail-cpp — a C++ port of holesail (https://github.com/holesail/holesail)
#include <gtest/gtest.h>

#include "holesail/logger.hpp"
#include "holesail/udp_pipe.hpp"

#include <uv.h>

#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace holesail;

namespace {

std::vector<std::string> collect(FrameDecoder& dec, const std::vector<uint8_t>& chunk,
                                 bool* ok = nullptr) {
    std::vector<std::string> out;
    const bool r = dec.push(chunk.data(), chunk.size(),
                            [&](const uint8_t* d, size_t n) {
                                out.emplace_back(reinterpret_cast<const char*>(d), n);
                            });
    if (ok != nullptr) *ok = r;
    return out;
}

Logger silent_logger() { return Logger("Holesail", false, Level::Info); }

std::vector<uint8_t> framed(const std::string& s) {
    return frame(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

// Every wait is bounded so a broken implementation fails instead of hanging.
template <typename Done>
void pump_until(uv_loop_t* loop, Done done, int max_passes = 500) {
    for (int i = 0; i < max_passes && !done(); i++) uv_run(loop, UV_RUN_NOWAIT);
}

void pump(uv_loop_t* loop, int passes = 50) {
    for (int i = 0; i < passes; i++) uv_run(loop, UV_RUN_NOWAIT);
}

// A loopback UDP endpoint. Records every datagram with the sender's address and
// can reply to whoever spoke last. Stands in for the local application in the
// UdpPipe tests and for a UDP client in the UdpProxy tests.
struct UdpEndpoint {
    uv_loop_t* loop;
    uv_udp_t handle{};
    uint16_t port = 0;
    std::vector<std::string> received;
    struct sockaddr_in last_sender {};
    std::vector<char> scratch = std::vector<char>(kMaxDatagram + 1);

    explicit UdpEndpoint(uv_loop_t* l) : loop(l) {
        uv_udp_init(loop, &handle);
        handle.data = this;
        struct sockaddr_in addr;
        uv_ip4_addr("127.0.0.1", 0, &addr);
        uv_udp_bind(&handle, reinterpret_cast<const struct sockaddr*>(&addr), 0);
        int len = sizeof(addr);
        uv_udp_getsockname(&handle, reinterpret_cast<struct sockaddr*>(&addr), &len);
        port = ntohs(addr.sin_port);
        uv_udp_recv_start(&handle, on_alloc, on_recv);
    }

    void close() {
        if (uv_is_closing(reinterpret_cast<uv_handle_t*>(&handle)) == 0) {
            uv_close(reinterpret_cast<uv_handle_t*>(&handle), nullptr);
        }
    }

    void send_to(uint16_t dst_port, const std::string& payload) {
        struct sockaddr_in dst;
        uv_ip4_addr("127.0.0.1", dst_port, &dst);
        do_send(reinterpret_cast<const struct sockaddr*>(&dst), payload);
    }

    void reply(const std::string& payload) {
        do_send(reinterpret_cast<const struct sockaddr*>(&last_sender), payload);
    }

    // The request owns its copy — libuv does not copy the payload.
    struct SendReq {
        uv_udp_send_t req{};
        std::string data;
    };

    void do_send(const struct sockaddr* dst, const std::string& payload) {
        auto* s = new SendReq{};
        s->data = payload;
        s->req.data = s;
        uv_buf_t buf = uv_buf_init(s->data.data(), static_cast<unsigned>(s->data.size()));
        const int rc = uv_udp_send(&s->req, &handle, &buf, 1, dst,
                                   [](uv_udp_send_t* r, int) {
                                       delete static_cast<SendReq*>(r->data);
                                   });
        if (rc != 0) delete s;
    }

    static void on_alloc(uv_handle_t* h, size_t suggested, uv_buf_t* buf) {
        auto* self = static_cast<UdpEndpoint*>(h->data);
        const size_t n = suggested < self->scratch.size() ? suggested : self->scratch.size();
        buf->base = self->scratch.data();
        buf->len = static_cast<decltype(buf->len)>(n);
    }

    static void on_recv(uv_udp_t* h, ssize_t nread, const uv_buf_t* buf,
                        const struct sockaddr* addr, unsigned) {
        auto* self = static_cast<UdpEndpoint*>(h->data);
        if (self == nullptr || addr == nullptr || nread < 0) return;
        self->received.emplace_back(buf->base, static_cast<size_t>(nread));
        std::memcpy(&self->last_sender, addr, sizeof(struct sockaddr_in));
    }
};

}  // namespace

// ---------------------------------------------------------------------------
// Framing codec
// ---------------------------------------------------------------------------

TEST(Frame, PrefixesBigEndianLength) {
    const auto f = frame(reinterpret_cast<const uint8_t*>("hi"), 2);
    ASSERT_EQ(f.size(), 6u);
    EXPECT_EQ(f[0], 0x00); EXPECT_EQ(f[1], 0x00);
    EXPECT_EQ(f[2], 0x00); EXPECT_EQ(f[3], 0x02);
    EXPECT_EQ(f[4], 'h');  EXPECT_EQ(f[5], 'i');
}

TEST(Frame, ZeroLengthDatagramIsRepresentable) {
    const auto f = frame(nullptr, 0);
    ASSERT_EQ(f.size(), 4u);
    FrameDecoder dec;
    const auto got = collect(dec, f);
    ASSERT_EQ(got.size(), 1u);
    EXPECT_TRUE(got[0].empty());
}

TEST(FrameDecoder, DecodesSeveralFramesInOneChunk) {
    std::vector<uint8_t> buf;
    for (const char* s : {"a", "bb", "ccc"}) {
        const auto f = frame(reinterpret_cast<const uint8_t*>(s), strlen(s));
        buf.insert(buf.end(), f.begin(), f.end());
    }
    FrameDecoder dec;
    const auto got = collect(dec, buf);
    ASSERT_EQ(got.size(), 3u);
    EXPECT_EQ(got[0], "a");
    EXPECT_EQ(got[1], "bb");
    EXPECT_EQ(got[2], "ccc");
}

TEST(FrameDecoder, ReassemblesAFrameSplitAcrossChunks) {
    const auto f = frame(reinterpret_cast<const uint8_t*>("hello world"), 11);
    FrameDecoder dec;
    for (size_t cut = 1; cut < f.size(); cut++) {
        dec.reset();
        const std::vector<uint8_t> a(f.begin(), f.begin() + cut);
        const std::vector<uint8_t> b(f.begin() + cut, f.end());
        auto got = collect(dec, a);
        EXPECT_TRUE(got.empty()) << "premature frame at cut " << cut;
        got = collect(dec, b);
        ASSERT_EQ(got.size(), 1u) << "missing frame at cut " << cut;
        EXPECT_EQ(got[0], "hello world");
    }
}

TEST(FrameDecoder, HandlesALengthHeaderSplitByteByByte) {
    const auto f = frame(reinterpret_cast<const uint8_t*>("x"), 1);
    FrameDecoder dec;
    std::vector<std::string> got;
    for (size_t i = 0; i < f.size(); i++) {
        dec.push(&f[i], 1, [&](const uint8_t* d, size_t n) {
            got.emplace_back(reinterpret_cast<const char*>(d), n);
        });
    }
    ASSERT_EQ(got.size(), 1u);
    EXPECT_EQ(got[0], "x");
}

TEST(FrameDecoder, RejectsAnOversizedLengthHeader) {
    // 0xFFFFFFFF would be a 4 GiB allocation in the JS implementation.
    const std::vector<uint8_t> hostile = {0xff, 0xff, 0xff, 0xff};
    FrameDecoder dec;
    bool ok = true;
    const auto got = collect(dec, hostile, &ok);
    EXPECT_FALSE(ok);
    EXPECT_TRUE(got.empty());
    EXPECT_LE(dec.buffered(), 4u) << "must not buffer toward the claimed length";
}

TEST(FrameDecoder, AcceptsTheLargestLegalDatagram) {
    const std::vector<uint8_t> payload(kMaxDatagram, 'z');
    const auto f = frame(payload.data(), payload.size());
    FrameDecoder dec;
    bool ok = false;
    const auto got = collect(dec, f, &ok);
    EXPECT_TRUE(ok);
    ASSERT_EQ(got.size(), 1u);
    EXPECT_EQ(got[0].size(), kMaxDatagram);
}

TEST(FrameDecoder, RejectsOneByteOverTheCapBeforeTheBodyArrives) {
    // The header claims 65536 bytes and only the header is delivered. The
    // refusal must not wait for the body.
    const std::vector<uint8_t> chunk = {0x00, 0x01, 0x00, 0x00};
    FrameDecoder dec;
    bool ok = true;
    const auto got = collect(dec, chunk, &ok);
    EXPECT_FALSE(ok);
    EXPECT_TRUE(got.empty());
    EXPECT_LE(dec.buffered(), 4u);
}

// ---------------------------------------------------------------------------
// UdpPipe — one local UDP socket bridged to one stream
// ---------------------------------------------------------------------------

TEST(UdpPipe, BridgesBothDirections) {
    uv_loop_t loop;
    ASSERT_EQ(uv_loop_init(&loop), 0);
    auto logger = silent_logger();
    UdpEndpoint app(&loop);  // stands in for the local application

    std::vector<uint8_t> to_stream;
    StreamSide side;
    side.write = [&](const uint8_t* d, size_t n, std::function<void()>) {
        to_stream.insert(to_stream.end(), d, d + n);
        return 1;
    };
    side.pause = [] {};
    side.resume = [] {};
    side.close = [] {};

    UdpPipe pipe(&loop, side, logger);
    ASSERT_EQ(pipe.start("127.0.0.1", app.port), 0);

    // stream -> local: the frame is stripped and the payload delivered as one
    // datagram to the configured local endpoint.
    const auto in = framed("ping");
    pipe.on_stream_data(in.data(), in.size());
    pump_until(&loop, [&] { return !app.received.empty(); });
    ASSERT_EQ(app.received.size(), 1u);
    EXPECT_EQ(app.received[0], "ping");

    // local -> stream: the reply comes back length-prefixed.
    app.reply("pong!");
    pump_until(&loop, [&] { return to_stream.size() >= 9; });
    EXPECT_EQ(to_stream, framed("pong!"));

    pipe.destroy();
    app.close();
    uv_run(&loop, UV_RUN_DEFAULT);
    EXPECT_EQ(uv_loop_close(&loop), 0);
}

TEST(UdpPipe, DeliversEveryFrameOfACoalescedChunk) {
    uv_loop_t loop;
    ASSERT_EQ(uv_loop_init(&loop), 0);
    auto logger = silent_logger();
    UdpEndpoint app(&loop);

    StreamSide side;
    side.write = [](const uint8_t*, size_t, std::function<void()>) { return 1; };
    side.pause = [] {};
    side.resume = [] {};
    side.close = [] {};

    UdpPipe pipe(&loop, side, logger);
    ASSERT_EQ(pipe.start("127.0.0.1", app.port), 0);

    std::vector<uint8_t> chunk;
    for (const char* s : {"one", "two", "three"}) {
        const auto f = framed(s);
        chunk.insert(chunk.end(), f.begin(), f.end());
    }
    pipe.on_stream_data(chunk.data(), chunk.size());
    pump_until(&loop, [&] { return app.received.size() >= 3; });

    ASSERT_EQ(app.received.size(), 3u);
    EXPECT_EQ(app.received[0], "one");
    EXPECT_EQ(app.received[1], "two");
    EXPECT_EQ(app.received[2], "three");

    pipe.destroy();
    app.close();
    uv_run(&loop, UV_RUN_DEFAULT);
    EXPECT_EQ(uv_loop_close(&loop), 0);
}

TEST(UdpPipe, AnOversizedLengthHeaderDestroysTheStream) {
    uv_loop_t loop;
    ASSERT_EQ(uv_loop_init(&loop), 0);
    auto logger = silent_logger();
    UdpEndpoint app(&loop);

    int closes = 0;
    StreamSide side;
    side.write = [](const uint8_t*, size_t, std::function<void()>) { return 1; };
    side.pause = [] {};
    side.resume = [] {};
    side.close = [&] { closes++; };

    UdpPipe pipe(&loop, side, logger);
    bool destroyed = false;
    pipe.set_on_destroy([&] { destroyed = true; });
    ASSERT_EQ(pipe.start("127.0.0.1", app.port), 0);

    const std::vector<uint8_t> hostile = {0xff, 0xff, 0xff, 0xff};
    pipe.on_stream_data(hostile.data(), hostile.size());
    pump(&loop);

    EXPECT_EQ(closes, 1);
    EXPECT_TRUE(destroyed);
    EXPECT_TRUE(app.received.empty());

    pipe.destroy();  // idempotent
    EXPECT_EQ(closes, 1);
    app.close();
    uv_run(&loop, UV_RUN_DEFAULT);
    EXPECT_EQ(uv_loop_close(&loop), 0);
}

TEST(UdpPipe, StreamCloseTearsDownTheLocalSocket) {
    uv_loop_t loop;
    ASSERT_EQ(uv_loop_init(&loop), 0);
    auto logger = silent_logger();
    UdpEndpoint app(&loop);

    StreamSide side;
    side.write = [](const uint8_t*, size_t, std::function<void()>) { return 1; };
    side.pause = [] {};
    side.resume = [] {};
    side.close = [] {};

    UdpPipe pipe(&loop, side, logger);
    ASSERT_EQ(pipe.start("127.0.0.1", app.port), 0);
    pump(&loop, 10);
    pipe.on_stream_close();
    app.close();
    uv_run(&loop, UV_RUN_DEFAULT);

    // uv_loop_close is non-zero while any handle is still open, so this is the
    // leak check for the pipe's own uv_udp_t.
    EXPECT_EQ(uv_loop_close(&loop), 0);
}

// ---------------------------------------------------------------------------
// UdpProxy — one bound socket, one stream per source address
// ---------------------------------------------------------------------------

TEST(UdpProxy, OpensOneStreamPerSourceAddressAndRoutesRepliesBack) {
    uv_loop_t loop;
    ASSERT_EQ(uv_loop_init(&loop), 0);
    auto logger = silent_logger();

    // Declared before the proxy so they outlive the StreamSide doubles.
    std::vector<std::string> opened;
    std::map<std::string, std::vector<uint8_t>> to_stream;

    UdpProxy proxy(&loop, logger);
    ASSERT_EQ(proxy.listen("127.0.0.1", 0, [&](const std::string& id) {
        opened.push_back(id);
        StreamSide s;
        s.write = [&to_stream, id](const uint8_t* d, size_t n, std::function<void()>) {
            auto& v = to_stream[id];
            v.insert(v.end(), d, d + n);
            return 1;
        };
        s.pause = [] {};
        s.resume = [] {};
        s.close = [] {};
        return s;
    }), 0);
    ASSERT_NE(proxy.bound_port(), 0);

    UdpEndpoint a(&loop);
    UdpEndpoint b(&loop);
    a.send_to(proxy.bound_port(), "from-a");
    b.send_to(proxy.bound_port(), "from-b");
    pump_until(&loop, [&] { return opened.size() >= 2; });

    const std::string id_a = "127.0.0.1:" + std::to_string(a.port);
    const std::string id_b = "127.0.0.1:" + std::to_string(b.port);

    ASSERT_EQ(opened.size(), 2u) << "two source addresses must open two streams";
    ASSERT_EQ(to_stream.count(id_a), 1u);
    ASSERT_EQ(to_stream.count(id_b), 1u);
    EXPECT_EQ(to_stream[id_a], framed("from-a"));
    EXPECT_EQ(to_stream[id_b], framed("from-b"));

    // A second datagram from the same source reuses the existing stream.
    a.send_to(proxy.bound_port(), "again");
    pump_until(&loop, [&] { return to_stream[id_a].size() > framed("from-a").size(); });
    EXPECT_EQ(opened.size(), 2u) << "an established client must not re-open a stream";

    // Each response goes back to its own sender.
    const auto ra = framed("resp-a");
    const auto rb = framed("resp-b");
    proxy.on_stream_data(id_a, ra.data(), ra.size());
    proxy.on_stream_data(id_b, rb.data(), rb.size());
    pump_until(&loop, [&] { return !a.received.empty() && !b.received.empty(); });

    ASSERT_EQ(a.received.size(), 1u);
    ASSERT_EQ(b.received.size(), 1u);
    EXPECT_EQ(a.received[0], "resp-a");
    EXPECT_EQ(b.received[0], "resp-b");

    proxy.close();
    a.close();
    b.close();
    uv_run(&loop, UV_RUN_DEFAULT);
    EXPECT_EQ(uv_loop_close(&loop), 0);
}

TEST(UdpProxy, AnOversizedLengthHeaderDestroysOnlyThatClientsStream) {
    uv_loop_t loop;
    ASSERT_EQ(uv_loop_init(&loop), 0);
    auto logger = silent_logger();

    std::vector<std::string> opened;
    std::vector<std::string> closed;

    UdpProxy proxy(&loop, logger);
    ASSERT_EQ(proxy.listen("127.0.0.1", 0, [&](const std::string& id) {
        opened.push_back(id);
        StreamSide s;
        s.write = [](const uint8_t*, size_t, std::function<void()>) { return 1; };
        s.pause = [] {};
        s.resume = [] {};
        s.close = [&closed, id] { closed.push_back(id); };
        return s;
    }), 0);

    UdpEndpoint a(&loop);
    UdpEndpoint b(&loop);
    a.send_to(proxy.bound_port(), "hi");
    b.send_to(proxy.bound_port(), "hi");
    pump_until(&loop, [&] { return opened.size() >= 2; });
    ASSERT_EQ(opened.size(), 2u);

    const std::string id_a = "127.0.0.1:" + std::to_string(a.port);
    const std::string id_b = "127.0.0.1:" + std::to_string(b.port);

    const std::vector<uint8_t> hostile = {0xff, 0xff, 0xff, 0xff};
    proxy.on_stream_data(id_a, hostile.data(), hostile.size());
    ASSERT_EQ(closed.size(), 1u);
    EXPECT_EQ(closed[0], id_a);

    // a is gone: anything further for it is dropped. b is untouched.
    const auto late = framed("late");
    const auto good = framed("good");
    proxy.on_stream_data(id_a, late.data(), late.size());
    proxy.on_stream_data(id_b, good.data(), good.size());
    pump_until(&loop, [&] { return !b.received.empty(); });
    pump(&loop);

    EXPECT_TRUE(a.received.empty());
    ASSERT_EQ(b.received.size(), 1u);
    EXPECT_EQ(b.received[0], "good");
    EXPECT_EQ(closed.size(), 1u);

    proxy.close();
    a.close();
    b.close();
    uv_run(&loop, UV_RUN_DEFAULT);
    EXPECT_EQ(uv_loop_close(&loop), 0);
}

TEST(UdpProxy, StreamCloseDropsTheClient) {
    uv_loop_t loop;
    ASSERT_EQ(uv_loop_init(&loop), 0);
    auto logger = silent_logger();

    std::vector<std::string> opened;

    UdpProxy proxy(&loop, logger);
    ASSERT_EQ(proxy.listen("127.0.0.1", 0, [&](const std::string& id) {
        opened.push_back(id);
        StreamSide s;
        s.write = [](const uint8_t*, size_t, std::function<void()>) { return 1; };
        s.pause = [] {};
        s.resume = [] {};
        s.close = [] {};
        return s;
    }), 0);

    UdpEndpoint a(&loop);
    a.send_to(proxy.bound_port(), "hi");
    pump_until(&loop, [&] { return !opened.empty(); });
    ASSERT_EQ(opened.size(), 1u);

    const std::string id_a = "127.0.0.1:" + std::to_string(a.port);
    proxy.on_stream_close(id_a);

    // Nothing is routed to a dropped client, and the next datagram from the
    // same source opens a fresh stream.
    const auto late = framed("late");
    proxy.on_stream_data(id_a, late.data(), late.size());
    pump(&loop);
    EXPECT_TRUE(a.received.empty());

    a.send_to(proxy.bound_port(), "hi again");
    pump_until(&loop, [&] { return opened.size() >= 2; });
    EXPECT_EQ(opened.size(), 2u);

    proxy.close();
    a.close();
    uv_run(&loop, UV_RUN_DEFAULT);
    EXPECT_EQ(uv_loop_close(&loop), 0);
}
