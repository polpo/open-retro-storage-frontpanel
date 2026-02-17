{
  description = "Front panel development environment";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";
    nixpkgs-esp-dev.url = "github:mirrexagon/nixpkgs-esp-dev";
    nixpkgs-esp-dev.inputs.nixpkgs.follows = "nixpkgs";
  };

  outputs = { self, nixpkgs, nixpkgs-esp-dev }:
    let
      system = "x86_64-linux";
      pkgs = import nixpkgs { inherit system; };
      esppkgs = import nixpkgs {
        inherit system;
        overlays = [ nixpkgs-esp-dev.overlays.default ];
        config.permittedInsecurePackages = [
          "python3.13-ecdsa-0.19.1"
        ];
      };

      pico-sdk = pkgs.fetchFromGitHub {
        owner = "raspberrypi";
        repo = "pico-sdk";
        rev = "2.2.0";
        fetchSubmodules = true;
        hash = "sha256-8ubZW6yQnUTYxQqYI6hi7s3kFVQhe5EaxVvHmo93vgk=";
      };

      pico-extras = pkgs.fetchFromGitHub {
        owner = "raspberrypi";
        repo = "pico-extras";
        rev = "sdk-2.2.0";
        fetchSubmodules = true;
        hash = "sha256-AfMycI+CMl76OERyRN8xQer7erh0wxpJnD4fu/Sl18c=";
      };
    in
    {
      devShells.${system}.default = pkgs.mkShell {
        shellHook = ''
          export PICO_SDK_PATH="${pico-sdk}"
          export PICO_EXTRAS_PATH="${pico-extras}"
          export IDF_TOOLS_PATH="${esppkgs.esp-idf-full}"
          echo "PicoIDE development environment activated."
          echo "PICO_SDK_PATH set to: $PICO_SDK_PATH"
          echo "PICO_EXTRAS_PATH set to: $PICO_EXTRAS_PATH"
          echo "IDF_TOOLS_PATH set to: $IDF_TOOLS_PATH"
        '';

        buildInputs = [
          pkgs.cmake
          pkgs.gcc-arm-embedded
          pkgs.picotool
          esppkgs.esp-idf-full
        ];
      };
    };
}
