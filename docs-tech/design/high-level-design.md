# **VideoSynth**

**Technical Design Specification**

---

## **Table of Contents**

1. [Overview](#overview)
2. [Core Requirements](#core-requirements)
3. [Architecture Overview](#architecture-overview)
4. [Generation Stage](#generation-stage)
5. [Output Stage](#output-stage)
6. [PAL and NTSC Analogue Specifications](#pal-and-ntsc-analogue-specifications)
  - [6.1. Signal Levels](#61-signal-levels)
  - [6.2. Half-Line Element Definitions](#62-half-line-element-definitions)
  - [6.3. PAL Non-Visible Line Structures](#63-pal-non-visible-line-structures)
  - [6.4. NTSC Non-Visible Line Structures](#64-ntsc-non-visible-line-structures)
7. [YAML Project File Specification](#yaml-project-file-specification)
8. [Section Types](#section-types)
  - [8.1. Frame-Based Sections](#81-frame-based-sections)
  - [8.2. Line Injections](#82-line-injections)
9. [Field and Line Handling](#field-and-line-handling)
10. [4fsc Sampling and Subcarrier Locking](#4fsc-sampling-and-subcarrier-locking)
11. [VBI Line Allocation](#vbi-line-allocation)
12. [Implementation Pipeline](#implementation-pipeline)
13. [Error Handling and Validation](#error-handling-and-validation)
14. [CLI Interface](#cli-interface)
15. [Build and Packaging](#build-and-packaging)
16. [Directory Structure](#directory-structure)
17. [Future Requirements](#future-requirements)
18. [Appendix: References](#appendix-references)

---

---

## **1. Overview**

### **Specification Cross-Check (Section 1)**

- This section is a product-summary section; normative technical detail is expanded and clause-traced in Sections 2-13.
- PAL/NTSC studio-signal scope aligns with [ITU-R BT.1700](../analogue-video-specifications/docs/video_formats/BT-1700-E/BT-1700-E.md) (Scope and Part A/Part B) and [SMPTE 170M-2004](../analogue-video-specifications/docs/video_formats/SMPTE-170M-2004/SMPTE-170M-2004.md) §1.1.
- Laserdisc feature scope aligns with [IEC 60856](../analogue-video-specifications/docs/laserdisc/IEC-60856-1986-Laservision-PAL/IEC-60856-1986-Laservision-PAL.md) §9.1/§10 and [IEC 60857](../analogue-video-specifications/docs/laserdisc/IEC-60857-1986-Laservision-NTSC/IEC-60857-1986-Laservision-NTSC.md) §9.1/§10.
- VITC scope aligns with [IEC 60461:2010](../analogue-video-specifications/docs/video_metadata/IEC-60461-2010-Time-and-control-code/IEC-60461-2010-Time-and-control-code.md) Clause 9 (vertical interval application).

### **Purpose**

**VideoSynth** is a **C++17 application** designed to generate **PAL or NTSC video signals** in the **CVBS file format**. It adheres closely to the **analogue video standards** and is built for **video engineers and media preservation professionals**.

### **Key Features**

- **Time-based signal generation** for PAL and NTSC, closely following analogue standards.
- Support for multiple sample rates: **4fsc**, **20MSPS**, **40MSPS**, or custom.
- **Frame-based content** from:
  - Progressive sources: EXR stills and MKV/MP4 video, validated for raster and frame rate and colour-normalised to BT.601 studio swing.
- **Line-based injections** for VBI content:
  - **VITS** (Vertical Interval Test Signals).
  - **Laserdisc biphase encoding** (IEC 60856/60857).
  - **VITC** (Vertical Interval Timecode, SMPTE 12M).
- **Per-section noise injection**: two-component Gaussian noise model (floor + proportional) targeting orc-gui Black PSNR and White SNR metrics.
- **Per-section dropout injection**: physical-tape and optical-disc dropout simulation (random surface dropouts and persistent scratch dropouts) with automatic sidecar generation conformant to the [Dropout Extension Format](../cvbs-file-format-specification/docs/extensions/dropout-extension-format.md) (schema version 5).

### **Target Users**

- Video engineers.
- Video media preservation developers and engineers.

### **Implementation Details**

- **Language**: C++17.
- **Build System**: Nix.
- **Logging**: [spdlog](https://github.com/gabime/spdlog).
- **Sample Rate Conversion**: [soxr](https://sourceforge.net/p/soxr/).
- **Testing**: [Google Test](https://github.com/google/googletest).

---

---

## **2. Core Requirements**

### **Specification Cross-Check (Section 2)**

- `525`/`625` lines per frame and nominal field rates are sourced from [ITU-R BT.470-6](../analogue-video-specifications/docs/video_formats/BT-470-6-1998/BT-470-6-1998.md), Annex 1, Table 1 (items 1-2), and [SMPTE 170M-2004](../analogue-video-specifications/docs/video_formats/SMPTE-170M-2004/SMPTE-170M-2004.md) §1.1 and §11.3.
- NTSC subcarrier `3.579545 MHz` is sourced from [SMPTE 170M-2004](../analogue-video-specifications/docs/video_formats/SMPTE-170M-2004/SMPTE-170M-2004.md) §11.1 and [ITU-R BT.470-6](../analogue-video-specifications/docs/video_formats/BT-470-6-1998/BT-470-6-1998.md), Table 2 item 2.11(a).
- PAL subcarrier `4.43361875 MHz` is sourced from [ITU-R BT.470-6](../analogue-video-specifications/docs/video_formats/BT-470-6-1998/BT-470-6-1998.md), Table 2 item 2.11(a).
- `4fsc` sample-rate values are sourced from [SMPTE 244M-2003](../analogue-video-specifications/docs/video_formats/SMPTE-244M-2003/SMPTE-244M-2003.md) §3.1/§3.4 (NTSC, 14.31818 MHz) and [EBU Tech. 3280-E](../analogue-video-specifications/docs/video_formats/EBU-Tech-3280-E/EBU-Tech-3280-E.md) §1.1.1 (PAL, 17.734475 MHz).

### **Supported Standards**

All timing, signal levels, and encoding parameters for **PAL** and **NTSC** are defined in the following specifications:

- [ITU-R BT.470-6](../analogue-video-specifications/docs/video_formats/BT-470-6-1998/BT-470-6-1998.md): Conventional Television Systems.
- [ITU-R BT.1700](../analogue-video-specifications/docs/video_formats/BT-1700-E/BT-1700-E.md): Composite Video Signal Characteristics.
- [SMPTE 170M-2004](../analogue-video-specifications/docs/video_formats/SMPTE-170M-2004/SMPTE-170M-2004.md): Composite Analog Video Signal for NTSC.
- [SMPTE 244M-2003](../analogue-video-specifications/docs/video_formats/SMPTE-244M-2003/SMPTE-244M-2003.md): Bit-Parallel Digital Interface for NTSC.
- [EBU Tech. 3280-E](../analogue-video-specifications/docs/video_formats/EBU-Tech-3280-E/EBU-Tech-3280-E.md): 625-Line Digital PAL Interfaces.

### **PAL/NTSC Variant Scope (Normative for this Specification)**

To avoid ambiguity, this specification uses **PAL** and **NTSC** with the following fixed meanings:

- **PAL** means the **625/50 PAL family with 4.43361875 MHz subcarrier** (PAL-B/G/H/I timing family in baseband composite terms).
- **NTSC** means **NTSC-M 525/59.94 with 3.579545 MHz subcarrier** as defined by SMPTE 170M.
- **SECAM** means **625/50 SECAM** with frequency-modulated chrominance on $f_{OR}$ = 4.40625 MHz / $f_{OB}$ = 4.25 MHz as defined by ITU-R BT.1700 Annex 1 Part C.

The following are **out of scope** for this specification unless explicitly added in a future revision:

- PAL-N, PAL-60, NTSC 4.43, and other variants outside 625/50 PAL, 625/50 SECAM and NTSC-M (PAL-M — 525/59.94 with the System M line structure and 3.575611 MHz subcarrier — **is** supported; see also [SECAM Chroma Encoding Model](#secam-chroma-encoding-model)). MESECAM is a videotape recording method whose decoded baseband output is SECAM, so it needs no preset of its own.
- RF transmission/channel-plan differences (for example PAL-I RF sound spacing), because this document defines **baseband CVBS generation** rather than broadcast RF modulation.

Practical interpretation:

- If a user asks for "PAL-I", VideoSynth uses the same baseband 625/50 PAL composite timing and levels defined here; PAL-I-specific RF-layer differences are not modeled.


| **Standard** | **Accepted Frame-Based Source Dimensions** | **Frame Rate** | **Field Rate** | **Colour Subcarrier** | **Lines/Frame** | **Reference**   |
| ------------ | ------------------------------------------- | -------------- | -------------- | --------------------- | --------------- | --------------- |
| PAL (625/50 family) | 720x576 | 25 fps         | 50 Hz          | 4.43361875 MHz        | 625             | ITU-R BT.470-6  |
| NTSC-M (525/59.94)  | 720x486 | ~29.97 fps     | ~59.94 Hz      | 3.579545 MHz          | 525             | SMPTE 170M-2004 |
| SECAM (625/50)      | 720x576 | 25 fps         | 50 Hz          | 4.40625 / 4.25 MHz (FM) | 625           | ITU-R BT.1700 Annex 1 Part C |

For file-based frame sources, ingestion is strict and fail-closed: only the exact supported raster for the selected standard is accepted.


### **Output Modes**

Current implementation status:

- **Locked**: Implemented. The runtime requires `signal_state_preset: STANDARD_STABLE_LOCKED` for PAL, NTSC and PAL-M.
- **Unlocked**: Implemented for SECAM only. SECAM chroma is frequency modulated and the CVBS File Format Specification v1.7.0 defines no Sc/H convention for it, so generated SECAM output (time-base stable at the standard rate) is declared `STANDARD_STABLE_UNLOCKED`, and that is the only state accepted for SECAM. Unlocked output for the quadrature standards is target design only.

### **Sample Rates**


Current implementation status:

- **4fsc**: Implemented for PAL and NTSC.
- **20MSPS / 40MSPS / Custom**: Design targets only; not implemented in the current runtime.
- The current validator and output path accept `CVBS_U10_4FSC`, `CVBS_U16_4FSC`, `CVBS_TPG21_4FSC`, `RAW_S16_28M`, and `RAW_S16_40M`. The 4fsc encodings remain locked to the 4fsc lattice, while raw encodings are resampled by the output stage.


| **Sample Rate** | **PAL (Hz)** | **NTSC (Hz)** | **Notes**                        |
| --------------- | ------------ | ------------- | -------------------------------- |
| 4fsc            | 17,734,475   | 14,318,180    | Locked to subcarrier (optional). |
| 20MSPS          | 20,000,000   | 20,000,000    | Oversampling.                    |
| 40MSPS          | 40,000,000   | 40,000,000    | Oversampling.                    |
| Custom          | User-defined | User-defined  | Must be > 4fsc.                  |

At `4fsc`, NTSC frame cadence is orthogonal (`910 x 525 = 477,750` samples/frame). PAL is non-orthogonal at `4fsc` and uses `709,379` samples/frame via a distributed one-sample slip pattern (`1135` nominal samples/line with four lines per frame at `1134`).


---

---

## **3. Architecture Overview**

### **Specification Cross-Check (Section 3)**

- The current implementation uses a sampled-domain split: frame-scoped CVBS-domain Y/C synthesis directly on the `4fsc` lattice, followed by composite quantisation and file output. This remains consistent with composite-signal decomposition in [SMPTE 170M-2004](../analogue-video-specifications/docs/video_formats/SMPTE-170M-2004/SMPTE-170M-2004.md) §3, §7-§10 and [ITU-R BT.1700](../analogue-video-specifications/docs/video_formats/BT-1700-E/BT-1700-E.md) Part A/Part B, but it is not a continuous-time then sampled split.
- Independent luma/chroma generation with later composition maps to luminance/chrominance model definitions in [SMPTE 170M-2004](../analogue-video-specifications/docs/video_formats/SMPTE-170M-2004/SMPTE-170M-2004.md) §6-§10 and [ITU-R BT.470-6](../analogue-video-specifications/docs/video_formats/BT-470-6-1998/BT-470-6-1998.md), Table 2 item 2.5.

VideoSynth currently follows a **four-stage sampled-domain architecture**:

1. **[Generation Stage](#generation-stage)**: Synthesizes **4fsc-discrete fixed-point mV representations** of **luma (Y)** and **chroma (C)** for complete frame batches.
2. **Noise Injection Stage** (`NoiseInjectionStage`): Applies optional per-section two-component Gaussian noise to the fixed-point mV Y/C buffers before quantisation, targeting orc-gui Black PSNR and White SNR metrics.
3. **Dropout Injection Stage** (`DropoutInjectionStage`): Applies optional per-section random and scratch dropout events to the fixed-point mV Y/C buffers after noise, and writes a conformant SQLite dropout sidecar (`<basename>.dropouts.meta`).
4. **[Output Stage](#output-stage)**: Validates frame-span alignment, combines Y and C, quantises to the active digital interface code space, and formats the final CVBS output files.

Alongside these four stages, an optional **Audio Track Generator** (`AudioTrackGenerator`) runs when at least one section declares an `audio:` channel pair. It synthesises per-channel test-tone waveforms (`AudioSynthesizer`) frame-locked to the video and streams one stereo 24-bit PCM RIFF/WAVE track per declared channel pair (0–7) to `<basename>_audio_<pair>.wav` (each via an `AudioWavWriter`). Audio is a pure function of output position, so the generator is driven with the output-order section sequence and stays sample-accurately aligned to the stored video frames. See [Section 7 `audio:`](#audio-sub-key-optional-per-section).

When `output.efm_audio` selects one of those channel pairs, the same generator additionally synthesises that pair at 44 100 Hz and streams it through the `videosynth_efm` module (`AudioEfmWriter`) as a LaserDisc digital audio EFM channel stream written to `<basename>.efm` with its own `<basename>.efm.meta` sidecar. The pair's 48 kHz WAV track is unaffected. See [LaserDisc Digital Audio (EFM) Output](#laserdisc-digital-audio-efm-output).

### **Key Principles**

- **Independent Y and C Generation**: Luma and chroma are generated separately and combined only at the output stage.
- **Sampled-Domain Generation**: Signals are synthesized directly on the locked `4fsc` sample lattice using analogue-derived timing and level parameters.
- **Analogue Parameterization in the Digital Domain**: Pulse widths, burst placement, signal levels, and active-picture apertures are derived from PAL/NTSC analogue specifications but realized directly as digital sample sequences.
- **Analogue Compliance**: Strict adherence to PAL/NTSC standards for timing, sync pulses, and colour encoding.
- **Noise After Generation**: Noise is injected after the generation stage and before quantisation, so all synthesised regions (VBI lines, active picture, blanking) receive noise and it is preserved in the final 10-bit output codes.

---

---

## **4. Generation Stage**

### **Specification Cross-Check (Section 4)**

- BT.601 source-domain assumptions (YCbCr construction/quantization and studio swing conventions) are grounded in [BT.601-5](../analogue-video-specifications/docs/video_formats/BT-601-5-1995/BT-601-5-1995.md) §3.5.1-§3.5.4 and Part A tables for 4:4:4 coding.
- NTSC colour encoding/filtering references are grounded in [SMPTE 170M-2004](../analogue-video-specifications/docs/video_formats/SMPTE-170M-2004/SMPTE-170M-2004.md) §6 (matrices), §7 (filtering), §8 (subcarrier modulation).
- PAL chroma sideband limits used for encoder bandwidth targets are grounded in [ITU-R BT.470-6](../analogue-video-specifications/docs/video_formats/BT-470-6-1998/BT-470-6-1998.md), Table 2 item 2.12.
- Sync/burst insertion basis is grounded in [ITU-R BT.1700](../analogue-video-specifications/docs/video_formats/BT-1700-E/BT-1700-E.md), Table 2/Table 3 and Figures 1-9, plus [SMPTE 170M-2004](../analogue-video-specifications/docs/video_formats/SMPTE-170M-2004/SMPTE-170M-2004.md) §13.
- PAL pilot burst and NTSC VBI-burst behavior are grounded in [IEC 60856](../analogue-video-specifications/docs/laserdisc/IEC-60856-1986-Laservision-PAL/IEC-60856-1986-Laservision-PAL.md) §9.1.2 and [IEC 60857](../analogue-video-specifications/docs/laserdisc/IEC-60857-1986-Laservision-NTSC/IEC-60857-1986-Laservision-NTSC.md) §9.1.2.

### **Key Principle: Frame-Based Generation**

**All generation is frame-scoped.** A frame (two sequential fields) is the **smallest synthesis unit** in the generation stage, and frames are emitted downstream in bounded frame batches for streaming output.

However, **line timing and source-row addressing inside that frame are field-aware by definition**. This is required to keep interlaced field sequence and active-picture placement compliant with the NTSC/PAL timing models.

For avoidance of doubt, the generator shall apply the following rules:

- **NTSC active-picture line starts (1-indexed frame lines):** field 1 starts at line **22**; field 2 starts at line **284**. Line 21 (field-1 transition) and line 283 (field-2 transition) are not treated as full active-picture lines.
- **PAL active-picture line starts (1-indexed frame lines):** field 1 starts at line **23**; field 2 starts at line **335**.
- **Progressive-to-interlaced row mapping:** for a field-local active line index $n$ (starting at 0), field 1 samples source row $2n+1$ and field 2 samples source row $2n$.

Frame-source raster preservation contract (normative):

- Progressive file sources must be `720x576` for PAL or `720x486` for NTSC.
- The internal frame-source working raster remains fixed at `720x576` for PAL and `720x486` for NTSC.
- Asset compliance for progressive sources is defined by [EXR BT.601 Compliance Requirements](../../videosynth-assets/docs/exr-bt601-compliance-requirements.md) and [MKV BT.601 Compliance Requirements](../../videosynth-assets/docs/mkv-bt601-compliance-requirements.md). Section 8.1 is required to stay aligned with those documents.
- Progressive ingestion is strict passthrough in raster geometry: no decode-time scaling, no resampling, no implicit aspect-ratio remapping, no horizontal crop, no vertical crop, and no implicit active-window extraction.
- The full source raster is preserved sample-for-sample at ingestion, including BT.601-related horizontal placement and padding semantics.
- For avoidance of doubt, this includes preserving all progressive source lines (`576` for PAL, `486` for NTSC) and preserving line width at `720` samples.
- Normative progressive-source profile expectations for the preserved `720`-sample line are:
  - **PAL (`720x576`)**: expected sample-aspect ratio is **`128:117`**; expected horizontal digital padding model is **`8-704-8`**.
  - **NTSC (`720x486`)**: expected sample-aspect ratio is **`108:119`**; expected horizontal digital padding model is **`8-704-8`**.
  - These are source-profile expectations, not runtime crop/remap instructions. Runtime ingestion preserves the input raster and padding layout exactly as delivered when the source passes profile validation.
- PAL analogue reference derivation (timing interpretation only; not an ingestion crop rule):
  - ITU-R BT.1700 Table 1 item `1a`: `576` active lines.
  - ITU-R BT.1700 Table 2: `64.0 us` line period and `12.0 us` line blanking, giving `52.0 us` analogue active line duration.
  - Under the shared ITU-R BT.601 `13.5 MHz` digital sampling model, `52.0 us * 13.5 MHz = 702` visible pixels.
  - The `720`-sample frame-source raster therefore has `18` non-visible horizontal samples, which split evenly into `9` samples of left margin and `9` samples of right margin.
  - The PAL frame-source visible aperture is therefore a centered `702x576` region at `x=9..710`, `y=0..575`.
- NTSC analogue reference derivation (timing interpretation only; not an ingestion crop rule):
  - SMPTE 170M-2004 analogue timing yields an active picture interval of approximately `52.666 us`.
  - Under the shared ITU-R BT.601 `13.5 MHz` digital sampling model, `52.666 us * 13.5 MHz = 711` visible pixels.
  - The `720`-sample frame-source raster therefore has `9` non-visible horizontal samples. Because that remainder is odd, the nearest centered integer placement is `4` samples of left margin and `5` samples of right margin.
  - The NTSC frame-source timing model yields a near-centered `711`-sample horizontal active interpretation at `x=4..714` within the `720`-sample source raster.
  - Progressive ingestion still preserves the full `720x486` source raster and does not crop any source lines.

### **Progressive Horizontal Mapping to 4fsc Active Samples (Normative)**

To preserve deterministic `4fsc` behavior from progressive inputs, horizontal mapping from frame-source pixels to active-line `4fsc` samples is defined as an exact integer-domain rule.

Definitions:

- Let $N_a$ be the number of active-line samples in the selected standard's `4fsc` active window.
- Let $W$ be the active source width used for mapping (`720` in the current runtime).
- Let $s \in [0, N_a-1]$ be the active-line sample index.
- Let $p(s)$ be the mapped source pixel index.

Normative mapping equation:

$$
p(s) = \left\lfloor \frac{s \cdot W}{N_a} \right\rfloor
$$

This mapping is mandatory for progressive-source ingestion in the generation stage.

Per-standard active-sample counts used by this mapping in current `4fsc` runtime:

- PAL: $N_a = \operatorname{round}(17{,}734{,}475 \times 52.0\,\mu s) = 922$
- NTSC: $N_a = \operatorname{round}(14{,}318{,}180 \times 52.0\,\mu s) = 745$

Properties required by this mapping:

- Every active sample maps to exactly one source pixel.
- The first and last active samples map exactly to the first and last pixels of the active source window.
- Every source active pixel maps to one or more active samples (no dropped source pixels).
- Mapping is deterministic and purely integer-domain (no floating-point coordinate interpolation in the mapping rule).

Equivalent per-pixel sample-span form (for reasoning and tests):

$$
s_{start}(i) = \left\lceil \frac{i \cdot N_a}{W} \right\rceil, \quad
s_{end}(i) = \left\lceil \frac{(i+1) \cdot N_a}{W} \right\rceil - 1
$$

for $i \in [0, W-1]$.

Practical width handling:

- Progressive sources use fixed-width mapping with $W=720$.

This rule applies identically to PAL and NTSC; only $N_a$ differs by standard.

The progressive source ingestion stage remains responsible for decoding and colour-space normalisation, but the generation stage is responsible for preserving the correct field sequence geometry when mapping normalised frame data into line-timed CVBS-domain Y/C waveforms.

---

### **Source Colour Space Requirement**

**All frame-based sources must provide pixel data to the chroma encoder in the following representation:**

> **10-bit 4:4:4 YCbCr, BT.601 studio swing**
> - Y: 64–940 (black–white)
> - Cb, Cr: 64–960 (midpoint 512)

This is a hard interface contract. The chroma encoder assumes this representation unconditionally and does not perform any colour space conversion internally. The rationale for 4:4:4 (rather than 4:2:2) is that the encoder must apply its own standard-compliant bandlimiting filters to the chroma channels before quadrature modulation. In the current implementation, PAL applies a symmetric low-pass filter of approximately 1.3 MHz on U and V before modulation, and NTSC applies a symmetric low-pass filter of approximately 1.2 MHz on Cb and Cr before modulation. Receiving pre-subsampled chroma would prevent deterministic filtering at the encoder boundary and would introduce chroma siting ambiguity that is incompatible with subcarrier-locked sampling at 4fsc.

### **PAL Chroma Encoding Model**

The PAL chroma path in this specification is normative for the implementation and follows [ITU-R BT.1700](../analogue-video-specifications/docs/video_formats/BT-1700-E/BT-1700-E.md), Annex 1 Part B, Table 1 items 10d/10f/10h and Figure 8, plus [EBU Tech. 3280-E](../analogue-video-specifications/docs/video_formats/EBU-Tech-3280-E/EBU-Tech-3280-E.md) §1.1.1/§1.2.

- PAL active chroma is synthesized as:

$$
E_M' = E_Y' + E_U'\sin(\omega t) + E_V'\cos(\omega t)
$$

- The PAL V-switch is applied by inverting the V axis on lines where the burst polarity sequence requires negative burst phase. This is sequence-aware and is not modeled as a simple global odd/even toggle.
- PAL burst phase uses the BT.1700 sequence map (I/II/III/IV across colour fields) with nominal burst phases of $+135^\circ$ and $-135^\circ$ per Table 1 item 10f.
- PAL burst suppression in the vertical interval follows BT.1700 Figure 8. The four 9-line blanking windows (I: 623–006, II: 310–318, III: 622–005, IV: 311–319) are defined in frame-line numbers and span frame boundaries, so they are applied per colour-frame parity: even-parity disc frames (fields I/II) blank lines 1–6, 310–318, and 622–625; odd-parity frames (fields III/IV) blank lines 1–5, 311–319, and 623–625. The alternation of the meander edge lines (6, 310, 319, 622) is what allows decoders (e.g. ld-decode/decode-orc) to identify the 4-field position of the 8-field sequence. PAL-M applies the corresponding 11-line windows of Figure 9 (I: 523–008, II: 260–270, III: 522–007, IV: 259–269) in the same parity-keyed manner.
- PAL subcarrier phase progression is continuous across the full frame sample timeline (non-orthogonal 4fsc lattice per EBU Tech. 3280-E, 709 379 samples/frame), and active-picture chroma uses the same sequence model as burst. Because 709 379 ≡ 3 (mod 4), the subcarrier-to-frame phase rotates by 270° per frame, producing the 4-frame (8-field) colour sequence.
- The 625-line PAL subcarrier lattice is rotated by a fixed 270° anchor so that disc frame 0 (meander parity 0, carrying fields I/II) presents the subcarrier-to-frame phase of colour fields 1/2. This pairing was validated against the ld-decode/decode-orc field-phase detection: without the anchor, the two fields of a frame decode as non-consecutive field IDs. PAL-M uses a zero anchor.

### **NTSC Chroma Encoding Model**

The NTSC chroma path in this specification is normative for the implementation and follows [SMPTE 170M-2004](../analogue-video-specifications/docs/video_formats/SMPTE-170M-2004/SMPTE-170M-2004.md) §8/§10/§13 together with [SMPTE 244M-2003](../analogue-video-specifications/docs/video_formats/SMPTE-244M-2003/SMPTE-244M-2003.md) §3.1/§4.1.1/§4.1.2.

- NTSC active chroma is synthesized as:

$$
E_M' = E_Y' + E_{Cb}'\sin(\theta) + E_{Cr}'\cos(\theta)
$$

- In the implementation, the active-picture NTSC phase reference uses burst plus $180^\circ$ (SMPTE 170M §10 convention), i.e.:

$$
	heta = \omega t + \varphi_{\text{burst,line}} + \varphi_{\text{frame}} + \pi
$$

- Line-to-line NTSC burst phase alternates at 4fsc because 910 samples/line corresponds to a $\pi$ radian subcarrier phase advance per line (SMPTE 244M §4.1.1 interpretation in this model).
- The NTSC colour sequence is modeled as a 2-frame SC-H progression using a frame phase offset of $0$ then $\pi$, repeating (SMPTE 170M field/frame color-sequence behavior).
- In baseline mode, burst is present on horizontal lines and suppressed on equalizing/broad-sync lines; when NTSC laserdisc VBI burst mode is enabled, burst is additionally inserted on equalizing and broad-sync pulses per [IEC 60857](../analogue-video-specifications/docs/laserdisc/IEC-60857-1986-Laservision-NTSC/IEC-60857-1986-Laservision-NTSC.md) §9.1.2.

### **SECAM Chroma Encoding Model**

SECAM is selected with `cvbs_presets.video_standard_preset: SECAM` and follows [ITU-R BT.1700](../analogue-video-specifications/docs/video_formats/BT-1700-E/BT-1700-E.md) Annex 1 Part C Table 4 together with [ITU-R BT.470-6](../analogue-video-specifications/docs/video_formats/BT-470-6-1998/BT-470-6-1998.md) Table 2 items 2.17/2.18. The [CVBS File Format Specification](../cvbs-file-format-specification/docs/video-standard-presets.md) v1.7.0 `SECAM` preset stores SECAM on the PAL 4fsc lattice (17,734,475 Hz, 709,379 samples/frame) with the PAL 10-bit level table, following the vhs-decode convention. `Standard::kSecam` therefore shares the 625-line timing, sync structure, levels, active raster and audio framing with `Standard::kPal` (`Is625LineStandard` in `model.h`); only the C channel differs, and the metadata declares `SECAM` with `STANDARD_STABLE_UNLOCKED`.

- `SecamChromaEncoder` (`src/secam_chroma_encoder.cpp`, behind `ISecamChromaEncoder`) replaces both the quadrature encoder and the colour burst; the line timing model disables the burst on every SECAM line. It renders each line from 0H: colour difference (item 9: $D_R' = -1.902(E_R'-E_Y')$, $D_B' = 1.505(E_B'-E_Y')$) → 33-tap 1.7 MHz low-pass (item 10c) → video precorrection $(1 + jf/f_1)/(1 + jf/3f_1)$, $f_1$ = 85 kHz, bilinear (item 10f) → FM of $f_{OR}$ = 4.40625 MHz (280 kHz per unit) or $f_{OB}$ = 4.25 MHz (230 kHz per unit), clipped to the item 10e maximum deviations → bell filter $(1+j16F)/(1+j1.26F)$, $f_0$ = 4.286 MHz, discretised by matched-z and normalised to unity at $f_0$ (item 10d) → amplitude $M_0$ = 0.115 × 700 mV → subcarrier gate open from 5.6 µs after 0H to the start of the front porch (BT.470-6 item 2.17a).
- Outside the active window the colour difference is zero, so the back porch carries the undeviated subcarrier as the line identification reference (item 13); no burst is generated.
- The D'R/D'B line sequence follows Figure 16 (line 7 of the first frame is D'R) and runs continuously through the odd-length frame, so it has a 2-frame period. The subcarrier phase at 0H follows the Note 1 pattern 0°, 0°, 180° by frame line, inverted every frame. Because the encoder renders every line from 0H independently of the 4fsc lattice position, the SECAM colour sequence period used by the frame template cache is 2 frames, matching the specification's 2-frame SECAM sequence.
- Field-blanking lines carry no subcarrier, except the field identification lines 7–15 and 320–328 (enabled by default, `secam_field_identification`), which carry the Figure 17 trapezoids (D'R* to +1.25 over 15 µs, D'B* to −1.52 over 18 µs, starting at the end of line blanking) without low-pass or precorrection.
- OSD overlays write the colourless (undeviated) subcarrier under their text: the synthesis resources precompute one neutral line per D'R/D'B component and 0H phase, and `OsdRenderer` copies it in place of the zero chroma used for the quadrature standards. The carrier phase restarts at the text edges.
- No SECAM LaserVision format exists, so laserdisc line injections, `disc_type`, laserdisc VITS placement, the pilot burst and EFM audio are validation errors for SECAM.
- Known gaps: VITS use the 625-line PAL catalogue and keep their PAL-modulated chroma packets (validator warning), and the optional SECAM luminance intermodulation correction (BT.470-6 Table 2 note 6) is not modelled.

**Progressive sources** are converted during ingestion by `ProgressiveFrameSource`, but only when they match one of the supported source profiles defined in [Section 8.1](#81-frame-based-sections). Inputs that do not match a supported profile are invalid.

All accepted progressive inputs are normalized to **10-bit 4:4:4 YCbCr BT.601 studio swing** before frame data is passed to the generator. EXR stills are converted from RGB float with the BT.601 matrix in-process. Video sources are normalised inside the `ffmpeg` decode by a filter chain planned from the probed metadata (`PlanSourceColourConversion`, `src/source_colour_conversion.cpp`):

- `color_range` `pc`/`jpeg` (or a `yuvj*` pixel format) → studio swing; `tv` or untagged is taken as studio swing. The range change runs at the source's own bit depth and is widened to 10 bits in a second `scale` stage, because swscale's combined path maps 8-bit 255 to 937 rather than 940.
- A known non-BT.601 matrix (`bt709`, `fcc`, `smpte240m`, `bt2020*`) → BT.601; `smpte170m`, `bt470bg` or untagged is taken as BT.601 (the SD convention).
- RGB pixel formats → BT.601 studio-swing YCbCr.
- Any other pixel format or bit depth → `yuv422p10le`, then unpacked to 4:4:4 by repeating each colour-difference sample for its co-sited luma pair (ITU-R BT.601-5 Annex 1 Table 2 item 3).
- Primaries and transfer characteristics are not converted; mismatches are logged at `debug`.
- A source already at `yuv422p10le`, studio range, BT.601 matrix takes no filter and passes through bit-exact.

Every `scale` stage uses `accurate_rnd+full_chroma_int+bitexact`, so the decoded codes do not depend on the host CPU's SIMD path.

Source-range capability note:

- **Video and EXR supported profiles** are required to preserve 10-bit studio-domain detail, including below-black and above-white code excursions when present in the source.

---

### **Responsibilities**


| **Responsibility**              | **Description**                                                                          | **Reference**                   |
| ------------------------------- | ---------------------------------------------------------------------------------------- | ------------------------------- |
| **Luma (Y) Generation**         | Generate luma component per frame, including sync pulses and blanking.                   | ITU-R BT.470-6, ITU-R BT.1700   |
| **Chroma (C) Generation**       | Generate chroma component per frame, including colour burst.                             | ITU-R BT.470-6, ITU-R BT.1700   |
| **Progressive Source Ingestion**| Decode progressive frames, convert to 10-bit 4:4:4 YCbCr BT.601 studio swing, split into interlaced fields. | ITU-R BT.470-6, ITU-R BT.601   |
| **Sync and Burst Insertion**    | Insert HSync, VSync, equalizing pulses, and colour burst.                                | ITU-R BT.470-6, ITU-R BT.1700   |
| **PAL Laserdisc Pilot Burst**   | Superimpose a 3.75 MHz pilot burst on all sync pulses when enabled (PAL only).           | IEC 60856 §9.1.2                |
| **NTSC Laserdisc VBI Burst**    | Insert colour burst on equalizing and broad sync pulses when enabled (NTSC only).        | IEC 60857 §9.1.2                |
| **Line Injections**             | Inject VITS, Laserdisc biphase, VITC, or custom line content into VBI.                  | IEC 60856, IEC 60857, SMPTE 12M |
| **Ramping and Smoothing**       | Apply ramping to transitions to simulate analogue behavior.                              | ITU-R BT.470-6, ITU-R BT.1700   |
| **OSD Overlay**                 | Render monochrome bitmap-font text into the active-picture luma channel, after biphase injection. | — |


### **Frame Template Cache and Per-Frame Patches**

Per-frame synthesis is split into a **clean frame template** and a set of **per-frame patches** (`GenerationStage::SynthesiseTemplate`, `src/generation_stage.cpp`):

- The **template** carries everything that is invariant for a given source frame within one colour sequence period: the sync/blanking skeleton, colour burst (including PAL burst blanking and V-switch), the PAL LaserDisc pilot burst, the active picture, and the project-wide VITS lines.
- The **patches** are the things that genuinely vary per output frame — Laserdisc VBI waveforms (biphase/FM/white flag) and OSD text — applied to a copy of the template and touching only their own lines and samples. Noise and dropout injection run downstream in their own stages, also per frame.

**Periodicity.** For identical source content the clean frame is an exact function of `(source, source_frame_index, disc_frame_index mod P)`:

- The subcarrier advances exactly π/2 per 4fsc sample, so its phase depends only on `absolute_sample_index mod 4`. Samples per frame mod 4 is 3 for PAL (709,379), 1 for PAL-M (477,225) and 2 for NTSC (477,750), giving a lattice period of 4, 4, and 2 frames respectively.
- The PAL/PAL-M burst-blanking meander and V-switch have period 2 frames, which divides 4.
- The PAL LaserDisc pilot burst is period 1 (17,734,475 pilot cycles = 25 × 709,379 samples).

Hence **P = 4 frames for PAL and PAL-M, and P = 2 frames for NTSC**; a still section contains only P distinct clean frames, and a video section that replays or loops its range contains at most P × range length. Synthesis reduces `disc_frame_index` to this residue (`sequence_phase`) before deriving any phase, which is exact — every use of the index inside clean synthesis depends only on the residue — so the split does not change output bits.

**Cache.** `GenerationStage` shares clean templates between worker threads through a bounded `TemplateCache`:

- **Key**: `(source path, section type, source frame identity, sequence_phase)`. Sections sharing a source share templates, and a still (`.exr`) source collapses every schedule position onto one identity because its decoder returns the same image for every frame index. Everything else the template depends on — the CVBS presets and the project VITS set — is held as the cache configuration, and a lookup against a different configuration clears the cache. `BuildFrameSchedule` also clears it, alongside the decoded-source cache.
- **Admission on second request**: a key's first request returns "synthesise directly" and only a repeated request builds and stores the template. A clip source played once produces every key exactly once and therefore bypasses the cache entirely, instead of filling it with templates nothing will ever read; still frames and `duration_repeat` passes revisit their keys and are admitted from the second request on.
- **Concurrency**: the lookup table is mutex-guarded and each entry is built exactly once under a per-key `std::once_flag` (single-flight); concurrent requests for the same key wait for that one build while other keys proceed. Published templates are immutable `shared_ptr`-to-const data.
- **Sizing**: entries are admitted until a configurable byte capacity (default 512 MiB, ~45 PAL templates; CLI `--template-cache-mb`, 0 disables) and never evicted; once the cache is full, further misses synthesise directly into the output buffer. Sources with more distinct frames than the cache can hold therefore degrade to the uncached path instead of thrashing.
- **Determinism**: output is byte-identical with the cache enabled, disabled, or capped at any size, and across thread counts — a cache hit is a copy of data produced by the same `SynthesiseTemplate` function the direct path runs.

### **Decoded Source Cache and Prefetch**

Decoded source images are held by `ProgressiveFrameSource` in a small
most-recently-used table (at most three sources: the section being rendered,
the previous section a straddling worker may still be finishing, and a section
decoded ahead of time). Images are immutable and delivered as
`shared_ptr<const FrameSourceImage>`, so delivery costs a reference count and a
frame stays valid after its entry is evicted.

The table lock is held only for the lookup; each entry carries its own lock,
held across that entry's decode. Two requests for the same source therefore
share one decoder, while decoding one source never blocks requests for another.

**Streaming video decode.** An EXR entry holds its single image. A video entry
never holds the whole clip — a 30-minute 625-line clip would need ~100 GiB —
but a bounded **window** of recently decoded frames fed by sequential
decoders (`IVideoFrameDecoder`, `src/video_frame_decoder.cpp`):

- `FfmpegVideoFrameDecoder` holds one `ffmpeg … -map 0:v:0 [-vf <plan>]
  -pix_fmt yuv422p10le -f rawvideo -` pipe open and reads one frame per call.
  Opening at frame *N* > 0 places an input-side `-ss` half a frame before
  *N* (offset by the video stream's start relative to the container start);
  ffmpeg decodes from the preceding keyframe and drops earlier frames, so the
  seek is frame-exact for intra and long-GOP codecs and robust to the 1 ms
  Matroska timestamp rounding.
- Each entry keeps up to two decode cursors. A request inside the window is a
  hit; one less than a window ahead of a cursor reads forward; anything else
  (a loop back to the range start, a far jump) reopens the least recently
  used cursor half a window before the request, never before the section's
  `start_frame`, so workers that reach a section start or a jump out of order
  are served by the same decoder. Two cursors keep a loop boundary — workers
  straddling the end of one pass and the start of the next — from reopening on
  every request.
- The window evicts the least recently used frame beyond its capacity of
  `max(32, 2 × hardware threads + 16)` frames: the frame-synthesis pool's
  in-flight bound (2 × threads) plus margin for the batch path and prefetch.
  Peak decoded-frame memory is therefore 3 sources × capacity × raster size,
  independent of clip length (a 3 000-frame clip measured 184 MiB peak RSS
  with `--threads 1`).
- `ResolveFrameCount` returns the probed count (`nb_frames`, or container
  duration × rate) without decoding. A stream that ends before that count is
  reported as an error naming the frame.

That separation is what makes **prefetching** useful. `BuildFrameSchedule`
records, for each section, the source the next section will read; when
synthesis reaches the first frame of a section run, it asks the frame source to
decode the start of the next section's source on a background thread
(`ProgressiveFrameSource::PrefetchSection`): an EXR convert, or half a window of
video frames (never past the section's `end_frame`), one frame per entry-lock
acquisition so a worker that arrives
early is served as soon as its frame exists. A section-start decode otherwise
stalls every worker at the boundary. Prefetch failures are ignored: the frame
request that follows repeats the decode and reports the error, so the prefetch
changes only when work happens, never what is produced. Hand-built schedules
carry no such mapping and simply do not prefetch.

### **OSD (On-Screen Display) Sub-system**

The `OsdRenderer` class writes monochrome bitmap-font text overlays into the luma sample buffer of an active video frame.  It is invoked as the final step of per-frame generation, after active video content and biphase injection, so that burn-in tokens have access to the current-frame biphase context.

**Data model** (defined in `model.h`):

- `OsdOverlay` — one text string (literal or token template), active-area pixel position (`x`, `y`), glyph scale factor (`scale` in [1, 4]), foreground level (`fg_level`, one of four discrete luma steps: `white` = 1.0, `light_grey` = 0.75, `dark_grey` = 0.25, `black` = 0.0), and background level (`bg_level`, the same four steps plus `transparent` = no background write; default `transparent`).
- `OsdConfig` — a list of zero or more `OsdOverlay` objects per section.  Stored as `Section::osd`.

**Rendering** (`OsdRenderer`, `src/osd_renderer.cpp`):

- Uses a static 96-glyph 8×8 pixel bitmap font (`src/osd_font.h`) covering printable ASCII 0x20–0x7F, derived from the IBM PC BIOS 8×8 VGA font.
- Each glyph pixel is rendered as a `scale` × `scale` output block.
- Only the luma channel is written; the chroma channel is unchanged (monochrome overlay).
- Pixels outside `[active_sample_start, active_sample_end)` or `[active_line_start, active_line_end)` are silently clipped.
- Token resolution is done by `OsdTokenResolver` (`include/videosynth/osd_token_resolver.h`) before passing `resolved_texts` to `Render()`.

**Token resolution** (`OsdTokenResolver`, `src/osd_token_resolver.cpp`):

- `{picture_number}` — zero-padded 5-digit CAV picture number from `PerFrameContext` (IEC 60856/60857 max 99999); `"00000"` when 0.
- `{biphase_hex}` — space-separated 6-digit uppercase hex biphase code words; `"000000"` when empty.
- `{phase_id}` — colour-frame sequence index (0–3 PAL, 0–1 NTSC).
- `{section_name}` — the section `name:` field verbatim.
- `{timecode}` — CLV programme timecode `HH:MM:SS:FF`, computed from the 0-based sequential output frame position at the standard's CLV frame rate (`ClvTimecodeForFrame`, 25 fps PAL / 30 fps NTSC); runs continuously from the start of the output on any CLV disc, independent of which VBI codes a section injects. `"00:00:00:00"` on non-CLV discs.
- `{frame_number}` — 1-based sequential position of the frame in the whole generated output, zero-padded to 5 digits; set from the schedule index in `GenerationStage`, never re-anchored by a section.
- Unavailable values render as all-zero fields with the same width as a real value, so overlay layout never shifts.
- Unknown token names are rejected at project-validation time by `HasOnlyKnownTokens()`; static text with no tokens passes through unchanged.

**Per-frame VBI context** (`PerFrameContext`, `include/videosynth/biphase_injection_manager.h`):

- Captured by `BiphaseInjectionManager::ProcessFrame()` before advancing generators.
- Exposed via `GetLastFrameContext()` for use by token resolver immediately after injection.
- Reset to defaults by `Reset()`.

**Pipeline wiring** (`GenerationStage`, `src/generation_stage.cpp`):

- OSD rendering executes after `biphase_manager_.ProcessFrame()` for each frame.
- Tokens are resolved from `biphase_manager_.GetLastFrameContext()`.
- `Render()` is called twice per frame — once for field 1 and once for field 2 — so overlays appear in both fields at the same y-offset within each field's active picture area.

**YAML** (`src/yaml_project_parser.cpp`): sections may include an `osd:` block with an `overlays:` list; each overlay supports `text`, `x`, `y`, `scale`, `fg_luma` (string enum: `white`, `light_grey`, `dark_grey`, or `black`), and `bg_luma` (string enum: `transparent`, `white`, `light_grey`, `dark_grey`, or `black`).

**Validation** (`src/project_validator.cpp`): `scale` ∈ [1, 4]; `fg_luma` must be one of `white`, `light_grey`, `dark_grey`, `black`; `bg_luma` must be one of `transparent`, `white`, `light_grey`, `dark_grey`, `black`; token names must be one of the four above.


### **Inputs**

- Frame-based content in **10-bit 4:4:4 YCbCr BT.601 studio swing** from normalised progressive sources.
- Line-based injections (VITS, Laserdisc biphase, VITC).
- CVBS presets (video_standard_preset, sample_encoding_preset, signal_state_preset).

### **Outputs**

- `4fsc`-discrete **luma (Y)** and **chroma (C)** sample buffers as **high-resolution fixed-point mV values**, emitted as whole-frame batches (typically small bounded groups of frames) for handoff to the Noise Injection Stage and then to the Output Stage. Values are relative to blanking and are stored internally as signed integers scaled by $2^{20}$ before final quantisation.
- Metadata (field order, dominance, timing).

---

---

## **5. Output Stage**

### **Specification Cross-Check (Section 5)**

- 10-bit quantization and legal/protected code-space behavior are grounded in [EBU Tech. 3280-E](../analogue-video-specifications/docs/video_formats/EBU-Tech-3280-E/EBU-Tech-3280-E.md) §1.1.1/§1.1.2 and [SMPTE 244M-2003](../analogue-video-specifications/docs/video_formats/SMPTE-244M-2003/SMPTE-244M-2003.md) §3.3, §4.2.4, Table 1.
- NTSC 4fsc sample-phase alignment and clock/subcarrier relationship are grounded in [SMPTE 244M-2003](../analogue-video-specifications/docs/video_formats/SMPTE-244M-2003/SMPTE-244M-2003.md) §3.1 and §4.1.2.
- PAL 4fsc sampling rate and interface assumptions are grounded in [EBU Tech. 3280-E](../analogue-video-specifications/docs/video_formats/EBU-Tech-3280-E/EBU-Tech-3280-E.md) §1.1.1 and §2.4.

### **Responsibilities**


| **Responsibility**      | **Description**                                                         | **Reference**                     |
| ----------------------- | ----------------------------------------------------------------------- | --------------------------------- |
| **Batch Validation**    | Validate that incoming Y and C buffers match whole-frame `4fsc` timing for the selected standard. | CVBS File Format Specification    |
| **mV to Integer Conversion** | Quantise fixed-point mV values to 10-bit integers per EBU 3280 (PAL) or SMPTE 244M (NTSC). | EBU Tech. 3280-E, SMPTE 244M-2003 |
| **Combining Y and C**   | Combine quantised luma and chroma into composite signal (CVBS), or write them to separate `.cvbsy`/`.cvbsc` files in Y/C mode. | CVBS File Format Specification    |
| **Output Formatting**   | Format the quantised composite signal into the output files (video and metadata).   | CVBS File Format Specification    |
| **Locked 4fsc Enforcement**  | Enforce the current runtime requirement that output remains locked `4fsc` only. | SMPTE 244M-2003, EBU Tech. 3280-E |
| **Metadata Generation** | Generate CVBS file metadata (magic number, version, sample rate, etc.). | CVBS File Format Specification    |


### **CVBS File Output**

The output stage generates files as per the [CVBS File Format Specification](../cvbs-file-format-specification/docs/index.md). The output mode is controlled by `output.signal_type` in the project YAML:

**Composite output** (`signal_type: composite`, default):
1. **Video File** (`.cvbs`): Raw samples of the composite signal (Y + C + sync).
2. **Metadata File** (`.meta`): SQLite metadata database (`user_version = 12`, CVBS File Format Specification v1.7.0) — video standard preset, sample encoding preset, signal state preset, `sequence_continuous` (always `TRUE`: synthesised output is one unbroken sequence), frame count, etc.

**Dual-file Y/C output** (`signal_type: yc`):
1. **Luma File** (`.cvbsy`): Raw luma samples (Y + sync) encoded identically to composite luma.
2. **Chroma File** (`.cvbsc`): Raw chroma samples, centred at code 512 in the 10-bit domain as defined by the [CVBS File Format Specification — Sample Encoding Presets](../cvbs-file-format-specification/docs/sample-encoding-presets.md).
3. **Metadata File** (`.meta`): SQLite metadata database, identical structure to composite output with `signal_type` set to `'yc'`.

For Y/C output, `output.video_path` must end in `.cvbsy`; the chroma path is derived by replacing the `.cvbsy` suffix with `.cvbsc`.

### **LaserDisc Digital Audio (EFM) Output**

A project may additionally emit the LaserDisc digital audio channel of one audio channel pair. The stage-by-stage design and its specification mapping are given below.

**Module boundary** — all EFM signal processing lives in a standalone static library `videosynth_efm` (`src/efm/`, `include/videosynth/efm/`, namespace `videosynth::efm`) that has **no dependency on `videosynth_core`**: its API takes 16-bit stereo PCM plus a track table and emits an EFM channel stream. `videosynth_core` links the module, not the reverse, so a decoder, data modes, or RF modulation can be added later without touching the audio subsystem. Its stages are:

| Stage | Responsibility | Reference |
|---|---|---|
| `AudioFrameAssembler` | Six 16-bit stereo samples → one 24-byte F1 frame (WmA = high byte, WmB = low byte) | IEC 60908-1999, 16.2 |
| `CircEncoder` | GF(2⁸) C2 (28,24) and C1 (32,28) Reed–Solomon parity with the CIRC delay/interleave structure and parity inversion | IEC 60908-1999, 16.3; ECMA-130 Annex C |
| `SubcodeGenerator` | 98-frame subcode sections at 75 Hz with S0/S1 sync, channel P, and Q-mode 4 (ADR = 0100) DATA-Q: lead-in TOC, per-track running/absolute time, lead-out | IEC 60908-1999, 17.3–17.5.1; IEC 60856:1986 Amd 2, 13.5; IEC 60857:1986 Amd 2, 13.6 |
| `EfmModulator` | Eight-to-fourteen coding, merging-bit selection (runs 3–11 channel bits, DSV minimised), 24-bit frame sync, 588-channel-bit frames | IEC 60908-1999, clauses 13–15; ECMA-130 Annex D/E |
| `EfmStreamEncoder` | The module's public facade: `Begin` / `PushSamples` / `Flush`, chaining the stages and exposing T-values | — |

**Output files** — the EFM extension is the two-file pair defined by the [EFM Extension Format](../cvbs-file-format-specification/docs/extensions/efm-extension-format.md): `<basename>.efm` holds the channel stream as one unsigned byte per pit/land run length (T3–T11), starting at the first frame sync, and `<basename>.efm.meta` is its own SQLite sidecar (schema version 1) carrying one `efm_frame` row per stored video frame — `cvbs_file_id`, `frame_id`, `t_value_offset` and `t_value_count`. Both share the CVBS basename, so the channel pair is not part of either name. `AudioEfmWriter` mirrors the `AudioWavWriter` streaming contract (`BeginWrite` / `AppendFrameAudio` / `FinalizeWrite` / `AbortWrite`), writing the sidecar on finalize, so an aborted run leaves neither file behind. No EFM metadata is written into the core `<basename>.meta` database: the core CVBS schema has no EFM concept, and the extension is self-describing.

**Sample timing** — the EFM path synthesises the selected pair independently at exactly 44 100 Hz (PAL 1764/625 × F_H, IEC 60856:1986 Amd 2, 13.2; NTSC 7007/2500 × F_H, IEC 60857:1986 Amd 2, 13.2) from the same `AudioParameters` as the 48 kHz WAV path — there is no resampling. Per video frame that is 1764 samples for PAL (exact) and the SMPTE 272M-1994 Table 1 sequence for NTSC (147147 samples per 100 frames). Samples are converted from 24-bit to the CD's 16-bit domain by rounding to nearest with saturation.

**Track layout** — every contiguous run of output frames sharing one `programme_area` section is one track, numbered 01, 02, … in output order (maximum 79 tracks). `lead_in` sections carry the repetitive mode-4 TOC (one POINT per track plus A0/A1/A2, with the video-system identification 22 = PAL / 12 = NTSC in the A0 P FRAME field), and `lead_out` sections carry lead-out subcode (TNO = AA). Track boundaries are placed on the nearest subcode section, so a boundary sits within 1/150 s of the video frame carrying the section change. The first 2 s of track 1 are the mandatory pause required by IEC 60908-1999, 17.5.1: digital silence in the EFM stream while the WAV track carries the section's tone.

**Timing alignment** — t = 0 is common to video frame 0, WAV sample 0, EFM source sample 0, and Q absolute time 00:00:00. The only structural delay is the CIRC interleave, exported as `kCircPipelineLatencyFrames = 108` (ECMA-130, C.9): a complementary decoder emits 108 F1 frames (648 stereo samples, ≈14.69 ms) of warm-up silence before source sample 0, after which decoded PCM is sample-exact against the 44.1 kHz source. The stream is **not** pre-compensated for that delay, so the shared datum with the video and WAV timelines holds.

### **Inputs**

- `4fsc`-discrete fixed-point Y and C signal buffers from the generation stage.
- CVBS presets (sample rate, subcarrier lock, endianness).

### **Outputs**

- **Video File**: Raw samples of the composite signal.
- **Metadata File**: Header metadata. EFM output adds no rows here; it carries its own `<basename>.efm.meta` sidecar.

---

---

## **6. PAL and NTSC Analogue Specifications**

### **Specification Cross-Check (Section 6)**

- PAL/NTSC signal level tables are grounded in [ITU-R BT.1700](../analogue-video-specifications/docs/video_formats/BT-1700-E/BT-1700-E.md), Table 2 (blanking/white/sync), and [SMPTE 170M-2004](../analogue-video-specifications/docs/video_formats/SMPTE-170M-2004/SMPTE-170M-2004.md) §15.4 / Annex B.
- NTSC IRE relationships are grounded in [SMPTE 170M-2004](../analogue-video-specifications/docs/video_formats/SMPTE-170M-2004/SMPTE-170M-2004.md), Annex B (IRE units) and §15.4.
- PAL pilot-burst headroom requirement is grounded in [IEC 60856](../analogue-video-specifications/docs/laserdisc/IEC-60856-1986-Laservision-PAL/IEC-60856-1986-Laservision-PAL.md) §9.1.2.
- 10-bit code anchors and excluded/reserved code ranges are grounded in [EBU Tech. 3280-E](../analogue-video-specifications/docs/video_formats/EBU-Tech-3280-E/EBU-Tech-3280-E.md) §1.1.1/§1.1.2 and [SMPTE 244M-2003](../analogue-video-specifications/docs/video_formats/SMPTE-244M-2003/SMPTE-244M-2003.md) §4.2.4, Table 1.
- Equalizing and broad sync pulse durations are grounded in [ITU-R BT.1700](../analogue-video-specifications/docs/video_formats/BT-1700-E/BT-1700-E.md), Table 3 and [SMPTE 170M-2004](../analogue-video-specifications/docs/video_formats/SMPTE-170M-2004/SMPTE-170M-2004.md) §13.3.
- PAL/NTSC non-visible line structures are grounded in [ITU-R BT.470-6](../analogue-video-specifications/docs/video_formats/BT-470-6-1998/BT-470-6-1998.md), Annex 1 Tables 1-1/1-2, [ITU-R BT.1700](../analogue-video-specifications/docs/video_formats/BT-1700-E/BT-1700-E.md), and laserdisc overlays in [IEC 60856](../analogue-video-specifications/docs/laserdisc/IEC-60856-1986-Laservision-PAL/IEC-60856-1986-Laservision-PAL.md) §9.1.3-§9.1.4 and [IEC 60857](../analogue-video-specifications/docs/laserdisc/IEC-60857-1986-Laservision-NTSC/IEC-60857-1986-Laservision-NTSC.md) §9.1.3-§9.1.5.

All specifications for **PAL** and **NTSC** are explicitly referenced from the [Analogue Video Specifications](../analogue-video-specifications/docs/index.md) repository.

---

### **6.1. Signal Levels**

All signal levels within VideoSynth are expressed in **millivolts (mV)**, measured relative to **blanking level (0 mV)**. IRE units are used in NTSC engineering documentation but are **not used internally by the generator**; they are defined here solely for reference when reading NTSC standards.

#### **PAL Signal Levels**

PAL uses mV natively. All PAL standard documents and the VITS definitions in this repository use mV.

| **Level**       | **mV (relative to blanking)** | **Description**                     |
| --------------- | ----------------------------- | ----------------------------------- |
| Sync tip        | −300 mV                       | Bottom of sync pulse.               |
| Blanking        | 0 mV                          | Reference level for active video.   |
| Black           | 0 mV                          | Black level (same as blanking, PAL).|
| White           | 700 mV                        | 100% white (luminance peak).        |
| Composite peak  | 933 mV                        | Approximate peak including chroma.  |

The full PAL composite signal swings from −300 mV (sync tip) to approximately +933 mV (peak chroma), for a total composite amplitude of ~1233 mV. The **luminance range is 0–700 mV**.

#### **NTSC Signal Levels and IRE Conversion**

NTSC documentation traditionally uses **IRE units** (Institute of Radio Engineers). The generator does not use IRE internally; all NTSC levels are converted to mV for processing.

The IRE scale is defined relative to blanking:

| **Level**       | **IRE** | **mV (relative to blanking)** | **Description**                      |
| --------------- | ------- | ----------------------------- | ------------------------------------ |
| Sync tip        | −40 IRE | −285.7 mV                     | Bottom of sync pulse.                |
| Blanking        | 0 IRE   | 0 mV                          | Reference level for active video.    |
| Black           | 7.5 IRE | 53.6 mV                       | Set-up (pedestal), used in NTSC USA. |
| White           | 100 IRE | 714.3 mV                      | 100% white (luminance peak).         |

VideoSynth supports an NTSC-specific black-level parameter under the NTSC-M timing model:

- **Standard NTSC setup**: black at **7.5 IRE** (`53.6 mV` above blanking).
- **Optional 0 IRE setup**: black at **0 IRE** (`0 mV`, equal to blanking). This matches the black-level convention often associated with NTSC-J usage, but in VideoSynth it is exposed only as a black-level parameter and does not define a separate video format.

This option affects **black reference and luma mapping only**. It does **not** change NTSC line timing, field cadence, sync amplitudes, burst behavior, subcarrier frequency, or 4fsc sample-rate rules.

**Conversion formula** (IRE ↔ mV, both relative to blanking):

$$\text{mV} = \text{IRE} \times 7.143$$
$$\text{IRE} = \frac{\text{mV}}{7.143}$$

This derives from the NTSC definition that 100 IRE = 714.3 mV (the luminance range from blanking to white).

> **Important**: PAL white (700 mV) and NTSC white (100 IRE = 714.3 mV) are **not the same level**. They must not be used interchangeably. The generator applies the correct levels for each standard independently.

#### **Summary Comparison**

| **Level**   | **PAL (mV)** | **NTSC standard (mV / IRE)** | **NTSC 0 IRE option (mV / IRE)** |
| ----------- | ------------ | ----------------------------- | --------------------------------- |
| Sync tip    | −300 mV      | −285.7 mV / −40 IRE          | −285.7 mV / −40 IRE              |
| Blanking    | 0 mV         | 0 mV / 0 IRE                 | 0 mV / 0 IRE                     |
| Black       | 0 mV         | 53.6 mV / 7.5 IRE            | 0 mV / 0 IRE                     |
| White       | 700 mV       | 714.3 mV / 100 IRE           | 714.3 mV / 100 IRE               |

---

#### **Internal Fixed-Point Representation**

The generator uses **signed fixed-point millivolt buffers** to represent all signal levels internally. The runtime path now stores Y and C samples in a high-resolution integer domain (Q20.12-style scaling in the implementation) so the generation stage and output stage can stay deterministic without carrying a floating-point representation.

**Why fixed-point is sufficient:**  
Certain signal additions push levels well outside the nominal PAL or NTSC signal range. The most significant case is the **PAL Laserdisc pilot burst** ([IEC 60856](../analogue-video-specifications/docs/laserdisc/IEC-60856-1986-Laservision-PAL/IEC-60856-1986-Laservision-PAL.md) §9.1.2), which is superimposed on the sync pulse at 3.75 MHz with a peak-to-peak amplitude of $\frac{6}{7} \times 700\ \text{mV} = 600\ \text{mV p-p}$. Centred on sync tip level (−300 mV), this burst swings between **−600 mV and 0 mV** — 300 mV below the standard sync tip. The fixed-point domain retains this headroom while still avoiding floating-point state in the runtime pipeline.

**Required internal headroom:**

| **Boundary**       | **Minimum range required** | **Driven by**                                          |
| ------------------ | -------------------------- | ------------------------------------------------------ |
| Negative (below blanking) | ≤ −600 mV           | PAL Laserdisc pilot burst (IEC 60856 §9.1.2)           |
| Positive (above blanking) | ≥ +1000 mV          | Peak chroma excursions on 100% colour bars (~933 mV) plus margin |

The fixed-point domain is wide enough to carry all supported excursions without clipping during generation. All signal components — sync, burst, active video, line injections — are additively composed in fixed-point mV before final quantisation occurs.

**Output stage quantisation (fixed-point mV → 10-bit integer):**

The output stage performs a single linear mapping from the internal fixed-point mV value to a 10-bit integer code, using the normative level tables from the relevant digital interface standard. Values outside the legal code range are clamped to the standard's legal range before writing.

*PAL — EBU Tech. 3280-E (10-bit):*

$$\text{code} = \operatorname{round}\!\left(\frac{\text{mV}}{1.1905} + 256\right)$$

Normative anchors: sync tip −300 mV → code 4; blanking 0 mV → code 256; white 700 mV → code 844. Legal range: codes 4–1019.

*NTSC — SMPTE 244M-2003 (10-bit):*

$$\text{code} = \operatorname{round}\!\left(\frac{\text{mV}}{1.2755} + 240\right)$$

Normative anchors: sync tip −285.7 mV → code 16; blanking 0 mV → code 240; white 714.3 mV → code 800. Legal range: codes 16–1019.

Codes 0–3 and 1020–1023 are reserved (excluded values) per both standards and must never appear in output.

---

### **6.2. Half-Line Element Definitions**

Each **line** in the vertical blanking interval (VBI) is composed of **two half-line elements**. The following symbols represent the **sync and blanking pulses**:


| **Symbol** | **Description**     | **PAL Duration**  | **NTSC Duration** | **Reference**                   |
| ---------- | ------------------- | ----------------- | ----------------- | ------------------------------- |
| **BL**     | Blanking Level      | 0.3V              | 0.3V              | ITU-R BT.1700                   |
| **SY**     | Sync Level          | 0.0V              | 0.0V              | ITU-R BT.1700                   |
| **EQ**     | Equalizing Pulse    | 2.3 µs (1/4 line) | 2.3 µs (1/4 line) | ITU-R BT.470-6, SMPTE 170M-2004 |
| **VS**     | Vertical Sync Pulse | 27.3 µs (2.5H)    | 31.778 µs (3H)    | ITU-R BT.470-6, SMPTE 170M-2004 |


**Notes**:

- **EQ**: Equalizing pulses are **2.3 µs** wide (1/4 line).
- **VS**: Vertical sync pulses are **2.5H (PAL)** or **3H (NTSC)** wide.

---

### **6.3. PAL Non-Visible Line Structures**

For **PAL (625-line system)**, the vertical blanking interval (VBI) is defined as follows, with **each line consisting of two half-line elements**:

#### **Field 1 (Odd Field)**


| **Line** | **Half-Line Elements** | **Notes**                                          |
| -------- | ---------------------- | -------------------------------------------------- |
| 1        | VS, VS                 | Vertical (broad) sync pulses (2.5H nominal, modeled line-granular on lines 1–3). Pre-equalizing pulses for field 1 occupy lines 623–625 of the preceding frame. |
| 2        | VS, VS                 | Vertical sync pulse.                               |
| 3        | VS, VS                 | Vertical sync pulse (end).                         |
| 4        | EQ, EQ                 | Post-equalizing pulses (2.5H nominal, modeled line-granular on lines 4–5). |
| 5        | EQ, EQ                 | Post-equalizing pulses (end).                      |
| 6        | H                      | Normal line sync; carries colour burst subject to the BT.1700 Figure 8 burst-blanking sequence (present on fields III/IV frames, blanked on fields I/II frames). |
| 7-15     | H                      | Blanked horizontal lines (line sync and burst, no content). |
| 16-18    | BL, BL                 | VBI (Laserdisc biphase: programme status on 16, picture stop/chapter on 17, chapter/time code on 18). |
| 19-21    | BL, BL                 | VBI (VITS on 19/20; VITC on 19–21; subtitle on 20–21).    |
| 22       | BL, BL                 | VBI (CEA-608; blanked when Laserdisc active).              |
| 23-310   | Active Video           | Field 1 visible content (burst on line 310 is subject to the Figure 8 window II). |


#### **Field 2 (Even Field)**


| **Line** | **Half-Line Elements** | **Notes**                                      |
| -------- | ---------------------- | ---------------------------------------------- |
| 311      | EQ, EQ                 | Pre-equalizing pulses.                         |
| 312      | EQ, EQ                 | Pre-equalizing pulses.                         |
| 313      | EQ, VS                 | Transition into broad-sync block (mixed half-line). |
| 314      | VS, VS                 | Vertical sync pulse.                           |
| 315      | VS, VS                 | Vertical sync pulse (end).                     |
| 316      | EQ, EQ                 | Post-equalizing pulses.                        |
| 317      | EQ, EQ                 | Post-equalizing pulses.                        |
| 318      | EQ, BL                 | Transition out of sync block (mixed half-line).|
| 319-334  | BL, BL                 | VBI lines (burst on line 319 is subject to the Figure 8 window IV). |
| 335-622  | Active Video           | Visible content (burst on line 622 is subject to the Figure 8 window III). |
| 623-625  | EQ, EQ                 | Pre-equalizing pulses for next frame boundary. |


---

### **6.4. NTSC Non-Visible Line Structures**

For **NTSC (525-line system)**, the vertical blanking interval (VBI) is defined as follows, with **each line consisting of two half-line elements**:

#### **Field 1 (Odd Field)**


| **Line** | **Half-Line Elements** | **Notes**                                          |
| -------- | ---------------------- | -------------------------------------------------- |
| 1        | EQ, EQ                 | Pre-equalizing pulses (6 pulses total, 3H).        |
| 2        | EQ, EQ                 | Pre-equalizing pulses.                             |
| 3        | EQ, EQ                 | Pre-equalizing pulses.                             |
| 4        | VS, VS                 | Vertical sync pulse (start).                       |
| 5        | VS, VS                 | Vertical sync pulse.                               |
| 6        | VS, VS                 | Vertical sync pulse (end).                         |
| 7        | EQ, EQ                 | Post-equalizing pulses (6 pulses total, 3H).       |
| 8        | EQ, EQ                 | Post-equalizing pulses.                            |
| 9        | EQ, EQ                 | Post-equalizing pulses.                            |
| 10-11    | BL, BL                 | VBI (Laserdisc 40-bit FM coded signal reserved: FM data on 10, white flag on 11). |
| 12-15    | BL, BL                 | VBI (Laserdisc biphase reserved; reserved for future applications).               |
| 16       | BL, BL                 | VBI (Laserdisc biphase: programme status / CLV picture number / users code).      |
| 17-18    | BL, BL                 | VBI (Laserdisc biphase: picture number / chapter / programme time code on 17–18). |
| 19       | BL, BL                 | VBI (VIR signal; mandatory on NTSC laserdisc per IEC 60857 §9.1.3).              |
| 20       | BL, BL                 | VBI (VITS composite test signal; recommended per IEC 60857 §9.1.4).              |
| 21       | BL, BL                 | VBI (CEA-608 / closed caption).                                                   |
| 22-262   | Active Video           | Visible content in field 1 through the field boundary lead-in. |
| 263      | SY, EQ                 | Field-transition lead-in (mixed half-line; horizontal-sync then equalizing). |
| 264-265  | EQ, EQ                 | Pre-equalizing pulses.                             |
| 266      | EQ, VS                 | Transition into broad-sync block (mixed half-line). |
| 267-268  | VS, VS                 | Vertical sync pulses.                              |
| 269      | VS, EQ                 | Transition out of broad-sync block (mixed half-line). |
| 270-271  | EQ, EQ                 | Post-equalizing pulses.                            |
| 272      | EQ, BL                 | Final transition to blanking (mixed half-line).    |


#### **Field 2 (Even Field)**


| **Line** | **Half-Line Elements** | **Notes**                                    |
| -------- | ---------------------- | -------------------------------------------- |
| 263      | SY, EQ                 | Field-transition lead-in (mixed half-line; horizontal-sync then equalizing).  |
| 264-265  | EQ, EQ                 | Pre-equalizing pulses.                       |
| 266      | EQ, VS                 | Transition into broad-sync block (mixed half-line). |
| 267-268  | VS, VS                 | Vertical sync pulses.                        |
| 269      | VS, EQ                 | Transition out of broad-sync block (mixed half-line). |
| 270-271  | EQ, EQ                 | Post-equalizing pulses.                      |
| 272      | EQ, BL                 | Final transition to blanking (mixed half-line). |
| 273-282  | BL, BL                 | VBI lines.                                   |
| 283      | BL, BL                 | Field-2 transition line; not treated as full active picture. |
| 284-520  | Active Video           | Visible content in field 2.                         |
| 521-525  | EQ/VS                  | Frame-end sync block; no active picture.            |


---

---

## **7. YAML Project File Specification**

### **Specification Cross-Check (Section 7)**

- Standard-dependent fixed line counts and timing families are grounded in [ITU-R BT.470-6](../analogue-video-specifications/docs/video_formats/BT-470-6-1998/BT-470-6-1998.md), Annex 1, Table 1 and [SMPTE 170M-2004](../analogue-video-specifications/docs/video_formats/SMPTE-170M-2004/SMPTE-170M-2004.md) §1.1/§11.
- `signal_state_preset` locked-state constraints for 4fsc encoding are grounded in [SMPTE 244M-2003](../analogue-video-specifications/docs/video_formats/SMPTE-244M-2003/SMPTE-244M-2003.md) §3.1 and [EBU Tech. 3280-E](../analogue-video-specifications/docs/video_formats/EBU-Tech-3280-E/EBU-Tech-3280-E.md) §1.1.1.
- Laserdisc-specific YAML constraints are grounded in [IEC 60856](../analogue-video-specifications/docs/laserdisc/IEC-60856-1986-Laservision-PAL/IEC-60856-1986-Laservision-PAL.md) §9.1.4/§10.1 and [IEC 60857](../analogue-video-specifications/docs/laserdisc/IEC-60857-1986-Laservision-NTSC/IEC-60857-1986-Laservision-NTSC.md) §9.1.5/§10.1/§10.2.
- VITC structure, flags, CRC, and line-placement constraints are grounded in [IEC 60461:2010](../analogue-video-specifications/docs/video_metadata/IEC-60461-2010-Time-and-control-code/IEC-60461-2010-Time-and-control-code.md) §9.2.3, §9.2.5, §9.2.7, §9.6.2, §9.6.3.
- VITS type families are grounded in [PAL VITS Definitions](../analogue-video-specifications/docs/video_metadata/VITS/PAL-VITS.md) and [NTSC VITS Definitions](../analogue-video-specifications/docs/video_metadata/VITS/NTSC-VITS.md).

---

### **Current Implementation Status**

The current parser, validator, and runtime implement only a subset of the YAML surface described in this section:

- Implemented top-level presets: `video_standard_preset`, `sample_encoding_preset`, `signal_state_preset`, `ntsc_black_setup_ire`, `output.video_path`, and `output.signal_type` (`"composite"` or `"yc"`; defaults to `"composite"`). The metadata sidecar is not configured in YAML: it is always colocated with the video output and its path is derived from `output.video_path` (`.cvbs`/`.cvbsy` → `.meta`).
- `output.efm_audio` is parsed, emitted, and validated: its presence enables LaserDisc digital audio (EFM) output for the single channel pair named by `pair`. See [`efm_audio:`](#efm_audio-sub-key-optional-project-level).
- The `project:` block fields `name`, `version`, and `description` are parsed and retained on the in-memory `Project` model, so they survive load/save round-trips.
- A YAML project **emitter** (`YamlProjectEmitter` in `videosynth_core`) serialises an in-memory `Project` back to this schema. It writes fields in the canonical order shown below and emits only explicitly-set optional blocks (the project-level `line_injections:`, and the per-section `noise:`, `dropouts:`, `osd:`, `audio:`, `line_injections:`), so emitted files stay minimal and diffable. Emit → parse is lossless: a saved file parses back to an equal `Project`, which is the contract that keeps GUI-saved projects loadable by the CLI and vice versa.
- Implemented section fields: `name`, `type`, `source`, `start_frame`, `end_frame`, `duration_frames`, `duration_repeat`, and the optional per-section `noise:`, `dropouts:`, `osd:`, and `audio:` blocks.
- The `line_injections` schema is split across two levels of the data model. A **project-level** `line_injections:` block (a sibling of `output:` and `sections:`) is parsed into a `ProjectLineInjections` value on `Project` holding the project-wide `disc_type` (`CAV`/`CLV`), the optional `placement` policy (`standard` default / `laserdisc` / `custom`, governing how VITS `target_lines` are constrained — see [VITS](#vits-vertical-interval-test-signals)), and the `vits` set (each entry a `vits_type` plus `target_lines`) applied to every frame of every section. Each **section-level** `line_injections:` list holds only `Section::LineInjection` entries (`type` plus, per type, `target_lines`/`codes`) — the per-section biphase `codes` (picture_number, chapter_number, lead_in/out, etc.) that legitimately differ between lead-in, programme, and lead-out. Both levels receive validator-level schema/compatibility checks for injection type, `target_lines`, and standard-dependent VITS constraints.
- VITS line injections have a generation-stage orchestration path and are applied only on their targeted frame lines within the owning section span.
- Built-in VITS catalog entries now carry full waveform-definition primitive/composite trees for every supported `vits_type`, so the default runtime path can render all supported PAL and NTSC VITS patterns.
- `video_standard_preset: SECAM` and `secam_field_identification` (default `true`) are parsed, emitted, validated, and implemented; SECAM requires `signal_state_preset: STANDARD_STABLE_UNLOCKED` (the default when the key is omitted). See [SECAM Chroma Encoding Model](#secam-chroma-encoding-model).
- `pal_laserdisc_pilot_burst` is parsed, validated, and fully implemented: a 3.75 MHz (240 × f_H) sinusoidal burst at ±300 mV is superimposed on every sync pulse in the Y channel when enabled.
- `ntsc_laserdisc_vbi_burst` is parsed and validated for standard compatibility, but its runtime signal behavior remains deferred. Because a silent no-op would produce a file that looks compliant and is not, enabling it is a **validation error** ("ntsc_laserdisc_vbi_burst is parsed but not implemented in the current runtime") rather than being ignored.

The remainder of Section 7 should therefore be read as the intended project-file design rather than the currently implemented parser surface.

---

### **Top-Level Structure**

```yaml
project:
  name: "Example PAL CVBS Output"  # User-friendly name
  version: "1.0"                  # Project file version
  description: "A test output with colour bars and line injections"  # Optional

cvbs_presets:
  video_standard_preset: PAL     # PAL, NTSC, PAL_M, or SECAM (only one allowed per project)
  sample_encoding_preset: CVBS_U10_4FSC  # CVBS_U10_4FSC, CVBS_U16_4FSC, CVBS_TPG21_4FSC, CVBS_S16_4FSC, RAW_S16_28M, RAW_S16_40M
  signal_state_preset: STANDARD_STABLE_LOCKED  # STANDARD_STABLE_LOCKED; STANDARD_STABLE_UNLOCKED for SECAM (default: per standard)
  pal_laserdisc_pilot_burst: false  # PAL only; inject 3.75 MHz pilot burst on all sync pulses (IEC 60856 §9.1.2); default: false
  ntsc_laserdisc_vbi_burst: false   # NTSC only; insert colour burst on equalizing and broad sync pulses (IEC 60857 §9.1.2); default: false
  ntsc_black_setup_ire: 7.5     # NTSC only; allowed values: 7.5 (standard setup) or 0.0 (black = blanking); default: 7.5
  secam_field_identification: true  # SECAM only; field identification signals on lines 7-15 / 320-328; default: true
  # No other key is accepted. Field order/dominance follow the standard,
  # sample data is little-endian, and the locked/unlocked distinction is
  # carried by signal_state_preset itself — there is no separate mode key.

output:
  video_path: "out/pal_test_video.cvbs"
  signal_type: composite      # "composite" (default) or "yc"; for "yc", video_path must end in ".cvbsy"
  efm_audio:                  # Optional; LaserDisc digital audio (EFM) for one channel pair (PAL/NTSC only)
    pair: 0                   # Channel-pair number 0–7; must be declared by at least one section

line_injections:               # Project-wide laserdisc/VITS settings (sibling of output/sections)
  disc_type: CAV               # CAV or CLV; applies to every section (omit for non-laserdisc projects)
  placement: standard          # VITS placement policy: standard (default) | laserdisc | custom (omit for standard)
  vits:                        # Project-wide VITS set applied to every frame of every section
    - vits_type: "virs"
      target_lines: [10, 11, 12]

sections:
  - name: "Progressive Source with VITS and Laserdisc"
    type: progressive
    source: "assets/test.mkv"
    duration_frames: 10
    noise:                     # Optional noise injection for this section
      noise_db: 48.0           # Floor noise level; sets Black PSNR target [20.0–61.0 dB]
      noise_spread_db: 4.0     # White is 4 dB noisier than black; White SNR = noise_db - noise_spread_db
    audio:                     # Optional frame-locked audio test tone for this section
      waveform: sine           # sine, square, sawtooth, or triangle
      frequency: 1000.0        # Fixed-tone frequency in Hz [0, 22000]
      amplitude: 0.5           # Peak amplitude as a fraction of full scale [0.0, 1.0]
    line_injections:           # Per-section laserdisc codes / VITC for this section
      - type: laserdisc
        # No disc_type or target_lines — disc_type is project-wide; line
        # placement is fixed by IEC 60856 §10
        codes:
          - code_type: picture_number
            start_value: 1
          - code_type: picture_stop
          - code_type: chapter_number
            chapter: 5
          - code_type: programme_status
            programme_status: "0x8F0000"
```

#### **Asset Roots & Path Resolution**

Section `source` and `output` paths may reference a **logical asset root** with a leading `{name}` brace token, resolved to a real directory at runtime. This keeps a project portable: it references *what* an asset is, not *where* it physically lives, so projects survive reinstalls, repackaging, and moving between machines.

Built-in roots:

| Root | Meaning | Resolved location |
|------|---------|-------------------|
| `{bundled}` | Read-only assets shipped with the application | `$VIDEOSYNTH_ASSET_DIR`, else the installed `share/videosynth/assets` (found via XDG data dirs / QStandardPaths — includes Flatpak `/app/share`), else the dev `videosynth-assets/assets` directory. |
| `{user}` | The user's own writable asset library | `$XDG_DATA_HOME/videosynth/assets`, else `~/.local/share/videosynth/assets`. |
| `{project}` | The project file's own directory | Directory containing the `.yaml`. |
| `{output}` | Where this run's generated media is written | `$VIDEOSYNTH_OUTPUT_DIR` or `--output-root <path>`, else the project file's own directory. |

Resolution rules (shared by CLI and GUI via `ResolveProjectPaths` /
`ResolveAssetPath` in `path_resolution.h`), for each source/output path:

- **`{name}/rest`** → `rootDir(name) / rest` (a bare `{name}` resolves to the root directory itself). A root directory that is itself relative is anchored to the project file's directory. An **unknown** root name is a validation error.
- **Absolute or empty** → unchanged.
- **Plain relative (no token)** → the **CLI** leaves it working-directory-relative (preserving the run-from-a-base-dir convention used by the fixtures and `videosynth-assets/`); the **GUI** anchors it to the saved project file's directory so the source probe, preview, and generation resolve identically. Use `{project}/rest` for a plain relative path that must resolve the same under both.

`{output}` separates *what a project is called* from *where a particular run puts it*: a project names its artefacts relative to `{output}`, and the caller decides the directory. It defaults to the project's own directory, so a project stays self-contained when run by hand, while a batch run redirects every project at once (`scripts/run-projects.sh` sends each suite to `build/project-output/<suite>/`) and the functional test suites redirect it to a scratch directory so a test run never writes into the checked-in project tree.

The CLI seeds `{bundled}`/`{user}` from the environment and install prefix and accepts repeatable `--asset-root <name>=<path>` overrides (also usable to register additional named roots); `--output-root <path>` is shorthand for `--asset-root output=<path>`. The GUI resolves them via `QStandardPaths`. New GUI projects are saved to disk as part of the create flow so the `{project}` anchor always exists, and their default source uses `{bundled}` so a fresh project previews immediately. Newly added sections (plain and laserdisc templates alike) default to the same bundled 75% colour-bar source for the project standard's active raster rather than a placeholder file path.

Rather than expose these roots directly, the GUI section editor presents a section `source` as a two-way choice — **Built-in asset** or **Local file** — mapped onto the tokens above:

- **Built-in asset** composes `{bundled}/<type>/<raster>/<file>`, where `<type>` is the asset kind (`exr` still / `mkv` video), `<raster>` is derived from the project's video standard (720x576 PAL / 720x486 System-M, never a user field), and only `<file>` is chosen from a dropdown of the shipped assets. The path is always recomposed from the *current* project raster, so changing the video standard self-heals the source to the matching raster's asset.
- **Local file** is a browsed path stored as `{project}/…` when "Relative to project" is ticked or as an absolute path otherwise; a source already carrying another logical token (e.g. `{user}/…`) is preserved verbatim.

The compose/classify logic is a pure, Qt-free helper (`source_path_model.h`), independent of the resolution rules above.

#### **`noise:` Sub-Key (Optional, Per-Section)**

The `noise:` block enables per-section additive Gaussian noise injection targeting orc-gui Black PSNR and White SNR metrics.

| Key | Type | Required | Range | Description |
|-----|------|----------|-------|-------------|
| `noise_db` | float | Conditional | [20.0, 61.0] | Noise floor in dB; sets Black PSNR target. Required if `noise_spread_db` is present. |
| `noise_spread_db` | float | No | [0.0, `noise_db`−20.0] | White is this many dB noisier than black; White SNR target = `noise_db` − `noise_spread_db`. Defaults to 0.0. |

Rules:
- Neither key present → no noise injection (pass-through).
- Only `noise_db` present → floor-only noise; White SNR = Black PSNR = `noise_db`.
- Both present → two-component model; `noise_db − noise_spread_db ≥ 20.0` is enforced.
- `noise_spread_db` without `noise_db` → validation error.

**Lower bound (20.0 dB)**: at 20 dB, σ_noise ≈ 10 IRE ≈ 70 mV (PAL). Approaching the sync pulse amplitude; sync separator reliability degrades below this threshold.

**Upper bound (61.0 dB)**: above 61 dB, injected noise would be smaller than the 10-bit quantisation noise floor (~0.085 IRE) and would have no measurable effect in orc-gui.

#### **`dropouts:` Sub-Key (Optional, Per-Section)**

The `dropouts:` block enables per-section dropout simulation. Two independent dropout types are supported: `random` (surface/media degradation modelled as a Poisson process) and `scratch` (radial physical defect with a triangular amplitude envelope that spans multiple frames).

**`random:` block** — Poisson random dropouts:

| Key | Type | Required | Range | Description |
|-----|------|----------|-------|-------------|
| `scale` | int | Yes | [1, 20] | Severity scale. `0` disables random dropouts (equivalent to omitting the block). |
| `seed` | int | No | any int64 | Fixed RNG seed for reproducible output. If absent, a run-specific seed is used. |

**`scratch:` block** — persistent radial scratch defects:

| Key | Type | Required | Range | Description |
|-----|------|----------|-------|-------------|
| `scale` | int | Yes | [1, 20] | Severity scale. `0` disables scratch dropouts (equivalent to omitting the block). |
| `seed` | int | No | any int64 | Fixed RNG seed for reproducible output. If absent, a run-specific seed is used. |

**Scale mapping (exponential)**:

| Parameter | Scale 1 | Scale 10 | Scale 20 |
|-----------|---------|----------|---------|
| Random frequency (events/frame) | 0.05 | ~2.8 | 100 |
| Random max duration (samples) | 5 | ~282 | 2000 |
| Scratch count | 1 | 8 | 15 |
| Scratch max lifespan (frames) | 2 | ~113 | 500 |
| Scratch max peak width (samples) | 5 | ~282 | 2000 |

Rules:
- A `dropouts:` block with both `random.scale = 0` and `scratch.scale = 0` (or neither sub-block present) is a validation error — omit the `dropouts:` block entirely to disable.
- `scale` outside `[1, 20]` is a validation error.
- If `scratch.scale > 0` and the derived maximum scratch lifespan exceeds `section.duration_frames`, a **warning** is emitted (scratch events may never reach their peak amplitude within the section).

**Sidecar output**: when any section has dropout injection enabled, the pipeline creates a companion SQLite file at `<metadata_stem>.dropouts.meta` (where `<metadata_stem>` is the derived, video-colocated metadata path; schema version 5) with a single table `dropout_run(cvbs_file_id, frame_id, sample_start, sample_count, severity)`. Severity is `25` for non-visible VBI runs and `75` for runs intersecting the active picture.

Example YAML:

```yaml
sections:
  - name: "GoodLaserdisc"
    type: progressive
    source: "assets/clip.mkv"
    duration_frames: 50
    noise:
      noise_db: 50.0
    dropouts:
      random:
        scale: 5
        seed: 42
      scratch:
        scale: 3
```

---

#### **`audio:` Sub-Key (Optional, Per-Section)**

The `audio:` block declares a list of **audio channel pairs** for a section, each synthesising an independent stereo test tone for the exact duration (in frames) of that section. Following the CVBS File Format Specification (Audio Data / SMPTE 272M-1994), a project may carry up to **8 channel pairs** (0–7, i.e. up to 16 SMPTE 272M channels). Each declared channel pair is written as its own frame-locked stereo WAV track alongside the CVBS/Y-C output as `<basename>_audio_<pair>.wav` (the basename is derived from `output.video_path` by stripping a trailing `.cvbs` or `.cvbsy` suffix).

Design decisions:
- **One WAV file per channel pair** — the set of emitted pairs is the union of the pair numbers declared across all sections. Every pair file spans the whole output; a section that omits a pair emits silence for its frames, and an omitted `left`/`right` channel is stored as all-zero silence (SMPTE 272M §6.4).
- **Independent left/right channels** — each pair has separate `left:` and `right:` tone descriptors, so the two interleaved channels (odd = left, even = right) can differ or one can be silent.
- **Phase reset per section run** — each contiguous run of output frames sharing one section restarts the oscillators at phase 0; audio is a pure function of output position, so no per-frame caching is needed.

**Frame lock** — every track is stereo, 24-bit signed little-endian PCM (RIFF/WAVE, format tag `0x0001`) at 48000 Hz, synchronous with video (SMPTE 272M §1.2). The per-frame sample count follows output position:

| Preset | `fmt` `nSamplesPerSec` | Samples/frame |
|--------|------------------------|---------------|
| `PAL` | 48000 | 1920 (constant) |
| `NTSC` | 48000 | 1602/1601 (SMPTE 272M §14.3 five-frame sequence: 1602, 1601, 1602, 1601, 1602 → 8008 per 5 frames) |
| `PAL_M` | 48000 | 1602/1601 (as NTSC) |
| `SECAM` | 48000 | 1920 (constant, as PAL) |

**`audio:` fields**:

| Key | Type | Required | Description |
|-----|------|----------|-------------|
| `channel_pairs` | list | Yes | One entry per channel pair (below). |

**`channel_pairs[]` entry**:

| Key | Type | Required | Range / Values | Description |
|-----|------|----------|----------------|-------------|
| `pair` | int | Yes | 0–7 | Channel-pair number; names the `_audio_<pair>.wav` file and the `audio_channel_pair` metadata row. Unique within a section. |
| `description` | string | No | — | Human-readable label recorded in the `audio_channel_pair` metadata (e.g. `Analogue stereo`, `Commentary`). |
| `left` | map | No\* | tone descriptor (below) | The odd (first) interleaved channel. Omitted → silent. |
| `right` | map | No\* | tone descriptor (below) | The even (second) interleaved channel. Omitted → silent. |

\* At least one of `left`/`right` must be present (a pair with no active channel is rejected by validation).

**Channel tone descriptor** (`left:` / `right:`):

| Key | Type | Required | Range / Values | Description |
|-----|------|----------|----------------|-------------|
| `waveform` | string | No | `sine`, `square`, `sawtooth`, `triangle` | Oscillator shape. Default `sine`. Waveforms are naive (non-band-limited) — acceptable for test signals. |
| `frequency` | float | No | [0, 22000] Hz | Fixed-tone frequency, used when no `ramp:` block is present. Default `1000.0`. |
| `amplitude` | float | No | [0.0, 1.0] | Peak amplitude as a fraction of full scale. Default `0.5`. |
| `ramp:` | map | No | — | Optional frequency-sweep block (below); mutually exclusive with the fixed `frequency` mode. |

**`ramp:` block** — frequency sweep:

| Key | Type | Required | Range / Values | Description |
|-----|------|----------|----------------|-------------|
| `start` | float | Yes | [0, 22000] Hz | Sweep start frequency. |
| `end` | float | Yes | [0, 22000] Hz | Sweep end frequency. |
| `mode` | string | No | `up`, `down`, `bounce` | `up` = start→end, `down` = end→start, `bounce` = start→end→start over the ramp span. Default `up`. |
| `period` | float | No | [0, section duration s] | `0` (default) sweeps once over the whole section run; `> 0` makes one sweep last this many seconds and repeat until the section ends. |

Frequency semantics: instantaneous frequency drives a phase accumulator (`phase += 2π·f(t)/fs`) so swept tones stay continuous within a section run. All frequencies (fixed and ramp `start`/`end`) must lie in `[0, 22000]` Hz — safely below the 24 kHz Nyquist limit of the 48 kHz sampling rate.

**Output-stream alignment**: audio follows the **output** frame stream. The generator is driven with the output-order section sequence, preserving each track's sample-count alignment. Sections with no `audio:` block emit silence for their frame span across every pair. When at least one pair is declared, one `audio_channel_pair` row per pair (with its optional `description`) is written to the `.meta` database.

Example YAML:

```yaml
sections:
  - name: "StereoAndCommentary"
    type: progressive
    source: "assets/bars.exr"
    duration_frames: 8
    audio:
      channel_pairs:
        - pair: 0
          description: Analogue stereo
          left:  { waveform: sine, frequency: 1000.0, amplitude: 0.5 }
          right: { waveform: sine, frequency: 1000.0, amplitude: 0.5 }
        - pair: 1
          description: Commentary (left only)
          left:  { waveform: square, frequency: 440.0 }
          # right omitted -> silent
  - name: "SilenceGap"       # No audio: block — silent, but still frame-locked
    type: progressive
    source: "assets/bars.exr"
    duration_frames: 8
  - name: "SweepUp"
    type: progressive
    source: "assets/bars.exr"
    duration_frames: 8
    audio:
      channel_pairs:
        - pair: 0
          left:
            waveform: sawtooth
            amplitude: 0.5
            ramp: { start: 200.0, end: 4000.0, mode: up }
```

---

#### **`efm_audio:` Sub-Key (Optional, Project-Level)**

The `output.efm_audio:` block selects **one** audio channel pair to be additionally encoded as a LaserDisc digital audio (EFM) channel stream, written as `<basename>.efm` with a `<basename>.efm.meta` frame-index sidecar beside that pair's WAV track. The pair's WAV output and every other pair are unaffected; audio parameters (waveform, frequency, amplitude, ramps) are shared between the two paths and declared only in the per-section [`audio:`](#audio-sub-key-optional-per-section) blocks. The encoding is described in [LaserDisc Digital Audio (EFM) Output](#laserdisc-digital-audio-efm-output).

| Key | Type | Required | Range / Values | Description |
|-----|------|----------|----------------|-------------|
| `pair` | int | Yes | 0–7 | Channel pair to encode. Presence of the `efm_audio:` block is what enables the output; omit the block to disable it. |

Constraints:

- **PAL and NTSC only** — LaserDisc digital audio is defined by IEC 60856:1986 Amd 2 clause 13 (PAL) and IEC 60857:1986 Amd 2 clause 13 (NTSC); any other `video_standard_preset` is a validation error.
- **The pair must be declared** — if no section declares `pair`, a warning is emitted and no `.efm` file is written.
- **Track rules** — one track per `programme_area` section, maximum 79 (IEC 60856 Amd 2, 13.5.3.3 / IEC 60857 Amd 2, 13.6.3.3); tracks shorter than 4 s (6 s for track 1, whose first 2 s are the mandatory pause) and a project with no `lead_in` section (no TOC is emitted) produce warnings.

Example YAML:

```yaml
output:
  video_path: "out/pal_disc.cvbs"
  efm_audio:
    pair: 0
```

---

### **File Path Resolution**

Any field in a project YAML that accepts a file path (e.g., `source` on a `progressive` section) resolves the path using the following priority order:

1. **Built-in sources** — paths prefixed with `builtin:` refer to source files bundled and installed with the application. The prefix is followed by the built-in asset name (e.g., `builtin:smpte_leader`). The application resolves these against its installation data directory at runtime, regardless of where the project YAML is located.
2. **Absolute paths** — paths beginning with `/` are used exactly as specified. The file must exist at that location on the host filesystem.
3. **Project-relative paths** — all other paths are resolved relative to the **directory containing the project YAML file**. This allows a project and its associated assets to be grouped together and moved as a unit.

The application must report a clear validation error if a referenced file cannot be found after applying these rules.

#### **Examples**

```yaml
source: "builtin:smpte_leader"        # built-in asset bundled with the application
source: "/media/archive/clip.mkv"     # absolute path
source: "assets/clip.mkv"             # relative to the project YAML directory
source: "../shared/clip.mkv"          # relative path traversal is permitted
video_path: "out/pal_test_video.cvbs" # project-relative output file path
```

---

### **Validation Rules**

1. **Single CVBS Preset**:
  - Only one `video_standard_preset` (PAL or NTSC) per project.
  - Only one `sample_encoding_preset` per project.
  - Only one `signal_state_preset` per project.
  - `output.video_path` is required in the project YAML. The metadata sidecar path is derived from it (video-colocated) and is not specified in YAML.
  - `output.signal_type` must be `"composite"` or `"yc"` (default: `"composite"`).
  - When `output.signal_type` is `"yc"`, `output.video_path` must end in `".cvbsy"`.
  - `output.signal_type` must be `"composite"` or `"yc"` (default: `"composite"`).
  - When `output.signal_type` is `"yc"`, `output.video_path` must end in `".cvbsy"`; the chroma path is derived automatically by replacing the suffix with `".cvbsc"`.
  - Output resolution is fixed by the standard (720x576 for PAL and SECAM, 720x486 for NTSC and PAL-M) and must not be specified in the project file.
  - 4fsc generation requires a 4fsc `sample_encoding_preset` and a locked `signal_state_preset` (unlocked for SECAM).
  - `pal_laserdisc_pilot_burst` can **only be enabled for PAL projects**. If enabled for NTSC, the YAML is considered **invalid**.
  - `ntsc_laserdisc_vbi_burst` can **only be enabled for NTSC projects**. If enabled for PAL, the YAML is considered **invalid**.
  - `ntsc_black_setup_ire` can **only be specified for NTSC projects**. If specified for PAL, the YAML is considered **invalid**.
  - `ntsc_black_setup_ire` allowed values are `7.5` and `0.0`. `7.5` selects standards-based NTSC setup; `0.0` moves NTSC black level to blanking (`0 IRE`).
2. **Sections**:
    - Each section must have a valid `type` (`progressive`).
  - Each section must have at least one of:
      - Frame-based content (`progressive`).
    - Line injections (`line_injections`).
  - For `progressive` sections, `source` must point to a valid file.
3. **Line Injection Constraints**:
  - `target_lines` must be within the valid range for the standard (1-625 for PAL, 1-525 for NTSC).
  - `target_lines` must **not** be specified for `laserdisc` injection types; specifying it is a validation error.
  - **If injected lines overlap within a section, the YAML must fail validation.**
  - **When a `laserdisc` injection is active in a section, no other injection type may target lines within the laserdisc reserved ranges** (PAL: Field 1 lines 6–18, Field 2 lines 319–331; NTSC: Field 1 lines 10–18, Field 2 lines 273–281).
  - **A `vitc` injection and a `laserdisc` injection must not appear in the same section.** Laserdisc does not use VITC.
4. **Progressive Sources**:
  - `source` must resolve to an accessible file after applying [File Path Resolution](#file-path-resolution) rules. If the resolved path does not exist, the YAML is considered **invalid**.
  - `source` must match one of the supported progressive source profiles defined in [Section 8.1](#81-frame-based-sections); container extension alone is not sufficient.
  - For EXR sources, metadata attributes listed in [Progressive Sources](#progressive-sources) are required and validated.
  - Progressive source dimensions must be standard-consistent and exactly:
    - PAL: `720x576`
    - NTSC: `720x486`
  - Scaling/resizing is not supported. Any source outside the accepted dimensions is invalid.
  - Ingestion must not apply scaling, implicit aspect-ratio remap, or ad-hoc crop normalization.
  - `duration_frames` must be either:
    - A positive integer (fixed number of frames).
    - `"all"` (play the selected source range); an omitted `duration_frames` means `"all"`.
  - `duration_repeat` is an optional positive integer (default `1`) that replays the selected range `duration_repeat` times when `duration_frames: "all"`. Total output frames = range length × `duration_repeat`; each replay restarts at `start_frame`. It is ignored — and a validation warning is emitted — when `duration_frames` is a fixed integer.
  - The selected range is [`start_frame`, `end_frame`] (or [`start_frame`, last frame] without `end_frame`), resolved by `ResolveSourceFrameRange` (`source_frame_range.h`) for the schedule, the validator and the GUI alike. `start_frame` must be ≥ 0, `end_frame` ≥ `start_frame`, and both must lie inside the probed video; `end_frame` must not be combined with an integer `duration_frames` (the range already sets the duration).
  - An EXR still takes neither `start_frame` nor `end_frame`, and needs an integer `duration_frames`: it has no length of its own, so `"all"` does not apply.

---

---

## **8. Section Types**

### **Specification Cross-Check (Section 8)**

- Progressive-source frame-rate matching to output standards is grounded in [ITU-R BT.470-6](../analogue-video-specifications/docs/video_formats/BT-470-6-1998/BT-470-6-1998.md), Annex 1, Table 1 and [SMPTE 170M-2004](../analogue-video-specifications/docs/video_formats/SMPTE-170M-2004/SMPTE-170M-2004.md) §11.3.
- Laserdisc code-type definitions and per-line placement are grounded in [IEC 60856](../analogue-video-specifications/docs/laserdisc/IEC-60856-1986-Laservision-PAL/IEC-60856-1986-Laservision-PAL.md) §10.1 and [IEC 60857](../analogue-video-specifications/docs/laserdisc/IEC-60857-1986-Laservision-NTSC/IEC-60857-1986-Laservision-NTSC.md) §10.1/§10.2, plus Amendment 2 updates for CLV picture-number behavior.
- VITC fields (`timecode`, flags, CRC) are grounded in [IEC 60461:2010](../analogue-video-specifications/docs/video_metadata/IEC-60461-2010-Time-and-control-code/IEC-60461-2010-Time-and-control-code.md) §9.2 and §9.5-§9.6.

---

### **Current Implementation Status**

- **Implemented**: `progressive` frame-based sections in Section 8.1.
- **Implemented**: Section 8.2 parser/validator constraints for VITS line-injection schema, VITS/line-allocation compatibility checks, and generation-stage VITS line application for all supported built-in PAL and NTSC VITS patterns.
- **Implemented**: Full laserdisc biphase injection for PAL and NTSC (CAV and CLV), including 24-bit biphase signal generation, 40-bit FM signal generation (NTSC), all code types (lead_in, lead_out, picture_number, picture_stop, chapter_number, programme_status, users_code, programme_time_code, clv_code, clv_picture_number, fm_picture_number, fm_programme_time, fm_white_flag), field-aware line placement, timecode continuity, chapter stop-bit logic, NTSC frozen values, NTSC CLV colour time error correction, and biphase/VITS line-conflict validation.
- **Not yet implemented**: runtime synthesis/application for VITC and custom per-line content. A section-level injection whose `type` is neither `laserdisc` nor `vits` is a **validation error** ("line injection type '<type>' is not implemented in the current runtime"), so a project cannot silently generate a file that lacks the content it declares.

For VITC and custom per-line content paths, Section 8.2 remains the intended design contract for later implementation work.

---

### **8.1. Frame-Based Sections**

These define the **primary content** for a set of frames. The current runtime supports only progressive frame sources in this section.

---

#### **Progressive Sources**

Ingests progressive sources and converts them to interlaced signals, subject to strict profile validation.

##### **Supported Source Profiles (Normative)**

The extension selects the family (`.exr` still, `.mkv`/`.mp4` video, case-insensitive); the probe then checks the file itself:

- **Video (MKV or MP4)**: a Matroska (`.mkv`) or MP4 (`.mp4`) container — the probed demuxer must match the extension — with at least one video stream (the first is used) in any codec `ffmpeg` decodes. **Hard requirements:** raster `720x576` (PAL/SECAM) or `720x486` (NTSC/PAL-M), `r_frame_rate` 25 or 30000/1001 matching the standard, constant rate (`avg_frame_rate` within 0.2 % of `r_frame_rate`), and no stream crop side data. **Normalised** (see *Source Colour Space Requirement*): colour range, matrix, RGB coding, chroma format and bit depth. **Warnings:** a field-order tag opposite to the output's dominance (625-line `tb`, 525-line `bt` expected; `bt`/`bb` or `tb`/`tt` respectively warn) and a sample aspect ratio outside ±0.002 of 128:117 or 108:119. **Info:** the planned colour conversion is logged during validation. The probe also records the average frame rate, the container and video start times, and each audio stream's index, codec, channel count, sample rate, start time and duration.
- **EXR still image**: Single-frame OpenEXR scanline input with `R/G/B` channels in `FLOAT` (32-bit), no compression, full-raster data/display windows, standard-matching frame-rate metadata (`25/1` PAL or `30000/1001` NTSC), and matching pixel-aspect metadata.
- **BT.601 source-content compliance (bundled assets)**: the bundled MKV and EXR assets satisfy the source-content constraints documented in [MKV BT.601 Compliance Requirements](../../videosynth-assets/docs/mkv-bt601-compliance-requirements.md) and [EXR BT.601 Compliance Requirements](../../videosynth-assets/docs/exr-bt601-compliance-requirements.md), including sampling model interpretation, active-window placement, and padding expectations, enforced by the asset verification scripts rather than at runtime. User material is accepted on the hard requirements above.

The same rules apply to every source regardless of the asset root it resolves through.

##### **Fields**


| **Field**         | **Type**       | **Required** | **Description**                                                                | **Example**          |
| ----------------- | -------------- | ------------ | ------------------------------------------------------------------------------ | -------------------- |
| `name`            | string         | Yes          | User-friendly name.                                                            | `"Video Source"`     |
| `type`            | string         | Yes          | Must be `"progressive"`.                                                       | `"progressive"`      |
| `source`          | string         | Yes          | Path to the source file. May be a `builtin:` prefixed name, an absolute path, or a path relative to the project YAML. See [File Path Resolution](#file-path-resolution). | `"assets/test.mkv"` |
| `start_frame`     | integer        | No           | First source frame of the selected range, 0-based (default: `0`). Video only. | `1500`               |
| `end_frame`       | integer        | No           | Last source frame of the selected range, inclusive (default: the source's last frame). Video only; not combined with an integer `duration_frames`. Emitted only when set. | `2999`               |
| `duration_frames` | integer/string | No           | Output frames. `"all"` (the default when omitted) plays the selected range once; a positive integer plays that many frames from `start_frame`, looping the range when it is shorter (source index = `start_frame` + (i mod range length)). The emitter always writes it. | `100` or `"all"`     |
| `duration_repeat` | integer        | No           | Replay count when `duration_frames: "all"` (default `1`). Total frames = range length × repeat. Ignored for fixed integer durations. | `2`                  |


##### **Colour Space and Frame Rate**

- **Colour Space**: Source data is converted into 10-bit 4:4:4 YCbCr BT.601 studio swing at ingestion (see *Source Colour Space Requirement*).
- **Frame Rate**: **Fixed by the output standard** (25 fps for PAL, ~29.97 fps for NTSC). Input sources **must match** this frame rate.
- **BT.601 content requirement**: for the bundled assets, BT.601 compliance is a source-content requirement, not only a container/metadata requirement. User sources are shown pixel-for-pixel; content that is not BT.601-consistent (for example square-pixel material) is not rescaled.
- **10-bit studio-range preservation**:
  - Video and EXR supported profiles must preserve studio-domain sub-black and over-white excursions when present.

##### **Accepted Dimensions and Ingestion Behavior**

- PAL progressive sources: `720x576`.
- NTSC progressive sources: `720x486`.
- Scaling or resampling is **not supported** for progressive-source ingestion. Sources must already be in one of the accepted dimensions.
- Ingestion is fail-closed on unsupported or ambiguous rasters; there is no 704-width normalization path.
- Ingestion must not crop active-picture samples, must not crop source lines, and must not rewrite horizontal padding layout. The full source raster (`720x576` or `720x486`) is preserved through ingestion.
- Expected source-profile SAR and horizontal padding semantics for accepted progressive rasters are:
  - PAL `720x576`: SAR `128:117`, padding `8-704-8`.
  - NTSC `720x486`: SAR `108:119`, padding `8-704-8`.

##### **Example**

```yaml
sections:
  - name: "MKV with VITC"
    type: progressive
    source: "/assets/test.mkv"
    duration_frames: "all"  # Use all frames from the source
    duration_repeat: 2      # Optional: play the whole source twice
    line_injections:
      - type: vitc
        target_lines: [19, 20, 21]
        timecode: "00:00:00:00"
        flags:
          drop_frame: false
          colour_frame: true
          field_flag: 1
        crc: true
```

---

---

### **8.2. Line Injections**

Line-based content to inject into the **VBI of frames**.

Line injections live at two levels:

- **Project level** — the top-level `line_injections:` block (a sibling of `output:` and `sections:`) carries the project-wide `disc_type` (`CAV`/`CLV`) and the `vits` set. The VITS set is applied to every frame of every section, so a disc's reference/test signals are declared once for the whole disc. See [VITS](#vits-vertical-interval-test-signals) below.
- **Section level** — each section's own `line_injections:` list carries the per-section `laserdisc` code injections and any `vitc` injection, i.e. content that legitimately differs between sections (lead-in vs programme vs lead-out).

---

#### **Common Fields for All Line Injections**


| **Field**      | **Type** | **Required**              | **Description**                                                                                                                                                  | **Example**    |
| -------------- | -------- | ------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------- | -------------- |
| `type`         | string   | Section-level only        | Type of a section-level line injection: `laserdisc`, `vitc`, or `line_content`. (`vits` is not a section-level type — VITS is declared project-wide.)            | `"laserdisc"`  |
| `target_lines` | list     | Yes (except `laserdisc`)  | Lines to inject into (1-based, sequential across the entire frame). **Not applicable for `laserdisc` injections** — line placement is fixed by the standard.     | `[10, 11, 12]` |


---

#### **VITS (Vertical Interval Test Signals)**

VITS are a **project-level** signal set. Each VITS entry lives in the top-level `line_injections.vits` list and is applied to every frame of every section — you do not repeat it per section. For laserdisc discs this is where the mandatory NTSC `virs` reference is declared.

**Placement modes.** The optional `line_injections.placement` field selects how the validator constrains each VITS entry's `target_lines`:

- `standard` (default; also assumed when the field is absent) — each VITS type must sit on its broadcast recommended line (`vits17` → 17, `ntc7-composite` → 17, etc.). Any other line is rejected. This preserves the behaviour of projects authored before the field existed.
- `laserdisc` — VITS must sit on the laserdisc VBI lines, which are clear of the address-code ranges: **PAL** 19, 20, 332, 333 (IEC 60856 §9.1.3); **NTSC/PAL-M** 19, 20, 282, 283 (IEC 60857 §9.1.3 VIRS on 19/282, §9.1.4 composite/combination ITS on 20/283). The editor's one-click laserdisc preset seeds the spec-required set (PAL: `uk-national` on 19/332, `vits20` on 20/333; NTSC: `virs` on 19/282, `ntc7-composite` on 20, `ntc7-combination` on 283).
- `custom` — VITS may sit on any valid VBI line; the validator still rejects overlapping lines and, on a disc carrying biphase codes, the reserved address-code ranges (see §8.2 reserved-range rule).

For **VITS waveform definitions**, refer to:

- [NTSC VITS Definitions](../analogue-video-specifications/docs/video_metadata/VITS/NTSC-VITS.md)
- [PAL VITS Definitions](../analogue-video-specifications/docs/video_metadata/VITS/PAL-VITS.md)

Valid `vits_type` values depend on the active video standard. PAL types must only be used in PAL projects; NTSC types must only be used in NTSC projects.

##### **Fields**


| **Field**   | **Type** | **Required** | **Description**                                             | **Example**         |
| ----------- | -------- | ------------ | ----------------------------------------------------------- | ------------------- |
| `vits_type` | string   | Yes          | Type of VITS waveform. Valid values depend on the standard. | `"ntc7-composite"`  |


##### **`vits_type` Options — PAL**


| **VITS Type**     | **Description**                                                         | **Reference**                                                                                               |
| ----------------- | ----------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------- |
| `vits17`          | EBU/CCIR primary insertion test signal (VBI line 17, field 1).          | [PAL VITS Definitions](../analogue-video-specifications/docs/video_metadata/VITS/PAL-VITS.md)               |
| `itu-multiburst`  | ITU multiburst signal for PAL systems B, D, G, H, I (VBI line 18, field 1). | [PAL VITS Definitions](../analogue-video-specifications/docs/video_metadata/VITS/PAL-VITS.md)          |
| `uk-national`     | IBA/EBU UK national insertion test signal for PAL-I (VBI line 19, field 1). | [PAL VITS Definitions](../analogue-video-specifications/docs/video_metadata/VITS/PAL-VITS.md)           |
| `vits20`          | IBA/EBU UK ITS-2 chrominance amplitude reference signal (VBI line 20, field 1). | [PAL VITS Definitions](../analogue-video-specifications/docs/video_metadata/VITS/PAL-VITS.md)      |
| `itu-composite`   | ITU PAL composite insertion test signal, BT.628/BT.473 (VBI line 330, field 2). | [PAL VITS Definitions](../analogue-video-specifications/docs/video_metadata/VITS/PAL-VITS.md)      |
| `itu-combination` | ITU combination insertion test signal for PAL, BT.473 (VBI line 331, field 2). | [PAL VITS Definitions](../analogue-video-specifications/docs/video_metadata/VITS/PAL-VITS.md)       |


##### **`vits_type` Options — NTSC**


| **VITS Type**      | **Description**                                                                        | **Reference**                                                                                                |
| ------------------ | -------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------ |
| `ntc7-composite`   | NTC-7 composite insertion test signal, EIA RS-498 / SMPTE RP 168 (VBI line 17, field 1).   | [NTSC VITS Definitions](../analogue-video-specifications/docs/video_metadata/VITS/NTSC-VITS.md)         |
| `ntc7-combination` | NTC-7 combination insertion test signal, EIA RS-498 / SMPTE RP 168 (VBI line 280, field 2). | [NTSC VITS Definitions](../analogue-video-specifications/docs/video_metadata/VITS/NTSC-VITS.md)        |
| `fcc-multiburst`   | FCC multi-burst insertion test signal, FCC Part 73 / EIA RS-498 (VBI line 18, field 1).    | [NTSC VITS Definitions](../analogue-video-specifications/docs/video_metadata/VITS/NTSC-VITS.md)         |
| `fcc-composite`    | FCC composite insertion test signal, FCC Part 73 / EIA RS-498 (VBI line 281, field 2).     | [NTSC VITS Definitions](../analogue-video-specifications/docs/video_metadata/VITS/NTSC-VITS.md)         |
| `virs`             | Video Index Reference Signal, SMPTE RP 168 / EIA RS-498. Format identification and level calibration aid for NTSC recordings. | [NTSC VITS Definitions](../analogue-video-specifications/docs/video_metadata/VITS/NTSC-VITS.md) |


##### **Example**

```yaml
# Top-level block (sibling of output/sections)
line_injections:
  vits:
    - vits_type: "ntc7-composite"
      target_lines: [17]
```

---

#### **Laserdisc Biphase Encoding**

For **Laserdisc biphase encoding**, refer to:

- [IEC 60856](../analogue-video-specifications/docs/laserdisc/IEC-60856-1986-Laservision-PAL/IEC-60856-1986-Laservision-PAL.md) (and Amendments 1–2): Laservision PAL
- [IEC 60857](../analogue-video-specifications/docs/laserdisc/IEC-60857-1986-Laservision-NTSC/IEC-60857-1986-Laservision-NTSC.md) (and Amendments 1–2): Laservision NTSC

> **Important**: For `laserdisc` injections, **`target_lines` must not be specified**. VBI line placement is entirely determined by the standard: each code type is assigned to specific lines based on the `disc_type` and the current field identity (which field opens the picture). User-specified `target_lines` on a `laserdisc` injection will fail validation.

Laserdisc VBI encoding is a **frame-level operation**: both fields of each frame carry codes on specific lines, with many codes split across fields or placed conditionally based on which field is the first field of the picture. This field-conditional placement is intrinsic to the standard and is handled automatically by the encoder.

##### **Reserved VBI Address Ranges**

The following VBI lines are reserved exclusively for laserdisc address/data signals. No other injection type may target lines within these ranges when a `laserdisc` injection is active in the same section:

| **Standard** | **Field** | **Reserved Line Range** | **Reference**    |
| ------------ | --------- | ----------------------- | ---------------- |
| PAL          | Field 1   | 6–18                    | IEC 60856 §9.1.4 |
| PAL          | Field 2   | 319–331                 | IEC 60856 §9.1.4 |
| NTSC         | Field 1   | 10–18                   | IEC 60857 §9.1.5 |
| NTSC         | Field 2   | 273–281                 | IEC 60857 §9.1.5 |

> **PAL note**: Lines 6–15 within the Field 1 reserved range are occupied by equalizing and vertical sync pulses and carry no biphase content. The active biphase insertion lines within the reserved range are **16–18** (Field 1) and **329–331** (Field 2).
>
> **NTSC note**: Lines 10–15 are within the reserved range; lines 10–11 and 273–274 additionally carry the 40-bit FM coded signal (see below). The active 24-bit biphase insertion lines are **16–18** (Field 1) and **279–281** (Field 2). Because the reserved range spans 10–18, **NTSC NTC-7 VITS types that target lines 17 and 280 cannot coexist with a laserdisc injection** in the same section and will fail validation.

##### **Code Systems**

Two coding systems are defined:

1. **24-bit biphase coded signal** (PAL and NTSC, §10.1): The primary address/control code system. Each bit cell is 2 µs; the digital level ranges from 30% to 100% white (PAL) or 0 to 100 IRE (NTSC). The 24-bit payload is subdivided into 6 hex nibbles; the first nibble is always the key and begins with a logic-one bit.

2. **40-bit FM coded signal** (NTSC only, §10.2): An additional signal on lines 10/273 (data) and 11/274 (white flag). It provides picture number information (CAV) or programme time code (CLV) independently of the 24-bit biphase system and is always present alongside it on compliant NTSC discs.

##### **24-bit Biphase Code Types and Line Placement**

Lines are assigned per code type and field identity. "First field of picture" is the field that carries the picture number (CAV) or programme time code (CLV); "second field of picture" is the other field.

**PAL — IEC 60856 §10.1 (as amended):**

| **Code Type**              | **First-Field Lines** | **Second-Field Lines** | **Priority / Notes**                                                                                     |
| -------------------------- | --------------------- | ---------------------- | -------------------------------------------------------------------------------------------------------- |
| Lead-in                    | 17, 18                | 330, 331               | Both fields; ≥ 1.5 mm of tracks before programme start. Code: `88FFFF`                                 |
| Lead-out                   | 17, 18                | 330, 331               | Both fields; ≥ 2 mm of tracks after programme end. Code: `80EEEE`                                      |
| Picture number (CAV)       | 17, 18                | —                      | First field only. Code: `FX₁X₂X₃X₄X₅`; max 99 999; auto-increments each frame.                        |
| Picture stop (CAV)         | —                     | 329, 330               | Second field only (field after picture-number field). Code: `82CFFF`; overrides programme status on 329. |
| Chapter number (CAV)       | 17, 18                | 330, 331               | Fields without picture number. Lines 17/330 are lower priority than picture stop. Code: `8X₁X₂DDD`; max 79. |
| Programme time code (CLV)  | 17, 18                | —                      | First field only. Code: `FX₁DDX₂X₃`; X₁ = hours; X₂X₃ = minutes; auto-updates each frame.             |
| CLV code                   | 17 or 330             | —                      | Fields without programme time code and CLV picture number. Code: `87FFFF`                               |
| CLV picture number         | 16 or 329             | —                      | First field only (Amendment 2). Code: `8X₁EX₃X₄X₅`; auto-updates each frame.                          |
| Chapter number (CLV)       | 18 or 331             | —                      | Fields without programme time code and CLV picture number. Same hex format as CAV.                      |
| Programme status (CAV)     | 16, 329               | —                      | Every frame. Picture stop has priority on line 329. Code: `8DC/BAX₃X₄X₅` (see Appendix C).              |
| Programme status (CLV)     | 16 or 329             | —                      | Same fields as CLV code insertion. Code: `8DC/BAX₃X₄X₅`                                                |
| Users code                 | 16, 329               | —                      | Lead-in and/or lead-out area only. Code: `8X₁DX₃X₄X₅`                                                  |

**NTSC — IEC 60857 §10.1 (as amended):**

| **Code Type**              | **First-Field Lines** | **Second-Field Lines** | **Priority / Notes**                                                                                      |
| -------------------------- | --------------------- | ---------------------- | --------------------------------------------------------------------------------------------------------- |
| Lead-in                    | 17, 18                | 280, 281               | Both fields; ≥ 1.5 mm of tracks before programme start. Code: `88FFFF`                                  |
| Lead-out                   | 17, 18                | 280, 281               | Both fields; ≥ 2 mm of tracks after programme end. Code: `80EEEE`                                       |
| Picture number (CAV)       | 17, 18                | —                      | First field only. Code: `FX₁X₂X₃X₄X₅`; max 79 999 (Amendment 2); auto-increments each frame.           |
| Picture stop (CAV)         | —                     | 279, 280               | Second field only (field after picture-number field). Code: `82CFFF`; overrides programme status on 279. |
| Chapter number (CAV)       | 17, 18                | 280, 281               | Fields without picture number. Lines 17/280 are lower priority than picture stop. Code: `8X₁X₂DDD`; max 79. |
| Programme time code (CLV)  | 17, 18                | —                      | First field only. Code: `FX₁DDX₂X₃`; auto-updates each frame.                                           |
| CLV code                   | 17 or 280             | —                      | Fields without programme time code and CLV picture number. Code: `87FFFF`                                |
| CLV picture number         | 16 or 279             | —                      | First field only (Amendment 2). Code: `8X₁EX₃X₄X₅`; auto-updates each frame.                           |
| Chapter number (CLV)       | 18 or 281             | —                      | Fields without programme time code and CLV picture number.                                               |
| Programme status (CAV)     | 16, 279               | —                      | Every frame. Picture stop has priority on line 279. Code: `8DC/BAX₃X₄X₅`                                |
| Programme status (CLV)     | 16 or 279             | —                      | Same fields as CLV code insertion. Code: `8DC/BAX₃X₄X₅`                                                 |
| Users code                 | 16, 279               | —                      | Lead-in and/or lead-out area only. Code: `8X₁DX₃X₄X₅`                                                   |

##### **40-bit FM Coded Signal (NTSC only — IEC 60857 §10.2)**

This signal co-exists with the 24-bit biphase system and occupies separate lines within the laserdisc reserved range. It is mandatory on compliant NTSC discs and is enabled by including the appropriate `code_type` values in the `codes` list.

| **Code Type**                   | **Field 1 Line** | **Field 2 Line** | **Notes**                                                                                           |
| ------------------------------- | ---------------- | ---------------- | --------------------------------------------------------------------------------------------------- |
| Lead-in white flag              | 11               | —                | Present for ≥ tracks corresponding to 1.5 mm before programme start.                               |
| Lead-out white flag             | 11               | 274              | Present for ≥ 600 tracks after programme end.                                                       |
| Picture numbers (CAV)           | 10               | 273              | Always both lines; picture number updated on second field of each picture; max 79 999 (Amendment 2); auto-increments. |
| White flag (first-field marker) | 11 or 274        | —                | Line 11 = field that opens the picture; line 274 = the other field. Controls player still-picture logic. |
| Programme time code (CLV)       | 10               | 273              | Always both lines; minutes and seconds; auto-updates each frame.                                    |

##### **Fields**

| **Field**   | **Type** | **Required** | **Description**                            | **Example** |
| ----------- | -------- | ------------ | ------------------------------------------ | ----------- |
| `codes`     | list     | Yes          | List of biphase codes to encode per frame. | (See below) |

> **Note**: `disc_type` is **not** specified on a per-section `laserdisc` injection. It is a project-wide setting (`CAV` or `CLV`) declared once in the top-level `line_injections:` block, since a disc is entirely CAV or entirely CLV.
>
> **Note**: `target_lines` must **not** be specified for `laserdisc` injections.

##### **`codes` Item Fields**

| **Field**           | **Type**   | **Required**           | **Description**                                                                                 | **Example**    |
| ------------------- | ---------- | ---------------------- | ----------------------------------------------------------------------------------------------- | -------------- |
| `code_type`         | string     | Yes                    | Type of biphase code (see tables above for valid values per disc format).                       | `"picture_number"` |
| `start_value`       | integer    | Optional               | Anchors the count for `picture_number`/`fm_picture_number` at this section. Omit to continue the running count from the previous section (see [Timecode Continuity](../user/biphase-design.md#5-timecode-continuity)). | `1`        |
| `chapter`           | integer    | Optional               | Chapter number (0–79) for `chapter_number`. Omit to continue the previous section's chapter.     | `1`            |
| `programme_status`  | hex string | For `programme_status` | 24-bit programme status value per IEC 60856/60857 Appendix C (audio/video channel use).         | `"0x8F0000"`   |
| `users_code`        | hex string | For `users_code`       | 24-bit users code value; first nibble must be 8; `X₁` = 0–7; see IEC 60856/60857 §10.1.9.      | `"0x810123"`   |

These five keys are the complete accepted set; any other key in a `codes[]` entry is a parse error. The CLV clocks (`programme_time_code`, `clv_picture_number`) take **no** parameters — they are anchored at `0:00` at the start of the programme area and advance automatically, as IEC 60856/60857 §10.1.5 requires.

##### **Valid `code_type` Values — CAV**

| **`code_type`**    | **Hex Format**   | **Description**                                                                                        |
| ------------------ | ---------------- | ------------------------------------------------------------------------------------------------------ |
| `lead_in`          | `88FFFF`         | Lead-in marker; inserted for ≥ 1.5 mm of tracks before programme start.                               |
| `lead_out`         | `80EEEE`         | Lead-out marker; inserted for ≥ 2 mm of tracks after programme end.                                   |
| `picture_number`   | `FX₁X₂X₃X₄X₅`   | Frame address; max 99 999 (PAL) / 79 999 (NTSC); auto-increments each frame, anchored by `start_value` where present. |
| `picture_stop`     | `82CFFF`         | Inserted in the field following the picture-number field to trigger player still-frame mode.           |
| `chapter_number`   | `8X₁X₂DDD`       | Chapter identifier (0–79); uses the optional `chapter` field.                                          |
| `programme_status` | `8DC/BAX₃X₄X₅`   | Audio/video channel use identification per Appendix C; requires `programme_status` field.             |
| `users_code`       | `8X₁DX₃X₄X₅`    | Optional user identification; lead-in/lead-out only; requires `users_code` field.                     |
| `fm_picture_number`| (40-bit FM)      | **NTSC only.** 40-bit FM coded picture number on lines 10/273; auto-increments alongside `picture_number`. |
| `fm_white_flag`    | (100 IRE white)  | **NTSC only.** First-field white flag on line 11 or 274.                                               |

##### **Valid `code_type` Values — CLV**

| **`code_type`**         | **Hex Format**  | **Description**                                                                                         |
| ----------------------- | --------------- | ------------------------------------------------------------------------------------------------------- |
| `lead_in`               | `88FFFF`        | Lead-in marker; inserted for ≥ 1.5 mm of tracks before programme start.                                |
| `lead_out`              | `80EEEE`        | Lead-out marker; inserted for ≥ 2 mm of tracks after programme end.                                    |
| `programme_time_code`   | `FX₁DDX₂X₃`    | Running time (hours + minutes); starts at `0:00` and auto-updates each frame. Takes no parameters.      |
| `clv_code`              | `87FFFF`        | CLV format indicator; on fields without programme time code or CLV picture number.                      |
| `clv_picture_number`    | `8X₁EX₃X₄X₅`   | Frame identifier within second (seconds + picture count); auto-updates each frame. Takes no parameters. |
| `chapter_number`        | `8X₁X₂DDD`      | Chapter identifier (0–79); uses the optional `chapter` field.                                          |
| `programme_status`      | `8DC/BAX₃X₄X₅`  | Audio/video channel use identification per Appendix C; requires `programme_status` field.              |
| `users_code`            | `8X₁DX₃X₄X₅`   | Optional user identification; lead-in/lead-out only; requires `users_code` field.                      |
| `fm_programme_time`     | (40-bit FM)     | **NTSC only.** 40-bit FM coded programme time (minutes + seconds) on lines 10/273; auto-updates.       |
| `fm_white_flag`         | (100 IRE white) | **NTSC only.** First-field white flag on line 11 or 274.                                               |

##### **Example (PAL CAV)**

`disc_type` (CAV vs CLV) is set once in the project-level `line_injections:` block; the per-section fragments below carry only the `codes:`.

```yaml
# Project-level: line_injections: { disc_type: CAV }
line_injections:
  - type: laserdisc
    # No disc_type or target_lines — disc_type is project-wide; line placement
    # is fixed by IEC 60856 §10
    codes:
      - code_type: picture_number
        start_value: 1          # Auto-increments each frame
      - code_type: picture_stop
      - code_type: chapter_number
        chapter: 1
      - code_type: programme_status
        programme_status: "0x8F0000"
```

##### **Example (NTSC CLV)**

```yaml
# Project-level: line_injections: { disc_type: CLV, vits: [ { vits_type: virs, target_lines: [19, 282] } ] }
line_injections:
  - type: laserdisc
    # No disc_type or target_lines — disc_type is project-wide; line placement
    # is fixed by IEC 60857 §10
    codes:
      - code_type: programme_time_code  # Starts at 0:00; no parameters
      - code_type: clv_code
      - code_type: clv_picture_number   # Auto-updates each frame; no parameters
      - code_type: chapter_number
        chapter: 1
      - code_type: programme_status
        programme_status: "0x8F0000"
      - code_type: fm_programme_time    # 40-bit FM signal (NTSC mandatory)
      - code_type: fm_white_flag
```

---

#### **VITC (Vertical Interval Timecode)**

> **Note**: VITC is **incompatible with laserdisc biphase encoding**. Laserdisc discs use the IEC biphase system as their sole timecode and address mechanism; VITC is not used on laserdisc and must not be combined with a `laserdisc` injection in the same section. Doing so will fail validation.

For **VITC timecode**, refer to:

- [SMPTE 12M](../analogue-video-specifications/docs/video_metadata/IEC-60461-2010-Time-and-control-code/IEC-60461-2010-Time-and-control-code.md): Vertical Interval Timecode

##### **Fields**


| **Field**   | **Type** | **Required** | **Description**                           | **Example**             |
| ----------- | -------- | ------------ | ----------------------------------------- | ----------------------- |
| `timecode`  | string   | Yes          | Timecode in `HH:MM:SS:FF` format.         | `"01:23:45:12"`         |
| `flags`     | object   | No           | VITC flags.                               | `{ drop_frame: false }` |
| `crc`       | boolean  | No           | Enable CRC-16 checksum (default: `true`). | `true`                  |
| `user_data` | string   | No           | Optional 16-bit user data (hex).          | `"0x1234"`              |


##### `**flags` Fields**


| **Field**      | **Type** | **Required** | **Description**                             | **Example** |
| -------------- | -------- | ------------ | ------------------------------------------- | ----------- |
| `drop_frame`   | boolean  | No           | Drop-frame flag for NTSC.                   | `false`     |
| `colour_frame` | boolean  | No           | Colour frame flag.                          | `true`      |
| `field_flag`   | integer  | No           | Field flag: `1` (Field 1) or `2` (Field 2). | `1`         |


##### **Example**

```yaml
line_injections:
  - type: vitc
    target_lines: [19, 20, 21]
    timecode: "01:23:45:12"
    flags:
      drop_frame: false
      colour_frame: true
      field_flag: 1
    crc: true
    user_data: "0x1234"
```

---

---

## **9. Field and Line Handling**

### **Specification Cross-Check (Section 9)**

- PAL/NTSC frame/field line totals and scanning relationships are grounded in [ITU-R BT.470-6](../analogue-video-specifications/docs/video_formats/BT-470-6-1998/BT-470-6-1998.md), Annex 1, Table 1 and [SMPTE 170M-2004](../analogue-video-specifications/docs/video_formats/SMPTE-170M-2004/SMPTE-170M-2004.md) §1.1/§9/§13.
- Horizontal sync width (`4.7 us`), equalizing pulse width (`2.3 us`), vertical-sync pulse block durations (`2.5H` PAL, `3H` NTSC), and burst phase conventions are grounded in [ITU-R BT.1700](../analogue-video-specifications/docs/video_formats/BT-1700-E/BT-1700-E.md), Table 2/Table 3/Figures 5/8/9, and [SMPTE 170M-2004](../analogue-video-specifications/docs/video_formats/SMPTE-170M-2004/SMPTE-170M-2004.md) §13.1-§13.3.
- PAL laserdisc pilot burst and NTSC VBI burst behavior are grounded in [IEC 60856](../analogue-video-specifications/docs/laserdisc/IEC-60856-1986-Laservision-PAL/IEC-60856-1986-Laservision-PAL.md) §9.1.2 and [IEC 60857](../analogue-video-specifications/docs/laserdisc/IEC-60857-1986-Laservision-NTSC/IEC-60857-1986-Laservision-NTSC.md) §9.1.2.

For **field and line handling**, refer to:

- [ITU-R BT.470-6](../analogue-video-specifications/docs/video_formats/BT-470-6-1998/BT-470-6-1998.md): Conventional Television Systems.

---

### **Line Numbering**

VideoSynth generates frames as **two sequential fields** written one after the other. There is no interlacing — lines are numbered and processed purely sequentially within the frame. The line numbering scheme follows the analogue standard line counts (625 for PAL, 525 for NTSC), where each line number maps directly to a position in the sequential output.

- **Lines are numbered 1 to 625 (PAL) or 1 to 525 (NTSC), sequentially across the frame in field order.**
- **Field 1** occupies the first half of the frame: lines 1–312 (PAL) or lines 1–262 (NTSC).
- **Field 2** occupies the second half of the frame: lines 313–625 (PAL) or lines 263–525 (NTSC).
- **VBI Lines**: Lines 1–22 (PAL) and 1–21 (NTSC) in each field are the VBI region available for metadata injection (e.g., VITS, VITC, Laserdisc biphase).

> **Note**: This numbering is the generator's internal sequential line addressing. It does not imply anything about how the signal is displayed on an analogue monitor — the analogue receiver is responsible for interpreting the two sequential fields as an interlaced image. VideoSynth itself performs no interlacing.

---

### **Progressive Source to Two-Field Frame Conversion**

When ingesting a progressive source, the generator splits each progressive frame into **two fields** and writes them sequentially into the output frame. This is a spatial line-splitting operation — not interlacing. The output is two fields in sequence, each containing half the source lines.

1. **Split Fields**:
  - For a progressive source frame of height `H`:
    - **Field 1**: Odd source lines (1, 3, 5, ..., H-1), written sequentially as lines 1, 2, 3, ... of Field 1.
    - **Field 2**: Even source lines (2, 4, 6, ..., H), written sequentially as lines 1, 2, 3, ... of Field 2.
  - **PAL Example (720×576 progressive source)**:
    - Field 1: source lines 1, 3, 5, ..., 575 → 288 lines.
    - Field 2: source lines 2, 4, 6, ..., 576 → 288 lines.
  - **NTSC Example (720×486 progressive source)**:
    - Field 1: source lines 1, 3, 5, ..., 485 → 243 lines.
    - Field 2: source lines 2, 4, 6, ..., 486 → 243 lines.
2. **Field Order**:
  - **PAL**: Field 1 is always written first, then Field 2.
  - **NTSC**: Field 1 is written first, then Field 2 (native 29.97 fps sources).
3. **Field Dominance**:
  - **PAL**: Field 1 is **temporally first** (dominant).
  - **NTSC**: Field 2 is **temporally first** for 3:2 pulldown sequences (24 fps sources).

---

### **Sync and Burst Insertion**

For **HSync, VSync, and colour burst insertion**, refer to:

- [ITU-R BT.470-6](../analogue-video-specifications/docs/video_formats/BT-470-6-1998/BT-470-6-1998.md)
- [ITU-R BT.1700](../analogue-video-specifications/docs/video_formats/BT-1700-E/BT-1700-E.md)

1. **Horizontal Sync (HSync)**:
  - Insert a **4.7 µs pulse at 0V** at the start of each line.
  - Followed by **back porch (0.3V)** and **colour burst (2.25 µs, 0.3V p-p)**.
  - **PAL**: Burst phase polarity follows the BT.1700 Table 1 item 10f sequence (I/II/III/IV over colour fields), with nominal **±135°** phase per line and sequence-dependent odd/even polarity assignment.
  - **NTSC**: Burst phase follows continuous-subcarrier line progression (line-to-line $\pi$ alternation at 4fsc in the current implementation).
2. **Vertical Sync (VSync)**:
  - **PAL**:
    - **Lines 623-625**: 5 broad pulses (2.5H each) with equalizing pulses.
    - **Equalizing Pulses**: 5 pulses before/after VSync (2.3 µs each).
  - **NTSC**:
    - **Lines 523-525**: 3 broad pulses (3H each) with equalizing pulses.
    - **Equalizing Pulses**: 6 pulses before/after VSync (2.3 µs each).
  - **PAL burst blanking during vertical interval**: burst suppression windows follow BT.1700 Figure 8 sequence windows:
    - Sequence I: lines 623–625 and 1–6.
    - Sequence II: lines 310–318.
    - Sequence III: lines 622–625 and 1–5.
    - Sequence IV: lines 311–319.
3. **PAL Laserdisc Pilot Burst** (when `pal_laserdisc_pilot_burst: true`):
  - Superimpose a continuous **3.75 MHz** ($240 \times f_H$) sinusoidal burst on the **sync pulse level** of every horizontal and vertical sync pulse in every line (see [IEC 60856](../analogue-video-specifications/docs/laserdisc/IEC-60856-1986-Laservision-PAL/IEC-60856-1986-Laservision-PAL.md) §9.1.2).
  - Amplitude: $\frac{6}{7} \times 700\ \text{mV} = 600\ \text{mV p-p}$, centred on sync tip (−300 mV), so the burst swings between **−600 mV and 0 mV**.
  - The pilot burst is applied to the full duration of every sync pulse (line-sync, broad/vertical sync, and equalizing pulses).
  - $17\,734\,475 = 25 \times 709\,379$, so the pilot advances a whole number of cycles per PAL frame and its waveform is identical on every frame. The runtime therefore derives each burst's phase from its offset **within** the frame — not from the absolute sample index — and renders every burst of the frame once per run.
  - This extends the signal range 300 mV below standard sync tip; see §6.1 for the headroom rationale and the required internal floating-point representation.
4. **NTSC Laserdisc VBI Burst** (when `ntsc_laserdisc_vbi_burst: true`):
  - Insert the standard NTSC **colour burst (3.579545 MHz)** on the back porch of **equalizing pulses and broad (field) sync pulses** during the vertical interval, where burst is normally suppressed in standard NTSC (see [IEC 60857](../analogue-video-specifications/docs/laserdisc/IEC-60857-1986-Laservision-NTSC/IEC-60857-1986-Laservision-NTSC.md) §9.1.2).
  - The burst uses the same amplitude and phase as regular NTSC colour burst; no new frequency or amplitude is introduced.
  - This applies to all equalizing pulse and broad sync pulse lines in both Field 1 (lines 1–9) and Field 2 (lines 263–271).

---

---

## **10. 4fsc Sampling and Subcarrier Locking**

### **Specification Cross-Check (Section 10)**

- Core 4fsc sampling rule is grounded in [SMPTE 244M-2003](../analogue-video-specifications/docs/video_formats/SMPTE-244M-2003/SMPTE-244M-2003.md) §3.1 (sample at `4 x fsc`, phase-referenced to subcarrier).
- NTSC `14.31818 MHz` and sample-axis relationship (I/Q axes) are grounded in [SMPTE 244M-2003](../analogue-video-specifications/docs/video_formats/SMPTE-244M-2003/SMPTE-244M-2003.md) §3.4 and §4.1.2.
- PAL `17.734475 MHz` interface sampling reference is grounded in [EBU Tech. 3280-E](../analogue-video-specifications/docs/video_formats/EBU-Tech-3280-E/EBU-Tech-3280-E.md) §1.1.1.
- Subcarrier nominal frequencies and frequency tolerances are grounded in [ITU-R BT.470-6](../analogue-video-specifications/docs/video_formats/BT-470-6-1998/BT-470-6-1998.md), Table 2 item 2.11(a), and [SMPTE 170M-2004](../analogue-video-specifications/docs/video_formats/SMPTE-170M-2004/SMPTE-170M-2004.md) §11.1.

For **4fsc sampling**, refer to:

- [SMPTE 244M-2003](../analogue-video-specifications/docs/video_formats/SMPTE-244M-2003/SMPTE-244M-2003.md): Bit-Parallel Digital Interface for NTSC.
- [EBU Tech. 3280-E](../analogue-video-specifications/docs/video_formats/EBU-Tech-3280-E/EBU-Tech-3280-E.md): 625-Line Digital PAL Interfaces.

---

### **4fsc Sampling Modes**


Current implementation status:

- Only **locked `4fsc`** operation is implemented.
- The current runtime does **not** implement a free-running mode.
- The current runtime does **not** expose a separate NCO-driven sample-clock subsystem; instead it synthesizes carrier phase directly from the absolute sample index on the `4fsc` lattice. Because the carrier advances exactly $\pi/2$ per sample, the index is reduced modulo 4 before the phase is formed, which makes the phase exact for any render length instead of losing precision as the absolute index grows.


| **Mode**                 | **Description**                                                                                                                  | **Use Case**                                                                         | **Reference**                     |
| ------------------------ | -------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------ | --------------------------------- |
| **Locked to Subcarrier** | Sample clock is **phase-locked** to the colour subcarrier (e.g., 4 × f_sc) using an **NCO (Numerically Controlled Oscillator)**. | Studio applications, testing, or when phase coherence is critical.                   | SMPTE 244M-2003, EBU Tech. 3280-E |
| **Free-Running**         | Sample clock runs **independently** of the subcarrier (e.g., 17.734475 MHz for PAL, but not locked).                             | General-purpose generation, emulation, or when subcarrier alignment is not required. | SMPTE 244M-2003, EBU Tech. 3280-E |


---

### **Subcarrier Lock Implementation**

For **testing applications**, the subcarrier locking implementation **must meet or exceed** the phase accuracy specified in the PAL/NTSC standards. The current implementation achieves lock implicitly by synthesizing Y/C directly on the `4fsc` sample lattice; a dedicated NCO-based clocking subsystem remains a target design for future extension to alternate sampling modes.

- **Current sampled-domain lock model**:
  - Use the normative `4fsc` sample rate for the selected standard.
  - Derive burst and active-picture carrier phase from the absolute sample index so line-to-line phase progression follows the standard-specific `4fsc` geometry. The index is reduced onto the 4-sample subcarrier lattice and the resulting phase bounded to $[0, 2\pi)$, so the arguments handed to the trigonometric functions never grow with render length.
  - Preserve whole-frame sample counts defined by the active standard (`709,379` PAL, `477,750` NTSC).

- **Future extension target**:
  - Introduce an explicit **NCO** or equivalent phase-accumulator abstraction when unlocked modes or alternate sample-rate outputs are implemented.
  - Demonstrate **≤ ±1° phase error** and suitable frequency accuracy relative to the ideal subcarrier-locked rate.

---

### **YAML Configuration**

```yaml
cvbs_presets:
  sample_encoding_preset: CVBS_U10_4FSC
  signal_state_preset: STANDARD_STABLE_LOCKED
```

---

### **Implementation Notes**

- **Current runtime**:
  - Generate complete frame batches directly at `4 × f_sc`.
  - Use absolute-sample-index carrier synthesis for PAL/NTSC burst and active-picture chroma.
  - Reject non-locked and non-`4fsc` output requests during validation.
- **Future runtime extensions**:
  - Add explicit unlocked and non-`4fsc` output modes behind a dedicated sample-clock abstraction.

---

---

## **11. VBI Line Allocation**

### **Specification Cross-Check (Section 11)**

- PAL laserdisc reserved ranges and address-signal constraints are grounded in [IEC 60856](../analogue-video-specifications/docs/laserdisc/IEC-60856-1986-Laservision-PAL/IEC-60856-1986-Laservision-PAL.md) §9.1.4 and Clause 10.
- NTSC laserdisc reserved ranges and 40-bit FM coexistence are grounded in [IEC 60857](../analogue-video-specifications/docs/laserdisc/IEC-60857-1986-Laservision-NTSC/IEC-60857-1986-Laservision-NTSC.md) §9.1.5, §10.1, §10.2.
- VIRS/ITS line assignments for NTSC laserdisc are grounded in [IEC 60857](../analogue-video-specifications/docs/laserdisc/IEC-60857-1986-Laservision-NTSC/IEC-60857-1986-Laservision-NTSC.md) §9.1.3-§9.1.4.
- PAL amendment-specific alternates (e.g., added CLV picture-number behavior and alternate VBI usage) are grounded in [IEC 60856 Amendment 2](../analogue-video-specifications/docs/laserdisc/IEC-60856-1986-Laservision-PAL-Amendment-2/IEC-60856-1986-Laservision-PAL-Amendment-2.md) §9.1.3/§9.1.4/§10.1.10.
- NTSC amendment-specific CLV/FM updates are grounded in [IEC 60857 Amendment 2](../analogue-video-specifications/docs/laserdisc/IEC-60857-1986-Laservision-NTSC-Amendment-2/IEC-60857-1986-Laservision-NTSC-Amendment-2.md) §10.1.10 and §10.2.3.
- VITC location constraints used for conflict checks are grounded in [IEC 60461:2010](../analogue-video-specifications/docs/video_metadata/IEC-60461-2010-Time-and-control-code/IEC-60461-2010-Time-and-control-code.md) §9.6.2 and §9.6.3.

VBI line allocations differ between PAL and NTSC and between Laserdisc and non-Laserdisc use. The laserdisc standards define exclusive reserved ranges that must not overlap with any other injection type.

Current implementation status:

- The runtime synthesizes sync, burst, blanking, and active-picture placement.
- VITS line injections are fully implemented for all supported PAL and NTSC types.
- Laserdisc biphase injection is fully implemented for PAL and NTSC (CAV and CLV), including 24-bit biphase and 40-bit FM coded signals, all code types, field-aware line placement, and all associated validation rules.
- VITC and custom per-line content runtime paths remain deferred; the VBI allocation rules for those injection types in this section are target design constraints for future implementation.

---

### **PAL VBI Line Allocation (IEC 60856 as amended, ITU-R BT.470-6)**

| **Signal**                         | **Field 1 Lines** | **Field 2 Lines** | **Notes**                                                                                                  |
| ---------------------------------- | ----------------- | ----------------- | ---------------------------------------------------------------------------------------------------------- |
| Laserdisc biphase — reserved range | 6–18              | 319–331           | Entire range reserved; no other injection allowed here when laserdisc is active. Lines 6–15 carry sync/eq pulses and hold no biphase content. |
| Laserdisc biphase — key code lines | 16–18             | 329–331           | Active biphase payload: picture numbers, chapter codes, status, etc. (within reserved range).             |
| VITS                               | 19, 20 (or 13 per Amend. 2) | 332, 333 (or 326 per Amend. 2) | Lines 22 and 335 shall be blanked when laserdisc is active (disk noise measurement).   |
| VITC                               | 19–21             | 332–334           | **Incompatible with laserdisc biphase.** Lines are outside the laserdisc reserved range but VITC must not be used in any section that also contains a `laserdisc` injection. |
| Subtitle / closed caption          | 20, 21            | 333, 334          | No VITS on lines 20/333 when subtitle is present; lines 14, 15/327, 328 for extra subtitle capacity (Amend. 2). |
| CEA-608                            | 22                | 335               | Line 335 shall be blanked when laserdisc is active.                                                       |

---

### **NTSC VBI Line Allocation (IEC 60857 as amended, SMPTE 170M-2004)**

| **Signal**                              | **Field 1 Lines** | **Field 2 Lines** | **Notes**                                                                                                                    |
| --------------------------------------- | ----------------- | ----------------- | ---------------------------------------------------------------------------------------------------------------------------- |
| Laserdisc biphase — reserved range      | 10–18             | 273–281           | Entire range reserved; no other injection allowed here when laserdisc is active.                                            |
| Laserdisc 40-bit FM coded signal (NTSC) | 10, 11            | 273, 274          | Within reserved range; 40-bit FM data on 10/273, white flag on 11/274.                                                      |
| Laserdisc biphase — key code lines      | 16–18             | 279–281           | Active 24-bit biphase payload (within reserved range).                                                                       |
| VITC                                    | 14–16             | 277–279           | **Incompatible with laserdisc biphase, and lines 14–16/277–279 fall within the NTSC laserdisc reserved range.** Must not appear in any section that contains a `laserdisc` injection. |
| VIR signal                              | 19                | 282               | Mandatory on NTSC laserdisc per IEC 60857 §9.1.3; FCC Recommendation 73-699.                                               |
| VITS (NTC-7 composite/combination)      | 17, 280           | —                 | **Conflicts with laserdisc reserved range.** NTC-7 types cannot coexist with a laserdisc injection in the same section.      |
| VITS (composite/combination test)       | 20                | 283               | Recommended by IEC 60857 §9.1.4; outside reserved range; compatible with laserdisc.                                        |
| CEA-608 / closed caption                | 21                | 284 (first half)  | Per PBS Report E7709 (handicapped caption data).                                                                             |

---

### **Conflict Resolution**

- **If injected lines overlap within a section, the YAML must fail validation.**
- **When a `laserdisc` injection is active in a section, any other injection targeting lines within the reserved range for that standard must fail validation** (PAL: F1 6–18, F2 319–331; NTSC: F1 10–18, F2 273–281).
- **A `vitc` injection and a `laserdisc` injection must not appear in the same section.** Laserdisc does not use VITC; the biphase system is the sole timecode and address mechanism on laserdisc.
---

---

## **12. Implementation Pipeline**

### **Specification Cross-Check (Section 12)**

- Validation-stage checks listed here are a direct implementation of constraints traced in Sections 7, 8, 11, and 13 (and therefore map to BT.470/1700, SMPTE 170M/244M, IEC 60856/60857, IEC 60461).
- Generation-stage sync, burst, and modulation behavior is grounded in [ITU-R BT.1700](../analogue-video-specifications/docs/video_formats/BT-1700-E/BT-1700-E.md) and [SMPTE 170M-2004](../analogue-video-specifications/docs/video_formats/SMPTE-170M-2004/SMPTE-170M-2004.md).
- Output-stage quantization and reserved/excluded code handling is grounded in [EBU Tech. 3280-E](../analogue-video-specifications/docs/video_formats/EBU-Tech-3280-E/EBU-Tech-3280-E.md) and [SMPTE 244M-2003](../analogue-video-specifications/docs/video_formats/SMPTE-244M-2003/SMPTE-244M-2003.md) §4.2.
- Rise/fall-time targets (`140 ns ± 20 ns`, `200 ns ± 50 ns`) are grounded in [SMPTE 170M-2004](../analogue-video-specifications/docs/video_formats/SMPTE-170M-2004/SMPTE-170M-2004.md) §13 notes (default pulse transitions) and [ITU-R BT.1700](../analogue-video-specifications/docs/video_formats/BT-1700-E/BT-1700-E.md), Table 2/Table 3.

---

The runtime uses a **central pipeline module** to process sections and combine them into the final output. The pipeline controls schedule construction, batched generation, streamed output writes, and progress logging, and is divided into the **Generation Stage** and **Output Stage**.

### **Pipeline Overview**

```
[YAML Project File] → [Validation] → [Pipeline Controller (schedule + progress)] → [Generation Stage (batched frames)] → [Noise Injection Stage (per-section)] → [Dropout Injection Stage (per-section)] → [Output Stage (append + finalize)] → [CVBS File (Video + Metadata) + Dropout Sidecar (.dropouts.meta)]
```

### **Pipeline Entry Points and Concurrency**

- `VideoSynthPipeline::Run(options, observer, cancellation)` parses the YAML project file and delegates to `RunProject`. `VideoSynthPipeline::RunProject(project, options, observer, cancellation)` runs the pipeline for an already-parsed in-memory `Project` (validate → generate → noise → dropout → output); relative paths in the project are used as-is and must be resolved by the caller. The CLI uses `Run`; front-ends that hold an in-memory project (e.g. a GUI) use `RunProject`.
- `observer` (optional, nullable) is an `IPipelineObserver` receiving stage transitions (`validate`, `generate`, `finalize`), per-batch frame progress (`frames_completed / frames_total`), validation warnings, and a terminal `PipelineRunStatus` (`kSucceeded` / `kCancelled` / `kFailed`) reported exactly once. Callbacks run synchronously on the pipeline's executing thread.
- `cancellation` (optional, nullable) is a thread-safe one-shot `CancellationToken` polled between frame batches. On cancellation the pipeline aborts all in-progress artefacts — video/chroma/metadata via `IOutputStage::AbortWrite`, the WAV tracks via `AudioTrackGenerator::Abort`, and the dropout sidecar via `DropoutInjectionStage::Abort` — so a cancelled run leaves no partially-written output files, reports `kCancelled` (not an error), and returns `false`.
- A whole pipeline run executes on a single thread, which may be a worker thread; there is no main-thread affinity. `ILogger`, `IProjectParser`, and `IProjectValidator` implementations are thread-safe; the stage collaborators are single-owner and NOT thread-safe, so a pipeline instance must not run concurrently with itself.

---

### **Detailed Pipeline Steps**

#### **1. Validation Stage**

- Parse the YAML file and validate all fields.
- In the current runtime, validate the implemented subset of fields and constraints only.
- Ensure the selected `signal_state_preset` denotes locked operation when using a 4fsc `sample_encoding_preset`.
- For progressive assets, enforce source-profile and BT.601 source-content compliance requirements defined in [Section 8.1](#81-frame-based-sections) and the referenced asset-spec documents.
- **Fail validation if any errors are found.**

#### **2. Generation Stage**

- **Input**: Validated YAML project file.
- **Output**: `4fsc`-discrete fixed-point representations of **luma (Y)** and **chroma (C)** signals.
- **Steps**:
  1. For each section:
    - Generate the **frame-based content** (`progressive`) from source data that already satisfies BT.601 source-content constraints.
    - Preserve source raster geometry during ingestion (no scaling, no resampling, and no crop of samples or lines).
    - For each frame in the section:
      - Insert **sync pulses** and **colour burst** (see [ITU-R BT.470-6](../analogue-video-specifications/docs/video_formats/BT-470-6-1998/BT-470-6-1998.md), [ITU-R BT.1700](../analogue-video-specifications/docs/video_formats/BT-1700-E/BT-1700-E.md)).
      - Apply **ramping and transition smoothing** to simulate analogue behavior (see [ITU-R BT.470-6](../analogue-video-specifications/docs/video_formats/BT-470-6-1998/BT-470-6-1998.md) for ramping requirements).
    - Emit bounded batches of `4fsc` Y and C sample buffers in fixed-point mV units, ready for handoff to the Noise Injection Stage.

Current implementation note:

- VITS line injections are applied in the generation-stage runtime path on validated target lines.
- Laserdisc biphase injection is applied in the generation-stage runtime path via BiphaseInjectionManager (24-bit biphase and 40-bit FM for NTSC).
- Runtime synthesis/application for VITC and custom per-line content remains deferred.

#### **3. Noise Injection Stage**

- **Input**: `4fsc`-discrete fixed-point Y/C mV batches from the generation stage; project noise parameters per section.
- **Output**: In-place modified Y/C mV buffers with additive Gaussian noise applied to sections that have `noise.enabled == true`.
- **Steps**:
  1. For each frame in the batch, look up the owning section from the frame schedule.
  2. Skip frames whose section has no noise configured.
  3. For each enabled frame:
     - Derive floor sigma (`σ_f_mV`) and proportional coefficient (`k`) from `noise_db` and `noise_spread_db`.
     - Seed `std::mt19937_64` from a hash of `(section_index, global_frame_index)` for reproducible, batch-size-independent output.
     - For each sample position: draw a single noise value and add it to both Y and C buffers (correlated noise). The noise standard deviation at each sample is `sqrt(σ_f² + (k × Y_mV)²)`.
  4. Clamp all output samples to the legal fixed-point mV range for the standard.

#### **4. Dropout Injection Stage**

- **Input**: In-place fixed-point Y/C mV batches (after noise); project dropout parameters per section.
- **Output**: In-place modified Y/C mV buffers with random and/or scratch dropouts applied; `<basename>.dropouts.meta` SQLite sidecar populated.
- **Steps**:
  1. On first call (`Begin`), open the sidecar SQLite file at the path derived from the (video-colocated) metadata path (`.meta` → `.dropouts.meta`). Create the `dropout_run` table (schema version 5) and begin a single transaction.
  2. For each frame in the batch:
     - Determine owning section; skip if no dropout type is active.
     - **Scratch pass**: for each pre-computed scratch event, apply the triangular amplitude/width envelope at the current frame index; record covered intervals.
     - **Random pass**: draw `N ~ Poisson(frequency)` random events; clip each event against scratch-covered intervals; merge adjacent residuals; apply signal push.
     - For each surviving run: split at active-picture boundaries; apply `lerp(original_mV, target_mV, push_fraction)` to both Y and C; write `dropout_run` row with `severity = 75` (visible) or `25` (non-visible).
  3. Clamp all modified samples to the legal fixed-point mV range.
  4. On `Finalize`, commit the transaction and close the database handle.
  5. If no section has dropout injection enabled, the sidecar is not created.

#### **5. Output Stage**

- **Input**: `4fsc`-discrete Y and C signal batches from the generation stage.
- **Output**: CVBS file (video + metadata).
- **Steps**:
  1. **Generate Metadata**:
    - Create the CVBS file header with all required metadata (see [CVBS File Format Specification](../cvbs-file-format-specification/docs/index.md)).
  2. **Composite Formation and Quantisation** (composite mode) / **Y/C Quantisation** (Y/C mode):
    - **Composite** (`signal_type: composite`): Combine the fixed-point luma and chroma sample buffers into a composite sample stream; encode to the active output preset representation.
    - **Y/C** (`signal_type: yc`): Encode luma samples identically to composite luma; encode chroma samples centred at code 512 per the CVBS File Format Specification (§ Sample Encoding Presets); write to separate `.cvbsy` and `.cvbsc` files.
    - In both modes, map fixed-point mV samples to 10-bit integers using the normative linear mapping for the active standard (EBU Tech. 3280-E for PAL, SMPTE 244M-2003 for NTSC). See [§6.1](#61-signal-levels) for the formulae.
    - Clamp to the legal code range; excluded values (codes 0–3 and 1020–1023) must not appear in output.
  3. **mV to Integer Conversion**:
    - Encode the quantised composite code stream into the active output preset representation.
  4. **Output Formatting**:
    - Append each generated batch directly to the **video file** as it completes.
    - Finalize by writing metadata to the **metadata file** once all expected frames have been written.

Current implementation note:

- The output stage currently accepts only locked `4fsc` batches and does not perform a separate resampling or NCO-driven sampling step.

---

### **Ramping and Transition Smoothing**

To simulate **analogue output**, the generator must:

- Avoid **sharp transitions** in the signal.
- Apply **ramping** to sync pulses, colour burst, and active video transitions as specified in [ITU-R BT.470-6](../analogue-video-specifications/docs/video_formats/BT-470-6-1998/BT-470-6-1998.md) and [ITU-R BT.1700](../analogue-video-specifications/docs/video_formats/BT-1700-E/BT-1700-E.md).
- **Rise/Fall Times**:
  - **Luma**: 140 ns ± 20 ns (10% to 90% of white level) (see [SMPTE 170M-2004](../analogue-video-specifications/docs/video_formats/SMPTE-170M-2004/SMPTE-170M-2004.md)).
  - **Chroma**: 200 ns ± 50 ns (10% to 90% of chroma amplitude) (see [SMPTE 170M-2004](../analogue-video-specifications/docs/video_formats/SMPTE-170M-2004/SMPTE-170M-2004.md)).

---

---

## **13. Error Handling and Validation**

### **Specification Cross-Check (Section 13)**

- `video_standard_preset`/line-range consistency checks map to [ITU-R BT.470-6](../analogue-video-specifications/docs/video_formats/BT-470-6-1998/BT-470-6-1998.md), Annex 1, Table 1 and [SMPTE 170M-2004](../analogue-video-specifications/docs/video_formats/SMPTE-170M-2004/SMPTE-170M-2004.md) §1.1.
- `signal_state_preset` and 4fsc sample-encoding checks map to [SMPTE 244M-2003](../analogue-video-specifications/docs/video_formats/SMPTE-244M-2003/SMPTE-244M-2003.md) §3.1 and [EBU Tech. 3280-E](../analogue-video-specifications/docs/video_formats/EBU-Tech-3280-E/EBU-Tech-3280-E.md) §1.1.1.
- Laserdisc reserved-range conflicts and laserdisc-only placement logic map to [IEC 60856](../analogue-video-specifications/docs/laserdisc/IEC-60856-1986-Laservision-PAL/IEC-60856-1986-Laservision-PAL.md) §9.1.4/§10 and [IEC 60857](../analogue-video-specifications/docs/laserdisc/IEC-60857-1986-Laservision-NTSC/IEC-60857-1986-Laservision-NTSC.md) §9.1.5/§10.
- VITC constraints (including incompatibility decisions when laserdisc is active in the same section) map to [IEC 60461:2010](../analogue-video-specifications/docs/video_metadata/IEC-60461-2010-Time-and-control-code/IEC-60461-2010-Time-and-control-code.md) Clause 9 and laserdisc Clause 10 families.
- Frame-rate checks map to [ITU-R BT.470-6](../analogue-video-specifications/docs/video_formats/BT-470-6-1998/BT-470-6-1998.md), Annex 1 Table 1 and [SMPTE 170M-2004](../analogue-video-specifications/docs/video_formats/SMPTE-170M-2004/SMPTE-170M-2004.md) §11.3.

---

### **Current Implementation Status**

The current validator enforces a narrower subset than the full design intent in this section:

- Implemented: standard selection, locked `4fsc` preset constraints, output-path requirements (including `signal_type` validation and `.cvbsy`-suffix enforcement for Y/C mode), progressive source profile checks, accepted raster checks, NTSC black-setup constraints, and validator-side VITS/line-injection compatibility checks including overlap detection, laserdisc reserved-range conflicts, and VITC/laserdisc incompatibility.
- Implemented: full laserdisc biphase validation including section_type/code_type matrix enforcement, IEC value range constraints (picture_number, chapter_number, users_code X₁, CLV picture number digits, programme time code BCD), CAV minimum duration checks (lead-in ≥ 938 frames, lead-out ≥ 1250 frames), minimum chapter length (30 tracks), the NTSC VIRS mandatory presence check (a `virs` entry in the project-level `line_injections.vits`), and VITS/biphase reserved-range line conflict detection.
- Implemented: per-section noise parameter validation (range, spread floor, mutual dependency).
- Implemented: disc-structure section ordering. When any section declares a `section_type`, the section sequence must be `[lead_in] programme_area... [lead_out]`: at most one `lead_in` (first) and one `lead_out` (last), no section may precede the `lead_in` or follow the `lead_out`, and every section after a `lead_in` or before a `lead_out` must be `programme_area`. Out-of-order sections would break monotonic picture-number and time-code generation (IEC 60856/60857). The GUI surfaces these errors through the shared validator in the validation issues dock.
- Not yet implemented in the validator/runtime pair: VITC and custom per-line content runtime paths.

The rule set below remains the intended validation contract for VITC and custom per-line content, which are not yet implemented.

---

### **Validation Rules**

1. **CVBS Presets**:
  - Only one `video_standard_preset` (PAL or NTSC) per project.
  - Only one `sample_encoding_preset` per project.
  - Only one `signal_state_preset` per project.
  - Output resolution is fixed by the standard (720x576 for PAL and SECAM, 720x486 for NTSC and PAL-M) and must not be specified in the project file.
  - 4fsc generation requires a 4fsc `sample_encoding_preset` and a locked `signal_state_preset` (unlocked for SECAM).
  - `pal_laserdisc_pilot_burst` can **only be enabled for PAL projects**. If enabled for NTSC, the YAML is considered **invalid**.
  - `ntsc_laserdisc_vbi_burst` can **only be enabled for NTSC projects**. If enabled for PAL, the YAML is considered **invalid**.
  - `ntsc_black_setup_ire` can **only be specified for NTSC projects**. If specified for PAL, the YAML is considered **invalid**.
  - `ntsc_black_setup_ire` must be either `7.5` or `0.0`.
2. **Sections**:
  - Each section must have a valid `type` (`progressive`).
  - Each section must have at least one of:
    - Frame-based content (`progressive`).
    - Line injections (`line_injections`).
  - For `progressive` sections, `source` must point to a valid file.
  - **Input source frame rate must match the required output format and be constant (including MKV and MP4 sources)**.
  - **Progressive source must match a supported source profile (Section 8.1): EXR still rules, or for video a container matching its extension, a video stream and no crop metadata.**
  - **Video field order opposite to the output dominance, or a non-BT.601 sample aspect ratio, is a warning.**
  - **Progressive source dimensions must be PAL: `720x576`; NTSC: `720x486`**.
  - **Scaling/resizing of progressive sources is not supported**.
  - **Ingestion must preserve the full source raster geometry (`720x576` or `720x486`) with no horizontal crop, no vertical crop, and no sample-rate conversion.**
  - **Ingestion must not apply implicit width normalization or ad-hoc crop/remap operations**.
  - **Disc-structure section ordering** (applies when any section declares a `section_type`):
    - At most one `lead_in` section; if present, it must be the **first** section (no section may precede it).
    - At most one `lead_out` section; if present, it must be the **last** section (no section may follow it).
    - Every section after a `lead_in` or before a `lead_out` must be `programme_area`.
    - Violations are **errors**: out-of-order sections would break monotonic picture-number and time-code generation (IEC 60856/60857).
3. **Line Injection Constraints**:
  - `target_lines` must be within the valid range for the standard (1-625 for PAL, 1-525 for NTSC).
  - `target_lines` must **not** be specified for `laserdisc` injection types; specifying it is a validation error.
  - **If injected lines overlap within a section, the YAML must fail validation.**
  - **When a `laserdisc` injection is active in a section, no other injection type may target lines within the laserdisc reserved ranges** (PAL: Field 1 lines 6–18, Field 2 lines 319–331; NTSC: Field 1 lines 10–18, Field 2 lines 273–281).
  - **A `vitc` injection and a `laserdisc` injection must not appear in the same section.** Laserdisc does not use VITC.
4. **Noise Parameters** (per-section `noise:` block):
  - `noise_db` must be in **[20.0, 61.0] dB** (error if outside this range).
  - `noise_spread_db` must be **≥ 0.0** (error if negative).
  - `noise_db − noise_spread_db` must be **≥ 20.0** (White SNR floor limit; error if violated).
  - `noise_spread_db` present without `noise_db` is a validation error.
  - If `noise_spread_db > 0` and no VITS injection targets the orc-gui White SNR measurement line (PAL: frame line 19; NTSC: frame line 20), a **warning** is emitted that the White SNR target will not be verifiable in orc-gui without a suitable VITS white-flag injection.
5. **Dropout Parameters** (per-section `dropouts:` block):
  - `random.scale` must be in **[0, 20]** (error if outside this range; `0` means disabled).
  - `scratch.scale` must be in **[0, 20]** (error if outside this range; `0` means disabled).
  - A `dropouts:` block where both `random.scale` and `scratch.scale` are `0` (or both sub-blocks are absent) is a **validation error** — omit the `dropouts:` key entirely to disable.
  - If `scratch.scale > 0` and the derived maximum scratch lifespan (from `DeriveScratchDropoutParams(scale).max_dur_frames`) exceeds `section.duration_frames`, a **warning** is emitted indicating the scratch event will not complete its full triangle envelope within the section.
6. **EFM Digital Audio** (`output.efm_audio:` block):
  - `pair` must be in **[0, 7]** (error if outside this range).
  - `video_standard_preset` must be `PAL` or `NTSC` — no other standard has a LaserDisc digital audio specification (error otherwise).
  - If no section declares the selected `pair`, a **warning** is emitted: no `.efm` file is written.
  - More than **79** `programme_area` sections is an error (IEC 60856 Amd 2, 13.5.3.3 / IEC 60857 Amd 2, 13.6.3.3 — one track per section).
  - A `programme_area` section shorter than **4 s** (**6 s** for the first, whose leading 2 s are the mandatory pause) emits a **warning** (IEC 60908-1999, 17.5.1).
  - A project with no `lead_in` section emits a **warning**: no table of contents is emitted.

---

### **Error Messages**


| **Error**                                  | **Message**                                                                                                                              |
| ------------------------------------------ | ---------------------------------------------------------------------------------------------------------------------------------------- |
| Invalid standard                           | "video_standard_preset must be 'PAL' or 'NTSC'."                                                                                         |
| `resolution` field present                 | "`resolution` must not be specified; it is fixed by the standard."                                                                       |
| Invalid sample rate                        | "Sample rate 10MSPS may not capture the full CVBS bandwidth (5.5 MHz for PAL)."                                                          |
| Missing source file                        | "File '/nonexistent.mkv' not found."                                                                                                     |
| Invalid line injection target lines        | "Line 700 is out of range for PAL (max 625)."                                                                                            |
| Laserdisc `target_lines` specified         | "`target_lines` must not be specified for laserdisc injections; line placement is fixed by the standard."                                |
| Laserdisc reserved range conflict          | "Injection type [type] targets line [n] which is within the laserdisc reserved range for [standard]; cannot coexist with laserdisc injection." |
| VITC with laserdisc                        | "`vitc` injection cannot be used in the same section as a `laserdisc` injection; laserdisc does not use VITC."                              |
| Overlapping target lines                   | "Overlapping target_lines in section: [section_name]."                                                                                   |
| Invalid signal state preset                | "signal_state_preset must indicate a time-base stable, phase-locked state."                                                              |
| Invalid pilot burst                        | "pal_laserdisc_pilot_burst can only be enabled for PAL projects."                                                                        |
| Invalid VBI burst                          | "ntsc_laserdisc_vbi_burst can only be enabled for NTSC projects."                                                                        |
| Invalid NTSC black setup scope             | "ntsc_black_setup_ire can only be specified for NTSC projects."                                                                          |
| Invalid NTSC black setup value             | "ntsc_black_setup_ire must be 7.5 or 0.0."                                                                                               |
| Invalid frame rate                         | "Input frame rate must match the output standard's frame rate (25 fps for PAL, ~29.97 fps for NTSC)."                                    |
| Unsupported progressive source profile     | "Progressive source is not in a supported profile. Validate container, codec, chroma format, and bit depth against the supported profile list." |
| Invalid progressive source dimensions      | "Progressive source dimensions are invalid for the selected standard. PAL requires 720x576; NTSC requires 720x486." |
| Scaling requested or implied               | "Progressive source scaling is not supported. Source dimensions must already match PAL 720x576 or NTSC 720x486." |


---

---

## **14. CLI Interface**

### **Specification Cross-Check (Section 14)**

- This section defines application interface behavior (non-normative to analogue waveform standards).
- `--validate` behavior is intended to enforce constraints traced to Sections 7, 11, and 13 (which are normatively tied to BT.470/1700, SMPTE 170M/244M, IEC 60856/60857, IEC 60461).

---

### **Usage**

```bash
videosynth --project project.yaml [options]
```

---

### **Options**


| **Option**   | **Description**                                   | **Default** |
| ------------ | ------------------------------------------------- | ----------- |
| `--project`  | Path to the YAML project file (required).         | -           |
| `--validate` | Validate the YAML file without generating output. | `false`     |
| `--version`  | Print the build version (short git commit hash, `-dirty` suffix for modified trees, or the release override string) and exit. | -           |
| `--threads <n>` | Frame synthesis worker threads: `auto` (one per hardware thread) or a positive integer. `1` selects the pure sequential path. Output is byte-identical regardless of the thread count. | `auto` |
| `--template-cache-mb <n>` | Frame template cache capacity in MiB; `0` disables the cache (see [Section 4](#4-generation-stage), Frame Template Cache). Output is byte-identical regardless of the capacity. | `512` |
| `--log-level <level>` | Set the log level to `info`, `debug`, or `trace`. | `info` |
| `--log-file <filename>` | Write log output to the specified file in addition to the console. | none |

The output path is configured in YAML under `output.video_path`; the metadata sidecar and audio track are colocated with it.

Multi-threaded generation splits frame processing into a sequential
schedule-enrichment pass (per-frame VBI code words, colour-sequence indices,
OSD token strings) followed by order-independent per-frame sample synthesis on
a worker pool; frames are reassembled in order (bounded to 2 × thread count
in-flight frames) before output, dropout-sidecar, and audio emission, so all
emitted artefacts match the sequential path byte for byte.

Workers carry as much of the per-frame work as ordering allows. Besides
synthesis, noise and dropout computation, each worker also **encodes** its
frame into output sample codes (`IOutputStage::EncodeFrame`, a const function
of the session state resolved by `BeginWrite`), leaving the single consumer
thread with one `write` per file per frame plus the sidecar and audio
bookkeeping that must happen in order. Frame buffers are recycled through a
free list sized to the reassembly window, so steady-state generation allocates
no whole-frame buffers and peak resident memory stays bounded by that window
rather than by allocator churn.

---

### **Example**

```bash
# Generate CVBS output from a project file
videosynth --project pal_test.yaml

# Validate a project file without generating output
videosynth --project ntsc_test.yaml --validate

# Generate on the pure sequential path (single-threaded)
videosynth --project pal_test.yaml --threads 1

# Generate with debug logging and a log file
videosynth --project pal_test.yaml --log-level debug --log-file out/videosynth.log
```

---

### **GUI logging options**

`videosynth-gui` accepts the same `--log-level <level>` and `--log-file
<filename>` options (both `--option value` and `--option=value` forms). They
configure the application logger — which also receives Qt's own diagnostics
via the message bridge — and, for the lifetime of the session, override the
persisted generation preferences for every pipeline run started from the GUI.
When the options are absent, the application logger stays at `info` on stderr
and generation runs follow the Preferences dialog settings. An invalid level
or missing value exits with code 2, matching the CLI.

```bash
# Run the GUI with trace logging captured to a file
videosynth-gui --log-level trace --log-file out/videosynth-gui.log
```

---

---

## **15. Build and Packaging**

### **Specification Cross-Check (Section 15)**

- This section is implementation/tooling guidance and does not introduce analogue waveform constants.
- No additional normative analogue-spec clauses are required here beyond those already referenced in design requirements sections.

---

### **Build Environment**

- **Language**: C++17.
- **Build System**: Nix.
- **Dependencies**:
  - **YAML Parsing**: [yaml-cpp](https://github.com/jbeder/yaml-cpp).
  - **Image/Video Decoding**: [FFmpeg](https://ffmpeg.org/) (for the constrained progressive source profiles in Section 8.1).
  - **Sample Rate Conversion**: [soxr](https://sourceforge.net/p/soxr/).
  - **Logging**: [spdlog](https://github.com/gabime/spdlog).
  - **Testing**: [Google Test](https://github.com/google/googletest).

---

### **Nix Configuration**

#### `**flake.nix` Example**

```nix
{
  description = "VideoSynth - Analogue Video Format Generator";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs { inherit system; };
      in {
        packages.default = pkgs.stdenv.mkDerivation {
          name = "videosynth";
          src = ./.;

          buildInputs = with pkgs; [
            cmake
            yaml-cpp
            ffmpeg
            soxr
            spdlog
            googletest
          ];

          cmakeFlags = [ "-DCMAKE_BUILD_TYPE=Release" ];

          installPhase = ''
            mkdir -p $out/bin
            cp videosynth $out/bin/
          '';
        };
      }
    );
}
```

---

#### `**default.nix` Example**

```nix
{ pkgs ? import <nixpkgs> {} }:

pkgs.stdenv.mkDerivation {
  name = "videosynth";
  src = ./.;

  buildInputs = with pkgs; [
    cmake
    yaml-cpp
    ffmpeg
    soxr
    spdlog
    googletest
  ];

  cmakeFlags = [ "-DCMAKE_BUILD_TYPE=Release" ];

  installPhase = ''
    mkdir -p $out/bin
    cp videosynth $out/bin/
  '';
}
```

---

### **CMake Configuration**

#### `**CMakeLists.txt` Example**

```cmake
cmake_minimum_required(VERSION 3.15)
project(VideoSynth LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find dependencies
find_package(yaml-cpp REQUIRED)
find_package(FFmpeg REQUIRED)
find_package(soxr REQUIRED)
find_package(spdlog REQUIRED)

# Add executable
add_executable(videosynth
    src/main.cpp
    src/yaml_parser.cpp
    src/yaml_parser.h
    src/generation_stage.cpp
    src/generation_stage.h
    src/output_stage.cpp
    src/output_stage.h
    src/field_handler.cpp
    src/field_handler.h
    src/line_injection.cpp
    src/line_injection.h
    src/vits_generator.cpp
    src/vits_generator.h
    src/laserdisc_generator.cpp
    src/laserdisc_generator.h
    src/vitc_generator.cpp
    src/vitc_generator.h
    src/nco.cpp
    src/nco.h
    src/ramping.cpp
    src/ramping.h
)

# Link libraries
target_link_libraries(videosynth
    yaml-cpp
    avcodec
    avformat
    avutil
    soxr
    spdlog::spdlog
    GTest::GTest
)

# Install
install(TARGETS videosynth DESTINATION bin)
```

---

### **Desktop Integration**

The GUI is a desktop application and installs the Freedesktop metadata that makes it appear in the GNOME (and other XDG) application menus. The reverse-DNS application ID `io.github.decode_orc.VideoSynth` is the single identity used by all of them, and is also passed to `QApplication::setDesktopFileName()` so a desktop shell can associate a running window with its menu entry.

`cmake --install` places, when `VIDEOSYNTH_BUILD_GUI` is on:

| Installed path | Source |
| --- | --- |
| `share/applications/io.github.decode_orc.VideoSynth.desktop` | `packaging/linux/` |
| `share/metainfo/io.github.decode_orc.VideoSynth.metainfo.xml` | `packaging/linux/*.metainfo.xml.in`, configured by CMake |
| `share/icons/hicolor/<size>/apps/io.github.decode_orc.VideoSynth.png` | `assets/videosynth-icon-<size>.png` |
| `share/icons/hicolor/scalable/apps/io.github.decode_orc.VideoSynth.svg` | `assets/videosynth-logo.svg` |

The AppStream release entry is filled from the `VIDEOSYNTH_RELEASE_VERSION` and `VIDEOSYNTH_RELEASE_DATE` cache variables, which default to the build version and the current UTC date; packaged builds pass the release tag and its date.

### **Packaging Build Options**

Three CMake options separate the shipped artefacts from the development gates, so a packaging build never fails on tooling that only the dev shell pins:

| Option | Default | Packaging value |
| --- | --- | --- |
| `VIDEOSYNTH_BUILD_TESTS` | `ON` | `OFF` — skips the test suites and the Google Test dependency |
| `VIDEOSYNTH_ENABLE_CLANG_FORMAT` | `ON` | `OFF` — the formatter version differs between environments |
| `VIDEOSYNTH_ENABLE_CLANG_TIDY` | `ON` | `OFF` — see the note in Section 15 above |

### **Flatpak**

`packaging/flatpak/io.github.decode_orc.VideoSynth.yml` builds the Linux desktop package on the `org.kde.Platform` runtime (Qt 6). The GUI is the manifest's `command`; the CLI ships in the same bundle and is reached with `flatpak run --command=videosynth`.

- **Filesystem access**: `--filesystem=host`, plus `/media` and `/run/media`. Generation reads source media and writes multi-gigabyte sample files at user-chosen paths, so the sandbox is given unrestricted read/write access to the user's own filesystem rather than a portal-mediated subset.
- **Bundled modules**: yaml-cpp, spdlog, Imath and OpenEXR are built into `/app` (the runtime carries neither), as is FFmpeg — Section 8.1's progressive video sources shell out to the `ffmpeg` and `ffprobe` programs, which no runtime provides. sqlite3 and zlib come from the runtime.
- **Asset root**: `VIDEOSYNTH_BUNDLED_ASSET_DIR` is set to `/app/share/videosynth/assets`, so `{bundled}` (Section 9.1) resolves inside the sandbox.
- **Version metadata**: the build sandbox has no git metadata, so `scripts/prepare-flatpak-manifest.py` substitutes the release version and date into a generated copy of the manifest before the build.

`.github/workflows/flatpak.yml` builds the bundle on every push and pull request and publishes it as a release asset only for `v*` tags. See `packaging/README.md` for the full packaging reference.

---

---

## **16. Directory Structure**

### **Specification Cross-Check (Section 16)**

- This section is repository-layout guidance (non-normative to analogue waveform standards).
- The listed spec submodule locations correspond to the normative sources cited in Sections 2-13.

```
videosynth/
├── CMakeLists.txt
├── flake.nix
├── default.nix
├── src/
│   ├── main.cpp
│   ├── yaml_parser.cpp
│   ├── yaml_parser.h
│   ├── generation_stage.cpp
│   ├── generation_stage.h
│   ├── output_stage.cpp
│   ├── output_stage.h
│   ├── field_handler.cpp
│   ├── field_handler.h
│   ├── line_injection.cpp
│   ├── line_injection.h
│   ├── vits_generator.cpp
│   ├── vits_generator.h
│   ├── laserdisc_generator.cpp
│   ├── laserdisc_generator.h
│   ├── vitc_generator.cpp
│   ├── vitc_generator.h
│   ├── nco.cpp
│   ├── nco.h
│   ├── ramping.cpp
│   ├── ramping.h
│   ├── progressive_source.cpp
│   ├── progressive_source.h
│   ├── efm/                             # videosynth_efm module (no core dependencies)
│   │   ├── audio_frame_assembler.cpp
│   │   ├── circ_encoder.cpp
│   │   ├── subcode_generator.cpp
│   │   ├── efm_modulator.cpp
│   │   └── efm_stream_encoder.cpp
│   └── ...
├── docs/                                # MkDocs sources for the published site
│   ├── .nav.yml
│   ├── index.md
│   ├── getting-started/
│   ├── user-manual/
│   ├── reference/
│   └── misc/
├── docs-tech/
│   ├── analogue-video-specifications/  (submodule)
│   ├── cvbs-file-format-specification/ (submodule)
│   ├── design/
│   │   ├── high-level-design.md
│   │   └── ...
│   └── user/
│       ├── biphase-design.md
│       └── ...
├── resources/
│   └── doc-diagrams/
├── videosynth-assets/
│   └── assets/                          # Authoritative progressive source corpus
│       ├── exr/
│       │   ├── 720x576/
│       │   └── 720x486/
│       └── mkv/
│           ├── 720x576/
│           └── 720x486/
├── projects/                            # Hand-authored project fixtures (inputs only)
│   ├── general/                         # Feature fixtures — composite
│   ├── general-yc/                      # Feature fixtures — Y/C
│   ├── stacking/                        # Disc-simulation / stacking fixtures
│   └── variants.json                    # Rules for the build-time derived variants
├── scripts/
│   ├── generate_test_projects.py        # Derives the Y/C and clean variants
│   ├── run-projects.sh                  # Runs a fixture suite through the CLI
│   └── ...
├── tests/                               # Automated tests only — no YAML, no output
│   ├── CMakeLists.txt                   # Owns every test target
│   ├── support/                         # Shared helpers (fixture_paths.h, …)
│   ├── unit/                            # Mocked and fast → ctest label "unit"
│   ├── functional/                      # Filesystem/pipeline → label "functional"
│   └── gui/
│       ├── support/                     # Shared QCoreApplication entry point
│       ├── unit/
│       └── functional/
└── README.md
```

---

---

## **17. Future Requirements**

### **Specification Cross-Check (Section 17)**

- CEA-608 future feature reference is grounded in [ANSI/CTA-608-E S-2019](../analogue-video-specifications/docs/video_metadata/ANSI-CTA-608-E-S-2019/ANSI-CTA-608-E-S-2019.md).
- No additional current PAL/NTSC waveform constants are introduced in this section.

The following features are **reserved for future implementation**:

1. **CEA-608 Closed Captions**:
  - Support for embedding CEA-608 closed captions in the VBI (Line 21 for NTSC, Line 22 for PAL).
  - References:
    - [ANSI/CTA-608-E S-2019](../analogue-video-specifications/docs/video_metadata/ANSI-CTA-608-E-S-2019/ANSI-CTA-608-E-S-2019.md): Line 21 Data Services.
2. **Multi-Threading**:
  - Parallel processing of sections or frames for improved performance on multi-core systems.

---

---

## **18. Appendix: References**

### **Specification Cross-Check (Section 18)**

- To support clause-level traceability used above, include amendment documents explicitly with base laserdisc standards.
- Include [IEC 60856 Amendment 2](../analogue-video-specifications/docs/laserdisc/IEC-60856-1986-Laservision-PAL-Amendment-2/IEC-60856-1986-Laservision-PAL-Amendment-2.md) and [IEC 60857 Amendment 2](../analogue-video-specifications/docs/laserdisc/IEC-60857-1986-Laservision-NTSC-Amendment-2/IEC-60857-1986-Laservision-NTSC-Amendment-2.md) when citing CLV picture-number and line-allocation updates.

All specifications referenced in this document are available in the following repositories:

- **[CVBS File Format Specification](../cvbs-file-format-specification/docs/index.md)**
- **[Analogue Video Specifications](../analogue-video-specifications/docs/index.md)**
  - [ITU-R BT.470-6](../analogue-video-specifications/docs/video_formats/BT-470-6-1998/BT-470-6-1998.md): Conventional Television Systems.
  - [ITU-R BT.1700](../analogue-video-specifications/docs/video_formats/BT-1700-E/BT-1700-E.md): Composite Video Signal Characteristics.
  - [SMPTE 170M-2004](../analogue-video-specifications/docs/video_formats/SMPTE-170M-2004/SMPTE-170M-2004.md): Composite Analog Video Signal for NTSC.
  - [SMPTE 244M-2003](../analogue-video-specifications/docs/video_formats/SMPTE-244M-2003/SMPTE-244M-2003.md): Bit-Parallel Digital Interface for NTSC.
  - [EBU Tech. 3280-E](../analogue-video-specifications/docs/video_formats/EBU-Tech-3280-E/EBU-Tech-3280-E.md): 625-Line Digital PAL Interfaces.
  - [IEC 60856](../analogue-video-specifications/docs/laserdisc/IEC-60856-1986-Laservision-PAL/IEC-60856-1986-Laservision-PAL.md): Laservision PAL.
  - [IEC 60856 Amendment 1](../analogue-video-specifications/docs/laserdisc/IEC-60856-1986-Laservision-PAL-Amendment-1/IEC-60856-1986-Laservision-PAL-Amendment-1.md): Laservision PAL Amendment 1.
  - [IEC 60856 Amendment 2](../analogue-video-specifications/docs/laserdisc/IEC-60856-1986-Laservision-PAL-Amendment-2/IEC-60856-1986-Laservision-PAL-Amendment-2.md): Laservision PAL Amendment 2.
  - [IEC 60857](../analogue-video-specifications/docs/laserdisc/IEC-60857-1986-Laservision-NTSC/IEC-60857-1986-Laservision-NTSC.md): Laservision NTSC.
  - [IEC 60857 Amendment 1](../analogue-video-specifications/docs/laserdisc/IEC-60857-1986-Laservision-NTSC-Amendment-1/IEC-60857-1986-Laservision-NTSC-Amendment-1.md): Laservision NTSC Amendment 1.
  - [IEC 60857 Amendment 2](../analogue-video-specifications/docs/laserdisc/IEC-60857-1986-Laservision-NTSC-Amendment-2/IEC-60857-1986-Laservision-NTSC-Amendment-2.md): Laservision NTSC Amendment 2.
  - [IEC 60908-1999](../analogue-video-specifications/docs/efm/IEC-60908-1999/IEC-60908-1999.md): Compact disc digital audio system (CD-DA) — EFM code, CIRC, and subcode used by LaserDisc digital audio.
  - [ECMA-130](../analogue-video-specifications/docs/efm/ECMA-130/ECMA-130.md): Data interchange on read-only 120 mm optical discs — machine-readable cross-check of the CD layer (CIRC Annex C, EFM table Annex D, merging bits Annex E).
  - [SMPTE 272M-1994](../analogue-video-specifications/docs/video_formats/SMPTE-272M-1994/SMPTE-272M-1994.md): Audio frame sequences for embedded audio, including the 44.1 kHz / 29.97 Hz sequence.
  - [SMPTE 12M](../analogue-video-specifications/docs/video_metadata/IEC-60461-2010-Time-and-control-code/IEC-60461-2010-Time-and-control-code.md): Vertical Interval Timecode (VITC).
  - [Vertical Interval Test Signals - NTSC and PAL definitions](../analogue-video-specifications/docs/video_metadata/VITS/index.md): VITS waveforms.
