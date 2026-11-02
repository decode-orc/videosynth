# `laserdisc`

Everything about a project that is specific to LaserDisc (LaserVision): the disc format, the LaserVision signal options, EFM digital audio and the LaserVision replay channel. Optional — a project that is not a disc has no `laserdisc:` block, and none of these settings appear anywhere else in the file.

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

## `channel`

The LaserVision pressing and replay channel: the FM-beat herringbone recorded on the master, the group-delay pre-distortion of IEC 60856, the replay amplitude response (fixed, or varying with disc position) and the misregistration of field 2 against field 1. See [LaserVision channel](laserdisc-channel.md).

## Rules

- An empty block, an unknown key, a non-string `disc_type`, a non-boolean `pilot_burst` or `vbi_burst`, or a non-integer `efm_audio.pair` is a parse error.
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
