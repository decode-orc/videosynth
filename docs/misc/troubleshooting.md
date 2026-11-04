# Troubleshooting

Common validation errors and what to do about each.

## Project structure

### `Top-level YAML contains unsupported field: 'x'`

Only nine top-level blocks exist: `project`, `cvbs_presets`, `output`, `line_injections`, `laserdisc`, `channel`, `chroma_error`, `line_timing` and `sections`. Check for a typo, or for a key that belongs one level deeper.

The same message shape appears for every block — `section contains unsupported field: 'duration_frame'`, `cvbs_presets contains unsupported field: 'field_order'`. Unknown keys are always errors, never silently ignored.

## Presets

### `signal_state_preset must be 'STANDARD_STABLE_UNLOCKED' for SECAM projects`

SECAM chroma is frequency modulated, so the CVBS file format defines no subcarrier phase lock for it and a SECAM file must not be declared phase locked. Set `signal_state_preset: STANDARD_STABLE_UNLOCKED`, or leave the key out to get it by default. PAL, NTSC and PAL-M projects need `STANDARD_STABLE_LOCKED`.

### `there is no SECAM LaserVision format, so the laserdisc block, section laserdisc blocks and laserdisc VITS placement are not available for SECAM projects`

Remove the project's `laserdisc:` block and every section's `laserdisc:` block, and use `standard` or `custom` VITS placement.

### `SECAM warning: VITS test signals are rendered with their PAL definitions`

A warning, not an error. SECAM uses the 625-line PAL VITS catalogue, so VITS lines keep their PAL waveforms, including PAL-modulated chroma packets. Remove the VITS set if a pure SECAM signal matters.

### `secam_field_identification has no effect unless video_standard_preset is SECAM`

A warning, not an error. Field identification is a SECAM signal; remove the key from PAL, NTSC and PAL-M projects.

### `ntsc_black_setup_ire can only be specified for NTSC or PAL-M projects`

The setup pedestal is a System M concept. Remove the key from PAL projects.

## LaserDisc settings

### `cvbs_presets.pal_laserdisc_pilot_burst has moved to laserdisc.pilot_burst.`

The project uses a LaserDisc key outside the `laserdisc:` blocks. Every LaserDisc setting lives in the project's `laserdisc:` block or in a section's own `laserdisc:` block, and each key found elsewhere gives this error naming its place:

| Rejected key | Use instead |
|---------|---------|
| `cvbs_presets.pal_laserdisc_pilot_burst` | `laserdisc.pilot_burst` |
| `cvbs_presets.ntsc_laserdisc_vbi_burst` | `laserdisc.vbi_burst` |
| `output.efm_audio` | `laserdisc.efm_audio` |
| `line_injections.disc_type` | `laserdisc.disc_type` |
| `channel.fm_beat` | `laserdisc.channel.fm_beat` |
| `sections[].section_type` | `sections[].laserdisc.area` |
| `sections[].line_injections` entry with `type: laserdisc` | `sections[].laserdisc.codes` |

Move each key; the values are unchanged. A section's codes list moves up one level, directly under `laserdisc:`:

```yaml
    laserdisc:
      area: programme_area
      codes:
        - code_type: picture_number
          start_value: 1
```

