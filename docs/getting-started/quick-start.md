# Quick Start

This walkthrough builds a complete **PAL CAV laserdisc** from nothing: a lead-in, a programme area and a lead-out, carrying IEC 60856-compliant biphase VBI metadata and a pair of insertion test signals on the laserdisc VITS lines. At the end you will have a `.cvbs` sample file that a laserdisc-aware decoder will read as a real disc capture.

You can follow it in the GUI, on the command line, or both — the project file is the same either way.

## What you are going to build

| Section | Type | Frames | Carries |
|---------|------|--------|---------|
| `LeadIn` | `lead_in` | 938 | `lead_in` biphase code (`88FFFF`) |
| `Programme` | `programme_area` | 250 | Picture numbers 1–250, chapter 1, programme status, OSD burn-in |
| `LeadOut` | `lead_out` | 1250 | `lead_out` biphase code (`80EEEE`) |

Project-wide: CAV addressing, the PAL 3.75 MHz pilot burst on every sync pulse, and the UK national ITS plus the ITS-2 chrominance reference on the laserdisc VITS lines.

The 938- and 1250-frame lead-in and lead-out durations are not arbitrary — IEC 60856 requires at least 1.5 mm of track before the programme and 2 mm after it, which at the 1.6 µm track pitch works out at those frame counts. On a CAV disc the validator warns about a shorter lead-in or lead-out.

Total output is 2438 frames — about 3.5 GB of samples, generated in a few seconds on a modern machine.

---

## Part 1 — Create the project in the GUI

### 1. Start the application and create a project

Launch `videosynth-gui`. It opens on the welcome page — a project must be open before there is anything to edit. Click **New Project…**.

The **New Project** dialog is the project settings editor, so everything project-wide is set here before the project exists.

In the **Project** group:

- **Name**: `PalCavQuickStart`
- **Version**: `1.0`
- **Description**: `PAL CAV laserdisc with VITS and IEC 60856 biphase metadata`

In the **CVBS Presets** group:

- **Video standard**: `PAL`.
- **Sample encoding**: `CVBS_S16_4FSC`. This is a *signed* preset, which matters here: the laserdisc pilot burst swings to −600 mV, below the −300 mV floor the unsigned `CVBS_U10_4FSC` preset can represent. With an unsigned preset the burst trough is clipped, and videosynth warns you about it.
- **Signal state**: `STANDARD_STABLE_LOCKED`.
- Tick **PAL laserdisc pilot burst (IEC 60856 §9.1.2)**. The tick box is only enabled for PAL projects. The NTSC VBI burst option beside it is enabled only for NTSC, but it is not implemented yet: ticking it makes the project fail validation, so leave it clear.

!!! warning "The video standard is fixed at creation time"
    You can change the standard freely in this dialog, but once the project is created the control is disabled. Too much of the signal geometry, VBI line allocation and code validity depends on it. If you pick the wrong one, create a new project.

In the **Output** group:

- **Video path**: `{output}/quickstart_pal_cav.cvbs`. The `{output}` token means "wherever this run is told to put its media" — by default, the project's own directory.
- Leave **Signal type** as `composite`.

In the **Laserdisc & VITS** group:

- **Disc format**: `CAV`.
- **VITS placement**: **Laserdisc (IEC 60856/60857)**. This tells the validator that VITS live on the laserdisc VBI lines (19, 20, 332, 333 for PAL) rather than on their broadcast lines, which on a disc are occupied by the address codes. Selecting it seeds the spec-required PAL set for you.
- In the VITS list, confirm **uk-national** and **vits20** are ticked. Under laserdisc placement their lines are set to 19/332 and 20/333.

Click **Create**. You are asked where to save the project file. Choose a directory and save it — the project is written to disk as part of creation, so `{project}`-relative paths and the preview always have something to anchor to.

A new project starts with a single progressive section sourced from the bundled colour-bar still, so it previews immediately. To reopen this dialog later, use **Project → Edit Project…**.

### 2. Build the disc structure

The **Sections** dock holds the section list, with **Add**, **Remove**, **Duplicate**, **Up** and **Down** buttons. **Add** appends a plain progressive section; you turn it into a disc section by giving it a disc section type in the section editor.

Add two more sections so there are three, then arrange them as `LeadIn`, `Programme`, `LeadOut` using **Up** and **Down**.

