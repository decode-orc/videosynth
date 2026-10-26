# VITS — vertical interval test signals

Vertical interval test signals are reference waveforms inserted on VBI lines so that a signal path can be measured from the picture itself. videosynth renders the standard PAL and NTSC families from their published waveform definitions.

## VITS are project-wide

VITS live in the **top-level** `line_injections:` block and are applied to every frame of every section:

```yaml
line_injections:
  vits:
    - vits_type: vits17
      target_lines: [17]
```

This is deliberate. A signal path's reference signals do not change halfway through a recording, and on a disc they are a property of the disc rather than of the lead-in or the programme. Declaring them once means they cannot accidentally differ between sections.

There is no section-level `vits` injection type; a section injection with `type: vits` is a validation error pointing you to the top-level block.

## Available types

Valid `vits_type` values depend on the video standard. PAL types are accepted in PAL and SECAM projects, and NTSC types in NTSC and PAL-M projects.

### PAL

| `vits_type` | Signal | Recommended line |
|-------------|--------|------------------|
| `vits17` | EBU/CCIR primary insertion test signal | 17 |
| `itu-multiburst` | ITU multiburst for PAL B, D, G, H, I | 18 |
| `uk-national` | IBA/EBU UK national insertion test signal (PAL-I) | 19 |
| `vits20` | IBA/EBU UK ITS-2 chrominance amplitude reference | 20 |
| `itu-composite` | ITU PAL composite ITS (BT.628/BT.473) | 330 |
| `itu-combination` | ITU combination ITS for PAL (BT.473) | 331 |

### NTSC and PAL-M

| `vits_type` | Signal | Recommended line |
|-------------|--------|------------------|
| `ntc7-composite` | NTC-7 composite ITS (EIA RS-498 / SMPTE RP 168) | 17 |
| `fcc-multiburst` | FCC multiburst (FCC Part 73 / EIA RS-498) | 18 |
| `ntc7-combination` | NTC-7 combination ITS | 280 |
| `fcc-composite` | FCC composite ITS | 281 |
| `virs` | Video Index Reference Signal (SMPTE RP 168) | None fixed — conventionally 19 / 282 |

Remember that line numbers are sequential across the whole frame, so 280–331 are field 2 lines.

## Placement policy

`line_injections.placement` decides how strictly the validator constrains each entry's `target_lines`.

### `standard` (default)

Each VITS type must sit on its broadcast recommended line, as listed above. Any other line is rejected. `virs` has no fixed line, so it may go on any line. This is what you want for broadcast-style test material, and it is what projects written before the field existed get.

```yaml
line_injections:
  placement: standard          # or simply omit it
  vits:
    - vits_type: vits17
      target_lines: [17]
```

In the GUI, VITS lines are read-only under this policy — the line is a property of the signal type, not a choice.

### `laserdisc`

On a laserdisc the broadcast VITS lines collide with the reserved address-code ranges, so IEC 60856/60857 place the test signals elsewhere:

| Standard | Permitted VITS lines |
|----------|----------------------|
| PAL | 19, 20, 332, 333 (IEC 60856 §9.1.3) |
| NTSC / PAL-M | 19, 20, 282, 283 (IEC 60857 §9.1.3, §9.1.4) |

Selecting this policy in the GUI seeds the spec-required set for the standard:

- **PAL** — `uk-national` on 19/332, `vits20` on 20/333.
- **NTSC / PAL-M** — `virs` on 19/282, `ntc7-composite` on 20, `ntc7-combination` on 283.

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

### `custom`

VITS may sit on any valid VBI line. The validator still rejects overlapping lines, and when `line_injections.disc_type` is set it still rejects the reserved address-code ranges.

Use this when you are deliberately producing non-conforming material — testing how a decoder copes with a signal on the wrong line, for instance.

## VITS and laserdisc together

The laserdisc address codes reserve whole VBI ranges, and no VITS may target a line inside them when the project sets `line_injections.disc_type`:

| Standard | Field 1 | Field 2 |
|----------|---------|---------|
| PAL | 6–18 | 319–331 |
| NTSC / PAL-M | 10–18 | 273–281 |

That rules out most of the broadcast VITS lines on a disc:

**PAL**

| `vits_type` | Line | On a disc |
|-------------|------|-----------|
| `vits17` | 17 | Rejected |
| `itu-multiburst` | 18 | Rejected |
| `uk-national` | 19 | Allowed |
| `vits20` | 20 | Allowed |
| `itu-composite` | 330 | Rejected |
| `itu-combination` | 331 | Rejected |

**NTSC / PAL-M**

| `vits_type` | Line | On a disc |
|-------------|------|-----------|
| `ntc7-composite` | 17 | Rejected |
| `fcc-multiburst` | 18 | Rejected |
| `ntc7-combination` | 280 | Rejected |
| `fcc-composite` | 281 | Rejected |
| `virs` | 19, 282 | **Required** |

!!! important "NTSC discs must carry VIRS"
    IEC 60857 §9.1.3 makes the Video Index Reference Signal mandatory on NTSC laserdiscs. An NTSC or PAL-M project that sets `line_injections.disc_type` and has no `virs` entry in the project-level VITS set is a validation error.

## VITS and noise measurement

If a section uses `noise_spread_db` — the two-component noise model, where white is noisier than black — the White SNR figure can only be verified from the picture if there is a VITS carrying a white reference on the measurement line: PAL frame line 19, NTSC frame line 20.

videosynth warns when spread noise is configured without such a VITS. The signal is generated correctly either way; the warning is about whether an analysis tool will be able to measure what you asked for. See [Impairments](impairments.md).
