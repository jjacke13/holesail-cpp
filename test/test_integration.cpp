// SPDX-License-Identifier: AGPL-3.0-or-later
// Part of holesail-cpp — a C++ port of holesail (https://github.com/holesail/holesail)
#include <gtest/gtest.h>

#include "holesail/holesail.hpp"

#include <uv.h>

#include <arpa/inet.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <optional>
#include <string>
#include <vector>

using namespace holesail;

// These tests join the public DHT and are therefore slow and network-dependent.
// Gate them behind an environment variable so `ctest` stays fast by default:
//   HOLESAIL_NETWORK_TESTS=1 ctest -R test_integration --output-on-failure
namespace {

bool network_tests_enabled() {
    const char* v = std::getenv("HOLESAIL_NETWORK_TESTS");
    return v != nullptr && std::string(v) == "1";
}

// HOLESAIL_TEST_LOG=0 turns the library's own logging on at DEBUG. The only
// way to see what a tunnel that never came up was actually doing.
int test_log_level() {
    const char* v = std::getenv("HOLESAIL_TEST_LOG");
    return v == nullptr ? -1 : std::atoi(v);
}

// Announce plus holepunch takes tens of seconds on the public DHT, and a client
// that loses that race just gets its connection dropped — so readiness is
// polled by retrying, never slept for.
constexpr uint64_t kDeadlineMs = 60000;   // whole-test budget
constexpr uint64_t kAttemptMs = 12000;    // one probe attempt's patience
constexpr uint64_t kRetryMs = 2000;       // between probe attempts
constexpr uint64_t kResendMs = 2500;      // between UDP resends
constexpr uint64_t kLookupMs = 6000;      // between lookup attempts
constexpr uint64_t kDrainMs = 500;        // teardown turns after close()

// The vector from the plan's Global Constraints.
constexpr const char* kSecureKey = "this-is-a-32-char-holesail-key!!";

// Deterministic pseudo-random bytes. Deterministic so a mismatch is
// reproducible, and seeded per sender so a datagram delivered to the wrong
// socket fails the comparison instead of passing by accident.
std::vector<uint8_t> payload(uint32_t seed, size_t n) {
    std::vector<uint8_t> out(n);
    uint32_t x = (seed * 2654435761u) | 1u;
    for (auto& b : out) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        b = static_cast<uint8_t>(x);
    }
    return out;
}

// uv timer callbacks are plain C function pointers; this is the glue that lets
// a test hand one a lambda. `fn` must outlive the timer.
void every(uv_timer_t* timer, uint64_t repeat, std::function<void()>* fn) {
    timer->data = fn;
    uv_timer_start(
        timer, [](uv_timer_t* h) { (*static_cast<std::function<void()>*>(h->data))(); },
        0, repeat);
}

template <typename H>
void close_handle(H* h) {
    auto* handle = reinterpret_cast<uv_handle_t*>(h);
    if (uv_is_closing(handle) == 0) uv_close(handle, nullptr);
}

// Bind to port 0, read the assignment back, hand it in. The client's local
// proxy has to be told a port up front — `Holesail::info().port` reports what
// was asked for, not what libuv picked, so 0 would leave us nothing to dial.
uint16_t free_port(uv_loop_t* loop, bool udp) {
    struct sockaddr_in addr;
    uv_ip4_addr("127.0.0.1", 0, &addr);
    int len = sizeof(addr);

    // Stack handles are safe: the uv_run below drains the close queue before
    // they go out of scope.
    auto* sa = reinterpret_cast<struct sockaddr*>(&addr);
    uv_tcp_t tcp{};
    uv_udp_t sock{};
    if (udp) {
        uv_udp_init(loop, &sock);
        if (uv_udp_bind(&sock, sa, 0) == 0) uv_udp_getsockname(&sock, sa, &len);
        close_handle(&sock);
    } else {
        uv_tcp_init(loop, &tcp);
        if (uv_tcp_bind(&tcp, sa, 0) == 0) uv_tcp_getsockname(&tcp, sa, &len);
        close_handle(&tcp);
    }
    uv_run(loop, UV_RUN_NOWAIT);
    return ntohs(addr.sin_port);
}

// One loop for everything — the local service, both Holesail ends and the test
// client — plus the deadline that turns a wedged tunnel into a failure instead
// of a hung CI job.
struct TestLoop {
    uv_loop_t loop{};
    uv_timer_t timer{};
    bool timed_out = false;

