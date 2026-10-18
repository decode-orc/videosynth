# `cvbs_presets`

The signal format for the whole project. Required. There are no per-section overrides — a capture file has one format.

```yaml
cvbs_presets:
  video_standard_preset: PAL
  sample_encoding_preset: CVBS_S16_4FSC
  signal_state_preset: STANDARD_STABLE_LOCKED
  pal_laserdisc_pilot_burst: true
```

## Keys

| Key | Type | Required | Values | Default |
|-----|------|----------|--------|---------|
| `video_standard_preset` | string | Yes | `PAL`, `NTSC`, `PAL_M` | — |
| `sample_encoding_preset` | string | No | See below | `CVBS_U10_4FSC` |
| `signal_state_preset` | string | No | `STANDARD_STABLE_LOCKED` | `STANDARD_STABLE_LOCKED` |
| `pal_laserdisc_pilot_burst` | bool | No | `true`, `false` | `false` |
| `ntsc_laserdisc_vbi_burst` | bool | No | `true`, `false` | `false` |
| `ntsc_black_setup_ire` | float | No | `7.5`, `0.0` | `7.5` |
| `chroma_encoding` | string | No | `STANDARD`, `SECAM` | `STANDARD` |
| `secam_field_identification` | bool | No | `true`, `false` | `true` |

No other key is accepted. In particular there are no `mode`, `field_order`, `field_dominance` or `endianness` keys — field order and dominance follow the standard, and sample data is little-endian.

## `video_standard_preset`

| Value | Lines | Frame rate | Subcarrier | Raster |
|-------|-------|------------|------------|--------|
| `PAL` | 625 | 25 fps | 4.43361875 MHz | 720×576 |
| `NTSC` | 525 | 30000/1001 fps | 3.579545 MHz | 720×486 |
| `PAL_M` | 525 | 30000/1001 fps | 3.575611 MHz | 720×486 |

`PAL-M` is accepted as a spelling of `PAL_M`.

Only one standard per project. The output resolution follows from it and must not be specified anywhere in the file.

## `sample_encoding_preset`

| Value | Rate | Sample format |
|-------|------|---------------|
| `CVBS_U10_4FSC` | 4fsc | Unsigned, 10-bit codes |
| `CVBS_U16_4FSC` | 4fsc | Unsigned 16-bit |
| `CVBS_TPG21_4FSC` | 4fsc | Signed 16-bit, offset and scaled about mid-code |
| `CVBS_S16_4FSC` | 4fsc | Signed 16-bit relative to blanking; preserves sub-sync excursions |
| `RAW_S16_28M` | 28 MHz | Signed 16-bit millivolts, resampled from 4fsc |
| `RAW_S16_40M` | 40 MHz | Signed 16-bit millivolts, resampled from 4fsc |

4fsc rates are 17,734,475 Hz (PAL) and 14,318,180 Hz (NTSC/PAL-M). Frames are 709,379 samples (PAL) and 477,750 samples (NTSC/PAL-M).

**Rule**: 4fsc generation requires a 4fsc encoding *and* a time-base stable, phase-locked `signal_state_preset`.

**Warning**: enabling `pal_laserdisc_pilot_burst` with an unsigned preset clips the burst trough at −300 mV, where the waveform reaches −600 mV. Use `CVBS_S16_4FSC` or a raw preset.

## `signal_state_preset`

Must be `STANDARD_STABLE_LOCKED`: standard 4×fsc sample rate, time-base stable, and phase locked — the content is sampled at the standard subcarrier-reference-locked phase points, which synthetic generation satisfies by construction. The other presets defined by the CVBS file format specification describe captures that are unstable or not subcarrier-locked; videosynth never produces such output, so any other value is rejected.

Because the generated signal is one unbroken sequence — subcarrier phase and colour field sequence advance continuously with the output frame position — the metadata file always declares `sequence_continuous = TRUE` (see [outputs](../user-manual/outputs.md)).

## `pal_laserdisc_pilot_burst`

Superimposes a 3.75 MHz (240 × f_H) sinusoidal burst at ±300 mV on every sync pulse in the luma channel — IEC 60856 §9.1.2.

**PAL only.** Enabling it on an NTSC or PAL-M project is a validation error.

## `ntsc_laserdisc_vbi_burst`

Inserts colour burst on the equalising and broad sync pulses — IEC 60857 §9.1.2.

!!! warning "Not implemented — enabling it is an error"
    The key is parsed and its NTSC-only scope is checked, but the runtime signal behaviour does not exist. Setting it to `true` is a validation error:

    ```text
    Project configuration error: ntsc_laserdisc_vbi_burst is parsed but not
    implemented in the current runtime.
    ```

    Leave it out, or set it to `false`.

## `ntsc_black_setup_ire`

| Value | Effect |
|-------|--------|
| `7.5` | Standards-based NTSC setup — black sits 7.5 IRE above blanking |
| `0.0` | Black at blanking (0 IRE), as in Japanese NTSC and digital-origin material |

**NTSC and PAL-M only.** Specifying it on a PAL project is a validation error, and any value other than `7.5` or `0.0` is rejected.

```text
Project configuration error: ntsc_black_setup_ire can only be specified for
NTSC or PAL-M projects.
```

## `chroma_encoding`

| Value | Effect |
|-------|--------|
| `STANDARD` | The preset's own colour system: PAL, NTSC or PAL-M quadrature modulation with colour burst |
| `SECAM` | 625-line SECAM frequency-modulated chroma — ITU-R BT.1700 Annex 1 Part C |

**`SECAM` requires `video_standard_preset: PAL`.** The CVBS file format has no SECAM video standard preset; SECAM shares the 625-line, 50 Hz line structure, signal levels and the 4fsc PAL sample lattice, so the file is written as a `PAL` preset and only the chrominance differs. Combining it with another preset, or with `pal_laserdisc_pilot_burst`, is a validation error.

With `SECAM` the PAL colour burst is replaced by the undeviated SECAM subcarrier on the back porch (line identification), and the picture chroma is the line-sequential D'R/D'B FM signal described in the [video signal](../user-manual/video-signal.md#secam-chroma) page.

## `secam_field_identification`

Emits the SECAM field identification signals ("bottles") on lines 7–15 and 320–328 — ITU-R BT.1700 Annex 1 Part C Table 4 item 13. Only meaningful with `chroma_encoding: SECAM`; set it to `false` to generate line identification only. Setting it to `false` without SECAM is accepted but produces a warning.
