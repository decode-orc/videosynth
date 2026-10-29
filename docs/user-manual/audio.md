# Audio

videosynth writes frame-locked audio alongside the video. A project may carry up to **eight stereo channel pairs** (0–7, i.e. up to sixteen SMPTE 272M channels), each written as its own WAV track, and one of those pairs may additionally be encoded as laserdisc EFM digital audio.

Each pair carries synthetic test tones, an audio file, or the soundtrack of the section's video. Whatever it carries, every output frame gets exactly its standard's number of samples, so every track stays sample-accurately locked to the video regardless of what the sections are doing.

## Channel pairs

Audio is declared per section, one entry per channel pair:

```yaml
sections:
  - name: StereoAndCommentary
    type: progressive
    source: "{bundled}/exr/720x576/75_BARS.exr"
    duration_frames: 250
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
```

The rules:

- **One WAV file per pair.** The set of files written is the union of the pair numbers declared anywhere in the project. Every pair file spans the whole output.
- **A section that omits a pair emits silence for its frames**, still frame-locked. There are no gaps in a track.
- **One kind of content per pair.** Tones (`left`/`right`), [an audio file](#audio-files) (`file`) or [the video's soundtrack](#video-soundtracks) (`video_audio`); combining them is an error.
- **Left and right tones are independent.** Either may be omitted and stored as silence; at least one must be present.
- **Each section run starts afresh.** Each contiguous run of frames sharing one section restarts tone oscillators at phase 0 and plays an audio file from its start.

## Frame lock

Every track is stereo, 24-bit signed little-endian PCM (RIFF/WAVE) at 48 000 Hz, synchronous with the video per SMPTE 272M §1.2. The per-frame sample count follows the standard:

| Standard | Samples per frame |
|----------|-------------------|
| PAL | 1920 (constant) |
| NTSC / PAL-M | 1602/1601 in the SMPTE 272M §14.3 five-frame sequence — 1602, 1601, 1602, 1601, 1602 → 8008 per 5 frames |

## Tones

Each channel carries a tone descriptor:

| Key | Values | Default |
|-----|--------|---------|
| `waveform` | `sine`, `square`, `sawtooth`, `triangle` | `sine` |
| `frequency` | 0–22 000 Hz | 1000.0 |
| `amplitude` | 0.0–1.0 of full scale | 0.5 |
| `ramp` | A frequency sweep block (below) | — |

Waveforms are naive rather than band-limited, which is appropriate for test signals — a square wave should look like a square wave, aliasing and all.

The 22 kHz ceiling keeps every tone safely under the 24 kHz Nyquist limit of the 48 kHz sample rate.

## Frequency ramps

A `ramp:` block replaces the fixed `frequency` with a sweep:

```yaml
        - pair: 0
          left:
            waveform: sawtooth
            amplitude: 0.5
            ramp: { start: 200.0, end: 4000.0, mode: up }
```

| Key | Values | Meaning |
|-----|--------|---------|
| `start` | 0–22 000 Hz | Sweep start |
| `end` | 0–22 000 Hz | Sweep end |
| `mode` | `up`, `down`, `bounce` | `up` = start→end, `down` = end→start, `bounce` = start→end→start |
| `period` | seconds | `0` (default) sweeps once over the whole section; `> 0` makes one sweep last this long and repeat |

Instantaneous frequency drives a phase accumulator, so swept tones stay phase-continuous within a section run.

A `ramp:` block and a fixed `frequency` are mutually exclusive.

## Audio files

A pair can play a WAV or FLAC file instead of tones:

```yaml
        - pair: 1
          description: Narration
          file: { path: "{project}/narration.flac", repeat: true }
```

The path resolves through the [asset roots](../reference/asset-roots.md) like a section `source`. The file plays from its start at the beginning of each section run. If it is shorter than the run, the rest of the run is silent; validation warns about this. With `repeat: true` the file starts again each time it ends.

Mono and stereo files are accepted; a mono file plays on both channels. Files at a sample rate other than 48 kHz are resampled, which validation notes in the log.

## Video soundtracks

A section whose source is a video can pass its soundtrack through:

```yaml
  - name: Programme
    type: progressive
    source: "{project}/capture.mp4"
    start_frame: 1500
    end_frame: 2999
    audio:
      channel_pairs:
        - pair: 0
          description: Programme audio
          video_audio: { stream: 0 }     # first audio stream; {} means the same
```

The sound follows the picture: each output frame carries the soundtrack from the moment its source frame was shown. Starting at `start_frame` starts the sound there too. When a fixed duration loops the range, or `duration_repeat` replays it, the sound jumps back with the picture. Where the soundtrack starts after the first picture or ends before the last, those frames are silent.

`stream` counts the video's audio streams from 0; the section editor lists them once the source has been probed. Mono soundtracks play on both channels, and soundtracks with more than two channels (5.1, for example) are downmixed to stereo. Both, and any resampling, are noted in the log.

## Output files

Each declared pair is written beside the video as:

```text
<basename>_audio_<pair>.wav
```

where `<basename>` is `output.video_path` with its trailing `.cvbs` or `.cvbsy` removed. When at least one pair is declared, one `audio_channel_pair` row per pair — with its `description` — is written to the `.meta` sidecar.

## Laserdisc EFM digital audio

A laserdisc's digital audio channel can be generated from one of the pairs:

```yaml
output:
  video_path: "{output}/pal_disc.cvbs"
  efm_audio:
    pair: 0
```

The presence of the `efm_audio:` block is what enables it. That pair is produced a second time at exactly 44 100 Hz — the laserdisc digital audio rate (IEC 60856/60857 Amd 2 clause 13.2) — and encoded as an IEC 60908 EFM channel stream. PAL-M, which no LaserVision standard names, follows the 525-line IEC 60857 definition, as its biphase codes do: the NTSC sample sequence and a 525-line disc identification in the table of contents. Tones are synthesised directly at 44 100 Hz with no resampling; audio files and video soundtracks are read again and resampled to that rate. Its 48 kHz WAV track and every other pair are untouched; the pair's content is declared only once, in the per-section `audio:` blocks.

### What gets written

```text
<basename>.efm        the channel stream, one byte per pit/land run length (T3-T11)
<basename>.efm.meta   a SQLite frame index: one row per video frame
```

Both share the CVBS basename, so the channel pair number is not part of either name. An aborted run leaves neither file behind.

Each stream byte carries its run length in bits 3-0 and the producer's *doubt* about that value in bits 7-4 — 0 for fully trusted, 15 for an erasure candidate. videosynth synthesises the stream, so every run length is exact and written at zero doubt (maximum confidence); the bytes are therefore the plain run lengths. Read a t-value back as `byte & 0x0F`.

### Disc structure drives track layout

EFM track layout is derived from `section_type`, not from the biphase codes:

| Section type | EFM content |
|--------------|-------------|
| `lead_in` | The repetitive mode-4 table of contents — one POINT per track plus A0/A1/A2 |
| `programme_area` | One track per contiguous run of frames sharing that section, numbered 01, 02, … in output order |
| `lead_out` | Lead-out subcode (TNO = AA) |

Because it depends only on `section_type`, a project can produce a proper EFM disc **without carrying any biphase codes at all** — which keeps the lead-in short. A CAV biphase lead-in would have to run for at least 938 frames; an EFM-only lead-in can be a couple of seconds.

### Constraints

| Rule | Severity |
|------|----------|
| `pair` must be 0–7 | Error |
| Standard must be PAL, NTSC or PAL-M — SECAM has no laserdisc digital audio specification | Error |
| At most 79 `programme_area` sections (one track each) | Error |
| No section declares the selected pair → no `.efm` is written | Warning |
| A `programme_area` section shorter than 4 s (6 s for the first, whose leading 2 s are the mandatory pause) | Warning |
| No `lead_in` section → no table of contents is emitted | Warning |

### Timing

Time zero is common to video frame 0, WAV sample 0, EFM source sample 0 and Q absolute time 00:00:00. The only structural delay is the CIRC interleave — 108 F1 frames, about 14.69 ms — and the stream is deliberately *not* pre-compensated for it, so the shared datum with the video and WAV timelines holds. A complementary decoder emits that much warm-up silence and is then sample-exact against the source.

## A complete EFM example

```yaml
project:
  name: PalEfmAudio
  version: "1.0"

cvbs_presets:
  video_standard_preset: PAL
  sample_encoding_preset: CVBS_U10_4FSC
  signal_state_preset: STANDARD_STABLE_LOCKED

output:
  video_path: "{output}/pal_efm_audio.cvbs"
  efm_audio:
    pair: 0

sections:
  - name: LeadIn
    type: progressive
    source: "{bundled}/exr/720x576/PLUGE.exr"
    duration_frames: 50            # 2 s of TOC
    section_type: lead_in

  - name: Track1
    type: progressive
    source: "{bundled}/exr/720x576/75_BARS.exr"
    duration_frames: 175           # 7 s: 2 s mandatory pause + 5 s programme
    section_type: programme_area
    audio:
      channel_pairs:
        - pair: 0
          description: LaserDisc digital audio (EFM)
          left:  { waveform: sine, frequency: 1000.0, amplitude: 0.5 }
          right: { waveform: sine, frequency: 1000.0, amplitude: 0.5 }

  - name: Track2
    type: progressive
    source: "{bundled}/exr/720x576/Y_CB_CR_RAMPS.exr"
    duration_frames: 125           # 5 s
    section_type: programme_area
    audio:
      channel_pairs:
        - pair: 0
          left:  { waveform: sine, frequency: 440.0, amplitude: 0.5 }
          right: { waveform: sine, frequency: 880.0, amplitude: 0.5 }

  - name: LeadOut
    type: progressive
    source: "{bundled}/exr/720x576/PLUGE.exr"
    duration_frames: 75
    section_type: lead_out
```
