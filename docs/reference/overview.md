# The project file

A videosynth project is a single YAML file. This reference documents it block by block; each page covers one block, its keys, their types and ranges, and the rules the validator applies.

## Top-level structure

There are exactly seven top-level blocks. Any other top-level key is a parse error.

```yaml
project:          # Identity — name, version, description
cvbs_presets:     # Signal format — standard, encoding, signal state
output:           # Where the media goes and in what form
line_injections:  # Project-wide disc format and VITS set
channel:          # Pressing impairments — timing offset, edge smear, FM beat
chroma_error:     # Capture chroma gain and phase error
sections:         # The ordered timeline
```

| Block | Required | Page |
|-------|----------|------|
| `project` | No | [project](project.md) |
| `cvbs_presets` | Yes | [cvbs_presets](cvbs-presets.md) |
| `output` | Yes | [output](output.md) |
| `line_injections` | No | [line_injections](line-injections.md) |
| `channel` | No | [channel](channel.md) |
| `chroma_error` | No | [chroma_error](chroma-error.md) |
| `sections` | Yes | [sections](sections.md) |

## Section blocks

Each entry in `sections:` may carry these optional sub-blocks:

| Sub-block | Purpose | Page |
|-----------|---------|------|
| `line_injections` | Laserdisc biphase codes for this section | [section line_injections](section-line-injections.md) |
| `luma_bandwidth` | Low-pass on the picture luma | [sections](sections.md#luma_bandwidth) |
| `noise` | Gaussian noise injection | [noise](section-noise.md) |
| `dropouts` | Random and scratch dropouts | [dropouts](section-dropouts.md) |
| `osd` | On-screen display overlays | [osd](section-osd.md) |
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

videosynth can write projects as well as read them, and the emitter uses a fixed key order. A project saved by the GUI comes back in this shape, which keeps files diffable. Hand-written projects need not follow it — order is not significant to the parser — but matching it avoids gratuitous diffs the first time a file passes through the GUI.

```yaml
project:
  name:
  version:
  description:

cvbs_presets:
  video_standard_preset:
  sample_encoding_preset:
  signal_state_preset:
  pal_laserdisc_pilot_burst:
  ntsc_laserdisc_vbi_burst:
  ntsc_black_setup_ire:
  secam_field_identification:

output:
  video_path:
  signal_type:
  efm_audio:
    pair:

line_injections:
  disc_type:
  placement:
  vits:
    - vits_type:
      target_lines:

channel:
  timing_offset_samples:

chroma_error:
  gain:
  phase_deg:
  line_jitter:
    phase_deg:
    gain:
    seed:

sections:
  - name:
    type:
    source:
    duration_frames:
    duration_repeat:
    section_type:
    start_frame:
    end_frame:
    line_injections:
      - type:
        target_lines:
        codes:
          - code_type:
            start_value:
            chapter:
            programme_status:
            users_code:
    luma_bandwidth:
      cutoff_mhz:
      transition_mhz:
    noise:
      noise_db:
      noise_spread_db:
      noise_seed:
      spectrum:
      shared:
        noise_db:
        noise_seed:
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
    osd:
      overlays:
        - text:
          x:
          y:
          scale:
          fg_luma:
          bg_luma:
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
