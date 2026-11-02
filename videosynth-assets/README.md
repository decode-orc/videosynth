# videosynth-assets

Video and still-frame test assets for the `videosynth` project.

This directory contains reference media in EXR and MKV formats, plus verification tools to check BT.601 compliance. It was previously the separate `videosynth-assets` submodule and is now vendored directly into the main repository; it is the directory the `{bundled}` logical asset root resolves to in a development tree.

## Layout

- `assets/exr/`: EXR still-image test patterns organized by resolution.
- `assets/mkv/`: MKV video assets organized by resolution.
- `docs/`: Format and compliance requirements.
- `scripts/`: Generation and verification scripts.
- `sources/`: Committed source data that generators render from (`sources/osm/`: the OpenStreetMap extract for the map stills, under the ODbL — see [sources/osm/LICENSE.md](sources/osm/LICENSE.md)).

## Documentation

- [EXR BT.601 Compliance Requirements](docs/exr-bt601-compliance-requirements.md)
- [MKV BT.601 Compliance Requirements](docs/mkv-bt601-compliance-requirements.md)

## Verification Scripts

- [Verify EXR assets (Python)](scripts/verify_bt601_exr.py)
- [Verify MKV assets (Python)](scripts/verify_bt601_mkv.py)
- [Run EXR verification (shell)](scripts/run_verify_bt601_exr.sh)
- [Run MKV verification (shell)](scripts/run_verify_bt601_mkv.sh)

## Generation Scripts

The still generators share [write_exr.sh](scripts/write_exr.sh), which wraps a planar float32 R′G′B′ frame as an EXR with the metadata required by the EXR compliance document.