Opening and saving the project in the GUI does not convert it: the keys fail to parse. `scripts/migrate_laserdisc_keys.py` rewrites such files in place; see [`laserdisc`](../reference/laserdisc.md#laserdisc-keys-in-other-blocks).

### `section.laserdisc.area has unrecognised value 'x'`

`area` accepts only `lead_in`, `programme_area` and `lead_out`.

### `section 'x' has a laserdisc block, but the project has no top-level laserdisc block`

A section can sit on a disc only when the project is one. Add a top-level `laserdisc:` block — with `disc_type` if any section carries biphase codes.

### `a section declares laserdisc codes but laserdisc.disc_type is not set`

Biphase codes need a disc format. Set `laserdisc.disc_type` to `CAV` or `CLV`.

### `section 'x' carries laserdisc codes, so its laserdisc.area must be set`

The disc area decides which codes are legal, so a section with `codes` needs `area` beside them.

### Sections in the wrong order

Once any section's `area` is `lead_in` or `lead_out`, the sequence must be `[lead_in] programme_area… [lead_out]` — at most one lead-in and it must be first, at most one lead-out and it must be last, everything between them `programme_area`. Reorder the sections.

### `laserdisc.pilot_burst can only be enabled for PAL projects`

The pilot burst is defined by IEC 60856 for PAL LaserVision. The NTSC equivalent is `laserdisc.vbi_burst`.

### `laserdisc.vbi_burst can only be enabled for NTSC projects`

The VBI burst is defined by IEC 60857 for NTSC LaserVision. Remove it from PAL and PAL-M projects; PAL discs use `laserdisc.pilot_burst`.

### `laserdisc.pilot_burst warning: preset 'CVBS_U10_4FSC' clips sub-sync excursions below -300 mV`

A warning, not an error. The pilot burst trough reaches −600 mV, which an unsigned preset cannot represent. Switch to `CVBS_S16_4FSC`, `RAW_S16_28M` or `RAW_S16_40M` to keep the full waveform, or accept the clipping if the burst is not what you are testing.

### `laserdisc.vbi_burst warning: preset 'CVBS_U10_4FSC' clips sub-sync excursions below sync tip`

A warning, not an error. The colour burst on the broad field-sync pulses is centred on sync tip and reaches −60 IRE, below the lowest legal code of an unsigned preset. Switch to `CVBS_S16_4FSC`, `RAW_S16_28M` or `RAW_S16_40M` to keep the full waveform, or accept the clipping if the burst is not what you are testing.

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

### `source frame rate must be …`

25 fps for PAL and SECAM, 30000/1001 for NTSC and PAL-M, and 24000/1001 film for a section with a `"2:3"` [cadence](../reference/sections.md#cadence). Frame rate conversion is not performed; convert with FFmpeg's `fps` filter first. A film clip in a video-cadence section, or a 29.97 fps clip in a `"2:3"` section, also lands here: set the section's `cadence` to match the clip.

### `cadence pattern "2:3" needs a 525-line standard`

2:3 pulldown converts 24 fps film to 59.94 Hz fields. 625-line film runs at 25 fps with one frame per two fields, which is the default video cadence, so remove the `cadence` block.

### `cadence pattern "2:3" needs a video source`

An EXR still has no film frames to spread over fields. Use the video cadence for stills.

### `cadence continuity 'continue' needs the previous section to use pattern "2:3"`

There is no cadence to carry on from. Use `continuity: restart` with a `phase`.

### `cadence phase is ignored with continuity 'continue'` / `cadence phase and continuity apply only to pattern "2:3"`

These warnings mean a key has no effect. A continuing section takes its position in the cycle from the previous section, and a video-cadence section has no cycle at all. Remove the key, or switch to `continuity: restart` or `pattern: "2:3"` if that was the intent.

### A film section shows combed or repeated frames

That is 2:3 pulldown working as intended: in every five output frames, two weave fields from two film frames, and B and D film frames each fill three fields, so one of their fields repeats. Use the `{film_frame}` and `{cadence}` [OSD tokens](../user-manual/osd.md#tokens) to see which film frame each field shows. See [Film and pulldown](../user-manual/sources.md#film-and-pulldown).

### A 2:3 section's picture numbers skip frames

On a CAV disc the picture number counts film frames and goes on the first field of each one, so one output frame in five carries no picture number and no white flag. That is how film is mastered to CAV; see [Laserdisc: Film sections](../user-manual/laserdisc.md#film-sections).

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

### `laserdisc.codes[] contains unsupported field: 'target_lines'`

Laserdisc line placement is fixed by IEC 60856/60857 and computed per frame, so codes have no `target_lines`. Remove the key.

### `line injection type 'vitc' is not implemented in the current runtime`

VITC is described by the schema but has no runtime behaviour, so it is rejected rather than silently dropped. `type: line_content` is likewise rejected, with `unsupported injection type 'line_content'`. Laserdisc codes are not line injections: they go in the section's [`laserdisc.codes`](../reference/section-laserdisc.md#codes) list.

### `VITS target line 17 conflicts with laserdisc reserved VBI ranges for NTSC`

The broadcast VITS lines collide with the laserdisc address-code ranges, which are reserved whenever `laserdisc.disc_type` is set. On a disc, use the laserdisc VITS lines instead — set `placement: laserdisc` and use `virs` on 19/282 for colour reference. See [VITS types](../reference/vits-types.md).

### `NTSC and PAL-M laserdisc projects require a virs VITS injection for colour`

IEC 60857 §9.1.3 makes VIRS mandatory on NTSC and PAL-M discs, so any such project with `laserdisc.disc_type` set needs one. Add it to the **project-level** `line_injections:` block:

```yaml
line_injections:
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

### `channel block must set at least one of: timing_offset_samples, edge_smear`

An empty `channel:` block configures nothing. Omit the block for an ideal channel, or set `timing_offset_samples` or `edge_smear`. Moving `fm_beat` to `laserdisc.channel` out of a `channel:` block that held nothing else leaves it empty, so remove the block.

### `timing_offset_samples must be in the range [-8.0, 8.0]`

The offset is meant for sub-sample and few-sample alignment errors between pressings. Larger shifts are not a timing impairment; change the content instead. See [channel](../reference/channel.md).

### `channel.fm_beat has moved to laserdisc.channel.fm_beat.`

The FM beat is a LaserVision impairment, so it is in the [LaserVision channel](../reference/laserdisc-channel.md). Move the `fm_beat:` block under `laserdisc: channel:`; its keys are unchanged.

### `Laserdisc channel validation error: fm_beat is supported for 625-line PAL only`

The beat model is fitted to PAL LaserDisc pressings. Remove `fm_beat` from NTSC and PAL-M projects.

### `group_delay.preset iec60856_nominal is the 625-line PAL pre-distortion of IEC 60856`

The nominal curve is PAL's. On NTSC or PAL-M, give the curve yourself as `points_ns`.

### `response_db and response_by_picture cannot both be set`

`response_by_picture` already gives the replay response at every disc position. Keep one of the two.

### `chroma_error block must set at least one of: gain, phase_deg, line_jitter, differential, crosstalk`

An empty `chroma_error:` block configures nothing. Omit the block for an error-free chroma path. `include_burst` alone also configures nothing: it only widens where the error applies, so add one of the others. The same applies to an empty `line_jitter:`, which must set its `phase_deg` or `gain` (a `seed` alone draws nothing), an empty `differential:` (its `gain` or `phase_deg`) and an empty `crosstalk:` (its `v_to_u` or `u_to_v`).

### `chroma_error.include_burst must be true or false`

`include_burst` is a boolean. Write `include_burst: true` or `include_burst: false`.

### `Chroma error validation error: … must be in the range …`

The ranges model a mis-adjusted but working player: half to one and a half times saturation, any hue, and per-line jitter up to 10° and 0.1 gain. See [chroma_error](../reference/chroma-error.md).

### `crosstalk.alternate_with_switch follows the PAL V-switch, so it needs a PAL or PAL-M project`

NTSC has no V-switch for the crosstalk to alternate with. Leave `alternate_with_switch` out (or `false`) for a fixed leak.

### `chroma_error is not supported for SECAM`

SECAM carries its colour as frequency-modulated subcarriers, with no quadrature chroma to rotate. Remove the `chroma_error:` block from SECAM projects.

### `line_timing block must set at least one of: line_ns, field_ns, slope_ns, shared`

An empty `line_timing:` block configures nothing. Omit the block for a perfectly time-base-corrected picture. `line_correlation`, `seed` and `include_burst` alone configure nothing either: add one of the figures, or a `shared:` layer. `shared:` must likewise set its `line_ns`, `field_ns` or `slope_ns`.

### `line_timing.seed seeds the capture layer, which sets none of: line_ns, field_ns, slope_ns`

The top-level `seed` belongs to the capture layer. With only a `shared:` layer, put the seed inside `shared:`, or remove it.

### `Line timing validation error: … must be in the range …`

The figures are r.m.s. values in ns. A real capture after time-base correction measures a few ns (about a tenth of a 4fsc sample); 50 ns is close to a whole sample. `line_correlation` must stay below 1 for the process to be stationary. See [line_timing](../reference/line-timing.md).

### `Line timing validation error: shared.seed is required`

The pressing layer is the part recorded on the disc, so every capture of the pressing must draw the same values. Give `shared:` a fixed `seed`.

### `Line timing validation error: include_burst needs a colour burst; SECAM has none`

SECAM carries no colour burst for `include_burst` to move. Leave it out on SECAM projects; the rest of the block applies.

### `chroma_line_sign requires a PAL or PAL-M project`

The regions follow the PAL V-switch, which NTSC and SECAM do not have. Remove the `chroma_line_sign:` block, or use a PAL or PAL-M project. See [chroma_line_sign](../reference/sections.md#chroma_line_sign).

### `chroma_line_sign[N].region must have a positive width and height and lie inside the … source raster`

Regions are in source pixels and rows: x + width at most 720, y + height at most 576 (PAL) or 486 (PAL-M).

### `chroma_line_sign[N] changes nothing`

A region with both `u` and `v` left `normal` has no effect. Set `v: follow_switch` (line-alternating V) or `u: follow_switch` (anti-PAL U), or remove the region.

### A `chroma_line_sign` bar still shows colour in the decoded picture

That is what the bar is for. A correct PAL delay-line decoder averages the alternating component to grey; colour or "Venetian blinds" in the bar point at the decoder's delay line or demodulator, and a 2D decoder that separates by position alone will leave it coloured.

### `picture_response must be the preset name 'domesday' or a map of horizontal_db / vertical_db curves`

The only preset is `domesday`. For any other response, give the curves as a map: `picture_response: {horizontal_db: [[MHz, dB], ...], vertical_db: [[cycles/row, dB], ...]}`. See [picture_response](../reference/sections.md#picture_response).

### `Picture response validation error: … must be in the range …` / `strictly ascending`

Horizontal frequencies run from 0 MHz to 2 f_sc (8.867 MHz PAL, 7.159 MHz NTSC, 7.151 MHz PAL-M); vertical ones from 0 to 0.5 cycles per frame row. Levels must lie between −60 and +20 dB, and each curve's frequencies must increase from point to point.

### A softened still looks no different below 2 MHz

That is intended. The response is normalised to unit gain at zero frequency, and the `domesday` preset is flat up to 2.25 MHz horizontally and 0.2 cycles per row vertically. It removes the fine detail that real programme pictures lack, not the coarse detail they have.

### `noise_spread_db requires noise_db to be specified`

`noise_spread_db` is relative to the floor, so it needs one. Add `noise_db`.

### `spectrum requires noise_db to be specified`

A top-level `spectrum` shapes the capture layer, which only exists when `noise_db` is set. Add `noise_db`, or move the table under `shared:` if it describes the pressing.

### `spread_spectrum requires noise_db to be specified`

`spread_spectrum` shapes the capture layer's level-dependent term, which needs a capture layer. Add `noise_db` (and a non-zero `noise_spread_db`).

### `spread_spectrum is set, but noise_spread_db is 0`

A warning: with no white spread there is no level-dependent term, so the table has no effect. Set `noise_spread_db`, or remove `spread_spectrum`.

### Noise values out of range

`noise_db` must be 20.0–61.0, and `noise_db − noise_spread_db` must be at least 20.0. Below 20 dB the noise approaches the amplitude of sync; above 61 dB it is smaller than the quantisation floor. `shared.noise_db` and `shared.level_term.noise_db` have the same 20.0–61.0 range.

### `spectrum frequency must be in the range` / `strictly ascending` / `relative_db must be in the range`

The same messages name `spread_spectrum`, `shared.spectrum` or `shared.level_term.spectrum` when that table is at fault. Spectrum breakpoints run from 0 MHz up to the 4fsc Nyquist frequency (8.867 MHz for PAL, 7.159 MHz for NTSC, 7.151 MHz for PAL-M, whatever the `sample_encoding_preset`), each frequency higher than the one before, with levels between −60 and +40 dB.

### `noise.shared does not take noise_spread_db`

The capture layer's white spread follows each capture's own signal, which would make the pressing noise differ between captures. Remove `noise_spread_db` from `shared:`. For pressing noise that changes with the picture level, use [`shared.level_term`](../reference/section-noise.md#shared-level_term), which follows the pressing's own picture.

### `noise.shared.level_term requires noise_db and level_db`

The level term needs a level and a curve. Add both; a one-point `level_db` such as `[[0, 0.0]]` gives a constant second component.

### `shared.level_term.level_db …`

`level_db` needs at least one `[level_mv, relative_db]` point. Levels run from 0.0 to 800.0 mV above blanking, each higher than the one before; gains run from −60.0 to +20.0 dB.

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

A warning will say that no section declares the pair named in `laserdisc.efm_audio`. Add an `audio:` block declaring that pair to at least one section.

### `output.truth requires a PAL, NTSC or PAL-M project`

SECAM carries its colour as frequency-modulated subcarriers, with no U/V quadrature components for the truth to record. Remove `truth: true` from SECAM projects.

### `output.truth excludes OSD overlays`

OSD text is drawn after the truth is taken, so the truth shows the picture under the text. Remove the overlays from projects whose decoded picture will be scored against the truth.

### EFM track warnings

Tracks shorter than 4 s (6 s for the first, whose leading 2 s are the mandatory pause) and a project with no `lead_in` section both produce warnings. The file is still written; it just is not what a real disc would look like.

## Performance and storage

### The output is enormous

A PAL 4fsc frame is about 1.4 MB, so roughly 2.1 GB per minute. Use `--output-root` to send generation somewhere with room, and check the frame count before starting a long run.

### Generation is slow

Check `--threads` — `auto` is the default, and `1` is much slower by design. The frame template cache (`--template-cache-mb`, default 512) also matters; disabling it costs speed without changing the output.

## Getting help

If none of this covers your problem, see [Reporting an issue](issue-reporting.md).
