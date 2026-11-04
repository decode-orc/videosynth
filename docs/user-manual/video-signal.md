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

Choose a **signed** preset — `CVBS_S16_4FSC` — or a raw preset when the signal contains **excursions below the 10-bit legal-code floor**. The clearest case is the PAL laserdisc pilot burst, whose trough reaches −600 mV, well under the −300 mV that the unsigned presets can represent. The NTSC laserdisc VBI burst is the other: on the broad field-sync pulses it reaches −60 IRE, below sync tip. Generating either with an unsigned preset is legal, and videosynth warns you that the burst trough is being clipped:

```text
[warning] laserdisc.pilot_burst warning: preset 'CVBS_U10_4FSC' clips
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

SECAM projects cannot use the LaserDisc features: there is no SECAM LaserVision format, so the project and section [`laserdisc:`](../reference/laserdisc.md) blocks and laserdisc VITS placement are validation errors.

!!! note "Limitations"
    VITS test signals use the 625-line PAL catalogue, so any chroma packet in them is PAL-modulated; a SECAM project with VITS produces a validation warning. The luminance intermodulation correction that BT.470 allows for SECAM is not applied.

### NTSC black setup

```yaml
cvbs_presets:
  ntsc_black_setup_ire: 7.5   # or 0.0
```

NTSC (and PAL-M) place black at 7.5 IRE above blanking by default — the "setup" pedestal. Setting `0.0` puts black at blanking instead, which is how Japanese NTSC and much digital-origin material behave.

Only `7.5` and `0.0` are accepted, and only on NTSC or PAL-M projects. Specifying it on a PAL project is a validation error.

## LaserDisc signal options

Two LaserVision signal features are set in the project's [`laserdisc:`](../reference/laserdisc.md) block rather than in `cvbs_presets`, with the rest of the LaserDisc settings. A project that is not a disc can ignore this section. See [Laserdisc discs](laserdisc.md) for the rest of a disc project.

### PAL pilot burst

```yaml
laserdisc:
  pilot_burst: true
```

Superimposes a 3.75 MHz (240 × f_H) sinusoidal burst at ±300 mV on **every** sync pulse in the luma channel, per IEC 60856 §9.1.2. Laserdisc players use it as a tracking reference.

PAL only — enabling it on an NTSC project is a validation error. Pair it with a signed or raw sample encoding so the −600 mV trough survives quantisation.

### NTSC VBI burst

```yaml
laserdisc:
  vbi_burst: true
```

Standard NTSC suppresses the colour burst on the equalising and broad sync lines of the vertical interval. An NTSC LaserDisc keeps it there: IEC 60857 §9.1.2 requires the burst on every line, once per line at its normal position after 0H (Figure 6a). With this option the burst continues through lines 1–9 and 264–272 with the same amplitude and the same continuous subcarrier phase as every other line. The burst is added in the chroma channel, so on the equalising lines it sits at blanking and on the broad field-sync lines, where the gate falls inside the broad pulse, it is centred on sync tip.

NTSC only — enabling it on a PAL, PAL-M or SECAM project is a validation error. The burst on the broad pulses dips to −60 IRE, below the NTSC 10-bit legal-code floor at sync tip, so pair it with a signed or raw sample encoding to keep the whole waveform; an unsigned preset clips it and produces a warning.

## Active picture

Each active line takes one row of the source picture. The upper field (field 1 on 625-line systems, field 2 on 525-line) reads the even rows and the other field the odd rows. The 720 source pixels are mapped onto the 52 µs active window at 4fsc, the luma is painted as millivolts, and the two colour-difference components are low-pass filtered (33-tap Hamming-windowed 1.3 MHz for PAL and PAL-M, 17-tap 1.2 MHz for NTSC) and modulated onto the subcarrier. Three section keys change that path, and one output option records its result.

### Horizontal mapping

By default each 4fsc sample takes the nearest source pixel at or to its left — a zero-order hold. The hold leaves images of the source spectrum across the band. A source component at *f* reappears at 13.5 MHz − *f*, folded back by the 4fsc sampling: low-frequency picture detail in a PAL picture leaves energy around 4.4 MHz, which a chroma decoder rightly reads as cross-colour.

[`horizontal_resample: bandlimited`](../reference/sections.md#horizontal_resample) replaces the hold with a polyphase Kaiser-windowed sinc at the source Nyquist frequency. It removes those images and the hold's in-band droop, for luma and chroma alike, and leaves flat areas and edge positions where the hold puts them. Use it for still-image decoder testing, where the cross-colour a decoder produces should come from the picture and not from the mapping.

A [`luma_bandwidth`](../reference/sections.md#luma_bandwidth) limit removes the images above its cutoff, but not the droop, and it does not touch the chroma.

### Picture response

Real programme pictures are softer than a sharp still. A camera's aperture, a caption generator and the production chain all roll off fine detail before it reaches the disc. On the BBC Domesday discs, luma above 3.5 MHz carries a tenth of the share that sharp stills such as the bundled maps and text card carry, and there is far less detail between rows. A still that stands in for programme material should be softened the same way, or a decoder is rewarded for recovering detail no disc has.

[`picture_response`](../reference/sections.md#picture_response) gives a section that softening, horizontally and vertically:

```yaml
  - name: TownMap
    type: progressive
    source: "{bundled}/exr/720x576/OSM_TOWN.exr"
    duration_frames: 25
    picture_response: domesday
