# Laserdisc discs

videosynth can generate complete laserdisc-style discs: a lead-in, a programme area and a lead-out, carrying the IEC 60856 (PAL Laservision) and IEC 60857 (NTSC Laservision) VBI address and control codes that a player reads to know where it is on the disc.

This page explains how a disc project is put together. The exact fields for each code type are in the [laserdisc code reference](../reference/laserdisc-codes.md).

## What the codes do

Every field of a compliant disc carries a **24-bit biphase-modulated code** on reserved VBI lines. NTSC discs additionally carry a **40-bit FM code** on separate lines. Between them they tell the player:

- the current picture number (CAV) or elapsed programme time (CLV);
- which chapter is playing, and whether it is a valid search-stop point;
- whether it is in the lead-in, the programme, or the lead-out;
- the programme status — audio mode, CX noise reduction, copy permission;
- when to freeze on a still frame.

## Disc structure

A disc is expressed as sections carrying a `section_type`:

| `section_type` | Purpose | Minimum duration |
|----------------|---------|------------------|
| `lead_in` | Before the programme; the player spins up and synchronises here | 938 frames |
| `programme_area` | The content | — |
| `lead_out` | After the programme; signals the end of the disc | 1250 frames |

The minimum durations come from the track pitch: IEC requires 1.5 mm of track before the programme and 2 mm after it, which at 1.6 µm per track works out at 938 and 1250 frames. On a CAV disc carrying laserdisc codes, a shorter lead-in or lead-out is a validation **warning**: the project still generates, but the disc is shorter than the standard allows. CLV track density varies, so CLV sections are not checked.

Section order is validated. Once any section is a `lead_in` or `lead_out`, the sequence must be `[lead_in] programme_area… [lead_out]`:

- at most one `lead_in`, and nothing may precede it;
- at most one `lead_out`, and nothing may follow it;
- everything between them must be `programme_area`.

Out-of-order sections would break monotonic picture-number and time-code generation, so this is an error rather than a warning.

## CAV or CLV

The disc format is a **project-wide** setting, because a disc is entirely one or the other:

```yaml
line_injections:
  disc_type: CAV        # or CLV
```

| | CAV | CLV |
|---|-----|-----|
| Rotation | Constant angular velocity | Constant linear velocity |
| Addressing | Picture number, one frame per track | Programme time code + CLV picture number |
| Playing time | ~30 min per side (PAL) | ~60 min per side (PAL) |
| Still frame | Native — one frame per revolution | Limited |
| Chapter skip | Frame-accurate | Time-based |

The disc format decides which code types are legal. Setting `disc_type` changes the code checklist in the GUI and the set the validator will accept.

## Declaring codes

Codes are declared per section, because they legitimately differ between disc areas:

```yaml
sections:
  - name: Programme
    type: progressive
    source: "{bundled}/exr/720x576/100_BARS.exr"
    duration_frames: 250
    section_type: programme_area
    line_injections:
      - type: laserdisc
        codes:
          - code_type: picture_number
            start_value: 1
          - code_type: chapter_number
            chapter: 1
          - code_type: programme_status
            programme_status: "0x8DC000"
```

!!! important "You never choose VBI lines"
    `target_lines` must **not** appear on a `laserdisc` injection — specifying it is a validation error. Line placement is entirely determined by the standard from the code type, the disc format, and which field opens the current picture. videosynth works it out per frame.

## Which codes go where

Only certain code types are legal in each section type. These matrices are what the validator enforces.

### CAV

| Code type | `lead_in` | `programme_area` | `lead_out` |
|-----------|:---------:|:----------------:|:----------:|
| `lead_in` | ✅ | ❌ | ❌ |
| `lead_out` | ❌ | ❌ | ✅ |
| `picture_number` | ❌ | ✅ | ❌ |
| `picture_stop` | ❌ | ✅ | ❌ |
| `chapter_number` | ❌ | ✅ | ❌ |
| `programme_status` | ❌ | ✅ | ❌ |
| `users_code` | ✅ | ❌ | ✅ |
| `fm_picture_number` (NTSC) | ❌ | ✅ | ❌ |
| `fm_white_flag` (NTSC) | ✅ | ✅ | ✅ |

