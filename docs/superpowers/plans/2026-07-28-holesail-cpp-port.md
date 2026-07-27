# holesail-cpp Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A C++ reimplementation of holesail 2.4.1 — the TCP/UDP peer-to-peer proxy — on top of hyperdht-cpp, wire-compatible with the JavaScript original.

**Architecture:** A static library (`libholesail`) plus a thin CLI (`holesail`). The library talks to hyperdht through its C API (`<hyperdht/hyperdht.h>`) and drives local TCP/UDP sockets with `uv_tcp_t` / `uv_udp_t` on the DHT's own libuv loop. Single-threaded, callback-driven, no polling, no threads. Pure logic (z32, key derivation, the DHT record codec, UDP framing) lives in dependency-free units that are unit-tested against vectors captured from the JS reference.

**Tech Stack:** C++20, CMake 3.20+, libuv 1.51.x, libsodium, hyperdht-cpp v0.3.0 (C API), GoogleTest, Nix flake.

**Spec:** `docs/superpowers/specs/2026-07-28-holesail-cpp-port-design.md` — read it before starting. It contains the exact key-derivation and connection-string rules this plan implements.

## Global Constraints

- **C++20.** Error codes and `std::optional`, not exceptions, in the data plane. Exceptions are acceptable only in startup/validation paths that run once.
- **libuv MUST be 1.51.x.** 1.52.0/1.52.1 have a UDP `POLLERR` regression that silently wedges libudx streams on real NAT paths. The flake pins nixos-25.11 for this reason.
- **No new third-party dependencies.** libsodium, libuv, libudx and hyperdht are already in the tree via hyperdht-cpp. Do not add a JSON library, a QR encoder, a CLI parser, or a logging framework.
- **Immutability.** Return new values rather than mutating arguments in place. Functions take `const` inputs and return results.
- **Files stay focused:** 200-400 lines typical, 800 hard maximum. One responsibility per file.
- **License: AGPL-3.0** (matches upstream holesail). Every new source file starts with a two-line SPDX header:
  ```
  // SPDX-License-Identifier: AGPL-3.0-or-later
  // Part of holesail-cpp — a C++ port of holesail (https://github.com/holesail/holesail)
  ```
- **Commit format:** `<type>: <description>` where type is one of feat, fix, refactor, docs, test, chore. No AI attribution trailers.
- **Reference source** for any behavioral question:
  `/nix/store/mlhix2xnb4hsa4zf85rkln5ngr8ncm14-holesail-2.4.1/lib/node_modules/holesail/`
  Set `HOLESAIL_JS=/nix/store/mlhix2xnb4hsa4zf85rkln5ngr8ncm14-holesail-2.4.1/lib/node_modules/holesail` for convenience.

## Verified Cross-Language Vectors

These were produced by running the JS reference and confirmed to reproduce in C++
with libsodium + hyperdht. Use them verbatim in tests — do not invent new ones.

```
key        = "this-is-a-32-char-holesail-key!!"          (32 chars)
SHA256(key)= a4e1504af3323f7665dd1ea745f7c6f86d3ad32a0eca5988fbac19f0d31376f0
public_key = c5fffd407ea7d83056da8e193f33967b2d9d42b5f4a9d54757f0253a1ce56a30
z32(pubkey)= az994od6w9cdyis4tacu6chsxcs34oii61w7kt4z6y1uw88fpeay
z32(seed)  = wuoiy1zuge9zc3q7d4uwm76g9bsuiw3kb5ffun85ioc9bwauq5ay
```

z32 encoding pairs (hex input → z32 output):

```
""                 -> ""
"00"               -> "yy"
"0001"             -> "yyyo"
"000102"           -> "yyyor"
"00010203"         -> "yyyorya"
"0001020304"       -> "yyyoryar"
"ff"               -> "9h"
"ffffffff"         -> "999999a"
"00"*32            -> "yyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyy"
"0102...1f20"      -> "yrbygbyfyadoonekbcgy4doxnyetrrawnwmbqgy3depta8e6dhoy"
```

(the last input is the 32 bytes `01 02 03 ... 1f 20`)

DHT record encoding:

```
host=127.0.0.1 port=8080 udp unset -> {"host":"127.0.0.1","port":8080}
host=127.0.0.1 port=8080 udp=true  -> {"host":"127.0.0.1","udp":true,"port":8080}
host=127.0.0.1 port=8080 udp=false -> {"host":"127.0.0.1","udp":false,"port":8080}
```

ANSI codes used by `barely-colours` (always emitted — no TTY detection, matching JS):

```
bold \x1b[1m   dim \x1b[2m   underline \x1b[4m   reset \x1b[0m
red \x1b[31m   green \x1b[32m   yellow \x1b[33m
blue \x1b[34m  magenta \x1b[35m cyan \x1b[36m    brightBlack \x1b[90m
```

Every colour helper appends `\x1b[0m` (reset-all), so nested calls emit multiple
resets. Reproduce that — do not "fix" it.

## File Structure

| File | Responsibility |
|---|---|
| `CMakeLists.txt` | Build `libholesail` + `holesail`, find hyperdht, wire GoogleTest |
| `flake.nix` | Dev shell + package, hyperdht-cpp as an input |
| `include/holesail/keys.hpp` / `src/keys.cpp` | z32, SHA-256, hex, `hs://` URL parsing. No I/O, no deps beyond libsodium. |
| `include/holesail/record.hpp` / `src/record.cpp` | The DHT metadata JSON record: encode + decode. No deps. |
| `include/holesail/logger.hpp` / `src/logger.cpp` | `holesail-logger` format and levels |
| `include/holesail/colors.hpp` | Header-only ANSI helpers matching `barely-colours` |
| `include/holesail/pipe.hpp` / `src/pipe.cpp` | `StreamSide` + `TcpPipe` (one bidirectional TCP↔stream bridge) + `TcpProxy` (listener) |
| `include/holesail/udp_pipe.hpp` / `src/udp_pipe.cpp` | `frame()`, `FrameDecoder`, `UdpPipe`, `UdpProxy` |
| `include/holesail/server.hpp` / `src/server.cpp` | `HolesailServer`: keypair, firewall, listen, record publish + refresh |
| `include/holesail/client.hpp` / `src/client.cpp` | `HolesailClient`: target resolution, record fetch, local proxy |
| `include/holesail/holesail.hpp` / `src/holesail.cpp` | `Holesail` facade + `Info` + `lookup()` |
| `app/main.cpp` | argv parsing, `validateInput` port, terminal output |
| `test/*.cpp` | GoogleTest suites, one per unit |
| `scripts/cross-test.sh` | Live interop harness against the JS `holesail` binary |

**Each task's `Interfaces → Produces` block is the header contract.** Write it
into the `.hpp` verbatim, adding doc comments but changing no name, parameter
type, or return type — later tasks call these exact signatures, and the tests
in this plan are written against them.

---

### Task 1: Project scaffold, build wiring, and the crypto smoke test

Establishes the build and proves in one test that the C++ toolchain reproduces
the JS key derivation. If this test passes, the riskiest assumption in the
project is retired.

**Files:**
- Create: `flake.nix`
- Create: `CMakeLists.txt`
- Create: `.gitignore`
- Create: `LICENSE`
- Test: `test/test_smoke.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces: a build in which `#include <hyperdht/hyperdht.h>` and `#include <sodium.h>` resolve, `libholesail` is a linkable target, and `ctest` runs GoogleTest suites. Later tasks add `.cpp` files to the `holesail_lib` target and `test_*.cpp` files via `holesail_add_test(<name>)`.

- [ ] **Step 1: Write the failing test**

Create `test/test_smoke.cpp`:

```cpp
// SPDX-License-Identifier: AGPL-3.0-or-later
// Part of holesail-cpp — a C++ port of holesail (https://github.com/holesail/holesail)
#include <gtest/gtest.h>

#include <hyperdht/hyperdht.h>
#include <sodium.h>

#include <cstdio>
#include <string>

namespace {

std::string to_hex(const uint8_t* data, size_t len) {
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; i++) {
        out.push_back(kHex[data[i] >> 4]);
        out.push_back(kHex[data[i] & 0x0f]);
    }
    return out;
}

}  // namespace

// The whole port rests on this: C++ SHA-256 of the key STRING, fed to
// hyperdht_keypair_from_seed, must yield the same public key that
// JS `HyperDHT.keyPair(sha256(key))` yields. Vector captured from
// holesail 2.4.1's dependency tree.
TEST(Smoke, KeyDerivationMatchesJsReference) {
    ASSERT_GE(sodium_init(), 0);

    const std::string key = "this-is-a-32-char-holesail-key!!";

    uint8_t seed[32];
    crypto_hash_sha256(seed, reinterpret_cast<const uint8_t*>(key.data()), key.size());
    EXPECT_EQ(to_hex(seed, 32),
              "a4e1504af3323f7665dd1ea745f7c6f86d3ad32a0eca5988fbac19f0d31376f0");

    hyperdht_keypair_t kp;
    hyperdht_keypair_from_seed(&kp, seed);
    EXPECT_EQ(to_hex(kp.public_key, 32),
              "c5fffd407ea7d83056da8e193f33967b2d9d42b5f4a9d54757f0253a1ce56a30");
}
```

- [ ] **Step 2: Write `flake.nix`**

```nix
{
  description = "holesail-cpp — C++ port of holesail, powered by hyperdht-cpp";

  inputs = {
    # libuv 1.51.x — see Global Constraints. Do NOT bump past 25.11.
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-25.11";
    hyperdht-cpp.url = "path:../hyperdht-cpp";
    hyperdht-cpp.inputs.nixpkgs.follows = "nixpkgs";
  };

  outputs = { self, nixpkgs, hyperdht-cpp }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAll = f: nixpkgs.lib.genAttrs systems (system: f system nixpkgs.legacyPackages.${system});
    in {
      devShells = forAll (system: pkgs: {
        default = pkgs.mkShell {
          packages = [
            pkgs.cmake pkgs.ninja pkgs.pkg-config
            pkgs.libsodium pkgs.libuv pkgs.gtest
            hyperdht-cpp.packages.${system}.default
          ];
          shellHook = ''
            echo "holesail-cpp dev shell — cmake ${pkgs.cmake.version}"
          '';
        };
      });

      packages = forAll (system: pkgs: {
        default = pkgs.stdenv.mkDerivation {
          pname = "holesail-cpp";
          version = "0.1.0";
          src = self;
          nativeBuildInputs = [ pkgs.cmake pkgs.ninja pkgs.pkg-config ];
          buildInputs = [
            pkgs.libsodium pkgs.libuv
            hyperdht-cpp.packages.${system}.default
          ];
          cmakeFlags = [ "-DHOLESAIL_BUILD_TESTS=OFF" ];
        };
      });
    };
}
```

If `hyperdht-cpp.packages.<system>.default` does not exist, run
`nix flake show ../hyperdht-cpp` and use the attribute that produces the
installed library (the one containing `lib/cmake/hyperdht/hyperdht-config.cmake`).

- [ ] **Step 3: Write `CMakeLists.txt`**

```cmake
cmake_minimum_required(VERSION 3.20)
project(holesail-cpp VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(HOLESAIL_BUILD_TESTS "Build the test suite" ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium)
pkg_check_modules(UV REQUIRED IMPORTED_TARGET libuv)

# hyperdht-cpp installs hyperdht-config.cmake; fall back to pkg-config.
find_package(hyperdht QUIET)
if(NOT hyperdht_FOUND)
    pkg_check_modules(HYPERDHT REQUIRED IMPORTED_TARGET hyperdht)
    add_library(hyperdht::hyperdht ALIAS PkgConfig::HYPERDHT)
endif()

add_library(holesail_lib STATIC)
target_include_directories(holesail_lib PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_link_libraries(holesail_lib PUBLIC
    hyperdht::hyperdht PkgConfig::SODIUM PkgConfig::UV)
target_compile_options(holesail_lib PRIVATE -Wall -Wextra)
# Sources are appended by later tasks:
#   target_sources(holesail_lib PRIVATE src/keys.cpp ...)

add_executable(holesail app/main.cpp)
target_link_libraries(holesail PRIVATE holesail_lib)

if(HOLESAIL_BUILD_TESTS)
    enable_testing()
    find_package(GTest REQUIRED)
    function(holesail_add_test name)
        add_executable(${name} test/${name}.cpp)
        target_link_libraries(${name} PRIVATE holesail_lib GTest::gtest GTest::gtest_main)
        add_test(NAME ${name} COMMAND ${name})
    endfunction()
    holesail_add_test(test_smoke)
endif()
```

`add_executable(holesail app/main.cpp)` needs a file to exist. Create a placeholder
`app/main.cpp` now — Task 11 replaces it wholesale:

```cpp
// SPDX-License-Identifier: AGPL-3.0-or-later
// Part of holesail-cpp — a C++ port of holesail (https://github.com/holesail/holesail)
int main() { return 0; }
```

`holesail_lib` has no sources yet; add a placeholder so CMake accepts the target.
Create `src/version.cpp`:

```cpp
// SPDX-License-Identifier: AGPL-3.0-or-later
// Part of holesail-cpp — a C++ port of holesail (https://github.com/holesail/holesail)
namespace holesail {
const char* version() { return "0.1.0"; }
}  // namespace holesail
```

and add `target_sources(holesail_lib PRIVATE src/version.cpp)` after the
`add_library` block.

- [ ] **Step 4: Write `.gitignore` and `LICENSE`**

`.gitignore`:

```
build/
build-*/
result
result-*
compile_commands.json
.cache/
.direnv/
```

`LICENSE`: the full GNU Affero General Public License v3.0 text. Fetch it with:

```bash
curl -sSL https://www.gnu.org/licenses/agpl-3.0.txt -o LICENSE
```

- [ ] **Step 5: Run the test to verify it fails**

```bash
nix develop --command bash -c "cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug && cmake --build build"
```

Expected on the first attempt: a configure or link error. Resolve it (usually
the hyperdht flake attribute name, or a missing `pkg-config` path) until the
build succeeds and `./build/test_smoke` runs.

- [ ] **Step 6: Run the test to verify it passes**

```bash
nix develop --command bash -c "cd build && ctest --output-on-failure"
```

Expected: `test_smoke` PASSES, both `EXPECT_EQ`s green.

If `KeyDerivationMatchesJsReference` fails, **stop and report** — the port's core
assumption is broken and the rest of the plan does not apply.

- [ ] **Step 7: Commit**

```bash
git add flake.nix flake.lock CMakeLists.txt .gitignore LICENSE app/main.cpp src/version.cpp test/test_smoke.cpp
git commit -m "chore: project scaffold with hyperdht + JS key-derivation smoke test"
```

---

### Task 2: Add stream backpressure and client reusable-socket to hyperdht-cpp

**This task edits the sibling repository `../hyperdht-cpp`, not holesail-cpp.**
Both changes are purely additive to the C API and cannot break existing consumers.
`src/secret_stream.cpp:790` already carries a comment naming this as the intended
long-term fix.

**Files:**
- Modify: `../hyperdht-cpp/include/hyperdht/secret_stream.hpp` (add two public methods + one member near `read_started_` at line 343)
- Modify: `../hyperdht-cpp/src/secret_stream.cpp` (implement them; `udx_stream_read_start` is already called at line 466)
- Modify: `../hyperdht-cpp/include/hyperdht/hyperdht.h` (declare two functions; append one struct field)
- Modify: `../hyperdht-cpp/src/ffi_stream.cpp` (implement the two functions)
- Modify: `../hyperdht-cpp/src/ffi_core.cpp` (honour the new connect option)
- Test: `../hyperdht-cpp/test/test_stream_pause.cpp`

**Interfaces:**
- Consumes: nothing from this plan.
- Produces:
  ```c
  HYPERDHT_API void hyperdht_stream_pause(hyperdht_stream_t* stream);
  HYPERDHT_API void hyperdht_stream_resume(hyperdht_stream_t* stream);
  /* new tail field of hyperdht_connect_opts_t */
  int reusable_socket;
  ```
  Task 6 and Task 7 call `hyperdht_stream_pause`/`resume`. Task 9 sets
  `opts.reusable_socket = 1`.

