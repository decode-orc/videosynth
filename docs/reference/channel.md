# `channel`

Project-wide impairments of the **pressing**: properties of the disc and the channel it is read through, rather than of one capture. Optional — omit the block for an ideal channel.

```yaml
channel:
  timing_offset_samples: -0.3
  edge_smear:
    fraction: 0.12
    tau_samples: 3.0
```

These two apply to any standard and any capture chain. The impairments that come from LaserVision itself — the FM-beat herringbone, the IEC 60856 group-delay pre-distortion, the replay response and the field-2 misregistration — are in the [LaserVision channel](laserdisc-channel.md), `laserdisc.channel`.

Every project that models one pressing must use the same `channel:` block. The [capture noise layer](section-noise.md), the [chroma error](chroma-error.md) and the dropouts describe one capture and can differ between projects; the channel cannot.

## Keys

The block must set at least one of `timing_offset_samples` and `edge_smear`. An empty `channel:` block, or any other key, is a parse error; `fm_beat` here is rejected with an error naming its new place, `laserdisc.channel.fm_beat`.

| Key | Type | Required | Range | Description |
|-----|------|----------|-------|-------------|
| `timing_offset_samples` | float | No | −8.0 to 8.0 | Sub-sample timing offset in 4fsc samples. Positive values make the signal arrive later |
| `edge_smear` | map | No | — | Readout edge smear; see [`edge_smear`](#edge_smear) |

### `edge_smear` keys

| Key | Type | Required | Range | Description |
|-----|------|----------|-------|-------------|
| `fraction` | float | Yes | 0.0 to 0.5 | Fraction of the signal that is smeared |
| `tau_samples` | float | Yes | above 0, up to 100 | Time constant of the smear, in 4fsc samples |

## Behaviour

| Configuration | Result |
|---------------|--------|
| Block absent | No channel stage; output is unchanged |
| `timing_offset_samples: 0.0` only | No processing; output is bit-identical to the block being absent |
| Integer offset | An exact shift of the frame by that many samples |
| Fractional offset | A band-limited delay of the frame |
| `edge_smear.fraction: 0.0` | No smear; output is bit-identical to leaving `edge_smear` out |

## `timing_offset_samples`

The offset moves the whole sampled signal in time relative to the sampling grid — sync, VBI, VITS and picture alike — as two players would when their capture clocks lock with a slightly different phase. A stacking tool has to align captures with different offsets before it can average them.

- The integer part is an exact index shift.
- The fractional part is a 32-tap Kaiser-windowed-sinc fractional-delay filter (β = 8) normalised to unit DC gain, so flat levels pass unchanged. Up to 6 MHz its response is within 0.001 dB and 0.0002 sample of an ideal delay on every standard; content closer to Nyquist is progressively attenuated.
- Each frame is shifted circularly within itself. The frame boundary lies in vertical blanking, so the samples that wrap are sync and equalising pulses only.

## `edge_smear`

A fraction of the signal passes through a one-pole low-pass, as a player's readout trails each sharp transition:

H(z) = (1 − c) + c · (1 − a) / (1 − a z⁻¹),  with c = `fraction` and a = exp(−1 / `tau_samples`)

- After a step, the output reaches 1 − c·aⁿ⁺¹ at sample n. Flat levels pass unchanged (the DC gain is 1), and the filter never overshoots.
- It applies to Y and C over the whole frame, VITS included, so a stacking tool can measure the smear from a VITS bar edge.
- Each frame is treated as circular: the filter state is primed over the frame's own last ⌈20·`tau_samples`⌉ samples, so the result depends on that frame alone and is the same whatever order frames are rendered in.

## Common rules

- The smear and the delay are applied in that order, after the [LaserVision channel](laserdisc-channel.md#stage-order): the FM beat and the replay filter belong to the disc and the player's replay path, so the readout smear and the sampling offset apply to them.
- Each step rounds to the internal fixed-point step. Y is clamped to the same legal range as noise injection; C is centred on zero and is left for the output stage to quantise, so saturated chroma is not clipped.
- Both run on the Y and C signals after generation and before noise, so both [noise layers](section-noise.md) are added afterwards and keep the configured spectrum. The shared (pressing) layer is then delayed by `timing_offset_samples`, as the picture was, so it stays on the picture's samples; the band-limited delay leaves its spectrum unchanged to within 0.001 dB up to 6 MHz. The capture layer is not delayed.

## Stage order

The channel stage runs once per frame, between generation and noise:

1. Generation
2. **Channel**: the [LaserVision channel](laserdisc-channel.md) (FM beat, then group delay and replay response), then edge smear, then timing offset
3. [Chroma error](chroma-error.md)
4. Noise (capture layer, then shared layer)
5. Dropouts
6. Output

## Rules

- `timing_offset_samples` outside −8.0 to 8.0 is a validation error.
- `edge_smear.fraction` outside 0.0 to 0.5, or `edge_smear.tau_samples` not above 0 or above 100, is a validation error.
- `edge_smear` without both of its keys is a parse error.
- A non-numeric value is a parse error.
