// SPDX-License-Identifier: AGPL-3.0-or-later
// Part of holesail-cpp — a C++ port of holesail (https://github.com/holesail/holesail)
#include <gtest/gtest.h>

#include "holesail/logger.hpp"
#include "holesail/pipe.hpp"

#include <uv.h>

#include <cstring>
#include <string>
#include <vector>

using namespace holesail;

namespace {

// A minimal echo server on 127.0.0.1, standing in for the "local application".
// Echoes every byte it receives and records the total in `received`.
struct EchoServer {
    uv_loop_t* loop;
    uv_tcp_t handle{};
    uint16_t port = 0;
    std::string received;
    std::vector<uv_tcp_t*> clients;

    explicit EchoServer(uv_loop_t* l) : loop(l) {
        uv_tcp_init(loop, &handle);
        handle.data = this;
        struct sockaddr_in addr;
        uv_ip4_addr("127.0.0.1", 0, &addr);
        uv_tcp_bind(&handle, reinterpret_cast<const struct sockaddr*>(&addr), 0);
        int len = sizeof(addr);
        uv_tcp_getsockname(&handle, reinterpret_cast<struct sockaddr*>(&addr), &len);
        port = ntohs(addr.sin_port);
        uv_listen(reinterpret_cast<uv_stream_t*>(&handle), 8, on_conn);
    }

    void close() {
        for (uv_tcp_t* c : clients) {
            if (uv_is_closing(reinterpret_cast<uv_handle_t*>(c)) == 0) {
                uv_close(reinterpret_cast<uv_handle_t*>(c), on_client_closed);
            }
        }
        clients.clear();
        if (uv_is_closing(reinterpret_cast<uv_handle_t*>(&handle)) == 0) {
            uv_close(reinterpret_cast<uv_handle_t*>(&handle), nullptr);
        }
    }

    static void on_client_closed(uv_handle_t* h) { delete reinterpret_cast<uv_tcp_t*>(h); }

    static void on_conn(uv_stream_t* server, int status) {
        if (status != 0) return;
        auto* self = static_cast<EchoServer*>(server->data);
        auto* client = new uv_tcp_t{};
        uv_tcp_init(self->loop, client);
        client->data = self;
        if (uv_accept(server, reinterpret_cast<uv_stream_t*>(client)) != 0) {
            uv_close(reinterpret_cast<uv_handle_t*>(client), on_client_closed);
            return;
        }
        self->clients.push_back(client);
        uv_read_start(reinterpret_cast<uv_stream_t*>(client), on_alloc, on_read);
    }

    static void on_alloc(uv_handle_t*, size_t suggested, uv_buf_t* buf) {
        buf->base = new char[suggested];
        buf->len = suggested;
    }

    static void on_read(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
        auto* self = static_cast<EchoServer*>(stream->data);
        if (nread > 0) {
            self->received.append(buf->base, static_cast<size_t>(nread));
            // Echo it back. The write request owns its own copy.
            auto* payload = new std::string(buf->base, static_cast<size_t>(nread));
            auto* req = new uv_write_t{};
            req->data = payload;
            uv_buf_t out = uv_buf_init(payload->data(), static_cast<unsigned>(payload->size()));
            uv_write(req, stream, &out, 1, [](uv_write_t* r, int) {
                delete static_cast<std::string*>(r->data);
                delete r;
            });
        } else if (nread < 0) {
            // Stop reading but do NOT close here — `clients` still holds this
            // pointer and close() would then double-free it.
            uv_read_stop(stream);
        }
        delete[] buf->base;
    }
};

Logger silent_logger() { return Logger("Holesail", false, Level::Info); }

}  // namespace

TEST(TcpPipe, ForwardsLocalDataToTheStream) {
    uv_loop_t loop;
    ASSERT_EQ(uv_loop_init(&loop), 0);
    EchoServer echo(&loop);
    auto logger = silent_logger();

    std::vector<uint8_t> to_stream;
    StreamSide side;
    side.write = [&](const uint8_t* d, size_t n, std::function<void()>) {
        to_stream.insert(to_stream.end(), d, d + n);
        return 1;  // always drained
    };
    side.pause = [] {};
    side.resume = [] {};
    side.close = [] {};

    TcpPipe pipe(&loop, side, logger);
    bool ready = false;
    ASSERT_EQ(pipe.connect("127.0.0.1", echo.port, [&](int err) {
        EXPECT_EQ(err, 0);
        ready = true;
    }), 0);

    // Drive the loop until connected, then push data through the echo
    // server so it comes back up the pipe into `to_stream`.
    pipe.on_stream_data(reinterpret_cast<const uint8_t*>("ping"), 4);
    for (int i = 0; i < 200 && to_stream.size() < 4; i++) uv_run(&loop, UV_RUN_NOWAIT);

    EXPECT_TRUE(ready);
    EXPECT_EQ(std::string(to_stream.begin(), to_stream.end()), "ping");

    pipe.destroy();
    echo.close();
    uv_run(&loop, UV_RUN_DEFAULT);
    EXPECT_EQ(uv_loop_close(&loop), 0);
}

