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

### `chroma_encoding SECAM requires video_standard_preset PAL (625-line, 50 Hz)`

SECAM is a 625-line, 50 Hz system and is carried on the PAL preset's line structure and sample lattice. Set `video_standard_preset: PAL`.

### `pal_laserdisc_pilot_burst cannot be combined with chroma_encoding SECAM`

The pilot burst belongs to PAL Laservision. Remove one of the two keys.

### `cvbs_presets.chroma_encoding must be STANDARD or SECAM`

Only these two values exist; MESECAM and other variants are not supported.

### `chroma_encoding SECAM warning: VITS test signals are rendered with their PAL definitions`

A warning, not an error. VITS lines keep their PAL waveforms, including PAL-modulated chroma packets. Remove the VITS set if a pure SECAM signal matters.

### `ntsc_laserdisc_vbi_burst is parsed but not implemented in the current runtime`

The option exists in the schema but has no signal behaviour, so enabling it is an error rather than a silent no-op. Remove it or set it to `false`.

### `ntsc_black_setup_ire can only be specified for NTSC or PAL-M projects`

The setup pedestal is a System M concept. Remove the key from PAL projects.

### `pal_laserdisc_pilot_burst warning: preset 'CVBS_U10_4FSC' clips sub-sync excursions below -300 mV`

A warning, not an error. The pilot burst trough reaches −600 mV, which an unsigned preset cannot represent. Switch to `CVBS_S16_4FSC`, `RAW_S16_28M` or `RAW_S16_40M` to keep the full waveform, or accept the clipping if the burst is not what you are testing.

## Sources

### `File '…' not found`

Check the path and the [asset root](../reference/asset-roots.md) it uses. The commonest cause is a bare relative path, which resolves against the working directory in the CLI but against the project directory in the GUI. Use `{bundled}/…` for shipped assets and `{project}/…` for files beside the project.

### `Progressive source is not in a supported profile`

The file's container, codec, chroma format or bit depth does not match. EXR must be single-frame scanline with `R/G/B` `FLOAT` channels and no compression; MKV must be FFV1 at `yuv422p10le`. The extension alone is never enough.

### `Progressive source dimensions are invalid for the selected standard`

PAL requires exactly 720×576, NTSC and PAL-M exactly 720×486. There is no scaling path — resize the source before ingesting it.

### `Input frame rate must match the output standard's frame rate`

25 fps for PAL, 30000/1001 for NTSC and PAL-M. Frame rate conversion is not performed.

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