### CLV

| Code type | `lead_in` | `programme_area` | `lead_out` |
|-----------|:---------:|:----------------:|:----------:|
| `lead_in` | ✅ | ❌ | ❌ |
| `lead_out` | ❌ | ❌ | ✅ |
| `programme_time_code` | ❌ | ✅ | ❌ |
| `clv_code` | ❌ | ✅ | ❌ |
| `clv_picture_number` | ❌ | ✅ | ❌ |
| `chapter_number` | ❌ | ✅ | ❌ |
| `programme_status` | ❌ | ✅ | ❌ |
| `users_code` | ✅ | ❌ | ✅ |
| `fm_programme_time` (NTSC) | ❌ | ✅ | ❌ |
| `fm_white_flag` (NTSC) | ✅ | ✅ | ✅ |

## Timecode continuity

IEC 60856/60857 §10.1.5 requires timecodes to run **continuously** from the first frame of the programme area to the last, without resetting for any reason.

videosynth enforces this for you. The `picture_number`, `programme_time_code` and `clv_picture_number` counters persist across every section boundary and keep incrementing. The CLV clocks have nothing to configure at all — they run from the start of the output.

For `picture_number`, the rule is simply: **set `start_value` on the first programme section and omit it thereafter.**

```yaml
# First programme section — anchor the numbering.
- name: Chapter0
  section_type: programme_area
  line_injections:
    - type: laserdisc
      codes:
        - code_type: picture_number
          start_value: 1
        - code_type: chapter_number
          chapter: 0

# Later section — numbering continues automatically.
- name: Chapter1
  section_type: programme_area
  line_injections:
    - type: laserdisc
      codes:
        - code_type: picture_number     # no start_value
        - code_type: chapter_number
          chapter: 1
```

Setting `start_value` on a later section **re-anchors** the count there. That is a deliberate discontinuity — useful for modelling a player skip or a replayed passage, and wrong for ordinary chapter boundaries. Chapter changes never reset a timecode.

A simulated skip lives entirely in the VBI codes: the signal itself — subcarrier phase, colour field sequence, frame boundaries — stays continuous, so the metadata sidecar still declares `sequence_continuous = TRUE`. Detecting the jump from the picture numbers is exactly the exercise such a project sets a decoder.

### NTSC frozen values

On NTSC discs, timecodes are frozen outside the programme area:

| Section | Picture number | Programme time |
|---------|----------------|----------------|
| Lead-in | 0 (frozen) | 0:00 (frozen) |
| Programme area | Incrementing | Incrementing |
| Lead-out | Frozen at the last programme value | Frozen at the last programme value |

This is handled automatically.

## Chapters

Chapters are numbered 0–79. IEC 60856/60857 require a chapter to last at least 30 tracks (30 frames on CAV); a shorter CAV chapter is a validation **warning**. Chapters that include a `duration_frames: all` section have no statically known length and are not checked, and nor are CLV chapters.

```yaml
- code_type: chapter_number
  chapter: 3
```

Omitting `chapter` continues the previous section's chapter, carrying its stop-bit track counter across the boundary. Setting it starts a new chapter, restarting that counter.

### The stop-bit

Every chapter code carries a **stop-bit** telling the player whether it may stop at this chapter during a search:

| Position within the chapter | Stop-bit |
|-----------------------------|----------|
| Tracks 0–399 | 0 — search passes through |
| Track 400 onwards | 1 — search may stop |
| Any track of a chapter shorter than 800 tracks | 1 |
| The first chapter after the lead-in | Always 1 |

videosynth computes and encodes this for you from the chapter number and the track position; you only ever supply the chapter number.

## Programme status

The programme status code carries audio/video mode and copy permission, with a SECDED Hamming check digit:

```yaml
- code_type: programme_status
  programme_status: "0x8DC000"
```

The value encodes:

- **Nibbles 1–2** — `DC` for CX noise reduction on, `BA` for off.
- **X₃** — disc size, disc side, teletext presence and copy permission bits.
- **X₄** — audio/video mode (0–3 standard, 8 = mono dump).
- **X₅** — Hamming check, computed automatically.

