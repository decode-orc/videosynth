# `laserdisc.channel`

The LaserVision pressing and replay channel: what the glass master, the disc and a player's replay path do to the video signal before it is captured. It belongs to the pressing, so every project that models one pressing must use the same block. Optional — omit it for an ideal disc.

```yaml
laserdisc:
  channel:
    fm_beat:
      carrier_black_hz: 7088000
      level_db: -24.0
    group_delay:
      preset: iec60856_nominal
      scale: 1.0
    response_db: [[0.0, 0.0], [3.0, -0.5], [4.4, -2.0], [5.0, -6.0], [5.5, -20.0]]
    field2_offset:
      x_samples: 0.25
      y_lines: 0.0
```

The general [`channel`](channel.md) block (timing offset and edge smear) models impairments that any capture chain has. This block holds the ones that come from LaserVision itself.

## Keys

The block must set at least one key. An empty `channel:` block, or any other key, is a parse error.

| Key | Type | Description |
|-----|------|-------------|
| `fm_beat` | map | PAL FM-beat herringbone recorded on the master; see [`fm_beat`](#fm_beat) |
| `group_delay` | map | IEC 60856 group-delay pre-distortion; see [`group_delay`](#group_delay) |
| `response_db` | list | Replay amplitude response; see [`response_db`](#response_db) |
| `response_by_picture` | list | Replay response that changes across the disc; see [`response_by_picture`](#response_by_picture) |
| `field2_offset` | map | Misregistration of field 2; see [`field2_offset`](#field2_offset) |

`response_db` and `response_by_picture` cannot both be set.

## Stage order

The channel stage runs once per frame, between generation and the chroma error:

1. Generation, with [`field2_offset`](#field2_offset) applied to the source picture
2. **Channel**: `fm_beat`, then `group_delay` with `response_db` / `response_by_picture` (one filter), then the general [`edge_smear`](channel.md#edge_smear), then [`timing_offset_samples`](channel.md#timing_offset_samples)
3. [Chroma error](chroma-error.md)
4. [Line timing](line-timing.md)
5. Noise (capture layer, then shared layer)
6. Dropouts
7. Output

The beat is on the disc, so the replay filter, the readout smear and the sampling offset all apply to it. The [picture truth](output.md#truth) includes `field2_offset`, which changes the picture that is modulated, and excludes every other key here.

## `fm_beat`

Some PAL LaserDisc glass masters recorded a beat between the FM video carrier and the colour signal. It shows as a faint herringbone in saturated flat colour, and because it is on the master, every capture of the pressing carries it and stacking does not remove it.

| Key | Type | Required | Range | Description |
|-----|------|----------|-------|-------------|
| `carrier_black_hz` | float | No (default 7 088 000) | 6 900 000 to 7 300 000 | FM video carrier at black, in Hz |
| `level_db` | float | Yes | −60.0 to 0.0 | Beat level relative to the local chroma amplitude, in dB, on a 50 IRE colour |

The beat is predicted from the frame's own signal, following the reference model (`_beat_basis` in [domesday-aiv-restore `deherring.py`](https://github.com/dp111/domesday-aiv-restore/blob/main/newdecode/deherring.py), measured on the EBU bars of PAL pressings):

1. The composite is converted to IRE and pre-emphasised (400/100 ns, as ld-decode's PAL de-emphasis inverts).
2. The instantaneous FM carrier is `carrier_black_hz` + 8 kHz per IRE of the pre-emphasised composite, with part of the chroma's own phase modulation removed.
3. The beat is the carrier phase mixed with the picture chroma: a tone at carrier − subcarrier, about 2.72 MHz on blue and 3.19 MHz on yellow. Its phase follows the carrier phase from the start of the frame, and its amplitude follows the chroma and falls with brightness.
4. It is de-emphasised and band-passed to 2.2–3.9 MHz.

- The beat is added to **Y** only, inside the active window of active picture lines. Burst, VBI, VITS lines and C are untouched.
- `level_db` scales it so that on a flat 50 IRE colour the beat amplitude is `level_db` below that colour's chroma amplitude. Brighter colours carry a weaker beat and darker ones a stronger one, as the reference model's amplitude law describes.
- The beat depends only on the frame's content. It is computed before the [chroma error](chroma-error.md) and the noise, so captures of one pressing carry exactly the same beat.
- Picture areas without chroma carry no beat at all.
- **625-line PAL only**: the model is fitted to PAL LaserDisc pressings.

## `group_delay`

LaserVision video is recorded with its group delay deliberately pre-distorted, and the player's replay low-pass filter is meant to cancel it (IEC 60856 §9.1.6). An RF capture decoded in software has no such filter, so some of the pre-distortion may still be in the signal. This key adds it back: an all-pass element whose group delay follows a curve.

| Key | Type | Required | Range | Description |
|-----|------|----------|-------|-------------|
| `preset` | string | One of `preset` or `points_ns` | `iec60856_nominal` | A built-in curve |
| `points_ns` | list of [MHz, ns] pairs | One of `preset` or `points_ns` | 0 MHz to Nyquist, strictly ascending; −1000 to 1000 ns | A custom curve |
| `scale` | float | No (default 1.0) | −2.0 to 2.0 | Fraction of the curve left in the signal |

`iec60856_nominal` is the nominal pre-distortion of IEC 60856 §9.1.6, relative to 0.5 MHz:

| f (MHz) | 0.5 | 2.0 | 3.0 | 4.0 | 4.43 | 4.8 |
|---|---|---|---|---|---|---|
| t_d (ns) | 0 | −10 | −35 | −85 | −135 | −200 |

−135 ns is about 2.4 samples at 4fsc. The preset is the 625-line PAL curve, so it is a validation error on NTSC or PAL-M; give a `points_ns` curve for those.

- Breakpoints are joined by straight lines, and the end values are held beyond them. Negative delays arrive early.
- `scale` multiplies the whole curve: 1.0 leaves all of the recorded pre-distortion in the signal, 0.5 half of it, and −1.0 models a player whose replay filter has been applied to a signal that never carried it.
- Delays are relative: low frequencies keep their timing, so sync and the picture's luma stay where they were while the chroma band moves.
- `scale: 0` or an all-zero curve is the identity, and the output is bit-identical to leaving `group_delay` out.

## `response_db`

The replay amplitude response, as a list of `[frequency_mhz, relative_db]` breakpoints in the same syntax as the noise [`spectrum`](section-noise.md). A player's replay response rolls off at about 5 MHz, which cuts the upper chroma sideband while the lower one stays intact.

```yaml
    response_db: [[0.0, 0.0], [3.0, -0.5], [4.4, -2.0], [5.0, -6.0], [5.5, -20.0]]
```

- Breakpoints are joined by straight lines in dB against MHz, and the end values are held beyond them.
- Frequencies run from 0 MHz to the 4fsc Nyquist frequency (8.87 MHz on 625-line, 7.16 MHz on 525-line) and must be strictly ascending. Levels lie between −60 dB and +20 dB.
- The response is linear-phase with no delay, so it is independent of `group_delay`.
- The DC gain is exactly the response at 0 MHz, so with 0 dB there flat levels — blanking, sync tip, flat grey — pass unchanged.
- A table that is 0 dB everywhere is the identity, and the output is bit-identical to leaving it out.

## `response_by_picture`

On a CAV disc the inner tracks lose high frequencies, so the replay response changes with disc position. This key gives the response at two or more positions:

```yaml
    response_by_picture:
      - {picture: 1,     response_db: [[0, 0], [4.4, -1.0], [5.0, -4.0]]}
      - {picture: 54000, response_db: [[0, 0], [4.4, -3.0], [5.0, -9.0]]}
```

| Key | Type | Required | Range |
|-----|------|----------|-------|
| `picture` | int | Yes | 1 and above, strictly ascending across the entries |
| `response_db` | list of [MHz, dB] pairs | Yes | As [`response_db`](#response_db) |

- At least two entries are needed; use `response_db` for a single response.
- Each frame takes the response at its own picture number, interpolated linearly in picture number between the entries that bracket it, frequency by frequency in dB. Pictures before the first entry or after the last take that entry's response.
- A frame's picture number is its disc position: the CAV picture number when its section anchors a `picture_number` code with `start_value`, otherwise its 1-based position in the output (in a `"2:3"` section, the disc frame its picture falls on in a continuous film cadence) — the same key the [shared noise layer](section-noise.md#shared-noise-and-disc-position) uses. Every capture of one pressing therefore sees the same response on the same picture.

## `field2_offset`

On still content, field 2 may not register exactly against field 1. A decoder that separates chroma across the two fields of a still frame depends on that registration. This key misregisters field 2 in the **picture domain**: field 2 reads its rows from an offset copy of its source picture, before encoding, so its chroma stays correctly modulated.

| Key | Type | Required | Range | Description |
|-----|------|----------|-------|-------------|
| `x_samples` | float | One of the two | −8.0 to 8.0 | Horizontal offset in 4fsc samples; positive moves field 2 right |
| `y_lines` | float | One of the two | −4.0 to 4.0 | Vertical offset in frame lines (picture rows); positive moves field 2 down |

- `x_samples` is converted to source pixels across the active window (922 samples for 720 pixels on PAL, 745 for 720 on NTSC and PAL-M).
- A whole-number offset is an exact shift of the picture. A fractional offset is a separable 16-tap Kaiser-windowed-sinc interpolation (β = 8), normalised to unit DC gain so flat areas keep their values. Positions beyond the picture take its nearest edge, and results are rounded to the nearest source code.
- `y_lines: 2` moves field 2 down by one of its own lines.
- Field 1 is untouched. Zero offsets are the identity, and the output is bit-identical to leaving the key out.
- The [picture truth](output.md#truth) carries the offset, because it is what was modulated.
- On a [2:3 pulldown](sections.md#cadence) section, field 2's own film frame is offset.

## Common rules

- The filter of `group_delay` and `response_db` / `response_by_picture` is one 255-tap FIR designed by frequency sampling with a Hann window. Over the IEC table its group delay is within 3 ns of the curve, and its amplitude is within 0.1 dB of the response table away from the breakpoints (a breakpoint's corner is rounded slightly).
- It filters Y and C over the whole frame — sync, burst, VBI and VITS included — so a decoder or an equaliser can measure the channel from the VITS as it would on a real disc. Each frame is treated as circular. The frame boundary lies in vertical blanking, so the wrapped samples are sync and equalising pulses only.
- Y is clamped to the same legal range as noise injection. C is centred on zero and is left for the output stage to quantise.
- The noise layers are added after the channel, so they keep their configured spectrum: the replay response does not filter the shared (pressing) noise layer.
- The general [`edge_smear`](channel.md#edge_smear) models a similar readout tail as a one-pole low-pass. Part of a measured edge tail may be `group_delay`; keeping both lets either model be fitted to the VITS.

## Rules

- Values outside their ranges, or not finite, are validation errors.
- An empty block, an unknown key, a non-numeric value, a `group_delay` with both or neither of `preset` and `points_ns`, an unknown `preset`, a `[frequency, value]` entry that is not a numeric pair, or a `response_by_picture` entry without both keys is a parse error.
- `fm_beat` on any standard other than 625-line PAL is a validation error, as is `group_delay.preset: iec60856_nominal`.
- `response_db` together with `response_by_picture`, a `response_by_picture` with fewer than two entries, or pictures that do not ascend is a validation error.
- `fm_beat` without `level_db` is a parse error.
