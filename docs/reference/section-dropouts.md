# `sections[].dropouts`

Per-section dropout simulation. Optional — omit the block entirely to disable dropouts.

```yaml
    dropouts:
      random:
        scale: 5
        seed: 42
      scratch:
        scale: 3
        seed: 42
```

## Keys

| Key | Type | Required | Description |
|-----|------|----------|-------------|
| `random` | map | No | Poisson random dropouts (below) |
| `scratch` | map | No | Persistent radial scratch defects (below) |

At least one must be present and non-zero. No other key is accepted.

### `random`

Surface and media degradation, modelled as a Poisson process — short losses scattered across the picture, independent frame to frame.

| Key | Type | Required | Range | Description |
|-----|------|----------|-------|-------------|
| `scale` | int | No | 0–20 | Severity, default `0`. `0` disables, equivalent to omitting the block |
| `seed` | int | No (required with `anchor: disc`) | any int64 | Fixed RNG seed. Omit for a run-specific seed |
| `flagged` | bool | No | `true`, `false` | Default `true`. `false` applies the dropouts but leaves them out of the [sidecar](#the-dropout-sidecar); see [`flagged`](#flagged) |
| `anchor` | string | No | `section`, `disc` | Default `section`. What the dropouts are keyed to; see [`anchor`](#anchor) |

### `scratch`

A persistent physical defect: it appears, grows to a peak severity and fades away over a run of frames — a triangular amplitude envelope — so it spans many consecutive frames rather than flickering.

| Key | Type | Required | Range | Description |
|-----|------|----------|-------|-------------|
| `scale` | int | No | 0–20 | Severity, default `0`. `0` disables |
| `seed` | int | No (required with `anchor: disc`) | any int64 | Fixed RNG seed |
| `flagged` | bool | No | `true`, `false` | Default `true`. As for `random` |
| `anchor` | string | No | `section`, `disc` | Default `section`. As for `random` |

## `flagged`

A real player flags most dropouts, but not all: a defect the RF detector misses still damages the picture. `flagged: false` models those.

- The dropouts are applied to the signal exactly as with `flagged: true`. The flag changes no random draw, so the same project with `flagged: true` reproduces the same events, and their rows.
- No sidecar row is written for them, so the sidecar holds no record of them at all.
- The sidecar is still created whenever any section has dropouts, so the set of output files does not change when the flag is toggled.
- `random` and `scratch` are flagged independently. In a section with one of each flagged, the sidecar records only that type's runs.

## `anchor`

`anchor` sets whether the dropouts belong to one capture or to the disc.

| Value | Keyed by | Models |
|-------|----------|--------|
| `section` (default) | The section and the frame's output position | Dropouts of one playback: a different section, or a later start, draws different ones |
| `disc` | The disc frame (picture number − 1 on CAV) and the seed | A defect of the pressing: every capture with the same seed carries it on the same picture, whichever section and output position the picture lands on |

With `anchor: disc`:

- **Random** dropouts on a frame depend only on `seed` and the frame's disc position, so a capture that starts later, skips or replays a range shows the same dropouts on the same pictures.
- **Scratch** events depend on `seed` and the disc frame the section starts on (its `picture_number` `start_value` − 1 on CAV), and each event's lifespan is counted from that frame. A replay section with the same `start_value` therefore repeats the first play's scratches, while a section starting elsewhere on the disc draws its own.
- `seed` is required, since every capture must draw the same events.

On a disc without picture numbers, the disc frame is the output position plus the project's first-picture offset.

## Severity scale

`scale` is a severity level, not a physical unit. The mapping is exponential in t = (scale − 1) / 19:

| Parameter | Formula | Scale 1 | Scale 10 | Scale 20 |
|-----------|---------|---------|----------|----------|
| Random events per frame (Poisson mean) | 0.5 × 400^t | 0.5 | ~8.5 | 200 |
| Random minimum duration (samples) | 5 × 10^t | 5 | ~15 | 50 |
| Random maximum duration (samples) | 50 × 8^t | 50 | ~134 | 400 |
| Scratch count | 2 × 20^t, at least 2 | 2 | 8 | 40 |
| Scratch maximum lifespan (frames) | 2 × 250^t | 2 | ~27 | 500 |
| Scratch maximum peak width (samples) | 5 × 400^t | 5 | ~85 | 2000 |

Values are rounded to whole numbers except the event rate. A random dropout's duration is drawn uniformly between its minimum and maximum.

## Rules

| Rule | Severity |
|------|----------|
| `scale` outside 0–20 | Error |
| `anchor: disc` without a `seed` | Error |
| `anchor` other than `section` or `disc`, or `flagged` not a boolean | Parse error |
| A `dropouts:` block that enables nothing — `dropouts: {}`, or both scales 0 or absent | Error — omit the block instead |
| Derived maximum scratch lifespan longer than an integer `duration_frames` | Warning: the scratch envelope is truncated |

```text
Dropout validation error in section 'Programme': dropouts block has no active
type (both random.scale and scratch.scale are 0). Remove the dropouts block or
set at least one scale to 1–20.
```

The "disable by omission, not by zeroing" rule exists so that a block that looks like it configures something always does.

## The dropout sidecar

When any section has dropouts enabled, the pipeline writes a companion SQLite database:

```text
<basename>.dropouts.meta      schema version 5
```

with a single table:

```sql
dropout_run(cvbs_file_id, frame_id, sample_start, sample_count, severity)
```

`severity` is `25` for runs entirely within the non-visible VBI and `75` for runs intersecting the active picture.

This is ground truth for every flagged dropout videosynth introduced, with its exact extent, so a dropout detector or concealer can be scored directly against it. Dropouts with `flagged: false` are absent from it.
