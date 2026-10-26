# Media Sources Implementation Plan

Adds a bundled black still, general-purpose video ingestion (MKV and MP4),
source frame ranges, and file/video-derived audio channel pairs.

References:

- [High-level design §4 Generation Stage](design/high-level-design.md) — *Source Colour Space Requirement*, *Decoded Source Cache and Prefetch*
- [High-level design §8.1 Progressive Sources](design/high-level-design.md)
- [High-level design §7 `audio:` sub-key](design/high-level-design.md)
- [CVBS File Format Specification — Audio Data, `audio_channel_pair` table](cvbs-file-format-specification/docs/index.md)
- [ITU-R BT.601-5 (1995)](analogue-video-specifications/docs/video_formats/BT-601-5-1995) — studio-swing coding (Y 64–940, Cb/Cr 64–960)
- [SMPTE 272M-1994](analogue-video-specifications/docs/video_formats/SMPTE-272M-1994) — §14.3 audio samples per frame
- [EXR BT.601 compliance](../videosynth-assets/docs/exr-bt601-compliance-requirements.md), [MKV BT.601 compliance](../videosynth-assets/docs/mkv-bt601-compliance-requirements.md)
- Code: [progressive_frame_source.cpp](../src/progressive_frame_source.cpp), [progressive_frame_source_probe.cpp](../src/progressive_frame_source_probe.cpp), [project_validator.cpp](../src/project_validator.cpp), [generation_stage.cpp](../src/generation_stage.cpp) (`BuildFramePatternSchedule`), [audio_track_generator.cpp](../src/audio_track_generator.cpp), [model.h](../include/videosynth/model.h), [section_editor.cpp](../src/gui/section_editor.cpp), [audio_channel_pairs_editor.cpp](../src/gui/audio_channel_pairs_editor.cpp)

Design decisions that apply across all phases:

- **One video profile for every source.** The relaxed rules apply to bundled
  and local files alike; behaviour never depends on which asset root a path
  came from. Bundled assets stay BT.601-conformant through the asset
  verification scripts, not through runtime strictness.
- **Hard requirements are raster and rate only.** 720×576 @ 25/1 (625-line)
  or 720×486 @ 30000/1001 (525-line), constant frame rate. Everything else is
  normalised (range, matrix, chroma format, bit depth) or reported as a
  warning (field order, SAR). There is still no scaling or rate conversion.
- **Streaming decode.** Whole-clip decode into memory (~2.4 MiB per decoded
  frame) cannot hold user-length material (a 30-minute PAL file is ~100 GiB),
  so video decode becomes a bounded, forward-reading window.
- **ffmpeg stays an external process** (`ffprobe`/`ffmpeg` via `popen`),
  matching the existing decode path and the Flatpak packaging. All decode
  goes behind interfaces so unit tests use fakes.
- **Pass-through stays bit-exact.** A source already at `yuv422p10le`, `tv`
  range, `smpte170m` matrix takes no conversion filter, so
  `scripts/output-hashes.sh` baselines for existing projects are unchanged.

---

## Phase 1 — Black still and general video ingestion

### Task 1.1 — Bundled `BLACK.exr`

Add `videosynth-assets/assets/exr/720x576/BLACK.exr` and
`videosynth-assets/assets/exr/720x486/BLACK.exr`: every pixel `R=G=B=0.0`,
which ingests as Y′=64, Cb=Cr=512 (BT.601 studio black). Generate with a
small script under `scripts/` (or an option on
`convert_progressive_raw_to_exr.py`) so the file is reproducible; it must
satisfy the probe rules (FLOAT RGB, no compression, full-raster windows,
`gamma=1`, standard frame-rate and pixel-aspect metadata).

Acceptance:
- `videosynth-assets/scripts/run_verify_bt601_exr.sh` passes for both files.
- Functional test: `ProgressiveFrameSource` decodes each file to a raster of
  exactly `{64, 512, 512}` in every pixel.
- Functional test: a PAL project using it emits active-line samples at
  blanking level; an NTSC project emits the configured setup level (7.5 or
  0 IRE).
- Appears automatically in the GUI built-in EXR list (directory listing in
  `SectionEditor::PopulateBuiltinFiles`).

### Task 1.2 — Video decoder interface

