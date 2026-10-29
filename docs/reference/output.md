# `output`

Where the generated media goes and in what form. Required.

```yaml
output:
  video_path: "{output}/pal_disc.cvbs"
  signal_type: composite
  efm_audio:
    pair: 0
```

## Keys

| Key | Type | Required | Values | Default |
|-----|------|----------|--------|---------|
| `video_path` | string | Yes | A path, optionally with an [asset root token](asset-roots.md) | — |
| `signal_type` | string | No | `composite`, `yc` | `composite` |
| `efm_audio` | map | No | See below | absent |

No other key is accepted. In particular the metadata sidecar path is **not** configurable — it is derived from `video_path` and always sits beside it.

## `video_path`

The path of the primary sample file. Every other artefact is colocated with it and named from it.

| `signal_type` | Required suffix | Files written |
|---------------|-----------------|---------------|
| `composite` | `.cvbs` by convention | `<name>.cvbs` |
| `yc` | `.cvbsy` (enforced) | `<name>.cvbsy` and `<name>.cvbsc` |

With `signal_type: yc` the path **must** end in `.cvbsy`; the chroma path is derived by replacing that suffix with `.cvbsc`.

The derived paths:

| Artefact | Derivation |
|----------|------------|
| `.meta` | `video_path` with `.cvbs`/`.cvbsy` replaced by `.meta` (any other extension is replaced too; with none, `.meta` is appended) |
| `.dropouts.meta` | The metadata path with `.meta` replaced by `.dropouts.meta` |
| `_audio_<pair>.wav` | `video_path` with `.cvbs`/`.cvbsy` stripped, plus `_audio_<pair>.wav` |
| `.efm`, `.efm.meta` | `video_path` with `.cvbs`/`.cvbsy` stripped, plus the suffix |

Parent directories are created automatically.

!!! tip "Use `{output}`"
    Write `"{output}/name.cvbs"` rather than a bare relative path. `{output}` defaults to the project file's own directory, so a project stays self-contained when run by hand, while `--output-root` lets a batch run redirect every project at once. A bare relative path resolves against the working directory in the CLI but against the project directory in the GUI.

## `signal_type`

- **`composite`** — luma and chroma summed into a single signal.
- **`yc`** — luma and chroma written separately.

Luma and chroma are generated independently regardless; this only selects whether they are combined before writing.

## `efm_audio`

Its presence enables laserdisc EFM digital audio output for one channel pair. Omit the block to disable.

PAL uses the 625-line definition of IEC 60856 Amd 2 clause 13. NTSC and PAL-M use the 525-line definition of IEC 60857 Amd 2 clause 13: no LaserVision standard names PAL-M, and a PAL-M disc is a 525-line disc at the NTSC frame rate, so it takes the NTSC sample sequence and identifies as a 525-line disc in its table of contents.

| Key | Type | Required | Range |
|-----|------|----------|-------|
| `pair` | int | Yes | 0–7 |

Constraints:

| Rule | Severity |
|------|----------|
| `pair` in 0–7 | Error |
| Standard must be `PAL`, `NTSC` or `PAL_M` (not `SECAM`) | Error |
| At most 79 `programme_area` sections (one EFM track each) | Error |
| No section declares `pair` — nothing to encode, no `.efm` written | Warning |
| A `programme_area` section under 4 s (6 s for the first) | Warning |
| No `lead_in` section, so no table of contents | Warning |

The pair's tone parameters are declared only in the per-section [`audio:`](section-audio.md) blocks; its 48 kHz WAV track is written as normal and is unaffected. See [Audio](../user-manual/audio.md#laserdisc-efm-digital-audio).
