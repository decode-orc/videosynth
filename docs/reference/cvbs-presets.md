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
| `video_standard_preset` | string | Yes | `PAL`, `NTSC`, `PAL_M`, `SECAM` | — |
| `sample_encoding_preset` | string | No | See below | `CVBS_U10_4FSC` |
| `signal_state_preset` | string | No | `STANDARD_STABLE_LOCKED`; `STANDARD_STABLE_UNLOCKED` for `SECAM` | Per standard (see below) |
| `pal_laserdisc_pilot_burst` | bool | No | `true`, `false` | `false` |
| `ntsc_laserdisc_vbi_burst` | bool | No | `true`, `false` | `false` |
| `ntsc_black_setup_ire` | float | No | `7.5`, `0.0` | `7.5` |
| `secam_field_identification` | bool | No | `true`, `false` | `true` |

No other key is accepted. In particular there are no `mode`, `field_order`, `field_dominance` or `endianness` keys — field order and dominance follow the standard, and sample data is little-endian.

## `video_standard_preset`

| Value | Lines | Frame rate | Subcarrier | Raster |
|-------|-------|------------|------------|--------|
| `PAL` | 625 | 25 fps | 4.43361875 MHz | 720×576 |
| `NTSC` | 525 | 30000/1001 fps | 3.579545 MHz | 720×486 |
| `PAL_M` | 525 | 30000/1001 fps | 3.575611 MHz | 720×486 |
| `SECAM` | 625 | 25 fps | 4.40625 MHz (D′R) / 4.25 MHz (D′B), frequency modulated | 720×576 |

`PAL-M` is accepted as a spelling of `PAL_M`.

`SECAM` has the same line and field timing, signal levels, active raster and 4fsc storage lattice as `PAL`; only the colour encoding differs. It has no single colour subcarrier, so the CVBS file format adopts the PAL 4fsc rate as its standard rate (CVBS File Format Specification, Preset: `SECAM`). See [SECAM chroma](../user-manual/video-signal.md#secam-chroma) for what is generated.

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

4fsc rates are 17,734,475 Hz (PAL and SECAM) and 14,318,180 Hz (NTSC/PAL-M). Frames are 709,379 samples (PAL and SECAM) and 477,750 samples (NTSC/PAL-M).

**Rule**: 4fsc generation requires a 4fsc encoding *and* the `signal_state_preset` the standard requires (see below).

**Warning**: enabling `pal_laserdisc_pilot_burst` with `CVBS_U10_4FSC`, `CVBS_U16_4FSC` or `CVBS_TPG21_4FSC` clips the burst trough at −300 mV, where the waveform reaches −600 mV. Use `CVBS_S16_4FSC` or a raw preset.

**Warning**: enabling `ntsc_laserdisc_vbi_burst` with `CVBS_U10_4FSC`, `CVBS_U16_4FSC` or `CVBS_TPG21_4FSC` clips the colour burst on the broad field-sync pulses at sync tip (−40 IRE), where the waveform reaches −60 IRE. Use `CVBS_S16_4FSC` or a raw preset.

## `signal_state_preset`

| Standard | Required value |
|----------|----------------|
| `PAL`, `NTSC`, `PAL_M` | `STANDARD_STABLE_LOCKED` |
| `SECAM` | `STANDARD_STABLE_UNLOCKED` |

`STANDARD_STABLE_LOCKED` means standard 4×fsc sample rate, time-base stable, and phase locked — the content is sampled at the standard subcarrier-reference-locked phase points, which synthetic generation satisfies by construction.

SECAM chrominance is frequency modulated, so its subcarrier phase depends on picture content and the CVBS file format defines no Sc/H convention for it. A `SECAM` file must not be declared phase locked; generated SECAM output is time-base stable at the standard rate and is declared `STANDARD_STABLE_UNLOCKED`.

When the key is omitted, the required value for the standard is used. Any other value is rejected: the remaining presets describe captures that are unstable or at a non-standard rate, which videosynth never produces.

Because the generated signal is one unbroken sequence — subcarrier phase and colour field sequence advance continuously with the output frame position — the metadata file always declares `sequence_continuous = TRUE` (see [outputs](../user-manual/outputs.md)).

## `pal_laserdisc_pilot_burst`

Superimposes a 3.75 MHz (240 × f_H) sinusoidal burst at ±300 mV on every sync pulse in the luma channel — IEC 60856 §9.1.2.

**PAL only.** Enabling it on an NTSC, PAL-M or SECAM project is a validation error.

## `ntsc_laserdisc_vbi_burst`

Keeps the colour burst on every line of the vertical interval, including the equalising and broad field-sync lines (lines 1–9 and 264–272) where standard NTSC suppresses it — IEC 60857 §9.1.2 and Figure 6a. Each burst is the normal NTSC burst: same gate position after 0H, amplitude and continuous subcarrier phase. On the broad field-sync lines the gate falls inside the broad pulse, so the burst is centred on sync tip.

**NTSC only.** Enabling it on a PAL, PAL-M or SECAM project is a validation error.

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

## `secam_field_identification`

Emits the SECAM field identification signals ("bottles") on lines 7–15 and 320–328 — ITU-R BT.1700 Annex 1 Part C Table 4 item 13. Only meaningful with `video_standard_preset: SECAM`; set it to `false` to generate line identification only. Setting it to `false` on another standard is accepted but produces a warning:

```text
secam_field_identification has no effect unless video_standard_preset is SECAM.
```
