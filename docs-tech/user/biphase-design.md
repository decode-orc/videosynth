# LaserDisc Biphase Encoding User Guide

**Document ID:** VS-USER-BIPHASE-001  
**Related Specifications:** IEC 60856 (PAL), IEC 60857 (NTSC)

---

## Table of Contents

1. [Introduction](#1-introduction)
2. [Core Concepts](#2-core-concepts)
3. [Section Type Rules](#3-section-type-rules)
4. [Code Type Reference](#4-code-type-reference)
5. [Timecode Continuity](#5-timecode-continuity)
6. [Chapter System](#6-chapter-system)
7. [Line Placement](#7-line-placement)
8. [NTSC 40-bit FM System](#8-ntsc-40-bit-fm-system)
9. [Configuration Examples](#9-configuration-examples)
10. [YAML Configuration Guide](#10-yaml-configuration-guide)
11. [Quick Reference](#11-quick-reference)
12. [Troubleshooting](#12-troubleshooting)

---

## 1. Introduction

### Overview of LaserDisc Biphase Codes

LaserDisc uses a **biphase-modulated signal** embedded in the Vertical Blanking Interval (VBI) of each video field to carry addressing, chapter, and control information. Every field contains one 24-bit biphase code on specific reserved lines; NTSC discs additionally carry a 40-bit FM coded signal on separate lines.

These codes allow a LaserDisc player to:
- Know the current picture number (CAV) or programme time (CLV)
- Navigate chapters and perform still-frame stops
- Identify the disc section it is playing (lead-in, programme, lead-out)
- Report programme status (audio mode, copy permission, CX noise reduction)

### CAV vs CLV: Key Differences

| Property | CAV | CLV |
|----------|-----|-----|
| **Disc rotation** | Constant angular velocity | Constant linear velocity |
| **Addressing** | Picture number (1 frame per track) | Programme time code + CLV picture number |
| **Playing time** | ~30 min per side (PAL) | ~60 min per side (PAL) |
| **Still frame** | Native (one frame per revolution) | Limited |
| **Chapter skip** | Frame-accurate | Time-based |

### When to Use Biphase Injection

Use the `laserdisc` injection type when mastering content for LaserDisc replication. The injection is mandatory on all three disc sections (lead-in, programme area, lead-out) and is automatically placed on the correct VBI lines by the encoder — no manual line selection is needed.

---

## 2. Core Concepts

### Section Types

A LaserDisc is divided into three sections, each with a distinct purpose:

| Section | `section_type` | Purpose |
|---------|---------------|---------|
| **Lead-in** | `lead_in` | Area before programme content; player spins up and synchronises here |
| **Programme area** | `programme_area` | The actual video content |
| **Lead-out** | `lead_out` | Area after programme content; signals end of disc |

Each section must have a `laserdisc` line injection configured with the appropriate code types for that section.

**Minimum durations:**

| Section | PAL CAV | NTSC CAV | Note |
|---------|---------|---------|------|
| Lead-in | ≥ 938 frames | ≥ 938 frames | ≥ 1.5 mm at 1.6 µm pitch |
| Lead-out | ≥ 1250 frames | ≥ 1250 frames | ≥ 2.0 mm at 1.6 µm pitch |

### Disc Types

Set `disc_type` to `CAV` or `CLV` **once, at the project level**, in the top-level `line_injections:` block (a sibling of `output:` and `sections:`). A disc is entirely CAV or entirely CLV, so this single project-wide selection applies to every section and determines which code types are valid and how the player addresses frames:

```yaml
line_injections:
  disc_type: CAV        # project-wide CAV/CLV selection
```

Individual sections then carry only their per-section `laserdisc` `codes:` — they no longer repeat `disc_type`.

### Standards

Set the video standard via `cvbs_presets.video_standard_preset`:

| Standard | Frame rate | Lines | Notes |
|----------|-----------|-------|-------|
| `PAL` | 25 fps | 625 | 24-bit biphase only |
| `NTSC` | 29.97 fps | 525 | 24-bit biphase + 40-bit FM |
| `PAL_M` | 29.97 fps | 525 | Treated as System M: NTSC line placement, 40-bit FM, 79 999 maximum CAV picture number, 1800 frames/min and 30 fps CLV picture numbers with colour time error correction |

SECAM has no LaserVision format; laserdisc injections are validation errors for SECAM projects.

---

## 3. Section Type Rules

### The Code-Type / Section-Type Matrix

Only certain code types are allowed in each section type. The following matrices are normative — any combination marked ❌ will be rejected by the validator.

#### CAV Discs

| Code Type | lead_in | programme_area | lead_out |
|-----------|:-------:|:--------------:|:--------:|
| `lead_in` | ✅ | ❌ | ❌ |
| `lead_out` | ❌ | ❌ | ✅ |
| `picture_number` | ❌ | ✅ | ❌ |
| `picture_stop` | ❌ | ✅ | ❌ |
| `chapter_number` | ❌ | ✅ | ❌ |
| `programme_status` | ❌ | ✅ | ❌ |
| `users_code` | ✅ | ❌ | ✅ |
| `fm_picture_number` (NTSC) | ❌ | ✅ | ❌ |
| `fm_white_flag` (NTSC) | ✅ | ✅ | ✅ |

#### CLV Discs

| Code Type | lead_in | programme_area | lead_out |
|-----------|:-------:|:--------------:|:--------:|
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

### Valid and Invalid Configuration Examples

The `disc_type` (CAV vs CLV) is a project-wide setting in the top-level `line_injections:` block; the fragments below show only the per-section `section_type` and `codes:`.

```yaml
# VALID: lead_in code in lead_in section
section_type: lead_in
codes:
  - code_type: lead_in       # ✅ correct section

# INVALID: picture_number in lead_in
section_type: lead_in
codes:
  - code_type: picture_number  # ❌ ERROR: not allowed in lead_in

# VALID: users_code in lead_out
section_type: lead_out
codes:
  - code_type: lead_out
  - code_type: users_code      # ✅ users_code allowed in lead_out
    users_code: "0x80D000"

# INVALID: users_code in programme_area
section_type: programme_area
codes:
  - code_type: users_code      # ❌ ERROR: users_code only in lead_in/lead_out
```

---

## 4. Code Type Reference

### CAV Code Types

#### `lead_in` — Lead-In Code

| Property | Value |
|----------|-------|
| Hex format | `88FFFF` |
| Section | `lead_in` only |
| Auto-increment | No |
| Lines (PAL) | 17, 18, 330, 331 |
| Lines (NTSC) | 17, 18, 280, 281 |

Fixed code that marks the lead-in section of the disc. No configuration parameters needed.

```yaml
- code_type: lead_in
```

---

#### `lead_out` — Lead-Out Code

| Property | Value |
|----------|-------|
| Hex format | `80EEEE` |
| Section | `lead_out` only |
| Auto-increment | No |
| Lines (PAL) | 17, 18, 330, 331 |
| Lines (NTSC) | 17, 18, 280, 281 |

Fixed code that marks the lead-out section.

```yaml
- code_type: lead_out
```

---

#### `picture_number` — CAV Picture Number

| Property | Value |
|----------|-------|
| Hex format | `FX₁X₂X₃X₄X₅` |
| Section | `programme_area` only |
| Disc type | CAV only |
| Auto-increment | Yes (per frame) |
| Lines (PAL) | 17, 18 (field 1) |
| Lines (NTSC/PAL-M) | 17, 18 (field 1) |
| Value range | 1–99,999 (PAL) / 1–79,999 (NTSC, PAL-M) |
| Digit constraint | X₁–X₅ must each be 0–9 (decimal digits) |

The picture number identifies each unique frame on a CAV disc. It auto-increments by 1 per frame.

`start_value` is **optional**:

- **Omit it** to continue numbering from the previous section. The counter runs continuously across every section, so a disc-wide sequential picture number needs `start_value` on the *first* section only (or nowhere, in which case numbering begins at 1).
- **Set it** to re-anchor the count for that section — the first frame of the section takes `start_value`, overriding the continued count (useful for skip/replay sections whose picture numbers are non-contiguous).

```yaml
# First programme section: anchor at 1 (or omit start_value to default to 1).
- code_type: picture_number
  start_value: 1

# Later section: omit start_value to continue the count from the previous
# section, or set it to jump the numbering to a specific frame.
- code_type: picture_number
```

---

#### `picture_stop` — CAV Picture Stop

| Property | Value |
|----------|-------|
| Hex format | `82CFFF` |
| Section | `programme_area` only |
| Disc type | CAV only |
| Lines (PAL) | 329, 330 (field 2) |
| Lines (NTSC/PAL-M) | 279, 280 (field 2) |

Signals the player to stop at a specific frame (freeze-frame marker). Generated in field 2, the field immediately following the picture-number field; it takes priority over `programme_status` on 329/279 and over `chapter_number` on 330/280. In a section without `picture_number` it is written in both fields (16, 17 and 329, 330 / 279, 280).

```yaml
- code_type: picture_stop
```

---

#### `chapter_number` — Chapter Number Code

| Property | Value |
|----------|-------|
| Hex format | `8X₁X₂DDD` |
| Section | `programme_area` only |
| Auto-increment | No (static per section) |
| Lines (CAV PAL) | 330 (unless `picture_stop`), 331 — field 2 |
| Lines (CLV PAL) | 331 — field 2 |
| Lines (CAV NTSC/PAL-M) | 280 (unless `picture_stop`), 281 — field 2 |
| Lines (CLV NTSC/PAL-M) | 281 — field 2 |
| Chapter range | 0–79 |
| Minimum length | 30 tracks (CAV: validator warning) |

In a section without the picture-field codes (`picture_number` on CAV; `programme_time_code`/`clv_picture_number` on CLV) the chapter is written in both fields.

Identifies the current chapter. Contains an encoded **stop-bit** that controls whether the player can use this chapter as a still-frame search stop point. See [Chapter System](#6-chapter-system) for stop-bit rules.

`chapter` is **optional**:

- **Omit it** to continue the previous section's chapter — the chapter number and its stop-bit track counter carry across the section boundary (if no earlier section set a chapter, numbering begins at 0).
- **Set it** to start a new chapter at that section; the stop-bit track counter restarts for the new chapter per IEC 60856/60857 §10.1.5.

```yaml
# First programme section: start chapter 0 (or omit chapter to default to 0).
- code_type: chapter_number
  chapter: 0           # Chapter number 0–79

# Later section: omit chapter to continue the previous section's chapter,
# or set it to start a new chapter here.
- code_type: chapter_number
```

---

#### `programme_status` — Programme Status Code

| Property | Value |
|----------|-------|
| Hex format | `8DC/BA X₃X₄X₅` |
| Section | `programme_area` only |
| Lines (CAV) | 16 (field 1) and 329 / 279 (field 2, unless `picture_stop`) |
| Lines (CLV) | 329 / 279 (field 2); also 16 when the section has no `programme_time_code`/`clv_picture_number` |
| Horizontal start | 0.172H |

Carries audio/video mode and copy-permission information. Must include a valid Hamming check code in X₅. Use the `ProgrammeStatusCodeBuilder` to calculate valid codes.

The hex value encodes:
- Nibbles 1–2: `DC` = CX noise reduction on, `BA` = CX off
- X₃ LSB: copy permission (0 = prohibited, 1 = permitted)
- X₄: audio/video mode (0–3 standard, 8 = mono dump)
- X₅: Hamming check (computed automatically)

```yaml
- code_type: programme_status
  programme_status: "0x8DC000"   # CX on, copy prohibited, stereo, Hamming=0
```

---

#### `users_code` — User's Code

| Property | Value |
|----------|-------|
| Hex format | `8X₁DX₃X₄X₅` |
| Section | `lead_in` or `lead_out` only |
| X₁ constraint | X₁ = 0–7 only (not 8–F) |

Carries disc-specific metadata for the disc manufacturer.

```yaml
- code_type: users_code
  users_code: "0x80D234"    # X₁=0 (valid: 0–7)
```

---

### CLV Code Types

#### `programme_time_code` — CLV Programme Time Code

| Property | Value |
|----------|-------|
| Hex format | `FX₁DDX₂X₃` |
| Section | `programme_area` only |
| Disc type | CLV only |
| Auto-increment | Yes (minutes) |
| Lines (PAL) | 17, 18 (field 1) |
| Lines (NTSC/PAL-M) | 17, 18 (field 1) |
| X₁ range | 0–F (hours, 0–15) |
| X₂ range | 0–5 (minutes tens, BCD) |
| X₃ range | 0–9 (minutes units, BCD) |

Encodes elapsed programme time (hours and minutes) for CLV discs. Starts at `0:00` and auto-increments. No configuration needed beyond including the code type — the generator starts at `0:00` automatically.

```yaml
- code_type: programme_time_code
```

---

#### `clv_code` — CLV Code

| Property | Value |
|----------|-------|
| Hex format | `87FFFF` |
| Section | `programme_area` only |
| Disc type | CLV only |
| Lines (PAL) | 330 (field 2) |
| Lines (NTSC/PAL-M) | 280 (field 2), 0.172H horizontal start |

A fixed marker code that identifies the disc as CLV, placed in field 2 (the field without programme time code and CLV picture number). In a section with neither of those codes it is written in both fields.

```yaml
- code_type: clv_code
```

---

#### `clv_picture_number` — CLV Picture Number

| Property | Value |
|----------|-------|
| Hex format | `8X₁EX₃X₄X₅` |
| Section | `programme_area` only |
| Disc type | CLV only |
| Auto-increment | Yes (per frame) |
| Lines (PAL) | 16 (field 1) |
| Lines (NTSC/PAL-M) | 16 (field 1) |
| X₁ range | A–F |
| X₃ range | 0–9 |
| X₄ range | 0–2 |
| X₅ range | 0–9 |

Encodes the frame position within the current second (X₄×10 + X₅ = frame 0–29) and the second offset within a 60-second block (X₁, X₃). Auto-increments each frame.

**NTSC/PAL-M note**: Includes colour time error correction (IEC 60857 Amendment 2 §10.1.10) — the seconds count periodically jumps forward at specific frame counts to maintain accurate colour timing.

```yaml
- code_type: clv_picture_number
```

---

### NTSC 40-bit FM Code Types

These code types are System M only (NTSC and PAL-M) and must not be used with PAL projects.

#### `fm_picture_number` — FM Picture Number (NTSC CAV)

| Property | Value |
|----------|-------|
| Signal type | 40-bit FM |
| Section | `programme_area` only |
| Disc type | CAV + NTSC/PAL-M only |
| Lines | 10, 273 |
| Value range | 0–79,999 |
| Update timing | Second field of each new picture |

Carries the CAV picture number in the 40-bit FM format on lines 10/273. Starts at 1 and auto-increments with the 24-bit `picture_number`.

```yaml
- code_type: fm_picture_number
```

---

#### `fm_programme_time` — FM Programme Time (NTSC CLV)

| Property | Value |
|----------|-------|
| Signal type | 40-bit FM |
| Section | `programme_area` only |
| Disc type | CLV + NTSC/PAL-M only |
| Lines | 10, 273 |

Carries CLV programme time in the 40-bit FM format. X₅ mode indicator: A = lead-in, B = end of lead-in, D = programme, C = lead-out.

```yaml
- code_type: fm_programme_time
```

---

#### `fm_white_flag` — FM White Flag (NTSC)

| Property | Value |
|----------|-------|
| Signal type | 100 IRE constant level |
| Section | `lead_in`, `programme_area`, `lead_out` |
| Lines (lead-in) | Line 11 only |
| Lines (programme) | Line 11 (field 1) |
| Lines (lead-out) | Lines 11 and 274 |

The white flag is a constant 100 IRE signal that marks the start of each new picture. Every output frame is one picture opened by field 1, so in the programme area the flag is always on line 11.

```yaml
- code_type: fm_white_flag
```

---

## 5. Timecode Continuity

### Continuous Counting

IEC 60856/60857 §10.1.5 requires that timecodes run **continuously** from the first frame of the programme area to the last, without resetting for any reason.

The affected code types are:
- `picture_number` (CAV): increments every frame and continues across sections (anchored by `start_value` on the first section, or re-anchored where a section sets it explicitly)
- `programme_time_code` (CLV): counts from `0:00`, increments minutes every 1500 (PAL) or 1800 (NTSC, PAL-M) frames
- `clv_picture_number` (CLV): counts seconds and frames from `0:00`, increments every frame

### Chapters Do NOT Reset Timecodes

This is a critical rule that is often misunderstood: **chapter boundaries do not reset any timecode counter**. The generator enforces this for you — the `picture_number`, `programme_time_code`, and `clv_picture_number` counters persist across every section boundary and keep incrementing continuously. For the CLV clocks there is nothing to set. For `picture_number`, simply **omit `start_value`** on the later sections and the count continues automatically.

**Example (recommended — continuous across chapters):**
```yaml
# Chapter 0: anchor the numbering at 1 on the first section.
- name: Chapter0
  section_type: programme_area
  codes:
    - code_type: picture_number
      start_value: 1
    - code_type: chapter_number
      chapter: 0

# Chapter 1: omit start_value — numbering continues from the previous section.
- name: Chapter1
  section_type: programme_area
  codes:
    - code_type: picture_number   # ← continues automatically (no start_value)
    - code_type: chapter_number
      chapter: 1
```

Setting `start_value` explicitly on a later section is still allowed and **re-anchors** the count for that section — use it only when you deliberately want a discontinuity (for example, a skip/replay section whose picture numbers jump). Setting `start_value: 1` on every chapter, which would restart numbering at each boundary, is almost never what you want:

```yaml
# Chapter 1: re-anchors picture_number back to 1 — a deliberate discontinuity,
# not continuous numbering.
- name: Chapter1
  section_type: programme_area
  codes:
    - code_type: picture_number
      start_value: 1          # ← re-anchors; only correct if you *want* a jump
```

### NTSC Frozen Values

On NTSC discs, certain timecodes are frozen at fixed values during lead-in and lead-out:

| Section | picture_number / fm_picture_number | programme_time / fm_programme_time |
|---------|------------------------------------|------------------------------------|
| Lead-in | 0 (frozen) | 0:00 (frozen) |
| Programme area | Active, incrementing | Active, incrementing |
| Lead-out | Frozen at last programme value | Frozen at last programme value |

This is handled automatically — you do not need to do anything special in your YAML configuration.

---

## 6. Chapter System

### Chapter Numbering

- Chapters are numbered 0–79
- Each programme-area section can contain one chapter marker
- Minimum chapter length: **30 tracks** (30 frames on CAV)

### Stop-Bit Explained

Every chapter code contains a **stop-bit** that controls player search behaviour:

- **Stop-bit = 0**: disables the player's search-stop action (the player will skip past this chapter during search)
- **Stop-bit = 1**: enables the player's search-stop action (the player will stop at this chapter during search)

The stop-bit follows these rules per IEC 60856/60857 §10.1.5:

| Track | Stop-bit | Exception |
|-------|---------|-----------|
| 0–399 | 0 | First chapter after lead-in: always 1 |
| 400+ | 1 | — |
| Any track on short chapter (< 800 tracks) | 1 | — |

**The first chapter after lead-in must always have stop-bit = 1.** This is enforced automatically.

### Encoding Formula

The stop-bit is encoded in the MSB of X₁ in the chapter code `8X₁X₂DDD`:

```
X₁ = (stop_bit << 3) | (chapter_number / 16)
X₂ = chapter_number % 16
```

For example, chapter 5 with stop-bit = 1:
- `X₁ = (1<<3) | 0 = 0x8`
- `X₂ = 5`
- Code = `0x885DDD`

The encoder manages this automatically — you only need to specify the chapter number.

---

## 7. Line Placement

### Field-Aware Line Numbering

LaserDisc uses sequential line numbering across the full frame (both fields). PAL line 1 maps to the first line of field 1; line 313 maps to the first line of field 2. NTSC line 1 maps to field 1; line 263 maps to field 2.

Every output frame is one picture whose **first field is field 1**. Codes that IEC 60856/60857 clause 10 place in "the first field of the picture" go in field 1; codes for the remaining fields go in field 2. If a programme-area section carries none of the first-field codes (`picture_number` on CAV; `programme_time_code` or `clv_picture_number` on CLV), the field-2 assignments are written in **both** fields.

### Reserved VBI Ranges

The following line ranges are exclusively reserved for laserdisc. When the project declares a `disc_type` (a laserdisc project), no project VITS entry may target them:

| Standard | Field 1 range | Field 2 range |
|----------|-------------|-------------|
| PAL | Lines 6–18 | Lines 319–331 |
| NTSC / PAL-M | Lines 10–18 | Lines 273–281 |

### Active Line Assignments

Field-2 lines are 329/330/331 on PAL and 279/280/281 on NTSC/PAL-M; "A > B" means A takes the line when present, otherwise B.

#### Lead-in / lead-out (CAV and CLV)

| Line (F1 / PAL F2 / NTSC F2) | Code |
|------------------------------|------|
| 16 / 329 / 279 | `users_code` (when declared) |
| 17, 18 / 330, 331 / 280, 281 | `lead_in` or `lead_out` |

#### CAV programme area

| Line | Field 1 | Field 2 |
|------|---------|---------|
| 16 / 329 / 279 | `programme_status` | `picture_stop` > `programme_status` |
| 17 / 330 / 280 | `picture_number` | `picture_stop` > `chapter_number` |
| 18 / 331 / 281 | `picture_number` | `chapter_number` |

#### CLV programme area

| Line | Field 1 | Field 2 |
|------|---------|---------|
| 16 / 329 / 279 | `clv_picture_number` | `programme_status` |
| 17 / 330 / 280 | `programme_time_code` | `clv_code` |
| 18 / 331 / 281 | `programme_time_code` | `chapter_number` |

A field-1 line whose code the section does not declare is left empty.

#### NTSC / PAL-M 40-bit FM lines

| Code type | Field 1 | Field 2 |
|-----------|---------|---------|
| `fm_picture_number` (CAV) / `fm_programme_time` (CLV) | 10 | 273 |
| `fm_white_flag` — lead-in, programme area | 11 | — |
| `fm_white_flag` — lead-out | 11 | 274 |

### Horizontal Start Timing

Two code types have a delayed horizontal start of 0.172H from the start of the active line (IEC 60856 Figure 14, IEC 60857 Figure 11):

- `programme_status` (all standards)
- `clv_code` (NTSC and PAL-M only)

All other code types begin at the start of the active video line. This offset is applied automatically.

### Priority Rules

When multiple code types would occupy the same line, the following priorities apply:

1. Lead-in/lead-out codes are the only codes (besides `users_code` on line 16/329/279) in their sections.
2. Field 1 of a programme-area picture carries only the first-field codes (CAV: `programme_status`, `picture_number`; CLV: `clv_picture_number`, `programme_time_code`), so `chapter_number`, `picture_stop` and `clv_code` never compete with them.
3. In field 2, `picture_stop` has priority over `programme_status` on 329/279 and over `chapter_number` on 330/280 (CAV).
4. `chapter_number` alone occupies 331/281 in field 2 (CAV and CLV).

---

## 8. NTSC 40-bit FM System

### Overview

NTSC discs carry **two parallel encoding systems**:
1. **24-bit biphase** on lines 16–18 / 279–281 — the primary system shared with PAL
2. **40-bit FM** on lines 10–11 / 273–274 — System M (NTSC/PAL-M) supplementary system

Both systems must be configured simultaneously on NTSC discs. They differ in both bit-cell duration and transition time:

| Property | 24-bit biphase | 40-bit FM |
|----------|---------------|-----------|
| Bit cell duration | 2.0 µs ± 0.01 µs | 1.0 µs ± 0.01 µs (IEC 60857 §10.2 — half the biphase cell) |
| Transition time (10 %–90 %) | 225 ns ± 25 ns | 135 ns ± 15 ns |
| Modulation | Biphase mark (transition at cell centre) | Pulse-spacing FM (see below) |

### 40-bit FM Bit Layout

The 40-bit code is structured as follows (MSB = bit 1):

| Bits | Content |
|------|---------|
| 1–4 | Clock sync (`0011`) |
| 5 | Field indicator (1 = field 1, 0 = field 2) |
| 6–12 | Leading recognition bits (`1110010`) |
| 13–32 | Data bits: X₅, X₄, X₃, X₂, X₁ (4 bits each, transmitted LSB first) |
| 33 | Odd parity over the **data bits only** (bits 13–32) |
| 34–40 | Trailing recognition bits (`0001101`) |

The parity bit covers the data field alone; the clock sync, field indicator and recognition patterns are excluded from the calculation.

### FM Modulation

The 40-bit signal is **pulse-spacing FM**, not Manchester/biphase. The signal starts HIGH, and a decoder anchors itself on the first HIGH sample:

- **`0`** — hold the current level for the full bit cell (T₀ ≈ 1.0 µs), then flip. The decoder sees a long inter-transition interval (≥ 0.75 µs).
- **`1`** — hold for T₁ = T₀/2 ≈ 0.5 µs, flip briefly (the "pip"), then return at T₀. The net level is unchanged across the cell. The decoder sees a short interval (< 0.75 µs), then scans past the pip end to re-anchor.

All transitions are S-curve shaped with a 135 ns ± 15 ns 10 %–90 % time.

### White Flag

The white flag is a **100 IRE constant-level signal** (not modulated) on line 11 (field 1) and, in lead-out, also line 274 (field 2). It marks the start of each complete picture and is placed automatically:

- **Lead-in**: line 11 only (field 1 line; line 274 is not used)
- **Programme area**: line 11 only — every output frame is one picture opened by field 1
- **Lead-out**: both lines 11 and 274 for the full lead-out duration

### Coexistence with 24-bit Biphase

On NTSC discs, the 40-bit FM system occupies lines 10–11 / 273–274, which are within the laserdisc reserved range. The 24-bit biphase system occupies lines 16–18 / 279–281. Both are generated simultaneously — you only need to include both the 40-bit (`fm_picture_number` or `fm_programme_time`, plus `fm_white_flag`) and 24-bit code types in your section's `codes` list.

---

## 9. Configuration Examples

### Example 1: Complete PAL CAV Disc

A minimal PAL CAV disc with lead-in, two programme chapters, and lead-out.

```yaml
project:
  name: MyPalCavDisc
  version: "1.0"
  description: PAL CAV example with 2 chapters

cvbs_presets:
  video_standard_preset: PAL
  sample_encoding_preset: CVBS_TPG21_4FSC
  signal_state_preset: STANDARD_STABLE_LOCKED

output:
  video_path: output/my_disc.cvbs

# Project-wide disc type — applies to every section.
line_injections:
  disc_type: CAV

sections:
  # Lead-in: minimum 938 frames (≥ 1.5 mm)
  - name: LeadIn
    type: progressive
    source: assets/lead_in_content.mkv
    duration_frames: 938
    section_type: lead_in
    line_injections:
      - type: laserdisc
        codes:
          - code_type: lead_in
          - code_type: users_code
            users_code: "0x80D000"    # Manufacturer data (X₁=0)

  # Programme chapter 0 (100 frames, pictures 1–100)
  - name: Chapter0
    type: progressive
    source: assets/chapter0.mkv
    duration_frames: 100
    section_type: programme_area
    line_injections:
      - type: laserdisc
        codes:
          - code_type: picture_number
            start_value: 1            # First picture
          - code_type: chapter_number
            chapter: 0
          - code_type: programme_status
            programme_status: "0x8DC000"   # CX on, stereo, copy prohibited

  # Programme chapter 1 (200 frames, pictures 101–300)
  - name: Chapter1
    type: progressive
    source: assets/chapter1.mkv
    duration_frames: 200
    section_type: programme_area
    line_injections:
      - type: laserdisc
        codes:
          - code_type: picture_number
            start_value: 101          # Continues from chapter 0
          - code_type: chapter_number
            chapter: 1
          - code_type: programme_status
            programme_status: "0x8DC000"

  # Lead-out: minimum 1250 frames (≥ 2.0 mm)
  - name: LeadOut
    type: progressive
    source: assets/lead_out_content.mkv
    duration_frames: 1250
    section_type: lead_out
    line_injections:
      - type: laserdisc
        codes:
          - code_type: lead_out
          - code_type: users_code
            users_code: "0x80D000"
```

---

### Example 2: Complete PAL CLV Disc

```yaml
project:
  name: MyPalClvDisc
  version: "1.0"

cvbs_presets:
  video_standard_preset: PAL
  sample_encoding_preset: CVBS_TPG21_4FSC
  signal_state_preset: STANDARD_STABLE_LOCKED

output:
  video_path: output/clv_disc.cvbs

# Project-wide disc type — applies to every section.
line_injections:
  disc_type: CLV

sections:
  - name: LeadIn
    type: progressive
    source: assets/lead_in.mkv
    duration_frames: 938
    section_type: lead_in
    line_injections:
      - type: laserdisc
        codes:
          - code_type: lead_in

  - name: Programme
    type: progressive
    source: assets/content.mkv
    duration_frames: 75000       # 50 minutes at 25 fps
    section_type: programme_area
    line_injections:
      - type: laserdisc
        codes:
          - code_type: programme_time_code    # auto-increments from 0:00
          - code_type: clv_code
          - code_type: clv_picture_number     # auto-increments each frame
          - code_type: chapter_number
            chapter: 0
          - code_type: programme_status
            programme_status: "0x8DC000"

  - name: LeadOut
    type: progressive
    source: assets/lead_out.mkv
    duration_frames: 1250
    section_type: lead_out
    line_injections:
      - type: laserdisc
        codes:
          - code_type: lead_out
```

---

### Example 3: NTSC CAV Disc

NTSC discs require both 24-bit biphase codes and 40-bit FM codes, plus a mandatory VIRS signal on lines 19/282. The disc type and the VIRS VITS are declared once at the project level, so every section is covered automatically.

```yaml
project:
  name: MyNtscCavDisc
  version: "1.0"

cvbs_presets:
  video_standard_preset: NTSC
  sample_encoding_preset: CVBS_TPG21_4FSC
  signal_state_preset: STANDARD_STABLE_LOCKED

output:
  video_path: output/ntsc_disc.cvbs

# Project-wide disc type and the mandatory NTSC VIRS reference, applied to
# every frame of every section (IEC 60857 §9.1.3).
line_injections:
  disc_type: CAV
  vits:
    - vits_type: virs
      target_lines: [19, 282]

sections:
  - name: LeadIn
    type: progressive
    source: assets/lead_in.mkv
    duration_frames: 938
    section_type: lead_in
    line_injections:
      - type: laserdisc
        codes:
          - code_type: lead_in
          - code_type: fm_white_flag          # NTSC: required in all sections
          - code_type: users_code
            users_code: "0x80D000"

  - name: Chapter0
    type: progressive
    source: assets/chapter0.mkv
    duration_frames: 100
    section_type: programme_area
    line_injections:
      - type: laserdisc
        codes:
          - code_type: picture_number
            start_value: 1
          - code_type: chapter_number
            chapter: 0
          - code_type: programme_status
            programme_status: "0x8DC001"
          - code_type: fm_picture_number      # 40-bit FM on lines 10/273
          - code_type: fm_white_flag          # 100 IRE on line 11 (field 1)

  - name: LeadOut
    type: progressive
    source: assets/lead_out.mkv
    duration_frames: 1250
    section_type: lead_out
    line_injections:
      - type: laserdisc
        codes:
          - code_type: lead_out
          - code_type: fm_white_flag
          - code_type: users_code
            users_code: "0x80D000"
```

---

### Example 4: Multi-Chapter CAV Disc with Picture Stops

A CAV disc with 4 chapters, each 500 frames long. Chapter 0 is the first chapter after lead-in (stop-bit always 1 by IEC rule); chapters 1–3 follow normal stop-bit behaviour.

```yaml
# Project-wide disc type — applies to every section.
line_injections:
  disc_type: CAV

sections:
  - name: LeadIn
    type: progressive
    source: assets/bars.exr
    duration_frames: 938
    section_type: lead_in
    line_injections:
      - type: laserdisc
        codes:
          - code_type: lead_in

  # Chapter 0: pictures 1–500 (stop-bit = 1 always, per IEC)
  - name: Ch0
    type: progressive
    source: assets/ch0.mkv
    duration_frames: 500
    section_type: programme_area
    line_injections:
      - type: laserdisc
        codes:
          - code_type: picture_number
            start_value: 1
          - code_type: chapter_number
            chapter: 0
          - code_type: picture_stop           # Still-frame at last frame
          - code_type: programme_status
            programme_status: "0x8DC000"

  # Chapter 1: pictures 501–1000 (stop-bit starts 0, transitions to 1 at track 400)
  - name: Ch1
    type: progressive
    source: assets/ch1.mkv
    duration_frames: 500
    section_type: programme_area
    line_injections:
      - type: laserdisc
        codes:
          - code_type: picture_number
            start_value: 501
          - code_type: chapter_number
            chapter: 1
          - code_type: programme_status
            programme_status: "0x8DC000"

  # Chapter 2: pictures 1001–1500
  - name: Ch2
    type: progressive
    source: assets/ch2.mkv
    duration_frames: 500
    section_type: programme_area
    line_injections:
      - type: laserdisc
        codes:
          - code_type: picture_number
            start_value: 1001
          - code_type: chapter_number
            chapter: 2
          - code_type: programme_status
            programme_status: "0x8DC000"

  # Chapter 3: pictures 1501–2000
  - name: Ch3
    type: progressive
    source: assets/ch3.mkv
    duration_frames: 500
    section_type: programme_area
    line_injections:
      - type: laserdisc
        codes:
          - code_type: picture_number
            start_value: 1501
          - code_type: chapter_number
            chapter: 3
          - code_type: programme_status
            programme_status: "0x8DC000"

  - name: LeadOut
    type: progressive
    source: assets/bars.exr
    duration_frames: 1250
    section_type: lead_out
    line_injections:
      - type: laserdisc
        codes:
          - code_type: lead_out
```

---

## 10. YAML Configuration Guide

### Required Fields

The disc type (and, on NTSC, the mandatory VIRS VITS) is declared once in the top-level `line_injections:` block:

| Field | Level | Required | Description |
|-------|-------|----------|-------------|
| `disc_type` | Project `line_injections` | Yes (laserdisc projects) | `CAV` or `CLV`; applies to every section |
| `vits` | Project `line_injections` | NTSC laserdisc: Yes (`virs`) | Project-wide VITS set applied to every frame of every section |

Every section with a laserdisc injection must then have:

| Field | Level | Required | Description |
|-------|-------|----------|-------------|
| `section_type` | Section | Yes | `lead_in`, `programme_area`, or `lead_out` |
| `type: laserdisc` | line_injection | Yes | Injection type selector |
| `codes` | laserdisc | Yes | List of at least one code type |
| `code_type` | code | Yes | The code type identifier (see tables above) |

### Optional Fields per Code Type

| Code type | Optional field | Default | Description |
|-----------|---------------|---------|-------------|
| `picture_number` / `fm_picture_number` | `start_value` | Continue from the previous section (1 on the first) | Re-anchors the picture number at this section |
| `chapter_number` | `chapter` | Continue the previous section's chapter (0 on the first) | Chapter number 0–79; setting it starts a new chapter |
| `programme_status` | `programme_status` | — | **Required**: 24-bit hex value with Hamming |
| `users_code` | `users_code` | — | **Required**: 24-bit hex value (X₁ = 0–7) |

### Validation Rules

The validator checks all of the following before generation begins; items marked **warning** are reported but do not block generation:

1. `section_type` must be `lead_in`, `programme_area`, or `lead_out`
2. `disc_type` must be `CAV` or `CLV`
3. Each `code_type` must be compatible with the `disc_type` and `section_type` (see matrix in §3)
4. `fm_*` code types require a System M standard (NTSC or PAL-M)
5. `picture_number` and `picture_stop` require CAV disc type
6. `programme_time_code`, `clv_code`, `clv_picture_number` require CLV disc type
7. `users_code` X₁ field must be 0–7
8. `picture_number` `start_value` must not exceed 99,999 (PAL) or 79,999 (NTSC, PAL-M); `fm_picture_number` `start_value` must not exceed 79,999. Only the start value is checked, not the incremented count
9. `chapter` must be 0–79
10. Lead-in duration ≥ 938 frames (CAV) at 1.6 µm pitch — **warning**, not checked for `duration_frames: "all"`
11. Lead-out duration ≥ 1250 frames (CAV) at 1.6 µm pitch — **warning**, not checked for `duration_frames: "all"`
12. Each chapter (a run of sections from one `chapter` setting to the next) ≥ 30 frames — **warning**, CAV only; chapters containing a `duration_frames: "all"` section are skipped, and CLV discs are not checked
13. Project VITS entries that target the biphase reserved range are rejected when the project declares a `disc_type`
14. NTSC and PAL-M: a `virs` VITS is required in the project-level `line_injections.vits` list (it is applied to every laserdisc section)
15. PAL: `vits17`, `itu-multiburst`, `itu-composite`, `itu-combination` are rejected on their standard lines in a laserdisc project (reserved range)
16. NTSC: `ntc7-composite`, `fcc-multiburst`, `ntc7-combination`, `fcc-composite` are rejected on their standard lines in a laserdisc project (reserved range)
17. `target_lines` must not be specified for laserdisc injections
18. VITC injections must not coexist with laserdisc injections in the same section (VITC is in any case rejected as not implemented)

---

## 11. Quick Reference

### Code Types at a Glance

| Code type | Disc | Standard | Section | Auto-inc | Key field |
|-----------|------|----------|---------|---------|-----------|
| `lead_in` | CAV/CLV | PAL/NTSC | lead_in | — | — |
| `lead_out` | CAV/CLV | PAL/NTSC | lead_out | — | — |
| `picture_number` | CAV | PAL/NTSC | programme | ✅ | `start_value` |
| `picture_stop` | CAV | PAL/NTSC | programme | — | — |
| `chapter_number` | CAV/CLV | PAL/NTSC | programme | — | `chapter` |
| `programme_status` | CAV/CLV | PAL/NTSC | programme | — | `programme_status` |
| `users_code` | CAV/CLV | PAL/NTSC | lead_in/out | — | `users_code` |
| `programme_time_code` | CLV | PAL/NTSC | programme | ✅ | — |
| `clv_code` | CLV | PAL/NTSC | programme | — | — |
| `clv_picture_number` | CLV | PAL/NTSC | programme | ✅ | — |
| `fm_picture_number` | CAV | NTSC/PAL-M | programme | ✅ | `start_value` |
| `fm_programme_time` | CLV | NTSC/PAL-M | programme | ✅ | — |
| `fm_white_flag` | CAV/CLV | NTSC/PAL-M | all | — | — |

### Section Type Compatibility Summary

```
lead_in:      lead_in, users_code, fm_white_flag (NTSC)
programme:    picture_number, picture_stop, chapter_number, programme_status,
              programme_time_code, clv_code, clv_picture_number,
              fm_picture_number, fm_programme_time, fm_white_flag (NTSC)
lead_out:     lead_out, users_code, fm_white_flag (NTSC)
```

### Minimum Duration Reference

| Standard | Section | Minimum frames | Basis |
|----------|---------|---------------|-------|
| PAL/NTSC/PAL-M | Lead-in (CAV, warning) | 938 | 1.5 mm ÷ 1.6 µm pitch |
| PAL/NTSC/PAL-M | Lead-out (CAV, warning) | 1250 | 2.0 mm ÷ 1.6 µm pitch |
| PAL/NTSC/PAL-M | Chapter (CAV, warning) | 30 | IEC §10.1.5 |

### VITS Compatibility with Laserdisc

#### PAL
| VITS type | Lines | Compatible? |
|-----------|-------|------------|
| `vits17` | 17 | ❌ Reject |
| `itu-multiburst` | 18 | ❌ Reject |
| `uk-national` | 19 | ✅ Allow |
| `vits20` | 20 | ✅ Allow |
| `itu-composite` | 330 | ❌ Reject |
| `itu-combination` | 331 | ❌ Reject |

#### NTSC
| VITS type | Lines | Compatible? |
|-----------|-------|------------|
| `ntc7-composite` | 17 | ❌ Reject |
| `fcc-multiburst` | 18 | ❌ Reject |
| `ntc7-combination` | 280 | ❌ Reject |
| `fcc-composite` | 281 | ❌ Reject |
| `virs` | 19, 282 | ✅ **Required** |

---

## 12. Troubleshooting

### Common Errors and Solutions

Messages are quoted as the validator emits them (without the `Laserdisc injection validation error:` / `Line injection validation error:` prefix).

#### `code_type 'picture_number' is not allowed in 'lead_in' sections.`

You have placed a programme-area code type in a lead-in section. Check the section type matrix in §3 and move the code type to the correct section.

---

#### `users_code X1 nibble must be 0-7; '0x88D000' has X1=8.`

The users_code hex value `0x8X₁DX₃X₄X₅` must have X₁ = 0–7, and its third nibble must be `D` (a separate error reports a wrong D nibble). Example: `"0x87D000"` is valid (X₁ = 7); `"0x88D000"` is invalid (X₁ = 8).

---

#### `picture_number start_value must be in the range 0-99999 for PAL; got 100000.`

`start_value` must lie within 0–99,999 on PAL or 0–79,999 on NTSC and PAL-M. Only the start value is checked; keep the section short enough that the incremented picture number stays within the maximum too.

---

#### `VITS target line 17 conflicts with laserdisc reserved VBI ranges for NTSC.`

In a laserdisc project (one that declares `disc_type`), no VITS may sit on lines 10–18 or 273–281 (NTSC/PAL-M) or 6–18 / 319–331 (PAL). Remove the incompatible VITS type or move it to a laserdisc VITS line (`placement: laserdisc`). If you need colour measurement, use `virs` on lines 19/282.

---

#### `NTSC and PAL-M laserdisc projects require a virs VITS injection for colour (IEC 60857 §9.1.3).`

NTSC and PAL-M laserdisc discs require a `virs` VITS on lines 19/282. It is declared once at the project level and applied to every section — add the following to the top-level `line_injections:` block (a sibling of `output:` and `sections:`):

```yaml
line_injections:
  disc_type: CAV        # or CLV
  vits:
    - vits_type: virs
      target_lines: [19, 282]
```

---

#### Warning: `CAV lead_out section has 500 frames; IEC 2 mm minimum at 1.6 um track pitch requires 1250.`

The lead-out section should be at least 1250 frames (2.0 mm at 1.6 µm pitch); the lead-in at least 938. This is a warning: generation still runs. Extend the section duration to comply.

---

#### Warning: `CAV chapter 2 starting in section 'Ch2' lasts 20 frames; IEC 60856/60857 requires at least 30.`

A CAV chapter is shorter than the IEC minimum of 30 tracks. Merge it with a neighbouring chapter or lengthen its sections.

---

#### `chapter_number chapter must be in the range 0-79; got 80.`

Chapter numbers are 0–79. Use a value in this range.

---

#### Timecodes appearing to reset between sections

If you start a new programme-area section and set `start_value: 1` again on `picture_number`, the timecode will appear to reset. Always continue from where the previous section left off. See §5 for the correct pattern.

---

#### `target_lines must not be specified for laserdisc injections.`

Laserdisc line placement is determined automatically by the standard. Remove the `target_lines` field from the `laserdisc` injection block.

---

*This document covers IEC 60856 (PAL Laservision) and IEC 60857 (NTSC Laservision) including Amendments 1 and 2.*
