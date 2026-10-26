# Laserdisc code types

Every `code_type` accepted in a section's [`line_injections[].codes`](section-line-injections.md), with its hex format, where it is legal and what it needs.

`disc_type` (CAV/CLV) is project-wide; `section_type` is per section. Together they decide which code types are accepted. A section that carries a `laserdisc` injection must set `section_type`.

There is no SECAM LaserVision format, so a `SECAM` project rejects every laserdisc code, `disc_type` and `placement: laserdisc`:

```text
Project configuration error: laserdisc line injections,
line_injections.disc_type and laserdisc VITS placement are not available for
SECAM projects.
```

"All" in the table below therefore means PAL, NTSC and PAL-M.

## At a glance

| `code_type` | Disc | Standard | Section | Auto-increments | Field |
|-------------|------|----------|---------|-----------------|-------|
| `lead_in` | CAV/CLV | All | `lead_in` | — | — |
| `lead_out` | CAV/CLV | All | `lead_out` | — | — |
| `picture_number` | CAV | All | `programme_area` | ✅ | `start_value` |
| `picture_stop` | CAV | All | `programme_area` | — | — |
| `chapter_number` | CAV/CLV | All | `programme_area` | — | `chapter` |
| `programme_status` | CAV/CLV | All | `programme_area` | — | `programme_status` |
| `users_code` | CAV/CLV | All | `lead_in`, `lead_out` | — | `users_code` |
| `programme_time_code` | CLV | All | `programme_area` | ✅ | — |
| `clv_code` | CLV | All | `programme_area` | — | — |
| `clv_picture_number` | CLV | All | `programme_area` | ✅ | — |
| `fm_picture_number` | CAV | NTSC/PAL-M | `programme_area` | ✅ | `start_value` |
| `fm_programme_time` | CLV | NTSC/PAL-M | `programme_area` | ✅ | — |
| `fm_white_flag` | CAV/CLV | NTSC/PAL-M | all | — | — |

## Fields

Every output frame is one picture, and its first field is always field 1. IEC 60856/60857 clause 10 places some codes in the first field of a picture and the rest in the other field, so in the programme area:

- **Field 1** carries the addressing codes — `picture_number` on CAV, `programme_time_code` and `clv_picture_number` on CLV.
- **Field 2** carries the other codes — `picture_stop`, `chapter_number`, `programme_status`, `clv_code`.
- On CAV, `programme_status` is written on line 16 of field 1 as well.

If a section carries none of the field 1 codes, the field 2 codes are written in both fields.

Lead-in and lead-out codes are written in both fields.

Field 2 lines are 313 higher than field 1 on PAL (16/17/18 → 329/330/331) and 263 higher on NTSC/PAL-M (16/17/18 → 279/280/281).

---

## `lead_in`

| Property | Value |
|----------|-------|
| Hex format | `88FFFF` |
| Section | `lead_in` only |
| Lines (PAL) | 17, 18, 330, 331 |
| Lines (NTSC) | 17, 18, 280, 281 |

Fixed code marking the lead-in area. No parameters.

```yaml
- code_type: lead_in
```

## `lead_out`

| Property | Value |
|----------|-------|
| Hex format | `80EEEE` |
| Section | `lead_out` only |
| Lines (PAL) | 17, 18, 330, 331 |
| Lines (NTSC) | 17, 18, 280, 281 |

Fixed code marking the lead-out area. No parameters.

```yaml
- code_type: lead_out
```

---

## `picture_number` (CAV)

| Property | Value |
|----------|-------|
| Hex format | `FX₁X₂X₃X₄X₅` |
| Section | `programme_area` |
| Lines (PAL) | 17, 18 (field 1) |
| Lines (NTSC) | 17, 18 (field 1) |
| Range | 0–99 999 (PAL) / 0–79 999 (NTSC, PAL-M) |
| Digits | X₁–X₅ each 0–9 |

The frame address on a CAV disc, incrementing by 1 every frame.

`start_value` is optional:

- **Omit it** to continue from the previous section. The counter runs continuously across every section; a disc-wide sequential numbering needs `start_value` on the *first* programme section only, or nowhere at all (numbering then begins at 1).
- **Set it** to re-anchor the count for that section — a deliberate discontinuity, for a skip or replay.

```yaml
- code_type: picture_number
  start_value: 1
```

## `picture_stop` (CAV)

