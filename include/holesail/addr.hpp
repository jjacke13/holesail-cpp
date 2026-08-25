#pragma once

#include <uv.h>

#include <netinet/in.h>
#include <string>

namespace holesail {

// Fill `out` with the IPv4 address for host:port.
//
// This exists because `uv_ip4_addr()` is inet_pton-style: it parses a numeric
// dotted-quad and nothing else, returning UV_EINVAL for a name. JS holesail
// accepts names — node's `net.createServer().listen(port, host)` and
// `net.connect(port, host)` both resolve — so a JS server that published
// `{"host":"localhost"}` in its DHT record made every C++ client here die with
// a bare "Error: invalid argument" before it ever reached the tunnel.
//
// Numeric hosts keep the zero-cost path; only a name reaches getaddrinfo.
//
// That lookup is deliberately SYNCHRONOUS (uv_getaddrinfo with a NULL
// callback), so it blocks the loop for its duration. The realistic non-numeric
// host is "localhost", which /etc/hosts answers without touching the network,
// and the one per-connection caller (TcpPipe dialling the local origin) points
// at a service on the same box. If a deployment ever aims --host at a name
// needing real DNS, this is the single place to make it async.
int resolve_ip4(uv_loop_t* loop, const std::string& host, int port,
                struct sockaddr_in* out);

}  // namespace holesail
