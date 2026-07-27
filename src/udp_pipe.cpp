// SPDX-License-Identifier: AGPL-3.0-or-later
// Part of holesail-cpp — a C++ port of holesail (https://github.com/holesail/holesail)
#include "holesail/udp_pipe.hpp"

#include <cstring>
#include <string>
#include <utility>

namespace holesail {
namespace {

// libuv does not copy the payload of a uv_udp_send, so every request owns its
// own copy and frees both in the send callback.
struct SendCtx {
    uv_udp_send_t req{};
    std::vector<char> data;
};

// ponytail: no back-pointer and no logging here. That is what makes an in-flight
// send immune to its pipe being torn down underneath it — libuv cancels pending
// requests during uv_close and this callback only ever frees itself. A datagram
// that fails to leave the box is a lost datagram, which UDP already permits.
void send_cb(uv_udp_send_t* req, int /*status*/) {
    delete static_cast<SendCtx*>(req->data);
}

int send_datagram(uv_udp_t* sock, const struct sockaddr* dst,
                  const uint8_t* data, size_t len) {
    auto* ctx = new SendCtx{};
    ctx->data.resize(len + 1);  // +1 keeps data() non-null for a 0-length datagram
    if (len > 0) std::memcpy(ctx->data.data(), data, len);
    ctx->req.data = ctx;

    uv_buf_t buf = uv_buf_init(ctx->data.data(), static_cast<unsigned>(len));
    const int rc = uv_udp_send(&ctx->req, sock, &buf, 1, dst, send_cb);
    if (rc != 0) delete ctx;
    return rc;
}

// `${rinfo.address}:${rinfo.port}` — the JS client-map key, byte for byte.
std::string client_key(const struct sockaddr* addr) {
    const auto* in = reinterpret_cast<const struct sockaddr_in*>(addr);
    char ip[INET_ADDRSTRLEN] = {0};
    uv_ip4_name(in, ip, sizeof(ip));
    return std::string(ip) + ":" + std::to_string(ntohs(in->sin_port));
}

void alloc_from(std::vector<char>& scratch, size_t suggested, uv_buf_t* buf) {
    const size_t n = suggested < scratch.size() ? suggested : scratch.size();
    buf->base = scratch.data();
    buf->len = static_cast<decltype(buf->len)>(n);
}

// libuv signals "no more datagrams this pass" with nread == 0 AND a null addr.
// A real zero-length datagram carries a non-null addr, so both halves matter.
bool is_end_of_pass(ssize_t nread, const struct sockaddr* addr) {
    return nread == 0 && addr == nullptr;
}

}  // namespace

// ---------------------------------------------------------------------------
// Framing codec
// ---------------------------------------------------------------------------

std::vector<uint8_t> frame(const uint8_t* data, size_t len) {
    std::vector<uint8_t> out;
    out.reserve(4 + len);
    out.push_back(static_cast<uint8_t>((len >> 24) & 0xff));
    out.push_back(static_cast<uint8_t>((len >> 16) & 0xff));
    out.push_back(static_cast<uint8_t>((len >> 8) & 0xff));
    out.push_back(static_cast<uint8_t>(len & 0xff));
    if (data != nullptr && len > 0) out.insert(out.end(), data, data + len);
    return out;
}

bool FrameDecoder::push(const uint8_t* data, size_t len,
                        const std::function<void(const uint8_t*, size_t)>& on_frame) {
    buffer_.insert(buffer_.end(), data, data + len);

    size_t offset = 0;
    while (buffer_.size() - offset >= 4) {
        const uint8_t* p = buffer_.data() + offset;
        const uint32_t frame_len = (static_cast<uint32_t>(p[0]) << 24) |
                                   (static_cast<uint32_t>(p[1]) << 16) |
                                   (static_cast<uint32_t>(p[2]) << 8) |
                                    static_cast<uint32_t>(p[3]);

        // Reject BEFORE reserving or waiting for the claimed length. JS keeps
        // concatenating until buffer.length >= 4 + len, so a 0xFFFFFFFF header
        // makes it buffer toward 4 GiB. In C++ it is worse than a big
        // allocation: see the size_t widening below.
        if (frame_len > kMaxDatagram) {
            buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<long>(offset));
            return false;
        }
        // Widen to size_t before adding. In unsigned-int arithmetic
        // `4 + 0xFFFFFFFF` wraps to 3, which would sail past this
        // incompleteness guard and hand on_frame a 4 GiB out-of-bounds read.
        // The cap above already prevents that, so this is belt and braces —
        // it keeps the guard sound if kMaxDatagram is ever raised or the
        // check above is ever reordered.
        const size_t total = static_cast<size_t>(4) + frame_len;
        if (buffer_.size() - offset < total) break;  // incomplete, wait

        on_frame(buffer_.data() + offset + 4, frame_len);
        offset += total;
    }