| Property | Value |
|----------|-------|
| Hex format | `82CFFF` |
| Section | `programme_area` |
| Lines (PAL) | 329, 330 (field 2) |
| Lines (NTSC) | 279, 280 (field 2) |

Tells the player to freeze on this frame. Written in field 2, after the picture-number field, where it takes priority over `programme_status` on line 329/279 and over `chapter_number` on line 330/280. Without a `picture_number` in the section it is written in both fields.

```yaml
- code_type: picture_stop
```

## `chapter_number` (CAV and CLV)

| Property | Value |
|----------|-------|
| Hex format | `8X₁X₂DDD` |
| Section | `programme_area` |
| Range | 0–79 |
| Minimum length | 30 tracks (checked on CAV, warning) |
| Lines (CAV PAL) | 331 (field 2); also 330 when there is no `picture_stop` |
| Lines (CLV PAL) | 331 (field 2) |
| Lines (CAV NTSC) | 281 (field 2); also 280 when there is no `picture_stop` |
| Lines (CLV NTSC) | 281 (field 2) |

`chapter` is optional:

- **Omit it** to continue the previous section's chapter, carrying its stop-bit track counter across the boundary. If no earlier section set one, numbering begins at 0.
- **Set it** to start a new chapter here, restarting the stop-bit counter (IEC 60856/60857 §10.1.5).

```yaml
- code_type: chapter_number
  chapter: 3
```

On a CAV disc a chapter shorter than 30 frames produces a warning. A chapter starts at the first section that carries `chapter_number` and at each section that sets `chapter`; later sections that carry `chapter_number` without `chapter` add to it. A chapter that includes a `duration_frames: all` section has no known length and is not checked, and CLV chapters are not checked.

```text
Laserdisc injection warning: CAV chapter 3 starting in section 'Chapter3'
lasts 25 frames; IEC 60856/60857 requires at least 30.
```

### The stop-bit

Encoded in the MSB of X₁, and computed for you:

| Position within the chapter | Stop-bit |
|-----------------------------|----------|
| Tracks 0–399 | 0 — the player's search passes through |
| Track 400 onwards | 1 — search may stop here |
| Any track of a chapter shorter than 800 tracks | 1 |
| The first chapter after the lead-in | Always 1 |

```text
X₁ = (stop_bit << 3) | (chapter / 16)
X₂ = chapter % 16
```

Chapter 5 with stop-bit 1 encodes as `885DDD`.

## `programme_status` (CAV and CLV)

| Property | Value |
|----------|-------|
| Hex format | `8DC/BA X₃X₄X₅` |
| Section | `programme_area` |
| Lines (CAV) | 16 (field 1) and 329/279 (field 2, unless `picture_stop`) |
| Lines (CLV) | 329/279 (field 2) |
| Horizontal start | 0.172H into the active line |

Audio/video mode and copy permission, with a SECDED Hamming check digit.

| Field | Meaning |
|-------|---------|
| Nibbles 1–2 | `DC` = CX noise reduction on, `BA` = off |
| X₃ | Disc size, disc side, teletext presence, copy permission |
| X₄ | Audio/video mode — 0–3 standard, 8 = mono dump |
| X₅ | Hamming check, computed automatically |

```yaml
- code_type: programme_status
  programme_status: "0x8DC000"    # CX on, copy prohibited, stereo
```

Without a `programme_status` value the code is not written.

The GUI's **Programme Status Code** dialog composes this word from readable fields, which is a great deal easier than assembling it by hand.

## `users_code`

| Property | Value |
|----------|-------|
| Hex format | `8X₁DX₃X₄X₅` |
| Section | `lead_in` or `lead_out` only |
| X₁ | 0–7 only |
| Third nibble | Must be `D` |
| Lines (PAL) | 16, 329 |
| Lines (NTSC) | 16, 279 |

Disc-specific metadata for the manufacturer.

```yaml
- code_type: users_code
  users_code: "0x80D234"
```

Without a `users_code` value the code is not written. X₁ values of 8–F are reserved and rejected, as is any third nibble other than `D`:

```text
Laserdisc injection validation error: users_code X1 nibble must be 0-7;
'0x88D234' has X1=8.
```

---

## `programme_time_code` (CLV)

| Property | Value |
|----------|-------|
| Hex format | `FX₁DDX₂X₃` |
| Section | `programme_area` |
| Lines (PAL) | 17, 18 (field 1) |
| Lines (NTSC) | 17, 18 (field 1) |
| X₁ | 0–F (hours 0–15) |
| X₂ | 0–5 (minutes tens, BCD) |
| X₃ | 0–9 (minutes units, BCD) |

