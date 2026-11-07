# `sections[].laserdisc`

Where a section sits on the disc, and the biphase codes it carries. Optional — a section that is not part of a disc structure has no `laserdisc:` block. A project whose sections have one must also have the top-level [`laserdisc`](laserdisc.md) block.

```yaml
    laserdisc:
      area: programme_area
      codes:
        - code_type: picture_number
          start_value: 1
        - code_type: chapter_number
          chapter: 1
        - code_type: programme_status
          programme_status: "0x8DC000"
```

## Keys

| Key | Type | Required | Description |
|-----|------|----------|-------------|
| `area` | string | No | The disc area: `lead_in`, `programme_area` or `lead_out`; see [`area`](#area) |
| `codes` | list | No | The biphase codes this section carries; see [`codes`](#codes) |
| `efm_data` | map | No | `{}` — the section's frames carry the EFM data channel; see [`efm_data`](#efm_data) |

The block must set at least one key. An empty block, or any other key, is a parse error.

## `area`

Marks the section as part of the disc structure.

| Value | Meaning | Recommended minimum (CAV) |
|-------|---------|---------------------------|
| `lead_in` | Before the programme content | 938 frames |
| `programme_area` | The content itself | — |
| `lead_out` | After the programme content | 1250 frames |

A section that carries `codes` must set it. The minimums are checked as **warnings**, only on a CAV disc, only for a section that carries codes, and only when `duration_frames` is an integer:

```text
Laserdisc injection warning: CAV lead_in section has 500 frames; IEC 1.5 mm
minimum at 1.6 um track pitch requires 938.
```

### Ordering rules

Once any section is a `lead_in` or a `lead_out`, the whole sequence is checked (a project that uses only `programme_area` is not):

- At most one `lead_in`, and no section may precede it.
- At most one `lead_out`, and no section may follow it.
- Every section between them must be `programme_area`. With only a lead-in this applies to every section after it, and with only a lead-out to every section before it.

Violations are errors, not warnings: out-of-order sections would break monotonic picture-number and time-code generation (IEC 60856/60857).

`area` also drives the [EFM](../user-manual/audio.md#laserdisc-efm-digital-audio) disc-area mapping — a lead-in carries the table of contents, each programme-area section is one EFM track, and the lead-out carries lead-out subcode. That mapping works whether or not the section carries codes, which is why an EFM-only project can have a short lead-in without the 938-frame warning a biphase CAV lead-in would get.

## `efm_data`

```yaml
    laserdisc:
      area: programme_area
      efm_data: {}
```

Marks the section's frames as carrying the EFM data channel of the project's [`laserdisc.efm_data`](laserdisc.md#efm_data). The map takes no keys: which sectors a frame carries follows from the placement rule. Frames of sections without it carry no EFM signal at all.

- It is a validation error without a project-level `laserdisc.efm_data`.
- On PAL the EFM signal takes the place of the analogue sound carriers, so a section that declares `efm_data` and an `audio:` channel pair is a validation error.
- A non-map value, or any key inside the map, is a parse error.

## `codes`

| Key | Type | Applies to | Description |
|-----|------|------------|-------------|
| `code_type` | string | All | Which code — see [laserdisc code types](laserdisc-codes.md) |
| `start_value` | int | `picture_number`, `fm_picture_number` | Anchors the count for this section (`fm_picture_number`: first section only) |
| `chapter` | int | `chapter_number` | Chapter number 0–79 |
| `programme_status` | hex string | `programme_status` | 24-bit status word, e.g. `"0x8DC000"` |
| `users_code` | hex string | `users_code` | 24-bit user's code, e.g. `"0x80D234"` |

No other key is accepted. `code_type` is required on every entry, `start_value` and `chapter` must be integers, and the hex values must be strings.

Each optional field is read only by the code types listed. A field supplied on another code type is accepted by the parser but ignored. A `programme_status` or `users_code` code without its hex value is not written.

### No line selection

There is no `target_lines` key. VBI line placement is determined entirely by IEC 60856/60857 from the code type, the disc format, and which field opens the current picture — videosynth computes it per frame. Nor is `disc_type` repeated here: it is project-wide, in the top-level [`laserdisc`](laserdisc.md#disc_type) block, and it is required as soon as any section carries codes.

### Which codes are legal here

The validator checks each `code_type` against the project's `disc_type` **and** this section's `area`. See the [compatibility matrices](../user-manual/laserdisc.md#which-codes-go-where).

In short:

```text
lead_in:         lead_in, users_code, fm_white_flag (NTSC)
programme_area:  picture_number, picture_stop, chapter_number, programme_status,
                 programme_time_code, clv_code, clv_picture_number,
                 fm_picture_number, fm_programme_time, fm_white_flag (NTSC)
lead_out:        lead_out, users_code, fm_white_flag (NTSC)
```

### Continuity across sections

`picture_number`, `programme_time_code` and `clv_picture_number` run continuously across section boundaries by design (IEC 60856/60857 §10.1.5 requires it). The CLV clocks have nothing to configure at all.

For `picture_number`:

- **Omit `start_value`** on a section to continue from the previous one. If no earlier section set it, numbering begins at 1.
- **Set `start_value`** to re-anchor the count at that section — a deliberate discontinuity, for modelling a player skip or a replayed passage.

The same pattern applies to `chapter`: omit it to continue the previous chapter (carrying its stop-bit track counter across the boundary), set it to start a new one.

Setting `start_value: 1` on every chapter would restart the numbering at each boundary, which is almost never what you want.

## Examples

**PAL CAV programme section**

```yaml
    laserdisc:
      area: programme_area
      codes:
        - code_type: picture_number
          start_value: 1
        - code_type: picture_stop
        - code_type: chapter_number
          chapter: 1
        - code_type: programme_status
          programme_status: "0x8DC000"
```

**NTSC CLV programme section** — the 40-bit FM codes are mandatory alongside the 24-bit ones

```yaml
    laserdisc:
      area: programme_area
      codes:
        - code_type: programme_time_code
        - code_type: clv_code
        - code_type: clv_picture_number
        - code_type: chapter_number
          chapter: 1
        - code_type: programme_status
          programme_status: "0x8DC000"
        - code_type: fm_programme_time
        - code_type: fm_white_flag
```

**Lead-in with a user's code**

```yaml
    laserdisc:
      area: lead_in
      codes:
        - code_type: lead_in
        - code_type: users_code
          users_code: "0x80D234"
```

**EFM-only lead-in** — no biphase codes, just the disc area

```yaml
    laserdisc:
      area: lead_in
```

## `vitc` line injections

A section's separate `line_injections:` list carries only `vitc`, which is **not implemented** — using it is a validation error:

```text
Project configuration error: line injection type 'vitc' is not implemented in
the current runtime.
```

A `vitc` injection still needs a non-empty `target_lines`, checked first, and cannot share a section with laserdisc `codes`. A `line_injections` entry with `type: laserdisc` is a parse error naming this block:

```text
line_injections[] type 'laserdisc' has moved: laserdisc codes go in the
section's laserdisc.codes list.
```