- [ ] **Step 1: Write the failing test**

Create `../hyperdht-cpp/test/test_stream_pause.cpp`:

```cpp
#include <gtest/gtest.h>

#include "hyperdht/secret_stream.hpp"

#include <udx.h>
#include <uv.h>

// Pausing an unstarted or destroyed duplex must be a safe no-op, and
// pause/resume must be idempotent. The transport-level behaviour is
// covered by the existing loopback stream tests.
TEST(SecretStreamPause, IdempotentAndSafeBeforeStart) {
    uv_loop_t loop;
    ASSERT_EQ(uv_loop_init(&loop), 0);

    udx_t udx;
    ASSERT_EQ(udx_init(&loop, &udx, nullptr), 0);

    udx_socket_t sock;
    ASSERT_EQ(udx_socket_init(&udx, &sock, nullptr), 0);

    auto* raw = new udx_stream_t{};
    ASSERT_EQ(udx_stream_init(&udx, raw, 1, nullptr, nullptr), 0);

    hyperdht::secret_stream::SecretStreamDuplex duplex(raw, &loop, {});

    // Before start(): both are no-ops, neither crashes.
    duplex.pause_read();
    duplex.pause_read();
    duplex.resume_read();
    duplex.resume_read();
    EXPECT_FALSE(duplex.is_read_paused());

    duplex.destroy(0);
    uv_run(&loop, UV_RUN_NOWAIT);
    uv_loop_close(&loop);
}
```

Check the exact `SecretStreamDuplex` constructor signature at
`include/hyperdht/secret_stream.hpp:226` and the neighbouring test files in
`../hyperdht-cpp/test/` for the established loop/udx setup idiom — copy whichever
pattern the existing stream tests use rather than the sketch above if they differ.

- [ ] **Step 2: Run the test to verify it fails**

```bash
cd ../hyperdht-cpp
nix develop --command bash -c "cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug && cmake --build build --target test_stream_pause"
```

Expected: compile error — `pause_read`, `resume_read`, `is_read_paused` are not
members of `SecretStreamDuplex`.

Register the new test in `../hyperdht-cpp/CMakeLists.txt` alongside the other
`test_*` entries first, following whatever helper that file already uses.

- [ ] **Step 3: Add the duplex methods**

In `include/hyperdht/secret_stream.hpp`, in the public section near the other
one-line accessors (around line 327, next to `raw_stream()`):

```cpp
    // Read-side backpressure. `pause_read()` stops UDX from delivering
    // further reliable frames so the peer's congestion window closes;
    // `resume_read()` re-arms delivery. Both are no-ops before `start()`
    // and after `destroy()`. Unordered datagrams (send/recv) are NOT
    // affected — they are lossy by contract.
    void pause_read();
    void resume_read();
    bool is_read_paused() const { return read_paused_; }
```

In the private member block near `read_started_` (line 343):

```cpp
    bool read_paused_ = false;
```

Leave `read_started_` alone — it is currently unused; do not repurpose it.

In `src/secret_stream.cpp`, next to `start()` (which calls
`udx_stream_read_start(raw_stream_, on_udx_read)` at line 466):

```cpp
void SecretStreamDuplex::pause_read() {
    if (!started_ || destroyed_ || read_paused_) return;
    udx_stream_read_stop(raw_stream_);
    read_paused_ = true;
}

void SecretStreamDuplex::resume_read() {
    if (!started_ || destroyed_ || !read_paused_) return;
    read_paused_ = false;
    udx_stream_read_start(raw_stream_, on_udx_read);
}
```

- [ ] **Step 4: Add the C API functions**

In `include/hyperdht/hyperdht.h`, immediately after
`hyperdht_stream_write_with_drain` (line 584):

```c
/**
 * Pause / resume delivery of reliable stream data.
 *
 * `hyperdht_stream_pause` stops the on_data callback from firing and lets
 * the peer's congestion window close — the correct way to apply
 * backpressure when the consumer of the data is slower than the stream.
 * `hyperdht_stream_resume` re-arms delivery. Both are idempotent and safe
 * on a closed stream. Unordered datagrams are unaffected.
 */
HYPERDHT_API void hyperdht_stream_pause(hyperdht_stream_t* stream);
HYPERDHT_API void hyperdht_stream_resume(hyperdht_stream_t* stream);
```

In `src/ffi_stream.cpp`:

```cpp
void hyperdht_stream_pause(hyperdht_stream_t* stream) {
    if (!stream || stream->closed || !stream->duplex) return;
    stream->duplex->pause_read();
}

void hyperdht_stream_resume(hyperdht_stream_t* stream) {
    if (!stream || stream->closed || !stream->duplex) return;
    stream->duplex->resume_read();
}
```

- [ ] **Step 5: Add the connect option**

Append to the **tail** of `hyperdht_connect_opts_t` in
`include/hyperdht/hyperdht.h` (the struct is documented as tail-extensible —
adding anywhere else breaks ABI):

```c
    /**
     * Reuse the cached UDX route from a previous connection to the same
     * peer instead of re-punching (JS: `opts.reusableSocket`). 0 = off
     * (default), 1 = on.
     */
    int reusable_socket;
```

`hyperdht_connect_opts_default()` zero-initialises the struct, so the default is
already correct — verify it uses `memset`/`= {}` rather than field-by-field
assignment, and add `opts->reusable_socket = 0;` if it is the latter.

In `src/ffi_core.cpp`, find where `hyperdht_connect_ex` translates
`hyperdht_connect_opts_t` into the C++ `ConnectOptions` and add:

```cpp
    cxx_opts.reusable_socket = opts->reusable_socket != 0;
```

The C++ field already exists at `include/hyperdht/dht.hpp:245`.

- [ ] **Step 6: Run the tests to verify they pass**

```bash
cd ../hyperdht-cpp
nix develop --command bash -c "cmake --build build && cd build && ctest --output-on-failure"
```

Expected: `test_stream_pause` passes **and all 584 pre-existing tests still pass.**
A regression here means the change was not additive — fix it before proceeding.

- [ ] **Step 7: Commit (in ../hyperdht-cpp)**

```bash
cd ../hyperdht-cpp
git add include/hyperdht/secret_stream.hpp src/secret_stream.cpp \
        include/hyperdht/hyperdht.h src/ffi_stream.cpp src/ffi_core.cpp \
        test/test_stream_pause.cpp CMakeLists.txt
git commit -m "feat: stream read backpressure (pause/resume) + client reusable_socket opt"
```

---

### Task 3: z32, SHA-256, hex, and `hs://` URL parsing

Pure functions, no I/O. This is where wire compatibility is won or lost.

**Files:**
- Create: `include/holesail/keys.hpp`, `src/keys.cpp`
- Modify: `CMakeLists.txt` (add `src/keys.cpp` to `holesail_lib`, add `holesail_add_test(test_keys)`)
- Test: `test/test_keys.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces:
  ```cpp
  namespace holesail {
  std::string z32_encode(const uint8_t* data, size_t len);
  std::string z32_encode(const std::vector<uint8_t>& data);
  std::optional<std::vector<uint8_t>> z32_decode(std::string_view s);
  std::array<uint8_t, 32> sha256(std::string_view s);
  std::string to_hex(const uint8_t* data, size_t len);
  std::optional<std::vector<uint8_t>> from_hex(std::string_view s);
  std::string random_hex32();                        // 64 lowercase hex chars
  struct ParsedUrl { std::string key; std::optional<bool> secure; };
  ParsedUrl parse_url(std::string_view url);
  }
  ```

- [ ] **Step 1: Write the failing test**

Create `test/test_keys.cpp`:

```cpp
// SPDX-License-Identifier: AGPL-3.0-or-later
// Part of holesail-cpp — a C++ port of holesail (https://github.com/holesail/holesail)
#include <gtest/gtest.h>

#include "holesail/keys.hpp"

#include <sodium.h>

using namespace holesail;

namespace {

std::vector<uint8_t> hexbytes(std::string_view hex) {
    auto out = from_hex(hex);
    EXPECT_TRUE(out.has_value()) << "bad hex literal in test: " << hex;
    return out.value_or(std::vector<uint8_t>{});
}

}  // namespace

TEST(Z32, MatchesJsVectors) {
    // hex input -> z32 output, captured from the JS `z32` module.
    const std::pair<const char*, const char*> kVectors[] = {
        {"", ""},
        {"00", "yy"},
        {"0001", "yyyo"},
        {"000102", "yyyor"},
        {"00010203", "yyyorya"},
        {"0001020304", "yyyoryar"},
        {"ff", "9h"},
        {"ffffffff", "999999a"},
        {"0000000000000000000000000000000000000000000000000000000000000000",
         "yyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyy"},
        {"0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20",
         "yrbygbyfyadoonekbcgy4doxnyetrrawnwmbqgy3depta8e6dhoy"},
    };
    for (const auto& [hex, z] : kVectors) {
        const auto bytes = hexbytes(hex);
        EXPECT_EQ(z32_encode(bytes), z) << "encoding " << hex;
        const auto back = z32_decode(z);
        ASSERT_TRUE(back.has_value()) << "decoding " << z;
        EXPECT_EQ(to_hex(back->data(), back->size()), hex) << "round-tripping " << z;
    }
}

TEST(Z32, RejectsCharactersOutsideTheAlphabet) {
    EXPECT_FALSE(z32_decode("!!!!").has_value());
    EXPECT_FALSE(z32_decode("yyyv").has_value());  // 'v' is not in the alphabet
    EXPECT_FALSE(z32_decode("YY").has_value());    // uppercase is not accepted
}

TEST(Keys, Sha256MatchesJsReference) {
    ASSERT_GE(sodium_init(), 0);
    const auto seed = sha256("this-is-a-32-char-holesail-key!!");
    EXPECT_EQ(to_hex(seed.data(), seed.size()),
              "a4e1504af3323f7665dd1ea745f7c6f86d3ad32a0eca5988fbac19f0d31376f0");
}

TEST(Keys, HexRoundTrip) {
    EXPECT_EQ(from_hex("zz"), std::nullopt);
    EXPECT_EQ(from_hex("abc"), std::nullopt);  // odd length
    const auto b = from_hex("00ff10");
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(to_hex(b->data(), b->size()), "00ff10");
}

TEST(Keys, RandomHex32Is64LowercaseHexChars) {
    ASSERT_GE(sodium_init(), 0);
    const auto a = random_hex32();
    const auto b = random_hex32();
    EXPECT_EQ(a.size(), 64u);
    EXPECT_NE(a, b);
    EXPECT_TRUE(from_hex(a).has_value());
    for (const char c : a) {
        EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))
            << "non-lowercase-hex character in " << a;
    }
}

TEST(UrlParser, SecureLink) {
    const auto p = parse_url("hs://s000abcdef");
    EXPECT_EQ(p.key, "abcdef");
    ASSERT_TRUE(p.secure.has_value());
    EXPECT_TRUE(*p.secure);
}

TEST(UrlParser, PublicLink) {
    const auto p = parse_url("hs://0000abcdef");
    EXPECT_EQ(p.key, "abcdef");
    EXPECT_FALSE(p.secure.has_value());  // JS leaves it `undefined`, never false
}

TEST(UrlParser, BareKeyPassesThrough) {
    const auto p = parse_url("az994od6w9cdyis4tacu6chsxcs34oii61w7kt4z6y1uw88fpeay");
    EXPECT_EQ(p.key, "az994od6w9cdyis4tacu6chsxcs34oii61w7kt4z6y1uw88fpeay");
    EXPECT_FALSE(p.secure.has_value());
}

TEST(UrlParser, EmptyUrl) {
    const auto p = parse_url("");
    EXPECT_EQ(p.key, "");
    EXPECT_FALSE(p.secure.has_value());
}

TEST(UrlParser, ShortUrlIsNotTreatedAsPrefixed) {
    // JS requires url.substring(5, 9).length === 4, i.e. at least 9 chars.
    const auto p = parse_url("hs://s0");
    EXPECT_EQ(p.key, "hs://s0");
}

// Quirk Q2, reproduced deliberately: the `secure` sniff runs on ANY string,
// so a bare key whose 6th character is 's' reads as secure.
TEST(UrlParser, QuirkQ2BareKeyWithSAtIndexFiveSniffsSecure) {
    const auto p = parse_url("abcdesomething");
    EXPECT_EQ(p.key, "abcdesomething");
    ASSERT_TRUE(p.secure.has_value());
    EXPECT_TRUE(*p.secure);
}
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
nix develop --command bash -c "cmake --build build --target test_keys"
```

Expected: `fatal error: holesail/keys.hpp: No such file or directory`.

- [ ] **Step 3: Write the header**

Create `include/holesail/keys.hpp`:

```cpp
// SPDX-License-Identifier: AGPL-3.0-or-later
// Part of holesail-cpp — a C++ port of holesail (https://github.com/holesail/holesail)
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace holesail {

// z-base-32, alphabet "ybndrfg8ejkmcpqxot1uwisza345h769".
// Port of the `z32` npm module used by holesail.
std::string z32_encode(const uint8_t* data, size_t len);
std::string z32_encode(const std::vector<uint8_t>& data);

// Returns nullopt if the input contains a character outside the alphabet.
std::optional<std::vector<uint8_t>> z32_decode(std::string_view s);

// SHA-256 of the string's bytes. holesail hashes the key STRING, so this
// takes a string_view rather than a byte span on purpose.
std::array<uint8_t, 32> sha256(std::string_view s);

std::string to_hex(const uint8_t* data, size_t len);

// Returns nullopt on odd length or a non-hex character.
std::optional<std::vector<uint8_t>> from_hex(std::string_view s);

// 32 random bytes as 64 lowercase hex characters — the auto-generated
// holesail key (JS: libKeys.randomBytes(32).toString('hex')).
std::string random_hex32();

struct ParsedUrl {
    std::string key;
    // nullopt means "the url did not determine it" — matching JS, which
    // leaves `secure` undefined rather than setting it to false.
    std::optional<bool> secure;
};

// Port of Holesail.urlParser. Strips an "hs://" prefix plus its 4-character
// mode field, and sniffs `secure` from character index 5.
ParsedUrl parse_url(std::string_view url);

}  // namespace holesail
```

- [ ] **Step 4: Write the implementation**

Create `src/keys.cpp`:

```cpp
// SPDX-License-Identifier: AGPL-3.0-or-later
// Part of holesail-cpp — a C++ port of holesail (https://github.com/holesail/holesail)
#include "holesail/keys.hpp"

#include <sodium.h>

namespace holesail {
namespace {

constexpr std::string_view kAlphabet = "ybndrfg8ejkmcpqxot1uwisza345h769";
constexpr char kHex[] = "0123456789abcdef";

int quintet_of(char c) {
    const auto pos = kAlphabet.find(c);
    return pos == std::string_view::npos ? -1 : static_cast<int>(pos);
}

int nibble_of(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace

std::string z32_encode(const uint8_t* data, size_t len) {
    std::string out;
    out.reserve((len * 8 + 4) / 5);
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < len; i++) {
        acc = (acc << 8) | data[i];
        bits += 8;
        while (bits >= 5) {
            bits -= 5;
            out.push_back(kAlphabet[(acc >> bits) & 0x1f]);
        }
    }
    if (bits > 0) out.push_back(kAlphabet[(acc << (5 - bits)) & 0x1f]);
    return out;
}

std::string z32_encode(const std::vector<uint8_t>& data) {
    return z32_encode(data.data(), data.size());
}

std::optional<std::vector<uint8_t>> z32_decode(std::string_view s) {
    std::vector<uint8_t> out;
    out.reserve(s.size() * 5 / 8);
    uint32_t acc = 0;
    int bits = 0;
    for (const char c : s) {
        const int v = quintet_of(c);
        if (v < 0) return std::nullopt;
        acc = (acc << 5) | static_cast<uint32_t>(v);
        bits += 5;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((acc >> bits) & 0xff));
        }
    }
    return out;
}

std::array<uint8_t, 32> sha256(std::string_view s) {
    std::array<uint8_t, 32> out{};
    crypto_hash_sha256(out.data(),
                       reinterpret_cast<const uint8_t*>(s.data()), s.size());
    return out;
}

