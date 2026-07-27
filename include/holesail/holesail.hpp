// SPDX-License-Identifier: AGPL-3.0-or-later
// Part of holesail-cpp — a C++ port of holesail (https://github.com/holesail/holesail)
#pragma once

#include "holesail/client.hpp"
#include "holesail/logger.hpp"
#include "holesail/server.hpp"

#include <hyperdht/hyperdht.h>
#include <uv.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace holesail {

// What the CLI collected. `key` is the raw argument — an `hs://…` url or a
// bare key — because the url is what decides `secure` (quirks Q2 and Q4).
struct Options {
    bool server = false;
    bool client = false;
    std::optional<uint16_t> port;
    std::optional<std::string> host;
    std::optional<std::string> key;
    std::optional<bool> secure;
    std::optional<bool> udp;
    int log_level = -1;            // -1 disables logging
};

// Port of the `Holesail.info` getter. Recomputed on every `info()` call, so it
// is never stale.
struct Info {
    std::string type;              // "server" | "client"
    std::string state;
    bool secure = false;
    uint16_t port = 0;
    std::string host;
    std::string protocol;          // "tcp" | "udp"
    std::string seed;              // hex
    std::string key;
    std::string url;               // hs://...
    std::string public_key;        // z32
};

struct LookupResult { std::string host; uint16_t port; std::string protocol; bool secure; };

// Port of Holesail#initialise. Pure — resolves key/seed/secure before any I/O.
struct Derived { bool secure; std::string key; std::string seed_hex; };
Derived derive(const Options& options);

std::string make_url(bool secure, std::string_view key);   // "hs://s000"/"hs://0000" + key

// Thrown by the constructor only. Port of validateOpts.
struct OptionsError { std::string message; };

// The public API: one holesail endpoint, server or client.
//
// Lifetime — this is the object that owns the `hyperdht_t`; `HolesailServer`
// and `HolesailClient` only borrow it. `close()` therefore tears down in
// order: the server/client first (which closes every stream and proxy), then
// the DHT. The DHT free is deferred onto the loop, because hyperdht still owes
// libuv close callbacks when `hyperdht_destroy()` returns and because `close()`
// is legal from inside a libuv callback (the CLI calls it from its SIGINT
// handler), where a nested `uv_run()` is not.
//
// Consequence for callers: run the loop once more after `close()` — that is
// what completes the teardown and lets `uv_loop_close()` return 0.
//
// The `uv_loop_t` is borrowed and must outlive this object.
class Holesail {
public:
    Holesail(uv_loop_t* loop, Options options);      // throws OptionsError
    ~Holesail();

    Holesail(const Holesail&) = delete;
    Holesail& operator=(const Holesail&) = delete;

    // Port of ReadyResource#ready -> _open(): creates the DHT, then starts the
    // server (listen + publish) or the client (record fetch + local proxy).
    // Returns 0 once that is under way, negative on immediate failure; `cb`
    // fires later with the listen result.
    int ready(std::function<void(int err)> cb);
    void pause();
    void resume();
    void close();
    const Info& info() const;

    // Port of Holesail.lookup + HolesailClient.ping: reads the metadata record
    // published under `url` using a throwaway DHT on `loop`.
    //
    // Returns 0 once the queries are under way — `cb` then fires exactly once,
    // with nullopt when no record was found. Returns UV_EINVAL without calling
    // `cb` when the key is not 32 bytes of z32; JS throws
    // `Invalid key format: <key>` for that case and the CLI renders it.
    static int lookup(uv_loop_t* loop, std::string_view url,
                      std::function<void(int err, const std::optional<LookupResult>&)> cb);

private:
    uv_loop_t* loop_;
    Options options_;
    Derived derived_;
    // Declared before the server/client: both hold a `Logger&`.
    Logger logger_;
    hyperdht_t* dht_ = nullptr;
    bool opened_ = false;
    std::unique_ptr<HolesailServer> server_;
    std::unique_ptr<HolesailClient> client_;
    mutable Info info_;
};

}  // namespace holesail
