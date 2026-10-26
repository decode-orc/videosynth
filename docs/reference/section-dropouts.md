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
| `seed` | int | No | any int64 | Fixed RNG seed. Omit for a run-specific seed |

### `scratch`

A persistent physical defect: it appears, grows to a peak severity and fades away over a run of frames — a triangular amplitude envelope — so it spans many consecutive frames rather than flickering.

| Key | Type | Required | Range | Description |
|-----|------|----------|-------|-------------|
| `scale` | int | No | 0–20 | Severity, default `0`. `0` disables |
| `seed` | int | No | any int64 | Fixed RNG seed |

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

This is ground truth — every dropout videosynth introduced, with its exact extent — so a dropout detector or concealer can be scored directly against it.
