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

`true` writes the picture truth: the component picture that went into the chroma modulator, for scoring a decoder against the source picture rather than against its own Y/C separation. PAL, NTSC and PAL-M record Y, U and V; SECAM records Y, D′R and D′B (see [SECAM planes](#secam-planes)).

The truth is taken after every step applied before modulation, and before any impairment:

| Included | Excluded |
|----------|----------|
| The source-to-4fsc [horizontal mapping](sections.md#horizontal_mapping) and [resampling](sections.md#horizontal_resample) | [`channel`](channel.md) (timing offset, edge smear) |
| The section's [`luma_bandwidth`](sections.md#luma_bandwidth) limit | The [LaserVision channel](laserdisc-channel.md) (FM beat, group delay, replay response) |
| The chroma low-pass (33-tap Hamming-windowed 1.3 MHz for PAL and PAL-M, 17-tap 1.2 MHz for NTSC, 33-tap 1.7 MHz for SECAM per ITU-R BT.1700 Annex 1 Part C Table 4 item 10c) | [`chroma_error`](chroma-error.md) and [`line_timing`](line-timing.md) |
| [`chroma_line_sign`](sections.md#chroma_line_sign) regions | [Noise](section-noise.md) and [dropouts](section-dropouts.md) |
| [`laserdisc.channel.field2_offset`](laserdisc-channel.md#field2_offset), which changes the picture that is modulated | [OSD](section-osd.md) text (drawn afterwards; a warning) |
| The section's [`picture_response`](sections.md#picture_response) (horizontal and vertical) | SECAM modulation: video precorrection, FM and its deviation limits, the bell filter, the subcarrier phase pattern, and the line and field identification signals |

### `.truth`

Raw little-endian `float32`, frame after frame in output order. Each frame is three planes — Y, then U, then V (SECAM: Y, then D′R, then D′B) — and each plane is `rows` × `samples_per_line` values in row-major order:

| Standard | `samples_per_line` | `rows` | `first_sample` | Field 1 / field 2 first line | Even rows from |
|----------|--------------------|--------|----------------|------------------------------|----------------|
| PAL | 922 | 576 | 177 | 23 / 336 | Field 1 |
| SECAM | 922 | 576 | 177 | 23 / 336 | Field 1 |
| NTSC | 745 | 480 | 150 | 22 / 284 | Field 2 |
| PAL-M | 745 | 480 | 150 | 22 / 284 | Field 2 |

- A row covers the 4fsc active window of one active line. Sample *j* of a row is the picture at `first_sample` + *j* + 0.5 sample periods after 0H (+ 0.633 on NTSC), so a row starts `first_sample` samples into the line counted from the line's first sample. The truth is 0H-relative: on PAL the stored line carries the same sample *j* at `first_sample` + *j* + *d*ₖ samples from its first sample, where *d*ₖ is the line's lattice delay (SECAM is stored on the same PAL lattice; see [Output files](../user-manual/outputs.md#sample-files)), so compare a decoder's 0H-aligned output with the truth directly, not the raw stored samples. Rows are in progressive order, as the source picture's rows are drawn: the field whose picture opens higher on the screen supplies the even rows.
- **Y** is in mV above blanking (NTSC and PAL-M include the setup, so black is at `black_mv`).
- **U** and **V** are the low-pass-filtered colour-difference components, in peak mV of subcarrier, exactly as modulated: U on the sin carrier axis and V on the cos axis (NTSC: B−Y and R−Y). V has the PAL V-switch removed, as a PAL decoder recovers it, so a [`chroma_line_sign`](sections.md#chroma_line_sign) `v: follow_switch` region appears as V alternating line by line.
- Over the active window the modulated chroma is C = U · sin φ + s · V · cos φ, where φ is the subcarrier phase and s the line's V-switch sign (+1 throughout NTSC).
- Samples past the end of the PAL and SECAM half-picture line 623 carry no picture and are zero.

### SECAM planes

SECAM transmits its colour difference as frequency-modulated subcarriers, one component per line, so it has no U/V quadrature components. Its truth records the colour difference before modulation instead:

- **Y** is exactly as for PAL, in mV above blanking. For the same project rendered as PAL, the Y planes are identical.
- **D′R** and **D′B** are D′R = −1.902 (R′ − Y′) and D′B = 1.505 (B′ − Y′) (ITU-R BT.1700 Annex 1 Part C Table 4 item 9), taken after the colour-difference low-pass (item 10c) and before the video precorrection (item 10f). They are dimensionless: 1.0 is the nominal deviation of item 10e, 280 kHz on D′R and 230 kHz on D′B, at low frequency.
- Both are given on **every row**, whichever one the row's line transmits, so a scorer can measure both the transmitted lines and the decoder's reconstruction of the missing component from its neighbours.
- They are taken before the frequency modulator, so the deviation limits (+350/−506 kHz on D′R, +506/−350 kHz on D′B) never clip them.
- **Line rule.** In each frame, the lines whose frame line number has one parity transmit D′R and the others D′B; the parity swaps every frame (BT.1700 Part C Figure 16: line 7 of the first frame of the sequence carries D′R). The `secam_frame` table of the sidecar gives the parity for every frame.
- Re-encoding a transmitted line's component through the precorrection, FM, bell filter and subcarrier gate reproduces the generated chroma over the active window.

### `.truth.meta`

A SQLite database, like the other sidecars, with `PRAGMA user_version = 2`, one row in a `truth_file` table and, for SECAM, one row per frame in a `secam_frame` table. Columns that do not apply to the project's standard are `NULL`.

`truth_file`:

| Column | Type | Contents |
|--------|------|----------|
| `truth_file_id` | INTEGER | Always `1` |
| `video_standard` | TEXT | `PAL`, `NTSC`, `PAL_M` or `SECAM` |
| `sample_rate_hz` | REAL | The 4fsc sampling rate |
| `frame_count` | INTEGER | Frames in the truth file |
| `samples_per_line`, `rows` | INTEGER | Plane dimensions |
| `first_sample` | INTEGER | Offset of a row's first sample from 0H |
| `first_line_field1`, `first_line_field2` | INTEGER | Frame line carrying each field's first row |
| `upper_field` | INTEGER | Field (1 or 2) supplying the even rows |
| `half_picture_line`, `half_picture_end_sample` | INTEGER | The PAL half-picture line (623) and the sample offset where its picture ends; `0` when every line is full |
| `plane_order` | TEXT | `YUV`, or `YDRDB` for SECAM |
| `sample_format` | TEXT | `float32le` |
| `units` | TEXT | `mV`: the unit of Y, and of U and V |
| `y_reference` | TEXT | `blanking` |
| `black_mv`, `white_mv` | REAL | Black and white levels above blanking, for normalising Y |
| `u_axis`, `v_axis` | TEXT | `sin`, `cos`: the carrier axes U and V modulate (`NULL` for SECAM) |
| `v_switch` | TEXT | `removed`: V is given as a PAL decoder recovers it (`NULL` for SECAM) |
| `colour_difference_units` | TEXT | SECAM: `nominal_deviation`, the D′R/D′B scale above |
| `colour_difference_clip` | TEXT | SECAM: `unclipped`, as D′R/D′B are taken before the deviation limits |
| `dr_deviation_hz`, `db_deviation_hz` | REAL | SECAM: `280000` and `230000`, the deviation in Hz of 1.0 on each plane |
| `dr_per_r_minus_y`, `db_per_b_minus_y` | REAL | SECAM: `-1.902` and `1.505`, for converting back to R′ − Y′ and B′ − Y′ |

`secam_frame` (SECAM only):

| Column | Type | Contents |
|--------|------|----------|
| `frame_index` | INTEGER | Frame of the truth file, from 0 |
| `truth_file_id` | INTEGER | Always `1` |
| `dr_line_parity` | INTEGER | `1` when the frame's odd-numbered lines transmit D′R, `0` when its even-numbered lines do |

Validation: on a project with OSD overlays `truth: true` is a warning, as the truth does not carry the text. A value other than `true` or `false` is a parse error. The emitter writes the key only when it is `true`.