Introduce `IVideoFrameDecoder` (in `interfaces.h` or a new
`video_frame_decoder.h`): `Open(source, standard, first_frame, colour_plan)`
and `ReadFrame(FrameSourceImage*)` returning frames sequentially, plus
`FfmpegVideoFrameDecoder` implementing it by holding one `ffmpeg … -f
rawvideo -pix_fmt yuv422p10le -` pipe open and reading one frame-sized block
per call. Seeking opens at `-ss <first_frame / rate>` (input-side, decoded
seek, which is frame-accurate). Move the existing yuv422p10le→`YCbCr444Pixel`
unpacking out of `DecodeMkvFrames` into a shared helper.

Acceptance:
- Unit tests for the unpacking helper (planar 4:2:2 → 4:4:4 siting, 10-bit
  masking) with in-memory buffers.
- Functional test: opening at frame N of a generated clip returns the same
  pixels as reading frames 0..N sequentially (exact seek).
- Thread-safety contract documented (one decoder per caller, not shared).

### Task 1.3 — Bounded decode window in `ProgressiveFrameSource`

Replace the whole-clip `DecodedSource::frames` vector for video sources with
a sliding window `[base, base + capacity)` fed by one `IVideoFrameDecoder`
per cache entry:
- Request inside the window → served from it.
- Request ahead of the window → decoder reads forward, evicting the oldest
  frames.
- Request behind the window (loop/repeat restart) → decoder reopens at the
  requested frame.
- Capacity covers the frame-synthesis pool's in-flight span (threads × batch
  plus margin), passed in at construction; document the memory bound
  (capacity × raster size) in the class comment.
- `ResolveFrameCount` uses the probe's count and no longer decodes the clip.
- `PrefetchSection` decodes the first window of the next section.
- Inject the decoder through a factory so unit tests use a fake decoder.

Acceptance:
- Unit tests with a fake decoder: forward reads, out-of-order requests within
  the window, backward restart, eviction bound, prefetch, concurrent callers
  on the same source share one decoder.
- Peak RSS for a 3 000-frame clip stays within the documented bound
  (functional benchmark, not in the unit lane).
- Byte-identical output for all existing projects (`output-hashes.sh`) and
  for `--threads 1` vs `--threads N`.

### Task 1.4 — General video probe

Extend `ProgressiveFrameSourceProbe` to accept `.mkv` and `.mp4` (Matroska
and `mov,mp4,…` demuxers), any decodable video codec. Additionally capture
`avg_frame_rate`, the audio stream list (index, codec, channels, sample
rate, duration) for Phase 3, and `start_time` of the video and audio
streams. Add `ProgressiveFrameSourceProfile` fields for these.

Acceptance:
- Functional tests over clips generated at test time with
  `ffmpeg -f lavfi` into the `{output}` root: H.264/MP4, H.264/MKV,
  FFV1/MKV, 8-bit 4:2:0, full range, BT.709-tagged, RGB-coded (e.g. PNG in
  MKV), with and without audio.
- Frame count for MP4 comes from `nb_frames`; no counting pass on files that
  declare it.

### Task 1.5 — Colour normalisation plan

Add a pure function `PlanSourceColourConversion(profile, standard)` that
yields the ffmpeg filter chain (or none) mapping the source to BT.601 10-bit
4:2:2 studio swing:
- `color_range` `pc`/`jpeg` → `scale=in_range=full:out_range=tv`; `tv` or
  unknown → treated as studio range.
- `color_space` `bt709` (or other known matrix) → `out_color_matrix=bt601`;
  unknown → assumed BT.601 (SD convention).
- RGB pixel formats → BT.601 studio-range YCbCr via the same `scale` filter.
- Any pixel format/bit depth → `format=yuv422p10le`.
- Primaries/transfer mismatches are not converted; reported at `debug`.
- Already-conformant sources → no filter (bit-exact pass-through).

Acceptance:
- Unit tests for every branch of the plan (pure function, no ffmpeg).
- Functional test: a full-range clip of 0/255 luma bars decodes to Y′ 64/940
  (±1 code); a BT.709-tagged colour bar clip decodes within ±2 codes of the
  BT.601 bar values.

### Task 1.6 — Validation rules

Replace the FFV1/`yuv422p10le`/metadata checks in `project_validator.cpp`
(and the duplicate checks in `ValidateMkvProfileWithFfprobe`, which is then
removed) with:
- Errors: unsupported extension/container, no video stream, raster ≠
  standard raster, frame rate ≠ standard rate, variable frame rate
  (`avg_frame_rate` ≠ `r_frame_rate` beyond tolerance), crop side-data.
- Warnings: interlaced field order opposite to the standard's dominance (PAL
  `tb`, NTSC/PAL-M `bt`); SAR outside the standard tolerance (picture is
  shown unscaled).
