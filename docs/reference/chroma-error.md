# `chroma_error`

The chroma gain and phase error of **one capture**: a player whose chroma path is slightly mis-adjusted, or whose hue and saturation drift from line to line. Optional — omit the block for an error-free chroma path.

```yaml
chroma_error:
  gain: 1.05
  phase_deg: 2.0
  line_jitter: { phase_deg: 0.5, gain: 0.01, seed: 5 }
  include_burst: false
```

Unlike [`channel`](channel.md), which belongs to the pressing, the chroma error belongs to the player that made the capture, so each capture of one pressing may use a different block.

## Keys

| Key | Type | Required | Range | Default | Description |
|-----|------|----------|-------|---------|-------------|
| `gain` | float | No | 0.5 to 1.5 | 1.0 | Chroma amplitude multiplier (saturation) |
| `phase_deg` | float | No | −180.0 to 180.0 | 0.0 | Chroma phase advance relative to burst (hue), in degrees |
| `line_jitter` | map | No | — | — | Per-line random variation, below |
| `include_burst` | bool | No | `true`, `false` | `false` | Apply the error to the colour burst as well; see [Including the burst](#including-the-burst) |

### `line_jitter`

| Key | Type | Required | Range | Description |
|-----|------|----------|-------|-------------|
| `phase_deg` | float | No | 0.0 to 10.0 | Standard deviation of the per-line phase, in degrees |
| `gain` | float | No | 0.0 to 0.1 | Standard deviation of the per-line gain, about `gain` |
| `seed` | integer | No | any | Fixed RNG seed. Omit for a run-specific seed |

`chroma_error:` must set at least one of `gain`, `phase_deg` or `line_jitter` (`include_burst` alone does not count), and `line_jitter:` must set at least one of its `phase_deg` or `gain`; an empty block is a parse error.

## Behaviour

| Configuration | Result |
|---------------|--------|
| Block absent | No chroma error stage; output is unchanged |
| `gain: 1.0`, `phase_deg: 0.0`, no jitter (or zero deviations) | No processing; output is bit-identical to the block being absent |
| `gain` | Every picture line's chroma amplitude is scaled |
| `phase_deg` | Every picture line's chroma phase is advanced relative to burst |
| `line_jitter` | Each picture line adds its own Gaussian deviation to the phase and gain |
| `include_burst: true` | Every line's burst is scaled and rotated as well |

### Picture chroma only

By default the error is applied to chroma inside the active window of active picture lines. The colour burst, VBI lines, lines carrying a project VITS and the blanking between are left bit-identical. A burst-locked decoder therefore sees a real hue and saturation error, as it would from a mis-adjusted player, rather than a burst that has moved with the picture.

### Including the burst

`include_burst: true` models the other real case: a gain or phase error in the **whole** chroma path, or an FM/MTF loss on the disc, which moves the burst with the picture. The burst of every line is changed as well:

- A picture line's burst takes that line's own error, jitter included, so picture and burst move together.
- Bursts on VBI and VITS lines take the block's `gain` and `phase_deg`, without jitter.
- The picture chroma is bit-identical to `include_burst: false`; only the bursts change.

A burst-locked decoder therefore sees no hue error against burst, and one that references its chroma gain to the burst amplitude can measure and correct the gain. The window rotated around each burst is the burst gate widened by 8 samples each side, enough to follow the largest [channel](channel.md) timing offset. The burst is short against the Hilbert transformer, so a phase rotation of the burst is accurate to about 1° and 3 %; a pure gain error is exact.

### How it is applied

On each picture line the stage forms the quadrature of the chroma with a Hilbert transformer and replaces the chroma `C` with

```text
g · (C · cos θ − Ĉ · sin θ)
```

which scales the subcarrier by `g` and advances its phase by `θ`. The Hilbert transformer is a 47-tap Kaiser-windowed design, accurate to within 10⁻⁴ from 2 MHz to 2 MHz below Nyquist on every standard (2–6.87 MHz on 625-line, 2–5.16 MHz on 525-line), which covers every chroma band. It is computed from the active window's own samples, so the burst never leaks into it.

On PAL and PAL-M the phase advance is the same on every line, so it alternates in sign once the decoder undoes the V-axis switch — the classic PAL hue error that a delay-line decoder turns into a saturation loss.

The result is not clamped: chroma is centred on zero and saturated colours legitimately exceed the composite limits. The output stage quantises the final signal.

### Jitter seeds

The jitter is a capture property, so it is keyed by the jitter seed and the **output** frame position, drawn line by line in frame order. With `seed` set the jitter is identical on every run and for any `--threads` value. Without it, each run draws a fresh seed, as an unseeded `noise_seed` does.

Each picture line always draws two values, phase then gain, whether or not each deviation is set, so changing one deviation never moves the other's values.

## Stage order

1. Generation
2. [Channel](channel.md) (timing offset)
3. **Chroma error**
4. Noise (capture layer, then shared layer)
5. Dropouts
6. Output

The chroma error runs after the channel, so pressing impairments are computed from error-free chroma, and before noise, so the noise is not rotated.

## Rules

- Values outside their ranges, or not finite, are validation errors.
- A non-numeric value, a non-boolean `include_burst`, an unknown key or an empty block is a parse error.
- SECAM is a validation error: it has no quadrature chroma to rotate.
