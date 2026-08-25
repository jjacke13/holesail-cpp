#include "holesail/addr.hpp"

#include <sys/socket.h>

#include <cstdint>
#include <cstring>

namespace holesail {

int resolve_ip4(uv_loop_t* loop, const std::string& host, int port,
                struct sockaddr_in* out) {
    if (out == nullptr || loop == nullptr) return UV_EINVAL;

    // Fast path: anything numeric never touches the resolver.
    if (uv_ip4_addr(host.c_str(), port, out) == 0) return 0;

    struct addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    // Only narrows the duplicate entries getaddrinfo would otherwise return
    // once per socket type; the address itself is the same for UDP, which is
    // why UdpProxy shares this helper.
    hints.ai_socktype = SOCK_STREAM;

    uv_getaddrinfo_t req;
    std::memset(&req, 0, sizeof(req));
    const int rc = uv_getaddrinfo(loop, &req, nullptr, host.c_str(), nullptr, &hints);
    if (rc != 0) return rc;
    if (req.addrinfo == nullptr) return UV_EAI_NONAME;

    // First A record wins — the same one connect(2) would pick for the name.
    std::memcpy(out, req.addrinfo->ai_addr, sizeof(*out));
    uv_freeaddrinfo(req.addrinfo);

    // getaddrinfo was asked for the name only, so it filled in port 0.
    out->sin_port = htons(static_cast<uint16_t>(port));
    return 0;
}

}  // namespace holesail