    TestLoop() {
        uv_loop_init(&loop);
        uv_timer_init(&loop, &timer);
        timer.data = this;
    }

    void run() {
        uv_timer_start(&timer,
                       [](uv_timer_t* t) {
                           static_cast<TestLoop*>(t->data)->timed_out = true;
                           uv_stop(t->loop);
                       },
                       kDeadlineMs, 0);
        uv_run(&loop, UV_RUN_DEFAULT);
    }

    void stop() { uv_stop(&loop); }

    // `Holesail::close()` defers the DHT teardown onto a timer, so the loop has
    // to keep turning afterwards or the handles leak.
    //
    // `uv_loop_close()` is deliberately NOT asserted here: hyperdht leaves one
    // stopped-but-unclosed uv_timer_t behind, so it cannot return 0 once a DHT
    // has existed. That check belongs in the pure-libuv suites.
    void drain() {
        uv_timer_start(&timer, [](uv_timer_t* t) { uv_stop(t->loop); }, kDrainMs, 0);
        uv_run(&loop, UV_RUN_DEFAULT);
        uv_close(reinterpret_cast<uv_handle_t*>(&timer), nullptr);
        uv_run(&loop, UV_RUN_NOWAIT);
        uv_loop_close(&loop);
    }
};

Options server_options(uint16_t port, bool secure, bool udp,
                       std::optional<std::string> key) {
    Options o;
    o.server = true;
    o.host = "127.0.0.1";
    o.port = port;
    o.secure = secure;
    o.udp = udp;
    o.key = std::move(key);
    o.log_level = test_log_level();
    return o;
}

// `secure` is deliberately left unset: the url the server printed decides it.
// `udp` and `host` are pinned so a stale record left on the DHT by an earlier
// run cannot flip the local proxy into the wrong mode.
Options client_options(const std::string& url, uint16_t port, bool udp) {
    Options o;
    o.client = true;
    o.key = url;
    o.host = "127.0.0.1";
    o.port = port;
    o.udp = udp;
    o.log_level = test_log_level();
    return o;
}

// ---------------------------------------------------------------------------
// TCP: the local service, and the test client that probes through the tunnel
// ---------------------------------------------------------------------------

// The "local application" the holesail server fronts. Echoes every byte, and
// answers a half-close with one of its own so EOF travels back down the tunnel.
struct TcpEcho {
    uv_loop_t* loop;
    uv_tcp_t handle{};
    uint16_t port = 0;
    bool saw_eof = false;
    ssize_t last_error = 0;   // what actually ended the read, for the failure message
    size_t received = 0;
    std::function<void()> on_end;
    std::vector<uv_tcp_t*> clients;
    std::vector<char> scratch = std::vector<char>(64 * 1024);

    explicit TcpEcho(uv_loop_t* l) : loop(l) {
        uv_tcp_init(loop, &handle);
        handle.data = this;
        struct sockaddr_in addr;
        uv_ip4_addr("127.0.0.1", 0, &addr);
        if (uv_tcp_bind(&handle, reinterpret_cast<const struct sockaddr*>(&addr), 0) != 0) return;
        int len = sizeof(addr);
        uv_tcp_getsockname(&handle, reinterpret_cast<struct sockaddr*>(&addr), &len);
        port = ntohs(addr.sin_port);
        uv_listen(reinterpret_cast<uv_stream_t*>(&handle), 8, on_conn);
    }

    void close() {
        for (uv_tcp_t* c : clients) {
            if (uv_is_closing(reinterpret_cast<uv_handle_t*>(c)) == 0) {
                uv_close(reinterpret_cast<uv_handle_t*>(c),
                         [](uv_handle_t* h) { delete reinterpret_cast<uv_tcp_t*>(h); });
            }
        }
        clients.clear();
        close_handle(&handle);
    }

    static void on_conn(uv_stream_t* server, int status) {
        if (status != 0) return;
        auto* self = static_cast<TcpEcho*>(server->data);
        auto* client = new uv_tcp_t{};
        uv_tcp_init(self->loop, client);
        client->data = self;
        if (uv_accept(server, reinterpret_cast<uv_stream_t*>(client)) != 0) {
            uv_close(reinterpret_cast<uv_handle_t*>(client),
                     [](uv_handle_t* h) { delete reinterpret_cast<uv_tcp_t*>(h); });
            return;
        }
        self->clients.push_back(client);
        uv_read_start(reinterpret_cast<uv_stream_t*>(client), on_alloc, on_read);
    }

