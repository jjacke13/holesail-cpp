#!/usr/bin/env bash
#
# Cross-compile the holesail CLI for Android arm64 (Termux).
#
# Why not the musl static: Android runs every app process behind a seccomp-bpf
# allowlist shaped around *bionic's* syscall set. A static musl binary steps
# outside it and is killed with SIGSYS on startup ("Unknown signal 31" in
# Termux). So Android needs a genuine bionic build.
#
# Runs unchanged in two places:
#   CI     — GitHub's ubuntu runners preinstall the NDK and set $ANDROID_NDK.
#   local  — nix-shell nix/android-shell.nix --run ./scripts/build-android.sh
#
# Every dependency is built the way hyperdht-cpp's own CI builds it
# (.github/workflows/build.yml, job build-android-arm64), so the two cannot
# drift apart. Deps are cached under build-android/deps; delete it to rebuild.
#
# Knobs:
#   HYPERDHT_SRC   checkout to build against. Defaults to the sibling
#                  ../hyperdht-cpp when present, else a shallow clone of the
#                  revision flake.lock pins — so the Android binary is always
#                  built against the same transport as the desktop one.
#   ANDROID_API    minimum platform level (default 26, matching hyperdht CI)
#   LIBUV_VERSION  pinned; 1.52.x has the UDP POLLERR regression, do not bump
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$ROOT/build-android"
DEPS="$OUT/deps"

ANDROID_API="${ANDROID_API:-26}"
ABI="${ABI:-arm64-v8a}"
LIBUV_VERSION="${LIBUV_VERSION:-v1.51.0}"

log() { printf '\033[0;32m[android]\033[0m %s\n' "$1"; }
die() { printf '\033[0;31m[android]\033[0m %s\n' "$1" >&2; exit 1; }

# The NDK arrives under a different variable depending on where we run.
NDK="${ANDROID_NDK_ROOT:-${ANDROID_NDK:-${ANDROID_NDK_HOME:-}}}"
[ -n "$NDK" ] || die "no NDK: set ANDROID_NDK_ROOT, or run inside nix-shell nix/android-shell.nix"
TOOLCHAIN="$NDK/build/cmake/android.toolchain.cmake"
[ -f "$TOOLCHAIN" ] || die "NDK toolchain file missing: $TOOLCHAIN"
log "NDK: $NDK"

mkdir -p "$DEPS"

# ------------------------------------------------------- hyperdht source
# Pin to flake.lock so a stale sibling checkout cannot silently produce an
# Android binary built on a different transport than every other target.
PINNED_REV="$(jq -r '.nodes["hyperdht-cpp"].locked.rev' "$ROOT/flake.lock")"
[ -n "$PINNED_REV" ] && [ "$PINNED_REV" != "null" ] || die "no hyperdht-cpp rev in flake.lock"

if [ -n "${HYPERDHT_SRC:-}" ]; then
  HYPERDHT_SRC="$(cd "$HYPERDHT_SRC" && pwd)"
elif [ -d "$ROOT/../hyperdht-cpp/.git" ]; then
  HYPERDHT_SRC="$(cd "$ROOT/../hyperdht-cpp" && pwd)"
  LOCAL_REV="$(git -C "$HYPERDHT_SRC" rev-parse HEAD)"
  if [ "$LOCAL_REV" != "$PINNED_REV" ]; then
    log "WARNING: sibling checkout is $LOCAL_REV, flake.lock pins $PINNED_REV"
    log "         building against the sibling anyway (set HYPERDHT_SRC= to force a clone)"
  fi
else
  HYPERDHT_SRC="$DEPS/hyperdht-cpp"
  if [ ! -d "$HYPERDHT_SRC/.git" ]; then
    log "cloning hyperdht-cpp @ $PINNED_REV"
    rm -rf "$HYPERDHT_SRC"
    git clone --filter=blob:none https://github.com/jjacke13/hyperdht-cpp.git "$HYPERDHT_SRC"
  fi
  git -C "$HYPERDHT_SRC" fetch --filter=blob:none origin "$PINNED_REV"
  git -C "$HYPERDHT_SRC" checkout --detach "$PINNED_REV"
  git -C "$HYPERDHT_SRC" submodule update --init --recursive --depth 1
fi

[ -f "$HYPERDHT_SRC/deps/libudx/src/udx.c" ] \
  || die "libudx submodule missing in $HYPERDHT_SRC (git submodule update --init --recursive)"

