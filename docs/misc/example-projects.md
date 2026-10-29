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

Derived variants — the `stacking-captures` set, the impairment-free `stacking-clean` set and the Y/C `stacking-yc` set — are generated into `build/generated-projects/` at build time and are not committed. Add a variant by editing `variants.json`, not by copying YAML.

### Pressings and captures

`projects/stacking/` also holds two pressings, A and B, for each of PAL, NTSC and PAL-M (`<standard>_pressing_A.yaml`, `<standard>_pressing_B.yaml`). Each is a 16-frame CAV disc (PNs 1–16) with:

- a shaped capture noise layer;
- a shaped [shared (pressing) noise layer](../user-manual/impairments.md#pressing-noise-and-capture-noise), seeded 7001 for pressing A and 7002 for pressing B;
- a [`channel:` timing offset](../reference/channel.md) of 0.0 samples for A and −1.0 for B. Captures of real pressings sit on a whole-sample grid of one another, so the two-pressing sets use a whole-sample offset; pressing C below is the sub-sample case.

PAL also has a third pressing, C (`pal_pressing_C.yaml`), with the same content and layout. Its shared noise is seeded 7003, and its `channel:` block adds the two glass-master and readout impairments that are PAL-only or new to it:

- [`fm_beat`](../reference/channel.md#fm_beat) at −24 dB, the FM-beat herringbone, visible in the bars and the colour ramps;
- [`edge_smear`](../reference/channel.md#edge_smear) with `fraction` 0.12 and `tau_samples` 3.0;
- a timing offset of 0.15 samples.

A fourth PAL pressing, D (`pal_pressing_D.yaml`), has pressing A's content, layout and channel, shared noise seeded 7004, and two kinds of dropout in every section:

- [pressing defects](../user-manual/impairments.md#pressing-defects-unflagged-and-disc-anchored-dropouts): `random` dropouts at scale 6 with `anchor: disc` and `flagged: false`, seeded 7004. They sit on the same pictures in every capture and in the replay, and no sidecar records them;
- capture dropouts: flagged `scratch` dropouts at scale 2, whose seed each capture replaces.

The `stacking-captures` variant derives six captures of every pressing. Each has its own capture seed and keeps the pressing's shared seed and disc-anchored dropout seeds. The capture seeds below are pressing A's; pressings B, C and D add 1000, 2000 and 3000 (pressing B's `_cap1` uses 6101), so captures of different pressings never share capture noise. Captures 1–4 also keep the pressing's channel; capture 5 adds a [chroma error](../reference/chroma-error.md) (a different player) and capture 6 replaces the timing offset with a whole-sample one:

| Capture | Capture seed (pressing A) | Timeline | Overrides |
|---------|---------------------------|----------|-----------|
| `_cap1` | 5101 | PNs 1–16 | — |
| `_cap2` | 5202 | Starts later, at PN 5 | — |
| `_cap3` | 5303 | Replays the multi-burst range: PNs 1–12, 9–12, 13–16 | — |
| `_cap4` | 5404 | PNs 1–16 | — |
| `_cap5` | 5505 | PNs 1–16 | `chroma_error`: gain 1.1, phase 3.0° |
| `_cap6` | 5606 | PNs 1–16 | `channel.timing_offset_samples`: +2.0 |

A `captures` entry in `projects/variants.json` may set `chroma_error` (a mapping that replaces the project's block, or `null` to remove it) and `timing_offset_samples` (written to `channel.timing_offset_samples`), alongside `capture_seed`, `remove_sections` and `repeat_sections`. The variant's `capture_seed_offsets` maps source filename globs to an integer added to the capture seeds of every matching source.

`stacking-clean` derives an impairment-free truth for every capture (`_capN_clean`). It removes the noise and the dropouts, pressing defects included, but keeps the `channel:` block, because the timing offset, the beat and the smear are part of the pressing. The captures of one pressing then match each other on every picture number, apart from capture noise and the overrides of captures 5 and 6. Pressings A and B's truths differ by exactly one sample.

### Disc-simulation sets

The `<standard>_discsim_A`–`_D` projects are four captures of one 40-frame CAV disc, each with its own start, skips and seeds. Their dropouts are disc-anchored (`anchor: disc`), so where a capture replays a range (PNs 17–20 in source A), the replay carries exactly the first play's dropouts, as a real disc would.

### Band-limited luma

`pal_bandlimited.yaml`, `ntsc_bandlimited.yaml` and `pal_m_bandlimited.yaml` are 16-frame CAV discs that play the multi-burst at full bandwidth, then at two [`luma_bandwidth`](../reference/sections.md#luma_bandwidth) cutoffs (5.0 and 3.0 MHz on PAL, 4.2 and 3.0 MHz on NTSC and PAL-M), and finish with a sharp-edged picture at the lower cutoff. Nothing else is impaired, so the luma low-pass is the only difference between sections.

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
| `scripts/generate_test_projects.py` | Derive variant projects from `projects/` (run automatically by the build), including the per-capture seeds and timelines of the pressing sets |
| `scripts/generate_fm_beat_reference.py` | Rewrite the FM-beat golden vectors (`tests/unit/fm_beat_reference_vectors.h`) from the reference beat model; needs numpy and scipy, and is not part of the build |
| `scripts/compare_frames.py` | Bit-exact comparison of a picture number across stacking sources |
| `scripts/compare_frames_detail.py` | As above, split by VBI versus active video |
| `scripts/convert_progressive_raw_to_exr.py` | Convert raw `yuv422p10le` stills to OpenEXR fixtures |
| `scripts/run_stage1_raw_to_exr_conversion.sh` | Run the EXR conversion in an ad-hoc Nix shell |
| `scripts/render-icons.sh` | Render the application icon PNG set from the logo SVG |
| `scripts/prepare-flatpak-manifest.py` | Write a copy of the Flatpak manifest with the release version and date filled in |