    static void on_alloc(uv_handle_t* h, size_t suggested, uv_buf_t* buf) {
        auto* self = static_cast<TcpEcho*>(h->data);
        const size_t n = suggested < self->scratch.size() ? suggested : self->scratch.size();
        *buf = uv_buf_init(self->scratch.data(), static_cast<unsigned>(n));
    }

    static void on_read(uv_stream_t* s, ssize_t nread, const uv_buf_t* buf) {
        auto* self = static_cast<TcpEcho*>(s->data);
        if (nread > 0) {
            self->received += static_cast<size_t>(nread);
            auto* copy = new std::string(buf->base, static_cast<size_t>(nread));
            auto* req = new uv_write_t{};
            req->data = copy;
            uv_buf_t out = uv_buf_init(copy->data(), static_cast<unsigned>(copy->size()));
            if (uv_write(req, s, &out, 1, [](uv_write_t* r, int) {
                    delete static_cast<std::string*>(r->data);
                    delete r;
                }) != 0) {
                delete copy;
                delete req;
            }
            return;
        }
        if (nread == 0) return;   // EAGAIN

        self->last_error = nread;
        if (nread == UV_EOF) {
            self->saw_eof = true;
            // Answer the half-close, otherwise EOF never makes it back through
            // the tunnel — hyperdht closes a stream only once both ends ended.
            auto* req = new uv_shutdown_t{};
            if (uv_shutdown(req, s, [](uv_shutdown_t* r, int) { delete r; }) != 0) delete req;
        }
        // Never close here: `clients` still holds the pointer and close() would
        // then double-free it.
        uv_read_stop(s);
        if (self->on_end) self->on_end();
    }
};

// One end-to-end attempt through the client's local proxy, retried until the
// tunnel comes up. Each attempt owns its socket and every request riding on it,
// so abandoning one is a single uv_close.
class TcpProbe {
public:
    TcpProbe(uv_loop_t* loop, uint16_t port, std::vector<uint8_t> data,
             std::function<void()> on_success)
        : loop_(loop), port_(port), payload_(std::move(data)),
          on_success_(std::move(on_success)) {
        uv_timer_init(loop_, &retry_);
        retry_.data = this;
    }

    void start() {
        if (done_) return;
        attempts_++;
        got_.clear();
        eof_ = false;
        half_closed_ = false;

        auto* a = new Attempt{};
        a->probe = this;
        uv_tcp_init(loop_, &a->tcp);
        a->tcp.data = a;
        a->req.data = a;
        attempt_ = a;

        struct sockaddr_in addr;
        uv_ip4_addr("127.0.0.1", port_, &addr);
        if (uv_tcp_connect(&a->req, &a->tcp, reinterpret_cast<const struct sockaddr*>(&addr),
                           on_connect) != 0) {
            retry_later();
            return;
        }
        uv_timer_start(&retry_, on_timer, kAttemptMs, 0);
    }

    void close_handles() {
        uv_timer_stop(&retry_);
        uv_close(reinterpret_cast<uv_handle_t*>(&retry_), nullptr);
        abandon();
    }

    bool done() const { return done_; }
    int attempts() const { return attempts_; }
    const std::vector<uint8_t>& got() const { return got_; }
    bool saw_eof() const { return eof_; }

private:
    struct Attempt {
        uv_tcp_t tcp{};
        uv_connect_t req{};
        uv_write_t write{};
        uv_shutdown_t shutdown{};
        TcpProbe* probe = nullptr;   // null once abandoned
        std::vector<char> scratch = std::vector<char>(64 * 1024);
    };

    void abandon() {
        if (attempt_ == nullptr) return;
        Attempt* a = attempt_;
        attempt_ = nullptr;
        a->probe = nullptr;   // in-flight callbacks now find a dead attempt
        if (uv_is_closing(reinterpret_cast<uv_handle_t*>(&a->tcp)) == 0) {
            uv_close(reinterpret_cast<uv_handle_t*>(&a->tcp),
                     [](uv_handle_t* h) { delete static_cast<Attempt*>(h->data); });
        }
    }

