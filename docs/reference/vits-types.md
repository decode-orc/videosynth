# VITS types

Every `vits_type` accepted in the project-level [`line_injections.vits`](line-injections.md) list.

Valid values depend on the video standard: PAL types are rejected in an NTSC project and vice versa. PAL-M projects use the NTSC catalogue. SECAM projects use the PAL catalogue, and any VITS on a SECAM project produces a warning:

```text
SECAM warning: VITS test signals are rendered with their PAL definitions,
including any PAL-modulated chroma packets.
```

Line numbers are 1-based and sequential across the whole frame, so PAL field 2 lines are 313–625 and NTSC field 2 lines are 263–525.

## PAL

| `vits_type` | Signal | Recommended line | Reference |
|-------------|--------|------------------|-----------|
| `vits17` | EBU/CCIR primary insertion test signal | 17 (field 1) | EBU/CCIR |
| `itu-multiburst` | ITU multiburst for PAL systems B, D, G, H, I | 18 (field 1) | ITU-R |
| `uk-national` | IBA/EBU UK national insertion test signal, PAL-I | 19 (field 1) | IBA/EBU |
| `vits20` | IBA/EBU UK ITS-2 chrominance amplitude reference | 20 (field 1) | IBA/EBU |
| `itu-composite` | ITU PAL composite insertion test signal | 330 (field 2) | ITU-R BT.628 / BT.473 |
| `itu-combination` | ITU combination insertion test signal for PAL | 331 (field 2) | ITU-R BT.473 |

## NTSC and PAL-M

| `vits_type` | Signal | Recommended line | Reference |
|-------------|--------|------------------|-----------|
| `ntc7-composite` | NTC-7 composite insertion test signal | 17 (field 1) | EIA RS-498 / SMPTE RP 168 |
| `fcc-multiburst` | FCC multi-burst insertion test signal | 18 (field 1) | FCC Part 73 / EIA RS-498 |
| `ntc7-combination` | NTC-7 combination insertion test signal | 280 (field 2) | EIA RS-498 / SMPTE RP 168 |
| `fcc-composite` | FCC composite insertion test signal | 281 (field 2) | FCC Part 73 / EIA RS-498 |
| `virs` | Video Index Reference Signal — format identification and level calibration | 19 / 282 (not enforced) | SMPTE RP 168 / EIA RS-498 |

## Placement

The `placement` policy decides which lines are accepted:

### `standard` (default)

Each type must sit on its recommended line, as listed above. Any other line is rejected. `virs` is the exception: it has no single recommended line, so `standard` placement accepts it on any valid line.

### `laserdisc`

VITS must sit on the laserdisc VBI lines, which are clear of the address-code ranges. Not available for `SECAM` projects.

| Standard | Permitted lines | Reference |
|----------|-----------------|-----------|
| PAL | 19, 20, 332, 333 | IEC 60856 §9.1.3 |
| NTSC / PAL-M | 19, 20, 282, 283 | IEC 60857 §9.1.3, §9.1.4 |

The spec-required sets, which the GUI seeds when you select this policy:

| Standard | Set |
|----------|-----|
| PAL | `uk-national` on 19/332, `vits20` on 20/333 |
| NTSC / PAL-M | `virs` on 19/282, `ntc7-composite` on 20, `ntc7-combination` on 283 |

### `custom`

Any valid VBI line. The overlap and reserved-range rules still apply.

## Compatibility with laserdisc codes

When `line_injections.disc_type` is set, no VITS may target a line in the reserved address-code ranges — PAL 6–18 and 319–331, NTSC/PAL-M 10–18 and 273–281. That rules out most of the broadcast lines.

### PAL

| `vits_type` | Line | On a disc |
|-------------|------|-----------|
| `vits17` | 17 | Rejected |
| `itu-multiburst` | 18 | Rejected |
| `uk-national` | 19 | Allowed |
| `vits20` | 20 | Allowed |
| `itu-composite` | 330 | Rejected |
| `itu-combination` | 331 | Rejected |

### NTSC and PAL-M

| `vits_type` | Line | On a disc |
|-------------|------|-----------|
| `ntc7-composite` | 17 | Rejected |
| `fcc-multiburst` | 18 | Rejected |
| `ntc7-combination` | 280 | Rejected |
| `fcc-composite` | 281 | Rejected |
| `virs` | 19, 282 | **Required** |

!!! important "NTSC discs must carry VIRS"
    IEC 60857 §9.1.3 makes the Video Index Reference Signal mandatory on NTSC laserdiscs. An NTSC or PAL-M project that sets `disc_type` and has no `virs` entry fails validation:

    ```text
    Line injection validation error: NTSC and PAL-M laserdisc projects require
    a virs VITS injection for colour (IEC 60857 §9.1.3).
    ```

## Noise measurement lines

Analysis tools measure White SNR from the white reference on **PAL frame line 19** or **NTSC frame line 20**. If a section uses `noise_spread_db` and no VITS targets that line, videosynth warns that the White SNR target will not be verifiable downstream. See [`noise`](section-noise.md).
