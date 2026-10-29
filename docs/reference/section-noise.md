# `sections[].noise`

Per-section additive Gaussian noise. Optional — omit the block entirely for a clean section.

The block carries two independent layers:

- the **capture layer** (the top-level keys), describing one playback of the disc;
- the **shared layer** (`shared:`), describing the pressing.

```yaml
    noise:
      noise_db: 43.0
      noise_spread_db: 4.0
      noise_seed: 1001
      spectrum:                # optional
        - [0.0, 0.0]
        - [3.5, 4.0]
        - [6.0, 10.0]
      shared:                  # optional
        noise_db: 44.0
        noise_seed: 77
        spectrum: [[0.0, 0.0], [5.0, -3.0]]
```

## Keys

| Key | Type | Required | Range | Description |
|-----|------|----------|-------|-------------|
| `noise_db` | float | Conditional | 20.0–61.0 | Noise floor in dB; sets the Black PSNR target. Required if `noise_spread_db` is present |
| `noise_spread_db` | float | No | 0.0 – (`noise_db` − 20.0) | How many dB noisier white is than black. White SNR = `noise_db` − `noise_spread_db` | 
| `noise_seed` | int | No | any int64 | Fixed RNG seed for reproducible noise. Omit for a run-specific seed |
| `spectrum` | list of `[MHz, dB]` | No | see [`spectrum`](#spectrum) | Shape of the capture noise spectrum. Requires `noise_db`. Omit for white noise |
| `shared` | map | No | see below | The shared (pressing) noise layer |

### `shared`

| Key | Type | Required | Range | Description |
|-----|------|----------|-------|-------------|
| `noise_db` | float | Yes | 20.0–61.0 | Pressing noise level in dB |
| `noise_seed` | int | Yes | any int64 | Seed of the pressing. Captures of one pressing use the same value |
| `spectrum` | list of `[MHz, dB]` | No | see [`spectrum`](#spectrum) | Shape of the pressing noise spectrum. Omit for white noise |

`shared` takes no `noise_spread_db`: pressing noise has a constant level, so it is identical in every capture whatever each capture's signal is.

No other key is accepted in either layer.

## Behaviour

| Configuration | Result |
|---------------|--------|
| Block absent | No noise |
| `noise_db` only | Flat floor; White SNR = Black PSNR = `noise_db` |
| Both keys | Two-component model; brighter picture content is noisier |
| `noise_spread_db` without `noise_db` | Parse error |
| `noise_seed` only | No noise — the block has no effect |
| `shared` only | Pressing noise only; no capture layer |
| `spectrum` without `noise_db` | Parse error |
| `shared` without `noise_db` or with `noise_spread_db` | Parse error |
| Both layers | Their noise values are summed per sample, so the variances add |

Noise is injected on the internal millivolt representation **before quantisation**, so it lands on every synthesised region — VBI lines, blanking and active picture — and survives into the final sample codes. One noise value per sample is added to both Y and C (correlated Y/C noise). Y is then clamped to the legal composite range; C is centred on zero, so saturated chroma is not clipped here, and the output stage clamps the combined signal.

## `spectrum`

A table of `[frequency_mhz, relative_db]` breakpoints describing the shape of the noise power spectrum.

| Rule | Value |
|------|-------|
| Points | At least one |
| `frequency_mhz` | 0.0 to the 4fsc Nyquist frequency, 2 × f<sub>sc</sub>, strictly ascending: 8.867 MHz (PAL), 7.159 MHz (NTSC), 7.151 MHz (PAL-M). Noise is shaped before the output stage resamples a RAW encoding, so the limit is the same for every `sample_encoding_preset` |
| `relative_db` | −60.0 to +40.0 |

Between breakpoints the level is a straight line in dB against MHz; below the first point and above the last, the end values are held.

Only the shape matters. The noise is filtered and then normalised so its total power is unchanged, which means `noise_db` still sets the overall level: a table that tilts the noise towards high frequencies moves power out of the low band into the high band, it does not add any. A one-point table is therefore the same as no table.

The shaping filter is a 255-tap linear-phase FIR designed once per table. Its frequency resolution is about 70 kHz at PAL 4fsc (56 kHz at NTSC 4fsc), and each breakpoint is smoothed over roughly four times that.

On the capture layer, `noise_spread_db` still applies sample by sample: the level-dependent term scales the shaped noise, so bright content carries louder noise of the same shape.
## The bounds

**20.0 dB lower bound** — at 20 dB the noise standard deviation is around 10 IRE, roughly 70 mV on PAL, approaching the amplitude of the sync pulse itself. Sync separator reliability degrades below this.

**61.0 dB upper bound** — above 61 dB the injected noise is smaller than the 10-bit quantisation floor (about 0.085 IRE) and would have no measurable effect.

`noise_db − noise_spread_db ≥ 20.0` is enforced for the same reason: the white component cannot be pushed below the floor either.

## `noise_seed`

With a seed, the noise is identical on every run of the project. Without one, each run gets a fresh seed.

Fixed seeds are what make multi-source stacking sets meaningful — see [Impairments](../user-manual/impairments.md#seeds).

## Shared noise and disc position

The capture layer is keyed by section and output position: two projects with the same seed match only when their timelines match.

The shared layer is keyed by **disc position** only — the CAV picture number when the section carries a `picture_number` code, otherwise the output position plus the project's first-picture offset. Section boundaries and output position play no part. Two captures of one pressing therefore carry identical pressing noise on the same picture even when one starts later, skips, or replays a range of pictures. Consecutive pictures always get different pressing noise, including the repeated pictures of a held still.

Its seed is mixed with a constant specific to the shared layer, so a `shared.noise_seed` equal to a capture `noise_seed` still gives independent noise.

On a CAV disc, a section with `shared` but without a `picture_number` code (a lead-in or lead-out, for example) gets a **warning**: its pressing noise falls back to output position and will not line up across captures that start at different points.

## White SNR measurability

The White SNR figure can only be measured from the picture if a VITS carries a white reference on the measurement line: **PAL frame line 19**, **NTSC frame line 20**.

If `noise_spread_db > 0` and no VITS targets that line, a **warning** is emitted. The signal is generated correctly regardless; the warning is about whether an analysis tool will be able to verify the target you asked for.
