# The GUI

`videosynth-gui` is a Qt 6 desktop application for authoring projects, previewing frames and running generation. It reads and writes the same YAML project files as the CLI, so a project can move between the two freely.

```bash
videosynth-gui
```

It accepts `--log-level` and `--log-file`, which override the persisted preferences for that session only.

## The window

The application opens on a **welcome page** with **New Project…**, **Open Project…** and a recent-projects list. There is no empty editor: a project must exist before there is anything to edit.

Once a project is open the window is laid out as:

| Area | Contents |
|------|----------|
| Centre | The **section editor** — everything about the currently selected section |
| Left dock | **Sections** — the ordered section list |
| Right dock | **Preview** — the picture view and frame navigator |
| Right dock (below) | **Line Scope** — the waveform of one line of the previewed frame |
| Bottom dock | **Issues** — live validation results |
| Bottom dock | **Log** — messages from a generation run (hidden by default) |

Every dock can be hidden, floated or rearranged; **View → Panels** toggles them and the menu always reflects the current layout. The window's size and position are remembered between sessions; the dock arrangement is not.

## Menus

### File

| Item | Shortcut | Effect |
|------|----------|--------|
| New Project… | ++ctrl+n++ | Opens the New Project dialog |
| Open Project… | ++ctrl+o++ | Opens an existing `.yaml`/`.yml` project |
| Open Recent | — | Recently opened projects |
| Save | ++ctrl+s++ | Writes the project back to its file |
| Save As… | ++ctrl+shift+s++ | Writes it to a new path, which becomes the current file |
| Exit | ++ctrl+q++ | Prompts if there are unsaved changes, or if a run is in progress |

### Edit

**Undo** (++ctrl+z++) and **Redo** (++ctrl+shift+z++) cover every project mutation, and name the action they will reverse — *Undo Remove sections*, *Redo Edit section*. Multi-section operations undo as one step. **Preferences…** is described below.

### Project

- **Edit Project…** reopens the project settings editor.
- **Validate** runs validation on demand and reports the result in a dialog. Validation also runs continuously in the background, so this is mostly for confirmation.

### Generate

- **Generate** (++ctrl+g++) starts a generation run.
- **Cancel Generation** stops one in progress. A cancelled run leaves no partially-written files.

### View

- **Panels** — show/hide each dock.
- **Theme** — **Auto**, **Light** or **Dark**. Auto follows the desktop's colour scheme. The choice is persisted.

### Help

**About** shows the build version — the git commit the binary was built from.

## Creating a project

**New Project…** opens the full project settings editor in a dialog, so all project-wide settings are decided before the project exists. Its groups, top to bottom:

