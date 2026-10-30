# Picture sources

Every section draws its picture content from one **progressive source** file. videosynth converts it to an interlaced signal on the way through — the source is progressive, the output is interlaced, and the two-field weave is the generator's job, not yours.

Source ingestion is strict about geometry and timing and forgiving about colour coding. There is no scaling, no cropping and no frame-rate conversion: a source must already have the standard's raster and frame rate, or it is rejected at validation. Colour coding — range, matrix, chroma format, bit depth — is normalised to BT.601 studio swing on the way in, and validation tells you which conversions it will apply.

## Supported profiles

### EXR still image

A single-frame OpenEXR image, used when a section should hold one picture for its whole duration.

| Requirement | Value |
|-------------|-------|
| Container | Scanline OpenEXR, single frame |
| Channels | `R`, `G`, `B` |
| Type | `FLOAT` (32-bit) |
| Compression | None |
| Line order | Increasing Y |
| Gamma metadata | `1` |
| Windows | Data and display windows must cover the full raster |
| Raster | 720×576 (PAL/SECAM) or 720×486 (NTSC/PAL-M) |
| Frame rate metadata | `25/1` (PAL/SECAM) or `30000/1001` (NTSC/PAL-M) |
| Pixel aspect metadata | Must match the standard |

RGB `0.0` maps to studio black (Y′ 64, Cb/Cr 512) and `1.0` to studio white (Y′ 940).

### Video clip (MKV or MP4)

A Matroska (`.mkv`) or MP4 (`.mp4`) file, used when a section should show moving content. Any video codec FFmpeg can decode is accepted — FFV1, H.264, HEVC, ProRes, MPEG-2 and so on.