- [Generate BLACK.exr for both rasters (shell)](scripts/generate_black_exr.sh) — writes `assets/exr/<raster>/BLACK.exr` with every pixel `R=G=B=0.0` (BT.601 studio black). Runs in an ad-hoc Nix shell with `ffmpeg` and `openexr`.
- [Generate the synthetic test patterns (shell)](scripts/generate_test_patterns.sh) — renders [generate_test_patterns.py](scripts/generate_test_patterns.py) and writes `assets/exr/720x576/PAL_STRESS.exr` (a PAL decoder-stress card: line-alternating ±(R−Y)/±(B−Y) bars, which a project makes alternate with `chroma_line_sign`, steady ±(R−Y), ±(B−Y) and ±(G−Y) fields, 0.8–4.8 MHz definition gratings, a red square on yellow and a 230 ns grid) and `LUMA_ZONE_PLATE.exr` / `CHROMA_ZONE_PLATE.exr` (static zone plates reaching the source Nyquist frequency) and `GREEN_MAGENTA_EDGES.exr` (row-to-row green/magenta edges at 100 % and 75 % and at equal luma, 1/2/4/8-row stripes, the vertical-edge counterpart, and red/cyan and blue/yellow 2-row controls) for both rasters. The patterns are defined in BT.601 component terms and converted to R′G′B′; runs in an ad-hoc Nix shell with `ffmpeg`, `openexr` and `python3`.
- [Generate the OpenStreetMap map stills (shell)](scripts/generate_osm_maps.sh) — renders [render_osm_map.py](scripts/render_osm_map.py) over the committed extract `sources/osm/bradford_on_avon.json` and writes `assets/exr/720x576/OSM_TOWN.exr` (Bradford-on-Avon town centre, 800 m across: hatched buildings, cased roads, dashed paths, walls, street names, 5 m contours, a 100 m grid) and `OSM_RURAL.exr` (the town and its countryside, 6 km across: land-cover fills, woodland dot fill, rivers and canal, main roads, place names, 10 m contours, a 1 km grid). The style follows 1980s Ordnance Survey maps (pale fills, blue water and water lettering, green woodland, red/orange/yellow cased roads, brown contours, black lettering and buildings, a thin blue grid), with every colour inside R′G′B′ [0, 1]. Drawn aliased at 4× in each direction and box-reduced in float, so lines and lettering land at sub-pixel positions without aliasing or further softening. The contours come from an analytic height field (rising away from the waterways, plus smooth hills), not survey data. Lettering uses the Liberation fonts (SIL OFL 1.1). Runs in an ad-hoc Nix shell with `ffmpeg`, `openexr`, `python3` (numpy, Pillow) and `liberation_ttf`.
- [Fetch the OpenStreetMap extract (shell)](scripts/fetch_osm_extract.sh) — refreshes `sources/osm/bradford_on_avon.json` with a single Overpass API query, trimmed to the tags the renderer reads. Only needed to refresh the data; the maps rebuild from the committed extract. No map tiles are fetched.
- [Generate the photograph stills (shell)](scripts/generate_photo_exrs.sh) — `generate_photo_exrs.sh <broken_butterfly.jpg> <galtstroms_bruk.jpg>` converts the photographer's camera JPEGs (not committed) with [convert_photo.py](scripts/convert_photo.py) into `assets/exr/<raster>/PHOTO_BUTTERFLY.exr` (skin tones and smooth gradients next to striped fabric and fine hair) and `PHOTO_GALTSTROMS.exr` (an aerial view of an ironworks: brickwork, stonework, railings and conifers next to saturated grass) for both rasters. Each is cropped to 4:3, downscaled in linear light with a non-negative triangle filter (no ringing or sharpening halos, no noise reduction) and re-encoded with the sRGB transfer as R′G′B′, keeping every value inside [0, 1]. Runs in an ad-hoc Nix shell with `ffmpeg`, `openexr` and `python3` (numpy, Pillow).
- [Generate TEXT_CARD.exr (shell)](scripts/generate_text_card.sh) — renders [generate_text_card.py](scripts/generate_text_card.py) and writes `assets/exr/720x576/TEXT_CARD.exr`: hard-edged, pixel-aligned text with glyphs 8, 16 and 24 rows tall in white, yellow and cyan on blue, black on white, red and green on black, yellow on red, and white and magenta on green, then the same text keyed over a strip of `PHOTO_GALTSTROMS.exr` (which must exist first). The bitmap font is drawn for the card and lives in the script. Runs in an ad-hoc Nix shell with `ffmpeg`, `openexr` and `python3`.
- [Generate MOVING_BARS_75.mkv for both rasters (shell)](scripts/generate_moving_bars_mkv.sh) — writes `assets/mkv/<raster>/MOVING_BARS_75.mkv`: 75% colour bars in the 704-sample active image, scrolling left 4 pixels per frame for 176 frames (one seamless loop), so the picture chroma moves. Runs in an ad-hoc Nix shell with `ffmpeg`.
- [Generate FILM_COUNTER_24P.mkv (shell)](scripts/generate_film_counter_mkv.sh) — writes `assets/mkv/720x486/FILM_COUNTER_24P.mkv`: 240 progressive film frames at 24000/1001 fps for 2:3 pulldown. Each frame shows its 0-based frame number in large digits, over a white bar and a 75% colour-bar strip that both move 8 pixels per frame, so pulldown's woven frames show combing. Runs in an ad-hoc Nix shell with `ffmpeg` and the DejaVu fonts.

## Licence Record

Every bundled asset, with its source, licence, required attribution and generator. "Repository licence" means the GPL-3.0-or-later licence of the videosynth repository. The rows marked *not recorded* predate this record; their origin and licence are still to be confirmed.

