# On-screen display

Any section can burn text into the picture. The intended use is diagnostic: putting the picture number, the timecode or the section name on screen so that a frame can be identified by eye or by an automated check, without having to decode the VBI.

```yaml
sections:
  - name: Bars
    type: progressive
    source: "{bundled}/exr/720x576/100_BARS.exr"
    duration_frames: 100
    osd:
      overlays:
        - text: "PN:{picture_number} HEX:{biphase_hex} PHASE:{phase_id}"
          x: 8
          y: 32
          scale: 1
          fg_luma: white
          bg_luma: black
```

## How it is rendered

- A static 96-glyph 8×8 bitmap font covering printable ASCII (0x20–0x7F), derived from the IBM PC BIOS VGA font.
- Each glyph pixel becomes a `scale` × `scale` block of output pixels.
- **Luma only.** The chroma channel is untouched, so overlays are monochrome regardless of what is underneath.
- Rendered **last** in per-frame generation, after the active picture and after VBI injection, so tokens can report what was actually written into the VBI of that frame.
- Rendered **twice per frame**, once per field, at the same y-offset within each field's active picture, so the text is stable rather than interlace-flickering.
- Pixels falling outside the active picture area are silently clipped.

## Overlay fields

| Key | Type | Range | Meaning |
|-----|------|-------|---------|
| `text` | string | — | Literal text, optionally containing substitution tokens |
| `x` | int | — | Horizontal position in active-area pixels |
| `y` | int | — | Vertical position in active-area pixels |
| `scale` | int | 1–4 | Glyph magnification |
| `fg_luma` | enum | `white`, `light_grey`, `dark_grey`, `black` | Text level |
| `bg_luma` | enum | `transparent`, `white`, `light_grey`, `dark_grey`, `black` | Background level; `transparent` writes no background (default) |

The four luma steps are full white (1.0), light grey (0.75), dark grey (0.25) and black (0.0). A `black` background behind `white` text is the most legible combination over arbitrary picture content.

A section may carry any number of overlays.

## Tokens

| Token | Resolves to |
|-------|-------------|
| `{picture_number}` | The CAV picture number written into this frame's VBI, zero-padded to five digits |
| `{biphase_hex}` | The 6-digit uppercase hex biphase code words for this frame, space separated |
| `{phase_id}` | The colour-frame sequence index — 0–3 for PAL, 0–1 for NTSC |
| `{section_name}` | The section's `name:` field, verbatim |
| `{timecode}` | CLV programme timecode `HH:MM:SS:FF` |
| `{frame_number}` | 1-based position of this frame in the whole output, zero-padded to five digits |

Tokens are resolved from the state the biphase encoder captured for that exact frame, so a burnt-in picture number and the VBI code it reports cannot disagree. That is the point of resolving them after injection rather than before.

### Two kinds of counter

`{picture_number}` and `{biphase_hex}` come from the VBI codes, so they follow whatever the disc is doing — they restart or jump when a section re-anchors `start_value`, and they read all zeros on a project with no laserdisc codes.

`{frame_number}` and `{timecode}` are **positional**. They count from the start of the generated output and are never re-anchored by a section. `{timecode}` runs continuously at the standard's CLV frame rate (25 fps PAL, 30 fps NTSC and PAL-M) on any CLV disc, regardless of which codes a particular section injects, and reads `00:00:00:00` on non-CLV projects.

Putting both on screen is the quickest way to see where a disc's addressing diverges from its physical position — exactly the situation a player skip or a replayed passage creates.

### Missing values

A token whose value is unavailable renders as an all-zero field of the **same width** as a real value, so the overlay layout never shifts between frames. A picture number that is not present reads `00000`, not an empty gap.

Unknown token names are rejected at validation time, so a typo fails the project rather than appearing literally on screen.

## Examples

Identify every frame by disc address and by output position:

```yaml
    osd:
      overlays:
        - text: "PN:{picture_number}"
          x: 16
          y: 24
          scale: 2
          fg_luma: white
          bg_luma: black
        - text: "OUT:{frame_number} {section_name}"
          x: 16
          y: 48
          scale: 1
          fg_luma: light_grey
          bg_luma: black
```

Show the raw VBI codes for debugging a decoder:

```yaml
    osd:
      overlays:
        - text: "{biphase_hex}"
          x: 8
          y: 8
          scale: 1
          fg_luma: white
          bg_luma: black
```

Timecode burn-in on a CLV disc:

```yaml
    osd:
      overlays:
        - text: "{timecode}"
          x: 240
          y: 500
          scale: 2
          fg_luma: white
          bg_luma: dark_grey
```
