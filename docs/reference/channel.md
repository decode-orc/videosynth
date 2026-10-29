# `channel`

Project-wide impairments of the **pressing**: properties of the disc and the channel it is read through, rather than of one capture. Optional — omit the block for an ideal channel.

```yaml
channel:
  timing_offset_samples: -0.3
  edge_smear:
    fraction: 0.12
    tau_samples: 3.0
  fm_beat:                     # 625-line PAL only
    carrier_black_hz: 7088000
    level_db: -24.0
```

Every project that models one pressing must use the same `channel:` block. The [capture noise layer](section-noise.md), the [chroma error](chroma-error.md) and the dropouts describe one capture and can differ between projects; the channel cannot.

## Keys

The block must set at least one of `timing_offset_samples`, `edge_smear` and `fm_beat`. An empty `channel:` block, or any other key, is a parse error.

| Key | Type | Required | Range | Description |
|-----|------|----------|-------|-------------|
| `timing_offset_samples` | float | No | −8.0 to 8.0 | Sub-sample timing offset in 4fsc samples. Positive values make the signal arrive later |
| `edge_smear` | map | No | — | Readout edge smear; see [`edge_smear`](#edge_smear) |
| `fm_beat` | map | No | — | PAL FM-beat herringbone; see [`fm_beat`](#fm_beat) |

### `edge_smear` keys

| Key | Type | Required | Range | Description |
|-----|------|----------|-------|-------------|
| `fraction` | float | Yes | 0.0 to 0.5 | Fraction of the signal that is smeared |
| `tau_samples` | float | Yes | above 0, up to 100 | Time constant of the smear, in 4fsc samples |

### `fm_beat` keys

| Key | Type | Required | Range | Description |
|-----|------|----------|-------|-------------|
| `carrier_black_hz` | float | No (default 7 088 000) | 6 900 000 to 7 300 000 | FM video carrier at black, in Hz |
| `level_db` | float | Yes | −60.0 to 0.0 | Beat level relative to the local chroma amplitude, in dB, on a 50 IRE colour |

## Behaviour

| Configuration | Result |
|---------------|--------|
| Block absent | No channel stage; output is unchanged |
| `timing_offset_samples: 0.0` only | No processing; output is bit-identical to the block being absent |
| Integer offset | An exact shift of the frame by that many samples |
| Fractional offset | A band-limited delay of the frame |
| `edge_smear.fraction: 0.0` | No smear; output is bit-identical to leaving `edge_smear` out |
| `fm_beat` on a picture without chroma | No beat; output is bit-identical to leaving `fm_beat` out |

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

## `fm_beat`

Some PAL LaserDisc glass masters recorded a beat between the FM video carrier and the colour signal. It shows as a faint herringbone in saturated flat colour, and because it is on the master, every capture of the pressing carries it and stacking does not remove it.

The beat is predicted from the frame's own signal, following the reference model (`_beat_basis` in [domesday-aiv-restore `deherring.py`](https://github.com/dp111/domesday-aiv-restore/blob/main/newdecode/deherring.py), measured on the EBU bars of PAL pressings):

1. The composite is converted to IRE and pre-emphasised (400/100 ns, as ld-decode's PAL de-emphasis inverts).
2. The instantaneous FM carrier is `carrier_black_hz` + 8 kHz per IRE of the pre-emphasised composite, with part of the chroma's own phase modulation removed.
3. The beat is the carrier phase mixed with the picture chroma: a tone at carrier − subcarrier, about 2.72 MHz on blue and 3.19 MHz on yellow. Its phase follows the carrier phase from the start of the frame, and its amplitude follows the chroma and falls with brightness.
4. It is de-emphasised and band-passed to 2.2–3.9 MHz.

- The beat is added to **Y** only, inside the active window of active picture lines. Burst, VBI, VITS lines and C are untouched.
- `level_db` scales it so that on a flat 50 IRE colour the beat amplitude is `level_db` below that colour's chroma amplitude. Brighter colours carry a weaker beat and darker ones a stronger one, as the reference model's amplitude law describes.
- The beat depends only on the frame's content. It is computed before the [chroma error](chroma-error.md) and the noise, so captures of one pressing carry exactly the same beat.
- Picture areas without chroma carry no beat at all.

## Common rules

- The smear and the delay are applied in that order after the beat: the beat is on the disc, so the readout smear and the sampling offset apply to it.
- Each step rounds to the internal fixed-point step. Y is clamped to the same legal range as noise injection; C is centred on zero and is left for the output stage to quantise, so saturated chroma is not clipped.
- All three run on the Y and C signals after generation and before noise, so both [noise layers](section-noise.md) are added afterwards and keep the configured spectrum exactly.

## Stage order

The channel stage runs once per frame, between generation and noise:

1. Generation
2. **Channel**: FM beat, then edge smear, then timing offset
3. [Chroma error](chroma-error.md)
4. Noise (capture layer, then shared layer)
5. Dropouts
6. Output

## Rules

- `timing_offset_samples` outside −8.0 to 8.0 is a validation error.
- `edge_smear.fraction` outside 0.0 to 0.5, or `edge_smear.tau_samples` not above 0 or above 100, is a validation error.
- `fm_beat` on any standard other than 625-line PAL (NTSC, PAL-M, SECAM) is a validation error: the beat model is fitted to PAL LaserDisc pressings.
- `fm_beat.carrier_black_hz` outside 6.9–7.3 MHz, or `fm_beat.level_db` outside −60 to 0 dB, is a validation error.
- `edge_smear` without both of its keys, or `fm_beat` without `level_db`, is a parse error.
- A non-numeric value is a parse error.
