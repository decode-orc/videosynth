# `sections[].noise`

Per-section additive Gaussian noise. Optional — omit the block entirely for a clean section.

```yaml
    noise:
      noise_db: 43.0
      noise_spread_db: 4.0
      noise_seed: 1001
```

## Keys

| Key | Type | Required | Range | Description |
|-----|------|----------|-------|-------------|
| `noise_db` | float | Conditional | 20.0–61.0 | Noise floor in dB; sets the Black PSNR target. Required if `noise_spread_db` is present |
| `noise_spread_db` | float | No | 0.0 – (`noise_db` − 20.0) | How many dB noisier white is than black. White SNR = `noise_db` − `noise_spread_db` | 
| `noise_seed` | int | No | any int64 | Fixed RNG seed for reproducible noise. Omit for a run-specific seed |

No other key is accepted.

## Behaviour

| Configuration | Result |
|---------------|--------|
| Block absent | No noise |
| `noise_db` only | Flat floor; White SNR = Black PSNR = `noise_db` |
| Both keys | Two-component model; brighter picture content is noisier |
| `noise_spread_db` without `noise_db` | Parse error |
| `noise_seed` only | No noise — the block has no effect |

Noise is injected on the internal millivolt representation **before quantisation**, so it lands on every synthesised region — VBI lines, blanking and active picture — and survives into the final sample codes.

## The bounds

**20.0 dB lower bound** — at 20 dB the noise standard deviation is around 10 IRE, roughly 70 mV on PAL, approaching the amplitude of the sync pulse itself. Sync separator reliability degrades below this.

**61.0 dB upper bound** — above 61 dB the injected noise is smaller than the 10-bit quantisation floor (about 0.085 IRE) and would have no measurable effect.

`noise_db − noise_spread_db ≥ 20.0` is enforced for the same reason: the white component cannot be pushed below the floor either.

## `noise_seed`

With a seed, the noise is identical on every run of the project. Without one, each run gets a fresh seed.

Fixed seeds are what make multi-source stacking sets meaningful — see [Impairments](../user-manual/impairments.md#seeds).

## White SNR measurability

The White SNR figure can only be measured from the picture if a VITS carries a white reference on the measurement line: **PAL frame line 19**, **NTSC frame line 20**.

If `noise_spread_db > 0` and no VITS targets that line, a **warning** is emitted. The signal is generated correctly regardless; the warning is about whether an analysis tool will be able to verify the target you asked for.
