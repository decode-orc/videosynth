# `output`

Where the generated media goes and in what form. Required.

```yaml
output:
  video_path: "{output}/pal_disc.cvbs"
  signal_type: composite
  truth: true
```

## Keys

| Key | Type | Required | Values | Default |
|-----|------|----------|--------|---------|
| `video_path` | string | Yes | A path, optionally with an [asset root token](asset-roots.md) | — |
| `signal_type` | string | No | `composite`, `yc` | `composite` |
| `truth` | bool | No | `true`, `false` | `false` |

No other key is accepted. In particular the metadata sidecar path is **not** configurable — it is derived from `video_path` and always sits beside it. LaserDisc EFM digital audio is selected in the [`laserdisc`](laserdisc.md#efm_audio) block; its files are written here, beside the video.

## `video_path`

The path of the primary sample file. Every other artefact is colocated with it and named from it.

| `signal_type` | Required suffix | Files written |
|---------------|-----------------|---------------|
| `composite` | `.cvbs` by convention | `<name>.cvbs` |
| `yc` | `.cvbsy` (enforced) | `<name>.cvbsy` and `<name>.cvbsc` |

With `signal_type: yc` the path **must** end in `.cvbsy`; the chroma path is derived by replacing that suffix with `.cvbsc`.

The derived paths:

| Artefact | Derivation |
|----------|------------|
| `.meta` | `video_path` with `.cvbs`/`.cvbsy` replaced by `.meta` (any other extension is replaced too; with none, `.meta` is appended) |
| `.dropouts.meta` | The metadata path with `.meta` replaced by `.dropouts.meta` |
| `_audio_<pair>.wav` | `video_path` with `.cvbs`/`.cvbsy` stripped, plus `_audio_<pair>.wav` |
| `.efm`, `.efm.meta` | `video_path` with `.cvbs`/`.cvbsy` stripped, plus the suffix |
| `.truth`, `.truth.meta` | `video_path` with `.cvbs`/`.cvbsy` stripped, plus the suffix |

Parent directories are created automatically.

!!! tip "Use `{output}`"
    Write `"{output}/name.cvbs"` rather than a bare relative path. `{output}` defaults to the project file's own directory, so a project stays self-contained when run by hand, while `--output-root` lets a batch run redirect every project at once. A bare relative path resolves against the working directory in the CLI but against the project directory in the GUI.

## `signal_type`

- **`composite`** — luma and chroma summed into a single signal.
- **`yc`** — luma and chroma written separately.

Luma and chroma are generated independently regardless; this only selects whether they are combined before writing.

## `truth`

`true` writes the picture truth: the component Y, U and V that went into the composite modulator, for scoring a decoder against the source picture rather than against its own Y/C separation. PAL, NTSC and PAL-M only.

The truth is taken after every step applied before modulation, and before any impairment:

| Included | Excluded |
|----------|----------|
| The source-to-4fsc [horizontal mapping](sections.md#horizontal_mapping) and [resampling](sections.md#horizontal_resample) | [`channel`](channel.md) (timing offset, edge smear) |
| The section's [`luma_bandwidth`](sections.md#luma_bandwidth) limit | The [LaserVision channel](laserdisc-channel.md) (FM beat, group delay, replay response) |
| The chroma low-pass (33-tap Hamming-windowed 1.3 MHz for PAL and PAL-M, 17-tap 1.2 MHz for NTSC) | [`chroma_error`](chroma-error.md) and [`line_timing`](line-timing.md) |
| [`chroma_line_sign`](sections.md#chroma_line_sign) regions | [Noise](section-noise.md) and [dropouts](section-dropouts.md) |
| [`laserdisc.channel.field2_offset`](laserdisc-channel.md#field2_offset), which changes the picture that is modulated | [OSD](section-osd.md) text (drawn afterwards; a warning) |
| The section's [`picture_response`](sections.md#picture_response) (horizontal and vertical) | |

### `.truth`

Raw little-endian `float32`, frame after frame in output order. Each frame is three planes — Y, then U, then V — and each plane is `rows` × `samples_per_line` values in row-major order:

| Standard | `samples_per_line` | `rows` | `first_sample` | Field 1 / field 2 first line | Even rows from |
|----------|--------------------|--------|----------------|------------------------------|----------------|
| PAL | 922 | 576 | 177 | 23 / 336 | Field 1 |
| NTSC | 745 | 480 | 150 | 22 / 284 | Field 2 |
| PAL-M | 745 | 480 | 150 | 22 / 284 | Field 2 |

- A row covers the 4fsc active window of one active line, starting `first_sample` samples after 0H. Rows are in progressive order, as the source picture's rows are drawn: the field whose picture opens higher on the screen supplies the even rows.
- **Y** is in mV above blanking (NTSC and PAL-M include the setup, so black is at `black_mv`).
- **U** and **V** are the low-pass-filtered colour-difference components, in peak mV of subcarrier, exactly as modulated: U on the sin carrier axis and V on the cos axis (NTSC: B−Y and R−Y). V has the PAL V-switch removed, as a PAL decoder recovers it, so a [`chroma_line_sign`](sections.md#chroma_line_sign) `v: follow_switch` region appears as V alternating line by line.
- Over the active window the modulated chroma is C = U · sin φ + s · V · cos φ, where φ is the subcarrier phase and s the line's V-switch sign (+1 throughout NTSC).
- Samples past the end of the PAL half-picture line 623 carry no picture and are zero.

### `.truth.meta`

A SQLite database, like the other sidecars, with `PRAGMA user_version = 1` and one row in a `truth_file` table:

| Column | Type | Contents |
|--------|------|----------|
| `truth_file_id` | INTEGER | Always `1` |
| `video_standard` | TEXT | `PAL`, `NTSC` or `PAL_M` |
| `sample_rate_hz` | REAL | The 4fsc sampling rate |
| `frame_count` | INTEGER | Frames in the truth file |
| `samples_per_line`, `rows` | INTEGER | Plane dimensions |
| `first_sample` | INTEGER | Offset of a row's first sample from 0H |
| `first_line_field1`, `first_line_field2` | INTEGER | Frame line carrying each field's first row |
| `upper_field` | INTEGER | Field (1 or 2) supplying the even rows |
| `half_picture_line`, `half_picture_end_sample` | INTEGER | The PAL half-picture line (623) and the sample offset where its picture ends; `0` when every line is full |
| `plane_order` | TEXT | `YUV` |
| `sample_format` | TEXT | `float32le` |
| `units` | TEXT | `mV` |
| `y_reference` | TEXT | `blanking` |
| `black_mv`, `white_mv` | REAL | Black and white levels above blanking, for normalising Y |
| `u_axis`, `v_axis` | TEXT | `sin`, `cos`: the carrier axes U and V modulate |
| `v_switch` | TEXT | `removed`: V is given as a PAL decoder recovers it |

Validation: `truth: true` on a SECAM project is an error. On a project with OSD overlays it is a warning, as the truth does not carry the text. A value other than `true` or `false` is a parse error. The emitter writes the key only when it is `true`.
