# Troubleshooting

Common validation errors and what to do about each.

## Project structure

### `Top-level YAML contains unsupported field: 'x'`

Only seven top-level blocks exist: `project`, `cvbs_presets`, `output`, `line_injections`, `channel`, `chroma_error` and `sections`. Check for a typo, or for a key that belongs one level deeper.

The same message shape appears for every block — `section contains unsupported field: 'duration_frame'`, `cvbs_presets contains unsupported field: 'field_order'`. Unknown keys are always errors, never silently ignored.

### `section field 'section_type' has unrecognised value 'x'`

`section_type` accepts only `lead_in`, `programme_area` and `lead_out`.

### Sections in the wrong order

Once any section is a `lead_in` or `lead_out`, the sequence must be `[lead_in] programme_area… [lead_out]` — at most one lead-in and it must be first, at most one lead-out and it must be last, everything between them `programme_area`. Reorder the sections.

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

### `Progressive source not found: …`

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

### `start_frame N is beyond the end of the source (M frames, last frame M-1)`

`start_frame` (or `end_frame`, in the matching `end_frame N is beyond…` message) points past the clip's last frame. Frame numbers are 0-based, so a 250-frame clip runs from 0 to 249. Check the frame count in the GUI's **Source Profile** box, or with `ffprobe`.

### `end_frame must be >= start_frame`

The selected range is reversed. Swap the two values, or remove `end_frame` to run to the end of the clip.

### `end_frame cannot be combined with an integer duration_frames`

A range with an `end_frame` already sets the section's length. Remove `duration_frames` (or set it to `"all"`), and use `duration_repeat` to play the range more than once. To loop a clip for a fixed time instead, keep `duration_frames` and remove `end_frame`.

### `start_frame and end_frame select frames from a video source; an EXR still has only one frame`

Remove `start_frame` and `end_frame` from the still section.

### `an EXR still needs an integer duration_frames`

A still has no length of its own, so `duration_frames: "all"` (or leaving `duration_frames` out) does not apply. Give the section a frame count, for example `duration_frames: 250` for ten seconds of PAL.

## Line injections

### `target_lines must not be specified for laserdisc injections`

Laserdisc line placement is fixed by IEC 60856/60857 and computed per frame. Remove the `target_lines` key.

### `line injection type 'vitc' is not implemented in the current runtime`

VITC is described by the schema but has no runtime behaviour, so it is rejected rather than silently dropped. `type: line_content` is likewise rejected, with `unsupported injection type 'line_content'`. `laserdisc` is the only usable section-level injection type today.

### `VITS target line 17 conflicts with laserdisc reserved VBI ranges for NTSC`

The broadcast VITS lines collide with the laserdisc address-code ranges, which are reserved whenever `line_injections.disc_type` is set. On a disc, use the laserdisc VITS lines instead — set `placement: laserdisc` and use `virs` on 19/282 for colour reference. See [VITS types](../reference/vits-types.md).

### `NTSC and PAL-M laserdisc projects require a virs VITS injection for colour`

IEC 60857 §9.1.3 makes VIRS mandatory on NTSC and PAL-M discs, so any such project with `disc_type` set needs one. Add it to the **project-level** `line_injections:` block:

```yaml
line_injections:
  disc_type: CAV
  placement: laserdisc
  vits:
    - vits_type: virs
      target_lines: [19, 282]
```

### `overlapping VITS target line N within the project VITS set`

Two VITS entries target the same frame line. Each line can carry one thing.

## Laserdisc codes

### `code_type 'picture_number' is not allowed in 'lead_in' sections`