    if (offset > 0) {
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<long>(offset));
    }
    return true;
}

// ---------------------------------------------------------------------------
// UdpPipe — mirrors pipeUdpFramedServer
// ---------------------------------------------------------------------------

UdpPipe::UdpPipe(uv_loop_t* loop, StreamSide stream, Logger& logger)
    : loop_(loop), stream_(std::move(stream)), logger_(logger) {}

UdpPipe::~UdpPipe() {
    // destroy() is the normal path and the owner deletes from on_destroy_. This
    // is only a net for an owner that skips it, so the loop is never left with a
    // handle whose data pointer has gone stale.
    if (sock_ == nullptr) return;
    sock_->data = nullptr;  // every callback here tolerates a null self
    if (uv_is_closing(reinterpret_cast<uv_handle_t*>(sock_)) == 0) {
        uv_close(reinterpret_cast<uv_handle_t*>(sock_),
                 [](uv_handle_t* h) { delete reinterpret_cast<uv_udp_t*>(h); });
    }
    sock_ = nullptr;
}

int UdpPipe::start(const std::string& host, uint16_t port) {
    if (destroyed_) return UV_ECANCELED;

    int rc = uv_ip4_addr(host.c_str(), port, &local_addr_);
    if (rc != 0) {
        logger_.error("Invalid local address " + host);
        return rc;
    }

    sock_ = new uv_udp_t{};
    rc = uv_udp_init(loop_, sock_);
    if (rc != 0) {
        delete sock_;
        sock_ = nullptr;
        return rc;
    }
    sock_->data = this;

    // JS leaves the dgram socket unbound and lets the first send pick a port.
    // Bind explicitly so replies can be received before anything has been sent,
    // and so the destination may be any host — binding to it would fail when the
    // local service lives on another machine.
    struct sockaddr_in any {};
    rc = uv_ip4_addr("0.0.0.0", 0, &any);
    if (rc == 0) rc = uv_udp_bind(sock_, reinterpret_cast<const struct sockaddr*>(&any), 0);
    if (rc == 0) rc = uv_udp_recv_start(sock_, alloc_cb, recv_cb);
    if (rc != 0) {
        logger_.error(std::string("Local UDP socket failed: ") + uv_strerror(rc));
        destroy();
        return rc;
    }
    logger_.debug("Local UDP socket ready");
    return 0;
}

void UdpPipe::alloc_cb(uv_handle_t* handle, size_t suggested, uv_buf_t* buf) {
    auto* self = static_cast<UdpPipe*>(handle->data);
    if (self == nullptr) {
        *buf = uv_buf_init(nullptr, 0);
        return;
    }
    alloc_from(self->scratch_, suggested, buf);
}