std::string to_hex(const uint8_t* data, size_t len) {
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; i++) {
        out.push_back(kHex[data[i] >> 4]);
        out.push_back(kHex[data[i] & 0x0f]);
    }
    return out;
}

std::optional<std::vector<uint8_t>> from_hex(std::string_view s) {
    if (s.size() % 2 != 0) return std::nullopt;
    std::vector<uint8_t> out;
    out.reserve(s.size() / 2);
    for (size_t i = 0; i < s.size(); i += 2) {
        const int hi = nibble_of(s[i]);
        const int lo = nibble_of(s[i + 1]);
        if (hi < 0 || lo < 0) return std::nullopt;
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return out;
}

std::string random_hex32() {
    uint8_t buf[32];
    randombytes_buf(buf, sizeof(buf));
    return to_hex(buf, sizeof(buf));
}

ParsedUrl parse_url(std::string_view url) {
    ParsedUrl out;
    constexpr std::string_view kProtocol = "hs://";
    // JS: url.substring(0,5) === 'hs://' && url.substring(5,9).length === 4
    if (url.size() >= 9 && url.substr(0, kProtocol.size()) == kProtocol) {
        out.key = std::string(url.substr(9));
    } else {
        out.key = std::string(url);
    }
    // JS sets `secure = true` only when url[5] === 's'; it is otherwise left
    // undefined. Reproduced verbatim, including quirk Q2 (the sniff runs on
    // unprefixed strings too).
    if (url.size() > 5 && url[5] == 's') out.secure = true;
    return out;
}

}  // namespace holesail
```

- [ ] **Step 5: Wire it into the build**

In `CMakeLists.txt`, replace the placeholder sources line with:

```cmake
target_sources(holesail_lib PRIVATE src/version.cpp src/keys.cpp)
```

and add after `holesail_add_test(test_smoke)`:

```cmake
    holesail_add_test(test_keys)
```

- [ ] **Step 6: Run the tests to verify they pass**

```bash
nix develop --command bash -c "cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug && cmake --build build && cd build && ctest --output-on-failure"
```

Expected: all `test_keys` cases PASS, `test_smoke` still PASSES.

- [ ] **Step 7: Commit**

```bash
git add include/holesail/keys.hpp src/keys.cpp test/test_keys.cpp CMakeLists.txt
git commit -m "feat: z32, sha256, hex and hs:// url parsing"
```

---

### Task 4: The DHT metadata record codec

**Files:**
- Create: `include/holesail/record.hpp`, `src/record.cpp`
- Modify: `CMakeLists.txt`
- Test: `test/test_record.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces:
  ```cpp
  namespace holesail {
  struct Record {
      std::string host;
      std::optional<bool> udp;
      std::optional<uint16_t> port;
  };
  std::string encode_record(std::string_view host, std::optional<bool> udp, uint16_t port);
  std::optional<Record> decode_record(std::string_view json);
  }
  ```
  Task 8 calls `encode_record`; Tasks 9 and 10 call `decode_record`.

- [ ] **Step 1: Write the failing test**

Create `test/test_record.cpp`:

```cpp
// SPDX-License-Identifier: AGPL-3.0-or-later
// Part of holesail-cpp — a C++ port of holesail (https://github.com/holesail/holesail)
#include <gtest/gtest.h>

#include "holesail/record.hpp"

using namespace holesail;

// JSON.stringify drops undefined values, so the common record has NO udp key.
TEST(RecordEncode, MatchesJsStringifyByteForByte) {
    EXPECT_EQ(encode_record("127.0.0.1", std::nullopt, 8080),
              R"({"host":"127.0.0.1","port":8080})");
    EXPECT_EQ(encode_record("127.0.0.1", true, 8080),
              R"({"host":"127.0.0.1","udp":true,"port":8080})");
    EXPECT_EQ(encode_record("127.0.0.1", false, 8080),
              R"({"host":"127.0.0.1","udp":false,"port":8080})");
}

// --host is user input and lands inside a JSON string literal.
TEST(RecordEncode, EscapesTheHostString) {
    EXPECT_EQ(encode_record(R"(a"b)", std::nullopt, 1),
              R"({"host":"a\"b","port":1})");
    EXPECT_EQ(encode_record(R"(a\b)", std::nullopt, 1),
              R"({"host":"a\\b","port":1})");
    EXPECT_EQ(encode_record("a\nb", std::nullopt, 1),
              R"({"host":"a\nb","port":1})");
}

TEST(RecordDecode, ReadsJsProducedRecords) {
    auto r = decode_record(R"({"host":"127.0.0.1","port":8080})");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->host, "127.0.0.1");
    EXPECT_FALSE(r->udp.has_value());
    ASSERT_TRUE(r->port.has_value());
    EXPECT_EQ(*r->port, 8080);

    r = decode_record(R"({"host":"0.0.0.0","udp":true,"port":53})");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->host, "0.0.0.0");
    ASSERT_TRUE(r->udp.has_value());
    EXPECT_TRUE(*r->udp);
    EXPECT_EQ(*r->port, 53);
}

TEST(RecordDecode, ToleratesWhitespaceReorderingAndUnknownKeys) {
    const auto r = decode_record(
        R"({ "port" : 9000 , "extra" : "ignored" , "udp" : false , "host" : "example" })");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->host, "example");
    ASSERT_TRUE(r->udp.has_value());
    EXPECT_FALSE(*r->udp);
    EXPECT_EQ(*r->port, 9000);
}

TEST(RecordDecode, HandlesEscapesInTheHostString) {
    const auto r = decode_record(R"({"host":"a\"b\\c\nd","port":1})");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->host, "a\"b\\c\nd");
}

TEST(RecordDecode, RejectsMalformedInput) {
    EXPECT_FALSE(decode_record("").has_value());
    EXPECT_FALSE(decode_record("not json").has_value());
    EXPECT_FALSE(decode_record("{").has_value());
    EXPECT_FALSE(decode_record(R"({"host":"x")").has_value());   // unterminated
    EXPECT_FALSE(decode_record(R"({"host":"x\)").has_value());   // trailing escape
}

TEST(RecordDecode, RejectsOutOfRangePorts) {
    // The record still parses; only the port is reported absent, so the
    // client falls back to 8989 rather than failing outright.
    auto r = decode_record(R"({"host":"x","port":70000})");
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(r->port.has_value());

    r = decode_record(R"({"host":"x","port":-1})");
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(r->port.has_value());
}

TEST(RecordDecode, MissingHostYieldsEmptyHostNotFailure) {
    // The client falls back to 127.0.0.1 when host is absent, so an
    // otherwise-valid object without `host` must still parse.
    const auto r = decode_record(R"({"port":8080})");
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->host.empty());
    EXPECT_EQ(*r->port, 8080);
}
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
nix develop --command bash -c "cmake --build build --target test_record"
```

Expected: `fatal error: holesail/record.hpp: No such file or directory`.

- [ ] **Step 3: Write the header**

Create `include/holesail/record.hpp`:

```cpp
// SPDX-License-Identifier: AGPL-3.0-or-later
// Part of holesail-cpp — a C++ port of holesail (https://github.com/holesail/holesail)
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace holesail {

// The mutable DHT record holesail publishes at its public key:
//   {"host":"127.0.0.1","udp":true,"port":8080}
// `udp` is absent when the server was not started with --udp, because
// JS JSON.stringify drops undefined values. Field order is host, udp, port.
struct Record {
    std::string host;
    std::optional<bool> udp;
    std::optional<uint16_t> port;
};

// Byte-for-byte identical to the JS JSON.stringify output. `udp` is omitted
// entirely when nullopt.
std::string encode_record(std::string_view host, std::optional<bool> udp, uint16_t port);

// Minimal reader for a flat JSON object with string / bool / integer values.
// Unknown keys are ignored. Returns nullopt if the input is not a
// well-formed flat object. A `port` outside 1..65535 is reported as absent
// rather than failing the whole record.
std::optional<Record> decode_record(std::string_view json);

}  // namespace holesail
```

- [ ] **Step 4: Write the implementation**

Create `src/record.cpp`. The parser is a hand-rolled scanner for a flat object;
it must not recurse and must not allocate based on untrusted length fields.

```cpp
// SPDX-License-Identifier: AGPL-3.0-or-later
// Part of holesail-cpp — a C++ port of holesail (https://github.com/holesail/holesail)
#include "holesail/record.hpp"

#include <cctype>
#include <cstdlib>

namespace holesail {
namespace {

void append_escaped(std::string& out, std::string_view s) {
    for (const char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    static const char* kHex = "0123456789abcdef";
                    out += "\\u00";
                    out.push_back(kHex[(c >> 4) & 0x0f]);
                    out.push_back(kHex[c & 0x0f]);
                } else {
                    out.push_back(c);
                }
        }
    }
}

void skip_ws(std::string_view s, size_t& i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++;
}

// Reads a JSON string starting at the opening quote. Returns nullopt on an
// unterminated string or a trailing escape.
std::optional<std::string> read_string(std::string_view s, size_t& i) {
    if (i >= s.size() || s[i] != '"') return std::nullopt;
    i++;
    std::string out;
    while (i < s.size()) {
        const char c = s[i++];
        if (c == '"') return out;
        if (c != '\\') { out.push_back(c); continue; }
        if (i >= s.size()) return std::nullopt;
        const char e = s[i++];
        switch (e) {
            case '"':  out.push_back('"');  break;
            case '\\': out.push_back('\\'); break;
            case '/':  out.push_back('/');  break;
            case 'n':  out.push_back('\n'); break;
            case 'r':  out.push_back('\r'); break;
            case 't':  out.push_back('\t'); break;
            case 'b':  out.push_back('\b'); break;
            case 'f':  out.push_back('\f'); break;
            case 'u': {
                if (i + 4 > s.size()) return std::nullopt;
                // Only the ASCII subset holesail ever emits is decoded;
                // anything above 0x7f is passed through as a literal '?'.
                unsigned code = 0;
                for (int k = 0; k < 4; k++) {
                    const char h = s[i + k];
                    unsigned v;
                    if (h >= '0' && h <= '9') v = static_cast<unsigned>(h - '0');
                    else if (h >= 'a' && h <= 'f') v = static_cast<unsigned>(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') v = static_cast<unsigned>(h - 'A' + 10);
                    else return std::nullopt;
                    code = (code << 4) | v;
                }
                i += 4;
                out.push_back(code < 0x80 ? static_cast<char>(code) : '?');
                break;
            }
            default: return std::nullopt;
        }
    }
    return std::nullopt;  // unterminated
}

// Skips one value of any scalar type, capturing it as raw text.
std::optional<std::string_view> read_scalar(std::string_view s, size_t& i) {
    const size_t start = i;
    while (i < s.size() && s[i] != ',' && s[i] != '}') i++;
    if (i > s.size()) return std::nullopt;
    size_t end = i;
    while (end > start && (s[end - 1] == ' ' || s[end - 1] == '\t' ||
                           s[end - 1] == '\n' || s[end - 1] == '\r')) end--;
    if (end == start) return std::nullopt;
    return s.substr(start, end - start);
}

}  // namespace

std::string encode_record(std::string_view host, std::optional<bool> udp, uint16_t port) {
    std::string out = R"({"host":")";
    append_escaped(out, host);
    out += '"';
    if (udp.has_value()) {
        out += R"(,"udp":)";
        out += *udp ? "true" : "false";
    }
    out += R"(,"port":)";
    out += std::to_string(port);
    out += '}';
    return out;
}

std::optional<Record> decode_record(std::string_view json) {
    size_t i = 0;
    skip_ws(json, i);
    if (i >= json.size() || json[i] != '{') return std::nullopt;
    i++;

    Record rec;
    skip_ws(json, i);
    if (i < json.size() && json[i] == '}') return rec;  // empty object

    while (i < json.size()) {
        skip_ws(json, i);
        const auto key = read_string(json, i);
        if (!key.has_value()) return std::nullopt;

        skip_ws(json, i);
        if (i >= json.size() || json[i] != ':') return std::nullopt;
        i++;
        skip_ws(json, i);
        if (i >= json.size()) return std::nullopt;

        if (json[i] == '"') {
            const auto value = read_string(json, i);
            if (!value.has_value()) return std::nullopt;
            if (*key == "host") rec.host = *value;
        } else {
            const auto raw = read_scalar(json, i);
            if (!raw.has_value()) return std::nullopt;
            if (*key == "udp") {
                if (*raw == "true") rec.udp = true;
                else if (*raw == "false") rec.udp = false;
                // any other shape leaves udp unset
            } else if (*key == "port") {
                const std::string text(*raw);
                char* end = nullptr;
                const long v = std::strtol(text.c_str(), &end, 10);
                if (end != nullptr && *end == '\0' && v >= 1 && v <= 65535) {
                    rec.port = static_cast<uint16_t>(v);
                }
            }
        }

        skip_ws(json, i);
        if (i >= json.size()) return std::nullopt;
        if (json[i] == ',') { i++; continue; }
        if (json[i] == '}') return rec;
        return std::nullopt;
    }
    return std::nullopt;  // never saw the closing brace
}

}  // namespace holesail
```

- [ ] **Step 5: Wire it into the build**

```cmake
target_sources(holesail_lib PRIVATE src/version.cpp src/keys.cpp src/record.cpp)
```

```cmake
    holesail_add_test(test_record)
```

- [ ] **Step 6: Run the tests to verify they pass**

```bash
nix develop --command bash -c "cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug && cmake --build build && cd build && ctest --output-on-failure"
```

Expected: all `test_record` cases PASS, earlier suites still PASS.

- [ ] **Step 7: Commit**

```bash
git add include/holesail/record.hpp src/record.cpp test/test_record.cpp CMakeLists.txt
git commit -m "feat: DHT metadata record encode/decode matching JS JSON.stringify"
```

---

### Task 5: Logger and ANSI colours

**Files:**
- Create: `include/holesail/colors.hpp` (header-only)
- Create: `include/holesail/logger.hpp`, `src/logger.cpp`
- Modify: `CMakeLists.txt`
- Test: `test/test_logger.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces:
  ```cpp
  namespace holesail::colors {
  std::string red(std::string_view), green(...), yellow(...), blue(...),
              magenta(...), cyan(...), bright_black(...),
              bold(...), dim(...), underline(...);
  }
  namespace holesail {
  enum class Level { Debug = 0, Info = 1, Warn = 2, Error = 3 };
  class Logger {
  public:
      Logger(std::string prefix, bool enabled, Level min_level);
      void log(Level level, std::string_view msg) const;
      void debug(std::string_view) const;  void info(std::string_view) const;
      void warn(std::string_view) const;   void error(std::string_view) const;
      bool enabled() const;
      // Seam for tests: replace the sink (default writes to stdout/stderr).
      void set_sink(std::function<void(Level, std::string_view line)> sink);
      // Seam for tests: fixed timestamp instead of the wall clock.
      void set_clock(std::function<std::string()> clock);
      static std::string iso8601_now();
  };
  }
  ```
  Every later task takes a `Logger&`.

- [ ] **Step 1: Write the failing test**

Create `test/test_logger.cpp`:

```cpp
// SPDX-License-Identifier: AGPL-3.0-or-later
// Part of holesail-cpp — a C++ port of holesail (https://github.com/holesail/holesail)
#include <gtest/gtest.h>

#include "holesail/colors.hpp"
#include "holesail/logger.hpp"

#include <regex>
#include <vector>

using namespace holesail;

namespace {

struct Captured { Level level; std::string line; };

Logger make_capturing(bool enabled, Level min_level, std::vector<Captured>& out) {
    Logger log("Holesail", enabled, min_level);
    log.set_clock([] { return std::string("2026-07-28T00:00:00.000Z"); });
    log.set_sink([&out](Level l, std::string_view line) {
        out.push_back({l, std::string(line)});
    });
    return log;
}

}  // namespace

