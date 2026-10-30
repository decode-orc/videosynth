# MKV BT.601 Compliance Requirements

Generated: 2026-06-01

This document defines the checks a verification script must perform to claim that an MKV file under output/mkv is:

1. structurally correct as a Matroska deliverable; and
2. consistent with BT.601-derived standard-definition component video.

BT.601 defines the component digital video model, not the file container. Compliance therefore requires both container checks and signal-model checks.

- MKV deliverable checks: verify container, stream layout, codec, and metadata.
- BT.601 content checks: verify code ranges, sampling model, timing model, and active-line placement.
- Source-coupled checks: verify properties that cannot be proven from a decoded frame alone.

## 1. Normative and supporting sources

### BT.601 core

- ITU-R BT.601-5 (1995), Part A, Table 2: 13.5 MHz 4:2:2 member.
- ITU-R BT.601-5 (1995), sections 3.1 to 3.5: sampling, co-siting, matrix, quantization, limiting.
- ITU-R BT.601-5 (1995), Appendix 1 to Part A: active-line placement relative to horizontal reference.
- ITU-R BT.601-5 (1995), Appendix 2 to Part A: luminance and chroma filter masks.
- ITU-R BT.601-5 (1995), Annex 1: delay and implementation constraints.

### System-specific supporting sources

- SMPTE 170M-2004: 525/59.94 colorimetry, timing, waveform values.
- ITU-R BT.470-6 (1998): 625/50 timing and PAL-system values.

### Practical container references

- Matroska container conventions as reported by ffprobe.
- FFV1 lossless 10-bit 4:2:2 representation as used in archival workflows.

## 2. Scope and assumptions

This document targets files in:

- output/mkv/720x486/*.mkv
- output/mkv/720x576/*.mkv

Assumed video essence profile:

- 10-bit Y'CbCr 4:2:2
- FFV1 lossless in MKV
- raster 720x486 or 720x576
- SD colorimetry according to 525/59.94 or 625/50 family

Audio and timecode streams may be present. Their presence should not invalidate video compliance if the video stream passes and the additional streams are well-formed.

## 3. Numeric requirements needed for test design

This section contains the direct values a test script should use.

### 3.1 MKV structural requirements

| Item | 525 set | 625 set | How to verify |
| --- | --- | --- | --- |
| Container | Matroska MKV | Matroska MKV | ffprobe format_name contains matroska |
| Video stream count | exactly 1 | exactly 1 | ffprobe stream list |
| Video codec | ffv1 | ffv1 | codec_name |
| Pixel format | yuv422p10le | yuv422p10le | ffprobe pix_fmt |
| bits_per_raw_sample | 10 preferred | 10 preferred | ffprobe stream field if present |
| Width x Height | 720x486 | 720x576 | ffprobe width/height |
| Sample aspect ratio | 108:119 (~0.907563) | 128:117 (~1.094) | ffprobe SAR; may be rationalized by ffprobe and should be checked by numeric ratio equivalence |
| Frame rate | 30000/1001 | 25/1 | r_frame_rate and avg_frame_rate |
| field_order | bt | tb | ffprobe field_order |

Film for 2:3 pulldown is a progressive 525-line source: a 720x486 file at 24000/1001 frames/s must be tagged `field_order` progressive. All the other 525 requirements apply to it unchanged.

Additional stream policy:

- Audio stream: optional, if present should be PCM at 48 kHz.
- Timecode stream: optional, if present should be internally consistent with frame rate.

### 3.2 Color metadata requirements

These metadata checks verify declared interpretation, not actual pixel correctness by themselves.

| Item | 525 set expected | 625 set expected | How to verify |
| --- | --- | --- | --- |
| color_space (matrix) | smpte170m | smpte170m | ffprobe color_space |
| color_primaries | smpte170m | bt470bg | ffprobe color_primaries |
| color_transfer | bt709 or smpte170m | bt709 or bt470bg | ffprobe color_transfer |
| color_range | tv (limited) preferred; unknown allowed with warning | tv (limited) preferred; unknown allowed with warning | ffprobe color_range |

Compliance policy for transfer metadata:

- If the pipeline standardizes on bt709 transfer tags for SD content, this is allowed if matrix and primaries are correct and code values are BT.601-legal.
- If transfer is missing or unknown, report warning unless project policy requires strict metadata.

### 3.3 BT.601 13.5 MHz 4:2:2 signal model

| Item | 525 system | 625 system | Reference |
| --- | --- | --- | --- |
| Total Y samples per line | 858 | 864 | BT.601 Part A Table 2 |
| Total C_R samples per line | 429 | 432 | BT.601 Part A Table 2 |
| Total C_B samples per line | 429 | 432 | BT.601 Part A Table 2 |
| Active Y samples per line | 720 | 720 | BT.601 Part A Table 2 |
| Active C_R/C_B samples per line | 360/360 | 360/360 | BT.601 Part A Table 2 |
| Y sampling frequency | 13.5 MHz | 13.5 MHz | BT.601 Part A Table 2 |
| C_R/C_B sampling frequency | 6.75 MHz | 6.75 MHz | BT.601 Part A Table 2 |
| Chroma co-siting | odd-luma co-sited | odd-luma co-sited | BT.601 section 3.2 |

### 3.4 10-bit code-value requirements

In this document, 10-bit means scalar component code values for Y', C_R, C_B in the range 0 to 1023.

Nominal active-video values:

- Y nominal black: 64
- Y nominal peak white: 940
- Cb/Cr neutral: 512
- Nominal active Y range: 64 to 940
- Nominal active Cb/Cr range: 64 to 960

Derived from 8-bit BT.601 values by multiplying by 4.

Quantization formulas in 10-bit domain:

$$
\overline{Y}_{10} = 876 E_Y' + 64
$$

$$
\overline{C_R}_{10} = 640(E_R' - E_Y') + 512
$$

$$
\overline{C_B}_{10} \approx 504(E_B' - E_Y') + 512
$$

Nearest-integer quantization applies.

Script-level legality checks should use at least:

- strict nominal range check: Y in [64, 940], Cb/Cr in [64, 960]
- tolerant boundary check: allow plus/minus 4 codes for edge interpolation and filtering when explicitly configured

When a source-coupled comparison is available, the validator should additionally distinguish between:

- nominal-range outputs that remain fully inside the BT.601 studio range; and
- exact source passthrough that intentionally preserves footroom or headroom from the source.

If decoded MKV samples fall outside nominal range but the decoded frame is an exact code-value match to the corresponding source frame after the declared crop and pad steps, the script should report that as source-matched studio-range passthrough rather than a hard compliance failure.

### 3.5 Active-line placement and horizontal padding checks

For BT.601/BT.610-style SD digital video workflows carrying a 704-sample active image inside a 720-sample container line, the compliance script should check:

- active width is exactly 704 samples
- active region position is x=8..711
- left and right horizontal padding are 8 samples each
- padding samples are black (Y near black code, Cb and Cr near neutral) within configured tolerance

Recommended default tolerance for padding-black in 10-bit code space:

- Y <= 68
- abs(Cb - 512) <= 8
- abs(Cr - 512) <= 8

If a file declares crop side-data that implies a non-8/8 split, this must be reported and evaluated against project policy:

- strict policy: fail
- permissive policy: warn if pixel-domain padding and active alignment remain consistent with expected content placement

### 3.6 System timing model values

These values are used for timing-model consistency checks.

#### 525/59.94

| Item | Value |
| --- | --- |
| Field rate | 59.94005994 Hz |
| Frame rate | 29.97002997 Hz |
| Line rate | 15734.2657 Hz |
| Line period | 63.556 us |
| Lines per frame | 525 |

#### 625/50

| Item | Value |
| --- | --- |
| Field rate | 50 Hz |
| Frame rate | 25 Hz |
| Line rate | 15625 Hz |
| Line period | 64 us |
| Lines per frame | 625 |

### 3.7 Appendix 2 filter and delay checks (source-coupled)

An MKV file alone does not prove full pre-encoding filter compliance. To claim full BT.601 compliance, the workflow should include source-coupled measurements against Appendix 2 masks:

- luminance path insertion loss and stopband attenuation
- chroma path insertion loss and stopband attenuation
- passband ripple and group delay relative to 1 kHz
- luminance and chroma delay equalization requirements from Annex 1

Report these as source-coupled checks unless original processing stages are directly measured.

## 4. What is and is not provable from MKV alone

### Directly provable from MKV alone

- container and stream structure
- codec and pixel format
- raster, frame rate, SAR, interlace/field order metadata
- declared color metadata values
- actual decoded pixel code values and their legal-range occupancy

### Not fully provable from MKV alone

- original analog timing phase relative to O_H
- exact original capture-side chroma filtering and anti-alias filter compliance
- full Annex 1 delay-equalization chain correctness before file encoding

## 5. Minimum script outputs

A useful compliance script for output/mkv should report:

- container: MKV format and stream inventory checks
- video-essence: codec, pixel format, bit depth, raster, field order
- color-metadata: matrix, primaries, transfer, range
- bt601-code-legality: Y/Cb/Cr range occupancy and violations
- active-window-and-padding: 704-in-720 placement, side padding widths, black padding checks
- timing-model: 525/59.94 or 625/50 interpretation and applied constants
- source-coupled-results: Appendix 2 and Annex 1 derived checks when available
- non-provable-from-mkv: clauses requiring external source-domain evidence

## 6. Practical conclusion

A compliant result for this profile means:

- the MKV file is structurally valid and decodes as FFV1 lossless 10-bit 4:2:2 SD video with correct raster and timing metadata;
- decoded code values and active-window placement are consistent with BT.601 expectations;
- any non-provable items are explicitly identified and, when required, validated through source-coupled analysis.

Without source-coupled measurements, the strongest claim is that the MKV is BT.601-consistent in decoded digital representation, not that the entire upstream acquisition and filtering chain is fully proven compliant.