Elapsed programme time in hours and minutes. Starts at `0:00` and advances automatically — minutes increment every 1500 frames (PAL) or 1800 (NTSC and PAL-M). There is nothing to configure.

```yaml
- code_type: programme_time_code
```

## `clv_code` (CLV)

| Property | Value |
|----------|-------|
| Hex format | `87FFFF` |
| Section | `programme_area` |
| Lines (PAL) | 330 (field 2) |
| Lines (NTSC) | 280 (field 2) |
| Horizontal start | 0.172H into the active line (NTSC and PAL-M only) |

A fixed marker identifying the disc as CLV, written in field 2. Without a `programme_time_code` or `clv_picture_number` in the section it is written in both fields.

```yaml
- code_type: clv_code
```

## `clv_picture_number` (CLV)

| Property | Value |
|----------|-------|
| Hex format | `8X₁EX₃X₄X₅` |
| Section | `programme_area` |
| Lines (PAL) | 16 (field 1) |
| Lines (NTSC) | 16 (field 1) |
| X₁ | A–F |
| X₃ | 0–9 |
| X₄ | 0–2 |
| X₅ | 0–9 |

The frame position within the current second (X₄×10 + X₅ = frame 0–24 on PAL, 0–29 on NTSC and PAL-M) and the second offset within a 60-second block (X₁, X₃). Advances every frame, automatically.

On NTSC and PAL-M it includes colour time error correction (IEC 60857 Amendment 2 §10.1.10): the seconds count jumps forward at specific frame counts to keep colour timing accurate.

```yaml
- code_type: clv_picture_number
```

---

## NTSC 40-bit FM codes

These are NTSC and PAL-M only, and are mandatory on a compliant NTSC disc alongside the 24-bit biphase codes. The two systems occupy different lines within the reserved range and are generated simultaneously.

### `fm_picture_number` (NTSC CAV)

| Property | Value |
|----------|-------|
| Signal | 40-bit FM |
| Section | `programme_area` |
| Lines | 10, 273 |
| Range | 0–79 999 |
| Update | Second field of each new picture |

Carries the CAV picture number in the 40-bit FM format, advancing with the 24-bit `picture_number`.

`start_value` (0–79 999) is optional and seeds the counter, which otherwise starts at 1. Only the first section carrying `fm_picture_number` seeds it; in later sections the counter continues and `start_value` is not applied.

```yaml
- code_type: fm_picture_number
```

### `fm_programme_time` (NTSC CLV)

| Property | Value |
|----------|-------|
| Signal | 40-bit FM |
| Section | `programme_area` |
| Lines | 10, 273 |

Carries CLV programme time in the 40-bit FM format. The X₅ mode indicator distinguishes the disc area: A = lead-in, B = end of lead-in, D = programme, C = lead-out.

```yaml
- code_type: fm_programme_time
```

### `fm_white_flag` (NTSC)

| Property | Value |
|----------|-------|
| Signal | 100 IRE constant level, 0.790H long, starting 0.160H from sync |
| Section | `lead_in`, `programme_area`, `lead_out` |
| Lines (lead-in) | 11 |
| Lines (programme) | 11 |
| Lines (lead-out) | 11 and 274 |

Marks the start of each new picture, which is how a player decides what a "frame" is when holding a still. Every output frame is a picture that opens on field 1, so in the lead-in and programme area the flag is on line 11 of every frame.

```yaml
- code_type: fm_white_flag
```

---

## Priority

Most codes have a line and field of their own (see [Fields](#fields)). Where two code types share a line, the standard fixes the order:

1. Lead-in and lead-out codes take absolute priority in their sections.
2. `picture_stop` beats `programme_status` on line 329/279 (CAV).
3. `picture_stop` beats `chapter_number` on line 330/280 (CAV).

## Horizontal timing

`programme_status` (all standards) and `clv_code` (NTSC and PAL-M) begin **0.172H** into the active line — IEC 60856 Figure 14, IEC 60857 Figure 11. All other code types begin at the start of the active line. The offset is applied automatically.

## Further reading

The repository carries the full bit layouts, encoding rules and worked examples in [`docs-tech/user/biphase-design.md`](https://github.com/decode-orc/videosynth/blob/main/docs-tech/user/biphase-design.md){target="_blank"}.