    void retry_later() {
        abandon();
        if (!done_) uv_timer_start(&retry_, on_timer, kRetryMs, 0);
    }

    void finish() {
        if (!eof_ || got_.size() < payload_.size()) {
            retry_later();
            return;
        }
        done_ = true;
        uv_timer_stop(&retry_);
        abandon();
        if (on_success_) on_success_();
    }

    static void on_timer(uv_timer_t* t) {
        auto* self = static_cast<TcpProbe*>(t->data);
        self->abandon();
        self->start();
    }

    static void on_connect(uv_connect_t* req, int status) {
        auto* a = static_cast<Attempt*>(req->data);
        if (a->probe == nullptr) return;   // abandoned; this is the UV_ECANCELED
        TcpProbe* self = a->probe;
        if (status != 0) {
            self->retry_later();
            return;
        }
        uv_read_start(reinterpret_cast<uv_stream_t*>(&a->tcp), on_alloc, on_read);

        a->write.data = a;
        uv_buf_t out = uv_buf_init(reinterpret_cast<char*>(self->payload_.data()),
                                   static_cast<unsigned>(self->payload_.size()));
        if (uv_write(&a->write, reinterpret_cast<uv_stream_t*>(&a->tcp), &out, 1,
                     on_write) != 0) {
            self->retry_later();
        }
    }

    static void on_write(uv_write_t* req, int status) {
        auto* a = static_cast<Attempt*>(req->data);
        if (a->probe == nullptr || status == 0) return;
        a->probe->retry_later();
    }

    static void on_alloc(uv_handle_t* h, size_t suggested, uv_buf_t* buf) {
        auto* a = static_cast<Attempt*>(h->data);
        const size_t n = suggested < a->scratch.size() ? suggested : a->scratch.size();
        *buf = uv_buf_init(a->scratch.data(), static_cast<unsigned>(n));
    }

    static void on_read(uv_stream_t* s, ssize_t nread, const uv_buf_t* buf) {
        auto* a = static_cast<Attempt*>(s->data);
        if (a->probe == nullptr) return;
        TcpProbe* self = a->probe;

        if (nread == UV_EOF) {
            self->eof_ = true;
            self->finish();
            return;
        }
        if (nread < 0) {
            self->retry_later();
            return;
        }
        self->got_.insert(self->got_.end(), buf->base, buf->base + nread);

        // Everything came back — half-close, then wait for the EOF to travel
        // the other way.
        if (!self->half_closed_ && self->got_.size() >= self->payload_.size()) {
            self->half_closed_ = true;
            a->shutdown.data = a;
            if (uv_shutdown(&a->shutdown, s, [](uv_shutdown_t*, int) {}) != 0) {
                self->retry_later();
            }
        }
    }

    uv_loop_t* loop_;
    uint16_t port_;
    std::vector<uint8_t> payload_;
    std::function<void()> on_success_;

    uv_timer_t retry_{};
    Attempt* attempt_ = nullptr;
    std::vector<uint8_t> got_;
    int attempts_ = 0;
    bool done_ = false;
    bool eof_ = false;
    bool half_closed_ = false;
};

// A holesail server and a client aimed at whatever url it published, brought up
// in that order on one loop. Every failure on the way stops the loop with the
// error recorded, so a test only has to check the two `_err` fields afterwards.
struct Pair {
    TestLoop& t;
    Holesail server;
    std::optional<Holesail> client;
    std::string url;
    int server_err = -1;
    int client_err = -1;

    Pair(TestLoop& loop, Options server_opts)
        : t(loop), server(&loop.loop, std::move(server_opts)) {}

    int start(uint16_t proxy_port, bool udp, std::function<void()> on_ready) {
        return server.ready([this, proxy_port, udp, on_ready](int err) {
            server_err = err;
            if (err != 0) {
                t.stop();
                return;
            }
            url = server.info().url;
            client.emplace(&t.loop, client_options(url, proxy_port, udp));
            const int rc = client->ready([this, on_ready](int cerr) {
                client_err = cerr;
                if (cerr != 0) {
                    t.stop();
                    return;
                }
                on_ready();
            });
            if (rc != 0) {
                client_err = rc;
                t.stop();
            }
        });
    }

    void close() {
        if (client) client->close();
        server.close();
    }
};