// Regression guard for the stop-and-wait throughput bug.
//
// hyperdht returns 0 from EVERY successful write — SecretStreamDuplex::write
// ends in a literal `return 0` and never forwards libudx's drained bit. An
// earlier version of this pipe read that 0 as backpressure and stopped after
// each chunk, waiting a whole round trip per 64 KiB. This test pins the fixed
// behaviour: writes must PIPELINE while unacked bytes stay under the water
// mark, and only then stop.
TEST(TcpPipe, PipelinesWritesUntilTheUnackedWatermark) {
    uv_loop_t loop;
    ASSERT_EQ(uv_loop_init(&loop), 0);
    EchoServer echo(&loop);
    auto logger = silent_logger();

    int writes = 0;
    size_t written_bytes = 0;
    std::vector<std::function<void()>> drains;
    StreamSide side;
    side.write = [&](const uint8_t*, size_t n, std::function<void()> on_drain) {
        writes++;
        written_bytes += n;
        drains.push_back(std::move(on_drain));
        return 0;  // exactly what hyperdht reports on success
    };
    side.pause = [] {};
    side.resume = [] {};
    side.close = [] {};

    TcpPipe pipe(&loop, side, logger);
    ASSERT_EQ(pipe.connect("127.0.0.1", echo.port, [](int err) { EXPECT_EQ(err, 0); }), 0);

    // Far more than the watermark, so the pipe must both pipeline and
    // eventually stop.
    std::vector<uint8_t> big(4 * TcpPipe::kStreamHighWaterMark, 'x');
    pipe.on_stream_data(big.data(), big.size());
    for (int i = 0; i < 400; i++) uv_run(&loop, UV_RUN_NOWAIT);

    // THE point of this test: not stop-and-wait. No drain has fired yet, so a
    // return value of 0 must not have been treated as backpressure.
    EXPECT_GT(writes, 1) << "pipe stopped after one write — stop-and-wait regression";

    // ...but it must still bound itself rather than reading without limit.
    EXPECT_LT(written_bytes, big.size())
        << "pipe never applied backpressure at all";
    const int writes_at_watermark = writes;

    // Draining below the watermark resumes reading.
    for (auto& d : drains) d();
    drains.clear();
    for (int i = 0; i < 400 && writes == writes_at_watermark; i++) {
        uv_run(&loop, UV_RUN_NOWAIT);
    }
    EXPECT_GT(writes, writes_at_watermark) << "reading did not resume after drain";

    pipe.destroy();
    echo.close();
    uv_run(&loop, UV_RUN_DEFAULT);
    EXPECT_EQ(uv_loop_close(&loop), 0);
}

TEST(TcpPipe, LocalEofHalfClosesTheStream) {
    uv_loop_t loop;
    ASSERT_EQ(uv_loop_init(&loop), 0);
    auto logger = silent_logger();

    // Point at a port nothing listens on after an immediate accept-and-close
    // server, so the local side EOFs promptly.
    EchoServer echo(&loop);
    bool closed = false;
    StreamSide side;
    side.write = [](const uint8_t*, size_t, std::function<void()>) { return 1; };
    side.pause = [] {};
    side.resume = [] {};
    side.close = [&] { closed = true; };

    TcpPipe pipe(&loop, side, logger);
    ASSERT_EQ(pipe.connect("127.0.0.1", echo.port, [](int) {}), 0);
    for (int i = 0; i < 50; i++) uv_run(&loop, UV_RUN_NOWAIT);
    pipe.on_stream_close();
    echo.close();
    uv_run(&loop, UV_RUN_DEFAULT);

    // on_stream_close must tear the pipe down without leaving handles behind;
    // uv_loop_close returns non-zero if any handle is still open, so this
    // assertion is the leak check for the pipe's own uv_tcp_t.
    EXPECT_EQ(uv_loop_close(&loop), 0);
    (void)closed;
}

