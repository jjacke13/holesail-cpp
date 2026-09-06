# Cross-compile shell for the Android (Termux) build.
#
# Deliberately NOT part of flake.nix: `src = self` there means every edit to the
# flake changes the store path of both static packages, and this toolchain is
# unfree (the NDK) so it has no business in the default build graph.
#
#   nix-shell nix/android-shell.nix --run ./scripts/build-android.sh
#
# We do NOT use pkgsCross.aarch64-android-prebuilt. The NDK ships its own CMake
# toolchain file, which is what hyperdht-cpp's CI uses and what upstream
# libsodium/libuv support; going through nixpkgs' cross machinery buys nothing
# and drags in a second, differently-broken toolchain.
{ pkgs ? import <nixpkgs> {
    config.allowUnfree = true;             # the NDK is unfree
    config.android_sdk.accept_license = true;
  }
}:

let
  composition = pkgs.androidenv.composeAndroidPackages {
    includeNDK = true;
    ndkVersions = [ "27.0.12077973" ];
    platformVersions = [ "34" ];
    includeEmulator = false;
    includeSystemImages = false;
  };
  sdk = composition.androidsdk;
in
pkgs.mkShell {
  name = "holesail-android";

  buildInputs = [
    sdk
    pkgs.cmake
    pkgs.ninja
    pkgs.git
    # libsodium's dist-build/android-armv8-a.sh is an autotools build.
    pkgs.autoconf
    pkgs.automake
    pkgs.libtool
    pkgs.gnumake
    pkgs.file
    pkgs.jq
  ];

  ANDROID_SDK_ROOT = "${sdk}/libexec/android-sdk";

  shellHook = ''
    # composeAndroidPackages puts the NDK under ndk/<version>; older layouts
    # used ndk-bundle. Accept either rather than pinning the path.
    for tc in "$ANDROID_SDK_ROOT"/ndk/*/build/cmake/android.toolchain.cmake \
              "$ANDROID_SDK_ROOT"/ndk-bundle/build/cmake/android.toolchain.cmake; do
      if [ -f "$tc" ]; then
        ANDROID_NDK_ROOT="$(dirname "$(dirname "$(dirname "$tc")")")"
        break
      fi
    done
    if [ -z "''${ANDROID_NDK_ROOT:-}" ]; then
      echo "ERROR: no NDK found under $ANDROID_SDK_ROOT" >&2
    fi
    export ANDROID_NDK_ROOT
    export ANDROID_NDK="$ANDROID_NDK_ROOT"
    export ANDROID_NDK_HOME="$ANDROID_NDK_ROOT"   # libsodium's script reads this

    echo "holesail-cpp Android shell"
    echo "  NDK: $ANDROID_NDK_ROOT"
    echo "  build: ./scripts/build-android.sh"
  '';
}
