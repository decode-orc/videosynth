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
      spread_spectrum:         # optional
        - [5.0, -20.0]
        - [6.0, 0.0]
      shared:                  # optional
        noise_db: 44.0
        noise_seed: 77
        spectrum: [[0.0, 0.0], [5.0, -3.0]]
        level_term:            # optional
          noise_db: 36.0
          level_db: [[0, 0.0], [420, -12.0], [700, -12.0]]
          spectrum: [[5.375, -40.0], [5.625, 0.0]]
```

## Keys

| Key | Type | Required | Range | Description |
|-----|------|----------|-------|-------------|
| `noise_db` | float | Conditional | 20.0–61.0 | Noise floor in dB; sets the Black PSNR target. Required if `noise_spread_db` is present |
| `noise_spread_db` | float | No | 0.0 – (`noise_db` − 20.0) | How many dB noisier white is than black. White SNR = `noise_db` − `noise_spread_db` | 
| `noise_seed` | int | No | any int64 | Fixed RNG seed for reproducible noise. Omit for a run-specific seed |
| `spectrum` | list of `[MHz, dB]` | No | see [`spectrum`](#spectrum) | Shape of the capture noise spectrum. Requires `noise_db`. Omit for white noise |
| `spread_spectrum` | list of `[MHz, dB]` | No | see [`spectrum`](#spectrum) | Shape of the level-dependent (`noise_spread_db`) term alone; `spectrum` then shapes the floor only. Requires `noise_db`. Omit for the level term to follow `spectrum`. See [`spread_spectrum`](#spread_spectrum) |
| `shared` | map | No | see below | The shared (pressing) noise layer |

### `shared`

| Key | Type | Required | Range | Description |
|-----|------|----------|-------|-------------|
| `noise_db` | float | Yes | 20.0–61.0 | Pressing noise level in dB |
| `noise_seed` | int | Yes | any int64 | Seed of the pressing. Captures of one pressing use the same value |
| `spectrum` | list of `[MHz, dB]` | No | see [`spectrum`](#spectrum) | Shape of the pressing noise spectrum. Omit for white noise |
| `level_term` | map | No | see below | A second pressing component whose level follows the picture's luma. See [`level_term`](#shared-level_term) |

`shared` takes no `noise_spread_db`. Its constant component is the same whatever the picture; its level dependence, when there is one, comes from `level_term`, which follows the pressing's own picture and not any capture's signal, so the layer stays identical in every capture.

### `shared.level_term`

| Key | Type | Required | Range | Description |
|-----|------|----------|-------|-------------|
| `noise_db` | float | Yes | 20.0–61.0 | Level of the term where `level_db` reads 0 dB |
| `level_db` | list of `[mV, dB]` | Yes | see [`level_term`](#shared-level_term) | Gain of the term against the picture's luma level |
| `spectrum` | list of `[MHz, dB]` | No | see [`spectrum`](#spectrum) | Shape of the term alone. Omit for white noise |

No other key is accepted in any of these blocks.

## Behaviour

| Configuration | Result |
|---------------|--------|
| Block absent | No noise |
| `noise_db` only | Flat floor; White SNR = Black PSNR = `noise_db` |
| Both keys | Two-component model; brighter picture content is noisier |
| `noise_spread_db` without `noise_db` | Parse error |
| `noise_seed` only | No noise — the block has no effect |
| `shared` only | Pressing noise only; no capture layer |
| `spectrum` or `spread_spectrum` without `noise_db` | Parse error |
| `spread_spectrum` with `noise_spread_db` 0 | Warning: there is no level-dependent term to shape |
| `shared` without `noise_db` or with `noise_spread_db` | Parse error |
| `level_term` without `noise_db` or `level_db` | Parse error |
| Both layers | Their noise values are summed per sample, so the variances add |

Noise is injected on the internal millivolt representation **before quantisation**, so it lands on every synthesised region — VBI lines, blanking and active picture — and survives into the final sample codes. `noise_db` is the level of each output signal:

| `output.signal_type` | Where the noise goes | Measured level |
|----------------------|----------------------|----------------|
| `composite` | One noise value per sample, added once (to Y before Y + C is formed) | The composite signal is at `noise_db` |
| `yc` | The same value added to both Y and C (correlated Y/C noise) | The luma and the chroma file are each at `noise_db` |

Y is then clamped to the legal composite range; C is centred on zero, so saturated chroma is not clipped here, and the output stage clamps the combined signal.

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

On the capture layer without `spread_spectrum`, `noise_spread_db` still applies sample by sample: the level-dependent term scales the shaped noise, so bright content carries louder noise of the same shape.

## `spread_spectrum`

A second table, with the same rules as [`spectrum`](#spectrum), that gives the capture layer's level-dependent term a shape of its own. Real captures often grow noisier with level in some bands and not in others; one `noise_spread_db` with a single shape cannot follow that.

With `spread_spectrum` present, the capture layer is the sum of two independent terms:

| Term | Level | Shape |
|------|-------|-------|
| Floor | σ<sub>f</sub>, from `noise_db` | `spectrum` (white when omitted) |
| Level-dependent | k · Y, from `noise_spread_db` and the sample's luma level | `spread_spectrum` |

The total power is unchanged — σ² = σ<sub>f</sub>² + (k · Y)², so Black PSNR and White SNR are still `noise_db` and `noise_db` − `noise_spread_db` — but the extra power that bright content carries lands only where `spread_spectrum` puts it. For example, `[[5.0, -20.0], [6.0, 0.0]]` keeps the noise below 5 MHz almost level-independent and puts the growth with level above 6 MHz.

The shared layer takes no `spread_spectrum`; its level-dependent part is [`level_term`](#shared-level_term), which carries its own `spectrum`.

## `shared.level_term` {#shared-level_term}

On real captures the pressing noise does not keep a constant level: in some bands it **falls** as the picture gets brighter. A capture-style σ² = σ<sub>f</sub>² + (k · Y)² can only grow with level, so the shared layer takes a separate term instead.

With `level_term` present, the shared layer is the sum of two independent components:

| Component | Level | Shape |
|-----------|-------|-------|
| Constant | `shared.noise_db` | `shared.spectrum` (white when omitted) |
| Level term | `level_term.noise_db` + the `level_db` gain at the sample's luma level | `level_term.spectrum` (white when omitted) |

`level_db` is a table of `[level_mv, relative_db]` breakpoints:

| Rule | Value |
|------|-------|
| Points | At least one |
| `level_mv` | 0.0 to 800.0, strictly ascending. The luma level in mV above blanking, on the same scale for every standard: white is 700 mV on PAL and SECAM, 714.3 mV on NTSC and PAL-M |
| `relative_db` | −60.0 to +20.0: the term's amplitude relative to `level_term.noise_db` |

Between breakpoints the gain is a straight line in dB against mV; below the first point and above the last, the end values are held, so sync and blanking take the first point's gain. A one-point table is a constant second component.

The level is read **per sample** from the pressing's own picture: the luma as generated, before the [channel](channel.md) stage's FM beat, edge smear and timing offset, and before any capture's noise. Every capture of the pressing therefore draws the same term on the same picture, whatever its channel or capture settings. The whole shared layer, term included, is then delayed by `channel.timing_offset_samples`, so it stays on the picture's samples.

The term has its own seed stream, derived from `shared.noise_seed` and the disc position, so adding, removing or editing it leaves the constant component's samples unchanged.

For example, this is pressing noise that is level-independent below 5.5 MHz and, above it, 12 dB quieter on mid-grey than on black:

```yaml
        shared:
          noise_db: 46.0
          noise_seed: 7101
          spectrum: [[5.375, 0.0], [5.625, -40.0]]   # constant part below 5.5 MHz
          level_term:
            noise_db: 36.0                           # above 5.5 MHz, at black
            level_db: [[0, 0.0], [420, -12.0]]
            spectrum: [[5.375, -40.0], [5.625, 0.0]]
```
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

The pressing noise is read off the disc with the picture, so the project's [`channel.timing_offset_samples`](channel.md) delays it exactly as it delays the picture: a capture whose offset differs from the rest of its pressing still carries the pressing noise on the same picture samples, and it cancels in the difference of two aligned captures. The capture layer stays on the output sample grid, since nothing else shares it.

On a CAV disc, a section with `shared` but without a `picture_number` code (a lead-in or lead-out, for example) gets a **warning**: its pressing noise falls back to output position and will not line up across captures that start at different points.

## White SNR measurability

The White SNR figure can only be measured from the picture if a VITS carries a white reference on the measurement line: **PAL frame line 19**, **NTSC frame line 20**.

If `noise_spread_db > 0` and no VITS targets that line, a **warning** is emitted. The signal is generated correctly regardless; the warning is about whether an analysis tool will be able to verify the target you asked for.
