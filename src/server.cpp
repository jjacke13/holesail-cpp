// SPDX-License-Identifier: AGPL-3.0-or-later
// Part of holesail-cpp — a C++ port of holesail (https://github.com/holesail/holesail)
#include "holesail/server.hpp"

#include "holesail/keys.hpp"
#include "holesail/record.hpp"

#include <sodium.h>

#include <functional>
#include <algorithm>
#include <optional>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

namespace holesail {
namespace {

// A write's completion handler. The C drain callback carries a void*, so the
// std::function has to live on the heap until the write is acknowledged. The
// Conn tracks every live one, so a write abandoned by a dying stream is still
// freed rather than leaked.
struct DrainCtx;

// The DHT put is a two-step query (get latest, then put); this rides along.
struct GetCtx {
    std::weak_ptr<int> alive;    // expired once the server is gone
    HolesailServer* server = nullptr;
    uint64_t seq = 0;
    std::string value;
    bool had = false;
};

}  // namespace

// One inbound connection: the encrypted stream plus the pipe bridging it to
// the local service.
//
// Two strong references keep a Conn alive, and it dies when the *later* of
// them is dropped:
//   A. the box handed to hyperdht as the stream's userdata — released by
//      stream_close_cb, because hyperdht deletes the stream immediately after
//      that callback returns;
//   B. one captured in the pipe's on_destroy — released once the pipe's libuv
//      handle has finished closing.
// Dropping either one early is a use-after-free: a pending drain callback
// holds a raw pipe pointer (A before B), and libuv's close callback holds a
// raw pipe pointer too (B before A).
struct HolesailServer::Conn {
    hyperdht_stream_t* stream = nullptr;   // null once the stream has closed
    std::unique_ptr<TcpPipe> tcp;
    std::unique_ptr<UdpPipe> udp;
    std::set<DrainCtx*> drains;            // writes still awaiting an ack

    // The encrypted stream is NOT writable the moment hyperdht_stream_open()
    // returns — it becomes writable when the header exchange completes and
    // on_open fires. Until then hyperdht_stream_write() returns -1, and
    // TcpPipe treats a negative write as fatal and destroys the connection.
    //
    // That window is invisible for a client-speaks-first protocol like HTTP,
    // because we have nothing to send until the request arrives. It is fatal
    // for a server-speaks-first one — sshd emits its banner the instant we
    // connect, and over a real holepunched path the header exchange is still
    // in flight, so the banner write failed and the connection was torn down
    // silently. Buffer instead, and flush in stream_open_cb.
    bool stream_open = false;
    std::vector<uint8_t> pending;
    // Drain callbacks withheld while buffering. Not calling them is what makes
    // TcpPipe apply backpressure to the local socket instead of dropping, so
    // they must be fired once the flush lands. A Conn torn down before the
    // stream opens simply discards them — the same thing hyperdht does with a
    // drain callback whose stream closed before the ack.
    std::vector<std::function<void()>> pending_drains;