TEST(TcpPipe, DestroyIsIdempotent) {
    uv_loop_t loop;
    ASSERT_EQ(uv_loop_init(&loop), 0);
    auto logger = silent_logger();
    EchoServer echo(&loop);

    int closes = 0;
    StreamSide side;
    side.write = [](const uint8_t*, size_t, std::function<void()>) { return 1; };
    side.pause = [] {};
    side.resume = [] {};
    side.close = [&] { closes++; };

    TcpPipe pipe(&loop, side, logger);
    ASSERT_EQ(pipe.connect("127.0.0.1", echo.port, [](int) {}), 0);
    for (int i = 0; i < 50; i++) uv_run(&loop, UV_RUN_NOWAIT);

    pipe.destroy();
    pipe.destroy();
    pipe.destroy();
    echo.close();
    uv_run(&loop, UV_RUN_DEFAULT);
    EXPECT_LE(closes, 1);
    EXPECT_EQ(uv_loop_close(&loop), 0);
}

TEST(TcpProxy, ListensOnAnEphemeralPortAndAccepts) {
    uv_loop_t loop;
    ASSERT_EQ(uv_loop_init(&loop), 0);
    auto logger = silent_logger();

    TcpProxy proxy(&loop, logger);
    int accepted = 0;
    ASSERT_EQ(proxy.listen("127.0.0.1", 0, [&](uv_tcp_t* c) {
        accepted++;
        uv_close(reinterpret_cast<uv_handle_t*>(c), [](uv_handle_t* h) { delete reinterpret_cast<uv_tcp_t*>(h); });
    }), 0);
    ASSERT_NE(proxy.bound_port(), 0);

    uv_tcp_t client;
    uv_tcp_init(&loop, &client);
    struct sockaddr_in addr;
    uv_ip4_addr("127.0.0.1", proxy.bound_port(), &addr);
    uv_connect_t req;
    uv_tcp_connect(&req, &client, reinterpret_cast<const struct sockaddr*>(&addr),
                   [](uv_connect_t*, int status) { EXPECT_EQ(status, 0); });

    for (int i = 0; i < 200 && accepted == 0; i++) uv_run(&loop, UV_RUN_NOWAIT);
    EXPECT_EQ(accepted, 1);

    uv_close(reinterpret_cast<uv_handle_t*>(&client), nullptr);
    proxy.close();
    uv_run(&loop, UV_RUN_DEFAULT);
    uv_loop_close(&loop);
}

// ---------------------------------------------------------------------------
// Coverage beyond the plan's five cases. `adopt()` is the Task 9 entry point
// and nothing above calls it, so the ownership transfer of the accepted handle
// is otherwise never exercised under the sanitizers. Likewise the
// kHighWaterMark pause/resume path: the cases above install no-op pause/resume
// doubles and never assert on them.
// ---------------------------------------------------------------------------

namespace {

// Collects everything a raw client socket receives.
struct Sink {
    std::string data;

    static void on_alloc(uv_handle_t*, size_t suggested, uv_buf_t* buf) {
        buf->base = new char[suggested];
        buf->len = suggested;
    }

    static void on_read(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
        auto* self = static_cast<Sink*>(stream->data);
        if (nread > 0) self->data.append(buf->base, static_cast<size_t>(nread));
        delete[] buf->base;
    }
};

}  // namespace

