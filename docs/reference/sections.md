# `sections`

The ordered timeline. Required, and must contain at least one entry. Sections play back to back in declaration order, and their frame counts sum to the length of the output.

```yaml
sections:
  - name: Programme
    type: progressive
    source: "{bundled}/exr/720x576/100_BARS.exr"
    duration_frames: 250
    section_type: programme_area
    start_frame: 0
    line_injections: []
    noise: {}
    dropouts: {}
    osd: {}
    audio: {}
```

## Keys

| Key | Type | Required | Values | Default |
|-----|------|----------|--------|---------|
| `name` | string | Yes | — | — |
| `type` | string | Yes | `progressive` | — |
| `source` | string | Yes | Path, optionally with an [asset root token](asset-roots.md) | — |
| `duration_frames` | int or string | No | Positive integer, or `"all"` | — |
| `duration_repeat` | int | No | Positive integer | `1` |
| `section_type` | string | No | `lead_in`, `programme_area`, `lead_out` | absent |
| `start_frame` | int | No | ≥ 0 | `0` |
| `line_injections` | list | No | [section line injections](section-line-injections.md) | absent |
| `noise` | map | No | [noise](section-noise.md) | absent |
| `dropouts` | map | No | [dropouts](section-dropouts.md) | absent |
| `osd` | map | No | [osd](section-osd.md) | absent |
| `audio` | map | No | [audio](section-audio.md) | absent |

No other key is accepted.

## `name`

Identifies the section in the GUI list, in log messages, in validation errors, and in the `{section_name}` OSD token. Names need not be unique, but making them so is considerably easier to work with.

## `type`

Must be `progressive`. This is the only implemented section type: a progressive source file converted to an interlaced signal.

## `source`

The picture content. Must resolve to an existing file that matches a supported progressive profile:

- **EXR** (`.exr`) — single-frame scanline OpenEXR, `R/G/B` `FLOAT`, uncompressed, full-raster windows, standard-matching frame-rate and pixel-aspect metadata.
- **Video** (`.mkv` Matroska or `.mp4` MP4) — any FFmpeg-decodable codec at a constant frame rate of 25 (PAL/SECAM) or 30000/1001 (NTSC/PAL-M). Colour range, matrix, chroma format and bit depth are normalised to BT.601 10-bit 4:2:2 studio swing; field order and sample aspect mismatches are warnings.

The raster must be exactly 720×576 (PAL/SECAM) or 720×486 (NTSC/PAL-M). There is no scaling or frame-rate conversion; a source of any other size or rate is rejected. See [Picture sources](../user-manual/sources.md).

Use `{bundled}/…` for shipped assets and `{project}/…` for files beside the project. A bare relative path resolves against the working directory in the CLI but against the project directory in the GUI, so it will not behave the same in both.

## `duration_frames`

How many output frames the section produces.

- **A positive integer** — exactly that many. A still is repeated. A clip must hold at least `start_frame` + the count frames; looping a shorter clip is not implemented yet, and generation fails when it runs out.
- **`"all"`** — the source's own frame count, discovered by probing.

## `duration_repeat`

With `duration_frames: "all"`, replays the whole source this many times. Total output frames = source length × `duration_repeat`; each replay restarts the source at frame 0.

Ignored with a fixed integer `duration_frames`, and a warning is emitted if you set it there.

## `section_type`

Marks the section as part of a laserdisc structure.

| Value | Meaning | Minimum duration |
|-------|---------|------------------|
| `lead_in` | Before the programme content | 938 frames (CAV) |
| `programme_area` | The content itself | — |
| `lead_out` | After the programme content | 1250 frames (CAV) |

Omitting it means the section is not part of a disc structure.

### Ordering rules

Once **any** section declares a `section_type`, the whole sequence is checked:

- At most one `lead_in`, and no section may precede it.
- At most one `lead_out`, and no section may follow it.
- Every section between them must be `programme_area`.

Violations are errors, not warnings: out-of-order sections would break monotonic picture-number and time-code generation (IEC 60856/60857).

`section_type` also drives the [EFM](../user-manual/audio.md#laserdisc-efm-digital-audio) disc-area mapping — a lead-in carries the table of contents, each programme-area section is one EFM track, and the lead-out carries lead-out subcode. That mapping works whether or not the section carries biphase codes, which is why an EFM-only project can have a short lead-in where a biphase CAV lead-in would need 938 frames.

## `start_frame`

The first source frame to use, 0-based. Useful for taking a passage from the middle of a clip with a fixed `duration_frames`. Combining it with `duration_frames: "all"` is not supported yet: the section would run past the end of the source and generation fails.

## Section sub-blocks

Each has its own reference page:

| Block | Purpose |
|-------|---------|
| [`line_injections`](section-line-injections.md) | Laserdisc biphase codes |
| [`noise`](section-noise.md) | Gaussian noise injection |
| [`dropouts`](section-dropouts.md) | Random and scratch dropouts |
| [`osd`](section-osd.md) | On-screen display overlays |
| [`audio`](section-audio.md) | Audio channel pairs |

All are optional, and only blocks that are explicitly set are written back out by the emitter.

## Validation summary

- `type` must be `progressive`.
- `source` must exist and match a supported profile, at the standard's raster and constant frame rate; a video's container must match its extension and carry no crop metadata.
- `duration_frames` must be a positive integer or `"all"`.
- The disc-structure ordering rules above.
- CAV lead-in and lead-out minimum durations.
