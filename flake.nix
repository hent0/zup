{
  description = "Shlange";

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
        pname = "slang";
        version = "0.0.1";

        src = ./.;

        nativeBuildInputs = with pkgs; [cmake];
        buildInputs = [];

        installPhase = ''
          runHook preInstall
          install -Dm755 shell $out/bin/slang
          runHook postInstall
        '';

        meta = {
          description = "Slang";
          mainProgram = "slang";
        };
      };

      devShells.default = pkgs.mkShell {
        packages = with pkgs; [
          cmake # build system
          clang
          valgrind # memory debugging
          gdb
        ];
      };
    });
}
