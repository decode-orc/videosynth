# Impairments — noise and dropouts

A clean synthetic signal proves that a decoder works. A deliberately degraded one proves how it fails. videosynth can add controlled amounts of noise and dropout to any section, with reproducible seeds, so a decoder or restoration tool can be exercised against a known level of damage.

Both are declared per section, so damage can vary along the timeline — a clean passage, then a noisy one, then a scratch that appears and fades.

## Noise

```yaml
sections:
  - name: NoisySection
    type: progressive
    source: "{bundled}/exr/720x576/100_BARS.exr"
    duration_frames: 100
    noise:
      noise_db: 43.0
      noise_spread_db: 4.0
      noise_seed: 1001
```

### The model

Noise is additive Gaussian, injected on the internal millivolt representation **before quantisation**, so it lands on everything the generator produced — VBI lines, blanking and active picture alike — and survives into the final sample codes.

It has two components:

| Key | Meaning |
|-----|---------|
| `noise_db` | The noise floor, in dB. Sets the Black PSNR target. Range 20.0–61.0 |
| `noise_spread_db` | How many dB *noisier* white is than black. White SNR = `noise_db` − `noise_spread_db`. Default 0.0 |
| `noise_seed` | Fixed RNG seed. Omit for a run-specific seed |

- Neither key present → no noise at all.
- `noise_db` alone → a flat floor; White SNR equals Black PSNR.
- Both → the two-component model, in which brighter parts of the picture are noisier, as they are on real analogue media.

`noise_db − noise_spread_db` must be at least 20.0.

### Why the bounds

**20 dB floor** — at 20 dB the noise standard deviation is around 10 IRE, roughly 70 mV on PAL. That is approaching the amplitude of the sync pulse itself, and below it a sync separator stops being reliable. Signals noisier than this are not usefully "degraded video" so much as noise with a picture in it.

**61 dB ceiling** — above 61 dB the injected noise is smaller than the 10-bit quantisation floor (about 0.085 IRE), so it would have no measurable effect at all.

### Measuring the result

The two-component model is designed to be measurable from the picture by analysis tools that report Black PSNR and White SNR. Measuring the *white* figure needs a white reference on the measurement line — PAL frame line 19, NTSC frame line 20 — which in practice means a VITS.

videosynth warns when `noise_spread_db` is set and no VITS targets that line. The signal is generated correctly regardless; the warning is about whether the target you asked for can be verified downstream. See [VITS](vits.md).

### Seeds

With `noise_seed` set, the noise is identical on every run. Without it, each run gets a fresh seed.

Fixed seeds are what make multi-source "stacking" sets meaningful: several projects generate the same disc content with the *same* dropout seed (surface defects are a property of the disc and repeat on every play) but *different* noise seeds (electronic noise is fresh each time). A stacking tool can then be tested on whether it correctly averages away the noise while respecting the repeatable damage.

## Dropouts

```yaml
    dropouts:
      random:
        scale: 5
        seed: 42
      scratch:
        scale: 3
        seed: 42
```

Two independent kinds of dropout are modelled.

### Random dropouts

Surface and media degradation, modelled as a Poisson process: short signal losses scattered across the picture, independent frame to frame.

### Scratch dropouts

A persistent physical defect. A scratch appears at a position, grows to a peak severity and fades away again over a run of frames — a triangular amplitude envelope — so it is present across many consecutive frames rather than flickering randomly.

### Severity scales

Both take a `scale` from 1 to 20. It is a severity level, not a physical unit; the mapping is exponential, so low numbers are subtle and high numbers are destructive.

| Parameter | Scale 1 | Scale 10 | Scale 20 |
|-----------|---------|----------|----------|
| Random events per frame (mean) | 0.5 | ~8.5 | 200 |
| Random duration range (samples) | 5–50 | 15–134 | 50–400 |
| Scratch count | 2 | 8 | 40 |
| Scratch maximum lifespan (frames) | 2 | 27 | 500 |
| Scratch maximum peak width (samples) | 5 | 85 | 2000 |

Scale 1 is a couple of brief, narrow events; scale 20 is a disc you would throw away.

### Rules

- A `dropouts:` block with both scales at 0, or with neither sub-block present, is a **validation error** — omit the whole block to disable dropouts rather than zeroing it.
- `scale` outside 0–20 is an error; `0` turns that kind off while the other stays active.
- If the derived maximum scratch lifespan is longer than a fixed-length section, a **warning** is emitted: scratch events may be cut off before they reach their peak amplitude. That is legal — it is how you model a scratch that starts mid-way — but it is worth knowing you asked for it.

### The dropout sidecar

When any section enables dropouts, the pipeline writes a companion SQLite database beside the metadata sidecar:

```text
<basename>.dropouts.meta
```

It carries one table, `dropout_run`, with one row per contiguous dropout run:

| Column | Meaning |
|--------|---------|
| `cvbs_file_id` | The video file the run belongs to |
| `frame_id` | Frame the run is in |
| `sample_start` | First affected sample |
| `sample_count` | Length of the run |
| `severity` | `25` for runs entirely in the non-visible VBI, `75` for runs intersecting the active picture |

This is ground truth. A dropout-detection or concealment tool can be scored directly against it — every dropout videosynth introduced is listed, with its exact extent.

## A worked example

Modelling a disc that starts clean and degrades:

```yaml
sections:
  - name: Clean
    type: progressive
    source: "{bundled}/exr/720x576/100_BARS.exr"
    duration_frames: 100
    noise:
      noise_db: 55.0
      noise_seed: 1001

  - name: Moderate
    type: progressive
    source: "{bundled}/exr/720x576/100_BARS.exr"
    duration_frames: 100
    noise:
      noise_db: 43.0
      noise_spread_db: 4.0
      noise_seed: 1001
    dropouts:
      random:
        scale: 5
        seed: 1001

  - name: Damaged
    type: progressive
    source: "{bundled}/exr/720x576/100_BARS.exr"
    duration_frames: 100
    noise:
      noise_db: 30.0
      noise_spread_db: 6.0
      noise_seed: 1001
    dropouts:
      random:
        scale: 12
        seed: 1001
      scratch:
        scale: 8
        seed: 1001
```

The repository's `projects/stacking/` directory contains fuller examples of this pattern, including four-source stacking sets where the same disc is "captured" four times with matched dropout seeds and differing noise seeds.
