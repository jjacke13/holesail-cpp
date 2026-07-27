// SPDX-License-Identifier: AGPL-3.0-or-later
// Part of holesail-cpp — a C++ port of holesail (https://github.com/holesail/holesail)
#include "holesail/client.hpp"

#include <cstring>
#include <string_view>
#include <utility>
#include <vector>

namespace holesail {

// One local socket bridged to one DHT stream.
//
// Lifetime: owned by `conns_` through a shared_ptr, erased only by `reap()`
// once nothing can call back into it — no pending connect callback, no live
// stream, no live pipe. Every hyperdht callback carries a weak_ptr, so a
// callback that arrives after teardown finds an expired handle instead of
// freed memory. `owner` is safe to dereference exactly while the weak_ptr
// locks: the map that keeps the Conn alive is a member of the owner.
struct HolesailClient::Conn {
    HolesailClient* owner = nullptr;
    uint64_t id = 0;

    hyperdht_stream_t* stream = nullptr;
    bool connect_pending = true;   // a hyperdht connect callback is still owed
    bool stream_open = false;
    bool stream_closed = false;

    uv_tcp_t* tcp = nullptr;         // accepted socket, until the pipe adopts it
    std::unique_ptr<TcpPipe> pipe;   // TCP mode
    bool pipe_done = false;

    bool udp = false;
    std::string client_id;           // UDP mode: "address:port"

