# Troubleshooting

Common validation errors and what to do about each.

## Project structure

### `Top-level YAML contains unsupported field: 'x'`

Only five top-level blocks exist: `project`, `cvbs_presets`, `output`, `line_injections` and `sections`. Check for a typo, or for a key that belongs one level deeper.

The same message shape appears for every block — `section contains unsupported field: 'duration_frame'`, `cvbs_presets contains unsupported field: 'field_order'`. Unknown keys are always errors, never silently ignored.

### `section field 'section_type' has unrecognised value 'x'`

`section_type` accepts only `lead_in`, `programme_area` and `lead_out`.

### Sections in the wrong order

Once any section declares a `section_type`, the sequence must be `[lead_in] programme_area… [lead_out]` — at most one lead-in and it must be first, at most one lead-out and it must be last, everything between them `programme_area`. Reorder the sections.

## Presets

### `pal_laserdisc_pilot_burst can only be enabled for PAL projects`

The pilot burst is defined by IEC 60856 for PAL Laservision. For NTSC there is no equivalent option that is currently implemented.

### `signal_state_preset must be 'STANDARD_STABLE_UNLOCKED' for SECAM projects`

SECAM chroma is frequency modulated, so the CVBS file format defines no subcarrier phase lock for it and a SECAM file must not be declared phase locked. Set `signal_state_preset: STANDARD_STABLE_UNLOCKED`, or leave the key out to get it by default. PAL, NTSC and PAL-M projects need `STANDARD_STABLE_LOCKED`.

### `laserdisc line injections, line_injections.disc_type and laserdisc VITS placement are not available for SECAM projects`

There is no SECAM LaserVision format. Remove the section `laserdisc` injections and `line_injections.disc_type`, and use `standard` or `custom` VITS placement.

### `SECAM warning: VITS test signals are rendered with their PAL definitions`

A warning, not an error. SECAM uses the 625-line PAL VITS catalogue, so VITS lines keep their PAL waveforms, including PAL-modulated chroma packets. Remove the VITS set if a pure SECAM signal matters.

### `secam_field_identification has no effect unless video_standard_preset is SECAM`

A warning, not an error. Field identification is a SECAM signal; remove the key from PAL, NTSC and PAL-M projects.

### `ntsc_laserdisc_vbi_burst is parsed but not implemented in the current runtime`

The option exists in the schema but has no signal behaviour, so enabling it is an error rather than a silent no-op. Remove it or set it to `false`.

### `ntsc_black_setup_ire can only be specified for NTSC or PAL-M projects`

The setup pedestal is a System M concept. Remove the key from PAL projects.

### `pal_laserdisc_pilot_burst warning: preset 'CVBS_U10_4FSC' clips sub-sync excursions below -300 mV`

A warning, not an error. The pilot burst trough reaches −600 mV, which an unsigned preset cannot represent. Switch to `CVBS_S16_4FSC`, `RAW_S16_28M` or `RAW_S16_40M` to keep the full waveform, or accept the clipping if the burst is not what you are testing.

## Sources

### `File '…' not found`

Check the path and the [asset root](../reference/asset-roots.md) it uses. The commonest cause is a bare relative path, which resolves against the working directory in the CLI but against the project directory in the GUI. Use `{bundled}/…` for shipped assets and `{project}/…` for files beside the project.

### `Unsupported progressive source family. Supported source families are EXR stills and MKV or MP4 video.`

The source extension is not `.exr`, `.mkv` or `.mp4`. Remux other containers to Matroska (`ffmpeg -i in.avi -c copy out.mkv`) or re-encode them.

### `Progressive EXR …` (channels, compression, windows, gamma, framesPerSecond, pixelAspectRatio)

The EXR does not match the still profile: it must be single-frame scanline with `R/G/B` `FLOAT` channels, no compression, full-raster windows, `gamma` 1 and the standard's frame-rate and pixel-aspect metadata. See [Picture sources](../user-manual/sources.md#exr-still-image).

### `source has no video stream`

The file has no video stream FFmpeg can see — it may be audio-only, or damaged.

### `source container '…' does not match its file extension`

A `.mkv` file must really be Matroska and a `.mp4` really MP4. Rename the file to match its content, or remux it.

### `source raster must be 720x576 for PAL and 720x486 for NTSC or PAL-M`