    ~Conn();
};

namespace {

struct DrainCtx {
    std::set<DrainCtx*>* owner;   // the Conn's set, so an ack can deregister
    std::function<void()> fn;
};

// hyperdht_stream_write_with_drain reports 0 for *every* accepted write — it
// never forwards libudx's 1-means-drained (SecretStreamDuplex::write ends in a
// literal `return 0`). Treating that as backpressure would stall the tunnel for
// a full round trip per 64 KB chunk.
//
// The fix lives in TcpPipe, not here: the pipe ignores the return value and
// bounds unacknowledged bytes itself (kStreamHighWaterMark), which fixes the
// client path too — the client cannot synthesise a drained signal at all.
// So make_side just reports "submitted" and lets the pipe decide.

}  // namespace

// Safe because a Conn outlives its stream, and hyperdht never fires a drain
// callback after the stream has closed.
HolesailServer::Conn::~Conn() {
    for (DrainCtx* ctx : drains) delete ctx;
}

// ---------------------------------------------------------------------------
// Construction — JS generateKeyPair()
// ---------------------------------------------------------------------------

HolesailServer::HolesailServer(uv_loop_t* loop, hyperdht_t* dht, Config config,
                               Logger& logger)
    : loop_(loop), dht_(dht), config_(std::move(config)), logger_(logger) {
    if (config_.seed_hex.empty()) config_.seed_hex = random_hex32();

    const auto bytes = from_hex(config_.seed_hex);
    if (!bytes || bytes->size() != 32) {
        throw std::invalid_argument("seed must be 64 hex characters");
    }
    std::copy(bytes->begin(), bytes->end(), seed_.begin());

    hyperdht_keypair_from_seed(&keypair_, seed_.data());
    std::copy(keypair_.public_key, keypair_.public_key + 32, public_key_.begin());

    logger_.debug("Generated key pair from seed: " + config_.seed_hex);
}

HolesailServer::~HolesailServer() {
    destroy();
    hyperdht_keypair_zero(&keypair_);
    sodium_memzero(seed_.data(), seed_.size());
}

// ---------------------------------------------------------------------------
// Identity
// ---------------------------------------------------------------------------

std::string HolesailServer::key() const {
    return config_.secure ? z32_encode(seed_.data(), seed_.size())
                          : z32_encode(public_key_.data(), public_key_.size());
}

std::string HolesailServer::public_key_z32() const {
    return z32_encode(public_key_.data(), public_key_.size());
}

const std::array<uint8_t, 32>& HolesailServer::public_key() const { return public_key_; }

std::string HolesailServer::state() const { return state_; }

const HolesailServer::Config& HolesailServer::config() const { return config_; }

// JS: seq is left undefined when there is no prior record, and hyperdht
// defaults it to 0.
uint64_t HolesailServer::next_seq(bool had_record, uint64_t old_seq,
                                  bool value_unchanged) {
    if (!had_record) return 0;
    return value_unchanged ? old_seq : old_seq + 1;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

int HolesailServer::start(std::function<void(int err)> on_listening) {
    if (dht_ == nullptr || srv_ != nullptr) return UV_EINVAL;

    srv_ = hyperdht_server_create(dht_);
    if (srv_ == nullptr) {
        logger_.error("Failed to create the DHT server");
        return UV_ENOMEM;
    }

    // Secure mode: only a peer that derived the same keypair from the key gets
    // past the handshake. This is the whole access-control story.
    if (config_.secure) {
        hyperdht_server_set_firewall(srv_, &firewall_cb, this);
        logger_.info("Using Private Mode");
    } else {
        logger_.info("Using Public Mode");
    }
    hyperdht_server_set_reusable_socket(srv_, 1);

    on_listening_ = std::move(on_listening);
    hyperdht_server_on_listening(srv_, &listening_cb, this);

    const int rc = hyperdht_server_listen(srv_, &keypair_, &connection_cb, this);
    if (rc < 0) {
        logger_.error("Server listen failed");
        return rc;
    }

    record_ = encode_record(config_.local_host,
                            config_.udp ? std::optional<bool>(true) : std::nullopt,
                            config_.local_port);
    logger_.debug("Initializing DHT with host info: " + record_);
    publish();

    refresh_ = new uv_timer_t{};
    uv_timer_init(loop_, refresh_);
    refresh_->data = this;
    uv_timer_start(refresh_, &refresh_cb, kRefreshIntervalMs, kRefreshIntervalMs);
    return 0;
}

void HolesailServer::listening_cb(void* userdata) {
    auto* self = static_cast<HolesailServer*>(userdata);
    if (self == nullptr) return;
    self->state_ = "listening";
    self->logger_.info("Server listening on key: " + self->key());
    if (self->on_listening_) self->on_listening_(0);
}

void HolesailServer::pause() {
    if (dht_ == nullptr) return;
    hyperdht_suspend(dht_);
    state_ = "paused";
    logger_.info("Server paused");
}

void HolesailServer::resume() {
    if (dht_ == nullptr) return;
    hyperdht_resume(dht_);
    state_ = "listening";
    logger_.info("Server resumed");
}

// Order matters: the refresh timer first, then the pipes, then the server.
// Closing the server first can fire a connection callback into a freed pipe.
void HolesailServer::destroy() {
    if (refresh_ != nullptr) {
        uv_timer_stop(refresh_);
        uv_close(reinterpret_cast<uv_handle_t*>(refresh_),
                 [](uv_handle_t* h) { delete reinterpret_cast<uv_timer_t*>(h); });
        refresh_ = nullptr;
    }

    for (auto& entry : conns_) {
        // The lock keeps the Conn alive for the body; it is released for real
        // by the pipe's on_destroy and by the stream's close callback.
        if (auto conn = entry.second.lock()) {
            if (conn->tcp) conn->tcp->destroy();
            if (conn->udp) conn->udp->destroy();
        }
    }
    conns_.clear();

    if (srv_ != nullptr) {
        hyperdht_server_close_force(srv_, nullptr, nullptr);
        srv_ = nullptr;
    }
    state_ = "destroyed";
}

// ---------------------------------------------------------------------------
// Firewall — secure mode only
// ---------------------------------------------------------------------------

int HolesailServer::firewall_cb(const uint8_t remote_pk[32], const char* /*host*/,
                                uint16_t /*port*/, void* userdata) {
    auto* self = static_cast<HolesailServer*>(userdata);
    if (self == nullptr || remote_pk == nullptr) return 1;
    // Constant time: the comparison is against our own public key, and a
    // timing oracle on it would leak which prefix a guess got right.
    // Contract: 0 accepts, non-zero rejects.
    return sodium_memcmp(remote_pk, self->public_key_.data(), 32) == 0 ? 0 : 1;
}

// ---------------------------------------------------------------------------
// Inbound connections
// ---------------------------------------------------------------------------

StreamSide HolesailServer::make_side(Conn* conn) {
    StreamSide side;
    side.write = [conn](const uint8_t* data, size_t len,
                        std::function<void()> on_drain) {
        // Not writable until the header exchange completes — see Conn::pending.
        // `data` is borrowed for this call only, so it has to be copied.
        //
        // JS parity: `connPiper` does `connection.write(d) || (loc.pause(),
        // connection.once('drain', () => loc.resume()))`. A Node duplex buffers
        // until the stream is ready and never fails the write; backpressure is
        // the `false` return plus a later `drain`. So we buffer and withhold
        // the drain callback rather than dropping — withholding it is exactly
        // what makes TcpPipe stop reading once its unacked count passes
        // kStreamHighWaterMark, which pushes back on the local socket instead
        // of losing its bytes. stream_open_cb fires the withheld drains.
        if (!conn->stream_open) {
            if (conn->pending.size() + len > kMaxPendingBytes) {
                // Backstop only. TCP never gets here: TcpPipe stops reading at
                // kStreamHighWaterMark first, and that is the same 256 KiB.
                // UDP has no such brake, and a dropped datagram is what UDP
                // promises anyway.
                return 1;
            }
            conn->pending.insert(conn->pending.end(), data, data + len);
            if (on_drain) conn->pending_drains.push_back(std::move(on_drain));
            return 1;
        }
        // Every hyperdht_stream_* call below tolerates a null stream, which is
        // what a closed connection leaves behind.
        if (!on_drain) return hyperdht_stream_write(conn->stream, data, len);

        auto* ctx = new DrainCtx{&conn->drains, std::move(on_drain)};
        conn->drains.insert(ctx);
        const int rc = hyperdht_stream_write_with_drain(conn->stream, data, len,
                                                        &drain_cb, ctx);
        if (rc < 0) {
            conn->drains.erase(ctx);
            delete ctx;
            return rc;
        }
        // Report submitted and let TcpPipe own backpressure. It bounds
        // unacked bytes itself (kStreamHighWaterMark) precisely because
        // hyperdht's write cannot report drained-vs-backpressured, so
        // returning a synthetic 1/0 here would be a second, redundant
        // mechanism that the pipe ignores anyway.
        return rc;
    };
    side.pause = [conn] { hyperdht_stream_pause(conn->stream); };
    side.resume = [conn] { hyperdht_stream_resume(conn->stream); };
    side.close = [conn] { hyperdht_stream_close(conn->stream); };
    return side;
}

void HolesailServer::drain_cb(hyperdht_stream_t* /*stream*/, void* userdata) {
    auto* ctx = static_cast<DrainCtx*>(userdata);
    if (ctx == nullptr) return;
    auto fn = std::move(ctx->fn);
    ctx->owner->erase(ctx);
    delete ctx;
    if (fn) fn();   // may tear the pipe down — nothing above is touched after
}

void HolesailServer::connection_cb(const hyperdht_connection_t* c, void* userdata) {
    auto* self = static_cast<HolesailServer*>(userdata);
    if (self == nullptr || c == nullptr || self->srv_ == nullptr) return;

    self->logger_.debug("Incoming connection received from " +
                        z32_encode(c->remote_public_key, 32));

    // ponytail: prune on accept instead of running a reaper — accepts are rare,
    // and the map only holds live conns plus dead ones whose stream pointer has
    // not been recycled yet. Revisit if a server ever holds thousands at once.
    std::erase_if(self->conns_, [](const auto& kv) { return kv.second.expired(); });

    auto conn = std::make_shared<Conn>();
    auto* box = new std::shared_ptr<Conn>(conn);   // reference A
    hyperdht_stream_t* stream = hyperdht_stream_open(
        self->dht_, c, &stream_open_cb, &stream_data_cb, &stream_close_cb, box);
    if (stream == nullptr) {
        delete box;
        self->logger_.error("Failed to open a stream for an inbound connection");
        return;
    }
    conn->stream = stream;
    self->conns_[stream] = conn;

    Conn* raw = conn.get();
    StreamSide side = self->make_side(raw);

    if (self->config_.udp) {
        raw->udp = std::make_unique<UdpPipe>(self->loop_, std::move(side), self->logger_);
        raw->udp->set_on_destroy([keep = conn]() mutable { keep.reset(); });
        raw->udp->start(self->config_.local_host, self->config_.local_port);
    } else {
        raw->tcp = std::make_unique<TcpPipe>(self->loop_, std::move(side), self->logger_);
        raw->tcp->set_on_destroy([keep = conn]() mutable { keep.reset(); });
        // libuv queues writes issued while the connect is still in flight, so
        // there is no need to hold the stream back until the local side is up.
        raw->tcp->connect(self->config_.local_host, self->config_.local_port, nullptr);
    }
}

// The header exchange has completed and the stream is finally writable. Flush
// whatever the local service greeted us with in the meantime, in order, before
// anything else reaches the wire.
//
// Note this does NOT own the box — stream_close_cb is what gives it back.
void HolesailServer::stream_open_cb(void* userdata) {
    auto* box = static_cast<std::shared_ptr<Conn>*>(userdata);
    if (box == nullptr || !*box) return;
    Conn* conn = box->get();

    conn->stream_open = true;

    if (!conn->pending.empty()) {
        const auto buffered = std::move(conn->pending);
        conn->pending.clear();
        if (hyperdht_stream_write(conn->stream, buffered.data(), buffered.size()) < 0) {
            // Nothing useful left to do: the pipes see the same failure on
            // their next write and tear the connection down the normal way.
            conn->pending_drains.clear();
            return;
        }
    }

    // Release the backpressure last, and only after the bytes are away, so a
    // resumed local socket cannot overtake the greeting it was queued behind.
    const auto drains = std::move(conn->pending_drains);
    conn->pending_drains.clear();
    for (const auto& drain : drains) {
        if (drain) drain();
    }
}

void HolesailServer::stream_data_cb(const uint8_t* data, size_t len, void* userdata) {
    auto* box = static_cast<std::shared_ptr<Conn>*>(userdata);
    if (box == nullptr || !*box) return;
    Conn* conn = box->get();
    if (conn->tcp) conn->tcp->on_stream_data(data, len);
    else if (conn->udp) conn->udp->on_stream_data(data, len);
}

void HolesailServer::stream_close_cb(void* userdata) {
    // Takes reference A back; the Conn dies here only if the pipe already
    // finished tearing down (reference B).
    std::unique_ptr<std::shared_ptr<Conn>> box(
        static_cast<std::shared_ptr<Conn>*>(userdata));
    if (!box || !*box) return;

    Conn* conn = box->get();
    conn->stream = nullptr;   // hyperdht frees the stream once we return
    if (conn->tcp) conn->tcp->on_stream_close();
    else if (conn->udp) conn->udp->on_stream_close();
}

// ---------------------------------------------------------------------------
// The DHT record — get latest, then put with JS's seq rule
// ---------------------------------------------------------------------------

void HolesailServer::refresh_cb(uv_timer_t* timer) {
    auto* self = static_cast<HolesailServer*>(timer->data);
    if (self == nullptr) return;
    self->logger_.debug("Refreshing DHT record: " + self->record_);
    self->publish();
}

void HolesailServer::publish() {
    if (dht_ == nullptr || record_.empty()) return;
    auto* ctx = new GetCtx{alive_, this, 0, std::string(), false};
    if (hyperdht_mutable_get(dht_, keypair_.public_key, 0, &get_value_cb,
                             &get_done_cb, ctx) < 0) {
        delete ctx;
        logger_.warn("Could not read the existing DHT record");
    }
}

void HolesailServer::get_value_cb(uint64_t seq, const uint8_t* value, size_t len,
                                  const uint8_t /*signature*/[64], void* userdata) {
    auto* ctx = static_cast<GetCtx*>(userdata);
    if (ctx == nullptr || value == nullptr) return;
    if (ctx->had && seq < ctx->seq) return;   // keep the newest reply
    ctx->seq = seq;
    ctx->value.assign(reinterpret_cast<const char*>(value), len);
    ctx->had = true;
}

void HolesailServer::get_done_cb(int /*err*/, void* userdata) {
    std::unique_ptr<GetCtx> ctx(static_cast<GetCtx*>(userdata));
    if (!ctx || ctx->alive.expired()) return;

    HolesailServer* self = ctx->server;
    const uint64_t seq = next_seq(ctx->had, ctx->seq, ctx->value == self->record_);
    self->logger_.debug("Putting DHT record with seq " + std::to_string(seq) + ": " +
                        self->record_);
    hyperdht_mutable_put(self->dht_, &self->keypair_,
                         reinterpret_cast<const uint8_t*>(self->record_.data()),
                         self->record_.size(), seq, nullptr, nullptr);
}

}  // namespace holesail
