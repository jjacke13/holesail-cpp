{
  description = "holesail-cpp — C++ port of holesail, powered by hyperdht-cpp";

  inputs = {
    # libuv 1.51.x — see Global Constraints. Do NOT bump past 25.11.
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-25.11";

    # Pull sibling changes with: nix flake update hyperdht-cpp
    #
    # For local co-development against an uncommitted sibling checkout, override
    # on the command line rather than editing this line:
    #   nix build --override-input hyperdht-cpp \
    #     'git+file:///home/jacke/Desktop/repos/hyperdht-cpp?ref=refs/heads/main'
    # Note `path:../hyperdht-cpp` does NOT work — Nix resolves relative inputs
    # against the store copy of this flake, so `../` escapes the tree; and
    # `path:/abs/path` copies gitignored build dirs (~15 GB in that checkout).
    hyperdht-cpp.url = "github:jjacke13/hyperdht-cpp";
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
          # One musl, fully-static target — a single binary to scp onto a host
          # with no runtime deps at all, and no nix needed there. hyperdht-cpp
          # only publishes native package outputs, so its shared builder is
          # instantiated here against the cross set (the same expression its
          # own flake uses).
          #
          # NOT `pkgs.pkgsCross.<target>.pkgsStatic`: that set makes the
          # BUILD-platform tools static-musl too, and nixpkgs 25.11's static
          # cmake fails to bootstrap ("CMake Error: Unknown argument
          # --disable-shared"). Declaring the musl target as `crossSystem`
          # instead keeps cmake/ninja native and only the target static.
          # Diagnostic: `pkgsStatic.cmake.__spliced.buildHost.name` is
          # `cmake-static-...-musl`, the crossSystem one is plain `cmake`.
          #
          # This holds for x86_64 too, even when it matches the build platform:
          # the triple differs from the default `-gnu` one, so nix still treats
          # it as cross and the native toolchain is preserved.
          mkStatic = { arch, blurb }:
            let
              staticPkgs = import nixpkgs {
                inherit system;
                crossSystem = {
                  config = "${arch}-unknown-linux-musl";
                  isStatic = true;
                };
              };
              hyperdhtStatic = (import "${hyperdht-cpp}/nix/lib.nix" {
                pkgs = staticPkgs;
                libudx = hyperdht-cpp.inputs.libudx;
                src = hyperdht-cpp;
              }).mkHyperdht { };
            in
            staticPkgs.stdenv.mkDerivation {
              pname = "holesail-cpp-${arch}-static";
              version = "0.1.1";
              src = self;
              nativeBuildInputs = [ staticPkgs.cmake staticPkgs.ninja staticPkgs.pkg-config ];
              buildInputs = [ staticPkgs.libsodium staticPkgs.libuv hyperdhtStatic ];
              # Tests are not built: GoogleTest would have to be linked statically
              # for a host that may not even be able to run them. The native lane
              # gates them instead.
              cmakeFlags = [ "-DHOLESAIL_BUILD_TESTS=OFF" "-DCMAKE_BUILD_TYPE=Release" ];
              meta.description = "holesail CLI — ${arch} fully-static build ${blurb}";
            };
        in
        {
          default = pkgs.stdenv.mkDerivation {
            pname = "holesail-cpp";
            version = "0.1.1";
            src = self;
            nativeBuildInputs = [ pkgs.cmake pkgs.ninja pkgs.pkg-config ];
            buildInputs = [
              pkgs.libsodium pkgs.libuv
              hyperdht-cpp.packages.${system}.default
            ];
            cmakeFlags = [ "-DHOLESAIL_BUILD_TESTS=OFF" ];
          };

          holesail-aarch64-static = mkStatic {
            arch = "aarch64";
            blurb = "for Raspberry Pi class hardware";
          };

          # The x86-64 counterpart. Without it the only thing an ordinary Linux
          # server or desktop could do was build from source through nix, which
          # is a far higher bar than `curl` + `chmod +x`.
          holesail-x86_64-static = mkStatic {
            arch = "x86_64";
            blurb = "for x86-64 Linux servers and desktops";
          };
        });
    };
}