The GUI has a **Programme Status Code** dialog that composes the word from readable fields and shows the resulting hex, which is considerably easier than assembling it by hand.

## The NTSC 40-bit FM system

NTSC discs carry two parallel encoding systems, and a compliant disc has both:

| System | Lines | Carries |
|--------|-------|---------|
| 24-bit biphase | 16–18 (field 1), 279–281 (field 2) | The primary address and control codes, shared with PAL |
| 40-bit FM | 10, 273 (data); 11, and 274 in the lead-out (white flag) | Picture number or programme time, independently |

To enable it, add the FM code types alongside the 24-bit ones:

```yaml
codes:
  - code_type: picture_number
    start_value: 1
  - code_type: fm_picture_number      # NTSC CAV
  - code_type: fm_white_flag
```

or, on a CLV disc, `fm_programme_time` in place of `fm_picture_number`.

### The white flag

`fm_white_flag` is a constant 100 IRE signal marking the start of each new picture — the player uses it to decide what a "frame" is when holding a still. In a video section every output frame is one picture starting on field 1, so the flag is on line 11 in the lead-in and the programme area, and on lines 11 and 274 in the lead-out. In a `"2:3"` programme section it marks each film picture instead (see [Film sections](#film-sections)). Nothing about this needs configuring.

## Line placement

For reference, this is where each code type actually lands. You do not configure any of it.

Placement is field-aware, per IEC 60856/60857 clause 10. In a video section every output frame is one picture whose first field is field 1, so the codes the standards place in "the first field of the picture" go in field 1 and the remaining codes go in field 2. The tables below show that case; [Film sections](#film-sections) covers `"2:3"` sections. Lines are given as frame lines; field 2 lines are PAL 329–331 and NTSC/PAL-M 279–281.

### CAV programme area

| Line (field 1 / PAL field 2 / NTSC field 2) | Field 1 | Field 2 |
|---------------------------------------------|---------|---------|
| 16 / 329 / 279 | `programme_status` | `picture_stop`, else `programme_status` |
| 17 / 330 / 280 | `picture_number` | `picture_stop`, else `chapter_number` |
| 18 / 331 / 281 | `picture_number` | `chapter_number` |

### CLV programme area

| Line (field 1 / PAL field 2 / NTSC field 2) | Field 1 | Field 2 |
|---------------------------------------------|---------|---------|
| 16 / 329 / 279 | `clv_picture_number` | `programme_status` |
| 17 / 330 / 280 | `programme_time_code` | `clv_code` |
| 18 / 331 / 281 | `programme_time_code` | `chapter_number` |

If a section carries none of the first-field codes — `picture_number` on CAV, or `programme_time_code` / `clv_picture_number` on CLV — there is nothing to put in field 1, and the field 2 codes are written in both fields.

### Lead-in and lead-out

In both fields, line 16 (329 PAL, 279 NTSC) carries `users_code` when the section has one, and lines 17–18 (330–331 PAL, 280–281 NTSC) carry the `lead_in` or `lead_out` code.

### NTSC and PAL-M FM lines

| Code type | Lead-in | Programme area | Lead-out |
|-----------|---------|----------------|----------|
| `fm_picture_number` (CAV) / `fm_programme_time` (CLV) | — | 10, 273 | — |
| `fm_white_flag` | 11 | 11 | 11, 274 |

`programme_status` (both standards) and `clv_code` (NTSC and PAL-M only) start 0.172H into the active line rather than at its start, per IEC 60856 Figure 14 / IEC 60857 Figure 11. This offset is applied automatically.

### Film sections

A [`"2:3"` section](../reference/sections.md#cadence) ([Film and pulldown](sources.md#film-and-pulldown)) shows each film frame for two or three fields, so a picture can start in either field and one frame in five starts none. The codes follow the film pictures, as a film transfer to disc places them:

- **CAV picture numbers count film frames.** `picture_number` and `fm_picture_number` advance once per film frame, four times in five output frames, and the count carries on into the next section either way.
- **The first field of each picture** carries `picture_number` (lines 17/18 or 280/281), or on CLV `clv_picture_number` and `programme_time_code`, and the white flag on line 11 or 274.
- **The field after it** carries `picture_stop`, even when that field is in the next frame. If it starts a picture of its own, its picture number keeps line 17/280 and the stop code is on line 16/279 only.
- **The remaining fields** carry `chapter_number`, `programme_status` and, on CLV, `clv_code`.
- **`fm_picture_number` changes on each picture's second field**, as in a video section, so a film picture's first field still carries the previous number (IEC 60857 §10.2.3).
- **CLV numbers stay video-frame time.** `clv_picture_number` is written only on picture-start fields, but its value is the number of the frame that field is in. Numbers skip the frame with no picture start, and the colour time error correction still counts video frames.

The colour field sequence follows the disc's video frames throughout, so it stays continuous through a film section even though the picture numbers there no longer number frames.

## What else a disc needs

- **VITS on the laserdisc lines.** The broadcast VITS lines collide with the reserved code ranges, so use `placement: laserdisc`. NTSC discs *must* carry `virs` — see [VITS](vits.md).
- **The pilot burst.** PAL discs carry a 3.75 MHz burst on every sync pulse (`pal_laserdisc_pilot_burst: true`), which needs a signed or raw [sample encoding](video-signal.md#sample-encodings) to avoid clipping.
- **The VBI burst.** NTSC discs keep the colour burst on the equalising and broad sync lines of the vertical interval (`ntsc_laserdisc_vbi_burst: true`); the burst on the broad pulses dips below sync tip, so it too needs a signed or raw [sample encoding](video-signal.md#sample-encodings) to avoid clipping.
- **No VITC.** Laserdisc uses the IEC biphase system as its sole timecode mechanism, and a `vitc` injection alongside a `laserdisc` one is a validation error. (VITC is not implemented at all in the current runtime, so the question does not arise in practice.)
- **EFM digital audio**, optionally — see [Audio](audio.md). Note that `section_type` alone drives the EFM disc-area mapping, so a project can have an EFM lead-in without carrying biphase codes at all.

## A complete example

```yaml
project:
  name: NtscClvDisc
  version: "1.0"

cvbs_presets:
  video_standard_preset: NTSC
  sample_encoding_preset: CVBS_U10_4FSC
  signal_state_preset: STANDARD_STABLE_LOCKED

output:
  video_path: "{output}/ntsc_clv_disc.cvbs"

line_injections:
  disc_type: CLV
  placement: laserdisc
  vits:
    - vits_type: virs                 # mandatory on NTSC discs
      target_lines: [19, 282]
    - vits_type: ntc7-composite
      target_lines: [20]
    - vits_type: ntc7-combination
      target_lines: [283]

sections:
  - name: LeadIn
    type: progressive
    source: "{bundled}/exr/720x486/PLUGE.exr"
    duration_frames: 938
    section_type: lead_in
    line_injections:
      - type: laserdisc
        codes:
          - code_type: lead_in
          - code_type: fm_white_flag

  - name: Programme
    type: progressive
    source: "{bundled}/exr/720x486/SMPTE_BARS_001.exr"
    duration_frames: 1800
    section_type: programme_area
    line_injections:
      - type: laserdisc
        codes:
          - code_type: programme_time_code
          - code_type: clv_code
          - code_type: clv_picture_number
          - code_type: chapter_number
            chapter: 1
          - code_type: programme_status
            programme_status: "0x8DC000"
          - code_type: fm_programme_time
          - code_type: fm_white_flag

  - name: LeadOut
    type: progressive
    source: "{bundled}/exr/720x486/PLUGE.exr"
    duration_frames: 1250
    section_type: lead_out
    line_injections:
      - type: laserdisc
        codes:
          - code_type: lead_out
          - code_type: fm_white_flag
```

## Further reading

The repository carries a deeper reference on biphase authoring — the full bit layouts, hex formats, encoding rules and worked examples — in [`docs-tech/user/biphase-design.md`](https://github.com/decode-orc/videosynth/blob/main/docs-tech/user/biphase-design.md){target="_blank"}.
