# Installation

videosynth is distributed as a Linux Flatpak bundle for tagged releases, and can be built from source on any platform the Nix development shell supports.

## Linux Flatpak

Download `videosynth-<version>-x86_64.flatpak` from the [releases page](https://github.com/decode-orc/videosynth/releases){target="_blank"} and install it for the current user:

```bash
flatpak install --user videosynth-<version>-x86_64.flatpak
```

The GUI is the default entry point and appears in the GNOME (and other XDG) application menus as **VideoSynth**. The command-line generator ships in the same bundle:

```bash
flatpak run io.github.decode_orc.VideoSynth                     # GUI
flatpak run --command=videosynth io.github.decode_orc.VideoSynth --help
```

The bundle carries the media assets, so `{bundled}` source paths resolve without a source checkout, and it is granted read/write access to the host filesystem so it can read source media and write generated output.

!!! note "Generated files are large"
    A single frame of 4fsc PAL composite is about 1.4 MB. A three-hour PAL project is several hundred gigabytes. Point the output at storage that can hold the result — see [`{output}`](../reference/asset-roots.md).

## Building from source

The Nix flake is the authoritative build environment. It supplies CMake, Ninja, a C++17 toolchain, yaml-cpp, spdlog, Google Test, SQLite, OpenEXR, zlib, FFmpeg, Qt 6 and Python 3.

```bash
git clone --recurse-submodules https://github.com/decode-orc/videosynth
cd videosynth
```

The submodules provide the specification documents under `docs-tech/`. The media assets in `videosynth-assets/` are vendored directly into the repository and need no extra checkout step. If you already cloned without the submodules:

```bash
git submodule update --init --recursive
```

### Configure and build

```bash
nix develop "path:$PWD" --command cmake -S . -B build -G Ninja
nix develop "path:$PWD" --command cmake --build build
```

Binaries land in `build/videosynth` (CLI) and `build/videosynth-gui` (GUI).

Generation is compute bound, so a build with no `CMAKE_BUILD_TYPE` set defaults to `Release` (`-O3 -DNDEBUG`) rather than to an unoptimised build. The chosen type is printed at configure time. Override it for debugging work:

```bash
nix develop "path:$PWD" --command cmake -S . -B build-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
```

### Build options

| Option | Default | Effect |
|--------|---------|--------|
| `-DCMAKE_BUILD_TYPE=<type>` | `Release` | `Debug`, `Release`, `RelWithDebInfo` or `MinSizeRel` |
| `-DVIDEOSYNTH_BUILD_GUI=OFF` | `ON` | Skip the Qt 6 GUI target |
| `-DVIDEOSYNTH_BUILD_TESTS=OFF` | `ON` | Skip the test targets and the Google Test dependency |
| `-DVIDEOSYNTH_ENABLE_CLANG_FORMAT=OFF` | `ON` | Skip the clang-format check during the build |
| `-DVIDEOSYNTH_ENABLE_CLANG_TIDY=OFF` | `ON` | Skip clang-tidy static analysis during the build |
| `-DVIDEOSYNTH_BUNDLED_ASSET_DIR=<dir>` | dev tree | Where `{bundled}` resolves for installed builds |

`cmake --install` places the binaries, the bundled assets and — when the GUI is built — the desktop entry, AppStream metadata and icon theme entries that make the application appear in the desktop menus.

### Running without installing

```bash
# Command line
nix develop "path:$PWD" --command ./build/videosynth --project projects/general/pal_vits.yaml

# GUI
nix develop "path:$PWD" --command ./build/videosynth-gui
```

To build and run the packaged Nix derivation instead of the development shell:

```bash
nix run . -- --project projects/general/pal_vits.yaml
```

## Verifying the installation

```bash
videosynth --version
```

prints the build version (the git commit hash the binary was built from). To confirm the toolchain end to end, validate one of the bundled example projects without generating anything:

```bash
videosynth --project projects/general/pal_vits.yaml --validate
```

A clean run reports the project as valid and exits zero.

## Next steps

Work through the [Quick Start](quick-start.md), which builds a complete PAL CAV laserdisc project with VITS and IEC-compliant biphase metadata.