void UdpPipe::recv_cb(uv_udp_t* handle, ssize_t nread, const uv_buf_t* buf,
                      const struct sockaddr* addr, unsigned flags) {
    auto* self = static_cast<UdpPipe*>(handle->data);
    if (self == nullptr || self->destroyed_) return;
    if (is_end_of_pass(nread, addr)) return;

    if (nread < 0) {
        self->logger_.error(std::string("Local UDP socket error: ") +
                            uv_strerror(static_cast<int>(nread)));
        self->destroy();
        return;
    }
    if (addr == nullptr) return;
    if ((flags & UV_UDP_PARTIAL) != 0) {
        // Cannot happen for IPv4 with a 65536-byte scratch, but forwarding half
        // a datagram would silently corrupt the tunnel.
        self->logger_.warn("Dropping a truncated local datagram");
        return;
    }
    if (!self->stream_.write) return;

    const auto out = frame(reinterpret_cast<const uint8_t*>(buf->base),
                           static_cast<size_t>(nread));
    // ponytail: no backpressure handling — JS does none either, and a datagram
    // dropped under congestion is exactly what UDP promises. Add pause/resume
    // here only if the encrypted stream is ever seen queueing without bound.
    if (self->stream_.write(out.data(), out.size(), {}) < 0) self->destroy();
}

void UdpPipe::on_stream_data(const uint8_t* data, size_t len) {
    if (destroyed_ || sock_ == nullptr || data == nullptr || len == 0) return;

    const bool ok = decoder_.push(data, len, [this](const uint8_t* d, size_t n) {
        send_datagram(sock_, reinterpret_cast<const struct sockaddr*>(&local_addr_), d, n);
    });
    if (!ok) {
        logger_.error("Oversized UDP frame from peer, destroying stream");
        destroy();
    }
}

void UdpPipe::on_stream_close() {
    if (destroyed_) return;
    logger_.debug("Connection end, closing local UDP socket");
    destroy();
}

void UdpPipe::destroy() {
    if (destroyed_) return;
    destroyed_ = true;

    if (sock_ != nullptr) uv_udp_recv_stop(sock_);
    if (stream_.close) stream_.close();

    if (sock_ != nullptr && uv_is_closing(reinterpret_cast<uv_handle_t*>(sock_)) == 0) {
        uv_close(reinterpret_cast<uv_handle_t*>(sock_), [](uv_handle_t* h) {
            auto* self = static_cast<UdpPipe*>(h->data);
            delete reinterpret_cast<uv_udp_t*>(h);
            if (self == nullptr) return;
            self->sock_ = nullptr;
            // Never `delete this` here — the owner does that from on_destroy_.
            if (self->on_destroy_) {
                auto cb = self->on_destroy_;
                self->on_destroy_ = nullptr;
                cb();
            }
        });
        return;  // the close callback runs on_destroy_
    }

    if (on_destroy_) {
        auto cb = on_destroy_;
        on_destroy_ = nullptr;
        cb();
    }
}

// ---------------------------------------------------------------------------
// UdpProxy — mirrors createUdpFramedProxy
// ---------------------------------------------------------------------------

UdpProxy::UdpProxy(uv_loop_t* loop, Logger& logger) : loop_(loop), logger_(logger) {}

UdpProxy::~UdpProxy() { close(); }

int UdpProxy::listen(const std::string& host, uint16_t port,
                     std::function<StreamSide(const std::string&)> open_stream) {
    sock_ = new uv_udp_t{};
    int rc = uv_udp_init(loop_, sock_);
    if (rc != 0) {
        delete sock_;
        sock_ = nullptr;
        return rc;
    }
    sock_->data = this;
    open_stream_ = std::move(open_stream);

    struct sockaddr_in addr {};
    rc = uv_ip4_addr(host.c_str(), port, &addr);
    if (rc == 0) rc = uv_udp_bind(sock_, reinterpret_cast<const struct sockaddr*>(&addr), 0);
    if (rc == 0) rc = uv_udp_recv_start(sock_, alloc_cb, recv_cb);
    if (rc != 0) {
        logger_.error(std::string("UDP listen failed: ") + uv_strerror(rc));
        close();
        return rc;
    }

    struct sockaddr_in bound {};
    int len = sizeof(bound);
    if (uv_udp_getsockname(sock_, reinterpret_cast<struct sockaddr*>(&bound), &len) == 0) {
        bound_port_ = ntohs(bound.sin_port);
    }
    return 0;
}

