// SPDX-License-Identifier: AGPL-3.0-or-later
// Part of holesail-cpp — a C++ port of holesail (https://github.com/holesail/holesail)
#pragma once

#include "holesail/logger.hpp"
#include "holesail/pipe.hpp"
#include "holesail/udp_pipe.hpp"

#include <hyperdht/hyperdht.h>
#include <uv.h>

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>

namespace holesail {

// Port of `holesail-server`. Owns one DHT server: derives the keypair from a
// seed, gates peers with a firewall in secure mode, publishes the local
// host/port record, and bridges every inbound stream to the local service.
//
// Lifetime: the `uv_loop_t`, the `hyperdht_t` and the `Logger` are borrowed and
// must outlive this object. Call `destroy()` and let the loop drain before
// destroying the DHT — hyperdht callbacks carry a raw `this`.
class HolesailServer {
public:
    struct Config {
        uint16_t local_port = 0;
        std::string local_host = "127.0.0.1";
        std::string seed_hex;      // 64 hex chars; empty means "generate"
        bool secure = true;
        bool udp = false;
    };

    // Derives the keypair immediately; `dht` may be null (nothing touches it
    // before `start()`), which is what makes key derivation unit-testable.
    // Throws std::invalid_argument when `seed_hex` is not 64 hex characters.
    HolesailServer(uv_loop_t* loop, hyperdht_t* dht, Config config, Logger& logger);
    ~HolesailServer();

    HolesailServer(const HolesailServer&) = delete;
    HolesailServer& operator=(const HolesailServer&) = delete;

    // Creates the DHT server, listens on the keypair and publishes the record.
    // Returns 0 once the announce is under way; `on_listening` fires later,
    // when the announcer has completed its first cycle.
    int start(std::function<void(int err)> on_listening);
    void pause();
    void resume();
    void destroy();

    // z32(seed) when secure, z32(public_key) otherwise — JS `HolesailServer.key`.
    std::string key() const;
    std::string public_key_z32() const;
    const std::array<uint8_t, 32>& public_key() const;
    std::string state() const;   // "listening" | "paused" | "destroyed" | ""
    const Config& config() const;

    // Exposed for testing the seq rule without a live DHT.
    static uint64_t next_seq(bool had_record, uint64_t old_seq, bool value_unchanged);
    static constexpr uint64_t kRefreshIntervalMs = 50 * 60 * 1000;

private:
    // One inbound connection: the encrypted stream plus the pipe bridging it to
    // the local service. Defined in server.cpp.
    struct Conn;

    static int firewall_cb(const uint8_t remote_pk[32], const char* host,
                           uint16_t port, void* userdata);
    static void connection_cb(const hyperdht_connection_t* conn, void* userdata);
    static void listening_cb(void* userdata);
    static void refresh_cb(uv_timer_t* timer);

    // Anything the local service produces before the encrypted stream has
    // finished its header exchange is buffered here; past this it is dropped.
    // Same budget as the client's identically-named cap.
    static constexpr size_t kMaxPendingBytes = 256 * 1024;

    // Encrypted-stream callbacks. `userdata` is the boxed strong reference to
    // the Conn; stream_close_cb is the one that gives it back.
    static void stream_open_cb(void* userdata);
    static void stream_data_cb(const uint8_t* data, size_t len, void* userdata);
    static void stream_close_cb(void* userdata);
    static void drain_cb(hyperdht_stream_t* stream, void* userdata);

    // mutable_get, then mutable_put with the seq JS would have used.
    static void get_value_cb(uint64_t seq, const uint8_t* value, size_t len,
                             const uint8_t signature[64], void* userdata);
    static void get_done_cb(int err, void* userdata);

    StreamSide make_side(Conn* conn);
    void publish();

    uv_loop_t* loop_;
    hyperdht_t* dht_;
    Config config_;
    Logger& logger_;

    hyperdht_keypair_t keypair_{};
    std::array<uint8_t, 32> seed_{};
    std::array<uint8_t, 32> public_key_{};

    hyperdht_server_t* srv_ = nullptr;
    uv_timer_t* refresh_ = nullptr;
    std::string record_;                 // the exact JSON bytes we publish
    std::string state_;
    std::function<void(int)> on_listening_;

    // Non-owning: a Conn is kept alive by a strong reference handed to its
    // stream and released from the stream's close callback, so a pending
    // drain can never fire into a freed pipe.
    std::map<hyperdht_stream_t*, std::weak_ptr<Conn>> conns_;

    // Dropped when this object dies, so DHT queries still in flight can tell.
    std::shared_ptr<int> alive_ = std::make_shared<int>(0);
};

}  // namespace holesail
