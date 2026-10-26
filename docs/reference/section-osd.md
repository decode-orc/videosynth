# `sections[].osd`

On-screen display overlays burnt into the picture. Optional.

```yaml
    osd:
      overlays:
        - text: "PN:{picture_number} HEX:{biphase_hex}"
          x: 8
          y: 32
          scale: 1
          fg_luma: white
          bg_luma: black
```

## Keys

| Key | Type | Required | Description |
|-----|------|----------|-------------|
| `overlays` | list | No | One entry per text overlay. An `osd:` block without it draws nothing |

No other key is accepted. A section may carry any number of overlays.

## `overlays[]`

| Key | Type | Required | Range / Values | Default |
|-----|------|----------|----------------|---------|
| `text` | string | No | Literal text, optionally with tokens | empty (draws nothing) |
| `x` | int | No | Active-area pixels | `0` |
| `y` | int | No | Active-area pixels | `0` |
| `scale` | int | No | 1–4 | `1` |
| `fg_luma` | string | No | `white`, `light_grey`, `dark_grey`, `black` | `white` |
| `bg_luma` | string | No | `transparent`, `white`, `light_grey`, `dark_grey`, `black` | `transparent` |

No other key is accepted.

## Luma levels

| Value | Level |
|-------|-------|
| `white` | 1.0 |
| `light_grey` | 0.75 |
| `dark_grey` | 0.25 |
| `black` | 0.0 |
| `transparent` | Background only — writes nothing, leaving the picture visible |

Only the **luma** channel is written; chroma is untouched, so overlays are monochrome. `white` on `black` is the most legible combination over arbitrary picture content.

## Tokens

| Token | Resolves to | When unavailable |
|-------|-------------|------------------|
| `{picture_number}` | CAV picture number for this frame, 5 digits zero-padded | `00000` |
| `{biphase_hex}` | 6-digit uppercase hex biphase code words, space separated | `000000` |
| `{phase_id}` | Colour-frame sequence index (0–3 PAL and PAL-M, 0–1 NTSC and SECAM) | — |
| `{section_name}` | The section's `name:` verbatim | — |
| `{timecode}` | CLV programme timecode `HH:MM:SS:FF` | `00:00:00:00` |
| `{frame_number}` | 1-based position in the whole output, 5 digits zero-padded | — |

Unavailable values render at the **same width** as a real value, so overlay layout never shifts between frames.

An unknown token name is a validation error rather than being rendered literally:

```text
OSD validation error in section 'Programme': overlay text contains unknown
token '{pn}'. Supported tokens: {picture_number}, {biphase_hex}, {phase_id},
{section_name}, {timecode}, {frame_number}.
```

### Token sources

`{picture_number}` and `{biphase_hex}` come from the VBI codes actually written into this frame, captured after biphase injection. They follow whatever the disc is doing — restarting or jumping where a section re-anchors `start_value`, and reading zeros on a project with no laserdisc codes.

`{frame_number}` and `{timecode}` are **positional**: they count from the start of the generated output and are never re-anchored by a section. `{timecode}` runs continuously at the standard's CLV frame rate on any CLV disc, regardless of which codes a section injects.

## Rendering

- Static 96-glyph 8×8 bitmap font covering printable ASCII 0x20–0x7F.
- Each glyph pixel becomes a `scale` × `scale` block.
- Rendered after active video and after VBI injection, so tokens report what the frame actually carries.
- Rendered once per field, at the same y-offset within each field's active picture, so text is stable rather than interlace-flickering.
- Pixels outside the active picture area are silently clipped.