TEST(Colors, MatchBarelyColoursCodes) {
    EXPECT_EQ(colors::red("x"), "\x1b[31mx\x1b[0m");
    EXPECT_EQ(colors::green("x"), "\x1b[32mx\x1b[0m");
    EXPECT_EQ(colors::yellow("x"), "\x1b[33mx\x1b[0m");
    EXPECT_EQ(colors::blue("x"), "\x1b[34mx\x1b[0m");
    EXPECT_EQ(colors::magenta("x"), "\x1b[35mx\x1b[0m");
    EXPECT_EQ(colors::cyan("x"), "\x1b[36mx\x1b[0m");
    EXPECT_EQ(colors::bright_black("x"), "\x1b[90mx\x1b[0m");
    EXPECT_EQ(colors::bold("x"), "\x1b[1mx\x1b[0m");
    EXPECT_EQ(colors::dim("x"), "\x1b[2mx\x1b[0m");
    EXPECT_EQ(colors::underline("x"), "\x1b[4mx\x1b[0m");
}

// JS nests colour calls and every helper appends a full reset, so the
// nesting produces several trailing resets. Reproduce it.
TEST(Colors, NestingProducesStackedResets) {
    EXPECT_EQ(colors::cyan(colors::underline(colors::bold("x"))),
              "\x1b[36m\x1b[4m\x1b[1mx\x1b[0m\x1b[0m\x1b[0m");
}

TEST(Logger, FormatMatchesHolesailLogger) {
    std::vector<Captured> out;
    auto log = make_capturing(true, Level::Debug, out);
    log.info("Starting server");
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].line,
              "\x1b[90m2026-07-28T00:00:00.000Z\x1b[0m "
              "\x1b[34m[Holesail]\x1b[0m "
              "\x1b[32m[INFO]\x1b[0m Starting server");
}

TEST(Logger, LevelColorsAndLabels) {
    std::vector<Captured> out;
    auto log = make_capturing(true, Level::Debug, out);
    log.debug("d"); log.info("i"); log.warn("w"); log.error("e");
    ASSERT_EQ(out.size(), 4u);
    EXPECT_NE(out[0].line.find("\x1b[90m[DEBUG]\x1b[0m"), std::string::npos);
    EXPECT_NE(out[1].line.find("\x1b[32m[INFO]\x1b[0m"), std::string::npos);
    EXPECT_NE(out[2].line.find("\x1b[33m[WARN]\x1b[0m"), std::string::npos);
    EXPECT_NE(out[3].line.find("\x1b[31m[ERROR]\x1b[0m"), std::string::npos);
}

TEST(Logger, DropsMessagesBelowMinLevel) {
    std::vector<Captured> out;
    auto log = make_capturing(true, Level::Warn, out);
    log.debug("no"); log.info("no"); log.warn("yes"); log.error("yes");
    EXPECT_EQ(out.size(), 2u);
}

TEST(Logger, DisabledEmitsNothing) {
    std::vector<Captured> out;
    auto log = make_capturing(false, Level::Debug, out);
    log.error("boom");
    EXPECT_TRUE(out.empty());
}

TEST(Logger, Iso8601HasMillisecondsAndZSuffix) {
    const std::string ts = Logger::iso8601_now();
    EXPECT_TRUE(std::regex_match(
        ts, std::regex(R"(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z)")))
        << "got: " << ts;
}
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
nix develop --command bash -c "cmake --build build --target test_logger"
```

Expected: `fatal error: holesail/colors.hpp: No such file or directory`.

- [ ] **Step 3: Write `include/holesail/colors.hpp`**

```cpp
// SPDX-License-Identifier: AGPL-3.0-or-later
// Part of holesail-cpp — a C++ port of holesail (https://github.com/holesail/holesail)
#pragma once

#include <string>
#include <string_view>

// Port of the `barely-colours` module. Every helper wraps its argument in an
// SGR code and a full reset (SGR 0), exactly as the JS does — including the
// stacked resets that nesting produces. There is no TTY detection in the
// original and none here.
namespace holesail::colors {

inline std::string wrap(std::string_view code, std::string_view s) {
    std::string out;
    out.reserve(code.size() + s.size() + 4);
    out += code;
    out += s;
    out += "\x1b[0m";
    return out;
}

inline std::string red(std::string_view s)          { return wrap("\x1b[31m", s); }
inline std::string green(std::string_view s)        { return wrap("\x1b[32m", s); }
inline std::string yellow(std::string_view s)       { return wrap("\x1b[33m", s); }
inline std::string blue(std::string_view s)         { return wrap("\x1b[34m", s); }
inline std::string magenta(std::string_view s)      { return wrap("\x1b[35m", s); }
inline std::string cyan(std::string_view s)         { return wrap("\x1b[36m", s); }
inline std::string bright_black(std::string_view s) { return wrap("\x1b[90m", s); }
inline std::string bold(std::string_view s)         { return wrap("\x1b[1m", s); }
inline std::string dim(std::string_view s)          { return wrap("\x1b[2m", s); }
inline std::string underline(std::string_view s)    { return wrap("\x1b[4m", s); }

}  // namespace holesail::colors
```

- [ ] **Step 4: Write `include/holesail/logger.hpp`**

```cpp
// SPDX-License-Identifier: AGPL-3.0-or-later
// Part of holesail-cpp — a C++ port of holesail (https://github.com/holesail/holesail)
#pragma once

#include <functional>
#include <string>
#include <string_view>

namespace holesail {

// Mirrors holesail-logger: DEBUG 0, INFO 1, WARN 2, ERROR 3.
enum class Level { Debug = 0, Info = 1, Warn = 2, Error = 3 };

// Emits `<ISO8601> [prefix] [LEVEL] message`. DEBUG and INFO go to stdout,
// WARN and ERROR to stderr, matching the JS console.log/warn/error split.
class Logger {
public:
    Logger(std::string prefix, bool enabled, Level min_level);

    void log(Level level, std::string_view msg) const;
    void debug(std::string_view msg) const { log(Level::Debug, msg); }
    void info(std::string_view msg) const  { log(Level::Info, msg); }
    void warn(std::string_view msg) const  { log(Level::Warn, msg); }
    void error(std::string_view msg) const { log(Level::Error, msg); }

    bool enabled() const { return enabled_; }

    // Test seams. The defaults write to stdout/stderr and read the wall clock.
    void set_sink(std::function<void(Level, std::string_view)> sink) {
        sink_ = std::move(sink);
    }
    void set_clock(std::function<std::string()> clock) { clock_ = std::move(clock); }

    // "2026-07-28T00:44:12.345Z" — same shape as JS Date#toISOString.
    static std::string iso8601_now();

private:
    std::string prefix_;
    bool enabled_;
    Level min_level_;
    std::function<void(Level, std::string_view)> sink_;
    std::function<std::string()> clock_;
};

}  // namespace holesail
```

- [ ] **Step 5: Write `src/logger.cpp`**

```cpp
// SPDX-License-Identifier: AGPL-3.0-or-later
// Part of holesail-cpp — a C++ port of holesail (https://github.com/holesail/holesail)
#include "holesail/logger.hpp"

#include "holesail/colors.hpp"

#include <cstdio>
#include <ctime>
#include <sys/time.h>

namespace holesail {
namespace {

struct LevelStyle { const char* label; std::string (*color)(std::string_view); };

LevelStyle style_of(Level level) {
    switch (level) {
        case Level::Debug: return {"DEBUG", colors::bright_black};
        case Level::Info:  return {"INFO",  colors::green};
        case Level::Warn:  return {"WARN",  colors::yellow};
        case Level::Error: return {"ERROR", colors::red};
    }
    return {"INFO", colors::green};
}

void default_sink(Level level, std::string_view line) {
    std::FILE* out = (level == Level::Warn || level == Level::Error) ? stderr : stdout;
    std::fprintf(out, "%.*s\n", static_cast<int>(line.size()), line.data());
    std::fflush(out);
}

}  // namespace

Logger::Logger(std::string prefix, bool enabled, Level min_level)
    : prefix_(std::move(prefix)),
      enabled_(enabled),
      min_level_(min_level),
      sink_(default_sink),
      clock_(&Logger::iso8601_now) {}

std::string Logger::iso8601_now() {
    struct timeval tv{};
    gettimeofday(&tv, nullptr);
    struct tm utc{};
    const time_t secs = tv.tv_sec;
    gmtime_r(&secs, &utc);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                  utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
                  utc.tm_hour, utc.tm_min, utc.tm_sec,
                  static_cast<int>(tv.tv_usec / 1000));
    return std::string(buf);
}

void Logger::log(Level level, std::string_view msg) const {
    if (!enabled_) return;
    if (static_cast<int>(level) < static_cast<int>(min_level_)) return;

    const LevelStyle style = style_of(level);
    std::string line = colors::bright_black(clock_());
    line += ' ';
    line += colors::blue("[" + prefix_ + "]");
    line += ' ';
    line += style.color(std::string("[") + style.label + "]");
    line += ' ';
    line += msg;
    sink_(level, line);
}

}  // namespace holesail
```

- [ ] **Step 6: Wire it into the build and run the tests**

```cmake
target_sources(holesail_lib PRIVATE
    src/version.cpp src/keys.cpp src/record.cpp src/logger.cpp)
```

```cmake
    holesail_add_test(test_logger)
```

```bash
nix develop --command bash -c "cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug && cmake --build build && cd build && ctest --output-on-failure"
```

Expected: all `test_logger` cases PASS, earlier suites still PASS.

- [ ] **Step 7: Commit**

```bash
git add include/holesail/colors.hpp include/holesail/logger.hpp src/logger.cpp test/test_logger.cpp CMakeLists.txt
git commit -m "feat: holesail-logger format and barely-colours ANSI helpers"
```

---

### Task 6: TCP pipe and proxy

The bidirectional bridge between a local `uv_tcp_t` and an encrypted stream.
`StreamSide` is a plain struct of callbacks so the pipe can be tested without a
DHT — the real wiring (Tasks 8 and 9) fills it with `hyperdht_stream_*` calls,
the tests fill it with a double.

**Files:**
- Create: `include/holesail/pipe.hpp`, `src/pipe.cpp`
- Modify: `CMakeLists.txt`
- Test: `test/test_pipe.cpp`

**Interfaces:**
- Consumes: `holesail::Logger` (Task 5).
- Produces:
  ```cpp
  namespace holesail {
  struct StreamSide {
      // Returns 1 when drained, 0 under backpressure, negative on error.
      // on_drain fires once when a backpressured write completes.
      std::function<int(const uint8_t* data, size_t len, std::function<void()> on_drain)> write;
      std::function<void()> pause;    // stop delivering to on_data
      std::function<void()> resume;
      std::function<void()> close;    // half-close (write_end), then teardown
  };

  class TcpPipe {
  public:
      TcpPipe(uv_loop_t* loop, StreamSide stream, Logger& logger);
      ~TcpPipe();
      // Connect to a local endpoint and start piping. Server side.
      int connect(const std::string& host, uint16_t port, std::function<void(int)> on_ready);
      // Adopt an already-accepted socket and start piping. Client side.
      int adopt(uv_tcp_t* accepted);
      // Feed data that arrived on the encrypted stream.
      void on_stream_data(const uint8_t* data, size_t len);
      // The peer closed the encrypted stream.
      void on_stream_close();
      void destroy();
      void set_on_destroy(std::function<void()> cb);
  };

  class TcpProxy {
  public:
      TcpProxy(uv_loop_t* loop, Logger& logger);
      int listen(const std::string& host, uint16_t port,
                 std::function<void(uv_tcp_t* accepted)> on_connection);
      void close();
      uint16_t bound_port() const;
  };
  }
  ```
  Task 8 uses `TcpPipe::connect`; Task 9 uses `TcpProxy` + `TcpPipe::adopt`.

**Behaviour to implement (from `@holesail/hyper-cmd-lib-net` `connPiper`):**

- local data → `stream.write(...)`; if it returns 0, `uv_read_stop` the socket and
  `uv_read_start` again inside `on_drain`.
- `on_stream_data` → `uv_write` to the socket; if the write does not complete
  synchronously, call `stream.pause()` and `stream.resume()` in the write callback.
- local EOF (`nread == UV_EOF`) → `stream.close()` (half-close).
- `on_stream_close` → `uv_shutdown` the socket, then close it.
- Any error on either side → `destroy()` both, exactly once.
- `destroy()` must be idempotent, and must not free the pipe while a `uv_write`
  or `uv_shutdown` request is still in flight — close the handle and free in the
  `uv_close` callback.

- [ ] **Step 1: Write the failing test**

Create `test/test_pipe.cpp`. It drives a real loopback TCP server with libuv and
a `StreamSide` double, so no DHT is involved.

```cpp
// SPDX-License-Identifier: AGPL-3.0-or-later
// Part of holesail-cpp — a C++ port of holesail (https://github.com/holesail/holesail)
#include <gtest/gtest.h>

#include "holesail/logger.hpp"
#include "holesail/pipe.hpp"

#include <uv.h>

#include <cstring>
#include <string>
#include <vector>

using namespace holesail;

namespace {

// A minimal echo server on 127.0.0.1, standing in for the "local application".
// Echoes every byte it receives and records the total in `received`.
struct EchoServer {
    uv_loop_t* loop;
    uv_tcp_t handle{};
    uint16_t port = 0;
    std::string received;
    std::vector<uv_tcp_t*> clients;

    explicit EchoServer(uv_loop_t* l) : loop(l) {
        uv_tcp_init(loop, &handle);
        handle.data = this;
        struct sockaddr_in addr;
        uv_ip4_addr("127.0.0.1", 0, &addr);
        uv_tcp_bind(&handle, reinterpret_cast<const struct sockaddr*>(&addr), 0);
        int len = sizeof(addr);
        uv_tcp_getsockname(&handle, reinterpret_cast<struct sockaddr*>(&addr), &len);
        port = ntohs(addr.sin_port);
        uv_listen(reinterpret_cast<uv_stream_t*>(&handle), 8, on_conn);
    }

    void close() {
        for (uv_tcp_t* c : clients) {
            if (uv_is_closing(reinterpret_cast<uv_handle_t*>(c)) == 0) {
                uv_close(reinterpret_cast<uv_handle_t*>(c), on_client_closed);
            }
        }
        clients.clear();
        if (uv_is_closing(reinterpret_cast<uv_handle_t*>(&handle)) == 0) {
            uv_close(reinterpret_cast<uv_handle_t*>(&handle), nullptr);
        }
    }

    static void on_client_closed(uv_handle_t* h) { delete reinterpret_cast<uv_tcp_t*>(h); }

    static void on_conn(uv_stream_t* server, int status) {
        if (status != 0) return;
        auto* self = static_cast<EchoServer*>(server->data);
        auto* client = new uv_tcp_t{};
        uv_tcp_init(self->loop, client);
        client->data = self;
        if (uv_accept(server, reinterpret_cast<uv_stream_t*>(client)) != 0) {
            uv_close(reinterpret_cast<uv_handle_t*>(client), on_client_closed);
            return;
        }
        self->clients.push_back(client);
        uv_read_start(reinterpret_cast<uv_stream_t*>(client), on_alloc, on_read);
    }

    static void on_alloc(uv_handle_t*, size_t suggested, uv_buf_t* buf) {
        buf->base = new char[suggested];
        buf->len = suggested;
    }

    static void on_read(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
        auto* self = static_cast<EchoServer*>(stream->data);
        if (nread > 0) {
            self->received.append(buf->base, static_cast<size_t>(nread));
            // Echo it back. The write request owns its own copy.
            auto* payload = new std::string(buf->base, static_cast<size_t>(nread));
            auto* req = new uv_write_t{};
            req->data = payload;
            uv_buf_t out = uv_buf_init(payload->data(), static_cast<unsigned>(payload->size()));
            uv_write(req, stream, &out, 1, [](uv_write_t* r, int) {
                delete static_cast<std::string*>(r->data);
                delete r;
            });
        } else if (nread < 0) {
            // Stop reading but do NOT close here — `clients` still holds this
            // pointer and close() would then double-free it.
            uv_read_stop(stream);
        }
        delete[] buf->base;
    }
};

Logger silent_logger() { return Logger("Holesail", false, Level::Info); }

}  // namespace