- **Project** — name, version, description.
- **CVBS Presets** — video standard, sample encoding, signal state, and the standard-specific options (NTSC black setup override, SECAM field identification). The signal state follows the standard: `STANDARD_STABLE_UNLOCKED` for [SECAM](video-signal.md#secam-chroma), `STANDARD_STABLE_LOCKED` otherwise. Options that do not apply to the selected standard are disabled rather than hidden.
- **Output** — video path and signal type, and **Picture truth**, which writes the [component picture](../reference/output.md#truth) beside the video (not offered for SECAM).
- **VITS** — the VITS **Placement** policy (*Standard (broadcast lines)*, *Laserdisc (IEC 60856/60857)* or *Custom (choose lines)*) and the VITS checklist. SECAM has no laserdisc placement. See [VITS](vits.md).
- **LaserDisc** — the [LaserDisc settings](#laserdisc), for disc projects only.
- **Channel (pressing)** — the [`channel:` block](../reference/channel.md): a sub-sample **timing offset**, plus optional **Edge smear** (fraction and time constant). These describe the pressing, so every project of one pressing should use the same values.
- **Chroma error (capture)** — the [`chroma_error:` block](../reference/chroma-error.md): chroma **gain** and **phase**, **Include burst**, plus optional **Per-line jitter** (phase and gain deviations, with an optional fixed seed), **Differential gain and phase** (gain and phase curves against luma level, in IRE) and **Crosstalk** (V into U, U into V, and **Alternate with PAL V-switch**, which NTSC cannot carry). It is not available for SECAM.

Ticking a group that was off fills in typical values, which you can then edit. Changing the standard in the New Project dialog clears the settings the new standard cannot carry: the picture truth and chroma error on SECAM, and the LaserDisc settings described below.

### LaserDisc

Everything specific to LaserDisc is in one checkable **LaserDisc** group in the project settings, after **VITS** — the project's [`laserdisc:` block](../reference/laserdisc.md). Leave it unticked for a project that is not a disc: its contents stay hidden, and sections show no LaserDisc group either. SECAM has no LaserVision format, so the group is disabled for SECAM projects. Ticked, it holds:

- **Disc format** — *(none)*, *CAV* or *CLV*. Sections need a disc format before they can carry laserdisc codes.
- **PAL pilot burst (IEC 60856 §9.1.2)** and **NTSC VBI burst (IEC 60857 §9.1.2)** — each enabled only on its own standard. See [LaserDisc signal options](video-signal.md#laserdisc-signal-options).
- **Digital audio** — **Encode one channel pair as EFM**, with the **Pair** to encode. See [Audio](audio.md#laserdisc-efm-digital-audio).
- **LaserVision channel (pressing)** — a checkable sub-group for the [LaserVision channel](../reference/laserdisc-channel.md), itself made of optional groups:
    - **FM beat (herringbone)** — carrier at black and level. PAL only.
    - **Group delay** — **Curve** *IEC 60856 nominal* (PAL only) or *Custom curve* (a frequency/delay table), and its **Scale**.
    - **Replay response** — **Response** *None*, *Fixed response* (a frequency/dB table) or *By disc position* (a response per picture number).
    - **Field-2 offset** — horizontal (samples) and vertical (lines).

Unticking the **LaserDisc** group clears every section's disc area and codes in the same undo step. Changing the standard clears the parts the new one cannot carry: the whole group on SECAM, the pilot burst, FM beat and nominal group delay off PAL, and the VBI burst off NTSC.

Clicking **Create** asks where to save the project. The file is written as part of creation, so `{project}`-relative paths and the preview always have a real anchor.

!!! warning "The video standard is fixed after creation"
    The standard can be changed freely inside the New Project dialog, but the control is disabled once the project exists. Signal geometry, VBI line allocation and code validity all depend on it. Changing the standard in the dialog also remaps the seeded bundled source to the matching raster, so the new project still previews.

A new project starts with one progressive section sourced from the bundled colour-bar still for the chosen standard.

**Project → Edit Project…** reopens the same editor for an existing project.

## The Sections dock

The section list shows the sections in output order, with buttons for **Add**, **Remove**, **Duplicate**, **Up** and **Down**.

- **Add** appends a plain progressive section using the bundled colour bars for the project's raster. Its arrow opens the section templates: **Section** (the same plain section) and, for NTSC and PAL-M projects, **Film (2:3 pulldown)** — a section that plays every frame of the bundled 24000/1001 film clip, `FILM_COUNTER_24P.mkv`, with a `"2:3"` [cadence](../reference/sections.md#cadence) from phase A.
- **Duplicate** copies the selection, naming the copies *"<name> (copy)"*, and inserts them as a contiguous block after the last selected row.
- Multi-selection (++ctrl++ / ++shift++ click) puts the editor into **batch mode**: an edit made to the current section is mirrored onto every selected section. The editor says so explicitly, and the laserdisc code checklist narrows to codes valid for *all* the selected disc areas.

Selecting a section moves the preview to that section's first output frame. Scrubbing the preview into another section selects it in the list, so the editor always describes the frame on screen.

## The section editor

The centre pane is a column of groups covering everything a section holds, in the same order as the section's keys in the [project file](../reference/overview.md#canonical-key-order). Optional groups are checkable: unticked, a group's block is absent from the project.

**Section** — name, source, source frames, duration and frame range.

- **Source frames** selects the run of a video's frames the section plays: a start frame, and an end frame or **To end of source**. The fields are enabled for video sources only, and the probed frame count caps them. Moving the start past a set end moves the end with it.
- **Duration** is shown in frames with the equivalent time at the project's frame rate beside it. **All source frames** plays the selected range, with a **repeat** count and a summary such as *10 frames x 3 = 30 total*. A fixed duration longer than the range loops it, and the summary says so.
- In a 2:3 pulldown section the summary counts film frames and the output frames they fill from where the cadence starts, such as *48 film frames x 1 = 48 → 60 frames (2:3 from A1)*. A fixed duration shows the film frames it plays. For a section that continues the previous cadence after a section of unknown length, the summary says the output frames depend on where that cadence ends. A still has no length of its own, so **All source frames** is unavailable for it.
- Choosing a different video sets the duration to **All source frames** and resets the range to the whole clip. Choosing a still clears the range and replaces an **All source frames** duration with a frame count. Opening a project never rewrites a duration.
- **Frame range** is the section's span in the output, counted across the preceding sections. In a disc project the row below it gives the section's **CAV picture numbers** or **CLV timecode**.

The **Source** is a two-way choice:

- **Built-in asset** — pick an asset type (*Still image (EXR)* or *Video*) and a file name from a dropdown of the shipped assets. The raster is derived from the project standard and is not a field you set, so changing the standard self-heals the path.
- **Local file** — a browsed `.mkv`, `.mp4` or `.exr` file, stored relative to the project when **Relative to project** is ticked, absolute otherwise.

**Source Profile** — the result of probing the chosen file: whether it matches a supported profile, its raster, rate (a 24000/1001 source shows as *Film 24000/1001*), codec, pixel format and frame count, the colour conversion a video will receive (or that it is used unconverted), and any warnings such as an opposite field order. The frame rate is checked against the section's cadence: a film source fits only a 2:3 pulldown section, and changing the pattern probes the source again. Probing runs in the background; the editor shows *Probing source…* until it completes.

**Cadence** — how the section's frames map onto output fields (see [`cadence`](../reference/sections.md#cadence)):

- **Pattern** — *Video* (one frame per two fields) or *2:3 pulldown* (24000/1001 film spread over 59.94 Hz fields). The group is unavailable for PAL and SECAM projects, unless a section already uses 2:3 and needs switching back.
- **Phase** — the letter (A–D) of the 2:3 cycle the section's first field takes.
- **Continue cadence from previous section** — carries on from where the previous 2:3 section's cycle stopped. Phase is disabled while this is ticked, because the previous section decides it.

Choosing *Video* removes the `cadence` block from the project file. Choosing an EXR still returns the pattern to *Video*, since a still has no film frames.

**LaserDisc** — shown only when the project's [LaserDisc group](#laserdisc) is ticked. Ticking it for a section makes the section part of the disc — its [`laserdisc:` block](../reference/section-laserdisc.md) — and seeds a disc area with its recommended codes:

- **Disc area** — *(none)*, `lead_in`, `programme_area` or `lead_out`. It decides which codes the section may carry.
- **Codes** — the laserdisc code checklist, filtered by the project's disc format and the section's disc area. The **…** button beside a programme status code opens the **Programme Status Code** dialog.

Unticking it clears the section's disc area and codes. See [Laserdisc discs](laserdisc.md).

**Picture mapping** — **Horizontal resampling** chooses between **Nearest pixel** (the default zero-order hold) and **Band-limited** [interpolation](video-signal.md#horizontal-mapping). The **Chroma line sign** table lists the [decoder-stress regions](video-signal.md#pal-decoder-stress-regions): each row is a source rectangle (X, Y, Width, Height in pixels and rows) and how its **U** and **V** are carried (**Normal** or **Follow switch**). **Add region** places a 48×48 block after the last one; **Remove region** removes the selected row. The table is available for PAL and PAL-M projects; on other standards it stays enabled only while it holds regions, so they can be removed.

**Luma bandwidth limit** — a low-pass on the picture luma, set by its **cutoff** (the −6 dB point) and **transition width**. The cutoff's upper limit follows the standard and the transition width. See [Luma bandwidth](impairments.md#luma-bandwidth).

**Picture response** — a camera-chain softening of the picture luma. **Response** picks the **Domesday** preset (selected when the group is ticked) or **Custom curves**, which shows a horizontal (MHz, dB) and a vertical (cycles per frame row, dB) breakpoint table, seeded with the Domesday curves. See [Picture response](video-signal.md#picture-response).

**On-screen display** — the overlay list, with text, position, scale and foreground/background luma per overlay. The available substitution tokens are listed beneath the text field.

**Noise injection** — two independent layers. See [Impairments](impairments.md).

- **Capture layer** — noise floor (Black PSNR), white spread, an optional fixed seed, a **Spectrum** table and a **Spread spectrum** table. The spread spectrum shapes the white-spread (level-dependent) noise on its own; left empty, that noise follows the spectrum.
- **Shared (pressing) layer** — noise level, seed and its own **Spectrum** table. The layer is keyed by disc position, so captures that share these settings carry identical pressing noise. It always has a seed; ticking the group sets one.
    - **Level term** — an optional second pressing component: its noise level, a **Level curve** table and its own **Spectrum** table. Ticking it starts with a flat one-point curve. See [`shared.level_term`](../reference/section-noise.md#shared-level_term).

Each **Spectrum** table lists breakpoints as frequency (MHz) and level (dB). **Add point** appends a point above the last one, and **Remove point** deletes the selected one. An empty table gives white noise. The **Level curve** table works the same way, with points as luma level (mV above blanking) and gain (dB); it needs at least one point.

**Dropouts** — random (Poisson) and scratch dropouts, each with:

- a severity **Scale** and an optional fixed seed;
- an **Anchor**: *Section (this capture)*, or *Disc (pressing defect)* so every capture of the pressing carries the same events. Choosing *Disc* ticks **Fixed seed** if it was off, because a disc anchor needs one;
- **Record in dropout sidecar**. Untick it for dropouts that damage the signal but are not recorded, like dropouts a player fails to detect.

See [Pressing defects](impairments.md#pressing-defects-unflagged-and-disc-anchored-dropouts).

**Audio channel pairs** — one entry per stereo pair, with a description and a *Content* choice:

- *Test tones* — either one tone for both channels or independent left/right tones, each optionally a frequency ramp.
- *Audio file* — a WAV or FLAC file chosen with *Browse…*, stored relative to the project when *Relative to project* is ticked and the file lies under the project directory. *Repeat* restarts the file when it ends; otherwise the rest of the section is silent.
- *Video soundtrack* — one of the section video's audio streams, listed (codec, channels, sample rate) once the source has been probed. The choice is unavailable for EXR stills and for videos without audio.

Switching the content replaces the pair's settings with the new kind's defaults; undo restores them.

Every editor is a view onto the project document, so every change is undoable and every change marks the project dirty.

## The Preview and Line Scope

The preview synthesises frames on demand through the same generation code the pipeline uses, so what you see is what will be written.

**Navigator** — a frame spinner over the whole output, showing which section the current frame belongs to. In a 2:3 pulldown section it also shows each field's film frame and its place in the cycle, such as *F1: film 37 (B3) · F2: film 38 (C1)*; the Encoded view shows both fields as written, and the Source tab shows field 1's film frame. Sections that contribute no output frames say so rather than showing a stale picture.

**Source** and **Encoded** tabs:

- **Source** — the decoded source frame, before encoding.
- **Encoded** — the woven full-frame raster including the VBI. This is the default. Its **Mode** shows **Composite**, **Y** or **C**, and its **Zoom** is **Fit**, **100%** or **200%**.

**Trace** — what the Line Scope plots: **Composite**, **Y**, **C** or a **Y+C overlay**.

**Line** — the line the scope is showing. Drag the crosshair on the picture to move it, or type a line number.

The **Line Scope** plots that line's waveform with the cursor readout showing sample index, time in microseconds and level in millivolts (and IRE). Its vertical **Range** offers:

| Range | Use |
|-------|-----|
| Standard | The nominal signal excursion |
| Sub-sync | Lowers the floor so excursions below sync — such as the laserdisc pilot burst — are visible |
| Blanking detail | Zoomed around blanking, for burst and VBI code inspection |
| Fit to trace | Scales to whatever this line actually contains |

Frames are cached, so scrubbing back and forth is cheap. When a project edit invalidates the cache the status bar reports *Preview is out of date — updating…* and the last good frame stays on screen until the new one is ready, rather than blanking.

## The Issues dock

Validation runs continuously in the background, debounced so typing does not trigger a run per keystroke. The Issues dock lists every error and warning; the status bar summarises as *Project valid* or *n error(s), m warning(s)*.

Double-clicking an issue selects the section it relates to and scrolls to the group the issue names: cadence, luma bandwidth, picture response, picture mapping, the capture or shared noise layer, or random or scratch dropouts. For a project-level issue it opens **Edit Project…**, scrolled to the **Channel**, **Chroma error** or **LaserVision channel** group when the issue is about one of them.

Errors block generation. Warnings do not — they flag things that are legal but probably not what you meant, such as a scratch whose lifespan is longer than the section it lives in.

## Generating

**Generate → Generate** runs the pipeline on a worker thread, so the interface stays responsive.

If the project has unsaved changes you are offered **Save and Generate** or **Generate Without Saving** — the run uses the in-memory project either way, so an unsaved experiment can be generated without committing it to disk.

During a run the status bar shows the current pipeline stage and a progress bar counts *n / m frames*. **Cancel Generation** aborts cleanly.

On completion a summary dialog lists the artefacts that were written and offers **Open Containing Folder**. On failure it points at the **Log** dock, which carries the full message stream at the configured log level.

## Preferences

**Edit → Preferences…**, persisted between sessions:

**Generation**

- **Synthesis threads** — **Auto** (all hardware threads) or a fixed count. Output is byte-identical either way; this only trades CPU for wall-clock.

**Logging**

- **Log level** — Info, Debug or Trace.
- **Write log to file** and a log file path.

Command-line `--log-level` and `--log-file` override these for one session without changing the saved values.
