// SPDX-License-Identifier: AGPL-3.0-or-later
// Part of holesail-cpp — a C++ port of holesail (https://github.com/holesail/holesail)
#include "holesail/pipe.hpp"
#include "holesail/addr.hpp"

#include <cstring>

namespace holesail {
namespace {

// Payload carried by each uv_write_t. libuv does NOT copy the buffer, so the
// request has to own it until the write callback fires.
struct WriteCtx {
    TcpPipe* pipe;
    std::vector<char> data;
    size_t bytes;
};

}  // namespace

TcpPipe::TcpPipe(uv_loop_t* loop, StreamSide stream, Logger& logger)
    : loop_(loop), stream_(std::move(stream)), logger_(logger) {}

TcpPipe::~TcpPipe() {
    // destroy() must have run and its close callback must have completed
    // before the pipe is deleted; the owner guarantees this by deleting from
    // the on_destroy callback.
}

int TcpPipe::connect(const std::string& host, uint16_t port,
                     std::function<void(int)> on_ready) {
    if (destroyed_) return UV_ECANCELED;

    tcp_ = new uv_tcp_t{};
    int rc = uv_tcp_init(loop_, tcp_);
    if (rc != 0) {
        delete tcp_;
        tcp_ = nullptr;
        return rc;
    }
    tcp_->data = this;
    uv_tcp_nodelay(tcp_, 1);

    struct sockaddr_in addr;
    rc = resolve_ip4(loop_, host, port, &addr);
    if (rc != 0) {
        logger_.error("Invalid local address " + host);
        destroy();
        return rc;
    }

    on_ready_ = std::move(on_ready);
    connect_req_.data = this;
    rc = uv_tcp_connect(&connect_req_, tcp_,
                        reinterpret_cast<const struct sockaddr*>(&addr), connect_cb);
    if (rc != 0) destroy();
    return rc;
}

int TcpPipe::adopt(uv_tcp_t* accepted) {
    if (destroyed_ || accepted == nullptr) return UV_EINVAL;
    tcp_ = accepted;
    tcp_->data = this;
    uv_tcp_nodelay(tcp_, 1);
    start_reading();
    return 0;
}

void TcpPipe::connect_cb(uv_connect_t* req, int status) {
    auto* self = static_cast<TcpPipe*>(req->data);
    if (self == nullptr) return;

    if (status != 0) {
        self->logger_.error(std::string("Local connect failed: ") + uv_strerror(status));
        if (self->on_ready_) self->on_ready_(status);
        self->destroy();
        return;
    }
    self->logger_.debug("Connected");
    self->start_reading();
    if (self->on_ready_) self->on_ready_(0);
}

void TcpPipe::start_reading() {
    if (reading_ || destroyed_ || tcp_ == nullptr) return;
    if (uv_read_start(reinterpret_cast<uv_stream_t*>(tcp_), alloc_cb, read_cb) == 0) {
        reading_ = true;
    }
}

void TcpPipe::stop_reading() {
    if (!reading_ || tcp_ == nullptr) return;
    uv_read_stop(reinterpret_cast<uv_stream_t*>(tcp_));
    reading_ = false;
}

void TcpPipe::resume_reading() {
    if (destroyed_) return;
    start_reading();
}

void TcpPipe::alloc_cb(uv_handle_t* handle, size_t suggested, uv_buf_t* buf) {
    auto* self = static_cast<TcpPipe*>(handle->data);
    // One scratch buffer per pipe — no per-read allocation, and no path that
    // can forget to free it.
    const size_t n = suggested < self->scratch_.size() ? suggested : self->scratch_.size();
    buf->base = self->scratch_.data();
    buf->len = static_cast<decltype(buf->len)>(n);
}

void TcpPipe::read_cb(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
    auto* self = static_cast<TcpPipe*>(stream->data);
    if (self == nullptr || self->destroyed_) return;

    if (nread == 0) return;  // EAGAIN

    if (nread < 0) {
        if (nread == UV_EOF) {
            // JS: loc.on('end', () => connection.end()) — half-close only.
            self->logger_.debug("Local end, ending connection");
            self->stop_reading();
            if (self->stream_.close) self->stream_.close();
        } else {
            self->logger_.debug(std::string("Local error: ") + uv_strerror(static_cast<int>(nread)));
            self->destroy();
        }
        return;
    }

    const auto len = static_cast<size_t>(nread);

    // Do NOT infer backpressure from the return value: hyperdht returns 0 on
    // every successful write (it drops udx's drained bit), so `rc == 0` would
    // mean "stop after every chunk and wait a round trip for the ack".
    // rc < 0 is an error; anything else means submitted.
    self->stream_pending_bytes_ += len;
    const int rc = self->stream_.write(
        reinterpret_cast<const uint8_t*>(buf->base), len,
        [self, len] {
            self->stream_pending_bytes_ -=
                (len < self->stream_pending_bytes_) ? len : self->stream_pending_bytes_;
            if (self->stream_pending_bytes_ < kStreamHighWaterMark) {
                self->resume_reading();
            }
        });

    if (rc < 0) {
        self->stream_pending_bytes_ -=
            (len < self->stream_pending_bytes_) ? len : self->stream_pending_bytes_;
        self->destroy();
        return;
    }
    if (self->stream_pending_bytes_ >= kStreamHighWaterMark) {
        // Genuinely too much unacked data in flight — stop reading until
        // enough drains. Note the drain callback is skipped entirely if the
        // stream closes first (hyperdht ffi_stream.cpp:219), so destroy() must
        // never wait on stream_pending_bytes_ reaching zero.
        self->stop_reading();
    }
}

void TcpPipe::on_stream_data(const uint8_t* data, size_t len) {
    if (destroyed_ || tcp_ == nullptr || closing_) return;
    if (len == 0) return;

    auto* ctx = new WriteCtx{this,
                             std::vector<char>(reinterpret_cast<const char*>(data),
                                               reinterpret_cast<const char*>(data) + len),
                             len};
    auto* req = new uv_write_t{};
    req->data = ctx;

    uv_buf_t out = uv_buf_init(ctx->data.data(), static_cast<unsigned>(ctx->data.size()));
    const int rc = uv_write(req, reinterpret_cast<uv_stream_t*>(tcp_), &out, 1, write_cb);
    if (rc != 0) {
        delete ctx;
        delete req;
        destroy();
        return;
    }

    inflight_writes_++;
    pending_bytes_ += len;
    if (!stream_paused_ && pending_bytes_ >= kHighWaterMark) {
        stream_paused_ = true;
        if (stream_.pause) stream_.pause();
    }
}

void TcpPipe::write_cb(uv_write_t* req, int status) {
    auto* ctx = static_cast<WriteCtx*>(req->data);
    TcpPipe* self = ctx->pipe;
    const size_t bytes = ctx->bytes;
    delete ctx;
    delete req;

    if (self == nullptr) return;
    self->inflight_writes_--;
    self->pending_bytes_ -= (bytes < self->pending_bytes_) ? bytes : self->pending_bytes_;

    if (status != 0) {
        self->destroy();
        return;
    }
    if (self->stream_paused_ && self->pending_bytes_ < kHighWaterMark) {
        self->stream_paused_ = false;
        if (self->stream_.resume) self->stream_.resume();
    }
    self->flush_finished();
}

void TcpPipe::on_stream_close() {
    if (destroyed_) return;
    logger_.debug("Connection end, ending local");
    stream_closed_ = true;
    flush_finished();
}

// Tear down once the remote side is finished AND everything it sent has been
// handed to the kernel. Without the inflight check the last response of a
// short-lived request would be dropped.
void TcpPipe::flush_finished() {
    if (!stream_closed_ || inflight_writes_ > 0) return;
    destroy();
}

void TcpPipe::destroy() {
    if (destroyed_) return;
    destroyed_ = true;

    stop_reading();
    if (stream_.close) stream_.close();

    if (tcp_ != nullptr && !closing_) {
        closing_ = true;
        if (uv_is_closing(reinterpret_cast<uv_handle_t*>(tcp_)) == 0) {
            uv_close(reinterpret_cast<uv_handle_t*>(tcp_), closed_cb);
            return;  // closed_cb runs on_destroy_
        }
    }
    // No handle to wait for.
    if (on_destroy_) {
        auto cb = on_destroy_;
        on_destroy_ = nullptr;
        cb();
    }
}

void TcpPipe::closed_cb(uv_handle_t* handle) {
    auto* self = static_cast<TcpPipe*>(handle->data);
    delete reinterpret_cast<uv_tcp_t*>(handle);
    if (self == nullptr) return;
    self->tcp_ = nullptr;
    // Never `delete this` here — the owner does that from on_destroy_.
    if (self->on_destroy_) {
        auto cb = self->on_destroy_;
        self->on_destroy_ = nullptr;
        cb();
    }
}

// ---------------------------------------------------------------------------
// TcpProxy
// ---------------------------------------------------------------------------

TcpProxy::TcpProxy(uv_loop_t* loop, Logger& logger) : loop_(loop), logger_(logger) {}

TcpProxy::~TcpProxy() { close(); }

int TcpProxy::listen(const std::string& host, uint16_t port,
                     std::function<void(uv_tcp_t*)> on_connection) {
    handle_ = new uv_tcp_t{};
    int rc = uv_tcp_init(loop_, handle_);
    if (rc != 0) {
        delete handle_;
        handle_ = nullptr;
        return rc;
    }
    handle_->data = this;
    on_connection_ = std::move(on_connection);

    struct sockaddr_in addr;
    rc = resolve_ip4(loop_, host, port, &addr);
    if (rc != 0) { close(); return rc; }

    rc = uv_tcp_bind(handle_, reinterpret_cast<const struct sockaddr*>(&addr), 0);
    if (rc != 0) { close(); return rc; }

    rc = uv_listen(reinterpret_cast<uv_stream_t*>(handle_), 128, connection_cb);
    if (rc != 0) { close(); return rc; }

    struct sockaddr_in bound{};
    int len = sizeof(bound);
    if (uv_tcp_getsockname(handle_, reinterpret_cast<struct sockaddr*>(&bound), &len) == 0) {
        bound_port_ = ntohs(bound.sin_port);
    }
    return 0;
}

void TcpProxy::connection_cb(uv_stream_t* server, int status) {
    auto* self = static_cast<TcpProxy*>(server->data);
    if (self == nullptr || status != 0) return;

    auto* client = new uv_tcp_t{};
    if (uv_tcp_init(self->loop_, client) != 0) { delete client; return; }
    if (uv_accept(server, reinterpret_cast<uv_stream_t*>(client)) != 0) {
        uv_close(reinterpret_cast<uv_handle_t*>(client),
                 [](uv_handle_t* h) { delete reinterpret_cast<uv_tcp_t*>(h); });
        return;
    }
    // Ownership transfers to the callback.
    if (self->on_connection_) self->on_connection_(client);
}

void TcpProxy::close() {
    if (handle_ == nullptr) return;
    uv_tcp_t* h = handle_;
    handle_ = nullptr;
    h->data = nullptr;
    if (uv_is_closing(reinterpret_cast<uv_handle_t*>(h)) == 0) {
        uv_close(reinterpret_cast<uv_handle_t*>(h),
                 [](uv_handle_t* x) { delete reinterpret_cast<uv_tcp_t*>(x); });
    }
}

}  // namespace holesail