TEST(TcpPipe, ForwardsLocalDataToTheStream) {
    uv_loop_t loop;
    ASSERT_EQ(uv_loop_init(&loop), 0);
    EchoServer echo(&loop);
    auto logger = silent_logger();

    std::vector<uint8_t> to_stream;
    StreamSide side;
    side.write = [&](const uint8_t* d, size_t n, std::function<void()>) {
        to_stream.insert(to_stream.end(), d, d + n);
        return 1;  // always drained
    };
    side.pause = [] {};
    side.resume = [] {};
    side.close = [] {};

    TcpPipe pipe(&loop, side, logger);
    bool ready = false;
    ASSERT_EQ(pipe.connect("127.0.0.1", echo.port, [&](int err) {
        EXPECT_EQ(err, 0);
        ready = true;
    }), 0);

    // Drive the loop until connected, then push data through the echo
    // server so it comes back up the pipe into `to_stream`.
    pipe.on_stream_data(reinterpret_cast<const uint8_t*>("ping"), 4);
    for (int i = 0; i < 200 && to_stream.size() < 4; i++) uv_run(&loop, UV_RUN_NOWAIT);

    EXPECT_TRUE(ready);
    EXPECT_EQ(std::string(to_stream.begin(), to_stream.end()), "ping");

    pipe.destroy();
    echo.close();
    uv_run(&loop, UV_RUN_DEFAULT);
    EXPECT_EQ(uv_loop_close(&loop), 0);
}

TEST(TcpPipe, StopsReadingWhenTheStreamBackpressures) {
    uv_loop_t loop;
    ASSERT_EQ(uv_loop_init(&loop), 0);
    EchoServer echo(&loop);
    auto logger = silent_logger();

    int writes = 0;
    std::function<void()> saved_drain;
    StreamSide side;
    side.write = [&](const uint8_t*, size_t, std::function<void()> on_drain) {
        writes++;
        saved_drain = std::move(on_drain);
        return 0;  // backpressure on every write
    };
    side.pause = [] {};
    side.resume = [] {};
    side.close = [] {};

    TcpPipe pipe(&loop, side, logger);
    ASSERT_EQ(pipe.connect("127.0.0.1", echo.port, [](int err) { EXPECT_EQ(err, 0); }), 0);

    // Push a payload big enough to arrive in several reads.
    std::vector<uint8_t> big(256 * 1024, 'x');
    pipe.on_stream_data(big.data(), big.size());
    for (int i = 0; i < 200; i++) uv_run(&loop, UV_RUN_NOWAIT);

    // With no drain callback fired, reading must have stopped after the
    // first backpressured write.
    EXPECT_EQ(writes, 1);
    ASSERT_TRUE(static_cast<bool>(saved_drain));

    saved_drain();
    for (int i = 0; i < 200 && writes < 2; i++) uv_run(&loop, UV_RUN_NOWAIT);
    EXPECT_GT(writes, 1);

    pipe.destroy();
    echo.close();
    uv_run(&loop, UV_RUN_DEFAULT);
    EXPECT_EQ(uv_loop_close(&loop), 0);
}

TEST(TcpPipe, LocalEofHalfClosesTheStream) {
    uv_loop_t loop;
    ASSERT_EQ(uv_loop_init(&loop), 0);
    auto logger = silent_logger();

    // Point at a port nothing listens on after an immediate accept-and-close
    // server, so the local side EOFs promptly.
    EchoServer echo(&loop);
    bool closed = false;
    StreamSide side;
    side.write = [](const uint8_t*, size_t, std::function<void()>) { return 1; };
    side.pause = [] {};
    side.resume = [] {};
    side.close = [&] { closed = true; };

    TcpPipe pipe(&loop, side, logger);
    ASSERT_EQ(pipe.connect("127.0.0.1", echo.port, [](int) {}), 0);
    for (int i = 0; i < 50; i++) uv_run(&loop, UV_RUN_NOWAIT);
    pipe.on_stream_close();
    echo.close();
    uv_run(&loop, UV_RUN_DEFAULT);

    // on_stream_close must tear the pipe down without leaving handles behind;
    // uv_loop_close returns non-zero if any handle is still open, so this
    // assertion is the leak check for the pipe's own uv_tcp_t.
    EXPECT_EQ(uv_loop_close(&loop), 0);
    (void)closed;
}

TEST(TcpPipe, DestroyIsIdempotent) {
    uv_loop_t loop;
    ASSERT_EQ(uv_loop_init(&loop), 0);
    auto logger = silent_logger();
    EchoServer echo(&loop);

    int closes = 0;
    StreamSide side;
    side.write = [](const uint8_t*, size_t, std::function<void()>) { return 1; };
    side.pause = [] {};
    side.resume = [] {};
    side.close = [&] { closes++; };

    TcpPipe pipe(&loop, side, logger);
    ASSERT_EQ(pipe.connect("127.0.0.1", echo.port, [](int) {}), 0);
    for (int i = 0; i < 50; i++) uv_run(&loop, UV_RUN_NOWAIT);

    pipe.destroy();
    pipe.destroy();
    pipe.destroy();
    echo.close();
    uv_run(&loop, UV_RUN_DEFAULT);
    EXPECT_LE(closes, 1);
    EXPECT_EQ(uv_loop_close(&loop), 0);
}

TEST(TcpProxy, ListensOnAnEphemeralPortAndAccepts) {
    uv_loop_t loop;
    ASSERT_EQ(uv_loop_init(&loop), 0);
    auto logger = silent_logger();

    TcpProxy proxy(&loop, logger);
    int accepted = 0;
    ASSERT_EQ(proxy.listen("127.0.0.1", 0, [&](uv_tcp_t* c) {
        accepted++;
        uv_close(reinterpret_cast<uv_handle_t*>(c), [](uv_handle_t* h) { delete reinterpret_cast<uv_tcp_t*>(h); });
    }), 0);
    ASSERT_NE(proxy.bound_port(), 0);

    uv_tcp_t client;
    uv_tcp_init(&loop, &client);
    struct sockaddr_in addr;
    uv_ip4_addr("127.0.0.1", proxy.bound_port(), &addr);
    uv_connect_t req;
    uv_tcp_connect(&req, &client, reinterpret_cast<const struct sockaddr*>(&addr),
                   [](uv_connect_t*, int status) { EXPECT_EQ(status, 0); });

    for (int i = 0; i < 200 && accepted == 0; i++) uv_run(&loop, UV_RUN_NOWAIT);
    EXPECT_EQ(accepted, 1);

    uv_close(reinterpret_cast<uv_handle_t*>(&client), nullptr);
    proxy.close();
    uv_run(&loop, UV_RUN_DEFAULT);
    uv_loop_close(&loop);
}
```

Complete the `EchoServer` helper — the sketch above omits its accept/read/write
body. Use the standard libuv echo idiom: `uv_accept` into a heap `uv_tcp_t`,
`uv_read_start` with an allocator that returns a fixed buffer, and `uv_write`
back what arrives.

- [ ] **Step 2: Run the test to verify it fails**

```bash
nix develop --command bash -c "cmake --build build --target test_pipe"
```

Expected: `fatal error: holesail/pipe.hpp: No such file or directory`.

- [ ] **Step 3: Write `include/holesail/pipe.hpp`**

```cpp
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
```

- [ ] **Step 4: Write `src/pipe.cpp`**

```cpp
// SPDX-License-Identifier: AGPL-3.0-or-later
// Part of holesail-cpp — a C++ port of holesail (https://github.com/holesail/holesail)
#include "holesail/pipe.hpp"

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
    rc = uv_ip4_addr(host.c_str(), port, &addr);
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
    const int rc = self->stream_.write(
        reinterpret_cast<const uint8_t*>(buf->base), len,
        [self] { self->resume_reading(); });

    if (rc < 0) {
        self->destroy();
        return;
    }
    if (rc == 0) {
        // Backpressure — stop reading until the drain callback fires.
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
    rc = uv_ip4_addr(host.c_str(), port, &addr);
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
```

Note the one deliberate divergence from a literal JS transcription: JS relies on
Node's `socket.write()` returning false to detect local congestion. libuv's
`uv_write` has no such return, so the pipe tracks queued bytes and pauses the
encrypted stream at `kHighWaterMark`. Same behaviour, different mechanism.

- [ ] **Step 5: Wire it into the build and run the tests**

```cmake
target_sources(holesail_lib PRIVATE
    src/version.cpp src/keys.cpp src/record.cpp src/logger.cpp src/pipe.cpp)
```

```cmake
    holesail_add_test(test_pipe)
```

```bash
nix develop --command bash -c "cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug && cmake --build build && cd build && ctest --output-on-failure"
```

Expected: all `test_pipe` cases PASS.

- [ ] **Step 6: Run it under sanitizers**

This is pointer-heavy libuv code — a clean sanitizer run is part of the
deliverable, not an optional extra.

```bash
nix develop --command bash -c "cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' && cmake --build build-asan && cd build-asan && ctest --output-on-failure"
```

Expected: PASS with no ASan/UBSan reports. Fix any that appear before committing.

- [ ] **Step 7: Commit**

```bash
git add include/holesail/pipe.hpp src/pipe.cpp test/test_pipe.cpp CMakeLists.txt
git commit -m "feat: TCP pipe and proxy with bidirectional backpressure"
```

---

### Task 7: UDP framing, pipe, and proxy

**Files:**
- Create: `include/holesail/udp_pipe.hpp`, `src/udp_pipe.cpp`
- Modify: `CMakeLists.txt`
- Test: `test/test_udp_pipe.cpp`

**Interfaces:**
- Consumes: `StreamSide` (Task 6), `Logger` (Task 5).
- Produces:
  ```cpp
  namespace holesail {
  // Datagrams are carried over the reliable stream as [uint32 BE length][payload].
  inline constexpr size_t kMaxDatagram = 65535;

  std::vector<uint8_t> frame(const uint8_t* data, size_t len);

  class FrameDecoder {
  public:
      // Feeds a chunk of stream bytes; invokes `on_frame` once per complete
      // frame. Returns false if a length header exceeds kMaxDatagram — the
      // caller must then destroy the stream.
      bool push(const uint8_t* data, size_t len,
                const std::function<void(const uint8_t*, size_t)>& on_frame);
      void reset();
      size_t buffered() const;
  };

  // Server side: one local UDP socket bridged to one encrypted stream.
  class UdpPipe {
  public:
      UdpPipe(uv_loop_t* loop, StreamSide stream, Logger& logger);
      int start(const std::string& host, uint16_t port);
      void on_stream_data(const uint8_t* data, size_t len);
      void on_stream_close();
      void destroy();
  };

  // Client side: one bound UDP socket fanning out to one stream per source.
  class UdpProxy {
  public:
      UdpProxy(uv_loop_t* loop, Logger& logger);
      int listen(const std::string& host, uint16_t port,
                 std::function<StreamSide(const std::string& client_id)> open_stream);
      void on_stream_data(const std::string& client_id, const uint8_t* data, size_t len);
      void on_stream_close(const std::string& client_id);
      void close();
      uint16_t bound_port() const;
  };
  }
  ```

**Divergence to implement (D5, add it to the spec's divergence table):** JS reads
a 32-bit length prefix and buffers without any bound, so a hostile peer can
request a multi-gigabyte allocation. The C++ port caps a frame at `kMaxDatagram`
(65535 — the largest possible UDP payload is 65507) and destroys the stream on
violation. Observable behaviour is identical for all legitimate traffic.

- [ ] **Step 1: Write the failing test**

Create `test/test_udp_pipe.cpp`:

```cpp
// SPDX-License-Identifier: AGPL-3.0-or-later
// Part of holesail-cpp — a C++ port of holesail (https://github.com/holesail/holesail)
#include <gtest/gtest.h>

#include "holesail/udp_pipe.hpp"

#include <cstring>
#include <string>
#include <vector>

using namespace holesail;

namespace {

std::vector<std::string> collect(FrameDecoder& dec, const std::vector<uint8_t>& chunk,
                                 bool* ok = nullptr) {
    std::vector<std::string> out;
    const bool r = dec.push(chunk.data(), chunk.size(),
                            [&](const uint8_t* d, size_t n) {
                                out.emplace_back(reinterpret_cast<const char*>(d), n);
                            });
    if (ok != nullptr) *ok = r;
    return out;
}

}  // namespace

TEST(Frame, PrefixesBigEndianLength) {
    const auto f = frame(reinterpret_cast<const uint8_t*>("hi"), 2);
    ASSERT_EQ(f.size(), 6u);
    EXPECT_EQ(f[0], 0x00); EXPECT_EQ(f[1], 0x00);
    EXPECT_EQ(f[2], 0x00); EXPECT_EQ(f[3], 0x02);
    EXPECT_EQ(f[4], 'h');  EXPECT_EQ(f[5], 'i');
}

TEST(Frame, ZeroLengthDatagramIsRepresentable) {
    const auto f = frame(nullptr, 0);
    ASSERT_EQ(f.size(), 4u);
    FrameDecoder dec;
    const auto got = collect(dec, f);
    ASSERT_EQ(got.size(), 1u);
    EXPECT_TRUE(got[0].empty());
}

TEST(FrameDecoder, DecodesSeveralFramesInOneChunk) {
    std::vector<uint8_t> buf;
    for (const char* s : {"a", "bb", "ccc"}) {
        const auto f = frame(reinterpret_cast<const uint8_t*>(s), strlen(s));
        buf.insert(buf.end(), f.begin(), f.end());
    }
    FrameDecoder dec;
    const auto got = collect(dec, buf);
    ASSERT_EQ(got.size(), 3u);
    EXPECT_EQ(got[0], "a");
    EXPECT_EQ(got[1], "bb");
    EXPECT_EQ(got[2], "ccc");
}

TEST(FrameDecoder, ReassemblesAFrameSplitAcrossChunks) {
    const auto f = frame(reinterpret_cast<const uint8_t*>("hello world"), 11);
    FrameDecoder dec;
    for (size_t cut = 1; cut < f.size(); cut++) {
        dec.reset();
        const std::vector<uint8_t> a(f.begin(), f.begin() + cut);
        const std::vector<uint8_t> b(f.begin() + cut, f.end());
        auto got = collect(dec, a);
        EXPECT_TRUE(got.empty()) << "premature frame at cut " << cut;
        got = collect(dec, b);
        ASSERT_EQ(got.size(), 1u) << "missing frame at cut " << cut;
        EXPECT_EQ(got[0], "hello world");
    }
}

TEST(FrameDecoder, HandlesALengthHeaderSplitByteByByte) {
    const auto f = frame(reinterpret_cast<const uint8_t*>("x"), 1);
    FrameDecoder dec;
    std::vector<std::string> got;
    for (size_t i = 0; i < f.size(); i++) {
        dec.push(&f[i], 1, [&](const uint8_t* d, size_t n) {
            got.emplace_back(reinterpret_cast<const char*>(d), n);
        });
    }
    ASSERT_EQ(got.size(), 1u);
    EXPECT_EQ(got[0], "x");
}

TEST(FrameDecoder, RejectsAnOversizedLengthHeader) {
    // 0xFFFFFFFF would be a 4 GiB allocation in the JS implementation.
    const std::vector<uint8_t> hostile = {0xff, 0xff, 0xff, 0xff};
    FrameDecoder dec;
    bool ok = true;
    const auto got = collect(dec, hostile, &ok);
    EXPECT_FALSE(ok);
    EXPECT_TRUE(got.empty());
    EXPECT_LE(dec.buffered(), 4u) << "must not buffer toward the claimed length";
}

TEST(FrameDecoder, AcceptsTheLargestLegalDatagram) {
    const std::vector<uint8_t> payload(kMaxDatagram, 'z');
    const auto f = frame(payload.data(), payload.size());
    FrameDecoder dec;
    bool ok = false;
    const auto got = collect(dec, f, &ok);
    EXPECT_TRUE(ok);
    ASSERT_EQ(got.size(), 1u);
    EXPECT_EQ(got[0].size(), kMaxDatagram);
}
```

Add socket-level tests for `UdpPipe` and `UdpProxy` following the loopback
pattern from Task 6: bind an ephemeral `uv_udp_t`, send a datagram, assert the
framed bytes reach the `StreamSide` double, and assert that framed bytes fed
back through `on_stream_data` arrive at the sender's address. Cover two distinct
source addresses hitting `UdpProxy` and assert two separate streams are opened.

- [ ] **Step 2: Run the test to verify it fails**

```bash
nix develop --command bash -c "cmake --build build --target test_udp_pipe"
```

Expected: `fatal error: holesail/udp_pipe.hpp: No such file or directory`.

- [ ] **Step 3: Write the framing codec**

This is the part that faces hostile input, so it is given in full. Put it at the
top of `include/holesail/udp_pipe.hpp` / `src/udp_pipe.cpp`.

Header:

```cpp
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
```

Implementation of the codec:

```cpp
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
        // makes it buffer toward 4 GiB. We refuse instead.
        if (frame_len > kMaxDatagram) {
            buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<long>(offset));
            return false;
        }
        if (buffer_.size() - offset < 4 + frame_len) break;  // incomplete, wait

        on_frame(buffer_.data() + offset + 4, frame_len);
        offset += 4 + frame_len;
    }

    if (offset > 0) {
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<long>(offset));
    }
    return true;
}
```

- [ ] **Step 4: Write the socket layer**

Complete `UdpPipe` and `UdpProxy` in `src/udp_pipe.cpp` per the header. Notes:

- `UdpPipe::start` binds an ephemeral local port and stores `(host, port)` as the
  send destination — mirroring `pipeUdpFramedServer`, which uses one unbound
  socket per stream and sends to the local service.
- `UdpProxy` keys its client map on `"address:port"`, formatted exactly as JS
  does with `${rinfo.address}:${rinfo.port}`; use `uv_ip4_name` for the address.
- Every `uv_udp_send_t` owns its buffer copy — libuv does not copy. Free both
  in the send callback, following the `WriteCtx` pattern from `pipe.cpp`.
- `recv_cb` fires with `nread == 0 && addr == nullptr` to signal "no more data
  this pass" — return early, do not treat it as an empty datagram.
- When `FrameDecoder::push` returns false, log an error and destroy the stream
  (`stream.close()`), then drop the client from the map.
- Both classes share the one-scratch-buffer allocator idiom from `TcpPipe`.

- [ ] **Step 5: Wire it into the build, run tests, run sanitizers**

```cmake
target_sources(holesail_lib PRIVATE
    src/version.cpp src/keys.cpp src/record.cpp src/logger.cpp
    src/pipe.cpp src/udp_pipe.cpp)