!!! note "Section order is validated"
    Once any section is a lead-in or lead-out, the sequence must be `[lead_in] programme_area… [lead_out]`: at most one lead-in and it must come first, at most one lead-out and it must come last. Out-of-order sections would break monotonic picture-number generation, so this is an error rather than a warning.

### 3. Configure the programme section

Select the middle section in the sections list.

**Section**

- **Name**: `Programme`.
- **Disc section type**: `programme_area`.

Setting the disc section type is what brings the laserdisc code editor to life: it now knows which code types IEC 60856 permits here, and pre-ticks the ones a CAV programme section normally carries.

**Section Source**

- Leave **Built-in asset** selected and choose `100_BARS.exr` from the file dropdown. The raster (`720x576`) is derived from the project standard and is never a field you set. The **Source Profile** box reports whether the file passes the profile checks.
- Set **Duration** to `250` frames. The editor shows the equivalent time beside it — 10 seconds at 25 fps.

**Line injections**

This editor is a checklist, not an add/remove list: you tick the codes this section carries, and only codes the validator accepts for this disc format, section type and standard are offered. Each has a one-line explanation of what it does at generation time. Tick:

- **picture_number**, and set its **start value** to `1`.
- **chapter_number**, and set **chapter** to `1`.
- **programme_status**. Click the button beside it to open the **Programme Status Code** dialog, which composes the 24-bit word from readable fields — CX noise reduction, disc size, disc side, teletext presence, copy permission and audio/video mode — and computes the Hamming check digit. Leave the defaults, which produce `0x8DC000`.

There is no line selection anywhere in this editor. Laserdisc VBI line placement is entirely determined by IEC 60856 from the code type, the disc format and which field opens the picture; videosynth works it out per frame.

**On-screen display**

Add an overlay so you can see the picture number burnt into the picture:

- Click **Add overlay**.
- **Text**: `PN:{picture_number}  {section_name}`
- **x**: `16`, **y**: `32`, **Scale**: `2`
- **Fg luma**: `White`, **Bg luma**: `Black`

The available tokens are listed beneath the text field. `{picture_number}` resolves to the CAV picture number the biphase encoder actually wrote into the VBI of this frame, so the burn-in and the VBI code cannot disagree.

### 4. Configure the lead-in and lead-out

Select the first section:

- **Name**: `LeadIn`, **Disc section type**: `lead_in`.
- **Duration**: `938` frames — the IEC 60856 minimum (1.5 mm of track at the 1.6 µm pitch). A shorter lead-in produces a validation warning.
- Source: `PLUGE.exr`, so the lead-in is visually distinct.
- In the line injections checklist, leave the `lead_in` code ticked. The only other code offered is `users_code`, which this disc does not need.

Then the last section:

- **Name**: `LeadOut`, **Disc section type**: `lead_out`, **Duration**: `1250` frames (2 mm of track).
- Source: `PLUGE.exr`, code `lead_out` ticked.

### 5. Preview

The **Preview** dock on the right synthesises the selected section's frames on demand; the **Line Scope** dock beneath it shows one line of that frame as a waveform. If either is hidden, bring it back from **View → Panels**.

- Switch to the **Encoded** tab to see the woven full-frame composite raster, including the VBI lines carrying the biphase codes.
- Drag the crosshair to a VBI line — 17 or 18 for the picture number (field 1), 330 or 331 for the chapter (field 2), 16 for the programme status — and the **Line Scope** shows the actual waveform for that line, with sample position, time in microseconds and level in millivolts under the cursor.
- Set the scope's vertical **Range** to **Sub-sync** to see the pilot burst excursions below sync.

### 6. Validate and generate

Validation runs continuously in the background; the **Issues** dock lists anything wrong and double-clicking an issue jumps to the section it relates to. Run it explicitly with **Project → Validate**.

When the project is clean, choose **Generate → Generate** (or ++ctrl+g++). The status bar shows the current pipeline stage and the progress bar counts frames. When it finishes, a summary dialog lists the artefacts and offers to open the containing folder.

---

## Part 2 — The same project as a YAML file

Everything the GUI just did is stored in a plain YAML file that the CLI reads unchanged. Here it is in full — you can save it as `quickstart.yaml` and skip Part 1 entirely.