void UdpProxy::alloc_cb(uv_handle_t* handle, size_t suggested, uv_buf_t* buf) {
    auto* self = static_cast<UdpProxy*>(handle->data);
    if (self == nullptr) {
        *buf = uv_buf_init(nullptr, 0);
        return;
    }
    alloc_from(self->scratch_, suggested, buf);
}

void UdpProxy::recv_cb(uv_udp_t* handle, ssize_t nread, const uv_buf_t* buf,
                       const struct sockaddr* addr, unsigned flags) {
    auto* self = static_cast<UdpProxy*>(handle->data);
    if (self == nullptr) return;
    if (is_end_of_pass(nread, addr)) return;

    if (nread < 0) {
        // JS closes the whole proxy socket on any error event. One ICMP
        // unreachable must not take every other client down with it, so the
        // datagram is dropped and the listener stays up.
        self->logger_.warn(std::string("UDP proxy receive error: ") +
                           uv_strerror(static_cast<int>(nread)));
        return;
    }
    if (addr == nullptr) return;
    if ((flags & UV_UDP_PARTIAL) != 0) {
        self->logger_.warn("Dropping a truncated datagram");
        return;
    }

    const std::string id = client_key(addr);
    auto it = self->clients_.find(id);
    if (it == self->clients_.end()) {
        if (!self->open_stream_) return;
        auto client = std::make_unique<Client>();
        std::memcpy(&client->addr, addr, sizeof(struct sockaddr_in));
        client->stream = self->open_stream_(id);
        if (!client->stream.write) {
            self->logger_.error("No stream available for " + id);
            return;
        }
        self->logger_.debug("New UDP client " + id);
        it = self->clients_.emplace(id, std::move(client)).first;
    }

    Client* client = it->second.get();
    const auto out = frame(reinterpret_cast<const uint8_t*>(buf->base),
                           static_cast<size_t>(nread));
    if (client->stream.write(out.data(), out.size(), {}) < 0) {
        self->logger_.error("Stream write failed for " + id + ", dropping client");
        auto close_fn = client->stream.close;
        self->clients_.erase(it);  // erase first: close_fn may re-enter
        if (close_fn) close_fn();
    }
}

void UdpProxy::on_stream_data(const std::string& client_id, const uint8_t* data, size_t len) {
    if (sock_ == nullptr || data == nullptr || len == 0) return;
    auto it = clients_.find(client_id);
    if (it == clients_.end()) return;  // the client is gone; the datagram is not

    Client* client = it->second.get();
    const bool ok = client->decoder.push(data, len, [this, client](const uint8_t* d, size_t n) {
        send_datagram(sock_, reinterpret_cast<const struct sockaddr*>(&client->addr), d, n);
    });
    if (ok) return;

    logger_.error("Oversized UDP frame from peer for " + client_id + ", destroying stream");
    auto close_fn = client->stream.close;
    clients_.erase(it);  // erase first: close_fn may re-enter through on_stream_close
    if (close_fn) close_fn();
}

void UdpProxy::on_stream_close(const std::string& client_id) {
    clients_.erase(client_id);  // JS: clients.delete(clientId)
}

void UdpProxy::close() {
    // The streams belong to the caller that opened them — JS closes only the
    // socket here too.
    clients_.clear();
    if (sock_ == nullptr) return;

    uv_udp_t* h = sock_;
    sock_ = nullptr;
    h->data = nullptr;
    if (uv_is_closing(reinterpret_cast<uv_handle_t*>(h)) == 0) {
        uv_close(reinterpret_cast<uv_handle_t*>(h),
                 [](uv_handle_t* x) { delete reinterpret_cast<uv_udp_t*>(x); });
    }
}

}  // namespace holesail
