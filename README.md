# holesail-cpp

[![CI](https://github.com/jjacke13/holesail-cpp/actions/workflows/ci.yml/badge.svg)](https://github.com/jjacke13/holesail-cpp/actions/workflows/ci.yml)

A C++20 reimplementation of [holesail](https://github.com/holesail/holesail) 2.4.1 — the
peer-to-peer TCP/UDP proxy — on top of [hyperdht-cpp](https://github.com/jjacke13/hyperdht-cpp)
instead of Node.js.

Expose a local port to the internet without a public IP, port forwarding, or a relay
server: one side runs `holesail --live <port>`, the other runs `holesail --connect <key>`,
and the traffic goes peer-to-peer over the public HyperDHT, hole-punched and encrypted.

It is **wire-compatible with the JavaScript original**, which is the whole point of the
port: same connection strings, same key derivation, same DHT records. A C++ server accepts
an unmodified JS `holesail --connect`, and a C++ client connects to an unmodified JS
`holesail --live`. `scripts/cross-test.sh` proves that against the real 2.4.1 binary on the
public DHT, in both directions, in secure and public mode — and the **official Holesail
phone app** connects to a C++ server over mobile data, across carrier NAT.

## The original Holesail

This is an unofficial, independent port. All of the protocol design, the connection-string
scheme, the DHT record format and the product itself are the work of the upstream Holesail
project — this repo only reimplements the client and server in C++.

| | |
|---|---|
| Website | <https://holesail.io> — "Open Source P2P Reverse Proxy" |
| Source | <https://github.com/holesail/holesail> |
| npm | [`holesail`](https://www.npmjs.com/package/holesail) — `npm i -g holesail` |
| Author | supersuryaansh, and the Holesail contributors |
| License | AGPL-3.0 |
| Built on | [Pear](https://pears.com) / [HyperDHT](https://github.com/holepunchto/hyperdht) by Holepunch |

**Use the original if you want the supported thing.** It has features this port does not
(`--filemanager`, the desktop and mobile apps, the QR-code pairing flow), it is what the
Holesail team maintains, and it runs anywhere Node does.

This port exists for one reason: to run the same protocol on hardware where a Node runtime
is too heavy — see [Footprint](#footprint) below, where it uses **24× less memory** than
the original on the same Raspberry Pi. Because the wire format is identical, the two
interoperate freely: put this on the constrained end and the official client, app or CLI
on the other.

Bugs in *this* port belong here. Bugs in the protocol, the apps, or the JS implementation
belong upstream — please do not send those to the Holesail team as if they were ours, or
ours to them.

## Relationship to the JS holesail and to hyperdht-cpp

| | |
|---|---|
| **holesail 2.4.1 (JS)** | The reference implementation. Every behaviour here was derived by reading its source, and every divergence is listed below. Runtime: Node.js + `hyperdht`. |
| **holesail-cpp** | This repo. A static library (`libholesail_lib.a`) plus a thin CLI (`holesail`). No Node, no JSON library, no CLI parser, no QR encoder — libsodium, libuv and hyperdht only. |
| **hyperdht-cpp** | The transport. holesail-cpp talks to it through its C API (`<hyperdht/hyperdht.h>`) and drives local sockets with `uv_tcp_t` / `uv_udp_t` on the DHT's own libuv loop. Single-threaded, callback-driven, no polling, no threads. |

`libuv MUST be 1.51.x.` 1.52.0/1.52.1 carry a UDP `POLLERR` regression that silently wedges
libudx streams on real NAT paths; the flake pins nixos-25.11 for that reason.

## Footprint

Measured on the same Raspberry Pi 5, both idle, both `--live 8000 --public` with the same
`--key`. holesail-cpp is the aarch64 fully-static build; the JS side is the real
`holesail` 2.4.1 on Node 24.

| | holesail-cpp | holesail 2.4.1 (JS) | |
|---|---|---|---|
| Resident (`VmRSS`) | **3.2 MB** | 77.8 MB | **24× smaller** |
| Peak ever (`VmHWM`) | 3.8 MB | 77.9 MB | 20× |
| Real heap (`Pss_Anon`) | **0.7 MB** | 27.9 MB | **40× smaller** |
| Virtual (`VmSize`) | 3.5 MB | 1.17 GB | 334× |
| Threads | **1** | 7 | |

`Pss_Anon` is the number that matters: **0.7 MB of genuinely dynamic memory**. The rest of
the C++ figure is the 3.4 MB static binary paged in — file-backed and evictable, so the
kernel reclaims it under pressure. One thread runs the DHT node, the holepunch machinery
and the socket bridge on a single libuv loop; V8 brings GC and worker threads regardless.
The 1.17 GB virtual is V8 reserving address space — harmless on 64-bit, fatal on 32-bit ARM.

For reference, the Python port of holesail on the same box sits at **20.9 MB RSS**,
1 thread — between the two, as an interpreter with no JIT should.

`nix build .#holesail-aarch64-static` and `.#holesail-x86_64-static` produce binaries you
can `scp` onto any aarch64 or x86-64 Linux box — Pi Zero 2 W upward — with **zero runtime
dependencies**: no nix, no Node, no libuv or libsodium needed on the target. Both are
attached to every [release](https://github.com/jjacke13/holesail-cpp/releases), so the
install is `curl` + `chmod +x`.

## Build

### Nix (what CI and the cross-test use)

```bash
nix develop                                      # cmake, ninja, libsodium, libuv, gtest,
                                                 # hyperdht-cpp, plus python3/curl/node
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
cd build && ctest --output-on-failure            # 12 suites, no network needed
```

Packages:

```bash
nix build .#default                    # result/bin/holesail for the host
nix build .#holesail-aarch64-static    # aarch64 musl, fully static — scp onto a Pi, run it
nix build .#holesail-x86_64-static     # x86-64 musl, fully static — same deal on a VPS
```

### CI

Every push and PR runs `.github/workflows/ci.yml`: the 12 suites in Release **and**
under ASan/UBSan, both fully-static binaries built and checked (right machine, really
static, no dynamic deps), and a guard that the four version strings agree with each
other and with the tag. The sanitizer lane is the one that matters most — the crash
that bit this project in the field only appeared in Release, where libudx compiles its
guarding assert out.

`scripts/cross-test.sh` is deliberately **not** a merge gate: it needs the public DHT
and will flake. Run it before a release.

Tagging `v*` runs `.github/workflows/release.yml`, which rebuilds both statics, refuses
to publish if the tag disagrees with `src/cli.cpp`, and attaches them with a combined
`SHA256SUMS.txt`. Note the aarch64 binary is cross-compiled and never executed in CI —
only real hardware proves that one runs.

### Plain CMake

Needs CMake ≥ 3.20, a C++20 compiler, and these on `PKG_CONFIG_PATH` / `CMAKE_PREFIX_PATH`:
`libsodium`, `libuv` (1.51.x), `hyperdht` (from hyperdht-cpp, which installs both
`hyperdht-config.cmake` and `hyperdht.pc`), and GoogleTest if you want the tests.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release   # add -DHOLESAIL_BUILD_TESTS=OFF to skip gtest
cmake --build build
```

After bumping the hyperdht dependency, **delete `build/` and reconfigure** —
`CMakeCache.txt` pins the resolved library path, so an incremental `cmake --build` keeps
linking the old one.

## CLI

```
holesail --live <port>    [--host H] [--port P] [--udp] [--public] [--key K] [--log[=N]]
holesail --connect <key>  [--host H] [--port P] [--udp] [--public] [--log[=N]]
holesail <key>            (same as --connect)
holesail --lookup <key>
holesail --help [live|connect]
holesail --version
```

### Share a local web server, privately

```bash
$ holesail --live 8080
Holesail TCP Server Started ⛵️
Connection Mode: Private Connection String
Holesail is now listening on 127.0.0.1:8080
Connect with key: hs://s0009f3c…64-hex-chars…b1
   NOTE: TREAT PRIVATE CONNECTION STRINGS HOW YOU WOULD TREAT SSH KEY, …
```

With no `--key`, a random 32-byte key is generated and printed as 64 hex characters. The
connection string is `hs://s000` + that key. Anyone with the string can derive the server's
keypair — that is exactly what the firewall checks, so treat it like a private key.

On the other machine:

```bash
$ holesail --connect hs://s0009f3c…b1
Holesail TCP Client Started ⛵️
Connection Mode: Private Connection String
Access application on http://127.0.0.1:8080/
Connected to key: hs://s0009f3c…b1
```

With no `--port`/`--host` the client binds locally to whatever the **server** advertised in
its DHT record — here 127.0.0.1:8080. Override with `--port 3000 --host 0.0.0.0`.

### Reuse a key you choose

```bash
holesail --live 8080 --key "this-is-a-32-char-holesail-key!!"
holesail --connect "hs://s000this-is-a-32-char-holesail-key!!"
```

The key must be at least 32 characters (`--force` overrides). The seed is
`SHA-256(key)` over the key **string's ASCII bytes**, so the same key always yields the
same keypair, on any machine, in either implementation.

### Public mode

```bash
$ holesail --live 8080 --public
Connection Mode: Public Connection String
Connect with key: hs://0000ochrpsr7qboxhn7huhg7diti5ah1i37y6du4yhni7pyot4wnz8qy
```

Public mode hands out `hs://0000` + z-base-32 of the **public key**. Knowing it lets anyone
connect but not impersonate the server; there is no firewall in this mode. `--public` on the
client side forces public mode for a bare (unprefixed) key — see D3 below.

With no `--key` the keypair is random, so the address changes on every restart. Pass `--key`
to make it stable:

```bash
holesail --live 8080 --public --key "my-super-secret-holesail-key-abc"
```

That gives a **permanent public address whose secret never leaves the machine** — unlike
private mode, where the connection string *is* the key. Lose the key and you lose the
address. Mind quirk Q2 below: if the key's 6th character is `s`, `--public` is silently
ignored and you get a private server.

### UDP

```bash
holesail --live 53 --udp --host 127.0.0.1     # expose a local DNS resolver
holesail --connect hs://s000… --udp --port 5353
```

Datagrams are framed over the reliable stream as `[uint32 big-endian length][payload]`. The
client side keeps an `addr:port → stream` map so several local UDP clients multiplex; the
server side owns one local UDP socket per stream.

### Look up a connection string without connecting

```bash
$ holesail --lookup hs://s000this-is-a-32-char-holesail-key!!
Holesail Lookup Result 🔍
Host: 127.0.0.1
Port: 8080
Protocol: TCP
Private: Yes
```

### Logging

`--log` enables INFO; `--log <0..3>` sets the level (0 DEBUG, 1 INFO, 2 WARN, 3 ERROR).
Format is `holesail-logger`'s, verbatim:
`<ISO8601> [Holesail] [INFO] <message>`. DEBUG/INFO go to stdout, WARN/ERROR to stderr.

### Validation

| Condition | Result | Exit |
|---|---|---|
| `--key` with no value | `Error: Key can not be empty` | **0** (upstream quirk Q3, reproduced) |
| `--key` shorter than 32 chars without `--force` | minimum-length error | 2 |
| `--connect` *and* a positional key | two-connection-strings error | 2 |
| `--live` / `--port` not a number | invalid-port error | 2 |
| `--udp` with host `localhost` or `0.0.0.0` | warning, run continues | — |
| `--filemanager` | `Error: --filemanager is not supported by holesail-cpp` | 2 |

## Library API

`include/holesail/holesail.hpp` is the whole public surface. The rest of the headers
(`keys.hpp`, `record.hpp`, `pipe.hpp`, `udp_pipe.hpp`, `server.hpp`, `client.hpp`,
`logger.hpp`) are usable directly if you want a piece rather than the facade.

```cpp
namespace holesail {

struct Options {                 // what the CLI collects
    bool server = false, client = false;
    std::optional<uint16_t> port;
    std::optional<std::string> host, key;
    std::optional<bool> secure;  // unset = sniff it from the hs:// url
    std::optional<bool> udp;
    int log_level = -1;          // -1 disables logging
};

struct Info {                    // recomputed on every info() call
    std::string type, state, host, protocol, seed, key, url, public_key;
    bool secure; uint16_t port;
};

struct LookupResult { std::string host; uint16_t port; std::string protocol; bool secure; };
struct OptionsError { std::string message; };            // thrown by the constructor only

// Pure helpers — no I/O, usable to precompute a connection string.
struct Derived { bool secure; std::string key; std::string seed_hex; };
Derived derive(const Options&);
std::string make_url(bool secure, std::string_view key);

class Holesail {
public:
    Holesail(uv_loop_t* loop, Options options);          // throws OptionsError
    int  ready(std::function<void(int err)> cb);         // creates the DHT, listens/connects
    void pause();
    void resume();
    void close();
    const Info& info() const;

    static int lookup(uv_loop_t*, std::string_view url,
                      std::function<void(int err, const std::optional<LookupResult>&)> cb);
};

}  // namespace holesail
```

### Embedding

```cpp
#include <holesail/holesail.hpp>
#include <uv.h>
#include <cstdio>

int main() {
    uv_loop_t loop;
    uv_loop_init(&loop);

    holesail::Options options;
    options.server = true;
    options.port   = 8080;                                  // the local service to expose
    options.host   = "127.0.0.1";
    options.key    = "this-is-a-32-char-holesail-key!!";     // omit for a random one
    options.secure = true;

    holesail::Holesail tunnel(&loop, options);              // throws holesail::OptionsError
    tunnel.ready([&tunnel](int err) {
        if (err < 0) { std::printf("listen failed: %d\n", err); return; }
        std::printf("connect with %s\n", tunnel.info().url.c_str());
    });

    uv_run(&loop, UV_RUN_DEFAULT);

    tunnel.close();                    // only *schedules* the DHT teardown…
    uv_run(&loop, UV_RUN_DEFAULT);     // …so run the loop once more to complete it
    uv_loop_close(&loop);
}
```

The `uv_loop_t` is borrowed and must outlive the object, and `close()` is safe to call from
inside a libuv callback (the CLI calls it from its SIGINT handler) — which is why it defers
the DHT free onto the loop instead of doing it inline.

`uv_loop_close()` can still return `EBUSY`: hyperdht-cpp leaves one stopped-but-unclosed
timer per DHT. The CLI works around it by `uv_walk`-closing whatever is left before
`uv_loop_close()`; see `drain_and_close()` in `src/cli.cpp`.

Linking — the install ships `libholesail_lib.a`, the headers, and a CMake package
config, so either form works:

```cmake
# Against an installed holesail (nix build .#default, or cmake --install)
find_package(holesail REQUIRED)
target_link_libraries(my_app PRIVATE holesail::holesail_lib)
```

```cmake
# Or straight from a source checkout
add_subdirectory(holesail-cpp)
target_link_libraries(my_app PRIVATE holesail_lib)   # brings hyperdht, libsodium, libuv along
```

## Divergences from the JS implementation

| # | Divergence | Rationale |
|---|---|---|
| D1 | `--filemanager` is **not implemented** | It requires porting `livefiles`, an HTTP file browser with auth and roles. `--filemanager` prints an explicit error and exits 2 rather than pretending. |
| D2 | **No `cli-box` output box and no terminal QR code** | Would need a vendored QR encoder and box-drawing logic for no functional gain. The wording and the colours are identical; only the frame around them is gone. |
| D3 | Client `--public` now means public mode | Bug fix — see Q1. |
| D4 | Remote→local backpressure uses explicit stream pause/resume | JS gets it free from Node stream semantics; C++ needed two new hyperdht-cpp entry points (`hyperdht_stream_pause` / `_resume`). Same observable behaviour. |
| D5 | UDP frames are capped at 65535 bytes; an oversized length header destroys the stream | JS buffers toward an unbounded 32-bit length. No legitimate UDP datagram exceeds 65507 bytes, so real traffic is unaffected. In C++ this is a memory-safety issue, not just an allocation-size one: `4 + frame_len` evaluates in `unsigned int`, so `0xFFFFFFFF + 4` wraps to `3`, bypasses the "frame incomplete" guard and hands the frame callback a 4 GiB out-of-bounds read. The cap is what prevents that; the addition is widened to `size_t` as well so the guard stays sound if the cap is ever raised. |

## Reproduced upstream quirks

**Q1 — fixed, not reproduced (this is D3).** `bin/holesail.mjs` passes `secure: argv.public`
on the client path, so JS `holesail --connect <bare-key> --public` sets `secure = true` —
the opposite of what its own `--help` documents. The C++ port uses `secure = !argv.public`.
This only changes behaviour for bare (unprefixed) keys combined with `--public`; every
`hs://`-prefixed flow is unaffected, because the URL sniff takes precedence.

**Q2 — reproduced.** The `secure` sniff is `url[5] == 's'` and runs on *any* string, prefixed
or not, so a bare key whose 6th character is `s` reads as secure even under `--public`. It
falls straight out of the shared parser; special-casing it would be a bigger divergence than
the bug.

**Q3 — reproduced.** `--key` with no value exits with status **0**, unlike every other
validation failure, which exits 2.

**Q4 — reproduced.** An `hs://s000…` URL always wins over `--public`: the flag cannot force
a secure link into public mode.

## Testing

```bash
# 81 unit cases, no network needed. The 4 loopback tunnel tests SKIP unless opted in.
nix develop --command bash -c "cd build && ctest --output-on-failure"

# Same suites plus the 4 real-DHT loopback tunnels (~14 s).
nix develop --command bash -c "cd build && HOLESAIL_NETWORK_TESTS=1 ctest --output-on-failure"

# The acceptance gate: live interop against the real JS binary.
nix develop --command ./scripts/cross-test.sh ./build/holesail
```

Under sanitizers, pass the suppression file. It names only hyperdht-cpp frames, so a leak
introduced *here* still fails the run:

```bash
LSAN_OPTIONS=suppressions=$PWD/test/lsan.supp HOLESAIL_NETWORK_TESTS=1 ctest
```

`scripts/cross-test.sh` is the acceptance gate: it starts a real HTTP origin, tunnels it,
and curls it back through five live cases on the public DHT — C++↔C++ secure (the baseline),
C++ server ↔ JS client and JS server ↔ C++ client in secure *and* public mode — plus
`--lookup` against a record a JS server published. Each case takes about a minute; the waits
are tunable with `HOLESAIL_SERVER_WAIT` / `HOLESAIL_CLIENT_WAIT`, and `HOLESAIL_JS` points at
another JS holesail.

## License

AGPL-3.0-or-later, matching upstream holesail. See `LICENSE`. hyperdht-cpp is LGPL-3.0,
which is compatible as a dependency.
