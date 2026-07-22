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
    in {
      packages.default = pkgs.stdenv.mkDerivation {
        pname = "zup";
        version = "0.0.1";

        src = ./.;

        nativeBuildInputs = with pkgs; [cmake makeWrapper];
        buildInputs = [];

        installPhase = ''
          runHook preInstall
          install -Dm755 zup $out/bin/zup
          cp -r ../std $out/std
          wrapProgram $out/bin/zup --prefix PATH : ${pkgs.lib.makeBinPath [pkgs.clang]}
          runHook postInstall
        '';

        meta = {
          description = "Zup";
          mainProgram = "zup";
        };
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
