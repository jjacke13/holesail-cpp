// SPDX-License-Identifier: AGPL-3.0-or-later
// Part of holesail-cpp — a C++ port of holesail (https://github.com/holesail/holesail)
#pragma once

#include "holesail/keys.hpp"
#include "holesail/logger.hpp"
#include "holesail/pipe.hpp"
#include "holesail/record.hpp"
#include "holesail/udp_pipe.hpp"

#include <hyperdht/hyperdht.h>
#include <uv.h>

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>

namespace holesail {

// Port of `holesail-client`. Resolves a connection key to a DHT public key,
// reads the server's metadata record, then listens locally and gives every
// local socket its own encrypted stream to that key.
//
// The DHT is borrowed, not owned: the caller creates it and destroys it
// *after* this object (`destroy()` closes the proxy and every stream, but
// never the DHT itself). `dht` may be null for tests that only exercise key
// derivation and `resolve()` — nothing dereferences it before `connect()`.
class HolesailClient {
public:
    struct Config {
        std::string key;                    // the connection key (post-url-parse)
        bool secure = false;
        std::optional<uint16_t> local_port; // --port
        std::optional<std::string> local_host;  // --host
        std::optional<bool> udp;            // --udp
    };
    struct Resolved { uint16_t port; std::string host; bool udp; };

    HolesailClient(uv_loop_t* loop, hyperdht_t* dht, Config config, Logger& logger);
    ~HolesailClient();

    HolesailClient(const HolesailClient&) = delete;
    HolesailClient& operator=(const HolesailClient&) = delete;

    // Fetches the DHT record, then starts the local proxy. `on_listening`
    // fires with the listen result once the proxy is up (or failed).
    // Returns 0 once the lookup is under way, negative if it could not start
    // — including a key that is not valid z32 in public mode.
    int connect(std::function<void(int err)> on_listening);
    void pause();
    void resume();
    void destroy();
    std::string state() const;
    const std::array<uint8_t, 32>& target_public_key() const;
    const Resolved& resolved() const;
    // Whether this client presents the derived keypair as its own identity.
    bool uses_derived_identity() const { return config_.secure; }

    // JS: options.port ?? record.port ?? 8989, etc. Pure — tested directly.
    static Resolved resolve(const Config& config, const std::optional<Record>& record);
    static constexpr uint16_t kDefaultPort = 8989;

private:
    // One local socket (TCP) or one local UDP source address, plus the DHT
    // stream it owns. Defined in the .cpp — held by shared_ptr so the
    // hyperdht C callbacks can carry a weak_ptr and stay safe across teardown.
    struct Conn;
    struct ConnRef;    // heap userdata for the connect / stream callbacks
    struct DrainCtx;   // heap userdata for one backpressured write

    // Anything written before the stream opens is buffered; past this the
    // datagrams are dropped, which is what UDP promises anyway.
    static constexpr size_t kMaxPendingBytes = 256 * 1024;

    static void mutable_cb(uint64_t seq, const uint8_t* value, size_t len,
                           const uint8_t signature[64], void* userdata);
    static void mutable_done_cb(int error, void* userdata);
    static void connect_cb(int error, const hyperdht_connection_t* conn, void* userdata);
    static void stream_open_cb(void* userdata);
    static void stream_data_cb(const uint8_t* data, size_t len, void* userdata);
    static void stream_close_cb(void* userdata);
    static void drain_cb(hyperdht_stream_t* stream, void* userdata);

    int start_proxy();
    void on_local_connection(uv_tcp_t* accepted);
    StreamSide open_udp_stream(const std::string& client_id);
    bool begin_dht_connect(const std::shared_ptr<Conn>& conn);
    StreamSide make_side(const std::shared_ptr<Conn>& conn);
    void on_stream_open(const std::shared_ptr<Conn>& conn);
    void shutdown_conn(const std::shared_ptr<Conn>& conn);
    void reap(uint64_t id);

    uv_loop_t* loop_;
    hyperdht_t* dht_;
    Config config_;
    Logger& logger_;

    // Secure mode only: the server's keypair, re-derived from the key. It is
    // both the connection target and our own DHT identity.
    hyperdht_keypair_t keypair_{};
    std::array<uint8_t, 32> target_{};
    int init_error_ = 0;

    TcpProxy tcp_proxy_;
    UdpProxy udp_proxy_;
    Resolved resolved_{kDefaultPort, "127.0.0.1", false};
    std::optional<Record> record_;
    uint64_t record_seq_ = 0;
    hyperdht_query_t* query_ = nullptr;
    std::function<void(int)> on_listening_;

    std::map<uint64_t, std::shared_ptr<Conn>> conns_;
    uint64_t next_id_ = 1;
    std::string state_;
    bool destroyed_ = false;
};

}  // namespace holesail
