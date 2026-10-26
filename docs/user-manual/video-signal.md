# Video signal

This page covers the signal format decisions — everything under [`cvbs_presets`](../reference/cvbs-presets.md) and `output.signal_type`. These are project-wide: a capture file has one format, and there are no per-section overrides.

## Video standards

| `video_standard_preset` | Lines | Frame rate | Subcarrier | Active raster |
|--------------------------|-------|------------|------------|---------------|
| `PAL` | 625 | 25 fps | 4.43361875 MHz | 720×576 |
| `NTSC` | 525 | 30000/1001 fps (≈29.97) | 3.579545 MHz | 720×486 |
| `PAL_M` | 525 | 30000/1001 fps | 3.575611 MHz | 720×486 |
| `SECAM` | 625 | 25 fps | 4.40625 / 4.25 MHz, frequency modulated | 720×576 |

`PAL` means the 625/50 PAL family (PAL-B/G/H/I in baseband terms) and `NTSC` means NTSC-M as defined by SMPTE 170M. `PAL_M` is the Brazilian System M line structure with PAL colour encoding — a 525-line raster and 60 Hz field rate, but PAL's alternating-phase chroma.

`SECAM` is 625-line SECAM per ITU-R BT.1700 Annex 1 Part C: the same line timing, levels and raster as `PAL`, with [frequency-modulated chroma](#secam-chroma). MESECAM is a videotape recording method rather than a baseband standard; its decoded output is SECAM. PAL-N, PAL-60 and NTSC 4.43 are out of scope. Only baseband composite is generated; RF-layer differences between broadcast variants (PAL-I sound spacing, for example) are not modelled.

The output resolution is fixed by the standard and cannot be set in the project. Sources must already match it — videosynth does not scale.

!!! warning "The standard cannot be changed later"
    In the GUI the video standard is fixed once a project is created. Everything downstream depends on it: line counts, VBI allocation, which VITS types and laserdisc code types are legal, and the raster the sources must match.

## Sample encodings

`sample_encoding_preset` selects how millivolt-domain samples are quantised and written.

| Preset | Rate | Sample format | Notes |
|--------|------|---------------|-------|
| `CVBS_U10_4FSC` | 4fsc | Unsigned 16-bit holding 10-bit codes | The common choice; matches ld-decode-family tooling |
| `CVBS_U16_4FSC` | 4fsc | Unsigned 16-bit, 10-bit codes scaled to full range | |
| `CVBS_TPG21_4FSC` | 4fsc | Signed 16-bit, offset and scaled about mid-code | Test pattern generator interchange |
| `CVBS_S16_4FSC` | 4fsc | Signed 16-bit relative to blanking | Preserves sub-sync excursions |
| `RAW_S16_28M` | 28 MHz | Signed 16-bit millivolts | Resampled from the 4fsc lattice |
| `RAW_S16_40M` | 40 MHz | Signed 16-bit millivolts | Resampled from the 4fsc lattice |

**4fsc** means the sample clock is phase-locked to the colour subcarrier at four times its frequency: 17,734,475 Hz for PAL and 14,318,180 Hz for NTSC. SECAM has no single subcarrier and is stored on the PAL 4fsc lattice, as the CVBS file format specifies. NTSC is orthogonal at 4fsc — 910 samples × 525 lines = 477,750 samples per frame. PAL is not: it has 1135.0064 samples per line, so a frame is 709,379 samples — 1135 per line plus four extra samples per frame, two on line 313 and two on line 625. PAL-M has exactly 909 samples per line, 477,225 per frame.

Generation always happens on the 4fsc lattice. The raw presets are produced by resampling that lattice to the requested rate in the output stage.

### Which one to choose

Start with `CVBS_U10_4FSC`. It is what the decoding tool chain expects and it is the smallest of the 4fsc formats in practice.

Choose a **signed** preset — `CVBS_S16_4FSC` — or a raw preset when the signal contains **excursions below the 10-bit legal-code floor**. The clearest case is the PAL laserdisc pilot burst, whose trough reaches −600 mV, well under the −300 mV that the unsigned presets can represent. Generating a pilot-burst project with an unsigned preset is legal, and videosynth warns you that the burst trough is being clipped:

```text
[warning] pal_laserdisc_pilot_burst warning: preset 'CVBS_U10_4FSC' clips
sub-sync excursions below -300 mV; the pilot burst trough reaches -600 mV.
Use CVBS_S16_4FSC or RAW_S16_28M/RAW_S16_40M to preserve the full burst waveform.
```

### Signal levels

Quantisation follows the digital interface standards for each system:

| Standard | mV per code | Blanking code | Legal code range |
|----------|-------------|---------------|------------------|
| PAL / SECAM | 1.1905 | 256 | 4–1019 |
| NTSC / PAL-M | 1.2755 | 240 | 16–1019 |

Sub-black and over-white excursions inside the legal range are preserved rather than clipped to nominal black and white — a test signal that deliberately runs outside 0–100 IRE stays intact.

## Signal state

`signal_state_preset` must be `STANDARD_STABLE_LOCKED` for `PAL`, `NTSC` and `PAL_M`, and `STANDARD_STABLE_UNLOCKED` for `SECAM` (see [SECAM chroma](#secam-chroma)); leaving it out selects the right one. For the quadrature standards this declares the signal standard-rate (4×fsc), time-base stable, and phase locked — every sample sits at the standard subcarrier-reference-locked phase point for its (field, line, sample) position, which is what 4fsc generation produces by construction. The CVBS specification's unstable and unlocked presets describe imperfect captures; videosynth never emits one, so any value other than the one the standard requires is rejected at validation.

The metadata sidecar additionally declares `sequence_continuous = TRUE`: the synthesised signal is one unbroken sequence, with subcarrier phase and colour field sequence advancing continuously from the first frame to the last. Simulated disc skips (re-anchored picture numbers) change the VBI content only — they never break the signal's own continuity.

## Composite or Y/C

`output.signal_type` chooses the form of the written signal:

- **`composite`** (default) — luma and chroma summed into one signal, written to a single `.cvbs` file.
- **`yc`** — luma and chroma written separately, to `.cvbsy` (luma) and `.cvbsc` (chroma). The video path must end in `.cvbsy`; the chroma path is derived automatically.

Luma and chroma are generated independently regardless, and combined only at the output stage, so Y/C is a format choice and not a different signal. Y/C output is useful for exercising decoders that accept a pre-separated signal, and for isolating whether a decoding artefact comes from comb filtering or from something earlier.

## Standard-specific options

### PAL laserdisc pilot burst

```yaml
cvbs_presets:
  pal_laserdisc_pilot_burst: true
```

Superimposes a 3.75 MHz (240 × f_H) sinusoidal burst at ±300 mV on **every** sync pulse in the luma channel, per IEC 60856 §9.1.2. Laserdisc players use it as a tracking reference.

PAL only — enabling it on an NTSC project is a validation error. Pair it with a signed or raw sample encoding so the −600 mV trough survives quantisation.

### SECAM chroma

```yaml
cvbs_presets:
  video_standard_preset: SECAM
  signal_state_preset: STANDARD_STABLE_UNLOCKED
```

The `SECAM` preset generates 625-line SECAM chroma per ITU-R BT.1700 Annex 1 Part C. Line timing, levels, the active raster and the 4fsc sample lattice are those of `PAL`, and only the chrominance differs. The metadata file declares the `SECAM` preset. Because FM chroma has no fixed subcarrier phase, the file is declared `STANDARD_STABLE_UNLOCKED` rather than phase locked (see [`signal_state_preset`](../reference/cvbs-presets.md#signal_state_preset)).

| Stage | Behaviour | Reference |
|-------|-----------|-----------|
| Colour difference | D'R = −1.902 (R'−Y'), D'B = 1.505 (B'−Y'), sent alternately line by line | BT.1700 Part C Table 4 item 9 |
| Low-pass | ≤ 3 dB at 1.3 MHz, ≥ 30 dB at 3.5 MHz | item 10c |
| Video precorrection | (1 + j f/85 kHz) / (1 + j f/255 kHz) | item 10f |
| Frequency modulation | f_OR = 4.40625 MHz ± 280 kHz·D'R*, f_OB = 4.25 MHz ± 230 kHz·D'B*, limited to +350/−506 kHz (D'R) and +506/−350 kHz (D'B) | items 5, 10e |
| Bell filter | (1 + j16F) / (1 + j1.26F), F = f/f0 − f0/f, f0 = 4.286 MHz; 2·M0 = 23 % of 700 mV | item 10d |
| Subcarrier phase | 0°, 0°, 180° line pattern, inverted every frame | Table 4 Note 1 |
| Subcarrier blanking | From the start of the front porch to 5.6 µs after 0H, and in field blanking | BT.470-6 Table 2 item 2.17 |
| Line identification | Undeviated subcarrier on the back porch instead of a colour burst | item 13 |
| Field identification | Trapezoids on lines 7–15 and 320–328 reaching +350 kHz (D'R) and −350 kHz (D'B); disable with `secam_field_identification: false` | item 13, Figures 16 and 17 |

The D'R/D'B line sequence follows BT.1700 Figure 16 — line 7 of the first frame is a D'R line — and, because a frame has an odd number of lines, every line swaps component from one frame to the next. The sequence repeats every 2 frames.

OSD overlays carry the undeviated subcarrier under their text, as a colourless area of a SECAM picture does. The subcarrier phase restarts at the edges of the text, so a decoder may show a brief transient there.

SECAM projects cannot use the laserdisc features: there is no SECAM LaserVision format, so laserdisc line injections, `line_injections.disc_type`, laserdisc VITS placement, the pilot burst and EFM digital audio are validation errors.

!!! note "Limitations"
    VITS test signals use the 625-line PAL catalogue, so any chroma packet in them is PAL-modulated; a SECAM project with VITS produces a validation warning. The luminance intermodulation correction that BT.470 allows for SECAM is not applied.

### NTSC laserdisc VBI burst

The NTSC counterpart to the pilot burst, inserting colour burst on the equalising and broad sync pulses (IEC 60857 §9.1.2).

!!! warning "Not implemented"
    `ntsc_laserdisc_vbi_burst` is parsed and its NTSC-only scope is checked, but the signal behaviour does not exist yet. Setting it to `true` is a validation error rather than a silent no-op. Leave it out.

### NTSC black setup

```yaml
cvbs_presets:
  ntsc_black_setup_ire: 7.5   # or 0.0
```

NTSC (and PAL-M) place black at 7.5 IRE above blanking by default — the "setup" pedestal. Setting `0.0` puts black at blanking instead, which is how Japanese NTSC and much digital-origin material behave.

Only `7.5` and `0.0` are accepted, and only on NTSC or PAL-M projects. Specifying it on a PAL project is a validation error.

## Field and line structure

Fields are generated in the standard's own order and dominance; there are no project options for field order, dominance or endianness. Sample data is little-endian.

Line numbering in the project file is **1-based and sequential across the whole frame**: PAL lines 1–625, NTSC lines 1–525. Field 2 lines therefore carry high numbers — PAL line 330 is in field 2, not "line 17 of field 2". This matters when writing VITS `target_lines`.
