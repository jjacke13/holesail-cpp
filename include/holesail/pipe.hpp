// SPDX-License-Identifier: AGPL-3.0-or-later
// Part of holesail-cpp — a C++ port of holesail (https://github.com/holesail/holesail)
#pragma once

#include "holesail/logger.hpp"

#include <uv.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace holesail {

// The encrypted-stream half of a pipe, as a plain struct of callbacks.
// Production code fills these with hyperdht_stream_* calls; tests fill them
// with a double, which is what keeps this file testable without a DHT.
struct StreamSide {
    // Returns 1 when the write drained, 0 under backpressure, negative on
    // error (hyperdht-cpp gotcha 11 — never test for == 0 as success).
    // `on_drain` fires once when a backpressured write completes.
    std::function<int(const uint8_t* data, size_t len, std::function<void()> on_drain)> write;
    std::function<void()> pause;   // stop delivering into on_stream_data
    std::function<void()> resume;
    std::function<void()> close;   // half-close (write_end), then teardown
};

// One bidirectional bridge between a local TCP socket and an encrypted stream.
// Owns its uv_tcp_t. Not copyable — pipes are held by unique_ptr in a map
// keyed by stream.
class TcpPipe {
public:
    // Matches the Node socket highWaterMark semantics the JS piper relies on:
    // once this much data is queued toward the local socket, the encrypted
    // stream is paused until the queue drains.
    static constexpr size_t kHighWaterMark = 64 * 1024;

    TcpPipe(uv_loop_t* loop, StreamSide stream, Logger& logger);
    ~TcpPipe();

    TcpPipe(const TcpPipe&) = delete;
    TcpPipe& operator=(const TcpPipe&) = delete;

    // Server side: dial the local application, then start piping.
    int connect(const std::string& host, uint16_t port, std::function<void(int)> on_ready);

    // Client side: take ownership of an already-accepted handle and start
    // piping immediately.
    int adopt(uv_tcp_t* accepted);

    // Data arrived on the encrypted stream.
    void on_stream_data(const uint8_t* data, size_t len);

    // The encrypted stream closed. hyperdht fires this only after both ends
    // have written end (gotcha 12), so it means "fully finished".
    void on_stream_close();

    void destroy();

    // Fires once, after the local handle has finished closing. The owner uses
    // it to drop the pipe from its map.
    void set_on_destroy(std::function<void()> cb) { on_destroy_ = std::move(cb); }

private:
    static void alloc_cb(uv_handle_t* handle, size_t suggested, uv_buf_t* buf);
    static void read_cb(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf);
    static void connect_cb(uv_connect_t* req, int status);
    static void write_cb(uv_write_t* req, int status);
    static void closed_cb(uv_handle_t* handle);

    void start_reading();
    void stop_reading();
    void resume_reading();
    void flush_finished();

    uv_loop_t* loop_;
    StreamSide stream_;
    Logger& logger_;

    uv_tcp_t* tcp_ = nullptr;
    uv_connect_t connect_req_{};
    std::function<void(int)> on_ready_;
    std::function<void()> on_destroy_;

    std::vector<char> scratch_ = std::vector<char>(64 * 1024);
    size_t pending_bytes_ = 0;   // queued toward the local socket
    int inflight_writes_ = 0;
    bool reading_ = false;
    bool stream_paused_ = false;
    bool stream_closed_ = false;
    bool closing_ = false;       // uv_close issued, waiting for closed_cb
    bool destroyed_ = false;
};

// Listens on a local endpoint and hands each accepted socket to a callback.
// Ownership of the accepted handle transfers to that callback.
class TcpProxy {
public:
    TcpProxy(uv_loop_t* loop, Logger& logger);
    ~TcpProxy();

    TcpProxy(const TcpProxy&) = delete;
    TcpProxy& operator=(const TcpProxy&) = delete;

    int listen(const std::string& host, uint16_t port,
               std::function<void(uv_tcp_t* accepted)> on_connection);
    void close();
    uint16_t bound_port() const { return bound_port_; }

private:
    static void connection_cb(uv_stream_t* server, int status);

    uv_loop_t* loop_;
    Logger& logger_;
    uv_tcp_t* handle_ = nullptr;
    std::function<void(uv_tcp_t*)> on_connection_;
    uint16_t bound_port_ = 0;
};

}  // namespace holesail