cmake_android() {
  cmake -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
    -DANDROID_ABI="$ABI" \
    -DANDROID_PLATFORM="android-$ANDROID_API" \
    -DANDROID_STL=c++_static \
    -DCMAKE_BUILD_TYPE=Release \
    "$@"
}

# ---------------------------------------------------------------- libsodium
# Upstream ships its own NDK script; it knows the armv8 crypto flags better
# than a hand-rolled cmake invocation would.
SODIUM_PREFIX="$DEPS/libsodium/libsodium-android-armv8-a+crypto"
if [ ! -f "$SODIUM_PREFIX/lib/libsodium.a" ]; then
  log "building libsodium for android-$ANDROID_API"
  rm -rf "$DEPS/libsodium"
  git clone --depth 1 --branch stable https://github.com/jedisct1/libsodium.git "$DEPS/libsodium"
  (
    cd "$DEPS/libsodium"
    ./autogen.sh
    export ANDROID_NDK_HOME="$NDK"
    export NDK_PLATFORM="android-$ANDROID_API"
    export LIBSODIUM_FULL_BUILD=1
    dist-build/android-armv8-a.sh
  )
  [ -f "$SODIUM_PREFIX/lib/libsodium.a" ] || die "libsodium build produced no archive"
else
  log "libsodium cached"
fi

# --------------------------------------------------------------------- libuv
UV_PREFIX="$DEPS/libuv-install"
if [ ! -d "$UV_PREFIX/include" ]; then
  log "building libuv $LIBUV_VERSION for android-$ANDROID_API"
  rm -rf "$DEPS/libuv" "$DEPS/libuv-build" "$UV_PREFIX"
  git clone --depth 1 --branch "$LIBUV_VERSION" https://github.com/libuv/libuv.git "$DEPS/libuv"
  cmake_android -B "$DEPS/libuv-build" -S "$DEPS/libuv" \
    -DLIBUV_BUILD_TESTS=OFF \
    -DCMAKE_INSTALL_PREFIX="$UV_PREFIX"
  ninja -C "$DEPS/libuv-build" install
else
  log "libuv cached"
fi
# The archive is libuv.a or libuv_a.a depending on version.
UV_LIB="$(find "$UV_PREFIX" -name '*.a' | head -1)"
[ -n "$UV_LIB" ] || die "no libuv static archive under $UV_PREFIX"

# ------------------------------------------------------------------ hyperdht
# Built out-of-tree so a sibling checkout stays clean.
HDHT_BUILD="$DEPS/hyperdht-build"
log "building hyperdht from $HYPERDHT_SRC ($(git -C "$HYPERDHT_SRC" rev-parse --short HEAD))"
cmake_android -B "$HDHT_BUILD" -S "$HYPERDHT_SRC" \
  -DHYPERDHT_BUILD_TESTS=OFF \
  -DCMAKE_DISABLE_FIND_PACKAGE_PkgConfig=ON \
  -DSODIUM_INCLUDE_DIR="$SODIUM_PREFIX/include" \
  -DSODIUM_LIBRARY="$SODIUM_PREFIX/lib/libsodium.a" \
  -DUV_INCLUDE_DIR="$UV_PREFIX/include" \
  -DUV_LIBRARY="$UV_LIB"
ninja -C "$HDHT_BUILD" hyperdht udx

# ------------------------------------------------------------------ holesail
log "building holesail"
cmake_android -B "$OUT/build" -S "$ROOT" \
  -DHOLESAIL_BUILD_TESTS=OFF \
  -DCMAKE_DISABLE_FIND_PACKAGE_PkgConfig=ON \
  -DSODIUM_INCLUDE_DIR="$SODIUM_PREFIX/include" \
  -DSODIUM_LIBRARY="$SODIUM_PREFIX/lib/libsodium.a" \
  -DUV_INCLUDE_DIR="$UV_PREFIX/include" \
  -DUV_LIBRARY="$UV_LIB" \
  -DHYPERDHT_INCLUDE_DIR="$HYPERDHT_SRC/include" \
  -DHYPERDHT_LIBRARY="$HDHT_BUILD/libhyperdht.a" \
  -DUDX_LIBRARY="$HDHT_BUILD/libudx.a"
ninja -C "$OUT/build" holesail

cp "$OUT/build/holesail" "$OUT/holesail"
log "built: $OUT/holesail"
file "$OUT/holesail" || true
