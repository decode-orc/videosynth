# Example projects and scripts

The repository ships a set of hand-authored projects under `projects/`, covering every feature this manual describes. They are the fastest way to see a working configuration for something.

## The suites

| Directory | Contents |
|-----------|----------|
| `projects/general/` | Feature examples — audio, EFM audio, VITS, progressive EXR and MKV sources, the PAL pilot burst |
| `projects/general-yc/` | PAL and NTSC audio examples with Y/C output |
| `projects/stacking/` | Laserdisc disc simulation and multi-source stacking sets for PAL, PAL-M and NTSC |
| `projects/long-form/` | Three-hour capture-sized PAL and NTSC examples, for testing against realistic file sizes |
| `projects/benchmark/` | Fixed-length PAL/NTSC still, moving-source and noise projects used by the benchmark script |
| `projects/variants.json` | Rules for mechanically derived variants |

!!! warning "The long-form projects are very large"
    They write hundreds of gigabytes each — roughly 383 GB for PAL and 309 GB for NTSC of composite samples, plus about 3.1 GB of audio. They are deliberately excluded from the automated suites; run them by hand with `--output-root` pointing at storage that can hold the result.

Derived variants — the impairment-free `stacking-clean` set and the Y/C `stacking-yc` set — are generated into `build/generated-projects/` at build time and are not committed. Add a variant by editing `variants.json`, not by copying YAML.

## Running a suite

`scripts/run-projects.sh` runs whole suites through the CLI. It re-execs itself inside the Nix development shell when needed (the MKV fixtures need `ffprobe`), and writes everything into `build/project-output/<suite>/` via the `{output}` root, so nothing lands in the source tree.

```bash
# List the suites and the directories each covers
scripts/run-projects.sh --list

# Run everything
scripts/run-projects.sh

# Run one suite
scripts/run-projects.sh general
scripts/run-projects.sh stacking

# Pass extra arguments through to videosynth
scripts/run-projects.sh general -- --threads 1 --log-level trace
```

Build first — the script expects `build/videosynth` and picks up the generated variants from the build tree. Set `VIDEOSYNTH_BUILD_DIR` to use a different build directory. It reports `PASS`/`FAIL` per project and exits non-zero if any run fails.

To run a single project by hand:

```bash
./build/videosynth --project projects/general/pal_progressive_exr.yaml
```

Without `--output-root` the media is written beside the YAML.

## Benchmarking

`scripts/benchmark.sh` times the fixed-length projects in `projects/benchmark/` once per thread configuration and prints a frames-per-second table. Frame counts come from the CLI's own log, and all media is written under `build/project-output/benchmark/`.

```bash
# Both thread configurations (1 and auto) over every benchmark project
scripts/benchmark.sh

# One project, best of three runs, results appended to a CSV
scripts/benchmark.sh pal_still --repeat 3 --csv build/benchmark.csv

# Single-threaded only
scripts/benchmark.sh --threads "1"
```

## Comparing output between builds

`scripts/output-hashes.sh` records and compares SHA-256 manifests of the generated media — `.cvbs`, `.cvbsy`, `.cvbsc`, the audio `.wav` files, and the `.meta` sidecar as its canonical `sqlite3 .dump` — so a change can be asserted byte-identical to a recorded baseline. It regenerates the suites through `run-projects.sh` unless `--skip-run` is given; manifests live in `build/output-hashes/`.

```bash
# On a known-good build: record the baseline
scripts/output-hashes.sh --record

# After a change: fails and lists the artefacts that differ
scripts/output-hashes.sh
```

## Other scripts

| Script | Purpose |
|--------|---------|
| `scripts/generate_test_projects.py` | Derive variant projects from `projects/` (run automatically by the build) |
| `scripts/compare_frames.py` | Bit-exact comparison of a picture number across stacking sources |
| `scripts/compare_frames_detail.py` | As above, split by VBI versus active video |
| `scripts/convert_progressive_raw_to_exr.py` | Convert raw `yuv422p10le` stills to OpenEXR fixtures |
| `scripts/run_stage1_raw_to_exr_conversion.sh` | Run the EXR conversion in an ad-hoc Nix shell |
| `scripts/render-icons.sh` | Render the application icon PNG set from the logo SVG |
| `scripts/prepare-flatpak-manifest.py` | Write a copy of the Flatpak manifest with the release version and date filled in |
