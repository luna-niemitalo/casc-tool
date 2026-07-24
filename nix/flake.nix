{
  description = "casc-tool: CASC storage browser/extractor CLI";

  inputs = {
    pins.url = "path:/home/luna/nix/pins";
    nixpkgs.follows = "pins/nixpkgs";
    flake-utils.url = "github:numtide/flake-utils";
    # CascLib is vendored as a git submodule under vendor/CascLib for local
    # dev (plain `cmake` builds against the on-disk checkout). A plain `nix
    # build`/flake consumer of *this* flake won't have that submodule
    # content, though: `self` here only exposes git-tracked files, and a
    # submodule's own files aren't part of the parent repo's `git ls-files`
    # output (only the gitlink entry is). Fetching CascLib as its own flake
    # input sidesteps that entirely -- it's always resolved by Nix's normal
    # flake-locking, with no dependency on the consumer remembering
    # `?submodules=1` on a github: reference to *this* repo.
    casclib = {
      url = "github:ladislav-zezula/CascLib";
      flake = false;
    };
  };

  outputs =
    inputs@{
      self,
      pins,
      nixpkgs,
      flake-utils,
      casclib,
    }:
    flake-utils.lib.eachDefaultSystem (
      system:
      let
        pkgs = import nixpkgs { inherit system; };
        lib = pkgs.lib;

        projectRoot = ../.;

        # Only what the packaged build actually needs: CMakeLists.txt + src/.
        # Deliberately excludes tests/, build/, .git, vendor/ -- none of
        # those should ever end up copied into the Nix store for this.
        appSource = lib.fileset.toSource {
          root = projectRoot;
          fileset = lib.fileset.unions [
            (projectRoot + "/CMakeLists.txt")
            (projectRoot + "/src")
          ];
        };

        cpp = with pkgs; [
          cmake
          ninja
          pkg-config
          gcc
          gdb
          ccache
          doctest
        ];
      in
      {
        packages.default = pkgs.stdenv.mkDerivation {
          pname = "casc-tool";
          version = "0.1.0";
          src = appSource;

          nativeBuildInputs = [
            pkgs.cmake
            pkgs.ninja
          ];

          # vendor/CascLib doesn't exist in appSource (see above) -- drop in
          # the fetched CascLib source right after unpacking, before cmake
          # configure runs and hits add_subdirectory(vendor/CascLib).
          postUnpack = ''
            mkdir -p "$sourceRoot/vendor/CascLib"
            cp -r ${casclib}/. "$sourceRoot/vendor/CascLib/"
            chmod -R u+w "$sourceRoot/vendor/CascLib"
          '';

          cmakeFlags = [ "-DCASC_TOOL_BUILD_TESTS=OFF" ];

          installPhase = ''
            mkdir -p $out/bin
            cp casc-tool $out/bin/
          '';
        };

        apps.default = {
          type = "app";
          program = "${self.packages.${system}.default}/bin/casc-tool";
        };

        devShells.default = pkgs.mkShell {
          packages = cpp;

          CCACHE_DIR = "/media/luna/cache/ccache";

          shellHook = ''
            echo "casc-tool dev shell"
            echo "  cmake:   $(cmake --version | head -n1)"
            echo "  ccache:  $CCACHE_DIR"
            echo "  doctest: available via find_package(doctest)"
          '';
        };

        formatter = pkgs.nixfmt-rfc-style;
      }
    );
}