// The body both TCP cases share; `secure` is the only difference. A public
// server is given no key at all, so it invents one per run — which also means
// the public case can never collide with a stale record on the DHT.
void run_tcp_tunnel_case(bool secure) {
    TestLoop t;
    TcpEcho echo(&t.loop);
    const uint16_t proxy_port = free_port(&t.loop, false);
    EXPECT_NE(echo.port, 0);
    EXPECT_NE(proxy_port, 0);

    const auto data = payload(secure ? 1 : 2, 64 * 1024);
    // Two things have to land, and the probe's own EOF is the earlier of them:
    // the local service sees the half-close a loop turn or two later, so
    // stopping on the probe alone would cut the tunnel before it propagates.
    std::function<void()> settled;
    TcpProbe probe(&t.loop, proxy_port, data, [&settled] { settled(); });
    settled = [&t, &probe, &echo] {
        if (probe.done() && echo.saw_eof) t.stop();
    };
    echo.on_end = [&settled] { settled(); };

    Pair pair(t, server_options(echo.port, secure, /*udp=*/false,
                                secure ? std::optional<std::string>(kSecureKey)
                                       : std::nullopt));
    EXPECT_EQ(pair.start(proxy_port, /*udp=*/false, [&probe] { probe.start(); }), 0);

    t.run();

    // Teardown before the assertions: an early return out of a failed ASSERT
    // would leave live handles on a loop that is about to be destroyed.
    probe.close_handles();
    pair.close();
    echo.close();
    t.drain();

    const std::string& url = pair.url;
    const int server_err = pair.server_err;
    const int client_err = pair.client_err;
    EXPECT_FALSE(t.timed_out)
        << "tunnel never delivered in " << kDeadlineMs << " ms; url=" << url
        << " server_err=" << server_err << " client_err=" << client_err
        << " attempts=" << probe.attempts() << " echoed=" << probe.got().size() << "/"
        << data.size();
    EXPECT_EQ(server_err, 0);
    EXPECT_EQ(client_err, 0);
    EXPECT_EQ(url.rfind(secure ? "hs://s000" : "hs://0000", 0), 0u);
    EXPECT_TRUE(probe.done());
    EXPECT_EQ(probe.got(), data);
    EXPECT_TRUE(probe.saw_eof());
    EXPECT_TRUE(echo.saw_eof) << "half-close did not reach the local service; last read was "
                              << echo.last_error;
    EXPECT_GE(echo.received, data.size());
}

// ---------------------------------------------------------------------------
// UDP: an echo responder and two independent senders
// ---------------------------------------------------------------------------

struct UdpEcho {
    uv_loop_t* loop;
    uv_udp_t sock{};
    uint16_t port = 0;
    std::vector<char> scratch = std::vector<char>(kMaxDatagram + 1);

    explicit UdpEcho(uv_loop_t* l) : loop(l) {
        uv_udp_init(loop, &sock);
        sock.data = this;
        struct sockaddr_in addr;
        uv_ip4_addr("127.0.0.1", 0, &addr);
        if (uv_udp_bind(&sock, reinterpret_cast<const struct sockaddr*>(&addr), 0) != 0) return;
        int len = sizeof(addr);
        uv_udp_getsockname(&sock, reinterpret_cast<struct sockaddr*>(&addr), &len);
        port = ntohs(addr.sin_port);
        uv_udp_recv_start(&sock, on_alloc, on_recv);
    }

    void close() { close_handle(&sock); }

    static void on_alloc(uv_handle_t* h, size_t, uv_buf_t* buf) {
        auto* self = static_cast<UdpEcho*>(h->data);
        *buf = uv_buf_init(self->scratch.data(), static_cast<unsigned>(self->scratch.size()));
    }

    static void on_recv(uv_udp_t* h, ssize_t nread, const uv_buf_t* buf,
                        const struct sockaddr* addr, unsigned) {
        if (nread <= 0 || addr == nullptr) return;
        auto* copy = new std::string(buf->base, static_cast<size_t>(nread));
        auto* req = new uv_udp_send_t{};
        req->data = copy;
        uv_buf_t out = uv_buf_init(copy->data(), static_cast<unsigned>(copy->size()));
        if (uv_udp_send(req, h, &out, 1, addr, [](uv_udp_send_t* r, int) {
                delete static_cast<std::string*>(r->data);
                delete r;
            }) != 0) {
            delete copy;
            delete req;
        }
    }
};