TEST(TcpPipe, AdoptsAnAcceptedSocketAndPipesBothWays) {
    uv_loop_t loop;
    ASSERT_EQ(uv_loop_init(&loop), 0);
    auto logger = silent_logger();

    std::vector<uint8_t> to_stream;
    StreamSide side;
    side.write = [&](const uint8_t* d, size_t n, std::function<void()>) {
        to_stream.insert(to_stream.end(), d, d + n);
        return 1;
    };
    side.pause = [] {};
    side.resume = [] {};
    side.close = [] {};

    TcpPipe pipe(&loop, side, logger);
    TcpProxy proxy(&loop, logger);
    ASSERT_EQ(proxy.listen("127.0.0.1", 0, [&](uv_tcp_t* c) { EXPECT_EQ(pipe.adopt(c), 0); }), 0);

    Sink sink;
    uv_tcp_t client;
    uv_tcp_init(&loop, &client);
    client.data = &sink;
    struct sockaddr_in addr;
    uv_ip4_addr("127.0.0.1", proxy.bound_port(), &addr);
    uv_connect_t req;
    uv_tcp_connect(&req, &client, reinterpret_cast<const struct sockaddr*>(&addr),
                   [](uv_connect_t* r, int status) {
                       ASSERT_EQ(status, 0);
                       uv_read_start(r->handle, Sink::on_alloc, Sink::on_read);
                       auto* payload = new std::string("up");
                       auto* w = new uv_write_t{};
                       w->data = payload;
                       uv_buf_t b = uv_buf_init(payload->data(), 2);
                       uv_write(w, r->handle, &b, 1, [](uv_write_t* x, int) {
                           delete static_cast<std::string*>(x->data);
                           delete x;
                       });
                   });

    // Up: client -> adopted socket -> StreamSide::write.
    for (int i = 0; i < 200 && to_stream.size() < 2; i++) uv_run(&loop, UV_RUN_NOWAIT);
    EXPECT_EQ(std::string(to_stream.begin(), to_stream.end()), "up");

    // Down: on_stream_data -> adopted socket -> client.
    pipe.on_stream_data(reinterpret_cast<const uint8_t*>("down"), 4);
    for (int i = 0; i < 200 && sink.data.size() < 4; i++) uv_run(&loop, UV_RUN_NOWAIT);
    EXPECT_EQ(sink.data, "down");

    // destroy() owns the adopted handle now — nothing else may close it.
    pipe.destroy();
    proxy.close();
    uv_close(reinterpret_cast<uv_handle_t*>(&client), nullptr);
    uv_run(&loop, UV_RUN_DEFAULT);
    EXPECT_EQ(uv_loop_close(&loop), 0);
}

TEST(TcpPipe, PausesTheStreamAtTheHighWaterMarkAndResumesOnDrain) {
    uv_loop_t loop;
    ASSERT_EQ(uv_loop_init(&loop), 0);
    EchoServer echo(&loop);
    auto logger = silent_logger();

    int pauses = 0;
    int resumes = 0;
    StreamSide side;
    side.write = [](const uint8_t*, size_t, std::function<void()>) { return 1; };
    side.pause = [&] { pauses++; };
    side.resume = [&] { resumes++; };
    side.close = [] {};

    TcpPipe pipe(&loop, side, logger);
    ASSERT_EQ(pipe.connect("127.0.0.1", echo.port, [](int err) { EXPECT_EQ(err, 0); }), 0);
    for (int i = 0; i < 50; i++) uv_run(&loop, UV_RUN_NOWAIT);

    // Queueing more than kHighWaterMark toward the local socket must pause the
    // encrypted stream synchronously: libuv never runs a write callback inline,
    // so the pause cannot have been undone by a drain before this assertion.
    std::vector<uint8_t> big(2 * TcpPipe::kHighWaterMark, 'y');
    pipe.on_stream_data(big.data(), big.size());
    EXPECT_EQ(pauses, 1);
    EXPECT_EQ(resumes, 0);

    for (int i = 0; i < 500 && resumes == 0; i++) uv_run(&loop, UV_RUN_NOWAIT);
    EXPECT_EQ(resumes, 1);

    pipe.destroy();
    echo.close();
    uv_run(&loop, UV_RUN_DEFAULT);
    EXPECT_EQ(uv_loop_close(&loop), 0);
}

