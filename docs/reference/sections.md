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
| `name` | string | No | — | empty |
| `type` | string | Yes | `progressive` | — |
| `source` | string | Yes | Path, optionally with an [asset root token](asset-roots.md) | — |
| `duration_frames` | int or string | No | Positive integer, or `"all"` | `"all"` |
| `duration_repeat` | int | No | Positive integer | `1` |
| `section_type` | string | No | `lead_in`, `programme_area`, `lead_out` | absent |
| `start_frame` | int | No | ≥ 0, inside the video source | `0` |
| `end_frame` | int | No | ≥ `start_frame`, inside the video source | last source frame |
| `line_injections` | list | No | [section line injections](section-line-injections.md) | absent |
| `noise` | map | No | [noise](section-noise.md) | absent |
| `dropouts` | map | No | [dropouts](section-dropouts.md) | absent |
| `osd` | map | No | [osd](section-osd.md) | absent |
| `audio` | map | No | [audio](section-audio.md) | absent |

No other key is accepted.

## `name`

Identifies the section in the GUI list, in log messages, in validation errors, and in the `{section_name}` OSD token. It is not required, and names need not be unique, but a unique name for every section is considerably easier to work with.

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

- **A positive integer** — exactly that many. A still is repeated. A clip plays from `start_frame`; a count longer than the rest of the clip loops it, restarting at `start_frame` after the last frame.
- **`"all"`** — the selected range once: [`start_frame`, `end_frame`], or [`start_frame`, last frame] without an `end_frame`. The clip's length is discovered by probing.

Omitting `duration_frames` means `"all"`. The emitter always writes the key out, so a saved project shows `duration_frames: all`. An EXR still needs an integer: it has no length of its own.

## `duration_repeat`

With `duration_frames: "all"`, plays the selected range this many times. Total output frames = range length × `duration_repeat`; each replay restarts at `start_frame`.

Ignored with a fixed integer `duration_frames`, and a warning is emitted if you set it there. The emitter writes it only alongside `duration_frames: all`, so a saved project drops an ignored `duration_repeat`.

## `section_type`

Marks the section as part of a laserdisc structure.

| Value | Meaning | Recommended minimum (CAV) |
|-------|---------|---------------------------|
| `lead_in` | Before the programme content | 938 frames |
| `programme_area` | The content itself | — |
| `lead_out` | After the programme content | 1250 frames |

Omitting it means the section is not part of a disc structure. A section that carries a `laserdisc` [line injection](section-line-injections.md) must set it.

The minimums are checked as **warnings**, only on a CAV disc, only for a section that carries a `laserdisc` injection, and only when `duration_frames` is an integer:

```text
Laserdisc injection warning: CAV lead_in section has 500 frames; IEC 1.5 mm
minimum at 1.6 um track pitch requires 938.
```

### Ordering rules

Once any section is a `lead_in` or a `lead_out`, the whole sequence is checked (a project that uses only `programme_area` is not):

- At most one `lead_in`, and no section may precede it.
- At most one `lead_out`, and no section may follow it.
- Every section between them must be `programme_area`. With only a lead-in this applies to every section after it, and with only a lead-out to every section before it.

Violations are errors, not warnings: out-of-order sections would break monotonic picture-number and time-code generation (IEC 60856/60857).

`section_type` also drives the [EFM](../user-manual/audio.md#laserdisc-efm-digital-audio) disc-area mapping — a lead-in carries the table of contents, each programme-area section is one EFM track, and the lead-out carries lead-out subcode. That mapping works whether or not the section carries biphase codes, which is why an EFM-only project can have a short lead-in without the 938-frame warning a biphase CAV lead-in would get.

## `start_frame`

The first source frame of the selected range, 0-based. Video sources only; it must be below the clip's frame count.

## `end_frame`

The last source frame of the selected range, 0-based and inclusive. Video sources only; it must be at least `start_frame` and below the clip's frame count. Omit it to run to the end of the clip.

```yaml
- name: Excerpt
  type: progressive
  source: "{project}/capture.mp4"
  start_frame: 1500
  end_frame: 2999        # 1 500 frames; duration derived
```

`end_frame` sets the section's length, so it cannot be combined with an integer `duration_frames`; leave `duration_frames` out (or set it to `"all"`), and use `duration_repeat` to play the range more than once. Emitted only when set.

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
- `start_frame` ≥ 0 and `end_frame` ≥ `start_frame`, both inside the video source; `end_frame` never with an integer `duration_frames`.
- An EXR still takes neither `start_frame` nor `end_frame`, and needs an integer `duration_frames`.
- The disc-structure ordering rules above.
- A section with a `laserdisc` injection must set `section_type`.
- CAV lead-in and lead-out minimum durations (warnings).