```yaml
project:
  name: PalCavQuickStart
  version: "1.0"
  description: PAL CAV laserdisc with VITS and IEC 60856 biphase metadata

cvbs_presets:
  video_standard_preset: PAL
  sample_encoding_preset: CVBS_S16_4FSC
  signal_state_preset: STANDARD_STABLE_LOCKED
  pal_laserdisc_pilot_burst: true

output:
  video_path: "{output}/quickstart_pal_cav.cvbs"

line_injections:
  disc_type: CAV
  placement: laserdisc
  vits:
    - vits_type: uk-national
      target_lines: [19, 332]
    - vits_type: vits20
      target_lines: [20, 333]

sections:
  - name: LeadIn
    type: progressive
    source: "{bundled}/exr/720x576/PLUGE.exr"
    duration_frames: 938
    section_type: lead_in
    line_injections:
      - type: laserdisc
        codes:
          - code_type: lead_in

  - name: Programme
    type: progressive
    source: "{bundled}/exr/720x576/100_BARS.exr"
    duration_frames: 250
    section_type: programme_area
    line_injections:
      - type: laserdisc
        codes:
          - code_type: picture_number
            start_value: 1
          - code_type: chapter_number
            chapter: 1
          - code_type: programme_status
            programme_status: "0x8DC000"
    osd:
      overlays:
        - text: "PN:{picture_number}  {section_name}"
          x: 16
          y: 32
          scale: 2
          fg_luma: white
          bg_luma: black

  - name: LeadOut
    type: progressive
    source: "{bundled}/exr/720x576/PLUGE.exr"
    duration_frames: 1250
    section_type: lead_out
    line_injections:
      - type: laserdisc
        codes:
          - code_type: lead_out
```

### Reading the file

- **`cvbs_presets`** fixes the signal for the whole project: one standard, one sample encoding, one signal state. There is no per-section override, because a capture file has one format.
- **`line_injections`** at the top level is *project-wide*: the disc format and the VITS set that every frame of every section carries. A disc is entirely CAV or entirely CLV, and its reference signals do not change between lead-in and programme, so these are declared once.
- **`line_injections`** inside a section is *per-section*: the biphase codes that legitimately differ between disc areas. Note that no section names a VBI line — `target_lines` is rejected outright on a `laserdisc` injection.
- **`section_type`** is what makes this a disc rather than a plain three-part video. It selects which code types are legal in each section and triggers the minimum-duration checks.

## Part 3 — Validate and generate from the command line

Validate first. This parses the file, checks every rule, probes the source files, and generates nothing:

```bash
videosynth --project quickstart.yaml --validate
```

```text
[info] Starting pipeline: parse -> validate -> generate -> output
[info] Parsing project file: quickstart.yaml
[info] Validation successful.
```

Then generate, sending the media somewhere with room for it:

```bash
videosynth --project quickstart.yaml --output-root /path/to/scratch
```

```text
[info] Pipeline progress: 2438/2438 frame(s) written.
[info] Wrote 2438 frame(s) to output files.
[info] Generation completed successfully.
```

`--output-root` is what `{output}` resolves to. Without it, `{output}` is the project file's own directory. This separation is deliberate: the project says *what* its artefacts are called, and the caller decides *where* this particular run puts them.

## Part 4 — What you got

```text
quickstart_pal_cav.cvbs     3.5 GB   composite samples, 2438 frames
quickstart_pal_cav.meta      12 KB   SQLite metadata sidecar
```

The `.cvbs` file is a flat stream of 4fsc composite samples — 1135 samples per line, 625 lines per frame for PAL. The `.meta` sidecar is a SQLite database describing the file as a whole: standard, sample encoding, signal state, frame count, black level and the build that produced it. Both are the input format that decode-orc and the ld-decode tool chain expect.

Because generation is deterministic, running the same project again — on a different machine, with a different thread count — produces byte-identical files.

## Where to go next

- Swap `disc_type: CAV` for `CLV` and replace `picture_number` with `programme_time_code`, `clv_code` and `clv_picture_number` — see [Laserdisc discs](../user-manual/laserdisc.md).
- Build the NTSC equivalent. A compliant NTSC disc also carries the 40-bit FM codes on lines 10/273 with the white flag on line 11, and the validator requires a `virs` VITS (conventionally on lines 19/282).
- Add [noise and dropouts](../user-manual/impairments.md) to make the disc look like a real, imperfect capture.
- Add [audio](../user-manual/audio.md) — analogue channel pairs, or EFM digital audio with a proper table of contents.
- Read the [Project File Reference](../reference/overview.md) for the exact meaning of every key.
