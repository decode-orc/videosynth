{
  description = "VideoSynth MVP foundation";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-26.05";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs { inherit system; };
        # Git metadata is unavailable inside the Nix build sandbox, so the
        # commit hash is passed in as a version override instead.
        versionString = self.shortRev or self.dirtyShortRev or "0.1.0";

        # Python environment for the MkDocs documentation site. Uses the default
        # python3 so packages come from the binary cache: Hydra only builds the
        # default interpreter's package set, so pinning e.g. python312 forces
        # local source builds (and their test suites).
        mkdocsPythonEnv = pkgs.python3.withPackages (ps: [
          ps.mkdocs
          ps.mkdocs-material
          ps.mkdocs-awesome-nav
        ]);

        videosynth-docs = pkgs.stdenv.mkDerivation {
          pname = "videosynth-docs";
          version = versionString;
          src = self;

          nativeBuildInputs = [ mkdocsPythonEnv ];

          buildPhase = ''
            mkdocs build
          '';

          installPhase = ''
            mkdir -p $out
            cp -r site/* $out/
          '';

          meta = with pkgs.lib; {
            description = "videosynth documentation site";
            homepage = "https://github.com/decode-orc/videosynth";
            license = licenses.gpl3Plus;
            platforms = platforms.all;
          };
        };
      in {
        packages.default = pkgs.stdenv.mkDerivation {
          pname = "videosynth";
          version = versionString;
          src = self;

          nativeBuildInputs = with pkgs; [
            cmake
            ninja
            pkg-config
            qt6.wrapQtAppsHook
          ];

          buildInputs = with pkgs; [
            yaml-cpp
            spdlog
            gtest
            sqlite
            openexr
            zlib
            ffmpeg
            qt6.qtbase
            qt6.qtsvg
          ];

          cmakeFlags = [
            "-DCMAKE_BUILD_TYPE=Release"
            "-DBUILD_TESTING=ON"
            "-DPROJECT_VERSION_OVERRIDE=${versionString}"
            # The build sandbox only exposes the stdenv's unwrapped clang-tidy,
            # which cannot resolve libc++ headers. clang-tidy is a dev/CI gate.
            "-DVIDEOSYNTH_ENABLE_CLANG_TIDY=OFF"
            # Point the bundled asset root at the installed data directory so the
            # binary resolves "{bundled}" assets after the sandbox is gone. The
            # assets are vendored in-tree under videosynth-assets/;
            # VIDEOSYNTH_ASSET_DIR overrides at runtime.
            "-DVIDEOSYNTH_BUNDLED_ASSET_DIR=${placeholder "out"}/share/videosynth/assets"
          ];

          doCheck = true;
          # Keep the sandboxed check to the fast, mocked unit lane (AGENTS.md
          # §3.1). Functional suites load real media from videosynth-assets/ and
          # run in the dev shell or a dedicated CI job.
          checkPhase = ''
            ctest -L unit --output-on-failure
          '';
        };

        packages.docs = videosynth-docs;

        checks.default = self.packages.${system}.default;

        devShells.default = pkgs.mkShell {
          packages = with pkgs; [
            cmake
            ninja
            pkg-config
            python3
            python3Packages.numpy
            gcc
            yaml-cpp
            spdlog
            gtest
            sqlite
            openexr
            zlib
            ffmpeg
            qt6.qtbase
            qt6.qtsvg
            clang-tools
            ccache
            mkdocsPythonEnv
          ];

          # Qt needs its platform plugins on the path when the GUI is run
          # directly from the dev shell (outside wrapQtAppsHook).
          shellHook = ''
            export QT_PLUGIN_PATH="${pkgs.qt6.qtbase}/lib/qt-6/plugins''${QT_PLUGIN_PATH:+:$QT_PLUGIN_PATH}"
          '';
        };
      });
}
