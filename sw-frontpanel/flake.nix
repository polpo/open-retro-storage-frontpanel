{
  description = "Front panel development environment";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";
    nixpkgs-esp-dev.url = "github:mirrexagon/nixpkgs-esp-dev";
    nixpkgs-esp-dev.inputs.nixpkgs.follows = "nixpkgs";
    # nixpkgs dropped python310 (2026-02-15), but the prebuilt esp-elf-gdb tool
    # hard-links libpython3.10.so.1.0, so autoPatchelf needs the real interpreter.
    # Pin a pre-drop nixpkgs solely to source python310 for the overlay below.
    nixpkgs-python310.url = "github:NixOS/nixpkgs/aa290c9891fa4ebe88f8889e59633d20cc06a5f2";
  };

  outputs = { self, nixpkgs, nixpkgs-esp-dev, nixpkgs-python310 }:
    let
      system = "x86_64-linux";
      pkgs = import nixpkgs { inherit system; };
      esppkgs = import nixpkgs {
        inherit system;
        overlays = [
          # Supply the dropped python310 (real interpreter, for the esp-elf-gdb
          # autoPatchelf) from the pinned pre-drop nixpkgs.
          (final: prev: {
            python310 = (import nixpkgs-python310 { inherit system; }).python310;
          })
          nixpkgs-esp-dev.overlays.default
        ];
        config.permittedInsecurePackages = [
          "python3.13-ecdsa-0.19.2"
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

      # Web-asset build deps (main/CMakeLists.txt -> tools/minify_www.py). Built
      # against the ESP-IDF interpreter so the modules are ABI-compatible, then
      # exposed to it via PYTHONPATH below (the IDF Python lives in the immutable
      # /nix/store, so its `pip install -r requirements.txt` is blocked by PEP 668).
      preprocess = esppkgs.python3Packages.buildPythonPackage rec {
        pname = "preprocess";
        version = "2.0.0";
        format = "setuptools";
        src = esppkgs.python3Packages.fetchPypi {
          inherit pname version;
          hash = "sha256-+3UfMhi3lIojzYIViMKWaVwBQfdaLkggGv6OJmUYq6o=";
        };
        # 2.0.0 pulls `cmp` from the py2-compat `past` shim (unsupported on
        # py3.13); its only use is a py2-only list.sort(cmp) we never hit. Inline it.
        postPatch = ''
          substituteInPlace lib/preprocess.py \
            --replace-fail "from past.builtins import cmp" "cmp = lambda a, b: (a > b) - (a < b)"
        '';
        pythonImportsCheck = [ "preprocess" ];
        doCheck = false;
      };

      wwwPythonEnv = esppkgs.python3.withPackages (ps: [
        ps.rjsmin   # JS minification
        ps.htmlmin  # HTML minification (script imports `htmlmin`, not htmlmin4)
        preprocess  # product-gating of web assets (hard requirement)
      ]);
    in
    {
      devShells.${system}.default = pkgs.mkShell {
        shellHook = ''
          export PICO_SDK_PATH="${pico-sdk}"
          export PICO_EXTRAS_PATH="${pico-extras}"
          export IDF_TOOLS_PATH="${esppkgs.esp-idf-full}"
          # Make the web-asset build deps importable by the IDF Python.
          export PYTHONPATH="${wwwPythonEnv}/${wwwPythonEnv.sitePackages}''${PYTHONPATH:+:$PYTHONPATH}"
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
