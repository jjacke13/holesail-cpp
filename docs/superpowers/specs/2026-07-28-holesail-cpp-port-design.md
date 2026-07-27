# holesail-cpp — C++ port of holesail

**Date:** 2026-07-28
**Status:** Approved
**Reference implementation:** [holesail](https://github.com/holesail/holesail) v2.4.1 (JS)
**Transport:** [hyperdht-cpp](../../../../hyperdht-cpp) v0.3.0 (C API)

---

## 1. Goal

A lightweight C++ reimplementation of holesail — the TCP/UDP peer-to-peer proxy — on
top of hyperdht-cpp instead of Node.js + `hyperdht`.

**Success criterion:** a C++ holesail server is connectable by an unmodified JS
`holesail --connect`, and a C++ holesail client can connect to an unmodified JS
`holesail --live`. Same connection strings, same defaults, same DHT records.

Non-goals: `--filemanager` (the `livefiles` HTTP file browser), the `cli-box`
output box, the terminal QR code, and the Pear/bare runtime integration.

---

## 2. Reference behavior (the part that must be byte-exact)

All of this is derived from reading holesail 2.4.1 and its deps:
`src/index.js`, `src/bin/holesail.mjs`, `src/lib/validateInput.js`,
`holesail-server/index.js`, `holesail-client/index.js`,
`@holesail/hyper-cmd-lib-net/index.js`, `hyper-cmd-lib-keys`, `z32`,
`holesail-logger`.

### 2.1 URL parsing

```
parse(url):
    if url starts with "hs://" and len(url) >= 9:  key = url[9:]
    else:                                          key = url
    secure = (url[5] == 's')          # applied to ANY url, prefixed or not
    # secure is "unset" when url is empty
```

The `secure` sniff runs on the raw string even when there is no `hs://` prefix.
That is upstream behavior and is **reproduced** (see §9, quirk Q2).

### 2.2 Server key derivation

```
secure = (--public given) ? false : true        # default: secure

if --key K given:
    key  = K
    seed = SHA256(key as ASCII bytes)           # 32 bytes  <-- hash of the STRING
else if secure:
    key  = lowercase_hex(random 32 bytes)       # 64 ASCII chars
    seed = SHA256(key as ASCII bytes)
else:                                           # public mode, no --key
    key  = <unset>
    seed = random 32 bytes

keypair = hyperdht_keypair_from_seed(seed)
```

Critical detail: the seed is `SHA256` of the key **string's ASCII bytes**, not of
the hex-decoded value. `SHA256("a3f1…")` over 64 ASCII characters.

Connection string:

| Mode | String |
|---|---|
| secure | `hs://s000` + `key` (the raw key string, **not** z32) |
| public | `hs://0000` + `z32(keypair.public_key)` |

Firewall (secure mode only): reject any connection whose
`remote_public_key != keypair.public_key`. Only a peer that knows the key can
derive the same keypair, so this is the access control.

`reusable_socket` is enabled on the server.

### 2.3 Client key derivation

```
(key, url_secure) = parse(connection_string)
secure = url_secure if set, else (--public given ? false : unset→false)

if secure:
    keypair    = hyperdht_keypair_from_seed(SHA256(key))
    target_pk  = keypair.public_key
    own_identity = keypair            # <-- same keypair, this is how the firewall passes
else:
    target_pk  = z32_decode(key)
    own_identity = random
```

In secure mode the client **adopts the server's keypair as its own DHT identity**.
That is the mechanism by which it satisfies the server's firewall.

### 2.4 The DHT metadata record

Server publishes, on `keypair`:

```
mutable_put(keypair, '{"host":"127.0.0.1","udp":true,"port":8080}')
```

- Key order is exactly `host`, `udp`, `port`.
- `udp` is **omitted entirely** when the flag was not passed — JS `JSON.stringify`
  drops `undefined` values, so the common record is `{"host":"127.0.0.1","port":8080}`.
  The C++ writer must omit it too, and the reader must tolerate its absence.
- `port` is a JSON number, `host` a JSON string.
- Refresh: re-put every `50 * 60 * 1000` ms = 3,000,000 ms.
- Sequence rule: read the latest record first;
  `seq = (old.value == new.value) ? old.seq : old.seq + 1`. No prior record → default seq.

Client reads it and applies defaults:

```
port = --port  ?? record.port ?? 8989
host = --host  ?? record.host ?? "127.0.0.1"
udp  = --udp   ?? record.udp  ?? false
```

Note the consequence: with no `--host`/`--port`, the client binds locally to the
host and port the **server** advertised.

### 2.5 `--lookup`

```
(key, secure) = parse(url)
if secure:  arg = z32(SHA256(key))
else:       arg = key            # must z32-decode, else error "Invalid key format: <key>"

pk = keypair_from_seed(z32_decode(arg)).public_key
record = mutable_get(pk, latest)          # try secure interpretation first
if not record:
    record = mutable_get(z32_decode(arg), latest)   # then public interpretation

print Host / Port / Protocol / Private
```

### 2.6 Data plane

**TCP, server side** — per inbound DHT connection, open a TCP connection to
`host:port` and pipe bidirectionally. Half-close is propagated in both directions
(`end` → `end`). Any error or close on either side destroys both.

**TCP, client side** — listen on `host:port`; every accepted socket gets its own
DHT stream to `target_pk`.

**UDP, both sides** — datagrams are framed over the reliable stream as
`[uint32 big-endian length][payload]`. The client-side proxy keeps a
`"addr:port" → stream` map so multiple local UDP clients multiplex; the
server side owns one local UDP socket per stream.

**Backpressure** — local→remote: write to the stream; on backpressure stop reading
the local socket and resume on drain. remote→local: pause the DHT stream while the
local socket is congested, resume when it drains. Mirrors the JS
`write() || (pause(), once('drain', resume))` pattern in both directions.

### 2.7 Logging

`holesail-logger` format, reproduced verbatim:

```
<ISO8601 timestamp> [Holesail] [LEVEL] <message>
```

Levels `0=DEBUG 1=INFO 2=WARN 3=ERROR`; messages below `min_level` are dropped.
`--log` → level 1. `--log N` → clamp N to 0..3. DEBUG/INFO → stdout, WARN → stderr,
ERROR → stderr. Colors: timestamp bright-black, `[Holesail]` blue, level
bright-black/green/yellow/red by severity.

---

## 3. Architecture

```
holesail-cpp/
├── CMakeLists.txt
├── flake.nix / flake.lock
├── LICENSE                      AGPL-3.0 (matches upstream holesail)
├── README.md
├── CLAUDE.md
├── include/holesail/
│   ├── holesail.hpp             Holesail facade — mirrors JS index.js
│   ├── server.hpp               HolesailServer
│   ├── client.hpp               HolesailClient
│   ├── keys.hpp                 z32, SHA256, url parse, seed derivation
│   ├── json.hpp                 minimal object reader/writer for the record
│   ├── pipe.hpp                 TCP piper + TCP proxy
│   ├── udp_pipe.hpp             framed UDP proxy + framed UDP server
│   └── logger.hpp               holesail-logger equivalent
├── src/                         one .cpp per header
├── app/main.cpp                 argv parse, validation, terminal output
└── test/                        GoogleTest
```

Build artifacts: `libholesail` (static) + `holesail` (executable). Headers are
installed so other projects in the workspace can embed the tunnel.

### 3.1 Key decisions

**Use the hyperdht C API, not the C++ headers.** `hyperdht.h` symbols are always
exported; the C++ classes are hidden in Release builds unless
`HYPERDHT_EXPORT_CXX=ON`. Both `examples/cpp/*` and the Python port already use
the C API — follow the house pattern.

**Local sockets are `uv_tcp_t` / `uv_udp_t` on the DHT's own loop.** Single
threaded, no polling, no raw file descriptors. (The Python port had to use
`hyperdht_poll_start` with raw sockets because it cannot touch libuv handles;
C++ has no such constraint.)

**SHA256 comes from libsodium** (`crypto_hash_sha256`), already linked
transitively through hyperdht.

**No JSON dependency.** The record is a flat object with three keys of known
type. A ~60-line reader/writer beats pulling in nlohmann (25k lines) or picojson
for this.

**z32 is ~60 lines** ported from `z32/index.js` — encode and decode, alphabet
`ybndrfg8ejkmcpqxot1uwisza345h769`.

**Error codes, not exceptions,** in the data plane — matches hyperdht-cpp house
style and keeps embedded targets viable. Exceptions are acceptable in
construction/validation paths that run once at startup.

### 3.2 Public API sketch

```cpp
namespace holesail {

struct Options {
    bool server = false;
    bool client = false;
    std::optional<uint16_t> port;
    std::optional<std::string> host;
    std::optional<std::string> key;
    std::optional<bool> secure;      // unset == "sniff from the url"
    bool udp = false;
    int log_level = -1;              // -1 == disabled
};

struct Info {
    std::string type;                // "server" | "client"
    std::string state;               // "listening" | "waiting" | "paused" | "destroyed"
    bool secure;
    uint16_t port;
    std::string host;
    std::string protocol;            // "tcp" | "udp"
    std::string seed;                // hex
    std::string key;
    std::string url;                 // hs://…
    std::string public_key;          // z32
};

class Holesail {
public:
    Holesail(uv_loop_t* loop, Options opts);
    int  ready(std::function<void(int err)> cb);   // == JS ready()
    void pause();
    void resume();
    void close();
    const Info& info() const;

    static int lookup(uv_loop_t*, const std::string& url,
                      std::function<void(int err, const LookupResult&)>);
};

}
```

---

## 4. Required additions to hyperdht-cpp

Two small changes land in the sibling repo before the data plane can be correct:

1. **`hyperdht_stream_pause()` / `hyperdht_stream_resume()`** — the C API currently
   pushes stream data through a callback with no way to apply backpressure.
   libudx already exposes `udx_stream_read_stop` / `udx_stream_read_start`, so
   this is a thin wrapper (~15 lines). Without it, a slow local application forces
   unbounded userspace buffering in the remote→local direction.

2. **`int reusable_socket` appended to `hyperdht_connect_opts_t`** — the C++
   `ConnectOptions` already has the field (`dht.hpp:245`); the C struct does not
   expose it, and JS `holesail-client` passes `reusableSocket: true` on every
   connect. The struct is documented as tail-extensible, so appending is the
   sanctioned move.

Both are additive and cannot break existing hyperdht-cpp consumers.

---

## 5. CLI surface

```
holesail --live <port>       [--host H] [--port P] [--udp] [--public] [--key K] [--log[=N]]
holesail --connect <key>     [--host H] [--port P] [--udp] [--public] [--log[=N]]
holesail <key>               (same as --connect)
holesail --lookup <key>
holesail --help [live|connect]
holesail --version
```

Validation, messages and exit codes reproduce `validateInput.js`:

| Condition | Message | Exit |
|---|---|---|
| `--key` with no value | `Error: Key can not be empty` | 0 (upstream quirk, reproduced) |
| `--udp` with host `localhost`/`0.0.0.0` | warning, continues | — |
| `--key` shorter than 32 chars without `--force` | minimum-length error | 2 |
| both `--connect` and a positional key | two-connection-strings error | 2 |
| `--live` not a number | invalid-port error | 2 |
| `--port` present but not a number | invalid-port error | 2 |

Output on success: same wording and colors as `stdout.js`, without the box and QR:

```
Holesail TCP Server Started
Connection Mode: Private Connection String
Holesail is now listening on 127.0.0.1:8080
Connect with key: hs://s000<key>
NOTE: TREAT PRIVATE CONNECTION STRINGS HOW YOU WOULD TREAT SSH KEY, …
```

Client variant uses `Access application on http://host:port/` and
`Connected to key:`. Public mode uses the yellow `Public Connection String` label
and the corresponding NOTICE text.

---

## 6. Testing

**Unit (GoogleTest)**

- z32 encode/decode round-trip, plus fixed vectors cross-checked against the JS `z32` module.
- URL parser: `hs://s000…`, `hs://0000…`, bare key, empty, short strings.
- Seed derivation: for a fixed key string, assert the C++ keypair public key equals
  the one JS produces (vector captured from `HyperDHT.keyPair(sha256(key))`).
- Record encode: assert `{"host":"127.0.0.1","port":8080}` when udp is unset and
  `{"host":"127.0.0.1","udp":true,"port":8080}` when set.
- Record parse: missing `udp`, extra unknown keys, malformed input.
- UDP framing: split frames across chunk boundaries, zero-length payload, a frame
  spanning several reads, a length header split across reads.
- Argument validation: each row of the table in §5.

**Integration** — loopback: echo server ← holesail server ← DHT ← holesail client
← test client. Covers TCP and UDP, half-close, and a payload large enough to
exercise backpressure in both directions.

**Live cross-test (the real acceptance test)**

- C++ `--live` ↔ JS `holesail --connect`, secure and public.
- JS `--live` ↔ C++ `--connect`, secure and public.
- `--lookup` against a record published by the JS server.

---

## 7. Build and packaging

`flake.nix` takes hyperdht-cpp as an input — `path:../hyperdht-cpp` while
developing, a GitHub ref for release. CMake consumes it with
`find_package(hyperdht)`; hyperdht-cpp already installs
`hyperdht-targets.cmake` and `hyperdht.pc`.

This also inherits hyperdht-cpp's aarch64 fully-static musl cross-build, so
`nix build .#holesail-aarch64-static` produces a dependency-free binary for
Raspberry Pi class hardware.

**libuv must be 1.51.x.** 1.52.0/1.52.1 carry a UDP `POLLERR` regression that
silently wedges libudx streams on real NAT paths. The flake pins nixos-25.11 for
this reason — see `hyperdht-cpp/docs/LIBUV-VERSION.md`.

**License: AGPL-3.0**, matching upstream holesail. hyperdht-cpp is LGPL-3.0,
which is compatible as a dependency.

---

## 8. Divergences from the JS implementation

| # | Divergence | Rationale |
|---|---|---|
| D1 | `--filemanager` is not implemented | Requires porting `livefiles`, an HTTP file browser with auth and roles. Out of scope. |
| D2 | No `cli-box` output box, no terminal QR code | Would need a vendored QR encoder and box-drawing logic for no functional gain. Same text, same colors. |
| D3 | Client `--public` now means public mode | Bug fix — see Q1 below. |
| D4 | Remote→local backpressure uses stream pause/resume | JS gets this from Node stream semantics; C++ needs the new hyperdht API in §4. Same observable behavior. |
| D5 | UDP frames are capped at 65535 bytes; an oversized length header destroys the stream | JS buffers toward an unbounded 32-bit length. No legitimate UDP datagram exceeds 65507 bytes, so real traffic is unaffected. **In C++ this is a memory-safety issue, not merely an allocation-size one** — confirmed by mutation-testing the cap out: `4 + frame_len` evaluates in `unsigned int`, so `0xFFFFFFFF + 4` wraps to `3`, bypasses the "frame incomplete" guard, and hands the frame callback a 4 GiB out-of-bounds read. The length check is what prevents it; the addition is additionally widened to `size_t` so the guard stays sound if the cap is ever raised. |

## 9. Reproduced upstream quirks

**Q1 — FIXED, not reproduced.** `bin/holesail.mjs` passes `secure: argv.public`
on the client path, so `holesail --connect <bare-key> --public` sets
`secure = true` — the opposite of what `--help` documents. The C++ port passes
`secure = !argv.public` instead. This only changes behavior for bare (unprefixed)
keys combined with `--public`; every `hs://`-prefixed flow is unaffected because
the URL sniff takes precedence.

**Q2 — reproduced.** The `secure` sniff (`url[5] == 's'`) runs on any string, so a
`--key` whose 6th character is `s` forces secure mode even under `--public`.
Reproduced because it falls directly out of the shared parser; special-casing it
would be a larger divergence than the bug.

**Q3 — reproduced.** `--key` with no value exits with status 0 rather than 2,
unlike every other validation failure.

**Q4 — reproduced.** A `hs://s000…` URL always wins over `--public`; the flag
cannot force a secure link into public mode.