| Requirement | Value |
|-------------|-------|
| Container | Matroska for `.mkv`, MP4 for `.mp4` (the extension must match the content) |
| Video stream | The first video stream is used |
| Raster | 720×576 (PAL/SECAM) or 720×486 (NTSC/PAL-M) |
| Frame rate | Exactly 25 (PAL/SECAM) or 30000/1001 (NTSC/PAL-M), constant; 24000/1001 film in a 2:3 pulldown section (see [Film and pulldown](#film-and-pulldown)) |
| Crop metadata | Must not be present |

Everything else is either converted or reported:

| Property | Handling |
|----------|----------|
| Colour range | Full range (`pc`/`jpeg`, or a `yuvj*` pixel format) is scaled to studio swing. `tv` or untagged is taken as studio swing already. |
| Colour matrix | BT.709, FCC, SMPTE 240M and BT.2020 are converted to BT.601. `smpte170m`, `bt470bg` or untagged is taken as BT.601. |
| RGB-coded video | Converted to BT.601 studio-swing YCbCr. |
| Pixel format and bit depth | Any format (4:2:0, 4:4:4, 8–16 bit) is converted to 10-bit 4:2:2. |
| Colour primaries, transfer | Used as they are — not converted. |
| Field order | A tag opposite to the output's dominance (PAL/SECAM top field first, NTSC/PAL-M bottom field first) is a **warning**: interlaced motion may play out of order. |
| Sample aspect ratio | One that is not the standard's BT.601 ratio is a **warning**: the picture is used unscaled. |

A source already coded as `yuv422p10le`, studio range, BT.601 matrix — like the bundled clips — passes through unconverted and bit-exact. Conversions happen inside FFmpeg (`scale` filter, bit-exact rounding); a full-range source is range-converted at its own bit depth before widening, so 8-bit 0 and 255 land exactly on 64 and 940.

Video is decoded as a stream, never held in memory whole, so clips of any length can be used. See [Memory use](#memory-use).

### BT.601 content compliance

The bundled assets are additionally BT.601-consistent in their *content*, not merely their metadata: the sampling model, active-window placement and horizontal padding follow the standard. Your own material does not have to, but it will look as intended only if it does.

| Standard | Raster | Sample aspect ratio | Horizontal padding |
|----------|--------|---------------------|--------------------|
| PAL / SECAM | 720×576 | 128:117 | 8–704–8 |
| NTSC / PAL-M | 720×486 | 108:119 | 8–704–8 |

Source data is converted to 10-bit 4:4:4 YCbCr BT.601 studio swing at ingestion, and studio-domain sub-black and over-white excursions are preserved where the source carries them.

The full requirements for the bundled assets are documented in the asset repository, in `videosynth-assets/docs/mkv-bt601-compliance-requirements.md` and `exr-bt601-compliance-requirements.md`.

## Bundled assets

videosynth ships a set of test patterns in both rasters, addressed through the `{bundled}` [asset root](../reference/asset-roots.md):

```yaml
source: "{bundled}/exr/720x576/100_BARS.exr"
```

The layout is `{bundled}/<type>/<raster>/<file>`, where `<type>` is `exr` or `mkv` and `<raster>` is `720x576` or `720x486`.

### Stills (`exr`)

| Asset | Content |
|-------|---------|
| `BLACK.exr` | Full-frame black at studio black level (Y′ 64, Cb/Cr 512) — blanking level in PAL, setup level in NTSC |
| `100_BARS.exr` | 100 % colour bars |
| `75_BARS.exr` | 75 % colour bars |
| `75_BARS_RED.exr` | 75 % bars with 100 % red |
| `SMPTE_BARS.exr` / `SMPTE_BARS_001.exr` | SMPTE bars (PAL / System-M raster) |
| `PLUGE.exr` | PLUGE black-level alignment pattern |
| `MULTIBURST.exr` | Frequency response multiburst |
| `LUMA_RAMP.exr`, `LUMA_RAMP_DOWN.exr`, `VERT_LUMA_RAMP.exr` | Luma ramps |
| `CHROMA_RAMP.exr` | Chroma ramp |
| `FULL_RAMP.exr`, `LEGAL_RAMP.exr`, `VALID_RAMPS.exr` | Full-range, legal-range and valid-range ramps |
| `Y_CB_CR_RAMPS.exr` | Separate Y, Cb and Cr ramps |
| `GREY_5H_STEP.exr`, `GREY_10H_STEP.exr` | Horizontal grey staircases |
| `GREY_5V_STEP.exr`, `GREY_10V_STEP.exr` | Vertical grey staircases |
| `TARTAN.exr` | Tartan/crosshatch pattern |
| `TESTCARD-F.exr` | UK Test Card F (PAL raster only) |

### Clips (`mkv`)

| Asset | Content |
|-------|---------|
| `SMPTE_BARS.mkv` / `SMPTE_BARS_001.mkv` | Static SMPTE bars as a clip |
| `MOVING_ZONE_2H.mkv` | Moving zone plate — motion and high-frequency content (luma only) |
| `MOVING_BARS_75.mkv` | 75 % colour bars (EBU 100/0/75/0) scrolling left 4 pixels per frame — moving chroma, for 3D comb filters and field-phase decisions. 176 frames, looping seamlessly |
| `FILM_COUNTER_24P.mkv` | 720×486 only: 24000/1001 progressive film for [2:3 pulldown](#film-and-pulldown). Each frame shows its 0-based frame number in large digits, over a white bar and a strip of colour bars that both move 8 pixels per frame, so a frame that weaves two film frames shows combing. 240 frames (10 s) |

Always reference bundled assets through `{bundled}/…` rather than a bare relative path. A bare relative path resolves against the working directory in the CLI but against the project directory in the GUI, so it will work in one and not the other.

## Choosing frames from the source

Four section fields control which frames a section takes and how many it emits:

| Field | Meaning |
|-------|---------|
| `start_frame` | First source frame of the selected range, 0-based (default `0`). Video only |
| `end_frame` | Last source frame of the selected range, inclusive and 0-based (default: the source's last frame). Video only |
| `duration_frames` | How many output frames the section produces — a positive integer, or `"all"` (the default when omitted) |
| `duration_repeat` | With `duration_frames: "all"`, how many times to play the selected range (default `1`) |

`start_frame` and `end_frame` select a run of frames from a clip; the duration then counts or replays that run. Both must lie inside the clip.

**`"all"`**, or leaving `duration_frames` out, plays the selected range once: `start_frame` to `end_frame`, or to the last frame of the clip when there is no `end_frame`. Its length comes from probing the file. With `duration_repeat`, the range plays that many times, each pass restarting at `start_frame`.

```yaml
- name: Excerpt
  type: progressive
  source: "{project}/capture.mp4"
  start_frame: 1500
  end_frame: 2999               # 1 500 frames; duration derived
```

```yaml
- name: WholeClipTwice
  type: progressive
  source: "{bundled}/mkv/720x576/MOVING_ZONE_2H.mkv"
  duration_frames: "all"
  duration_repeat: 2            # total = source length x 2
```

**A fixed count** produces exactly that many output frames. For a still, the same picture is emitted for all of them. For a clip, the frames are taken from `start_frame` onwards, and a count longer than the rest of the clip loops it: after the last frame, playback continues from `start_frame` again.

```yaml
- name: TenSecondsOfBars
  type: progressive
  source: "{bundled}/exr/720x576/100_BARS.exr"
  duration_frames: 250          # 10 s at 25 fps
```

A few combinations are validation errors:

- `end_frame` with an integer `duration_frames`: the range already sets the duration, so omit `duration_frames` or use `"all"`.
- `start_frame` or `end_frame` on an EXR still: a still has only one frame.
- `"all"`, or an omitted `duration_frames`, on an EXR still: a still has no length of its own, so give it a frame count.

`duration_repeat` is ignored with a fixed `duration_frames`, and validation warns if you set it there.

## Source probing

Validation probes each source file: it opens it, checks it against the profile rules and reads its frame count. A source that does not exist, or does not match a profile, is a validation error naming the reason. For a video, the probe also reads the average frame rate (to detect variable-rate files), the start times and the audio streams (which a [video soundtrack](audio.md#video-soundtracks) channel pair plays). The frame count comes from the container (`nb_frames` in MP4, the duration in Matroska), so probing never decodes the clip.

The colour conversions a video will receive are logged at `info` level during validation, for example:

```text
Progressive section 'Capture': colour conversion applied: full range -> studio swing, BT.709 matrix -> BT.601, yuv420p -> yuv422p10le.
```

In the GUI the probe runs in the background whenever you change a section's source, and the **Source Profile** box in the section editor reports the result — *Source profile OK*, *Source profile incompatible*, or *Probing source…* while it works — with the codec, pixel format, frame count, the planned colour conversion and any warnings. The frame count it discovers caps the **Source frames** range fields and feeds the `duration_frames: "all"` totals shown beside the duration field.

## Film and pulldown

525-line television shows 24 fps film with **2:3 pulldown**: film frames alternately fill two and three 59.94 Hz fields, so four film frames make ten fields, or five video frames. A section does this when its [`cadence`](../reference/sections.md#cadence) block sets `pattern: "2:3"` and its source is a video at exactly 24000/1001 fps. It is available on NTSC and PAL-M only. 625-line film runs at 25 fps with one frame per two fields, which is the normal video cadence, so a PAL or SECAM project needs nothing extra.

```yaml
- name: Feature
  type: progressive
  source: "{bundled}/mkv/720x486/FILM_COUNTER_24P.mkv"
  cadence:
    pattern: "2:3"
    phase: A
```

### The cadence

The cycle runs over output fields in the order they are shown: `A A B B B C C D D D`. Each letter is one film frame, and a new film frame starts wherever the letter changes. `phase` picks the letter the section's first field takes. Each field is drawn from its own film frame, so the frames whose two fields carry different letters (`B3` with `C1`, and `C2` with `D1`, in a cycle from phase A) weave two film frames together, exactly as a real transfer does. Those are the frames a film-mode decoder has to detect and undo.

`duration_frames` still counts output frames:

- **`"all"`** shows every film frame of the selected range in full: 48 film frames from phase A fill 120 fields, or 60 frames. When the fields come to an odd number, the last frame's second field repeats the last film frame.
- **A fixed count** plays that many output frames and loops the range if it runs out, cutting the last film frame short when the count ends part-way through it.

The section editor's duration summary works these numbers out for you.

### Mixing film and video

Film and video sections can follow each other freely, and the same source can appear in several sections with different `start_frame`/`end_frame` ranges. At each film section's start, `continuity` decides what happens to the cycle:

- **`restart`** (the default) starts at `phase`, so a boundary breaks the cadence as an edit between two separately transferred reels would.
- **`continue`** carries on from where the previous section's fields stopped. The previous section must also be 2:3. When it stopped part-way through a letter, the new section's first film frame gets only the rest of that letter, which leaves the one- or two-field film frames a real edit leaves behind.

A fixed duration that ends part-way through a letter leaves a short film frame at the end of the section in the same way. The fixture `projects/general/ntsc_film_mixed.yaml` mixes all of these, and its header lists the film frames and picture numbers each section produces.

The OSD tokens [`{film_frame}` and `{cadence}`](osd.md#tokens) print each field's film frame and cycle position, such as `37 B3`, which is the quickest way to see the cadence in a capture.

### Sound

A film section's [video soundtrack](audio.md#video-soundtracks) plays in real time. Four film frames last exactly as long as five video frames, so the sound needs no resampling and runs on unbroken across the cadence.

### On a laserdisc

A CAV disc numbers pictures, not frames, so in a film section `picture_number` counts film frames: four pictures in every five frames. The picture number goes on the first field of each film frame, whichever field that is, and so does the white flag on NTSC and PAL-M, so one frame in five carries neither. This is how film is mastered to CAV, and it lets a player hold a still on a whole film frame. Picture numbers continue across film and video sections alike. The colour field sequence keeps following the video frames. On CLV the time codes still count video frames. [Laserdisc: Film sections](laserdisc.md#film-sections) has the placement details.

### Converting 24p material

The source must already be at 24000/1001 fps, at the 525-line raster, and progressive. Material at exactly 24 fps needs its timestamps slowed by 0.1 %; this keeps every frame and needs no frame-rate conversion:

```bash
ffmpeg -i film_24p.mov -vf "setpts=PTS*1001/1000,scale=704:486,pad=720:486:8:0,setsar=108/119" \
  -r 24000/1001 -af "atempo=1000/1001" -c:v ffv1 -c:a flac film_23976.mkv
```

Material already at 23.976 fps needs only the scale and pad. Do not use the `fps` filter to make 24000/1001 from 29.97 fps video: that drops frames rather than recovering the film frames. Remove pulldown from telecined video first, with FFmpeg's `fieldmatch,decimate` filters for example.

## Memory use

Video is decoded through one FFmpeg process per decode position, reading forward frame by frame into a bounded window of recently decoded frames. The window holds `max(32, 2 × CPU threads + 16)` frames per source (about 2.4 MiB each for a 625-line raster) and up to three sources are cached at once — the current section, the previous one and the next one, which is decoded ahead in the background. Peak memory therefore depends on the thread count, not on the clip length.

## Converting your own material

A clip needs only the right raster and frame rate; FFmpeg can produce both. For example, to fit a 4:3 capture to the PAL raster with BT.601 padding:

```bash
ffmpeg -i capture.mov -vf "scale=704:576,pad=720:576:8:0,setsar=128/117,fps=25" \
  -c:v ffv1 -c:a flac capture_pal.mkv
```

and for NTSC/PAL-M:

```bash
ffmpeg -i capture.mov -vf "scale=704:486,pad=720:486:8:0,setsar=108/119,fps=30000/1001" \
  -c:v ffv1 -c:a flac capture_ntsc.mkv
```

`fps` drops or repeats frames to reach the target rate; for interlaced material, deinterlace first. Any codec works; a lossless one (FFV1) avoids a second generation of compression artefacts.

For stills, the repository ships `scripts/convert_progressive_raw_to_exr.py` for turning raw `yuv422p10le` stills into OpenEXR, with `scripts/run_stage1_raw_to_exr_conversion.sh` to run it in an ad-hoc Nix shell. Its output uses ZIP compression, which the EXR profile rejects, so re-encode the result without compression before use. `videosynth-assets/scripts/generate_black_exr.sh` shows the full recipe (FFmpeg `-c:v exr -compression none -format float -gamma 1`, then `exrstdattr` for the frame-rate and pixel-aspect metadata).