PAL and SECAM require exactly 720×576, NTSC and PAL-M exactly 720×486. There is no scaling path — resize the source before ingesting it (see [Converting your own material](../user-manual/sources.md#converting-your-own-material)).

### `source frame rate must match selected video standard`

25 fps for PAL and SECAM, 30000/1001 for NTSC and PAL-M. Frame rate conversion is not performed; convert with FFmpeg's `fps` filter first.

### `source has a variable frame rate; convert it to a constant frame rate first`

The clip's average frame rate differs from its base rate, typical of phone and screen recordings. Re-encode with a fixed rate, for example `-vf fps=25` (PAL) or `-vf fps=30000/1001` (NTSC).

### `source must not include stream crop metadata`

The stream carries crop side data, which would change the visible raster. Re-encode without it.

### `source is tagged field order '…', opposite to the output's … dominance`

A warning. If the clip really is interlaced with that field order, its fields will play in the wrong temporal order. Fix the material (or its tag, if the tag is wrong) with FFmpeg's `fieldorder` or `setfield` filters.

### `source sample aspect ratio differs from BT.601 for this standard`

A warning. The picture is used pixel-for-pixel, so content mastered for a different aspect ratio will look stretched or squeezed.

### `Progressive video source ended at frame N, before the probed frame count of M`

The container declared more frames than could be decoded — usually a truncated or damaged file. Re-mux or re-encode it.

### `Requested frame index exceeds progressive video source length`

A fixed `duration_frames` (counted from `start_frame`) asks for more frames than the clip holds, or `duration_frames: "all"` is combined with a non-zero `start_frame`. Looping a short clip is not implemented yet; use `duration_frames: "all"` with `duration_repeat` to replay a clip.

## Line injections

### `target_lines must not be specified for laserdisc injections`

Laserdisc line placement is fixed by IEC 60856/60857 and computed per frame. Remove the `target_lines` key.

### `line injection type 'vitc' is not implemented in the current runtime`

VITC and `line_content` are described by the schema but have no runtime behaviour, so they are rejected rather than silently dropped. `laserdisc` is the only usable section-level injection type today.

### `VITS type 'ntc7-composite' targets line 17 which is within the NTSC laserdisc reserved range`

The broadcast VITS lines collide with the laserdisc address-code ranges. On a disc, use the laserdisc VITS lines instead — set `placement: laserdisc` and use `virs` on 19/282 for colour reference. See [VITS types](../reference/vits-types.md).

### `NTSC laserdisc section 'x' requires a 'virs' VITS injection`

IEC 60857 §9.1.3 makes VIRS mandatory on NTSC discs. Add it to the **project-level** `line_injections:` block:

```yaml
line_injections:
  disc_type: CAV
  placement: laserdisc
  vits:
    - vits_type: virs
      target_lines: [19, 282]
```

### `Overlapping target_lines in section: x`

Two injections target the same frame line. Each line can carry one thing.

## Laserdisc codes

### `code_type 'picture_number' is not valid in section_type 'lead_in'`

A programme-area code in a lead-in section. Check the [compatibility matrices](../user-manual/laserdisc.md#which-codes-go-where) and move the code to the right section.

### `picture_number start_value 100000 exceeds PAL maximum of 99999`

Picture numbers are capped at 99,999 (PAL) and 79,999 (NTSC), and the *final* number in the section must fit too. Lower the `start_value` or split the section.

### `chapter_number 80 exceeds maximum of 79`

Chapters are 0–79.

### `code_type 'users_code': X₁ value must be 0-7, got 8`

The users code hex `0x8X₁D…` requires X₁ in 0–7. `"0x87D000"` is valid; `"0x88D000"` is not.

### `lead_out section duration 500 frames is below minimum of 1250 frames`

IEC requires 2 mm of track after the programme, which is 1250 frames at the 1.6 µm pitch. Lead-in needs 938 frames for the same reason. Extend the section.

### Timecodes appear to reset between sections

You have set `start_value: 1` on `picture_number` in a later section, which re-anchors the count there. IEC 60856/60857 §10.1.5 requires continuous numbering — set `start_value` on the **first** programme section only and omit it thereafter. Chapter boundaries never reset a timecode.

## Impairments

### `noise_spread_db present without noise_db`

`noise_spread_db` is relative to the floor, so it needs one. Add `noise_db`.

### Noise values out of range

`noise_db` must be 20.0–61.0, and `noise_db − noise_spread_db` must be at least 20.0. Below 20 dB the noise approaches the amplitude of sync; above 61 dB it is smaller than the quantisation floor.

### A `dropouts:` block with everything at zero is an error

Omit the whole block to disable dropouts rather than setting `scale: 0`. A block that looks like it configures something always does.

### `scratch lifespan exceeds section duration`

A warning. The scratch will not complete its triangular envelope before the section ends, so it never reaches full severity. Legal if that is what you want; otherwise lengthen the section or lower the scale.

## Audio

### No `.efm` file is written

A warning will say that no section declares the pair named in `output.efm_audio`. Add an `audio:` block declaring that pair to at least one section.

### EFM track warnings

Tracks shorter than 4 s (6 s for the first, whose leading 2 s are the mandatory pause) and a project with no `lead_in` section both produce warnings. The file is still written; it just is not what a real disc would look like.

## Performance and storage

### The output is enormous

A PAL 4fsc frame is about 1.4 MB, so roughly 2.1 GB per minute. Use `--output-root` to send generation somewhere with room, and check the frame count before starting a long run.

### Generation is slow

Check `--threads` — `auto` is the default, and `1` is much slower by design. The frame template cache (`--template-cache-mb`, default 512) also matters; disabling it costs speed without changing the output.

## Getting help

If none of this covers your problem, see [Reporting an issue](issue-reporting.md).