    // Bytes written before the stream finished opening. Copied, never
    // borrowed — StreamSide::write hands out a pointer valid only for the
    // duration of the call.
    std::vector<uint8_t> pending;
};

struct HolesailClient::ConnRef {
    std::weak_ptr<Conn> conn;
};

struct HolesailClient::DrainCtx {
    std::weak_ptr<Conn> conn;
    std::function<void()> on_drain;
};

// ---------------------------------------------------------------------------
// Construction and pure logic
// ---------------------------------------------------------------------------

HolesailClient::HolesailClient(uv_loop_t* loop, hyperdht_t* dht, Config config, Logger& logger)
    : loop_(loop),
      dht_(dht),
      config_(std::move(config)),
      logger_(logger),
      tcp_proxy_(loop, logger),
      udp_proxy_(loop, logger) {
    if (config_.secure) {
        // JS: HyperDHT.keyPair(<the seed>) — and the very same keypair becomes
        // the client's own DHT identity, which is the only reason the
        // server's firewall lets it through. See begin_dht_connect().
        const auto seed = sha256(config_.key);
        hyperdht_keypair_from_seed(&keypair_, seed.data());
        std::memcpy(target_.data(), keypair_.public_key, target_.size());
        return;
    }

    const auto raw = z32_decode(config_.key);
    if (!raw || raw->size() != target_.size()) {
        // Surfaced by connect(); there is no way to fail a constructor here
        // without exceptions in a path the CLI drives.
        init_error_ = UV_EINVAL;
        logger_.error("Invalid key format: " + config_.key);
        return;
    }
    std::memcpy(target_.data(), raw->data(), target_.size());
}

// Deleting the client without letting the loop drain first is legal but not
// free: a pipe that is still closing owns a uv handle whose close callback
// points back at it. Orphan those to libuv rather than deleting them — a
// bounded one-off leak at exit beats a use-after-free in uv_close.
HolesailClient::~HolesailClient() {
    destroy();
    for (const auto& entry : conns_) {
        Conn& c = *entry.second;
        if (c.pipe && !c.pipe_done) {
            c.pipe->set_on_destroy(nullptr);   // the lambda captures a dead `this`
            (void)c.pipe.release();
        }
    }
    conns_.clear();
}

std::string HolesailClient::state() const { return state_; }

const std::array<uint8_t, 32>& HolesailClient::target_public_key() const { return target_; }

const HolesailClient::Resolved& HolesailClient::resolved() const { return resolved_; }

// JS: options.port ?? dhtData.port ?? 8989 (and the same for host and udp).
HolesailClient::Resolved HolesailClient::resolve(const Config& config,
                                                 const std::optional<Record>& record) {
    Resolved r{kDefaultPort, "127.0.0.1", false};
    if (record) {
        if (record->port) r.port = *record->port;
        // decode_record reports a missing host as empty, which is the only
        // "absent" marker the struct has.
        if (!record->host.empty()) r.host = record->host;
        if (record->udp) r.udp = *record->udp;
    }
    if (config.local_port) r.port = *config.local_port;
    if (config.local_host) r.host = *config.local_host;
    if (config.udp) r.udp = *config.udp;
    return r;
}

// ---------------------------------------------------------------------------
// connect(): DHT record lookup, then the local proxy
// ---------------------------------------------------------------------------

int HolesailClient::connect(std::function<void(int err)> on_listening) {
    if (init_error_ != 0) return init_error_;
    if (destroyed_) return UV_ECANCELED;
    if (dht_ == nullptr) return UV_EINVAL;

    on_listening_ = std::move(on_listening);
    state_ = "waiting";
    logger_.info("Connecting to key: " + config_.key +
                 ", secure: " + (config_.secure ? "true" : "false"));

    query_ = hyperdht_mutable_get_ex(dht_, target_.data(), 0, &HolesailClient::mutable_cb,
                                     &HolesailClient::mutable_done_cb, this);
    if (query_ == nullptr) {
        // JS treats a failed get as "no record" and falls back to defaults.
        logger_.warn("No DHT data retrieved");
        return start_proxy();
    }
    return 0;
}

void HolesailClient::mutable_cb(uint64_t seq, const uint8_t* value, size_t len,
                                const uint8_t /*signature*/[64], void* userdata) {
    auto* self = static_cast<HolesailClient*>(userdata);
    if (self == nullptr || self->destroyed_ || value == nullptr || len == 0) return;
    if (self->record_ && seq <= self->record_seq_) return;

    auto rec = decode_record(std::string_view(reinterpret_cast<const char*>(value), len));
    if (!rec) {
        self->logger_.warn("Malformed DHT record, ignoring");
        return;
    }
    self->logger_.debug("Retrieved DHT data: " +
                        std::string(reinterpret_cast<const char*>(value), len));
    self->record_ = std::move(rec);
    self->record_seq_ = seq;
}

void HolesailClient::mutable_done_cb(int /*error*/, void* userdata) {
    auto* self = static_cast<HolesailClient*>(userdata);
    if (self == nullptr) return;
    if (self->query_ != nullptr) {
        hyperdht_query_free(self->query_);   // documented safe from inside done
        self->query_ = nullptr;
    }
    if (self->destroyed_) return;
    if (!self->record_) self->logger_.warn("No DHT data retrieved");
    self->start_proxy();
}

int HolesailClient::start_proxy() {
    resolved_ = resolve(config_, record_);
    const std::string where = resolved_.host + ":" + std::to_string(resolved_.port);

    int rc;
    if (resolved_.udp) {
        rc = udp_proxy_.listen(resolved_.host, resolved_.port,
                               [this](const std::string& id) { return open_udp_stream(id); });
    } else {
        rc = tcp_proxy_.listen(resolved_.host, resolved_.port,
                               [this](uv_tcp_t* accepted) { on_local_connection(accepted); });
    }

    if (rc == 0) {
        state_ = "listening";
        logger_.info("Proxy listening on " + where + (resolved_.udp ? " for UDP" : ""));
    } else {
        logger_.error("Proxy failed on " + where + ": " + uv_strerror(rc));
    }
    if (on_listening_) {
        auto cb = on_listening_;
        on_listening_ = nullptr;
        cb(rc);
    }
    return rc;
}

// ---------------------------------------------------------------------------
// One connection per local socket
// ---------------------------------------------------------------------------

void HolesailClient::on_local_connection(uv_tcp_t* accepted) {
    if (accepted == nullptr) return;
    if (destroyed_ || dht_ == nullptr) {
        uv_close(reinterpret_cast<uv_handle_t*>(accepted),
                 [](uv_handle_t* h) { delete reinterpret_cast<uv_tcp_t*>(h); });
        return;
    }
    auto conn = std::make_shared<Conn>();
    conn->owner = this;
    conn->id = next_id_++;
    conn->tcp = accepted;
    conns_.emplace(conn->id, conn);
    // The socket is not read until the pipe adopts it, so anything the local
    // client sends while we dial just waits in the kernel buffer.
    begin_dht_connect(conn);
}

StreamSide HolesailClient::open_udp_stream(const std::string& client_id) {
    if (destroyed_ || dht_ == nullptr) return {};

    auto conn = std::make_shared<Conn>();
    conn->owner = this;
    conn->id = next_id_++;
    conn->udp = true;
    conn->client_id = client_id;
    conns_.emplace(conn->id, conn);

    // UdpProxy needs a StreamSide right now, but the dial is asynchronous —
    // make_side() buffers until the stream opens.
    if (!begin_dht_connect(conn)) return {};
    return make_side(conn);
}

bool HolesailClient::begin_dht_connect(const std::shared_ptr<Conn>& conn) {
    hyperdht_connect_opts_t opts;
    hyperdht_connect_opts_default(&opts);
    // Secure mode: present the derived keypair as our own DHT identity.
    // JS does it once via `new HyperDHT({ keyPair })`; the DHT here is shared
    // and borrowed, so it goes per connection instead.
    if (config_.secure) opts.keypair = &keypair_;
    // JS: handleTCP dials with { reusableSocket: true }, handleUDP with nothing.
    opts.reusable_socket = conn->udp ? 0 : 1;

    auto* ref = new ConnRef{conn};
    const int rc = hyperdht_connect_ex(dht_, target_.data(), &opts,
                                       &HolesailClient::connect_cb, ref);
    if (rc < 0) {
        delete ref;
        conn->connect_pending = false;
        logger_.error(std::string("DHT connect failed: ") + hyperdht_connect_strerror(rc));
        shutdown_conn(conn);
        return false;
    }
    return true;
}

void HolesailClient::connect_cb(int error, const hyperdht_connection_t* conn, void* userdata) {
    const std::unique_ptr<ConnRef> ref(static_cast<ConnRef*>(userdata));
    const auto c = ref->conn.lock();
    if (!c) return;   // the client is gone; nothing left to do

    c->connect_pending = false;
    HolesailClient* self = c->owner;

    if (self->destroyed_ || error != 0 || conn == nullptr) {
        if (error != 0) {
            self->logger_.error(std::string("DHT connect failed: ") +
                                hyperdht_connect_strerror(error));
        }
        self->shutdown_conn(c);
        return;
    }

    // Opened inside the connect callback on purpose — `conn` is only
    // guaranteed alive for the duration of this call.
    auto* sref = new ConnRef{c};
    c->stream = hyperdht_stream_open(self->dht_, conn, &HolesailClient::stream_open_cb,
                                     &HolesailClient::stream_data_cb,
                                     &HolesailClient::stream_close_cb, sref);
    if (c->stream == nullptr) {
        delete sref;
        self->logger_.error("Failed to open stream");
        self->shutdown_conn(c);
    }
}

void HolesailClient::stream_open_cb(void* userdata) {
    // Not owned here — the same ConnRef serves on_data and on_close, and
    // stream_close_cb frees it.
    auto* ref = static_cast<ConnRef*>(userdata);
    const auto c = ref->conn.lock();
    // `stream` is assigned from hyperdht_stream_open's return value, so a null
    // here can only mean the stream already closed: the header exchange this
    // fires on needs a libuv read, which cannot land before that call returns.
    if (!c || c->stream == nullptr || c->stream_closed) return;
    c->owner->on_stream_open(c);
}

void HolesailClient::stream_data_cb(const uint8_t* data, size_t len, void* userdata) {
    auto* ref = static_cast<ConnRef*>(userdata);
    const auto c = ref->conn.lock();
    if (!c || data == nullptr || len == 0) return;

    if (c->udp) {
        c->owner->udp_proxy_.on_stream_data(c->client_id, data, len);
    } else if (c->pipe) {
        c->pipe->on_stream_data(data, len);
    }
}

void HolesailClient::stream_close_cb(void* userdata) {
    const std::unique_ptr<ConnRef> ref(static_cast<ConnRef*>(userdata));
    const auto c = ref->conn.lock();
    if (!c) return;

    c->stream_closed = true;
    c->stream = nullptr;   // hyperdht frees the wrapper once on_close returns
    HolesailClient* self = c->owner;

    if (c->udp) {
        self->udp_proxy_.on_stream_close(c->client_id);
    } else if (c->pipe) {
        c->pipe->on_stream_close();   // flushes, then destroys the pipe
    } else if (c->tcp != nullptr) {
        self->shutdown_conn(c);       // never got as far as a pipe
        return;
    }
    self->reap(c->id);
}

void HolesailClient::on_stream_open(const std::shared_ptr<Conn>& conn) {
    conn->stream_open = true;

    if (!conn->pending.empty()) {
        const auto buffered = std::move(conn->pending);
        conn->pending.clear();
        if (hyperdht_stream_write(conn->stream, buffered.data(), buffered.size()) < 0) {
            shutdown_conn(conn);
            return;
        }
    }
    if (conn->udp) return;   // UdpProxy already holds this stream's side

    uv_tcp_t* accepted = conn->tcp;
    conn->tcp = nullptr;
    if (accepted == nullptr) return;

    conn->pipe = std::make_unique<TcpPipe>(loop_, make_side(conn), logger_);
    const uint64_t id = conn->id;
    conn->pipe->set_on_destroy([this, id] {
        const auto it = conns_.find(id);
        if (it != conns_.end()) it->second->pipe_done = true;
        reap(id);
    });
    if (conn->pipe->adopt(accepted) != 0) shutdown_conn(conn);
}

// ---------------------------------------------------------------------------
// StreamSide over a hyperdht stream
// ---------------------------------------------------------------------------

StreamSide HolesailClient::make_side(const std::shared_ptr<Conn>& conn) {
    const std::weak_ptr<Conn> w = conn;
    StreamSide side;

    side.write = [w](const uint8_t* data, size_t len, std::function<void()> on_drain) -> int {
        const auto c = w.lock();
        if (!c || c->stream_closed) return -1;

        if (c->stream == nullptr || !c->stream_open) {
            // Still dialing. `data` is borrowed for this call only, so copy.
            if (c->pending.size() + len > kMaxPendingBytes) return 1;   // drop
            c->pending.insert(c->pending.end(), data, data + len);
            return 1;
        }
        // 1 = drained, 0 = backpressure, negative = error — never treat 0 as
        // success (hyperdht-cpp gotcha 11).
        if (!on_drain) {
            return hyperdht_stream_write_with_drain(c->stream, data, len, nullptr, nullptr);
        }
        // ponytail: the ctx is freed by the callback, which hyperdht skips if
        // the stream closed before the ack — at most one leak per stream,
        // since TcpPipe keeps a single write outstanding. Track them on the
        // Conn only if that ever shows up in a heap profile.
        auto* ctx = new DrainCtx{w, std::move(on_drain)};
        const int rc = hyperdht_stream_write_with_drain(c->stream, data, len,
                                                        &HolesailClient::drain_cb, ctx);
        if (rc < 0) delete ctx;
        return rc;
    };
    side.pause = [w] {
        const auto c = w.lock();
        if (c && c->stream != nullptr) hyperdht_stream_pause(c->stream);
    };
    side.resume = [w] {
        const auto c = w.lock();
        if (c && c->stream != nullptr) hyperdht_stream_resume(c->stream);
    };
    side.close = [w] {
        const auto c = w.lock();
        if (c && c->stream != nullptr && !c->stream_closed) hyperdht_stream_close(c->stream);
    };
    return side;
}

void HolesailClient::drain_cb(hyperdht_stream_t* /*stream*/, void* userdata) {
    const std::unique_ptr<DrainCtx> ctx(static_cast<DrainCtx*>(userdata));
    const auto c = ctx->conn.lock();
    // The pipe may already be gone — the drain lambda it handed us captures a
    // raw TcpPipe*, so this check is what keeps it from firing into free memory.
    if (!c || c->stream_closed || c->pipe_done) return;
    if (ctx->on_drain) ctx->on_drain();
}

// ---------------------------------------------------------------------------
// Teardown
// ---------------------------------------------------------------------------

void HolesailClient::shutdown_conn(const std::shared_ptr<Conn>& conn) {
    if (conn->tcp != nullptr) {
        uv_tcp_t* accepted = conn->tcp;
        conn->tcp = nullptr;
        accepted->data = nullptr;
        if (uv_is_closing(reinterpret_cast<uv_handle_t*>(accepted)) == 0) {
            uv_close(reinterpret_cast<uv_handle_t*>(accepted),
                     [](uv_handle_t* h) { delete reinterpret_cast<uv_tcp_t*>(h); });
        }
    }
    if (conn->udp) udp_proxy_.on_stream_close(conn->client_id);

    if (conn->pipe && !conn->pipe_done) {
        conn->pipe->destroy();   // closes the stream through StreamSide::close
    } else if (conn->stream != nullptr && !conn->stream_closed) {
        hyperdht_stream_close(conn->stream);
    }
    reap(conn->id);
}

// Erase a connection once nothing can call back into it. Deliberately keyed by
// id rather than pointer: teardown re-enters through several callbacks, and a
// stale iterator or a dangling Conn* is exactly the bug this avoids.
void HolesailClient::reap(uint64_t id) {
    const auto it = conns_.find(id);
    if (it == conns_.end()) return;

    const Conn& c = *it->second;
    if (c.connect_pending) return;
    if (c.stream != nullptr && !c.stream_closed) return;
    if (c.pipe && !c.pipe_done) return;
    conns_.erase(it);
}

void HolesailClient::pause() {
    if (destroyed_ || dht_ == nullptr) return;
    logger_.info("Pausing client");
    hyperdht_suspend(dht_);
    state_ = "paused";
}

void HolesailClient::resume() {
    if (destroyed_ || dht_ == nullptr) return;
    logger_.info("Resuming client");
    hyperdht_resume(dht_);
    state_ = "listening";
}

void HolesailClient::destroy() {
    if (destroyed_) return;
    destroyed_ = true;

    if (query_ != nullptr) {
        hyperdht_query_t* q = query_;
        query_ = nullptr;
        hyperdht_query_cancel(q);
        hyperdht_query_free(q);
    }

    // Stop accepting first: a socket accepted mid-teardown would start a
    // connection into pipes that are already being torn down.
    tcp_proxy_.close();
    udp_proxy_.close();

    // Snapshot — shutdown_conn erases from conns_ as each one settles.
    std::vector<std::shared_ptr<Conn>> live;
    live.reserve(conns_.size());
    for (const auto& entry : conns_) live.push_back(entry.second);
    for (const auto& conn : live) shutdown_conn(conn);

    // The DHT is borrowed; whoever created it destroys it after us.
    state_ = "destroyed";
    logger_.info("Client destroyed");
}

}  // namespace holesail
