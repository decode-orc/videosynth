# The project file

A videosynth project is a single YAML file. This reference documents it block by block; each page covers one block, its keys, their types and ranges, and the rules the validator applies.

## Top-level structure

There are exactly nine top-level blocks. Any other top-level key is a parse error.

```yaml
project:          # Identity — name, version, description
cvbs_presets:     # Signal format — standard, encoding, signal state
output:           # Where the media goes and in what form
line_injections:  # Project-wide VITS set
laserdisc:        # LaserDisc only — disc format, pilot/VBI burst, EFM audio, LaserVision channel
channel:          # Pressing impairments — timing offset, edge smear
chroma_error:     # Capture chroma error — gain, phase, jitter, differential, crosstalk
line_timing:      # Per-line timing error after time-base correction — capture and pressing
sections:         # The ordered timeline
```

Everything specific to LaserDisc is in the `laserdisc:` block, and in the `laserdisc:` block of each section that belongs to the disc. A project that is not a disc never needs either.

| Block | Required | Page |
|-------|----------|------|
| `project` | No | [project](project.md) |
| `cvbs_presets` | Yes | [cvbs_presets](cvbs-presets.md) |
| `output` | Yes | [output](output.md) |
| `line_injections` | No | [line_injections](line-injections.md) |
| `laserdisc` | No | [laserdisc](laserdisc.md), with its [LaserVision channel](laserdisc-channel.md) |
| `channel` | No | [channel](channel.md) |
| `chroma_error` | No | [chroma_error](chroma-error.md) |
| `line_timing` | No | [line_timing](line-timing.md) |
| `sections` | Yes | [sections](sections.md) |

## Section blocks

