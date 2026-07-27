// SPDX-License-Identifier: AGPL-3.0-or-later
// Part of holesail-cpp — a C++ port of holesail (https://github.com/holesail/holesail)
#pragma once

#include "holesail/logger.hpp"
#include "holesail/pipe.hpp"   // StreamSide

#include <uv.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace holesail {

// Datagrams cross the reliable stream as [uint32 big-endian length][payload].
// The largest possible UDP payload is 65507 bytes; the cap is set at the
// 16-bit maximum so no legitimate datagram is ever rejected.
inline constexpr size_t kMaxDatagram = 65535;

std::vector<uint8_t> frame(const uint8_t* data, size_t len);

class FrameDecoder {
public:
    // Appends a chunk of stream bytes and emits every complete frame.
    // Returns false when a length header exceeds kMaxDatagram — the caller
    // MUST then destroy the stream; the peer is either broken or hostile.
    bool push(const uint8_t* data, size_t len,
              const std::function<void(const uint8_t*, size_t)>& on_frame);
    void reset() { buffer_.clear(); }
    size_t buffered() const { return buffer_.size(); }

private:
    std::vector<uint8_t> buffer_;
};

// Server side: one local UDP socket bridged to one encrypted stream.
// Mirrors pipeUdpFramedServer.
class UdpPipe {
public:
    UdpPipe(uv_loop_t* loop, StreamSide stream, Logger& logger);
    ~UdpPipe();
    UdpPipe(const UdpPipe&) = delete;
    UdpPipe& operator=(const UdpPipe&) = delete;

    int start(const std::string& host, uint16_t port);
    void on_stream_data(const uint8_t* data, size_t len);
    void on_stream_close();
    void destroy();
    void set_on_destroy(std::function<void()> cb) { on_destroy_ = std::move(cb); }

private:
    static void alloc_cb(uv_handle_t* handle, size_t suggested, uv_buf_t* buf);
    static void recv_cb(uv_udp_t* handle, ssize_t nread, const uv_buf_t* buf,
                        const struct sockaddr* addr, unsigned flags);

    uv_loop_t* loop_;
    StreamSide stream_;
    Logger& logger_;
    uv_udp_t* sock_ = nullptr;
    struct sockaddr_in local_addr_{};
    FrameDecoder decoder_;
    std::vector<char> scratch_ = std::vector<char>(kMaxDatagram + 1);
    std::function<void()> on_destroy_;
    bool destroyed_ = false;
};

// Client side: one bound UDP socket, one encrypted stream per source address.
// Mirrors createUdpFramedProxy.
class UdpProxy {
public:
    UdpProxy(uv_loop_t* loop, Logger& logger);
    ~UdpProxy();
    UdpProxy(const UdpProxy&) = delete;
    UdpProxy& operator=(const UdpProxy&) = delete;

    // `open_stream` is called the first time a datagram arrives from a given
    // "address:port" and must return the StreamSide for that client's stream.
    int listen(const std::string& host, uint16_t port,
               std::function<StreamSide(const std::string& client_id)> open_stream);

    void on_stream_data(const std::string& client_id, const uint8_t* data, size_t len);
    void on_stream_close(const std::string& client_id);
    void close();
    uint16_t bound_port() const { return bound_port_; }

private:
    struct Client {
        StreamSide stream;
        struct sockaddr_in addr{};
        FrameDecoder decoder;
    };

    static void alloc_cb(uv_handle_t* handle, size_t suggested, uv_buf_t* buf);
    static void recv_cb(uv_udp_t* handle, ssize_t nread, const uv_buf_t* buf,
                        const struct sockaddr* addr, unsigned flags);

    uv_loop_t* loop_;
    Logger& logger_;
    uv_udp_t* sock_ = nullptr;
    std::function<StreamSide(const std::string&)> open_stream_;
    std::map<std::string, std::unique_ptr<Client>> clients_;
    std::vector<char> scratch_ = std::vector<char>(kMaxDatagram + 1);
    uint16_t bound_port_ = 0;
};

}  // namespace holesail
