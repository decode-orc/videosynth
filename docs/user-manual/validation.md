# Validation

Validation is what makes videosynth's strictness useful rather than annoying. A project either describes a signal the standards permit, or it is rejected with a message naming the rule it broke — no silently coerced values, no "close enough" behaviour.

## When it runs

| Where | When |
|-------|------|
| CLI | On every run, before generation; on demand with `--validate` |
| GUI | Continuously in the background, debounced; on demand via **Project → Validate** |

`--validate` parses, checks and probes the sources, then exits without writing anything. It is fast, and it is the right first move whenever you change a project.

## Errors and warnings

**Errors** block generation. They mean the project describes something the standards do not permit, or something the generator cannot do.

**Warnings** do not block generation. They mean the project is legal but probably not what you intended — a scratch longer than the section it lives in, an EFM pair no section declares, a `duration_repeat` on a fixed-length section.

In the GUI both appear in the **Issues** dock; double-clicking an issue selects the section it relates to, or opens **Edit Project…** for a project-level issue. The status bar summarises as *Project valid* or *n error(s), m warning(s)*.

## What gets checked

### Structure and schema

Unknown keys are rejected outright, at every level, naming the offending key and the block it is in. A misspelled `duration_frame` fails the project rather than being silently ignored.

### Presets

- One video standard, one sample encoding, one signal state per project.
- `sample_encoding_preset` must be one of the four 4fsc presets or `RAW_S16_28M` / `RAW_S16_40M`.
- `signal_state_preset` must be `STANDARD_STABLE_LOCKED`, or `STANDARD_STABLE_UNLOCKED` for `SECAM`.
- `SECAM` projects cannot use laserdisc line injections, `line_injections.disc_type` or laserdisc VITS placement; with VITS a warning notes that VITS chroma stays PAL-modulated. `secam_field_identification: false` on another standard is a warning.
- `pal_laserdisc_pilot_burst` is PAL only; `ntsc_laserdisc_vbi_burst` is NTSC only and, being unimplemented, is an error if enabled at all.
- `ntsc_black_setup_ire` is NTSC/PAL-M only and must be exactly `7.5` or `0.0`.
- `output.video_path` is required; with `signal_type: yc` it must end in `.cvbsy`.
- Output resolution is fixed by the standard and must not appear in the project.

### Sources

