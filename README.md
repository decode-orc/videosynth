# videosynth

A video signal synthesizer for PAL, PAL-M, NTSC and SECAM.

📖 **[Read the documentation at decode-orc.github.io/videosynth](https://decode-orc.github.io/videosynth)** —
installation, user manual and the complete project-file reference.

**videosynth** generates analogue composite (CVBS) and Y/C video signals as
sample files, closely following the published analogue video standards
(ITU-R BT.470/BT.1700, SMPTE 170M/244M, IEC 60856/60857, IEC 60461). It is
aimed at video engineers and media-preservation developers who need
known-good, reproducible test signals — including laserdisc-style discs with
VBI codes, noise, dropouts and player tracking errors — to exercise decoders,
time base correctors and restoration tools.

The project ships two front ends built from the same pipeline:

- `videosynth` — the command-line generator.
- `videosynth-gui` — a Qt 6 desktop application for authoring projects,
  previewing frames and running generation.

---

## Features

- **Standards-based signal generation** for PAL, PAL-M, NTSC and 625-line
  SECAM, generated in the time domain from the standards' timing and level
  definitions.
- **Sample encodings**: 4fsc (`CVBS_U10_4FSC`, `CVBS_U16_4FSC`,
  `CVBS_TPG21_4FSC`, `CVBS_S16_4FSC`) and raw high-rate (`RAW_S16_28M`,
  `RAW_S16_40M`). The signed and raw presets carry sub-sync excursions such as
  the laserdisc pilot burst.
- **Composite or Y/C output** (`output.signal_type: composite | yc`).
- **Frame content** from validated progressive source profiles (EXR stills and
  MKV video).
- **Line injections** for VBI content: VITS test signals, laserdisc biphase and
  FM codes (IEC 60856/60857), and VITC timecode (SMPTE 12M).
- **Laserdisc disc simulation**: CAV/CLV picture numbers, chapters, programme
  status codes, lead-in/out, picture stop, PAL pilot burst, NTSC VBI burst.
- **Per-section impairments**: two-component Gaussian noise (floor plus
  proportional) and dropout injection (random surface dropouts and persistent
  scratches) with a SQLite dropout sidecar.
- **Multi-track audio**: up to eight stereo channel pairs at 48 kHz/24-bit,
  plus EFM audio for laserdisc projects.
- **On-screen display** text with substitution tokens (picture number, CLV
  timecode, frame number).
- **Deterministic multi-threaded generation** — output is byte-identical
  regardless of thread count.

---

## Install

Tagged releases publish a Linux Flatpak bundle. Download
`videosynth-<version>-x86_64.flatpak` from the
[releases page](https://github.com/decode-orc/videosynth/releases) and install it
for the current user:

```bash
flatpak install --user videosynth-<version>-x86_64.flatpak
```

The GUI is the default entry point and appears in the GNOME (and other XDG)
application menus as **VideoSynth**. The CLI ships in the same bundle:

```bash
flatpak run io.github.decode_orc.VideoSynth                     # GUI
flatpak run --command=videosynth io.github.decode_orc.VideoSynth --help
```

The bundle carries the media assets from
[videosynth-assets/](videosynth-assets/), so `{bundled}` sources resolve without
a checkout, and it is granted read/write access to the host filesystem for
reading source media and writing generated output. See
[packaging/README.md](packaging/README.md) for the manifest and its permissions.

Building from source is covered below.

---

## Requirements

The Nix flake is the authoritative environment; the development shell supplies
CMake, Ninja, a C++17 toolchain, yaml-cpp, spdlog, Google Test, SQLite,
OpenEXR, zlib, FFmpeg, Qt 6 and Python 3 (with NumPy).

```bash
git clone --recurse-submodules https://github.com/decode-orc/videosynth
cd videosynth
```

Submodules provide the specification documents under [docs-tech/](docs-tech/).
The media assets in [videosynth-assets/](videosynth-assets/) are vendored
directly into this repository and need no extra checkout step. If you already
cloned without the specification submodules:

```bash
git submodule update --init --recursive
```

---

## Build

```bash
# Configure and build (CLI + GUI + tests)
nix develop "path:$PWD" --command cmake -S . -B build -G Ninja
nix develop "path:$PWD" --command cmake --build build
```

Binaries land in `build/videosynth` and `build/videosynth-gui`.

Generation is compute bound, so a build without `CMAKE_BUILD_TYPE` set defaults
to `Release` (`-O3 -DNDEBUG`) rather than to no optimisation at all; the chosen
type is printed at configure time. Override it for debugging work:

```bash
# Unoptimised build with debug symbols
nix develop "path:$PWD" --command cmake -S . -B build-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
```

Multi-config generators (Ninja Multi-Config, Visual Studio, Xcode) are left
alone — they select the configuration at build time.

Useful CMake options:

| Option | Default | Effect |
|--------|---------|--------|
| `-DCMAKE_BUILD_TYPE=<type>` | `Release` | `Debug`, `Release`, `RelWithDebInfo` or `MinSizeRel` |
| `-DVIDEOSYNTH_BUILD_GUI=OFF` | `ON` | Skip the Qt 6 GUI target |
| `-DVIDEOSYNTH_BUILD_TESTS=OFF` | `ON` | Skip the test targets and the Google Test dependency |
| `-DVIDEOSYNTH_ENABLE_CLANG_FORMAT=OFF` | `ON` | Skip the clang-format check during the build |
| `-DVIDEOSYNTH_ENABLE_CLANG_TIDY=OFF` | `ON` | Skip clang-tidy static analysis during the build |
| `-DVIDEOSYNTH_BUNDLED_ASSET_DIR=<dir>` | dev tree | Where `{bundled}` resolves for installed builds |
| `-DVIDEOSYNTH_RELEASE_VERSION=<v>` | build version | Version written into the installed AppStream metadata |
| `-DVIDEOSYNTH_RELEASE_DATE=<date>` | today, UTC | Date written into the installed AppStream metadata |

`cmake --install` places the binaries, the bundled assets and — when the GUI is
built — the desktop entry, AppStream metadata and icon theme entries that make
the application appear in the GNOME (and other XDG) menus. See
[packaging/README.md](packaging/README.md).

To build and run the packaged derivation instead of the dev shell:

```bash
nix run . -- --project projects/general/pal_vits.yaml
```

The bundled media assets are part of this repository, so no `?submodules=1`
qualifier is needed.

---

## Run

### Command line

```bash
nix develop "path:$PWD" --command ./build/videosynth --project projects/general/pal_vits.yaml
```

Options:

| Option | Description | Default |
|--------|-------------|---------|
| `--project <path>` | YAML project file (required) | — |
| `--validate` | Validate only; generate nothing | off |
| `--version` | Print the build version (git commit hash) | — |
| `--threads <n>` | Frame synthesis workers: `auto` or a positive integer (`1` = sequential) | `auto` |
| `--template-cache-mb <n>` | Frame template cache capacity in MiB (`0` disables; output is byte-identical either way) | `512` |
| `--log-level <level>` | `info`, `debug` or `trace` | `info` |
| `--log-file <path>` | Also write logs to a file | none |
| `--asset-root <name>=<path>` | Map the `{name}/…` logical asset root | built-ins |
| `--output-root <path>` | Where `{output}/…` resolves (sugar for `--asset-root output=<path>`) | project dir |

Examples:

```bash
# Validate without generating
./build/videosynth --project projects/general/ntsc_vits.yaml --validate

# Sequential generation with debug logging to a file
./build/videosynth --project projects/general/pal_audio.yaml \
    --threads 1 --log-level debug --log-file out/videosynth.log

# Send all generated media to a scratch directory
./build/videosynth --project projects/stacking/pal_discsim_A.yaml \
    --output-root /tmp/videosynth-out
```

### GUI

```bash
nix develop "path:$PWD" --command ./build/videosynth-gui
```

`videosynth-gui` accepts the same `--log-level` and `--log-file` options, which
override the persisted preferences for the session. It opens on a welcome page:
create a project (the video standard is fixed at creation time), add sections,
edit line injections, impairments and audio, preview frames in a detached
preview window, then generate. Validation runs continuously in the background
and reports into the issues dock.

---

## Projects

A project is a YAML file describing the output, the global line injections and
an ordered list of sections. A minimal example:

```yaml
project:
  name: PALVitsFixture
  version: "1.0"
  description: PAL VITS fixture

cvbs_presets:
  video_standard_preset: PAL          # PAL | PAL_M | NTSC
  sample_encoding_preset: CVBS_TPG21_4FSC
  signal_state_preset: STANDARD_STABLE_LOCKED

output:
  video_path: "{output}/videosynth_pal_vits.cvbs"

line_injections:
  vits:
    - vits_type: vits17
      target_lines: [17]

sections:
  - name: PalVits
    type: progressive
    source: "{bundled}/exr/720x576/100_BARS.exr"
    duration_frames: 1
```

The full schema is documented in
[docs-tech/design/high-level-design.md](docs-tech/design/high-level-design.md) §7–§11;
laserdisc biphase authoring is covered in
[docs-tech/user/biphase-design.md](docs-tech/user/biphase-design.md).

### Logical asset roots

Source and output paths use `{name}/…` brace tokens rather than machine-specific
paths, so projects stay portable between the CLI, the GUI and packaged builds:

| Root | Resolves to | Override |
|------|-------------|----------|
| `{bundled}` | Installed/bundled media assets | `VIDEOSYNTH_ASSET_DIR`, `--asset-root bundled=…` |
| `{user}` | `$XDG_DATA_HOME/videosynth/assets` (else `~/.local/share/videosynth/assets`) | `--asset-root user=…` |
| `{project}` | Directory holding the project file | — |
| `{output}` | Project directory unless overridden | `VIDEOSYNTH_OUTPUT_DIR`, `--output-root` |

Always use `{bundled}/…` for fixture sources: a bare relative path resolves
against the working directory in the CLI but against the project directory in
the GUI.

### Output artefacts

All artefacts are colocated with `output.video_path`:

| File | Contents |
|------|----------|
| `<name>.cvbs` | Composite samples |
| `<name>.cvbsy` / `<name>.cvbsc` | Y/C luma and chroma samples |
| `<name>.meta` | SQLite metadata sidecar |
| `<name>.dropouts.meta` | SQLite dropout sidecar (schema v5), when any section injects dropouts |
| `<name>_audio_<pair>.wav` | 48 kHz/24-bit stereo audio per channel pair |
| `<name>.efm` / `<name>.efm.meta` | LaserDisc digital audio channel stream and its frame index, when `output.efm_audio` is set |

---

## Example projects

Hand-authored projects live in [projects/](projects/):

| Directory | Contents |
|-----------|----------|
| [projects/general/](projects/general/) | Feature examples — audio, EFM audio, VITS, progressive EXR/MKV sources, PAL pilot burst (composite) |
| [projects/general-yc/](projects/general-yc/) | Feature examples with Y/C output |
| [projects/stacking/](projects/stacking/) | Laserdisc disc simulation and multi-source stacking sets for PAL, PAL-M and NTSC |
| [projects/long-form/](projects/long-form/) | Three-hour capture-sized PAL and NTSC examples (VITS, analogue stereo audio) for testing against realistic file sizes |
| [projects/benchmark/](projects/benchmark/) | Fixed-length PAL/NTSC still, moving-source and noise projects used by [scripts/benchmark.sh](scripts/benchmark.sh) |
| [projects/variants.json](projects/variants.json) | Rules for mechanically derived variants |

The `long-form` projects write hundreds of gigabytes each (~383 GB PAL,
~309 GB NTSC of composite samples, plus ~3.1 GB of audio) and are deliberately
excluded from the suites below; run them by hand with `--output-root` pointing
at storage that can hold the result.

Derived variants (the impairment-free `stacking-clean` set and the Y/C
`stacking-yc` set) are generated into `build/generated-projects/` at build time
by [scripts/generate_test_projects.py](scripts/generate_test_projects.py) and
are not committed. Add a variant by editing `variants.json`, not by copying
YAML.

### Running the example suites

[scripts/run-projects.sh](scripts/run-projects.sh) runs whole suites through the
CLI. It re-execs itself inside the Nix dev shell when needed (the MKV fixtures
need ffprobe), and writes everything into `build/project-output/<suite>/` via
the `{output}` root, so nothing lands in the source tree.

```bash
# List the suites and the directories each covers
scripts/run-projects.sh --list

# Run everything (general + stacking)
scripts/run-projects.sh

# Run one suite
scripts/run-projects.sh general
scripts/run-projects.sh stacking

# Pass extra arguments through to videosynth
scripts/run-projects.sh general -- --threads 1 --log-level trace
```

Build first — the script expects `build/videosynth` and picks up the generated
variants from the build tree. Set `VIDEOSYNTH_BUILD_DIR` to use a build
directory other than `build/`. The script reports `PASS`/`FAIL` per project and
exits non-zero if any run fails.

To run a single project by hand, invoke the binary directly; without
`--output-root` the media is written beside the YAML:

```bash
./build/videosynth --project projects/general/pal_progressive_exr.yaml
```

### Benchmarking and output comparison

[scripts/benchmark.sh](scripts/benchmark.sh) times the fixed-length projects in
[projects/benchmark/](projects/benchmark/) — PAL still, PAL still with noise,
PAL moving source (MKV), a PAL still-then-clip section boundary, and NTSC
still — once per thread configuration, and prints a
frames/second table. Frame counts come from the CLI's own log, and all
media is written under `build/project-output/benchmark/` via `{output}`.

```bash
# Both thread configurations (1 and auto) over every benchmark project
scripts/benchmark.sh

# One project, best of three runs, results appended to a CSV
scripts/benchmark.sh pal_still --repeat 3 --csv build/benchmark.csv

# Single-threaded only
scripts/benchmark.sh --threads "1"
```

[scripts/output-hashes.sh](scripts/output-hashes.sh) records and compares
SHA-256 manifests of the generated media (`.cvbs`, `.cvbsy`, `.cvbsc`, audio
`.wav`, and the `.meta` sidecar as its canonical `sqlite3 .dump`), so a change
can be asserted byte-identical to a recorded baseline. It regenerates the suites
through `run-projects.sh` unless `--skip-run` is given; manifests live in
`build/output-hashes/`.

```bash
# On the known-good build: record the baseline
scripts/output-hashes.sh --record

# After a change: fails and lists the artefacts that differ
scripts/output-hashes.sh
```

### Other scripts

| Script | Purpose |
|--------|---------|
| [scripts/generate_test_projects.py](scripts/generate_test_projects.py) | Derive variant projects from `projects/` (run automatically by the build) |
| [scripts/compare_frames.py](scripts/compare_frames.py) | Bit-exact comparison of a picture number across stacking sources |
| [scripts/compare_frames_detail.py](scripts/compare_frames_detail.py) | As above, split by VBI vs active video |
| [scripts/convert_progressive_raw_to_exr.py](scripts/convert_progressive_raw_to_exr.py) | Convert raw yuv422p10le stills to OpenEXR fixtures |
| [scripts/run_stage1_raw_to_exr_conversion.sh](scripts/run_stage1_raw_to_exr_conversion.sh) | Run the EXR conversion in an ad-hoc Nix shell |
| [scripts/render-icons.sh](scripts/render-icons.sh) | Render the application icon PNG set from the logo SVG |

---

## Tests

```bash
# Build every test binary
nix develop "path:$PWD" --command cmake --build build --target videosynth_tests

# Fast, mocked, hermetic lane
nix develop "path:$PWD" --command ctest --test-dir build -L unit --output-on-failure

# Filesystem, real media and full-pipeline tests (needs the bundled media assets)
nix develop "path:$PWD" --command ctest --test-dir build -L functional --output-on-failure
```

Classification follows the directory: sources under `tests/unit/` and
`tests/gui/unit/` are labelled `unit`, those under `tests/functional/` and
`tests/gui/functional/` are labelled `functional`. See
[TESTING.md](TESTING.md) for the testing strategy and conventions.

---

## Repository layout

```
include/videosynth/   Public headers (pipeline, model, encoders, generators)
src/                  CLI and library implementation
src/gui/              Qt 6 GUI application
tests/                Unit and functional test suites
projects/             Hand-authored example/fixture projects
scripts/              Project runners and maintenance utilities
packaging/            Desktop entry, AppStream metadata and Flatpak manifest
assets/               Application logo and icons
videosynth-assets/    Bundled media assets (source stills and video)
docs/                 MkDocs sources for the published documentation site
docs-tech/design/     High-level design specification
docs-tech/user/       In-repository feature documentation
docs-tech/*-specification*/  Specification submodules (CVBS format, analogue video)
```

---

## Documentation

The user manual and project-file reference are published at
**[decode-orc.github.io/videosynth](https://decode-orc.github.io/videosynth)** and
are built with MkDocs from [docs/](docs/). To build and preview them locally:

```bash
nix develop "path:$PWD" --command mkdocs serve   # http://127.0.0.1:8000
nix build .#docs                                 # static site in ./result
```

In-repository documents:

- [docs-tech/design/high-level-design.md](docs-tech/design/high-level-design.md) — design
  specification and YAML project reference.
- [docs-tech/user/biphase-design.md](docs-tech/user/biphase-design.md) — laserdisc
  biphase/FM code authoring.
- [docs-tech/cvbs-file-format-specification/](docs-tech/cvbs-file-format-specification/) —
  CVBS file format and its extensions.
- [docs-tech/analogue-video-specifications/](docs-tech/analogue-video-specifications/) —
  the analogue video and laserdisc standards the implementation cites.
- [TESTING.md](TESTING.md) — testing vision, strategy and repository rules.
- [AGENTS.md](AGENTS.md) — contribution and coding standards.

---

## Licence

GNU General Public License v3.0 or later. See [LICENSE](LICENSE).
