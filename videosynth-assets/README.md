# videosynth-assets

Video and still-frame test assets for the `videosynth` project.

This directory contains reference media in EXR and MKV formats, plus verification tools to check BT.601 compliance. It was previously the separate `videosynth-assets` submodule and is now vendored directly into the main repository; it is the directory the `{bundled}` logical asset root resolves to in a development tree.

## Layout

- `assets/exr/`: EXR still-image test patterns organized by resolution.
- `assets/mkv/`: MKV video assets organized by resolution.
- `docs/`: Format and compliance requirements.
- `scripts/`: Verification scripts and convenience runners.

## Documentation

- [EXR BT.601 Compliance Requirements](docs/exr-bt601-compliance-requirements.md)
- [MKV BT.601 Compliance Requirements](docs/mkv-bt601-compliance-requirements.md)

## Verification Scripts

- [Verify EXR assets (Python)](scripts/verify_bt601_exr.py)
- [Verify MKV assets (Python)](scripts/verify_bt601_mkv.py)
- [Run EXR verification (shell)](scripts/run_verify_bt601_exr.sh)
- [Run MKV verification (shell)](scripts/run_verify_bt601_mkv.sh)

## Generation Scripts

- [Generate BLACK.exr for both rasters (shell)](scripts/generate_black_exr.sh) — writes `assets/exr/<raster>/BLACK.exr` with every pixel `R=G=B=0.0` (BT.601 studio black). Runs in an ad-hoc Nix shell with `ffmpeg` and `openexr`.
- [Generate MOVING_BARS_75.mkv for both rasters (shell)](scripts/generate_moving_bars_mkv.sh) — writes `assets/mkv/<raster>/MOVING_BARS_75.mkv`: 75% colour bars in the 704-sample active image, scrolling left 4 pixels per frame for 176 frames (one seamless loop), so the picture chroma moves. Runs in an ad-hoc Nix shell with `ffmpeg`.

## Quick Start

Use the shell wrappers to validate assets:

```bash
./scripts/run_verify_bt601_exr.sh
./scripts/run_verify_bt601_mkv.sh
```

See the linked compliance documents for expected colorimetry, levels, and metadata requirements.