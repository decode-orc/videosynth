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

In the GUI both appear in the **Issues** dock; double-clicking an issue selects the section it relates to. The status bar summarises as *Project valid* or *n error(s), m warning(s)*.

## What gets checked

### Structure and schema

Unknown keys are rejected outright, at every level, naming the offending key and the block it is in. A misspelled `duration_frame` fails the project rather than being silently ignored.

### Presets

- One video standard, one sample encoding, one signal state per project.
- 4fsc generation requires a 4fsc encoding and the signal state the standard requires.
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

- Every section needs a valid `type` (`progressive`) and either frame content or line injections.
- `duration_frames` must be a positive integer or `"all"` (an omitted `duration_frames` means `"all"`).
- `start_frame` must be ≥ 0 and `end_frame` ≥ `start_frame`; both must lie inside the video source.
- `end_frame` must not be combined with an integer `duration_frames`.
- An EXR still takes neither `start_frame` nor `end_frame`, and needs an integer `duration_frames`.
- Once any section declares a `section_type`, the sequence must be `[lead_in] programme_area… [lead_out]` — at most one of each, in that order, with nothing outside them.
- CAV lead-in ≥ 938 frames; lead-out ≥ 1250 frames.

### Line injections

- `target_lines` must be within the standard's range (1–625 PAL, 1–525 NTSC/PAL-M).
- `target_lines` must **not** be given for `laserdisc` injections.
- Injected lines must not overlap within a section.
- When a `laserdisc` injection is active, nothing else may target a line in the reserved address ranges (PAL 6–18 and 319–331; NTSC 10–18 and 273–281).
- `vitc` and `laserdisc` must not appear in the same section.
- VITS `target_lines` are constrained by the `placement` policy — see [VITS](vits.md).

### Laserdisc codes

- The code type must be valid for the disc format, and for the section type it appears in.
- Picture numbers must not exceed 99,999 (PAL) or 79,999 (NTSC).
- Chapter numbers are 0–79, with a minimum chapter length of 30 tracks.
- `users_code` X₁ must be 0–7.
- CLV picture number and programme time code digits must be in their permitted BCD ranges.
- NTSC and PAL-M discs must carry a `virs` VITS entry.

### Noise and dropouts

- `noise_db` in 20.0–61.0; `noise_spread_db` ≥ 0; `noise_db − noise_spread_db` ≥ 20.0.
- `noise_spread_db` without `noise_db` is an error.
- Dropout `scale` values in 1–20; a `dropouts:` block that disables everything is an error — omit the block instead.
- *Warning*: spread noise with no VITS on the White SNR measurement line.
- *Warning*: scratch lifespan longer than the section.

### Audio and EFM

- Channel pair numbers 0–7, unique within a section; each pair carries one kind of content — tones, `file` or `video_audio` (mixing them is a parse error).
- Tone pairs: at least one of `left`/`right` present; frequencies within 0–22 000 Hz; amplitude 0.0–1.0. These rules are skipped for file and soundtrack pairs.
- `file`: `path` set, ending in `.wav` or `.flac`; the file exists, can be read, and has one or two channels.
- `video_audio`: `stream` ≥ 0; the section source is a video, not an EXR still, and has an audio stream at that index.
- EFM requires PAL or NTSC, at most 79 programme-area sections.
- *Warnings*: a non-repeating audio file shorter than its section (the rest is silent); EFM pair not declared by any section; a programme track under 4 s (6 s for the first); no lead-in, so no table of contents.
- *Log notes* (information, not warnings): an audio file or soundtrack that is resampled to 48 kHz, and a soundtrack with more than two channels that is downmixed to stereo.

Checks that read audio files or soundtrack streams run with the CLI, `--validate`, and generation in the GUI. The GUI's background validation checks only the structure, as it does for picture sources.

### On-screen display

- `scale` 1–4; `fg_luma` and `bg_luma` from their permitted sets.
- Every token in `text` must be one of the six known tokens.

## Not yet implemented

Some of the intended design is described by the schema but has no runtime behaviour. Rather than generating a file that quietly lacks the content you asked for, videosynth rejects these outright:

| Feature | Status |
|---------|--------|
| `type: vitc` — vertical interval timecode | Error: *line injection type 'vitc' is not implemented in the current runtime* |
| `type: line_content` — custom per-line content | Error, same message |
| `ntsc_laserdisc_vbi_burst: true` | Error: *parsed but not implemented in the current runtime* |

`laserdisc` is therefore the only usable section-level injection type today, and VITS remain project-level.

## Reading an error

Validation messages name the rule and the value that broke it:

```text
code_type 'picture_number' is not valid in section_type 'lead_in'
lead_out section duration 500 frames is below minimum of 1250 frames
VITS type 'ntc7-composite' targets line 17 which is within the NTSC laserdisc reserved range
target_lines must not be specified for laserdisc injections
```

The [troubleshooting page](../misc/troubleshooting.md) works through the common ones and what to do about each.
