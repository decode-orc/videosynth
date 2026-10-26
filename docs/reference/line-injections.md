# `line_injections` (project level)

The project-wide VBI settings: the disc format and the VITS set applied to every frame of every section. Optional — omit it for a non-laserdisc project with no test signals.

This is a **sibling of `output:` and `sections:`**, not a key inside them. Do not confuse it with the per-section `line_injections:` list, which is a different shape entirely — see [section `line_injections`](section-line-injections.md).

```yaml
line_injections:
  disc_type: CAV
  placement: laserdisc
  vits:
    - vits_type: uk-national
      target_lines: [19, 332]
    - vits_type: vits20
      target_lines: [20, 333]
```

## Keys

| Key | Type | Required | Values | Default |
|-----|------|----------|--------|---------|
| `disc_type` | string | No | `CAV`, `CLV` | absent (not a disc) |
| `placement` | string | No | `standard`, `laserdisc`, `custom` | `standard` |
| `vits` | list | No | VITS entries (below) | empty |

No other key is accepted.

## `disc_type`

The laserdisc format for the whole project. A disc is entirely one or the other, which is why this is project-wide and is *not* repeated on each section's `laserdisc` injection.

| Value | Addressing |
|-------|------------|
| `CAV` | Picture number — one frame per track |
| `CLV` | Programme time code plus CLV picture number |

It determines which `code_type` values the validator accepts, and setting it turns on the disc VITS rules below (reserved address-code lines, and VIRS on NTSC/PAL-M) whether or not any section carries codes. It is required as soon as any section carries a `laserdisc` injection. Omit it entirely for projects that are not discs. `SECAM` projects cannot set it.

## `placement`

How strictly the validator constrains each VITS entry's `target_lines`.

| Value | Rule |
|-------|------|
| `standard` | Each VITS type must sit on its broadcast recommended line. Any other line is rejected. `virs` has no single recommended line and is not constrained |
| `laserdisc` | VITS must sit on the laserdisc VBI lines — PAL 19, 20, 332, 333; NTSC/PAL-M 19, 20, 282, 283. Not available for `SECAM` |
| `custom` | Any valid VBI line, subject to the overlap and reserved-range rules |

`standard` is also assumed when the key is absent, which preserves the behaviour of projects written before it existed.

Under `custom` the validator still rejects overlapping lines, and when `disc_type` is set it still rejects the reserved address-code ranges.

## `vits[]`

Each entry is a test signal applied to every frame of every section.

| Key | Type | Required | Description |
|-----|------|----------|-------------|
| `vits_type` | string | Yes | The waveform — see [VITS types](vits-types.md) |
| `target_lines` | list of int | Yes | 1-based frame lines, sequential across the whole frame |

Line numbers run across the whole frame, so PAL field 2 lines are 313–625 and NTSC field 2 lines are 263–525. `vits20` on line 20 is a field 1 line; on line 333 it is the field 2 equivalent.

Valid `vits_type` values depend on the video standard; PAL types are rejected in NTSC projects and vice versa. `SECAM` projects use the PAL types, with a warning that they are rendered with their PAL definitions.

### Rules

- Lines must be within the standard's range (1–625 PAL, 1–525 NTSC/PAL-M).
- Every entry needs at least one line, and no line may appear twice, within or between entries.
- Under `standard` placement, each type must be on its recommended line (`virs` excepted).
- Under `laserdisc` placement, lines must be from that standard's laserdisc VITS set.
- When `disc_type` is set, no VITS may target a reserved address-code line (PAL 6–18, 319–331; NTSC/PAL-M 10–18, 273–281).
- When `disc_type` is set on an NTSC or PAL-M project, a `virs` entry is **required** (IEC 60857 §9.1.3).

## Why VITS are project-wide

A signal path's reference signals do not change halfway through a recording, and on a disc they are a property of the disc rather than of the lead-in or the programme area. Declaring them once removes any way for them to accidentally differ between sections.

There is no section-level `vits` injection type.