```

- `domesday` is the measured Domesday camera and PAL chain: about −2 dB from 2.5 to 4 MHz, falling to −10 dB by 5.25 MHz, and −10 dB above 3/8 cycle per frame row. Custom `horizontal_db` and `vertical_db` curves describe any other chain.
- The vertical part acts on the progressive picture's rows before it is split into fields, as an interlaced camera would.
- Only the picture luma is shaped, and the [picture truth](#picture-truth) includes it. Leave stress patterns (zone plates, multi-bursts, edges) without the block: testing the limits is their purpose.

### PAL decoder-stress regions

A PAL decoder removes the V-switch line by line. A [`chroma_line_sign`](../reference/sections.md#chroma_line_sign) region multiplies its U or V by each line's switch sign before modulation, which builds the PAL delay-line and demodulator traps of cards such as the Philips PM5544:

- **`v: follow_switch`** — the transmitted V carrier is unswitched, so the decoded V alternates line by line. A correct delay-line decoder shows grey; delay-line amplitude errors show as "Venetian blinds" and a non-orthogonal demodulator colours the bar. The unswitched V lands where U would be in the vertical spectrum, so a 2D or Transform PAL decoder cannot separate it from U by position alone.
- **`u: follow_switch`** — U alternates with the switch: the "+V/±U anti-PAL" field.

The switch sign belongs to the line and flips every frame for a given frame line, so this cannot be drawn into a still image. The bundled [`PAL_STRESS.exr`](sources.md#stills-exr) card carries the bars; its regions are set in the `pal_decoder_stress.yaml` [example project](../misc/example-projects.md#decoder-stress-and-zone-plates).

### Picture truth

[`output.truth: true`](../reference/output.md#truth) writes the Y, U and V that went into the modulator — after the horizontal mapping, the picture response, the luma bandwidth limit, the chroma low-pass and any chroma line sign regions, and before any impairment — as planar `float32` at 4fsc over the active window, with a SQLite sidecar like the other `.meta` files. A decoder's output can then be scored against exactly what was encoded rather than against a re-derivation of videosynth's filters, which could drift silently from them. PAL, NTSC and PAL-M only; OSD text is not included. See [Output files](outputs.md#picture-truth).

## Field and line structure

Fields are generated in the standard's own order and dominance; there are no project options for field order, dominance or endianness. Sample data is little-endian.

Line numbering in the project file is **1-based and sequential across the whole frame**: PAL lines 1–625, NTSC lines 1–525. Field 2 lines therefore carry high numbers — PAL line 330 is in field 2, not "line 17 of field 2". This matters when writing VITS `target_lines`.