- Info: the colour conversion that will be applied ("full range → studio
  swing", "BT.709 matrix → BT.601").

Acceptance:
- Unit tests with a fake probe for every error/warning/info path.
- Existing bundled MKV and EXR fixtures produce no new diagnostics.

### Task 1.7 — GUI source picker and probe report

- Local-file browse filter becomes `*.mkv *.mp4 *.exr`.
- Built-in type combo label becomes "Video" (bundled MKV clips unchanged).
- Source Profile box shows codec, pixel format, frame count and the planned
  colour conversion, plus any warnings.

Acceptance:
- GUI unit tests for the profile-summary text built from a fake report.
- Manual check: an MP4 picked as a local file probes OK and previews.

### Task 1.8 — Documentation

Update in the same task: `docs/user-manual/sources.md` (profiles, colour
normalisation, `BLACK.exr` in the bundled table, rewritten "Converting your
own material"), `docs/reference/sections.md` (`source`),
`docs/user-manual/validation.md` and `docs/misc/troubleshooting.md` (new
messages), `docs/user-manual/gui.md`, HLD §4 *Decoded Source Cache and
Prefetch* and §8.1 *Supported Source Profiles*.

Acceptance: `mkdocs build --strict` passes; no page still describes FFV1 as
the only accepted codec.

---

## Phase 2 — Source frame ranges and automatic duration

### Task 2.1 — Model and YAML

- Add `Section::end_frame` (inclusive, 0-based) and `end_frame_specified`.
- `duration_frames` becomes optional; omitted means `"all"`.
- `"all"` now means *the selected range*: `[start_frame, end_frame]`, or
  `[start_frame, last]` when `end_frame` is absent. `duration_repeat`
  replays that range.
- Parser, emitter (`end_frame` written only when specified; `duration_frames`
  still written explicitly), `operator==`.

```yaml
- name: Excerpt
  type: progressive
  source: "{project}/capture.mp4"
  start_frame: 1500
  end_frame: 2999        # 1 500 frames; duration derived
```

Acceptance:
- Parser/emitter round-trip unit tests including omitted `duration_frames`.
- Existing project files load unchanged and emit identically.

### Task 2.2 — Range resolution helper and schedule

Add a pure `ResolveSourceFrameRange(section, source_frame_count)` returning
`{first, count}` (or an error), used by `BuildFramePatternSchedule`, the
validator and the GUI. Fix two existing defects in the schedule:
- `"all"` with `start_frame > 0` currently schedules `count` frames starting
  at `start_frame`, running past the end of the source.
- A fixed `duration_frames` longer than the available frames currently fails
  with "Requested frame index exceeds…", although the user manual says the
  clip loops. Make it loop within the selected range: source index =
  `first + (i % count)`.

Acceptance:
- Unit tests: range with/without `end_frame`, `"all"` × repeat, fixed
  duration shorter/longer than the range (loop), stills ignore the range.
- Functional test: `start_frame: 10, end_frame: 19` on a generated clip
  with burnt-in frame numbers emits exactly source frames 10–19.

### Task 2.3 — Validation

- `end_frame < start_frame` → error.
- `start_frame` or `end_frame` ≥ source frame count → error.
- `end_frame` with an integer `duration_frames` → error (the range already
  sets the duration).
- `start_frame`/`end_frame` on an EXR still → error.
- Omitted `duration_frames` on an EXR still → error (a still has no natural
  length).

Acceptance: unit tests with a fake probe for each rule; messages added to
`validation.md`/`troubleshooting.md`.

### Task 2.4 — GUI range editing and automatic duration

- Section editor gains *Start frame* and *End frame* spin boxes (end with a
  "to end of source" checkbox), enabled only for video sources and capped
  by the probed frame count.
- Choosing a new video source sets the duration to "all source frames"
  (selected range) automatically; loading an existing project never rewrites
  its duration.
- Duration summary and the section list frame range show
  `range length × repeat`; `UpdateFrameRangeDisplay` and
  `BuildSectionListRows` use `ResolveSourceFrameRange`.

Acceptance:
- Presenter/model unit tests: source change → duration "all"; range edits →
  derived total; EXR source disables range fields.
- Undo/redo of range edits through `ProjectDocument` commands.

### Task 2.5 — Documentation

Update `docs/reference/sections.md` (`start_frame`, `end_frame`,
`duration_frames` optional), `docs/user-manual/sources.md` ("Choosing frames
from the source"), `docs/user-manual/gui.md`, HLD §8.1 *Fields* table.

Acceptance: `mkdocs build --strict` passes.

---

## Phase 3 — File and video audio for channel pairs

### Task 3.1 — Model and YAML

A channel pair takes exactly one content kind: tones (`left`/`right`, as
now), an audio file, or the section video's soundtrack.

```yaml
audio:
  channel_pairs:
    - pair: 0
      description: Programme audio
      video_audio: { stream: 0 }        # audio stream index in the video; default 0
    - pair: 1
      description: Narration
      file: { path: "{project}/narration.flac", repeat: true }
```

- Add `AudioPairContent {kTones, kFile, kVideo}`, `AudioFileInput {path,
  repeat}`, `VideoAudioInput {stream}` to `AudioChannelPair`; parser,
  emitter, `operator==`.
- `ResolveProjectPaths` resolves `file.path` through the asset roots like
  `section.source`.

Acceptance: parser/emitter round-trip tests; mixing `file`/`video_audio`
with `left`/`right` is a parse error.

### Task 3.2 — Audio sample source interface

Add `IAudioSampleSource` (`Open(path, stream, start_seconds, sample_rate)`,
`Read(n, left, right) → frames read`, EOF signalled by a short read) and
`FfmpegAudioSampleSource` streaming
`ffmpeg -ss <t> -i <src> -map 0:a:<n> -ac 2 -ar <rate> -f s32le -`.
Mono input is duplicated to both channels by `-ac 2`; video soundtracks with
more than two channels are downmixed to stereo. Samples convert to 24-bit
via [audio_sample_conversion.h](../include/videosynth/audio_sample_conversion.h).

Acceptance:
- Functional tests on generated WAV (mono/stereo, 44.1/48 kHz) and FLAC:
  sample count, channel duplication, resampled tone frequency.
- Seek test: opening at `t` matches the tail of a full read within the
  resampler's latency.

### Task 3.3 — Audio track generator integration

- `AudioTrackGenerator::Begin` additionally receives each output frame's
  source frame index (from the schedule), and a factory for
  `IAudioSampleSource`.
- **File content**: position starts at 0 at each section-run start. On EOF,
  the remainder of the run is silence, or the file restarts when `repeat` is
  true.
- **Video content**: frame *k* takes the samples at the source time of its
  source frame index (`index / rate`, adjusted by the audio/video stream
  `start_time` offset). A discontinuity in source indices (loop,
  `duration_repeat`, range restart) reopens the reader at the new time, so
  audio follows the picture. A soundtrack shorter than the video pads with
  silence.
- Per-frame sample counts stay `AudioSamplesForFrame` (SMPTE 272M §14.3).
- EFM-selected pairs open a second reader at 44 100 Hz for the EFM path.

Acceptance:
- Unit tests with a fake sample source: short file without/with repeat,
  exact sample counts across PAL and the NTSC 5-frame sequence, video loop
  reopen, range start offset, EFM second reader.
- Functional test: an MP4 with a click at frame N, sectioned with
  `start_frame`, places the click at the output sample of the corresponding
  output frame (±1 ms).

### Task 3.4 — Validation

- `file`: path exists; extension `.wav` or `.flac`; probe succeeds; 1 or 2
  channels (more → error).
- `video_audio`: section source is a video (not EXR) with an audio stream at
  the given index.
- Warning when a non-repeating file is shorter than the section run (the
  tail will be silent); info when resampling or downmixing.
- Tone-only rules (waveform, ramp, amplitude) skipped for non-tone pairs.

Acceptance: unit tests with a fake probe for each rule; messages documented.

### Task 3.5 — GUI channel-pair editor

In `AudioChannelPairsEditor`, each pair gets a *Content* selector: *Test
tones* (existing left/right editors), *Audio file* (path + browse with
`*.wav *.flac`, relative-to-project checkbox, *Repeat* checkbox), *Video
soundtrack* (stream combo populated from the source probe; disabled when the
section source has no audio).

Acceptance: presenter unit tests for content switching and round-trip into
the `Section` model; undo/redo through `ProjectDocument`.

### Task 3.6 — Documentation

Update `docs/reference/section-audio.md` (new keys, mutual exclusion,
repeat/silence semantics), `docs/user-manual/audio.md`,
`docs/user-manual/gui.md`, `validation.md`/`troubleshooting.md`, and HLD §7
`audio:` sub-key.

Acceptance: `mkdocs build --strict` passes; the HLD describes file and
video audio as implemented.