```

```cmake
    holesail_add_test(test_udp_pipe)
```

```bash
nix develop --command bash -c "cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug && cmake --build build && cd build && ctest --output-on-failure"
nix develop --command bash -c "cmake --build build-asan && cd build-asan && ctest --output-on-failure"
```

Expected: both PASS, no sanitizer reports.

- [ ] **Step 6: Record the divergence in the spec**

Add to the divergence table in
`docs/superpowers/specs/2026-07-28-holesail-cpp-port-design.md`:

```markdown
| D5 | UDP frames are capped at 65535 bytes; an oversized length header destroys the stream | JS buffers toward an unbounded 32-bit length, so a hostile peer can force a multi-gigabyte allocation. No legitimate UDP datagram exceeds 65507 bytes, so behaviour is unchanged for real traffic. |
```

- [ ] **Step 7: Commit**

```bash
git add include/holesail/udp_pipe.hpp src/udp_pipe.cpp test/test_udp_pipe.cpp CMakeLists.txt docs/superpowers/specs/2026-07-28-holesail-cpp-port-design.md
git commit -m "feat: framed UDP pipe and proxy with a bounded frame length"
```

---

### Lifetime constraints on StreamSide — read before Tasks 8 and 9

Surfaced while implementing Task 6. Both server and client build a `StreamSide`
over a `hyperdht_stream_t*`, and both are subject to these:

1. **The buffer handed to `StreamSide::write` is borrowed, not owned.**
   `TcpPipe::read_cb` passes a pointer into the pipe's per-pipe `scratch_`
   vector, valid only for the duration of that call. `hyperdht_stream_write*`
   copies synchronously, so the direct wiring is safe — but any wrapper that
   *defers* the write (a queue, a retry, a lambda that outlives the call) MUST
   copy first.

2. **The drain lambda captures a raw `TcpPipe*`.** The owner must not destroy a
   pipe while a backpressured write is still outstanding, or the drain callback
   fires into freed memory. `TcpPipe::resume_reading()` guards the
   destroyed-but-not-yet-deleted window; it cannot guard the already-deleted
   one. Practically: drop the pipe from the owning map only from its
   `on_destroy` callback, never eagerly on stream close.

3. **`hyperdht_stream_write_with_drain` needs a heap context** holding the
   `std::function`, freed in the callback — and that callback must check whether
   the stream has closed in the meantime before touching the pipe.

---

### Task 8: HolesailServer

**Files:**
- Create: `include/holesail/server.hpp`, `src/server.cpp`
- Modify: `CMakeLists.txt`
- Test: `test/test_server.cpp`

**Interfaces:**
- Consumes: `keys.hpp`, `record.hpp`, `logger.hpp`, `pipe.hpp`, `udp_pipe.hpp`.
- Produces:
  ```cpp
  namespace holesail {
  class HolesailServer {
  public:
      struct Config {
          uint16_t local_port = 0;
          std::string local_host = "127.0.0.1";
          std::string seed_hex;      // 64 hex chars; empty means "generate"
          bool secure = true;
          bool udp = false;
      };
      HolesailServer(uv_loop_t* loop, hyperdht_t* dht, Config config, Logger& logger);
      ~HolesailServer();
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
  };
  }
  ```

**Behaviour (from `holesail-server/index.js`):**

1. `generateKeyPair(seed)` — `seed_hex` empty → `random_hex32()`. Decode the hex
   to 32 bytes, `hyperdht_keypair_from_seed`.
2. Secure mode → `hyperdht_server_set_firewall` rejecting any peer whose public
   key differs from ours.
3. `hyperdht_server_set_reusable_socket(srv, 1)`.
4. `hyperdht_server_listen(srv, &kp, on_connection, this)`.
5. Publish the record: `mutable_get` latest first, compute the seq with
   `next_seq`, then `mutable_put`. Repeat on a `uv_timer_t` every
   `kRefreshIntervalMs`.
6. Per inbound connection: open a stream with `hyperdht_stream_open`, build a
   `StreamSide` over it, and hand it to a `TcpPipe::connect(local_host, local_port)`
   or a `UdpPipe::start(...)` depending on `config.udp`.

- [ ] **Step 1: Write the failing test**

Create `test/test_server.cpp`. It covers the pure decision logic without joining
the public DHT (which would make tests slow and flaky):

```cpp
// SPDX-License-Identifier: AGPL-3.0-or-later
// Part of holesail-cpp — a C++ port of holesail (https://github.com/holesail/holesail)
#include <gtest/gtest.h>

#include "holesail/keys.hpp"
#include "holesail/server.hpp"

using namespace holesail;

// JS: seq = old ? (same ? old.seq : old.seq + 1) : <default>
TEST(ServerSeq, MatchesJsPutRule) {
    EXPECT_EQ(HolesailServer::next_seq(false, 0, false), 0u);  // no prior record
    EXPECT_EQ(HolesailServer::next_seq(true, 7, true), 7u);    // unchanged value
    EXPECT_EQ(HolesailServer::next_seq(true, 7, false), 8u);   // changed value
}

TEST(ServerRefresh, IntervalIsFiftyMinutes) {
    EXPECT_EQ(HolesailServer::kRefreshIntervalMs, 3000000u);
}

TEST(ServerKey, SecureModeKeyIsZ32OfTheSeed) {
    uv_loop_t loop;
    ASSERT_EQ(uv_loop_init(&loop), 0);
    Logger logger("Holesail", false, Level::Info);

    HolesailServer::Config cfg;
    cfg.local_port = 8080;
    cfg.secure = true;
    cfg.seed_hex = "a4e1504af3323f7665dd1ea745f7c6f86d3ad32a0eca5988fbac19f0d31376f0";

    HolesailServer srv(&loop, nullptr, cfg, logger);
    EXPECT_EQ(srv.key(), "wuoiy1zuge9zc3q7d4uwm76g9bsuiw3kb5ffun85ioc9bwauq5ay");
    EXPECT_EQ(to_hex(srv.public_key().data(), 32),
              "c5fffd407ea7d83056da8e193f33967b2d9d42b5f4a9d54757f0253a1ce56a30");
    uv_loop_close(&loop);
}

TEST(ServerKey, PublicModeKeyIsZ32OfThePublicKey) {
    uv_loop_t loop;
    ASSERT_EQ(uv_loop_init(&loop), 0);
    Logger logger("Holesail", false, Level::Info);

    HolesailServer::Config cfg;
    cfg.local_port = 8080;
    cfg.secure = false;
    cfg.seed_hex = "a4e1504af3323f7665dd1ea745f7c6f86d3ad32a0eca5988fbac19f0d31376f0";

    HolesailServer srv(&loop, nullptr, cfg, logger);
    EXPECT_EQ(srv.key(), "az994od6w9cdyis4tacu6chsxcs34oii61w7kt4z6y1uw88fpeay");
    uv_loop_close(&loop);
}

TEST(ServerKey, EmptySeedGeneratesADistinctIdentityEachTime) {
    uv_loop_t loop;
    ASSERT_EQ(uv_loop_init(&loop), 0);
    Logger logger("Holesail", false, Level::Info);
    HolesailServer::Config cfg;
    cfg.local_port = 1;
    HolesailServer a(&loop, nullptr, cfg, logger);
    HolesailServer b(&loop, nullptr, cfg, logger);
    EXPECT_NE(a.key(), b.key());
    uv_loop_close(&loop);
}
```

The constructor must therefore derive the keypair without touching the `dht`
pointer — accept `nullptr` there and only dereference it in `start()`.

- [ ] **Step 2: Run the test to verify it fails**

```bash
nix develop --command bash -c "cmake --build build --target test_server"
```

Expected: `fatal error: holesail/server.hpp: No such file or directory`.

- [ ] **Step 3: Write the header and implementation**

Write `include/holesail/server.hpp` and `src/server.cpp` per the Interfaces and
Behaviour blocks. Notes:

- The firewall callback signature is
  `int (*)(const uint8_t remote_pk[32], const char* host, uint16_t port, void* userdata)`
  returning non-zero to reject — confirm against
  `hyperdht_server_set_firewall` in `../hyperdht-cpp/include/hyperdht/hyperdht.h:436`
  and use `sodium_memcmp` for the comparison, not `memcmp`.
- Build `StreamSide` over a `hyperdht_stream_t*` like this:
  ```cpp
  StreamSide side;
  side.write = [s](const uint8_t* d, size_t n, std::function<void()> on_drain) {
      // hyperdht_stream_write_with_drain returns 1 (drained), 0 (backpressure),
      // or negative (error) — see hyperdht-cpp CLAUDE.md gotcha 11.
      return hyperdht_stream_write_with_drain(s, d, n, &fire_drain, ctx);
  };
  side.pause  = [s] { hyperdht_stream_pause(s); };
  side.resume = [s] { hyperdht_stream_resume(s); };
  side.close  = [s] { hyperdht_stream_close(s); };
  ```
  The drain callback needs a heap context holding the `std::function`; free it in
  the callback. Guard against the stream having closed in the meantime.
- Keep a `std::map<hyperdht_stream_t*, std::unique_ptr<TcpPipe>>` (or the UDP
  equivalent) so pipes are destroyed when the stream closes and on `destroy()`.
- `destroy()`: stop the refresh timer, tear down every pipe, then
  `hyperdht_server_close_force`. Order matters — closing the server first can
  fire connection callbacks into freed pipes.

- [ ] **Step 4: Run the tests**

```cmake
target_sources(holesail_lib PRIVATE ... src/server.cpp)
```
```cmake
    holesail_add_test(test_server)
```
```bash
nix develop --command bash -c "cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug && cmake --build build && cd build && ctest --output-on-failure"
```

Expected: all `test_server` cases PASS.

- [ ] **Step 5: Commit**

```bash
git add include/holesail/server.hpp src/server.cpp test/test_server.cpp CMakeLists.txt
git commit -m "feat: HolesailServer with firewall, record publish and refresh"
```

---

### Task 9: HolesailClient

**Files:**
- Create: `include/holesail/client.hpp`, `src/client.cpp`
- Modify: `CMakeLists.txt`
- Test: `test/test_client.cpp`

**Interfaces:**
- Consumes: `keys.hpp`, `record.hpp`, `logger.hpp`, `pipe.hpp`, `udp_pipe.hpp`.
- Produces:
  ```cpp
  namespace holesail {
  class HolesailClient {
  public:
      struct Config {
          std::string key;                    // the connection key (post-url-parse)
          bool secure = false;
          std::optional<uint16_t> local_port; // --port
          std::optional<std::string> local_host;  // --host
          std::optional<bool> udp;            // --udp
      };
      struct Resolved { uint16_t port; std::string host; bool udp; };

      HolesailClient(uv_loop_t* loop, hyperdht_t* dht, Config config, Logger& logger);
      ~HolesailClient();
      int connect(std::function<void(int err)> on_listening);
      void pause();
      void resume();
      void destroy();
      std::string state() const;
      const std::array<uint8_t, 32>& target_public_key() const;
      const Resolved& resolved() const;
      // Whether this client presents the derived keypair as its own identity.
      bool uses_derived_identity() const { return config_.secure; }

      // JS: options.port ?? record.port ?? 8989, etc. Pure — tested directly.
      static Resolved resolve(const Config& config, const std::optional<Record>& record);
      static constexpr uint16_t kDefaultPort = 8989;
  };
  }
  ```

- [ ] **Step 1: Write the failing test**

Create `test/test_client.cpp`:

```cpp
// SPDX-License-Identifier: AGPL-3.0-or-later
// Part of holesail-cpp — a C++ port of holesail (https://github.com/holesail/holesail)
#include <gtest/gtest.h>

#include "holesail/client.hpp"
#include "holesail/keys.hpp"

using namespace holesail;

TEST(ClientResolve, PrefersCliOverRecordOverDefaults) {
    HolesailClient::Config cfg;
    Record rec;
    rec.host = "10.0.0.5";
    rec.port = 3000;
    rec.udp = true;

    auto r = HolesailClient::resolve(cfg, rec);
    EXPECT_EQ(r.port, 3000);
    EXPECT_EQ(r.host, "10.0.0.5");
    EXPECT_TRUE(r.udp);

    cfg.local_port = 9999;
    cfg.local_host = "127.0.0.1";
    cfg.udp = false;
    r = HolesailClient::resolve(cfg, rec);
    EXPECT_EQ(r.port, 9999);
    EXPECT_EQ(r.host, "127.0.0.1");
    EXPECT_FALSE(r.udp);
}

TEST(ClientResolve, FallsBackToJsDefaultsWithNoRecord) {
    const HolesailClient::Config cfg;
    const auto r = HolesailClient::resolve(cfg, std::nullopt);
    EXPECT_EQ(r.port, 8989);
    EXPECT_EQ(r.host, "127.0.0.1");
    EXPECT_FALSE(r.udp);
}

TEST(ClientResolve, RecordWithoutUdpKeyMeansTcp) {
    Record rec;
    rec.host = "127.0.0.1";
    rec.port = 8080;  // udp deliberately unset — the common JS record
    const HolesailClient::Config cfg;
    const auto r = HolesailClient::resolve(cfg, rec);
    EXPECT_FALSE(r.udp);
    EXPECT_EQ(r.port, 8080);
}

TEST(ClientTarget, SecureModeDerivesTheServersKeypair) {
    uv_loop_t loop;
    ASSERT_EQ(uv_loop_init(&loop), 0);
    Logger logger("Holesail", false, Level::Info);

    HolesailClient::Config cfg;
    cfg.key = "this-is-a-32-char-holesail-key!!";
    cfg.secure = true;

    HolesailClient c(&loop, nullptr, cfg, logger);
    EXPECT_EQ(to_hex(c.target_public_key().data(), 32),
              "c5fffd407ea7d83056da8e193f33967b2d9d42b5f4a9d54757f0253a1ce56a30");
    EXPECT_TRUE(c.uses_derived_identity());
    uv_loop_close(&loop);
}