A programme-area code in a lead-in section. Check the [compatibility matrices](../user-manual/laserdisc.md#which-codes-go-where) and move the code to the right section.

### `picture_number start_value must be in the range 0-99999 for PAL; got 100000`

Picture numbers are capped at 99,999 (PAL) and 79,999 (NTSC and PAL-M). Only `start_value` is checked, so keep the numbers the disc will reach within the limit too. Lower the `start_value` or split the section.

### `chapter_number chapter must be in the range 0-79; got 80`

Chapters are 0–79.

### `users_code X1 nibble must be 0-7; '0x88D000' has X1=8`

The users code hex `0x8X₁D…` requires X₁ in 0–7, and the D digit must be `D`. `"0x87D000"` is valid; `"0x88D000"` is not.

### `CAV lead_out section has 500 frames; IEC 2 mm minimum at 1.6 um track pitch requires 1250`

A warning, not an error. IEC requires 2 mm of track after the programme, which is 1250 frames at the 1.6 µm pitch; a CAV lead-in needs 938 frames for the same reason (`CAV lead_in section has N frames; IEC 1.5 mm minimum …`). Extend the section if the disc should conform.

### `CAV chapter N starting in section 'x' lasts M frames; IEC 60856/60857 requires at least 30`

A warning. A chapter must span at least 30 tracks, which is 30 frames on CAV. Lengthen the chapter, or merge it into its neighbour.

### Timecodes appear to reset between sections

You have set `start_value: 1` on `picture_number` in a later section, which re-anchors the count there. IEC 60856/60857 §10.1.5 requires continuous numbering — set `start_value` on the **first** programme section only and omit it thereafter. Chapter boundaries never reset a timecode.

## Impairments

### `channel block must set at least one of: timing_offset_samples`

An empty `channel:` block configures nothing. Omit the block for an ideal channel, or set `timing_offset_samples`.

### `timing_offset_samples must be in the range [-8.0, 8.0]`

The offset is meant for sub-sample and few-sample alignment errors between pressings. Larger shifts are not a timing impairment; change the content instead. See [channel](../reference/channel.md).

### `chroma_error block must set at least one of: gain, phase_deg, line_jitter`

An empty `chroma_error:` block configures nothing. Omit the block for an error-free chroma path. `include_burst` alone also configures nothing: it only widens where the error applies, so add a `gain`, `phase_deg` or `line_jitter`. The same applies to an empty `line_jitter:`, which must set its `phase_deg` or `gain`; a `seed` alone draws nothing.

### `chroma_error.include_burst must be true or false`

`include_burst` is a boolean. Write `include_burst: true` or `include_burst: false`.

### `Chroma error validation error: … must be in the range …`

The ranges model a mis-adjusted but working player: half to one and a half times saturation, any hue, and per-line jitter up to 10° and 0.1 gain. See [chroma_error](../reference/chroma-error.md).

### `chroma_error is not supported for SECAM`

SECAM carries its colour as frequency-modulated subcarriers, with no quadrature chroma to rotate. Remove the `chroma_error:` block from SECAM projects.

### `noise_spread_db requires noise_db to be specified`

`noise_spread_db` is relative to the floor, so it needs one. Add `noise_db`.

### `spectrum requires noise_db to be specified`

A top-level `spectrum` shapes the capture layer, which only exists when `noise_db` is set. Add `noise_db`, or move the table under `shared:` if it describes the pressing.

### Noise values out of range

`noise_db` must be 20.0–61.0, and `noise_db − noise_spread_db` must be at least 20.0. Below 20 dB the noise approaches the amplitude of sync; above 61 dB it is smaller than the quantisation floor. `shared.noise_db` has the same 20.0–61.0 range.

### `spectrum frequency must be in the range` / `strictly ascending` / `relative_db must be in the range`

Spectrum breakpoints run from 0 MHz up to the 4fsc Nyquist frequency (8.867 MHz for PAL, 7.159 MHz for NTSC, 7.151 MHz for PAL-M, whatever the `sample_encoding_preset`), each frequency higher than the one before, with levels between −60 and +40 dB.

### `noise.shared does not take noise_spread_db`

Pressing noise has a constant level so that it is identical in every capture. Remove `noise_spread_db` from `shared:`; level-dependent noise belongs on the capture layer.

### `noise.shared requires noise_db` / `shared.noise_seed is required`

The pressing layer needs a level and a seed. The seed is what makes several captures share the same pressing noise — give every project of one pressing the same value.

### `pressing noise falls back to output position`

A warning. On a CAV disc, a section with `shared:` but no `picture_number` code has no picture number to key its pressing noise on, so it uses the output position instead. Captures that start at different points will not share that section's pressing noise. Add a `picture_number` code, or accept the fallback for lead-in and lead-out sections.

### `dropouts block has no active type`

Omit the whole block to disable dropouts rather than setting `scale: 0`. A block that looks like it configures something always does.

### `scratch lifespan derived from scale N (M frames) exceeds section duration`

A warning. The scratch may not complete its triangular envelope before the section ends, so it can be cut off before reaching full severity. Legal if that is what you want; otherwise lengthen the section or lower the scale.

## Audio

### `must use only one of 'left'/'right' tones, 'file' or 'video_audio'`

A channel pair carries one kind of content. Split tones and a file (or a soundtrack) across two pairs with different `pair` numbers.

### `file.path must name a .wav or .flac file`

Only WAV and FLAC audio files are accepted. Convert other formats first, for example `ffmpeg -i music.mp3 music.flac`.

### `audio file '…' could not be read: the file does not exist.`

The path did not resolve to a file. `{project}/…` resolves against the project file's directory, while a bare relative path resolves against the working directory in the CLI (and the project directory in the GUI); check the spelling and the [asset roots](../reference/asset-roots.md). The same message ends *the file has no audio stream.* for a file with no audio in it.

### `audio file '…' has N channels; only mono and stereo files are supported.`

Mix the file down to stereo first, for example `ffmpeg -i surround.wav -ac 2 stereo.wav`. Video soundtracks, by contrast, are downmixed automatically.

### `audio file '…' lasts … s but the section runs … s`

A warning: the file ends before the section does, so the rest of the section is silent. Set `repeat: true` to loop the file, or shorten the section.

### `video_audio needs a video source (MKV or MP4)`

A soundtrack comes from the section's own video, so an EXR still has none. Use a `file:` pair, or tones, instead.

### `the section source has N audio stream(s), so video_audio.stream S does not exist.`

`stream` counts the video's audio streams from 0. List them with `ffprobe -v error -select_streams a -show_entries stream=index,codec_name,channels <file>`, or look at the stream list in the section editor.

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
