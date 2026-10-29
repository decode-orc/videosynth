# Impairments — noise, dropouts, pressings, chroma error and luma bandwidth

A clean synthetic signal proves that a decoder works. A deliberately degraded one proves how it fails. videosynth can add controlled amounts of noise and dropout to any section, with reproducible seeds, so a decoder or restoration tool can be exercised against a known level of damage.

Noise and dropouts are declared per section, so damage can vary along the timeline — a clean passage, then a noisy one, then a scratch that appears and fades. So is the luma bandwidth limit. The `channel:` and `chroma_error:` blocks are project-level: the first describes the pressing, the second the capture.

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

The levels are those of the output signal: on composite output the composite signal is at `noise_db`; on Y/C output the luma and the chroma file each are, with correlated noise.

### Shaping the spectrum

Real channel noise is rarely white: FM demodulation tilts it towards high frequencies, and a player's filters shape it further. `spectrum` describes that shape as `[MHz, dB]` breakpoints:

```yaml
    noise:
      noise_db: 46.0
      noise_seed: 1001
      spectrum:
        - [0.0, 0.0]
        - [3.5, 4.0]
        - [6.0, 10.0]
```

Levels are joined by straight lines in dB, and the end values are held out to 0 Hz and to Nyquist. The table sets the *shape* only — the shaped noise is normalised back to `noise_db`, so the overall Black PSNR is unchanged and the table just moves power between frequencies. With `noise_spread_db` set, the level-dependent term scales the shaped noise. See the [reference](../reference/section-noise.md#spectrum) for the limits.

On real captures the noise does not grow with level the same way in every band. `spread_spectrum` gives the level-dependent term its own shape, and `spectrum` then shapes the floor alone:

```yaml
    noise:
      noise_db: 46.0
      noise_spread_db: 5.0
      noise_seed: 1001
      spread_spectrum: [[5.0, -20.0], [6.0, 0.0]]  # only above ~6 MHz grows with level
```

Black PSNR and White SNR are unchanged; only where the extra noise of bright content lands changes. See the [reference](../reference/section-noise.md#spread_spectrum).

### Pressing noise and capture noise

Several captures of one disc share some of their noise and not the rest. Electronic noise in the player is fresh on every play; noise moulded into the pressing is the same each time the same picture is played. A stacking tool should average away the first and cannot remove the second.

The `shared:` sub-block is the pressing layer:

```yaml
    noise:
      noise_db: 46.0          # capture layer: this play
      noise_seed: 1001        # different in every capture
      shared:                 # pressing layer: the disc
        noise_db: 44.0
        noise_seed: 77        # the same in every capture of the pressing
        spectrum: [[0.0, 0.0], [5.0, -3.0]]
```

- The pressing layer is keyed by **disc position** — the CAV picture number, or the output position plus the first-picture offset when there is none. It ignores sections and output position, so captures line up on the picture even when one starts later, skips or replays.
- It has a constant level (no `noise_spread_db`), so it is identical in every capture whatever the rest of the signal is.
- It moves with the picture: `channel.timing_offset_samples` delays it as it delays the picture, so a capture with its own offset still lines up with the other captures of its pressing once the picture is aligned.
- `shared.noise_seed` is required. It is mixed with a layer-specific constant, so reusing a capture seed still gives independent noise.
- The two layers add, so the total noise power is the sum of both. A block with only `shared:` gives pressing noise alone.

To set up a pressing, give every project of that pressing the same `shared:` block and a different capture `noise_seed`. On CAV, put the `shared:` block on sections carrying `picture_number` codes; elsewhere it falls back to output position and videosynth warns.

### Why the bounds

**20 dB floor** — at 20 dB the noise standard deviation is around 10 IRE, roughly 70 mV on PAL. That is approaching the amplitude of the sync pulse itself, and below it a sync separator stops being reliable. Signals noisier than this are not usefully "degraded video" so much as noise with a picture in it.

**61 dB ceiling** — above 61 dB the injected noise is smaller than the 10-bit quantisation floor (about 0.085 IRE), so it would have no measurable effect at all.

### Measuring the result

The two-component model is designed to be measurable from the picture by analysis tools that report Black PSNR and White SNR. Measuring the *white* figure needs a white reference on the measurement line — PAL frame line 19, NTSC frame line 20 — which in practice means a VITS.

videosynth warns when `noise_spread_db` is set and no VITS targets that line. The signal is generated correctly regardless; the warning is about whether the target you asked for can be verified downstream. See [VITS](vits.md).

### Seeds

With `noise_seed` set, the noise is identical on every run. Without it, each run gets a fresh seed.

Fixed seeds are what make multi-source "stacking" sets meaningful: several projects generate the same disc content with the *same* dropout seed (surface defects are a property of the disc and repeat on every play) but *different* noise seeds (electronic noise is fresh each time). A stacking tool can then be tested on whether it correctly averages away the noise while respecting the repeatable damage. Add a common `shared:` layer to model noise that belongs to the pressing and does not average away.

Capture noise and dropouts are keyed by section and output position, so they match between projects only when the projects' timelines match. The shared noise layer, and dropouts with [`anchor: disc`](#pressing-defects-unflagged-and-disc-anchored-dropouts), are keyed by disc position and match regardless.

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

### Pressing defects: unflagged and disc-anchored dropouts

Each dropout type takes two more keys, `flagged` and `anchor`:

```yaml
    dropouts:
      random:              # a defect in the pressing
        scale: 6
        seed: 7004
        anchor: disc
        flagged: false
      scratch:             # damage seen by this capture only
        scale: 2
        seed: 1001
```

- `anchor: disc` makes the dropouts a property of the disc. They are keyed by the picture's disc position (the picture number on CAV) and the seed, not by section and output position. Every capture using the same seed then carries them on the same pictures, even when one capture starts later, skips or replays a range, and a replay within one capture repeats them. A disc-anchored scratch follows the section's first picture number, so a replay section with the same `start_value` repeats it. A seed is required.
- `flagged: false` applies the dropouts to the signal but leaves them out of the dropout sidecar, as a player's dropout detector misses some defects. The events are exactly those of `flagged: true`, so a test can switch the flag on to get their positions.

Unflagged, disc-anchored dropouts are the hardest case for a stacking tool. Every capture carries them, so averaging does not remove them, and nothing in the sidecar says where they are.

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

This is ground truth for the flagged dropouts. A dropout-detection or concealment tool can be scored directly against it: every flagged dropout videosynth introduced is listed, with its exact extent. Dropouts with `flagged: false` are not listed.

## Pressings and the channel

A real multi-capture set rarely comes from one disc. Two pressings of the same title carry the same programme but different pressing noise, and a glass master can record defects of its own. Two players also lock their capture clocks with a slightly different sampling phase, so one capture's samples fall a fraction of a sample away from another's. A stacking tool has to group the captures by pressing and align them in time before averaging.

The project-level `channel:` block describes the pressing and the channel it is read through:

```yaml
channel:
  timing_offset_samples: -0.3   # this pressing arrives 0.3 sample early
  edge_smear: { fraction: 0.12, tau_samples: 3.0 }
  fm_beat: { level_db: -24.0 }  # 625-line PAL only
```

- `timing_offset_samples` shifts the whole frame — sync, VBI and picture — by that many 4fsc samples; positive is later. The range is −8.0 to 8.0. Fractional offsets are a band-limited delay, accurate to well within 0.001 sample up to 6 MHz.
- `edge_smear` makes every sharp transition trail: a `fraction` (up to 0.5) of the signal passes through a low-pass with a time constant of `tau_samples` 4fsc samples. It applies to the whole frame, VITS included, so the smear can be measured from a VITS bar edge.
- `fm_beat` adds the FM-beat herringbone that some PAL LaserDisc glass masters recorded: a faint diagonal pattern in saturated flat colour, at 2.7–3.2 MHz depending on the colour's brightness. `level_db` sets it relative to the chroma of a 50 IRE colour. The beat is computed from the picture itself, so every capture of the pressing carries exactly the same beat, and stacking does not remove it. Greyscale areas carry none. It is a validation error on NTSC, PAL-M and SECAM.
- All three happen before noise is added, in the order beat, smear, offset. The capture noise is added afterwards, so its configured spectrum is exactly what the output carries. The pressing (`shared`) noise is delayed by the same timing offset as the picture, so it stays on the picture's samples; the smear does not apply to it.

Everything in `channel:` belongs to the pressing, so every project of one pressing uses the same block. A pressing is then:

| Belongs to | Keys | Same across captures of one pressing? |
|------------|------|----------------------------------------|
| The pressing | `channel:`, `noise.shared`, dropouts with `anchor: disc` | Yes |
| One capture | capture `noise_db`/`noise_spread_db`/`noise_seed`/`spectrum`, section-anchored `dropouts`, `chroma_error`, `luma_bandwidth` | No: a new seed (or player) for every capture |

The repository's [pressing sets](../misc/example-projects.md#pressings-and-captures) follow this pattern. There are two pressings per standard, offset by one whole sample, a third PAL pressing that adds the FM beat, an edge smear and a 0.15-sample offset, and a fourth PAL pressing with unflagged, disc-anchored pressing defects. Each has six derived captures, one of which starts later and one of which replays a range. No two pressings share a capture seed. See [channel](../reference/channel.md) for the full reference.

## Chroma gain and phase error

A player's chroma path is rarely perfect. A mis-set chroma gain changes saturation, a phase error changes hue, and an unsteady path makes both wander from line to line. The project-level `chroma_error:` block models this:

```yaml
chroma_error:
  gain: 1.05        # 5% too saturated
  phase_deg: 2.0    # hue advanced by 2 degrees
  line_jitter: { phase_deg: 0.5, gain: 0.01, seed: 5 }
```

- Only picture chroma changes. The burst, VBI, VITS lines and blanking are untouched, so a decoder that locks to burst sees a real hue and saturation error.
- With `include_burst: true` the burst takes the error too: a whole chroma path (or an FM/MTF loss) rather than a mis-adjusted picture path. A decoder that references its chroma gain to the burst can then measure and correct the gain, and sees no hue error against burst.
- `line_jitter` adds a fresh Gaussian deviation to phase and gain on every picture line.
- On PAL and PAL-M a constant phase error alternates in sign from line to line after decoding, which a delay-line decoder turns into a small loss of saturation; NTSC shows it as a hue shift.

The chroma error belongs to one **capture**, not the pressing: different players reading one disc each have their own, so captures of one pressing can differ in hue and saturation while their pressing's [channel](../reference/channel.md) impairments stay identical. Per-line jitter averages away across captures; a constant error does not. The pressing table in [Pressings and the channel](#pressings-and-the-channel) puts `chroma_error` with the capture keys.

The stage runs after the channel and before noise. See [chroma_error](../reference/chroma-error.md) for the full reference.

## Luma bandwidth

Players and transfers differ in how much luma detail they pass. A section's `luma_bandwidth:` block low-passes its picture luma:

```yaml
  - name: SoftPlayer
    type: progressive
    source: "{bundled}/exr/720x576/MULTIBURST.exr"
    duration_frames: 4
    luma_bandwidth:
      cutoff_mhz: 3.0        # -6 dB point
      transition_mhz: 0.4    # default
```

- The low-pass is −6 dB at `cutoff_mhz`. Over the `transition_mhz` band centred on the cutoff it falls from the passband to a stopband at least 60 dB down. It is flat (within 0.003 dB) below the band.
- Only the active picture luma is filtered. Sync, blanking, the VBI, the VITS lines and the chroma are bit-identical to an unfiltered section, and OSD text is drawn afterwards and stays sharp.
- Noise, dropouts and the channel impairments are added after the filter and keep their full bandwidth.

The [band-limited sets](../misc/example-projects.md#band-limited-luma) play a multi-burst at three bandwidths on PAL, NTSC and PAL-M. See [sections](../reference/sections.md#luma_bandwidth) for the full reference.

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

The repository's `projects/stacking/` directory contains fuller examples of this pattern. They include four-source disc-simulation sets, where the same disc is "captured" four times with differing noise seeds and disc-anchored dropouts that a replayed range repeats.
