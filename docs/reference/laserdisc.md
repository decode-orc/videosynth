# `laserdisc`

Everything about a project that is specific to LaserDisc (LaserVision): the disc format, the LaserVision signal options, the EFM channel (digital audio or data) and the LaserVision replay channel. Optional — a project that is not a disc has no `laserdisc:` block, and none of these settings appear anywhere else in the file.

```yaml
laserdisc:
  disc_type: CAV
  pilot_burst: true
  efm_audio:
    pair: 0
  channel:
    fm_beat:
      level_db: -24.0
    group_delay:
      preset: iec60856_nominal
```

Each section that belongs to the disc says where it sits on it, and which biphase codes it carries, in its own [section `laserdisc` block](section-laserdisc.md).

## Keys

| Key | Type | Required | Values | Default |
|-----|------|----------|--------|---------|
| `disc_type` | string | No | `CAV`, `CLV` | absent (no disc addressing) |
| `pilot_burst` | bool | No | `true`, `false` | `false` |
| `vbi_burst` | bool | No | `true`, `false` | `false` |
| `efm_audio` | map | No | See [`efm_audio`](#efm_audio) | absent |
| `efm_data` | map | No | See [`efm_data`](#efm_data) | absent |
| `channel` | map | No | See [LaserVision channel](laserdisc-channel.md) | absent |

The block must set at least one key. An empty `laserdisc:` block, or any other key, is a parse error.

**SECAM** projects cannot have the block: there is no SECAM LaserVision format (IEC 60856 defines PAL discs only).

## `disc_type`

The disc format for the whole project. A disc is entirely one or the other, which is why it is project-wide and is not repeated on each section.

| Value | Addressing |
|-------|------------|
| `CAV` | Picture number — one frame per track |
| `CLV` | Programme time code plus CLV picture number |

It determines which `code_type` values the validator accepts, and setting it turns on the disc VITS rules (reserved address-code lines, and VIRS on NTSC/PAL-M — see [`line_injections`](line-injections.md)) whether or not any section carries codes. It is required as soon as any section carries biphase [codes](section-laserdisc.md#codes). A project with no biphase codes — an EFM-only disc, or a pilot-burst test — can leave it out.

## `pilot_burst`

Superimposes a 3.75 MHz (240 × f_H) sinusoidal burst at ±300 mV on every sync pulse in the luma channel — IEC 60856 §9.1.2.

**PAL only.** Enabling it on an NTSC or PAL-M project is a validation error.

**Warning**: with `CVBS_U10_4FSC`, `CVBS_U16_4FSC` or `CVBS_TPG21_4FSC` the burst trough is clipped at −300 mV, where the waveform reaches −600 mV. Use `CVBS_S16_4FSC` or a raw preset.

## `vbi_burst`

Keeps the colour burst on every line of the vertical interval, including the equalising and broad field-sync lines (lines 1–9 and 264–272) where standard NTSC suppresses it — IEC 60857 §9.1.2 and Figure 6a. Each burst is the normal NTSC burst: same gate position after 0H, amplitude and continuous subcarrier phase. On the broad field-sync lines the gate falls inside the broad pulse, so the burst is centred on sync tip.

**NTSC only.** Enabling it on a PAL or PAL-M project is a validation error.

**Warning**: with `CVBS_U10_4FSC`, `CVBS_U16_4FSC` or `CVBS_TPG21_4FSC` the colour burst on the broad field-sync pulses is clipped at sync tip (−40 IRE), where the waveform reaches −60 IRE. Use `CVBS_S16_4FSC` or a raw preset.

## `efm_audio`

Its presence enables LaserDisc EFM digital audio output for one channel pair. Omit the key to disable.

PAL uses the 625-line definition of IEC 60856 Amd 2 clause 13. NTSC and PAL-M use the 525-line definition of IEC 60857 Amd 2 clause 13: no LaserVision standard names PAL-M, and a PAL-M disc is a 525-line disc at the NTSC frame rate, so it takes the NTSC sample sequence and identifies as a 525-line disc in its table of contents.

| Key | Type | Required | Range |
|-----|------|----------|-------|
| `pair` | int | Yes | 0–7 |

Constraints:

| Rule | Severity |
|------|----------|
| `pair` in 0–7 | Error |
| At most 79 `programme_area` sections (one EFM track each) | Error |
| No section declares `pair` — nothing to encode, no `.efm` written | Warning |
| A `programme_area` section under 4 s (6 s for the first) | Warning |
| No `lead_in` section, so no table of contents | Warning |

The disc areas come from each section's [`laserdisc.area`](section-laserdisc.md#area): a lead-in carries the table of contents, each programme-area section is one EFM track and the lead-out carries lead-out subcode. The pair's tone parameters are declared only in the per-section [`audio:`](section-audio.md) blocks; its 48 kHz WAV track is written as normal and is unaffected. The `.efm` and `.efm.meta` files are written beside the video output — see [output](output.md#video_path) and [Audio](../user-manual/audio.md#laserdisc-efm-digital-audio).

## `efm_data`

Its presence encodes a sector image into the EFM channel, as an Acorn Interactive Video (AIV) or LV-ROM disc carries computer data in place of digital audio. The image is the user data of consecutive sectors, 2 048 bytes each: the same `.dat` that decode-orc's EFM sink writes in `data` mode. Each sector becomes an ECMA-130 Mode 1 sector (sync, BCD header, EDC, RSPC P and Q parity, Annex B scrambling), one per subcode section, 75 a second. Only the sections that declare [`laserdisc.efm_data`](section-laserdisc.md#efm_data) carry the EFM signal.

```yaml
laserdisc:
  disc_type: CAV
  efm_data:
    image: "{project}/community_north.dat"
```

| Key | Type | Required | Range | Default |
|-----|------|----------|-------|---------|
| `image` | path | Yes | A file of whole 2 048-byte sectors | — |
| `first_address` | int | No | 0–449 999 | `0` |
| `picture_offset_sectors` | int | No | 0–449 999 | `150` |
| `q_control` | string | No | `data`, `data_copy_permitted` | `data` |
| `q_mode` | int | No | `4`, `1` | `4` |

- **`image`** — the sector image. Asset roots such as `{project}` apply.
- **`first_address`** — the Sector Address (ECMA-130 §14.2) of the image's first sector. Sector *n* of the image carries address `first_address + n`, and decode-orc's sink writes each sector at the offset its address gives.
- **`picture_offset_sectors`** — header time minus picture time, in sectors. AIV players find data by picture number and EFM time together, so a sector's position against the pictures is part of the format. The default 150 (2 s) is the AIV rule: picture *p* carries addresses 3*p* + 150 to 3*p* + 152 on PAL (NTSC and PAL-M: from ⌊*p* × 75 / 29.97⌋ + 150).
- **`q_control`** — the Q-channel CONTROL nibble: `data` is 0100 (data track, copy prohibited), `data_copy_permitted` is 0110 (IEC 60908 §17.5).
- **`q_mode`** — the Q-channel ADR. `4` is the LaserDisc mode of IEC 60856/60857 Amd 2 §13.5.1.1 / §13.6.1.1. Set `1` (the compact-disc mode of IEC 60908 §17.5) to match AIV discs such as the BBC Domesday discs, which were mastered before that amendment and record ADR 1. Only the ADR nibble changes.

**Placement.** The rule is anchored on the first output frame that carries a CAV picture number (a section's `picture_number` code), or, when no frame does, on the first `programme_area` frame, taken as picture 1. From there header addresses follow the subcode timeline continuously, so address minus Q time stays constant, which a decoder's sector placement relies on. A picture number that breaks the continuous count from the anchor is reported, and its sectors keep following the timeline. A sector's 98 F1 frames enter the CIRC encoder at the start of its subcode section, with the CIRC delay (108 F1 frames, about ⅓ of a PAL frame) not pre-compensated, as for audio.

**What each subcode section carries**, in a section that carries EFM:

| Header address | Content |
|----------------|---------|
| Inside the image | The image sector |
| Before the image, in a `lead_in` section (or below 0) | A Mode 1 lead-in sector: zero user data, header MIN + A0, time since the start of the stream (ECMA-130 §14.2) |
| Before the image elsewhere, or after it | A Mode 0 sector (ECMA-130 §14.1) with its address |

Frames of sections that do not declare `efm_data` carry **no EFM signal**: their `efm_frame` rows have no t-values, and the channel stream restarts after them. Q time keeps counting across the gap. A sector whose CIRC decode window runs into such a frame (the last sector or so of each run) cannot be decoded, and the run reports it.

The subcode is one data track over the whole programme area, with the 2 s pause before track 1. The lead-in carries the table of contents, and the lead-out carries lead-out subcode, as for audio.

Constraints:

| Rule | Severity |
|------|----------|
| Together with `efm_audio` (a disc has one EFM channel) | Error |
| Video standard not PAL, NTSC or PAL-M | Error |
| `image` empty, `first_address` or `picture_offset_sectors` out of range, `q_mode` not 1 or 4 | Error |
| A PAL section declares both `efm_data` and an audio channel pair | Error |
| The image is not a whole, non-zero number of 2 048-byte sectors | Error (at generation) |
| No section declares `efm_data` | Warning |
| Image sectors fall on frames without EFM, or beyond the stream | Warning (at generation), with the count and first address |
| A left-out image sector is not blank (every byte the same value) | Warning (at generation) |
| Placed sectors cut by a frame without EFM | Warning (at generation) |
| A picture number breaks the count from the anchor | Warning (at generation) |

The `.efm` and `.efm.meta` files are written beside the video output, as for `efm_audio`. See [EFM data](../user-manual/laserdisc.md#efm-data).

## `channel`

The LaserVision pressing and replay channel: the FM-beat herringbone recorded on the master, the group-delay pre-distortion of IEC 60856, the replay amplitude response (fixed, or varying with disc position) and the misregistration of field 2 against field 1. See [LaserVision channel](laserdisc-channel.md).

## Rules

- An empty block, an unknown key, a non-string `disc_type`, a non-boolean `pilot_burst` or `vbi_burst`, a non-integer `efm_audio.pair`, an `efm_data` block without `image`, an unknown `efm_data` key, a non-integer `efm_data` number or a `q_control` other than `data` or `data_copy_permitted` is a parse error.
- `disc_type` other than `CAV` or `CLV` is a validation error, as is a section with biphase codes in a project without `disc_type`.
- A section with a `laserdisc` block in a project without one is a validation error.
- The whole block on a SECAM project is a validation error.

## LaserDisc keys in other blocks

The LaserDisc settings are accepted only in the `laserdisc` blocks. Each of these keys is a parse error in the other blocks, and the error names the key to use instead:

| Rejected key | Use instead |
|--------------|-------------|
| `cvbs_presets.pal_laserdisc_pilot_burst` | `laserdisc.pilot_burst` |
| `cvbs_presets.ntsc_laserdisc_vbi_burst` | `laserdisc.vbi_burst` |
| `output.efm_audio` | `laserdisc.efm_audio` |
| `line_injections.disc_type` | `laserdisc.disc_type` |
| `channel.fm_beat` | `laserdisc.channel.fm_beat` |
| `sections[].section_type` | `sections[].laserdisc.area` |
| `sections[].line_injections` entry with `type: laserdisc` | `sections[].laserdisc.codes` |

```text
cvbs_presets.pal_laserdisc_pilot_burst has moved to laserdisc.pilot_burst.
```

`scripts/migrate_laserdisc_keys.py` rewrites project files that use these keys in place, keeping their comments and layout:

```bash
python3 scripts/migrate_laserdisc_keys.py --check projects/*.yaml   # list what would change
python3 scripts/migrate_laserdisc_keys.py projects/*.yaml           # rewrite
```

It expects block-style YAML as the emitter writes it, and leaves alone (with a message) any file whose layout it does not recognise. Comments that mention the rejected keys are not rewritten.
