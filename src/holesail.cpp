// SPDX-License-Identifier: AGPL-3.0-or-later
// Part of holesail-cpp — a C++ port of holesail (https://github.com/holesail/holesail)
#include "holesail/holesail.hpp"

#include "holesail/keys.hpp"
#include "holesail/record.hpp"

#include <algorithm>
#include <memory>
#include <utility>

namespace holesail {
namespace {

// hyperdht_destroy() only *schedules* teardown: libuv still owes the DHT a
// round of close callbacks, and hyperdht.h's own recipe is
// destroy -> uv_run -> free. A nested uv_run() is not an option (close() is
// legal from inside a libuv callback, and lookup() tears down from inside a
// DHT callback), so the drain is a short repeating timer instead: the first
// tick calls destroy — outside any hyperdht callback — and the last one frees.
//
// ponytail: a fixed tick count rather than a "loop is idle" probe. Teardown
// nests two levels of uv_close at most; 16 turns is an order of magnitude more
// than that and costs 16 ms once, at exit.
constexpr int kDrainTurns = 16;

struct DhtReaper {
    uv_timer_t timer;
    hyperdht_t* dht;
    int turns_left;
};

void reap_tick(uv_timer_t* timer) {
    auto* reaper = static_cast<DhtReaper*>(timer->data);
    if (reaper->turns_left == kDrainTurns) hyperdht_destroy(reaper->dht, nullptr, nullptr);
    if (--reaper->turns_left > 0) return;

    uv_timer_stop(timer);
    hyperdht_free(reaper->dht);
    uv_close(reinterpret_cast<uv_handle_t*>(timer),
             [](uv_handle_t* h) { delete static_cast<DhtReaper*>(h->data); });
}

// Takes ownership of `dht`. The caller must have torn down everything that
// borrows it first.
void reap_dht(uv_loop_t* loop, hyperdht_t* dht) {
    if (dht == nullptr) return;
    auto* reaper = new DhtReaper{{}, dht, kDrainTurns};
    uv_timer_init(loop, &reaper->timer);
    reaper->timer.data = reaper;
    uv_timer_start(&reaper->timer, &reap_tick, 0, 1);
}

// JS `new DHT()` — ephemeral, and bootstrapped against the canonical public
// nodes, which hyperdht-cpp leaves off by default.
hyperdht_t* create_dht(uv_loop_t* loop) {
    hyperdht_opts_t opts;
    hyperdht_opts_default(&opts);
    opts.use_public_bootstrap = 1;
    return hyperdht_create(loop, &opts);
}

// Port of validateOpts. `protocol` and `seed` are not fields of Options, so
// the two checks that guard them have nothing to check here.
Options validate_opts(Options options) {
    if ((options.client && !options.key) || (options.key && options.key->empty())) {
        throw OptionsError{"Key is empty"};
    }
    if (options.server && options.client) {
        throw OptionsError{"Can not set both server and client at once"};
    }
    if (options.server && (!options.host || options.host->empty())) {
        throw OptionsError{"No host specified"};
    }
    return options;
}

std::string hash_hex(const std::string& key) { return to_hex(sha256(key).data(), 32); }

}  // namespace

// ---------------------------------------------------------------------------
// Key derivation — Holesail#initialise
// ---------------------------------------------------------------------------

Derived derive(const Options& options) {
    const ParsedUrl parsed = parse_url(options.key.value_or(""));

    Derived derived{};
    // An `hs://s000…` url decides this on its own; only when it says nothing
    // does the flag get a vote (quirk Q4).
    derived.secure = parsed.secure.has_value() ? *parsed.secure
                                               : options.secure.value_or(false);

    if (options.server) {
        if (!parsed.key.empty()) {
            derived.key = parsed.key;
            derived.seed_hex = hash_hex(derived.key);
        } else if (derived.secure) {
            derived.key = random_hex32();
            derived.seed_hex = hash_hex(derived.key);
        }
        // Public server with no key: both stay empty and HolesailServer
        // generates a random seed.
    } else {
        derived.key = parsed.key;
        // The seed is SHA-256 of the key STRING's ASCII bytes — never of a
        // hex-decoded value. This is the wire contract with the JS side.
        if (derived.secure) derived.seed_hex = hash_hex(derived.key);
    }
    return derived;
}

std::string make_url(bool secure, std::string_view key) {
    return std::string(secure ? "hs://s000" : "hs://0000") + std::string(key);
}

// ---------------------------------------------------------------------------
// The facade
// ---------------------------------------------------------------------------

Holesail::Holesail(uv_loop_t* loop, Options options)
    : loop_(loop),
      options_(validate_opts(std::move(options))),
      derived_(derive(options_)),
      // JS: log===false -> off; log===true -> INFO; a number -> that level,
      // clamped to DEBUG..ERROR.
      logger_("Holesail", options_.log_level >= 0,
              static_cast<Level>(std::clamp(options_.log_level, 0, 3))) {}

Holesail::~Holesail() { close(); }

int Holesail::ready(std::function<void(int err)> cb) {
    if (dht_ != nullptr) return UV_EALREADY;   // JS: throw 'Already connected'

    dht_ = create_dht(loop_);
    if (dht_ == nullptr) return UV_ENOMEM;
    opened_ = true;

    if (options_.server) {
        HolesailServer::Config config;
        config.local_port = options_.port.value_or(0);
        config.local_host = options_.host.value_or("127.0.0.1");
        config.seed_hex = derived_.seed_hex;
        config.secure = derived_.secure;
        config.udp = options_.udp.value_or(false);
        server_ = std::make_unique<HolesailServer>(loop_, dht_, std::move(config), logger_);
        return server_->start(std::move(cb));
    }

    HolesailClient::Config config;
    config.key = derived_.key;
    config.secure = derived_.secure;
    config.local_port = options_.port;
    config.local_host = options_.host;
    config.udp = options_.udp;
    client_ = std::make_unique<HolesailClient>(loop_, dht_, std::move(config), logger_);
    return client_->connect(std::move(cb));
}

void Holesail::pause() {
    if (server_) server_->pause();
    if (client_) client_->pause();
}

void Holesail::resume() {
    if (server_) server_->resume();
    if (client_) client_->resume();
}

// Teardown order is the whole point of this class: the streams and proxies
// that borrow the DHT go first, then the DHT itself.
void Holesail::close() {
    if (server_) server_->destroy();
    if (client_) client_->destroy();
    // info() reads through these, so snapshot the last state before they go.
    (void)info();
    server_.reset();
    client_.reset();

    reap_dht(loop_, dht_);
    dht_ = nullptr;
    info_.state = "destroyed";
}

const Info& Holesail::info() const {
    info_.type = options_.server ? "server" : "client";
    info_.secure = derived_.secure;
    info_.seed = derived_.seed_hex;

    // The key the underlying object would report: z32(seed) or z32(publicKey)
    // for a server, the connection key itself for a client.
    std::string underlying = derived_.key;
    if (server_) {
        const auto& config = server_->config();
        info_.state = server_->state();
        info_.port = config.local_port;
        info_.host = config.local_host;
        info_.protocol = config.udp ? "udp" : "tcp";
        info_.public_key = server_->public_key_z32();
        underlying = server_->key();
    } else if (client_) {
        // Post-record defaults: JS reports the options it actually bound.
        const auto& resolved = client_->resolved();
        info_.state = client_->state();
        info_.port = resolved.port;
        info_.host = resolved.host;
        info_.protocol = resolved.udp ? "udp" : "tcp";
        info_.public_key = z32_encode(client_->target_public_key().data(), 32);
    } else if (!opened_) {
        // Never started: only the options say anything. Once it has been
        // opened these keep the last values the server/client reported, which
        // is what JS does too — its `destroy()` leaves `this.dht` in place.
        info_.port = options_.port.value_or(0);
        info_.host = options_.host.value_or("");
        info_.protocol = options_.udp.value_or(false) ? "udp" : "tcp";
    }

    // JS: `if (this.key && this.secure) key = this.key else key = info.key`.
    // A public server started with --key therefore advertises z32(publicKey),
    // not the key it was given.
    info_.key = (!derived_.key.empty() && derived_.secure) ? derived_.key : underlying;
    info_.url = make_url(derived_.secure, info_.key);
    return info_;
}

// ---------------------------------------------------------------------------
// lookup — Holesail.lookup + HolesailClient.ping
// ---------------------------------------------------------------------------

namespace {

struct LookupCtx;

// One of the two interpretations of the key, as the userdata hyperdht hands
// back. Lives inside the LookupCtx, so it is exactly as alive as the query.
struct LookupArm {
    LookupCtx* ctx = nullptr;
    bool primary = false;    // true = keypair_from_seed(arg), false = arg itself
};

struct LookupCtx {
    uv_loop_t* loop = nullptr;
    hyperdht_t* dht = nullptr;
    bool secure = false;
    int pending = 0;
    std::optional<Record> primary_record;
    std::optional<Record> fallback_record;
    uint64_t primary_seq = 0;
    uint64_t fallback_seq = 0;
    std::function<void(int, const std::optional<LookupResult>&)> cb;
    LookupArm arms[2];
};

void lookup_value_cb(uint64_t seq, const uint8_t* value, size_t len,
                     const uint8_t /*signature*/[64], void* userdata) {
    auto* arm = static_cast<LookupArm*>(userdata);
    if (arm == nullptr || value == nullptr || len == 0) return;
    LookupCtx* ctx = arm->ctx;

    auto& record = arm->primary ? ctx->primary_record : ctx->fallback_record;
    auto& record_seq = arm->primary ? ctx->primary_seq : ctx->fallback_seq;
    if (record && seq <= record_seq) return;   // keep the newest reply

    auto decoded = decode_record(std::string_view(reinterpret_cast<const char*>(value), len));
    if (!decoded) return;   // JS ignores a record that is not valid JSON
    record = std::move(decoded);
    record_seq = seq;
}

void lookup_finish(LookupCtx* ctx) {
    const std::unique_ptr<LookupCtx> owned(ctx);

    // JS ping() tries the secure interpretation first and only falls back.
    const auto& record = ctx->primary_record ? ctx->primary_record : ctx->fallback_record;
    std::optional<LookupResult> result;
    if (record) {
        result = LookupResult{record->host, record->port.value_or(0),
                              record->udp.value_or(false) ? "udp" : "tcp", ctx->secure};
    }

    // Scheduled, not immediate — this runs inside a hyperdht callback.
    reap_dht(ctx->loop, ctx->dht);
    if (ctx->cb) ctx->cb(0, result);
}

void lookup_done_cb(int /*error*/, void* userdata) {
    auto* arm = static_cast<LookupArm*>(userdata);
    if (arm == nullptr) return;
    if (--arm->ctx->pending > 0) return;
    lookup_finish(arm->ctx);
}

}  // namespace

int Holesail::lookup(uv_loop_t* loop, std::string_view url,
                     std::function<void(int err, const std::optional<LookupResult>&)> cb) {
    const ParsedUrl parsed = parse_url(url);
    const bool secure = parsed.secure.value_or(false);
    const std::string arg = secure ? z32_encode(sha256(parsed.key).data(), 32) : parsed.key;

    const auto raw = z32_decode(arg);
    // JS: `throw new Error('Invalid key format: ' + argKey)`.
    if (!raw || raw->size() != 32) return UV_EINVAL;

    hyperdht_t* dht = create_dht(loop);
    if (dht == nullptr) return UV_ENOMEM;

    hyperdht_keypair_t keypair{};
    hyperdht_keypair_from_seed(&keypair, raw->data());

    auto* ctx = new LookupCtx{};
    ctx->loop = loop;
    ctx->dht = dht;
    ctx->secure = secure;
    ctx->pending = 2;
    ctx->cb = std::move(cb);
    ctx->arms[0] = LookupArm{ctx, true};
    ctx->arms[1] = LookupArm{ctx, false};

    // Both interpretations are queried at once rather than one after the
    // other: same answer as JS's sequential await, one round trip instead of
    // two, and no query has to be started from inside another query's
    // callback. A mutable_get never completes before it returns — it needs at
    // least one network round trip — so `pending` is safe to set up front.
    const bool primary_ok =
        hyperdht_mutable_get(dht, keypair.public_key, 0, &lookup_value_cb,
                             &lookup_done_cb, &ctx->arms[0]) == 0;
    const bool fallback_ok =
        hyperdht_mutable_get(dht, raw->data(), 0, &lookup_value_cb,
                             &lookup_done_cb, &ctx->arms[1]) == 0;
    hyperdht_keypair_zero(&keypair);

    if (!primary_ok && !fallback_ok) {
        delete ctx;
        reap_dht(loop, dht);
        return UV_EIO;
    }
    // An arm that never started owes no done callback; settle it by hand.
    if (!primary_ok || !fallback_ok) lookup_done_cb(0, &ctx->arms[primary_ok ? 1 : 0]);
    return 0;
}

}  // namespace holesail