// One UDP source port. Its three payloads are seeded from `seed`, so a reply
// that lands on the wrong sender fails the byte comparison — which is the whole
// point of running two of these through one proxy.
struct UdpSender {
    uv_loop_t* loop;
    uv_udp_t sock{};
    struct sockaddr_in target{};
    std::vector<std::vector<uint8_t>> payloads;
    std::vector<bool> got;
    bool mismatch = false;
    std::vector<size_t> seen;   // every reply length, for the failure message
    std::vector<char> scratch = std::vector<char>(kMaxDatagram + 1);
    std::function<void()> on_change;

    UdpSender(uv_loop_t* l, uint16_t proxy_port, uint32_t seed) : loop(l) {
        for (size_t n : {size_t{1}, size_t{1400}, size_t{60000}}) {
            payloads.push_back(payload(seed + static_cast<uint32_t>(n), n));
        }
        got.assign(payloads.size(), false);

        uv_udp_init(loop, &sock);
        sock.data = this;
        struct sockaddr_in any;
        uv_ip4_addr("127.0.0.1", 0, &any);
        uv_udp_bind(&sock, reinterpret_cast<const struct sockaddr*>(&any), 0);
        uv_ip4_addr("127.0.0.1", proxy_port, &target);
        uv_udp_recv_start(&sock, on_alloc, on_recv);
    }

    void close() { close_handle(&sock); }

    bool complete() const {
        for (bool b : got) {
            if (!b) return false;
        }
        return true;
    }

    // Datagrams are allowed to vanish, and so is a connect that raced the
    // announce — resending whatever is still outstanding covers both.
    void send_outstanding() {
        for (size_t i = 0; i < payloads.size(); ++i) {
            if (got[i]) continue;
            auto* req = new uv_udp_send_t{};
            uv_buf_t out = uv_buf_init(reinterpret_cast<char*>(payloads[i].data()),
                                       static_cast<unsigned>(payloads[i].size()));
            if (uv_udp_send(req, &sock, &out, 1,
                            reinterpret_cast<const struct sockaddr*>(&target),
                            [](uv_udp_send_t* r, int) { delete r; }) != 0) {
                delete req;
            }
        }
    }

    static void on_alloc(uv_handle_t* h, size_t, uv_buf_t* buf) {
        auto* self = static_cast<UdpSender*>(h->data);
        *buf = uv_buf_init(self->scratch.data(), static_cast<unsigned>(self->scratch.size()));
    }

    static void on_recv(uv_udp_t* h, ssize_t nread, const uv_buf_t* buf,
                        const struct sockaddr* addr, unsigned) {
        auto* self = static_cast<UdpSender*>(h->data);
        if (nread <= 0 || addr == nullptr) return;
        const auto len = static_cast<size_t>(nread);
        self->seen.push_back(len);

        // Through `const uint8_t*`, never `buf->base`: char is signed here, and
        // comparing it against a uint8_t promotes both to int, so every byte
        // >= 0x80 would compare unequal.
        const auto* bytes = reinterpret_cast<const uint8_t*>(buf->base);
        for (size_t i = 0; i < self->payloads.size(); ++i) {
            if (self->payloads[i].size() != len) continue;
            if (std::equal(self->payloads[i].begin(), self->payloads[i].end(), bytes)) {
                self->got[i] = true;
            } else {
                self->mismatch = true;   // right size, wrong bytes: crossed wires
            }
            if (self->on_change) self->on_change();
            return;
        }
        self->mismatch = true;   // a length nobody here ever sent
        if (self->on_change) self->on_change();
    }
};

}  // namespace

TEST(Integration, TcpTunnelSecureMode) {
    if (!network_tests_enabled()) GTEST_SKIP() << "set HOLESAIL_NETWORK_TESTS=1";
    run_tcp_tunnel_case(/*secure=*/true);
}

TEST(Integration, TcpTunnelPublicMode) {
    if (!network_tests_enabled()) GTEST_SKIP() << "set HOLESAIL_NETWORK_TESTS=1";
    run_tcp_tunnel_case(/*secure=*/false);
}