| Asset | Rasters | Source | Licence | Attribution | Generator |
|-------|---------|--------|---------|-------------|-----------|
| `exr/BLACK.exr` | both | Synthetic | Repository licence | — | [generate_black_exr.sh](scripts/generate_black_exr.sh) |
| `exr/PAL_STRESS.exr` | 576 | Synthetic; layout after the Philips PM5544 (Hendil 1970) and the public-domain (CC0) Wikimedia Commons recreation of it | Repository licence | — | [generate_test_patterns.sh](scripts/generate_test_patterns.sh) |
| `exr/LUMA_ZONE_PLATE.exr`, `exr/CHROMA_ZONE_PLATE.exr` | both | Synthetic | Repository licence | — | [generate_test_patterns.sh](scripts/generate_test_patterns.sh) |
| `exr/GREEN_MAGENTA_EDGES.exr` | both | Synthetic | Repository licence | — | [generate_test_patterns.sh](scripts/generate_test_patterns.sh) |
| `exr/OSM_TOWN.exr`, `exr/OSM_RURAL.exr` | 576 | Rendered from OpenStreetMap data ([sources/osm/](sources/osm/LICENSE.md)); Liberation fonts (SIL OFL 1.1) for the lettering | Image: repository licence, as an ODbL Produced Work (not share-alike). Data extract: ODbL 1.0 | "© OpenStreetMap contributors. Data available under the Open Database License (ODbL)." | [generate_osm_maps.sh](scripts/generate_osm_maps.sh) |
| `exr/PHOTO_BUTTERFLY.exr` | both | Photograph `broken_butterfly.jpg`, Canon EOS 70D, 2015, by Simon Inns | CC BY 4.0 | "Photo © 2015 Simon Inns, CC BY 4.0" | [generate_photo_exrs.sh](scripts/generate_photo_exrs.sh) |
| `exr/PHOTO_GALTSTROMS.exr` | both | Photograph `galtstroms_bruk.jpg`, GoPro HERO3+ Black, by Simon Inns | CC BY 4.0 | "Photo © Simon Inns, CC BY 4.0" | [generate_photo_exrs.sh](scripts/generate_photo_exrs.sh) |
| `exr/TEXT_CARD.exr` | 576 | Synthetic text (font drawn for the card) over a strip of `PHOTO_GALTSTROMS.exr` | CC BY 4.0 (contains the photograph) | "Photo © Simon Inns, CC BY 4.0" | [generate_text_card.sh](scripts/generate_text_card.sh) |
| `mkv/MOVING_BARS_75.mkv` | both | Synthetic | Repository licence | — | [generate_moving_bars_mkv.sh](scripts/generate_moving_bars_mkv.sh) |
| `mkv/FILM_COUNTER_24P.mkv` | 486 | Synthetic; DejaVu fonts for the digits | Repository licence | — | [generate_film_counter_mkv.sh](scripts/generate_film_counter_mkv.sh) |
| `exr/TESTCARD-F.exr` | 576 | *Not recorded* (UK Test Card F) | *Not recorded* | *Not recorded* | *Not recorded* |
| `exr/100_BARS.exr`, `exr/75_BARS.exr`, `exr/75_BARS_RED.exr`, `exr/SMPTE_BARS.exr` (576), `exr/SMPTE_BARS_001.exr` (486), `exr/PLUGE.exr`, `exr/MULTIBURST.exr`, `exr/TARTAN.exr` | both | *Not recorded* | *Not recorded* | *Not recorded* | *Not recorded* |
| `exr/LUMA_RAMP.exr`, `exr/LUMA_RAMP_DOWN.exr`, `exr/VERT_LUMA_RAMP.exr`, `exr/CHROMA_RAMP.exr`, `exr/FULL_RAMP.exr`, `exr/LEGAL_RAMP.exr`, `exr/VALID_RAMPS.exr`, `exr/Y_CB_CR_RAMPS.exr`, `exr/GREY_5H_STEP.exr`, `exr/GREY_10H_STEP.exr`, `exr/GREY_5V_STEP.exr`, `exr/GREY_10V_STEP.exr` | both | *Not recorded* | *Not recorded* | *Not recorded* | *Not recorded* |
| `mkv/MOVING_ZONE_2H.mkv`, `mkv/SMPTE_BARS.mkv` (576), `mkv/SMPTE_BARS_001.mkv` (486) | both | *Not recorded* | *Not recorded* | *Not recorded* | *Not recorded* |

The full-width stills (`PAL_STRESS`, the zone plates, `GREEN_MAGENTA_EDGES`, the maps, the photographs and `TEXT_CARD`) fill all 720 samples, since videosynth maps the 720 source pixels onto the 52 µs active line; `verify_bt601_exr.py` therefore reports its 8-column black-padding check as failed for them, and every other check passes.

## Quick Start

Use the shell wrappers to validate assets:

```bash
./scripts/run_verify_bt601_exr.sh
./scripts/run_verify_bt601_mkv.sh
```

See the linked compliance documents for expected colorimetry, levels, and metadata requirements.