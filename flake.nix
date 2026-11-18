{
  description = "gamesir-cyclone-2-fw-fix: firmware for the GameSir Cyclone 2 and its receiver";

  # Everything a build, a flash or the host tests need, in one shell:
  #   nix develop           (or direnv: `use flake` in .envrc)
  # It works on any Linux with Nix (NixOS or not) and in CI
  # (`nix develop -c make -C firmware/test`). The JieLi SDK stays a git
  # checkout next to the repo (firmware/README.md): the build writes into it.
  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});

      # The JieLi toolchain (clang 4.0.1 for pi32v2), the build every
      # committed image was made with: fw-re/get-toolchain.sh's archive and
      # hash, its binaries pointed at Nix's glibc and libstdc++ (so no
      # nix-ld or /lib64 loader is needed), plus the two links the
      # distribution leaves out (cc1, the 4.0.1
      # resource dir). x86_64 only: JieLi ships no other build.
      jlToolchain = pkgs: pkgs.stdenv.mkDerivation {
        pname = "jieli-toolchain";
        version = "20260730.1";
        src = pkgs.fetchurl {
          url = "https://github.com/DimmKirr/setup-fm1-toolchain/releases/download/toolchain-20260730.1/toolchain-20260730.1.tar.xz";
          sha256 = "d3fc3be92cfc07ccb0ac682b3b2e41e4d14593426067e6c3c6e908db2f90c0e0";
        };
        nativeBuildInputs = [ pkgs.autoPatchelfHook ];
        buildInputs = [ pkgs.stdenv.cc.cc.lib ];
        dontConfigure = true;
        dontBuild = true;
        dontStrip = true;
        installPhase = ''
          runHook preInstall
          mkdir -p $out
          cp -a . $out/
          [ -e $out/common/bin/cc1 ] || ln -s clang $out/common/bin/cc1
          [ -e $out/common/lib/clang/4.0.1 ] || ln -s 4.0.0 $out/common/lib/clang/4.0.1
          runHook postInstall
        '';
      };

      # Python for the tools: the flasher's (fw-re/jlflash.sh, jl-uboot-tool:
      # pyyaml crcmod pycryptodomex tqdm) and tools/c2led-ui.py's PyGObject.
      jlPython = pkgs: pkgs.python3.withPackages (ps: [
        ps.pyyaml ps.crcmod ps.pycryptodomex ps.tqdm ps.pygobject3
      ]);
    in
    {
      packages = forAllSystems (pkgs:
        { python = jlPython pkgs; }
        // nixpkgs.lib.optionalAttrs (pkgs.stdenv.hostPlatform.system == "x86_64-linux") {
          jl-toolchain = jlToolchain pkgs;
        });

      devShells = forAllSystems (pkgs:
        let
          py = self.packages.${pkgs.stdenv.hostPlatform.system}.python;
          tc = self.packages.${pkgs.stdenv.hostPlatform.system}.jl-toolchain or null;
        in
        {
          default = pkgs.mkShell {
            packages = [
              py
              pkgs.gnumake pkgs.git             # firmware/build.sh, the host tests (gcc: the shell's cc)
              pkgs.curl pkgs.xz                 # fw-re/get-toolchain.sh (without Nix)
              pkgs.usbutils                     # lsusb: fw-re/jlflash.sh
              pkgs.socat                        # the Windows VM's monitor (USB passthrough)
            ] ++ nixpkgs.lib.optional (tc != null) tc;
            # tools/c2led-ui.py: gtk4, with its typelibs on GI_TYPELIB_PATH for PyGObject
            buildInputs = [ pkgs.gtk4 ];
            nativeBuildInputs = [ pkgs.gobject-introspection ];

            shellHook = ''
              ${nixpkgs.lib.optionalString (tc != null) ''
                # The scripts' toolchain (else fw-re/jl-toolchain, fw-re/get-toolchain.sh)
                export JL_TOOLCHAIN=${tc}
              ''}
              # fw-re/.jlpy: the flasher's Python. `sudo fw-re/jlflash.sh` drops
              # this shell's PATH, so the script finds it by this link. A real
              # directory there (not a link) is left alone.
              _root=$(git rev-parse --show-toplevel 2>/dev/null || pwd)
              if [ -d "$_root/fw-re" ] && { [ -L "$_root/fw-re/.jlpy" ] || [ ! -e "$_root/fw-re/.jlpy" ]; }; then
                ln -sfn ${py} "$_root/fw-re/.jlpy"
              fi
              unset _root
            '';
          };
        });
    };
}