TEST(Integration, UdpTunnel) {
    if (!network_tests_enabled()) GTEST_SKIP() << "set HOLESAIL_NETWORK_TESTS=1";

    TestLoop t;
    UdpEcho echo(&t.loop);
    const uint16_t proxy_port = free_port(&t.loop, true);
    EXPECT_NE(echo.port, 0);
    EXPECT_NE(proxy_port, 0);

    UdpSender a(&t.loop, proxy_port, 11);
    UdpSender b(&t.loop, proxy_port, 22);
    const auto settled = [&] {
        if (a.complete() && b.complete()) t.stop();
    };
    a.on_change = settled;
    b.on_change = settled;

    uv_timer_t resend{};
    uv_timer_init(&t.loop, &resend);
    std::function<void()> tick = [&a, &b] {
        a.send_outstanding();
        b.send_outstanding();
    };

    // Public mode: the server invents its own key, so this test can never
    // collide with a record some other run left on the DHT.
    Pair pair(t, server_options(echo.port, /*secure=*/false, /*udp=*/true, std::nullopt));
    EXPECT_EQ(pair.start(proxy_port, /*udp=*/true,
                         [&] { every(&resend, kResendMs, &tick); }),
              0);

    t.run();

    uv_timer_stop(&resend);
    close_handle(&resend);
    a.close();
    b.close();
    pair.close();
    echo.close();
    t.drain();

    EXPECT_FALSE(t.timed_out)
        << "udp tunnel never completed; url=" << pair.url
        << " server_err=" << pair.server_err << " client_err=" << pair.client_err;
    EXPECT_EQ(pair.server_err, 0);
    EXPECT_EQ(pair.client_err, 0);
    EXPECT_EQ(pair.server.info().protocol, "udp");
    for (size_t i = 0; i < a.got.size(); ++i) {
        EXPECT_TRUE(a.got[i]) << "sender A never got its " << a.payloads[i].size()
                              << "-byte datagram back";
        EXPECT_TRUE(b.got[i]) << "sender B never got its " << b.payloads[i].size()
                              << "-byte datagram back";
    }
    EXPECT_FALSE(a.mismatch) << "sender A received a datagram it never sent; lengths seen: "
                             << testing::PrintToString(a.seen);
    EXPECT_FALSE(b.mismatch) << "sender B received a datagram it never sent; lengths seen: "
                             << testing::PrintToString(b.seen);
}

TEST(Integration, LookupFindsThePublishedRecord) {
    if (!network_tests_enabled()) GTEST_SKIP() << "set HOLESAIL_NETWORK_TESTS=1";

    TestLoop t;
    // Nothing binds this — the server only publishes it as metadata.
    const uint16_t service_port = free_port(&t.loop, false);
    EXPECT_NE(service_port, 0);

    Holesail server(&t.loop, server_options(service_port, /*secure=*/false,
                                            /*udp=*/false, std::nullopt));
    std::string url;
    std::optional<LookupResult> found;
    int server_err = -1;
    int attempts = 0;
    bool in_flight = false;

    // A record needs a moment to propagate past the node that took the put, so
    // the lookup is retried rather than trusted to work first time. One at a
    // time — each one stands up a throwaway DHT of its own.
    uv_timer_t poll{};
    uv_timer_init(&t.loop, &poll);
    std::function<void()> tick = [&] {
        if (in_flight) return;
        in_flight = true;
        ++attempts;
        const int lrc = Holesail::lookup(
            &t.loop, url, [&](int, const std::optional<LookupResult>& result) {
                in_flight = false;
                if (!result) return;
                found = result;
                t.stop();
            });
        if (lrc != 0) in_flight = false;
    };

    const int rc = server.ready([&](int err) {
        server_err = err;
        if (err != 0) {
            t.stop();
            return;
        }
        url = server.info().url;
        every(&poll, kLookupMs, &tick);
    });
    EXPECT_EQ(rc, 0);

    t.run();

    uv_timer_stop(&poll);
    close_handle(&poll);
    server.close();
    t.drain();

    EXPECT_FALSE(t.timed_out)
        << "record never resolved after " << attempts << " lookups; url=" << url;
    EXPECT_EQ(server_err, 0);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->host, "127.0.0.1");
    EXPECT_EQ(found->port, service_port);
    EXPECT_EQ(found->protocol, "tcp");
    EXPECT_FALSE(found->secure);   // follows the url prefix, and this one is 0000
}
