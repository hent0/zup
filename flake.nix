{
  description = "Zup";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = {
    self,
    nixpkgs,
    flake-utils,
  }:
    flake-utils.lib.eachDefaultSystem (system: let
      pkgs = import nixpkgs {inherit system;};

      # The C bootstrap seed. Kept buildable forever as the auditable
      # from-source path (and the fallback if a released seed misbehaves).
      stage0 = pkgs.stdenv.mkDerivation {
        pname = "zup-stage0";
        version = "0.0.1";

        src = ./.;

        nativeBuildInputs = with pkgs; [cmake makeWrapper];

        installPhase = ''
          runHook preInstall
          install -Dm755 zup $out/bin/zup
          cp -r ../std $out/std
          wrapProgram $out/bin/zup --prefix PATH : ${pkgs.lib.makeBinPath [pkgs.clang]}
          runHook postInstall
        '';

        meta = {
          description = "Zup compiler (C bootstrap seed)";
          mainProgram = "zup";
        };
      };

      # The real compiler: zup compiled by zup. Builds the full chain
      # (stage0 -> stage1 -> stage2) and ships stage2. The binary resolves
      # std exe-relative ($out/bin/../std), so the $out/std layout works
      # from any cwd.
      zup = pkgs.stdenv.mkDerivation {
        pname = "zup";
        version = "0.0.1";

        src = ./.;

        nativeBuildInputs = with pkgs; [cmake makeWrapper clang];

        buildPhase = ''
          runHook preBuild
          make -j$NIX_BUILD_CORES
          ./zup ../stage1/src/main.zup -o stage1
          ./stage1 ../stage1/src/main.zup -o stage2
          runHook postBuild
        '';

        installPhase = ''
          runHook preInstall
          install -Dm755 stage2 $out/bin/zup
          cp -r ../std $out/std
          wrapProgram $out/bin/zup --prefix PATH : ${pkgs.lib.makeBinPath [pkgs.clang]}
          runHook postInstall
        '';

        meta = {
          description = "Zup compiler (self-hosted)";
          mainProgram = "zup";
        };
      };
    in {
      packages = {
        default = zup;
        inherit stage0;
      };

      # The release loop: bootstrap the chain, require the self-fixed-point,
      # run the full suite through the self-compiled compiler.
      checks.default = pkgs.stdenv.mkDerivation {
        pname = "zup-bootstrap-check";
        version = "0.0.1";

        src = ./.;

        nativeBuildInputs = with pkgs; [cmake clang];

        buildPhase = ''
          runHook preBuild
          make -j$NIX_BUILD_CORES
          ./zup ../stage1/src/main.zup -o stage1
          ./stage1 ../stage1/src/main.zup -o stage2
          ./stage1 ../stage1/src/main.zup -ir -o s1.ll
          ./stage2 ../stage1/src/main.zup -ir -o s2.ll
          diff s1.ll s2.ll
          cd ..
          ZUP_BIN=build/stage2 bash ./run-tests.sh
          cd build
          runHook postBuild
        '';

        installPhase = "touch $out";
      };

      devShells.default = pkgs.mkShell {
        packages = with pkgs; [
          cmake # build system
          clang
          just
          valgrind # memory debugging
          gdb
          llvm
        ];
      };
    });
}
