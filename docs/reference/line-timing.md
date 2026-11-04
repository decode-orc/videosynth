# `line_timing`

The per-line timing error of the active picture that is left after time-base correction: each picture line is displaced horizontally by a few nanoseconds, by a different amount on every line, field and frame. Optional — omit the block for a perfectly time-base-corrected picture.

```yaml
line_timing:
  line_ns: 4.9          # within-field r.m.s., per line
  line_correlation: 0.6 # first-order, line to line within a field
  field_ns: 3.1         # r.m.s. offset common to the field
  slope_ns: 3.4         # r.m.s. difference between the line's two halves
  seed: 7
  include_burst: false
  shared:               # the part recorded on the disc
    line_ns: 3.0
    line_correlation: 0.6
    field_ns: 1.5
    slope_ns: 2.0
    seed: 7001
```

The keys of the block itself are the **capture** layer: the residual of one capture's player and time-base corrector, so each capture of a pressing draws its own. `shared:` is the **pressing** layer: the part recorded on the disc, identical in every capture of the pressing, as the [shared noise layer](section-noise.md) is.

At 4fsc a 625-line sample is 56.4 ns and a 525-line sample 69.8 ns, so a few ns is about a tenth of a sample.

## Keys

| Key | Type | Required | Range | Default | Description |
|-----|------|----------|-------|---------|-------------|
| `line_ns` | float | One of the figures, or `shared` | 0.0 to 50.0 | 0.0 | R.m.s. shift of each line within its field (the field's mean removed), in ns |
| `line_correlation` | float | No | 0.0 to 0.99 | 0.0 | Correlation of a line's shift and slope with the next line of the same field |
| `field_ns` | float | One of the figures, or `shared` | 0.0 to 50.0 | 0.0 | R.m.s. offset common to a whole field, in ns |
| `slope_ns` | float | One of the figures, or `shared` | 0.0 to 50.0 | 0.0 | R.m.s. difference between the shifts of the line's two halves (a velocity error), in ns |
| `seed` | integer | No | any | — | Fixed seed of the capture layer. Omit for a run-specific seed |
| `include_burst` | bool | No | `true`, `false` | `false` | Move each picture line's colour burst with the line; see [The burst](#the-burst) |
| `shared` | map | No | — | — | The pressing layer, below |

### `shared`

| Key | Type | Required | Range | Default | Description |
|-----|------|----------|-------|---------|-------------|
| `line_ns` | float | One of the three figures | 0.0 to 50.0 | 0.0 | As above |
| `line_correlation` | float | No | 0.0 to 0.99 | 0.0 | As above |
| `field_ns` | float | One of the three figures | 0.0 to 50.0 | 0.0 | As above |
| `slope_ns` | float | One of the three figures | 0.0 to 50.0 | 0.0 | As above |
| `seed` | integer | Yes | any | — | Fixed seed of the pressing layer, so every capture shares it |

`line_timing:` must set at least one of `line_ns`, `field_ns`, `slope_ns` or `shared`, and `shared:` one of its `line_ns`, `field_ns` or `slope_ns`. `seed` at the top of the block seeds the capture layer, so it needs one of the capture figures.

## Behaviour

| Configuration | Result |
|---------------|--------|
| Block absent | No line timing stage; output is unchanged |
| Every figure 0.0 | No processing; output is bit-identical to the block being absent |
| `line_ns` | Each line takes its own shift, correlated with its neighbours by `line_correlation` |
| `field_ns` | Every line of a field takes the same extra shift |
| `slope_ns` | Each line's shift grows linearly along it |
| `shared` | A second, disc-keyed error is added to the capture layer's |

### The model

Each layer draws, for every field of every frame:

```text
shift(k) = F + x(k)
slope(k) = s(k)
x(k)     = r · x(k−1) + √(1 − r²) · line_ns  · e(k),   x(0) = line_ns  · e(0)
s(k)     = r · s(k−1) + √(1 − r²) · slope_ns · e'(k),  s(0) = slope_ns · e'(0)
F        = field_ns · e''
```

where `k` counts the lines of the field, `r` is `line_correlation`, and `e`, `e'` and `e''` are independent unit normals. Both processes are stationary: every line has the configured r.m.s. and the correlation between lines `n` apart is `rⁿ` (0.6, 0.36, 0.13 and 0.02 at 1, 2, 4 and 8 lines for `r` = 0.6). They advance over every line of the field, picture or not, so the correlation depends on the line spacing alone. The two fields of a frame, and successive frames, are independent.

Each sample `n` of a picture line is then read from the line at `n − d(n)`:

```text
d(n) = shift + slope · (n − c) / (W / 2)
```

with `c` the centre and `W` the width of a full active line (52 µs), so the line's two halves, whose centres are `W / 2` apart, differ by `slope`. Positive values arrive later. The read is band-limited: a Kaiser-windowed sinc of 32 taps, the kernel of the channel's [timing offset](channel.md#timing_offset_samples), tabulated at 512 fractional positions. Y and C move together, so a line's chroma moves with its luma, and a shift of `d` also delays the line's subcarrier by `2π · fsc · d` against its burst.

### What moves

Only the active window of picture lines moves: the lines and samples that carry the picture, less any line carrying a project VITS. Sync, the colour burst, the VBI and the VITS lines stay where they are, because the measured residual moves the picture relative to a burst-locked line. Samples just outside a window are read as they are, so a picture edge moves into the blanking next to it.

### The burst

`include_burst: true` moves each picture line's burst too, by the same `d(n)` evaluated at the burst's samples. A decoder that references every line to its own burst then sees the line's chroma phase follow its burst, which is how such a decoder corrects the error. The window moved is the burst gate widened by 8 samples each side. Only C moves there: Y is flat blanking under the burst. SECAM has no burst, so `include_burst` is a validation error on SECAM; the rest of the block applies to SECAM too.

### Seeds

The capture layer is keyed by its seed and the **output** frame position, so a capture that replays a picture draws a fresh error for it. With `seed` set it is identical on every run and for any `--threads` value; without it, each run draws a fresh seed. The pressing layer is keyed by its seed and the **disc** frame position (the CAV picture number when the section anchors one), so every capture of one pressing, and a replay of the same picture, carries the same recorded error. Each layer uses its own seed domain, so a capture and a pressing seed of the same value still draw independently.

Every line always draws two values, shift then slope, and every field one field offset, whether or not each figure is set, so changing one figure never moves another's values.

### The picture truth

The [picture truth](output.md#truth) is drawn from generation, so it is the unjittered picture: what a perfect time-base corrector would give, and what a mean over many frames of a real disc approximates.

## Stage order

1. Generation
2. [LaserVision channel](laserdisc-channel.md) and [channel](channel.md)
3. [Chroma error](chroma-error.md)
4. **Line timing**
5. Noise (capture layer, then shared layer)
6. Dropouts
7. Output

The error is applied after the channel and the chroma error, and before noise, so the noise is not shifted. Y is clamped to the same legal range as noise injection; C is centred on zero and is not clamped.

## Rules

- Values outside their ranges, or not finite, are validation errors.
- `shared:` without a `seed` is a validation error: the pressing layer must be identical in every capture of the pressing.
- `include_burst: true` on SECAM is a validation error.
- A non-numeric figure, a non-integer seed, a non-boolean `include_burst`, an unknown key or an empty block is a parse error.
