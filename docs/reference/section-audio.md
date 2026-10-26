# `sections[].audio`

Audio channel pairs for this section. Optional — a section without an `audio:` block emits silence for its frames across every pair, still frame-locked.

Each pair carries one kind of content: synthetic test tones (`left`/`right`), an audio file (`file`), or the soundtrack of the section's video (`video_audio`).

```yaml
    audio:
      channel_pairs:
        - pair: 0
          description: Analogue stereo
          left:  { waveform: sine, frequency: 1000.0, amplitude: 0.5 }
          right: { waveform: sine, frequency: 1000.0, amplitude: 0.5 }
        - pair: 1
          description: Programme audio
          video_audio: { stream: 0 }
        - pair: 2
          description: Narration
          file: { path: "{project}/narration.flac", repeat: true }
```

## Keys

| Key | Type | Required | Description |
|-----|------|----------|-------------|
| `channel_pairs` | list | Yes | One entry per stereo channel pair |

No other key is accepted.

## `channel_pairs[]`

| Key | Type | Required | Range | Description |
|-----|------|----------|-------|-------------|
| `pair` | int | Yes | 0–7 | Channel-pair number; names the `_audio_<pair>.wav` file and the `audio_channel_pair` metadata row. Unique within a section |
| `description` | string | No | — | Human-readable label recorded in the metadata, e.g. `Analogue stereo` |
| `left` | map | No\* | [Tone descriptor](#tone-descriptor) | The odd (first) interleaved channel. Omitted → silent |
| `right` | map | No\* | [Tone descriptor](#tone-descriptor) | The even (second) interleaved channel. Omitted → silent |
| `file` | map | No\* | [Audio file](#file) | Plays a WAV or FLAC file |
| `video_audio` | map | No\* | [Video soundtrack](#video_audio) | Plays an audio stream of the section's video |

\* A pair names exactly one kind of content: tones (`left` and/or `right`), `file`, or `video_audio`. Combining kinds is a parse error, and a tone pair with no active channel is rejected.

A project may carry up to 8 pairs (0–7), i.e. up to 16 SMPTE 272M channels. The set of files written is the union of pair numbers declared across all sections, and every pair file spans the whole output.

## Tone descriptor

`left:` and `right:` each take:

| Key | Type | Required | Range / Values | Default |
|-----|------|----------|----------------|---------|
| `waveform` | string | No | `sine`, `square`, `sawtooth`, `triangle` | `sine` |
| `frequency` | float | No | 0–22 000 Hz | `1000.0` |
| `amplitude` | float | No | 0.0–1.0 of full scale | `0.5` |
| `ramp` | map | No | Frequency sweep (below) | — |

Waveforms are naive rather than band-limited, which is what a test signal wants.

`ramp:` and a fixed `frequency` are mutually exclusive.

## `ramp`

| Key | Type | Required | Range / Values | Default |
|-----|------|----------|----------------|---------|
| `start` | float | Yes | 0–22 000 Hz | — |
| `end` | float | Yes | 0–22 000 Hz | — |
| `mode` | string | No | `up`, `down`, `bounce` | `up` |
| `period` | float | No | 0 – section duration in seconds | `0` |

| `mode` | Sweep |
|--------|-------|
| `up` | start → end |
| `down` | end → start |
| `bounce` | start → end → start |

`period: 0` sweeps once over the whole section run. A positive `period` makes one sweep last that many seconds and repeat until the section ends.

Instantaneous frequency drives a phase accumulator, so swept tones stay phase-continuous within a section run.

## `file`

| Key | Type | Required | Default | Description |
|-----|------|----------|---------|-------------|
| `path` | string | Yes | — | A `.wav` or `.flac` file. Resolved through the [asset roots](asset-roots.md) like `source`, e.g. `"{project}/narration.flac"` |
| `repeat` | bool | No | `false` | Restart the file when it ends |

The file plays from its first sample at the start of each section run (each contiguous run of output frames from one section). When it ends, the rest of the run is silent, or with `repeat: true` the file starts again, as often as needed.

Mono files play on both channels. Files with more than two channels are rejected. Any sample rate is accepted and resampled to 48 kHz; 16- and 24-bit files convert exactly, and wider samples are rounded to 24 bits.

## `video_audio`

| Key | Type | Required | Default | Description |
|-----|------|----------|---------|-------------|
| `stream` | int | No | `0` | Which audio stream of the video, counted from 0 among its audio streams |

`video_audio: {}` takes the first audio stream. The section `source` must be a video (MKV or MP4), not an EXR still.

The sound follows the picture. Each output frame takes the soundtrack samples at the time of the source frame it shows (source frame index ÷ frame rate, measured from the video's first frame). So a section with `start_frame` starts the sound at that frame, a fixed duration that [loops the range](../user-manual/sources.md#choosing-frames-from-the-source) loops the sound with it, and each `duration_repeat` pass replays it. Where the soundtrack starts after the first picture, or ends before the last, those frames are silent.

Mono soundtracks play on both channels and soundtracks with more than two channels are downmixed to stereo. A sample rate other than 48 kHz is resampled.

## Frame lock

Every track is stereo, 24-bit signed little-endian PCM (RIFF/WAVE, format tag `0x0001`) at 48 000 Hz, synchronous with the video per SMPTE 272M §1.2.

| Standard | Samples per frame |
|----------|-------------------|
| PAL | 1920 (constant) |
| NTSC / PAL-M | 1602/1601 in the five-frame sequence 1602, 1601, 1602, 1601, 1602 → 8008 per 5 frames (SMPTE 272M §14.3) |

Sample counts follow the **output** frame stream whatever a pair carries, so alignment holds regardless of what the sections do. Each contiguous run of frames sharing one section starts afresh: tone oscillators restart at phase 0, and an audio file plays from its start. A video soundtrack is placed by source frame instead (see [`video_audio`](#video_audio)).

When the pair is also encoded as [EFM digital audio](output.md#efm_audio), file and video audio are read a second time at 44.1 kHz for that stream.

## Output

```text
<basename>_audio_<pair>.wav
```

where `<basename>` is `output.video_path` with its trailing `.cvbs` or `.cvbsy` removed.

When at least one pair is declared, one `audio_channel_pair` row per pair — with its `description` — is written to the `.meta` sidecar.

To additionally encode one pair as laserdisc EFM digital audio, see [`output.efm_audio`](output.md#efm_audio).

## Example

```yaml
sections:
  - name: StereoAndCommentary
    type: progressive
    source: "{bundled}/exr/720x576/75_BARS.exr"
    duration_frames: 200
    audio:
      channel_pairs:
        - pair: 0
          description: Analogue stereo
          left:  { waveform: sine, frequency: 1000.0, amplitude: 0.5 }
          right: { waveform: sine, frequency: 1000.0, amplitude: 0.5 }
        - pair: 1
          description: Commentary (left only)
          left:  { waveform: square, frequency: 440.0 }

  - name: Programme
    type: progressive
    source: "{project}/capture.mp4"
    start_frame: 250                 # sound starts with the picture at frame 250
    audio:
      channel_pairs:
        - pair: 0
          description: Programme audio
          video_audio: {}
        - pair: 1
          description: Narration
          file: { path: "{project}/narration.wav" }   # silent once it ends

  - name: SilenceGap                # no audio: block — silent, still frame-locked
    type: progressive
    source: "{bundled}/exr/720x576/75_BARS.exr"
    duration_frames: 50

  - name: SweepUp
    type: progressive
    source: "{bundled}/exr/720x576/75_BARS.exr"
    duration_frames: 200
    audio:
      channel_pairs:
        - pair: 0
          left:
            waveform: sawtooth
            amplitude: 0.5
            ramp: { start: 200.0, end: 4000.0, mode: up }
```
