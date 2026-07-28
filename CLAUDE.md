# holesail-cpp

C++20 port of [holesail](https://github.com/holesail/holesail) 2.4.1 — the peer-to-peer
TCP/UDP proxy — on top of hyperdht-cpp. Wire-compatible with the JS original: same
connection strings, same key derivation, same DHT records.

## Project Status

**v0.1.0** | 10 headers, 9 library sources + the CLI (~3.6k lines), 11 GoogleTest binaries.

Live-validated on the public DHT against the real JS `holesail` 2.4.1 binary, in both
directions, secure and public: real HTTP traffic (200 + correct body) through the tunnel.

| Component | File | Status |
|---|---|---|
| z32 / SHA-256 / hex / `hs://` parsing | `keys.{hpp,cpp}` | Done (JS vectors) |
| DHT metadata record codec | `record.{hpp,cpp}` | Done (byte-exact vs `JSON.stringify`) |
| Logger + ANSI colours | `logger.*`, `colors.hpp` | Done (`holesail-logger` format) |
| TCP pipe + proxy | `pipe.{hpp,cpp}` | Done (bidirectional, half-close, backpressure) |
| UDP framing + proxy | `udp_pipe.{hpp,cpp}` | Done (`[u32 BE len][payload]`) |
| Server (keypair, firewall, publish) | `server.{hpp,cpp}` | Done |
| Client (resolve, fetch record, proxy) | `client.{hpp,cpp}` | Done |
| `Holesail` facade + `lookup()` | `holesail.{hpp,cpp}` | Done |
| CLI (argv, validation, output) | `cli.{hpp,cpp}`, `app/main.cpp` | Done |
| Live interop harness | `scripts/cross-test.sh` | Done (5 cases + lookup) |
| `--filemanager`, cli-box, QR code | — | **Not implemented** (D1, D2) |

## Stack

- **Language**: C++20 — error codes + `std::optional` in the data plane, exceptions only in
  startup/validation paths that run once
- **Build**: CMake 3.20+, Ninja, Nix flake
- **Transport**: hyperdht-cpp via its **C API** (`<hyperdht/hyperdht.h>`)
- **Event loop**: libuv **1.51.x** (`uv_tcp_t` / `uv_udp_t` on the DHT's own loop)
- **Crypto**: libsodium (`crypto_hash_sha256`, `randombytes_buf`)
- **Testing**: GoogleTest + CTest, plus a live JS interop harness
- **License**: AGPL-3.0-or-later (upstream holesail); hyperdht-cpp is LGPL-3.0

## Key Decisions

**C API, not the C++ headers.** `hyperdht.h` symbols are always exported; the C++ classes are
hidden in Release builds unless `HYPERDHT_EXPORT_CXX=ON`. The examples and the Python port
use the C API too.

**Local sockets are libuv handles on the DHT's loop.** Single-threaded, callback-driven, no
polling, no threads, no raw fds.

**No JSON dependency.** The record is a flat object with three keys of known type; a ~150-line
hand-rolled writer/reader beats pulling in nlohmann. Same for z32 (~60 lines), the argv
parser, and the ANSI colours.

**Immutability / focused files.** Functions take `const` inputs and return new values; files
stay in the 200-400 line range.

**Two additions were made to hyperdht-cpp** for this port (both purely additive to the C API):
`hyperdht_stream_pause()` / `hyperdht_stream_resume()` for remote→local backpressure, and
`int reusable_socket` appended to the tail of `hyperdht_connect_opts_t`.

## Architecture

```
holesail-cpp/
├── CMakeLists.txt            libholesail (static) + holesail (CLI) + ctest wiring
├── flake.nix                 devShell, .#default, .#holesail-aarch64-static
├── README.md                 user-facing docs (CLI, API, divergences)
├── CLAUDE.md                 this file
├── LICENSE                   AGPL-3.0
├── include/holesail/
│   ├── holesail.hpp          Holesail facade, Options, Info, derive(), lookup()
│   ├── server.hpp            HolesailServer — keypair, firewall, listen, publish/refresh
│   ├── client.hpp            HolesailClient — resolve, record fetch, local proxy
│   ├── keys.hpp              z32, sha256, hex, random_hex32, parse_url
│   ├── record.hpp            the DHT metadata record: encode + decode
│   ├── pipe.hpp              StreamSide, TcpPipe (one bridge), TcpProxy (listener)
│   ├── udp_pipe.hpp          frame(), FrameDecoder, UdpPipe, UdpProxy
│   ├── logger.hpp            holesail-logger format and levels
│   ├── colors.hpp            header-only ANSI helpers (barely-colours)
│   └── cli.hpp               argv parse, validateInput, help, stdout rendering
├── src/                      one .cpp per header (colors.hpp is header-only)
├── app/main.cpp              3 lines: calls holesail::cli::run
├── test/                     one suite per unit + test_integration (loopback) + lsan.supp
├── scripts/cross-test.sh     live interop against the JS holesail on the public DHT
└── docs/superpowers/         the spec and the implementation plan this port followed
```

## Verified cross-language vectors

Captured from the JS reference; used verbatim in `test_smoke` / `test_keys` / `test_record`.
Do not invent new ones — regenerate from JS if more are needed.

```
key         = "this-is-a-32-char-holesail-key!!"          (32 chars)
SHA256(key) = a4e1504af3323f7665dd1ea745f7c6f86d3ad32a0eca5988fbac19f0d31376f0
public_key  = c5fffd407ea7d83056da8e193f33967b2d9d42b5f4a9d54757f0253a1ce56a30
z32(pubkey) = az994od6w9cdyis4tacu6chsxcs34oii61w7kt4z6y1uw88fpeay
z32(seed)   = wuoiy1zuge9zc3q7d4uwm76g9bsuiw3kb5ffun85ioc9bwauq5ay
```

z32 alphabet: `ybndrfg8ejkmcpqxot1uwisza345h769`. `""→""`, `"00"→"yy"`, `"ff"→"9h"`,
`"ffffffff"→"999999a"`, `01 02 … 1f 20 → yrbygbyfyadoonekbcgy4doxnyetrrawnwmbqgy3depta8e6dhoy`.

Records: `{"host":"127.0.0.1","port":8080}` (udp unset),
`{"host":"127.0.0.1","udp":true,"port":8080}`, `{"host":"127.0.0.1","udp":false,"port":8080}`.

## GOTCHAS

These cost real debugging time. Read them before changing anything in the areas they touch.

1. **The DHT record OMITS the `udp` key entirely when not in udp mode.** JS
   `JSON.stringify` drops `undefined` values, so the common record is
   `{"host":"127.0.0.1","port":8080}` — *not* `"udp":false`. The writer must omit it and the
   reader must tolerate its absence. Key order is always `host`, `udp`, `port`.

2. **The seed is SHA-256 of the key STRING's ASCII bytes, never of a hex-decoded value.**
   `SHA256("a3f1…")` over 64 ASCII characters, not over the 32 bytes those characters spell.
   Get this wrong and every derived keypair silently diverges from the JS one.

3. **libuv MUST be 1.51.x.** 1.52.0/1.52.1 have a UDP `POLLERR` regression that wedges
   libudx streams on real NAT paths (field-confirmed via nospoon). The flake pins nixos-25.11
   for exactly this. Do not bump nixpkgs without redoing hyperdht-cpp's `docs/TODO.md` §H2
   re-check.

4. **`hyperdht_stream_write_with_drain` returns 0 for EVERY successful write** — it drops
   libudx's "drained" bit. `TcpPipe` must therefore NOT treat 0 as backpressure; it bounds
   unacked bytes itself via `kStreamHighWaterMark`. Treating 0 as backpressure turns the
   tunnel into stop-and-wait: ~64 KiB per round trip.

5. **After bumping the hyperdht flake input you MUST delete `build/` and reconfigure.**
   `CMakeCache.txt` pins the resolved `/nix/store/...` path, so `cmake --build` silently keeps
   linking the OLD library. This masked a fix for a full debugging cycle.

6. **Two known teardown defects live in hyperdht-cpp, not here** — both scoped narrowly in
   `test/lsan.supp`, both belong upstream:
   - *Cancelled queries are never reaped.* `hyperdht_query_cancel()` does not fire the
     completion lambda that holds the query state, despite the header documenting
     "on_done fires exactly once per query (natural completion OR cancel)". ~34 KB per client
     torn down with a lookup in flight.
   - *Each DHT leaves a stopped-but-unclosed `uv_timer_t`* (~1237 bytes / 4 allocations),
     reproducible with zero holesail code following `hyperdht.h`'s own recipe. That unclosed
     handle is why `uv_loop_close()` cannot return 0 after using a DHT — the CLI works around
     it with a `uv_walk` close-everything pass in `drain_and_close()` (`src/cli.cpp`).

7. **`Holesail::close()` only schedules the DHT teardown.** It is legal from inside a libuv
   callback (the CLI calls it from its SIGINT handler), so the free is deferred onto the loop.
   Callers must run the loop once more afterwards or the teardown never completes.

8. **Colour helpers always emit `\x1b[0m` and never sniff for a TTY** — matching
   `barely-colours`, so nested calls produce stacked resets. That is reproduced deliberately;
   do not "fix" it, the CLI-rendering tests assert the exact byte sequences.

9. **The aarch64 static package must NOT use `pkgsCross.aarch64-multiplatform.pkgsStatic`.**
   That set static-musls the *build*-platform tools too, so Nix tries to bootstrap a
   static-musl CMake — which fails on nixpkgs 25.11 with `CMake Error: Unknown argument
   --disable-shared`. Declaring the target as `crossSystem = { config =
   "aarch64-unknown-linux-musl"; isStatic = true; }` keeps cmake/ninja native and only the
   target static. Diagnostic: `pkgsStatic.cmake.__spliced.buildHost.name` is
   `cmake-static-x86_64-unknown-linux-musl`, the crossSystem one is plain `cmake`.

10. **`parse_url` sniffs `secure` from `url[5] == 's'` on ANY string**, prefixed or not
   (upstream quirk Q2, reproduced). The one deliberate fix is the client's `--public`
   (D3/Q1): JS passed `secure: argv.public`, making `--public` mean *secure*; here it means
   public. Everything else in §8/§9 of the spec is reproduced as-is.

## Build

```bash
nix develop
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Only the `holesail` binary is installed (`install(TARGETS holesail RUNTIME DESTINATION bin)`);
embed the library with `add_subdirectory()` + `target_link_libraries(app PRIVATE holesail_lib)`.

## Test

```bash
# Unit + loopback integration — no network.
nix develop --command bash -c "cd build && ctest --output-on-failure"

# Sanitizers (the leaks that remain are the hyperdht-cpp ones above).
cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
      -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build-asan
LSAN_OPTIONS=suppressions=$PWD/test/lsan.supp ctest --test-dir build-asan --output-on-failure

# Live interop against the real JS holesail — needs network, ~7 minutes.
nix develop --command ./scripts/cross-test.sh ./build/holesail
```

`cross-test.sh` runs C++↔C++ secure (baseline), C++ server ↔ JS client and JS server ↔ C++
client in secure and public mode, plus `--lookup` against a JS-published record. It needs
`python3`, `curl` and `node`, which is why it must be run through `nix develop` — the host
PATH has none of them. Knobs: `HOLESAIL_JS` (path to another JS holesail, binary or prefix),
`HOLESAIL_SERVER_WAIT` / `HOLESAIL_CLIENT_WAIT` (default 30s each — shorter is flaky on the
public DHT), `HOLESAIL_ORIGIN_PORT`.

## Packaging

```bash
nix build .#default                    # host binary
nix build .#holesail-aarch64-static    # aarch64 musl, fully static (Raspberry Pi class)
file result/bin/holesail               # ELF 64-bit LSB executable, ARM aarch64, statically linked
```

The static output instantiates hyperdht-cpp's own `nix/lib.nix` builder against the cross
set, because hyperdht-cpp only publishes native package outputs. Result: 3.5 MB, zero runtime
deps — `scp result/bin/holesail pi:` and run it on Raspberry Pi OS Lite.

## Reference material

- JS source: `/nix/store/mlhix2xnb4hsa4zf85rkln5ngr8ncm14-holesail-2.4.1/lib/node_modules/holesail/`
  (`src/index.js`, `src/bin/holesail.mjs`, `src/lib/validateInput.js`, plus `holesail-server`,
  `holesail-client`, `@holesail/hyper-cmd-lib-net`, `hyper-cmd-lib-keys`, `z32`,
  `holesail-logger`). Set `HOLESAIL_JS` to it when digging.
- Spec: `docs/superpowers/specs/2026-07-28-holesail-cpp-port-design.md` — §8 divergences,
  §9 reproduced quirks. Any new divergence needs a row there.
- Plan: `docs/superpowers/plans/2026-07-28-holesail-cpp-port.md`.
- Sibling repo: `../hyperdht-cpp` (transport, `CLAUDE.md` there for DHT-level gotchas).
