{
  description = "holesail-cpp — C++ port of holesail, powered by hyperdht-cpp";

  inputs = {
    # libuv 1.51.x — see Global Constraints. Do NOT bump past 25.11.
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-25.11";

    # The sibling checkout, referenced through the git fetcher on purpose:
    #   * `path:../hyperdht-cpp` does not work — Nix resolves relative inputs
    #     against the *store copy* of this flake, so `../` escapes the tree.
    #   * `path:/abs/path` copies the whole directory, gitignored build dirs
    #     included (~15 GB in that checkout).
    #   * `?ref=refs/heads/main` pins to the last commit instead of the working
    #     tree, so an uncommitted edit next door cannot silently change this
    #     build (and does not block writing flake.lock).
    # Pull sibling changes with: nix flake update hyperdht-cpp
    # Replace with `github:jjacke13/hyperdht-cpp` once it is published.
    hyperdht-cpp.url = "git+file:///home/jacke/Desktop/repos/hyperdht-cpp?ref=refs/heads/main";
    hyperdht-cpp.inputs.nixpkgs.follows = "nixpkgs";
  };

  outputs = { self, nixpkgs, hyperdht-cpp }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAll = f: nixpkgs.lib.genAttrs systems (system: f system nixpkgs.legacyPackages.${system});
    in {
      devShells = forAll (system: pkgs: {
        default = pkgs.mkShell {
          nativeBuildInputs = [
            pkgs.cmake pkgs.ninja pkgs.pkg-config
            # scripts/cross-test.sh needs all three: python3 serves the origin
            # being tunnelled, curl drives it through the proxy, and nodejs
            # runs the JS holesail we test interop against. None are on the
            # host PATH, and a missing python3 makes the harness report a
            # tunnel failure when the origin simply never started.
            pkgs.python3 pkgs.curl pkgs.nodejs
          ];
          # buildInputs, not `packages`: only host deps get CMAKE_PREFIX_PATH
          # and PKG_CONFIG_PATH from the cmake/pkg-config setup hooks.
          buildInputs = [
            pkgs.libsodium pkgs.libuv pkgs.gtest
            hyperdht-cpp.packages.${system}.default
          ];
          shellHook = ''
            echo "holesail-cpp dev shell — cmake ${pkgs.cmake.version}"
          '';
        };
      });

      packages = forAll (system: pkgs:
        let
          # aarch64, musl, fully static — one binary to scp onto a Raspberry Pi
          # with no runtime deps at all. hyperdht-cpp only publishes native
          # package outputs, so its shared builder is instantiated here against
          # the cross set (same expression its own flake uses).
          #
          # NOT `pkgs.pkgsCross.aarch64-multiplatform.pkgsStatic`: that set makes
          # the BUILD-platform tools static-musl too, and nixpkgs 25.11's static
          # cmake fails to bootstrap ("CMake Error: Unknown argument
          # --disable-shared"). Declaring the musl target as `crossSystem`
          # instead keeps cmake/ninja native and only the target static.
          staticPkgs = import nixpkgs {
            inherit system;
            crossSystem = { config = "aarch64-unknown-linux-musl"; isStatic = true; };
          };
          hyperdhtStatic = (import "${hyperdht-cpp}/nix/lib.nix" {
            pkgs = staticPkgs;
            libudx = hyperdht-cpp.inputs.libudx;
            src = hyperdht-cpp;
          }).mkHyperdht { };
        in
        {
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

          holesail-aarch64-static = staticPkgs.stdenv.mkDerivation {
            pname = "holesail-cpp-aarch64-static";
            version = "0.1.0";
            src = self;
            nativeBuildInputs = [ staticPkgs.cmake staticPkgs.ninja staticPkgs.pkg-config ];
            buildInputs = [ staticPkgs.libsodium staticPkgs.libuv hyperdhtStatic ];
            # Tests are not built: GoogleTest would have to be linked statically
            # for a host that cannot run them anyway. The native lane gates them.
            cmakeFlags = [ "-DHOLESAIL_BUILD_TESTS=OFF" "-DCMAKE_BUILD_TYPE=Release" ];
            meta.description = "holesail CLI — aarch64 fully-static build for Raspberry Pi class hardware";
          };
        });
    };
}
