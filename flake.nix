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

      stage0 = pkgs.stdenv.mkDerivation {
        pname = "zup-stage0";
        version = "0.0.1";

        src = ./.;

        nativeBuildInputs = with pkgs; [cmake makeWrapper];

        cmakeDir = "../stage0";

        installPhase = ''
          runHook preInstall
          install -Dm755 stage0 $out/bin/zup
          cp -r ../std $out/std
          wrapProgram $out/bin/zup --prefix PATH : ${pkgs.lib.makeBinPath [pkgs.clang]}
          runHook postInstall
        '';

        meta = {
          description = "Zup compiler (C bootstrap seed)";
          mainProgram = "zup";
        };
      };

      zup = pkgs.stdenv.mkDerivation {
        pname = "zup";
        version = "0.0.1";

        src = ./.;

        nativeBuildInputs = with pkgs; [cmake makeWrapper clang];

        cmakeDir = "../stage0";

        buildPhase = ''
          runHook preBuild
          make -j$NIX_BUILD_CORES
          ./stage0 ../src/main.zup -o stage1
          ./stage1 ../src/main.zup -o stage2
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
          description = "Zup compiler";
          mainProgram = "zup";
        };
      };
    in {
      packages = {
        default = zup;
        inherit stage0;
      };

      checks.default = pkgs.stdenv.mkDerivation {
        pname = "zup-bootstrap-check";
        version = "0.0.1";

        src = ./.;

        nativeBuildInputs = with pkgs; [cmake clang];

        cmakeDir = "../stage0";

        buildPhase = ''
          runHook preBuild
          make -j$NIX_BUILD_CORES
          ./stage0 ../src/main.zup -o stage1
          ./stage1 ../src/main.zup -o stage2
          ./stage1 ../src/main.zup -ir -o s1.ll
          ./stage2 ../src/main.zup -ir -o s2.ll
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
          cmake
          clang
          just
          valgrind
          gdb
          llvm
        ];
      };
    });
}