- The file must exist after [asset root resolution](../reference/asset-roots.md) and be an EXR still (`.exr`) or a video (`.mkv`, `.mp4`).
- An EXR must match the still profile — single-frame scanline, `R/G/B` `FLOAT`, uncompressed, with standard-matching metadata.
- A video must have a video stream, and its container must match its extension (Matroska for `.mkv`, MP4 for `.mp4`).
- Its raster must be exactly 720×576 (PAL/SECAM) or 720×486 (NTSC/PAL-M). No scaling path exists.
- Its frame rate must match the output standard and be constant — an average rate more than 0.2 % from the base rate is rejected as variable.
- A video must not carry stream crop metadata.
- **Warning:** a video field-order tag opposite to the output's dominance (PAL/SECAM top field first, NTSC/PAL-M bottom field first).
- **Warning:** a video sample aspect ratio other than the standard's BT.601 ratio (128:117 or 108:119); the picture is used unscaled.
- **Info (log only):** the colour conversions a video will receive, such as `full range -> studio swing` or `BT.709 matrix -> BT.601`. See [Picture sources](sources.md#video-clip-mkv-or-mp4).

### Sections and disc structure

- Every section needs a valid `type` (`progressive`) and a `source`.
- `duration_frames` must be a positive integer or `"all"` (an omitted `duration_frames` means `"all"`).
- `start_frame` must be ≥ 0 and `end_frame` ≥ `start_frame`; both must lie inside the video source.
- `end_frame` must not be combined with an integer `duration_frames`.
- An EXR still takes neither `start_frame` nor `end_frame`, and needs an integer `duration_frames`.
- Once any section is a `lead_in` or `lead_out`, the sequence must be `[lead_in] programme_area… [lead_out]` — at most one of each, in that order, with nothing outside them.
- *Warning*: on a CAV disc, a section carrying laserdisc codes that is a lead-in shorter than 938 frames or a lead-out shorter than 1250 frames.

### Line injections

- `target_lines` must be within the standard's range (1–625 PAL, 1–525 NTSC/PAL-M).
- `target_lines` must **not** be given for `laserdisc` injections.
- Injected lines must not overlap within a section, and VITS lines must not overlap within the project VITS set.
- When `line_injections.disc_type` is set, no VITS may target a line in the reserved address ranges (PAL 6–18 and 319–331; NTSC/PAL-M 10–18 and 273–281).
- `vitc` and `laserdisc` must not appear in the same section.
- VITS `target_lines` are constrained by the `placement` policy — see [VITS](vits.md).

### Laserdisc codes

- The code type must be valid for the disc format, and for the section type it appears in.
- A section declaring laserdisc codes needs `line_injections.disc_type` and a `section_type`.
- `picture_number` `start_value` must be 0–99,999 (PAL) or 0–79,999 (NTSC/PAL-M); `fm_picture_number` `start_value` 0–79,999. Only the start value is checked.
- Chapter numbers are 0–79.
- `users_code` must be a hex value of at most 24 bits, with X₁ 0–7 and the D digit (bits 15–12) equal to `D`.
- NTSC and PAL-M discs (any project with `disc_type` set) must carry a `virs` VITS entry.
- *Warning*: a CAV chapter shorter than 30 frames. Chapters that include a `duration_frames: all` section, and CLV chapters, are not checked.

### Channel

- `timing_offset_samples` in −8.0 to 8.0.
- `edge_smear.fraction` in 0.0–0.5; `edge_smear.tau_samples` above 0 and at most 100.
- `fm_beat:` on any standard but 625-line PAL is an error. `fm_beat.carrier_black_hz` in 6.9–7.3 MHz; `fm_beat.level_db` in −60.0 to 0.0.
- Values that are not finite are errors.
- A non-numeric value, an unknown key, an empty `channel:` block, an `edge_smear:` block without both `fraction` and `tau_samples`, or an `fm_beat:` block without `level_db` is a parse error.

### Chroma error

- `gain` in 0.5–1.5; `phase_deg` in −180.0 to 180.0; `line_jitter.phase_deg` in 0.0–10.0; `line_jitter.gain` in 0.0–0.1. Values that are not finite are errors.
- `chroma_error:` on a SECAM project is an error.
- A non-numeric value, a non-boolean `include_burst`, an unknown key, an empty `chroma_error:` block (one with only `include_burst` counts as empty) or an empty `line_jitter:` block is a parse error.

### Luma bandwidth

- `luma_bandwidth.transition_mhz` in 0.1–2.0; `luma_bandwidth.cutoff_mhz` from 0.5 MHz up to 2 f_sc less half the transition.
- `luma_bandwidth` without `cutoff_mhz`, a non-numeric value or an unknown key is a parse error.

### Noise and dropouts

- `noise_db` in 20.0–61.0; `noise_spread_db` ≥ 0; `noise_db − noise_spread_db` ≥ 20.0.
- `noise_spread_db`, `spectrum` or `spread_spectrum` without `noise_db` is an error.
- `spectrum` and `spread_spectrum` tables (capture and `shared`): at least one point, frequencies strictly ascending in 0 to the 4fsc Nyquist frequency (for every encoding preset), `relative_db` in −60.0 to +40.0.
- `shared`: `noise_db` is required and in 20.0–61.0; `noise_seed` is required; `noise_spread_db` is an error.
- `shared.level_term`: `noise_db` and `level_db` are required; `noise_db` in 20.0–61.0; `level_db` has at least one point, levels strictly ascending in 0.0–800.0 mV, `relative_db` in −60.0 to +20.0; its `spectrum` follows the spectrum rules above.
- Dropout `scale` values in 0–20, with at least one of them 1–20; a `dropouts:` block that disables everything is an error — omit the block instead.
- A dropout type with `anchor: disc` needs a `seed`. An `anchor` other than `section` or `disc`, or a non-boolean `flagged`, is a parse error.
- *Warning*: spread noise with no VITS on the White SNR measurement line.
- *Warning*: `spread_spectrum` with `noise_spread_db` 0 (no level-dependent term to shape).
- *Warning*: `noise.shared` on a CAV section without a `picture_number` code (pressing noise falls back to output position).
- *Warning*: scratch lifespan longer than the section.

### Audio and EFM

- Channel pair numbers 0–7, unique within a section; each pair carries one kind of content — tones, `file` or `video_audio` (mixing them is a parse error).
- Tone pairs: at least one of `left`/`right` present; a known `waveform`; frequencies within 0–22 000 Hz; amplitude 0.0–1.0; `ramp` and `frequency` not both given; a ramp needs `start`, `end` and a known `mode`, and its `period` must not exceed the section. These rules are skipped for file and soundtrack pairs.
- `file`: `path` set, ending in `.wav` or `.flac`; the file exists, can be read, and has one or two channels.
- `video_audio`: `stream` ≥ 0; the section source is a video, not an EXR still, and has an audio stream at that index.
- EFM requires PAL, NTSC or PAL-M (not SECAM), at most 79 programme-area sections.
- *Warnings*: a non-repeating audio file shorter than its section (the rest is silent); EFM pair not declared by any section; a programme track under 4 s (6 s for the first); no lead-in, so no table of contents.
- *Log notes* (information, not warnings): an audio file or soundtrack that is resampled to 48 kHz, and a soundtrack with more than two channels that is downmixed to stereo.

Checks that read audio files or soundtrack streams run with the CLI, `--validate`, and generation in the GUI. The GUI's background validation, and **Project → Validate**, check only the structure, as they do for picture sources.

### On-screen display

- `scale` 1–4; `fg_luma` and `bg_luma` from their permitted sets.
- Every token in `text` must be one of the six known tokens.

## Not yet implemented

Some of the intended design is described by the schema but has no runtime behaviour. Rather than generating a file that quietly lacks the content you asked for, videosynth rejects these outright:

| Feature | Status |
|---------|--------|
| `type: vitc` — vertical interval timecode | Error: *line injection type 'vitc' is not implemented in the current runtime* (without `target_lines` it fails earlier, on the missing lines) |
| `type: line_content` — custom per-line content | Error: *unsupported injection type 'line_content'* |
| `ntsc_laserdisc_vbi_burst: true` | Error: *parsed but not implemented in the current runtime* |

`laserdisc` is therefore the only usable section-level injection type today, and VITS remain project-level.

## Reading an error

Validation messages name the rule and the value that broke it:

```text
Laserdisc injection validation error: code_type 'picture_number' is not allowed in 'lead_in' sections.
Line injection validation error: VITS target line 17 conflicts with laserdisc reserved VBI ranges for NTSC.
Line injection validation error: target_lines must not be specified for laserdisc injections.
Laserdisc injection warning: CAV lead_out section has 500 frames; IEC 2 mm minimum at 1.6 um track pitch requires 1250.
```

The [troubleshooting page](../misc/troubleshooting.md) works through the common ones and what to do about each.