Each entry in `sections:` may carry these optional keys, listed in [canonical order](#canonical-key-order):

| Sub-block | Purpose | Page |
|-----------|---------|------|
| `cadence` | Film pulldown cadence | [sections](sections.md#cadence) |
| `laserdisc` | Disc area and biphase codes for this section | [section laserdisc](section-laserdisc.md) |
| `line_injections` | Section line injections (`vitc`, not implemented) | [section laserdisc](section-laserdisc.md#vitc-line-injections) |
| `horizontal_resample` | Source-pixel to 4fsc-sample mapping | [sections](sections.md#horizontal_resample) |
| `chroma_line_sign` | Per-region PAL chroma line sign | [sections](sections.md#chroma_line_sign) |
| `luma_bandwidth` | Low-pass on the picture luma | [sections](sections.md#luma_bandwidth) |
| `picture_response` | Camera-chain softening of the picture luma | [sections](sections.md#picture_response) |
| `osd` | On-screen display overlays | [osd](section-osd.md) |
| `noise` | Gaussian noise injection | [noise](section-noise.md) |
| `dropouts` | Random and scratch dropouts | [dropouts](section-dropouts.md) |
| `audio` | Audio channel pairs | [audio](section-audio.md) |

## Value catalogues

| Reference | Contents |
|-----------|----------|
| [Laserdisc code types](laserdisc-codes.md) | Every `code_type`, its fields and constraints |
| [VITS types](vits-types.md) | Every `vits_type` per standard |
| [Asset roots](asset-roots.md) | `{bundled}`, `{user}`, `{project}`, `{output}` and path resolution |
| [CLI options](cli-options.md) | Command-line reference |

## A minimal project

The smallest project that generates anything:

```yaml
cvbs_presets:
  video_standard_preset: PAL
  sample_encoding_preset: CVBS_U10_4FSC
  signal_state_preset: STANDARD_STABLE_LOCKED

output:
  video_path: "{output}/out.cvbs"

sections:
  - name: Bars
    type: progressive
    source: "{bundled}/exr/720x576/100_BARS.exr"
    duration_frames: 25
```

## Canonical key order

videosynth can write projects as well as read them, and the emitter uses a fixed key order. A project saved by the GUI comes back in this shape, which keeps files diffable. The order follows the signal: the format, outputs and signal content come before the pressing and capture impairments. Within a section the order is the source and its timing, the disc codes, the picture processing (horizontal mapping, chroma line sign, luma limit), the OSD burn-in, the impairments, and then audio. The GUI's project and section editors show their groups in the same order.

Hand-written projects need not follow it — order is not significant to the parser — but matching it avoids gratuitous diffs the first time a file passes through the GUI.

```yaml
project:
  name:
  version:
  description:

cvbs_presets:
  video_standard_preset:
  sample_encoding_preset:
  signal_state_preset:
  ntsc_black_setup_ire:
  secam_field_identification:

output:
  video_path:
  signal_type:
  truth:

line_injections:
  placement:
  vits:
    - vits_type:
      target_lines:

laserdisc:
  disc_type:
  pilot_burst:
  vbi_burst:
  efm_audio:
    pair:
  channel:
    fm_beat:
      carrier_black_hz:
      level_db:
    group_delay:
      preset:            # or points_ns:
      scale:
    response_db:         # or response_by_picture:
    response_by_picture:
      - picture:
        response_db:
    field2_offset:
      x_samples:
      y_lines:

channel:
  timing_offset_samples:
  edge_smear:
    fraction:
    tau_samples:

chroma_error:
  gain:
  phase_deg:
  line_jitter:
    phase_deg:
    gain:
    seed:
  differential:
    gain:
    phase_deg:
  crosstalk:
    v_to_u:
    u_to_v:
    alternate_with_switch:
  include_burst:

line_timing:
  line_ns:
  line_correlation:
  field_ns:
  slope_ns:
  seed:
  include_burst:
  shared:
    line_ns:
    line_correlation:
    field_ns:
    slope_ns:
    seed:

sections:
  - name:
    type:
    source:
    duration_frames:
    duration_repeat:
    start_frame:
    end_frame:
    cadence:
      pattern:
      phase:
      continuity:
    laserdisc:
      area:
      codes:
        - code_type:
          start_value:
          chapter:
          programme_status:
          users_code:
    line_injections:
      - type:
        target_lines:
    horizontal_resample:
    chroma_line_sign:
      - region:
          x:
          y:
          width:
          height:
        u:
        v:
    luma_bandwidth:
      cutoff_mhz:
      transition_mhz:
    osd:
      overlays:
        - text:
          x:
          y:
          scale:
          fg_luma:
          bg_luma:
    noise:
      noise_db:
      noise_spread_db:
      noise_seed:
      spectrum:
      spread_spectrum:
      shared:
        noise_db:
        noise_seed:
        spectrum:
        level_term:
          noise_db:
          level_db:
          spectrum:
    dropouts:
      random:
        scale:
        seed:
        flagged:
        anchor:
      scratch:
        scale:
        seed:
        flagged:
        anchor:
    audio:
      channel_pairs:
        - pair:
          description:
          left:
            waveform:
            frequency:
            amplitude:
            ramp:
              start:
              end:
              mode:
              period:
          right:
          file:
            path:
            repeat:
          video_audio:
            stream:
```

A channel pair carries only one of `left`/`right`, `file` or `video_audio`.

Only explicitly-set optional blocks are emitted, so a saved file stays minimal. Emit → parse is lossless: a saved project parses back to an identical project (the one exception is a `duration_repeat` alongside an integer `duration_frames`, which has no effect and is not written), which is the contract that keeps GUI-saved projects loadable by the CLI and vice versa.

## Unknown keys are errors

Every block validates its key set. An unrecognised key is reported by name, along with the block it appeared in:

```text
section contains unsupported field: 'duration_frame'.
cvbs_presets contains unsupported field: 'field_order'.
```

This is deliberate. A silently ignored typo in a signal-generation tool produces a plausible-looking file that is quietly wrong, which is worse than a failure.