TEST(ClientTarget, PublicModeDecodesTheKeyAsZ32) {
    uv_loop_t loop;
    ASSERT_EQ(uv_loop_init(&loop), 0);
    Logger logger("Holesail", false, Level::Info);

    HolesailClient::Config cfg;
    cfg.key = "az994od6w9cdyis4tacu6chsxcs34oii61w7kt4z6y1uw88fpeay";
    cfg.secure = false;

    HolesailClient c(&loop, nullptr, cfg, logger);
    EXPECT_EQ(to_hex(c.target_public_key().data(), 32),
              "c5fffd407ea7d83056da8e193f33967b2d9d42b5f4a9d54757f0253a1ce56a30");
    EXPECT_FALSE(c.uses_derived_identity());
    uv_loop_close(&loop);
}
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
nix develop --command bash -c "cmake --build build --target test_client"
```

Expected: `fatal error: holesail/client.hpp: No such file or directory`.

- [ ] **Step 3: Write the header and implementation**

Per `holesail-client/index.js`:

1. Constructor: secure → `sha256(key)` is the seed; derive the keypair; the
   target is its public key **and** the DHT must use that same keypair as its own
   identity (set `hyperdht_opts_t.seed` when creating the DHT, or pass it via
   `hyperdht_connect_ex(..., opts.keypair)`). Public → `z32_decode(key)` is the
   target; a z32 decode failure is an error the caller must surface.
2. `connect()`: `hyperdht_mutable_get(target, 0, ...)` → `decode_record` →
   `resolve()` → start `TcpProxy::listen(host, port)` or `UdpProxy::listen(...)`.
3. Per accepted local socket: `hyperdht_connect_ex` with
   `opts.reusable_socket = 1` (the field added in Task 2), then
   `hyperdht_stream_open`, wrap in a `StreamSide`, hand to a `TcpPipe::adopt`.
   Prefer `hyperdht_connect_and_open_stream` where it fits — its docstring warns
   that opening a stream after the connect callback returns can dangle.
4. `destroy()`: close the proxy first (stop accepting), then tear down pipes,
   then the DHT.

- [ ] **Step 4: Run the tests and commit**

```cmake
target_sources(holesail_lib PRIVATE ... src/client.cpp)
```
```cmake
    holesail_add_test(test_client)
```
```bash
nix develop --command bash -c "cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug && cmake --build build && cd build && ctest --output-on-failure"
```

Expected: all `test_client` cases PASS.

```bash
git add include/holesail/client.hpp src/client.cpp test/test_client.cpp CMakeLists.txt
git commit -m "feat: HolesailClient with record resolution and local proxy"
```

---

### Task 10: The Holesail facade, Info, and lookup

**Files:**
- Create: `include/holesail/holesail.hpp`, `src/holesail.cpp`
- Modify: `CMakeLists.txt`
- Test: `test/test_holesail.cpp`

**Interfaces:**
- Consumes: everything from Tasks 3-9.
- Produces:
  ```cpp
  namespace holesail {
  struct Options {
      bool server = false;
      bool client = false;
      std::optional<uint16_t> port;
      std::optional<std::string> host;
      std::optional<std::string> key;
      std::optional<bool> secure;
      std::optional<bool> udp;
      int log_level = -1;            // -1 disables logging
  };

  struct Info {
      std::string type;              // "server" | "client"
      std::string state;
      bool secure = false;
      uint16_t port = 0;
      std::string host;
      std::string protocol;          // "tcp" | "udp"
      std::string seed;              // hex
      std::string key;
      std::string url;               // hs://...
      std::string public_key;        // z32
  };

  struct LookupResult { std::string host; uint16_t port; std::string protocol; bool secure; };

  // Port of Holesail#initialise. Pure — resolves key/seed/secure before any I/O.
  struct Derived { bool secure; std::string key; std::string seed_hex; };
  Derived derive(const Options& options);

  std::string make_url(bool secure, std::string_view key);   // "hs://s000"/"hs://0000" + key

  // Thrown by the constructor only. Port of validateOpts.
  struct OptionsError { std::string message; };

  class Holesail {
  public:
      Holesail(uv_loop_t* loop, Options options);      // throws OptionsError
      ~Holesail();
      int ready(std::function<void(int err)> cb);
      void pause();
      void resume();
      void close();
      const Info& info() const;
      static int lookup(uv_loop_t* loop, std::string_view url,
                        std::function<void(int err, const std::optional<LookupResult>&)> cb);
  };
  }
  ```

- [ ] **Step 1: Write the failing test**

Create `test/test_holesail.cpp`:

```cpp
// SPDX-License-Identifier: AGPL-3.0-or-later
// Part of holesail-cpp — a C++ port of holesail (https://github.com/holesail/holesail)
#include <gtest/gtest.h>

#include "holesail/holesail.hpp"
#include "holesail/keys.hpp"

using namespace holesail;

TEST(Derive, ServerSecureWithAnExplicitKey) {
    Options o;
    o.server = true;
    o.secure = true;
    o.key = "this-is-a-32-char-holesail-key!!";
    const auto d = derive(o);
    EXPECT_TRUE(d.secure);
    EXPECT_EQ(d.key, "this-is-a-32-char-holesail-key!!");
    EXPECT_EQ(d.seed_hex,
              "a4e1504af3323f7665dd1ea745f7c6f86d3ad32a0eca5988fbac19f0d31376f0");
}

TEST(Derive, ServerSecureWithoutAKeyGenerates64HexChars) {
    Options o;
    o.server = true;
    o.secure = true;
    const auto d = derive(o);
    EXPECT_TRUE(d.secure);
    EXPECT_EQ(d.key.size(), 64u);
    EXPECT_EQ(d.seed_hex, to_hex(sha256(d.key).data(), 32));
}

TEST(Derive, ServerPublicWithoutAKeyLeavesSeedEmpty) {
    Options o;
    o.server = true;
    o.secure = false;
    const auto d = derive(o);
    EXPECT_FALSE(d.secure);
    EXPECT_TRUE(d.key.empty());
    EXPECT_TRUE(d.seed_hex.empty());  // HolesailServer generates one
}

TEST(Derive, ClientSecureSeedIsZ32OfTheHash) {
    Options o;
    o.client = true;
    o.key = "hs://s000this-is-a-32-char-holesail-key!!";
    const auto d = derive(o);
    EXPECT_TRUE(d.secure);
    EXPECT_EQ(d.key, "this-is-a-32-char-holesail-key!!");
}

TEST(Derive, ClientPublicKeepsTheKeyVerbatim) {
    Options o;
    o.client = true;
    o.key = "hs://0000az994od6w9cdyis4tacu6chsxcs34oii61w7kt4z6y1uw88fpeay";
    const auto d = derive(o);
    EXPECT_FALSE(d.secure);
    EXPECT_EQ(d.key, "az994od6w9cdyis4tacu6chsxcs34oii61w7kt4z6y1uw88fpeay");
}

// Divergence D3 / spec quirk Q1: JS passes `secure: argv.public` on the client
// path, so --public turned the connection SECURE. We pass !--public.
TEST(Derive, D3ClientPublicFlagMeansPublicMode) {
    Options o;
    o.client = true;
    o.key = "az994od6w9cdyis4tacu6chsxcs34oii61w7kt4z6y1uw88fpeay";
    o.secure = false;  // what main() sets when --public is present
    const auto d = derive(o);
    EXPECT_FALSE(d.secure);
}

// Quirk Q4, reproduced: an hs://s000 url beats --public.
TEST(Derive, Q4UrlSecurityWinsOverThePublicFlag) {
    Options o;
    o.client = true;
    o.key = "hs://s000this-is-a-32-char-holesail-key!!";
    o.secure = false;
    const auto d = derive(o);
    EXPECT_TRUE(d.secure);
}

TEST(MakeUrl, UsesTheRightModePrefix) {
    EXPECT_EQ(make_url(true, "abc"), "hs://s000abc");
    EXPECT_EQ(make_url(false, "abc"), "hs://0000abc");
}

TEST(Options, ValidateOptsRejectsBadCombinations) {
    uv_loop_t loop;
    ASSERT_EQ(uv_loop_init(&loop), 0);

    Options both;
    both.server = true;
    both.client = true;
    EXPECT_THROW({ Holesail h(&loop, both); }, OptionsError);

    Options client_no_key;
    client_no_key.client = true;
    EXPECT_THROW({ Holesail h(&loop, client_no_key); }, OptionsError);

    Options server_no_host;
    server_no_host.server = true;
    server_no_host.port = 8080;
    // JS validateOpts requires a host on the server path.
    EXPECT_THROW({ Holesail h(&loop, server_no_host); }, OptionsError);

    uv_loop_close(&loop);
}
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
nix develop --command bash -c "cmake --build build --target test_holesail"
```

Expected: `fatal error: holesail/holesail.hpp: No such file or directory`.

- [ ] **Step 3: Implement `derive`**

The exact rules, transcribed from `Holesail#initialise` with the D3 fix:

```
parsed = parse_url(options.key.value_or(""))
secure = parsed.secure.has_value() ? *parsed.secure : options.secure.value_or(false)

if options.server:
    if !parsed.key.empty():
        key = parsed.key;  seed_hex = to_hex(sha256(key))
    else if secure:
        key = random_hex32();  seed_hex = to_hex(sha256(key))
    else:
        key = "";  seed_hex = ""      // HolesailServer generates a random seed
else:  // client
    key = parsed.key
    seed_hex = secure ? to_hex(sha256(key)) : ""
```

`Info` assembly, from `Holesail.info`:

- `key` = the raw `derive().key` when secure, otherwise `HolesailServer::key()` /
  the client's own key string.
- `url` = `make_url(secure, key)`.
- `seed` = the hex seed; for a secure client it is `to_hex(sha256(key))`.
- `public_key` = z32 of the 32-byte public key.
- `protocol` = `"udp"` or `"tcp"`.

`lookup()` per §2.5 of the spec: derive `arg` (z32 of `sha256(key)` when secure,
else the key itself, failing with `Invalid key format: <key>` if it will not
z32-decode); try `mutable_get(keypair_from_seed(z32_decode(arg)).public_key)`
first, then `mutable_get(z32_decode(arg))`; decode the record.

- [ ] **Step 4: Run the tests and commit**

```cmake
target_sources(holesail_lib PRIVATE ... src/holesail.cpp)
```
```cmake
    holesail_add_test(test_holesail)
```
```bash
nix develop --command bash -c "cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug && cmake --build build && cd build && ctest --output-on-failure"
```

Expected: all `test_holesail` cases PASS.

```bash
git add include/holesail/holesail.hpp src/holesail.cpp test/test_holesail.cpp CMakeLists.txt
git commit -m "feat: Holesail facade with key derivation, info and lookup"
```

---

### Task 11: The CLI

**Files:**
- Replace: `app/main.cpp`
- Create: `include/holesail/cli.hpp`, `src/cli.cpp` (argv parsing, validation, output — kept out of `main.cpp` so it is testable)
- Modify: `CMakeLists.txt`
- Test: `test/test_cli.cpp`

**Interfaces:**
- Consumes: `holesail.hpp`, `colors.hpp`.
- Produces:
  ```cpp
  namespace holesail::cli {
  // A minimist-compatible argv view: --flag, --key=value, --key value,
  // negative-number-aware, with positionals in `rest`.
  struct Args {
      std::map<std::string, std::string> values;   // --key value / --key=value
      std::set<std::string> flags;                 // --key with no value
      std::vector<std::string> rest;               // positionals
      bool has(std::string_view name) const;
      std::optional<std::string> str(std::string_view name) const;
      std::optional<long> num(std::string_view name) const;
      bool boolean(std::string_view name) const;   // present as a bare flag
  };
  Args parse_argv(int argc, char** argv);

  struct Validation { bool ok; std::string message; int exit_code; };
  Validation validate(const Args& args);

  std::string render_help(std::string_view topic);   // "", "live", "connect", "filemanager"
  std::string render_started(const Info& info);
  std::string render_lookup(const std::optional<LookupResult>& result);

  int run(int argc, char** argv);   // called by main()
  }
  ```

- [ ] **Step 1: Write the failing test**

Create `test/test_cli.cpp`:

```cpp
// SPDX-License-Identifier: AGPL-3.0-or-later
// Part of holesail-cpp — a C++ port of holesail (https://github.com/holesail/holesail)
#include <gtest/gtest.h>

#include "holesail/cli.hpp"

#include <vector>

using namespace holesail;
using namespace holesail::cli;

namespace {

Args parse(std::vector<const char*> argv_in) {
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>("holesail"));
    for (const char* a : argv_in) argv.push_back(const_cast<char*>(a));
    return parse_argv(static_cast<int>(argv.size()), argv.data());
}

}  // namespace

TEST(ArgvParser, HandlesTheThreeFlagForms) {
    const auto a = parse({"--live", "8080", "--host=0.0.0.0", "--udp"});
    EXPECT_EQ(a.num("live"), 8080);
    EXPECT_EQ(a.str("host"), "0.0.0.0");
    EXPECT_TRUE(a.boolean("udp"));
    EXPECT_FALSE(a.has("nope"));
}

TEST(ArgvParser, CollectsPositionals) {
    const auto a = parse({"hs://s000abc"});
    ASSERT_EQ(a.rest.size(), 1u);
    EXPECT_EQ(a.rest[0], "hs://s000abc");
}

TEST(ArgvParser, LogTakesAnOptionalValue) {
    EXPECT_TRUE(parse({"--log"}).boolean("log"));
    EXPECT_EQ(parse({"--log", "3"}).num("log"), 3);
}

// Rows straight out of validateInput.js.
TEST(Validate, EmptyKeyExitsZero) {
    const auto v = validate(parse({"--live", "8080", "--key"}));
    EXPECT_FALSE(v.ok);
    EXPECT_NE(v.message.find("Key can not be empty"), std::string::npos);
    EXPECT_EQ(v.exit_code, 0);   // quirk Q3, reproduced
}

TEST(Validate, ShortKeyNeedsForce) {
    auto v = validate(parse({"--live", "8080", "--key", "tooshort"}));
    EXPECT_FALSE(v.ok);
    EXPECT_NE(v.message.find("minimum length of 32 chars"), std::string::npos);
    EXPECT_EQ(v.exit_code, 2);

    v = validate(parse({"--live", "8080", "--key", "tooshort", "--force"}));
    EXPECT_TRUE(v.ok);
}

TEST(Validate, TwoConnectionStringsIsAnError) {
    const auto v = validate(parse({"--connect", "abc", "def"}));
    EXPECT_FALSE(v.ok);
    EXPECT_NE(v.message.find("two connection strings"), std::string::npos);
    EXPECT_EQ(v.exit_code, 2);
}

TEST(Validate, NonNumericLivePortIsAnError) {
    const auto v = validate(parse({"--live", "abc"}));
    EXPECT_FALSE(v.ok);
    EXPECT_NE(v.message.find("not a valid number"), std::string::npos);
    EXPECT_EQ(v.exit_code, 2);
}

TEST(Validate, NonNumericPortIsAnError) {
    const auto v = validate(parse({"--connect", "abc", "--port", "xyz"}));
    EXPECT_FALSE(v.ok);
    EXPECT_EQ(v.exit_code, 2);
}

TEST(Validate, AcceptsAWellFormedServerInvocation) {
    EXPECT_TRUE(validate(parse({"--live", "8080"})).ok);
    EXPECT_TRUE(validate(parse({"--live", "8080", "--public"})).ok);
}

TEST(RenderStarted, ServerLinesMatchTheJsWording) {
    Info info;
    info.type = "server";
    info.protocol = "tcp";
    info.secure = true;
    info.host = "127.0.0.1";
    info.port = 8080;
    info.url = "hs://s000abc";

    const auto out = render_started(info);
    EXPECT_NE(out.find("Holesail TCP Server Started"), std::string::npos);
    EXPECT_NE(out.find("Private Connection String"), std::string::npos);
    EXPECT_NE(out.find("Holesail is now listening on 127.0.0.1:8080"), std::string::npos);
    EXPECT_NE(out.find("Connect with key: "), std::string::npos);
    EXPECT_NE(out.find("hs://s000abc"), std::string::npos);
    EXPECT_NE(out.find("TREAT PRIVATE CONNECTION STRINGS"), std::string::npos);
}

TEST(RenderStarted, ClientLinesMatchTheJsWording) {
    Info info;
    info.type = "client";
    info.protocol = "tcp";
    info.secure = false;
    info.host = "127.0.0.1";
    info.port = 8989;
    info.url = "hs://0000abc";

    const auto out = render_started(info);
    EXPECT_NE(out.find("Holesail TCP Client Started"), std::string::npos);
    EXPECT_NE(out.find("Public Connection String"), std::string::npos);
    EXPECT_NE(out.find("Access application on http://127.0.0.1:8989/"), std::string::npos);
    EXPECT_NE(out.find("Connected to key: "), std::string::npos);
    EXPECT_NE(out.find("NOTICE: TREAT PUBLIC STRING"), std::string::npos);
}

TEST(RenderHelp, ListsEveryImplementedCommand) {
    const auto out = render_help("");
    for (const char* needle : {"--live", "--connect", "--lookup", "--host",
                               "--udp", "--public", "--port", "--log", "--version"}) {
        EXPECT_NE(out.find(needle), std::string::npos) << needle;
    }
    // --filemanager is not implemented; it must not be advertised.
    EXPECT_EQ(out.find("--filemanager"), std::string::npos);
}

TEST(RenderLookup, PrintsAllFieldsAndHandlesAMiss) {
    LookupResult r{"127.0.0.1", 8080, "TCP", true};
    const auto out = render_lookup(r);
    EXPECT_NE(out.find("Holesail Lookup Result"), std::string::npos);
    EXPECT_NE(out.find("127.0.0.1"), std::string::npos);
    EXPECT_NE(out.find("8080"), std::string::npos);
    EXPECT_NE(out.find("TCP"), std::string::npos);
    EXPECT_NE(out.find("Yes"), std::string::npos);

    EXPECT_NE(render_lookup(std::nullopt).find("No record found"), std::string::npos);
}
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
nix develop --command bash -c "cmake --build build --target test_cli"
```

