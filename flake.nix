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
          nativeBuildInputs = [ pkgs.cmake pkgs.ninja pkgs.pkg-config ];
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