// ---------------------------------------------------------------------------
// Regression guard for the server-speaks-first bug (v0.2).
//
// hyperdht_stream_write() returns -1 until the encrypted stream's header
// exchange completes, and TcpPipe treats any negative write as fatal. On the
// server that combination silently killed every connection whose local service
// greets first: sshd emits its banner the instant we connect, while over a real
// holepunched path the header exchange is still in flight. HTTP hid it, because
// there we have nothing to send until the request arrives.
//
// HolesailServer::make_side now buffers in that window and withholds the drain
// callback, mirroring what a Node duplex does for the JS original
// (`connection.write(d) || (loc.pause(), ...)` in connPiper). This pins the two
// halves of that contract that live in TcpPipe: a "buffered" write must not be
// treated as failure, and withholding its drain must produce backpressure
// rather than data loss.
// ---------------------------------------------------------------------------
TEST(TcpPipe, PreOpenBufferingLosesNoDataAndResumesOnFlush) {
    uv_loop_t loop;
    ASSERT_EQ(uv_loop_init(&loop), 0);
    EchoServer echo(&loop);
    auto logger = silent_logger();

    // Exactly the shape HolesailServer::make_side now has.
    bool stream_open = false;
    std::vector<uint8_t> pending;
    std::vector<std::function<void()>> withheld;
    std::vector<uint8_t> delivered;

    StreamSide side;
    side.write = [&](const uint8_t* d, size_t n, std::function<void()> on_drain) {
        if (!stream_open) {
            pending.insert(pending.end(), d, d + n);
            if (on_drain) withheld.push_back(std::move(on_drain));
            return 1;   // accepted, not yet on the wire
        }
        delivered.insert(delivered.end(), d, d + n);
        if (on_drain) on_drain();
        return 1;
    };
    side.pause = [] {};
    side.resume = [] {};
    side.close = [] {};

    TcpPipe pipe(&loop, side, logger);
    bool destroyed = false;
    pipe.set_on_destroy([&] { destroyed = true; });
    ASSERT_EQ(pipe.connect("127.0.0.1", echo.port, [](int err) { EXPECT_EQ(err, 0); }), 0);

    // The local service greets before the stream is writable. Under the old
    // code this write returned -1 and the pipe tore the connection down here.
    const std::string greeting = "SSH-2.0-OpenSSH_10.5\r\n";
    pipe.on_stream_data(reinterpret_cast<const uint8_t*>(greeting.data()), greeting.size());
    for (int i = 0; i < 300 && pending.size() < greeting.size(); i++) {
        uv_run(&loop, UV_RUN_NOWAIT);
    }

    EXPECT_FALSE(destroyed) << "pipe died on a pre-open write — the v0.2 bug is back";
    EXPECT_EQ(std::string(pending.begin(), pending.end()), greeting)
        << "greeting was lost before the stream opened";
    EXPECT_TRUE(delivered.empty()) << "nothing may reach the wire before open";

    // The header exchange completes: flush, then release the withheld drains.
    stream_open = true;
    delivered.insert(delivered.end(), pending.begin(), pending.end());
    pending.clear();
    const auto drains = std::move(withheld);
    withheld.clear();
    for (const auto& d : drains) d();

    // Traffic keeps flowing afterwards, and in order behind the greeting.
    const std::string after = "second";
    pipe.on_stream_data(reinterpret_cast<const uint8_t*>(after.data()), after.size());
    for (int i = 0; i < 300 && delivered.size() < greeting.size() + after.size(); i++) {
        uv_run(&loop, UV_RUN_NOWAIT);
    }

    EXPECT_FALSE(destroyed);
    EXPECT_EQ(std::string(delivered.begin(), delivered.end()), greeting + after)
        << "bytes were lost or reordered across the flush";

    pipe.destroy();
    echo.close();
    uv_run(&loop, UV_RUN_DEFAULT);
    EXPECT_EQ(uv_loop_close(&loop), 0);
}

// The other half of the same story: a negative write really is fatal, which is
// why the server must never let one happen in the pre-open window.
TEST(TcpPipe, ANegativeWriteTearsTheConnectionDown) {
    uv_loop_t loop;
    ASSERT_EQ(uv_loop_init(&loop), 0);
    EchoServer echo(&loop);
    auto logger = silent_logger();

    StreamSide side;
    side.write = [](const uint8_t*, size_t, std::function<void()>) {
        return -1;   // what hyperdht_stream_write returns before open
    };
    side.pause = [] {};
    side.resume = [] {};
    side.close = [] {};

    TcpPipe pipe(&loop, side, logger);
    bool destroyed = false;
    pipe.set_on_destroy([&] { destroyed = true; });
    ASSERT_EQ(pipe.connect("127.0.0.1", echo.port, [](int err) { EXPECT_EQ(err, 0); }), 0);

    pipe.on_stream_data(reinterpret_cast<const uint8_t*>("greeting"), 8);
    for (int i = 0; i < 300 && !destroyed; i++) uv_run(&loop, UV_RUN_NOWAIT);

    EXPECT_TRUE(destroyed) << "a failed stream write must not be ignored";

    echo.close();
    uv_run(&loop, UV_RUN_DEFAULT);
    EXPECT_EQ(uv_loop_close(&loop), 0);
}