Expected: `fatal error: holesail/cli.hpp: No such file or directory`.

- [ ] **Step 3: Implement the CLI**

`parse_argv` reproduces the subset of `minimist` that holesail uses: `--flag`,
`--flag=value`, `--flag value` (where the next token does not start with `--`),
and positionals. A value that parses fully as a number is available through
`num()`; `boolean()` is true when the flag appeared without a value.

`main.cpp` becomes:

```cpp
// SPDX-License-Identifier: AGPL-3.0-or-later
// Part of holesail-cpp — a C++ port of holesail (https://github.com/holesail/holesail)
#include "holesail/cli.hpp"

int main(int argc, char** argv) { return holesail::cli::run(argc, argv); }
```

`run()` dispatches exactly as `bin/holesail.mjs` does, in this order:

1. `validate(args)` — on failure print the message and exit with `exit_code`.
2. `--help` / `-h` → `render_help(topic)`, exit 0.
3. `--version` → print the version, exit 0.
4. `--live <port>` → server. `secure = args.has("public") ? !args.boolean("public") : true`.
5. `--connect <key>` or a bare positional → client.
   **D3:** `secure = args.has("public") ? !args.boolean("public") : std::nullopt`
   — this is the deliberate fix; do not pass `argv.public` straight through.
6. `--lookup <key>` → `Holesail::lookup`, print, exit 0.
7. Otherwise → `render_help("")`, exit 0.

`--filemanager` is not implemented. If it is passed, print
`Error: --filemanager is not supported by holesail-cpp` and exit 2 rather than
silently falling through to the help text.

Install a `SIGINT`/`SIGTERM` handler (`uv_signal_t`) that calls `Holesail::close()`
and stops the loop — the JS uses `graceful-goodbye` for the same purpose.

- [ ] **Step 4: Run the tests**

```cmake
target_sources(holesail_lib PRIVATE ... src/cli.cpp)
```
```cmake
    holesail_add_test(test_cli)
```
```bash
nix develop --command bash -c "cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug && cmake --build build && cd build && ctest --output-on-failure"
```

Expected: all `test_cli` cases PASS.

- [ ] **Step 5: Compare the help output against the JS side by side**

```bash
export HOLESAIL_JS=/nix/store/mlhix2xnb4hsa4zf85rkln5ngr8ncm14-holesail-2.4.1
diff <(node $HOLESAIL_JS/lib/node_modules/holesail/src/bin/holesail.mjs --help) \
     <(./build/holesail --help)
```

Expected: differences confined to the `--filemanager` line and its example.
Anything else is a wording drift — fix it.

- [ ] **Step 6: Commit**

```bash
git add app/main.cpp include/holesail/cli.hpp src/cli.cpp test/test_cli.cpp CMakeLists.txt
git commit -m "feat: CLI with minimist-compatible parsing, validation and output"
```

---

### Task 12: Loopback integration test

Proves the whole stack end to end within one process: a local echo server, a
holesail server exposing it, a holesail client proxying it, and a test client
that gets its bytes back.

**Files:**
- Create: `test/test_integration.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: everything.
- Produces: no new API.

- [ ] **Step 1: Write the failing test**

Create `test/test_integration.cpp`. Structure:

```cpp
// SPDX-License-Identifier: AGPL-3.0-or-later
// Part of holesail-cpp — a C++ port of holesail (https://github.com/holesail/holesail)
#include <gtest/gtest.h>

#include "holesail/holesail.hpp"

#include <uv.h>

#include <cstdlib>
#include <string>

// These tests join the public DHT and are therefore slow and network-dependent.
// Gate them behind an environment variable so `ctest` stays fast by default:
//   HOLESAIL_NETWORK_TESTS=1 ctest -R test_integration --output-on-failure
namespace {
bool network_tests_enabled() {
    const char* v = std::getenv("HOLESAIL_NETWORK_TESTS");
    return v != nullptr && std::string(v) == "1";
}
}  // namespace

TEST(Integration, TcpTunnelSecureMode) {
    if (!network_tests_enabled()) GTEST_SKIP() << "set HOLESAIL_NETWORK_TESTS=1";

    // 1. Start a local echo server on an ephemeral port.
    // 2. Holesail server: server=true, secure=true, port=<echo port>,
    //    host="127.0.0.1", key="this-is-a-32-char-holesail-key!!".
    //    ready() -> capture info().url.
    // 3. Holesail client: client=true, key=<that url>, port=<another ephemeral>.
    //    ready() -> the local proxy is listening.
    // 4. Connect a plain TCP client to the proxy port, send 64 KiB of
    //    pseudo-random bytes, assert the identical bytes come back.
    // 5. Half-close from the test client; assert the echo server sees EOF.
    // 6. close() both; assert uv_loop_close returns 0 (no leaked handles).
}

TEST(Integration, TcpTunnelPublicMode) {
    if (!network_tests_enabled()) GTEST_SKIP() << "set HOLESAIL_NETWORK_TESTS=1";
    // Same as above with secure=false; the client uses info().url from the
    // server, which will carry the 0000 prefix and a z32 public key.
}

TEST(Integration, UdpTunnel) {
    if (!network_tests_enabled()) GTEST_SKIP() << "set HOLESAIL_NETWORK_TESTS=1";
    // Local UDP echo responder; holesail server and client both with udp=true.
    // Send three datagrams of different sizes (1 byte, 1400 bytes, 60000 bytes)
    // from two distinct source ports and assert each comes back to its own
    // sender — this exercises the per-source fan-out in UdpProxy.
}

TEST(Integration, LookupFindsThePublishedRecord) {
    if (!network_tests_enabled()) GTEST_SKIP() << "set HOLESAIL_NETWORK_TESTS=1";
    // Start a server, then Holesail::lookup(info().url) and assert the
    // returned host/port/protocol match what the server published.
}
```

Fill in each body. Use a single `uv_loop_t` shared by the echo server, both
Holesail instances, and the test client — everything is callback-driven on one
loop, so no threads are needed. Give each test a wall-clock deadline
(`uv_timer_t`, 60 s) that fails the test rather than hanging forever.

- [ ] **Step 2: Run it to verify it fails**

```bash
nix develop --command bash -c "cmake --build build --target test_integration && HOLESAIL_NETWORK_TESTS=1 ./build/test_integration"
```

Expected: failures (unimplemented bodies) or a timeout.

- [ ] **Step 3: Make the tests pass**

Fix whatever the integration test exposes. Expect issues in teardown ordering
and in the connect→stream_open handoff — these paths are not reachable from the
unit tests.

- [ ] **Step 4: Verify both the fast and the network suites**

```bash
nix develop --command bash -c "cd build && ctest --output-on-failure"
nix develop --command bash -c "cd build && HOLESAIL_NETWORK_TESTS=1 ctest --output-on-failure"
```

Expected: the first run skips the integration tests and is fast; the second runs
them all green.

- [ ] **Step 5: Run the network suite under sanitizers**

```bash
nix develop --command bash -c "cmake --build build-asan && cd build-asan && HOLESAIL_NETWORK_TESTS=1 ctest --output-on-failure"
```

Expected: PASS, no ASan/UBSan reports.

- [ ] **Step 6: Commit**

```bash
git add test/test_integration.cpp CMakeLists.txt
git commit -m "test: end-to-end loopback tunnel over the public DHT"
```

---

### Task 13: JS interop harness, docs, and packaging

The acceptance gate: the C++ binary must interoperate with the real JS holesail.

**Files:**
- Create: `scripts/cross-test.sh`
- Create: `README.md`
- Create: `CLAUDE.md`
- Modify: `flake.nix` (add the aarch64 static package)

**Interfaces:**
- Consumes: the `holesail` binary.
- Produces: no new API.

- [ ] **Step 1: Write the interop harness**

Create `scripts/cross-test.sh`:

```bash
#!/usr/bin/env bash
# Live interop test: C++ holesail against the JS reference implementation.
#
#   HOLESAIL_JS=/path/to/holesail/checkout ./scripts/cross-test.sh ./build/holesail
#
# Requires network access — it joins the public HyperDHT.
set -euo pipefail

CPP_BIN="${1:-./build/holesail}"
JS_BIN="${HOLESAIL_JS:-}/bin/holesail"
KEY="this-is-a-32-char-holesail-key!!"

[ -x "$CPP_BIN" ] || { echo "no such binary: $CPP_BIN" >&2; exit 1; }
[ -x "$JS_BIN" ]  || { echo "set HOLESAIL_JS to a holesail install" >&2; exit 1; }

workdir=$(mktemp -d)
pids=()
cleanup() {
    for pid in "${pids[@]:-}"; do kill "$pid" 2>/dev/null || true; done
    rm -rf "$workdir"
}
trap cleanup EXIT

# A local HTTP origin to tunnel.
python3 -m http.server 18080 --bind 127.0.0.1 --directory "$workdir" >/dev/null 2>&1 &
pids+=($!)
echo "hello-from-holesail" > "$workdir/probe.txt"
sleep 1

run_case() {
    local name="$1" server_cmd="$2" client_cmd="$3" client_port="$4"
    echo "=== $name ==="
    eval "$server_cmd" >"$workdir/$name.server.log" 2>&1 &
    local server_pid=$!
    pids+=("$server_pid")
    sleep 15   # DHT announce + holepunch readiness

    eval "$client_cmd" >"$workdir/$name.client.log" 2>&1 &
    local client_pid=$!
    pids+=("$client_pid")
    sleep 15

    if curl -sf --max-time 20 "http://127.0.0.1:$client_port/probe.txt" \
         | grep -q hello-from-holesail; then
        echo "PASS: $name"
    else
        echo "FAIL: $name"
        echo "--- server log ---"; cat "$workdir/$name.server.log"
        echo "--- client log ---"; cat "$workdir/$name.client.log"
        return 1
    fi
    kill "$server_pid" "$client_pid" 2>/dev/null || true
    sleep 2
}

run_case "cpp-server-js-client-secure" \
    "$CPP_BIN --live 18080 --host 127.0.0.1 --key '$KEY'" \
    "$JS_BIN --connect \"hs://s000$KEY\" --port 18081" 18081

run_case "js-server-cpp-client-secure" \
    "$JS_BIN --live 18080 --host 127.0.0.1 --key '$KEY'" \
    "$CPP_BIN --connect \"hs://s000$KEY\" --port 18082" 18082

echo "=== lookup: C++ reads a JS-published record ==="
"$JS_BIN" --live 18080 --host 127.0.0.1 --key "$KEY" >"$workdir/lookup.log" 2>&1 &
pids+=($!)
sleep 20
"$CPP_BIN" --lookup "hs://s000$KEY" | tee "$workdir/lookup.out"
grep -q "18080" "$workdir/lookup.out" || { echo "FAIL: lookup"; exit 1; }
echo "PASS: lookup"

echo "All interop cases passed."
```

`chmod +x scripts/cross-test.sh`.

**Prerequisite, verified 2026-07-28:** the harness needs `python3`, `curl` and
`nodejs` on PATH, and the host shell has no `python3`. Add
`pkgs.python3 pkgs.curl pkgs.nodejs` to the flake devShell `packages` list as
part of this task, and always invoke the harness through
`nix develop --command ./scripts/cross-test.sh`. The JS reference binary is
confirmed working at
`/nix/store/mlhix2xnb4hsa4zf85rkln5ngr8ncm14-holesail-2.4.1/bin/holesail`
(reports version 2.4.1), and all three public bootstrap nodes resolve.

- [ ] **Step 2: Run the harness**

```bash
export HOLESAIL_JS=/nix/store/mlhix2xnb4hsa4zf85rkln5ngr8ncm14-holesail-2.4.1
nix develop --command ./scripts/cross-test.sh ./build/holesail
```

Expected: every case PASSes. A failure here is a wire-compatibility bug — the
whole point of the port. Debug with `--log 0` on both sides and compare the
derived public keys.

Also run the public-mode variants by hand:

```bash
./build/holesail --live 18080 --public          # note the printed hs://0000... url
$HOLESAIL_JS/bin/holesail --connect <that-url> --port 18083
curl http://127.0.0.1:18083/probe.txt
```

- [ ] **Step 3: Write `README.md`**

Cover: what it is, the relationship to JS holesail and hyperdht-cpp, build
instructions (`nix develop` and manual), the full CLI with worked examples, the
library API with a short embedding example, the divergence table from the spec,
and the AGPL-3.0 notice.

- [ ] **Step 4: Write `CLAUDE.md`**

Follow the structure of `../hyperdht-cpp/CLAUDE.md`: project status, stack, key
decisions, architecture tree, the verified cross-language vectors from this plan,
a gotchas section (start it with the JSON `udp`-key omission, the SHA-256-of-the-
string derivation, and the libuv 1.51 pin), build and test commands.

- [ ] **Step 5: Add the aarch64 static package**

In `flake.nix`, add a `holesail-aarch64-static` output built against
hyperdht-cpp's `pkgsCross.aarch64.pkgsStatic` derivation — see
`../hyperdht-cpp/package.nix` and its `hades-aarch64-static` equivalent for the
pattern.

```bash
nix build .#holesail-aarch64-static
file result/bin/holesail   # expect: ELF 64-bit LSB executable, ARM aarch64, statically linked
```

- [ ] **Step 6: Final full verification**

```bash
nix develop --command bash -c "cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build && cd build && ctest --output-on-failure"
nix develop --command bash -c "cd build && HOLESAIL_NETWORK_TESTS=1 ctest --output-on-failure"
nix develop --command ./scripts/cross-test.sh ./build/holesail
nix build .#default
```

Expected: all green.

- [ ] **Step 7: Commit**

```bash
git add scripts/cross-test.sh README.md CLAUDE.md flake.nix flake.lock
git commit -m "docs: readme, project notes, JS interop harness and static aarch64 package"
```

---

## Post-Implementation Checklist

- [ ] `ctest` green in Debug and Release
- [ ] `ctest` green under ASan + UBSan
- [ ] `HOLESAIL_NETWORK_TESTS=1 ctest` green
- [ ] `scripts/cross-test.sh` green in both directions, secure and public
- [ ] `nix build .#default` and `.#holesail-aarch64-static` both succeed
- [ ] `../hyperdht-cpp` still passes its own 584 tests with the Task 2 additions
- [ ] Every divergence in the code has a matching row in the spec's D-table
