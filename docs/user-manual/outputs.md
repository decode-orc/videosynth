# Output files

Every artefact a run produces is colocated with `output.video_path`. The project names them; the caller decides where they land, via `{output}` and `--output-root`.

## The artefacts

| File | Written when | Contents |
|------|--------------|----------|
| `<name>.cvbs` | `signal_type: composite` | Composite samples |
| `<name>.cvbsy` | `signal_type: yc` | Luma samples |
| `<name>.cvbsc` | `signal_type: yc` | Chroma samples |
| `<name>.meta` | Always | SQLite metadata sidecar |
| `<name>.dropouts.meta` | Any section has dropouts | SQLite dropout sidecar |
| `<name>_audio_<pair>.wav` | Any section declares that pair | 48 kHz/24-bit stereo audio |
| `<name>.efm` | `output.efm_audio` is present | Laserdisc EFM channel stream |
| `<name>.efm.meta` | With the above | SQLite frame index for the EFM stream |

`<name>` for the audio and EFM files is `output.video_path` with its trailing `.cvbs` or `.cvbsy` removed. The `.meta` path is derived from the video path by replacing that suffix — it is never specified in the project.

Parent directories are created automatically, so an output path can point anywhere writable.

## Sample files

The `.cvbs` (or `.cvbsy`/`.cvbsc`) file is a flat, headerless stream of samples in the project's [sample encoding](video-signal.md#sample-encodings), little-endian, with no per-frame framing of any kind. Frame boundaries are implicit in the sample count:

| Standard | Samples per frame at 4fsc |
|----------|---------------------------|
| PAL / SECAM | 709,379 (1135 per line, plus two extra samples on each of lines 313 and 625) |
| NTSC | 477,750 (910 × 525, orthogonal) |
| PAL-M | 477,225 (909 × 525) |

At 16 bits per sample that is roughly 1.4 MB per PAL frame and 0.95 MB per NTSC frame — so about 2.1 GB per minute of PAL. Plan storage accordingly, and use `--output-root` to send bulk runs somewhere with room.

## The metadata sidecar

`<name>.meta` is a SQLite database following the CVBS File Format Specification. Its `cvbs_file` table describes the file as a whole:

| Column | Contents |
|--------|----------|
| `preset` | `PAL`, `NTSC`, `PAL_M` or `SECAM` |
| `sample_encoding_preset` | The encoding the samples are in |
| `signal_state_preset` | `STANDARD_STABLE_LOCKED`; `STANDARD_STABLE_UNLOCKED` for `SECAM` |
| `sequence_continuous` | Always `TRUE` — the synthesised signal is one unbroken sequence |
| `signal_type` | `composite` or `yc` |
| `decoder` | The producing tool |
| `git_branch`, `git_commit` | The build the file came from |
| `number_of_sequential_frames` | Frame count |
| `black_level` | The black level in use — relevant for the NTSC setup option |
| `has_nonstandard_values` | Whether the file deliberately departs from the standard |
| `capture_notes` | Free text |

When any section declares audio, an `audio_channel_pair` table carries one row per emitted WAV track, with the `description` from the project.

Recording the git commit in the sidecar is deliberate: a generated file always says which build produced it, so a decoding result can be traced back to a specific state of videosynth.

## The dropout sidecar

`<name>.dropouts.meta` (schema version 5) carries one `dropout_run` row per contiguous dropout, listing the frame, first sample, length and a severity of `25` (non-visible VBI) or `75` (intersecting the active picture). It is ground truth against which a dropout detector can be scored directly. Dropouts with `flagged: false` are applied to the signal but have no rows, so they are absent from the sidecar; the sidecar itself is still written whenever any section has dropouts. See [Impairments](impairments.md#pressing-defects-unflagged-and-disc-anchored-dropouts).

## Audio and EFM

Audio tracks are stereo, 24-bit signed little-endian PCM (RIFF/WAVE) at 48 000 Hz, one file per declared channel pair, each spanning the whole output. EFM output is the two-file pair defined by the EFM extension format — the channel stream and its own frame-index sidecar. See [Audio](audio.md).

## Determinism

The same project produces byte-identical files on every run, on any machine, at any thread count, with or without the frame template cache. Where randomness is involved — noise and dropouts — a fixed seed makes it reproducible too.

This is a testable property, not just an intention: the repository ships `scripts/output-hashes.sh`, which records SHA-256 manifests of every artefact (using SQLite's canonical `.dump` for the databases) and fails if a later build differs from a recorded baseline.

```bash
# On a known-good build
scripts/output-hashes.sh --record

# After a change: lists any artefact that differs
scripts/output-hashes.sh
```

## Cancellation

A cancelled run aborts every artefact it was writing — video, chroma, metadata, WAV tracks, dropout sidecar and EFM stream — so it leaves no partially written files behind. A cancelled run is reported as cancelled, not as a failure.
