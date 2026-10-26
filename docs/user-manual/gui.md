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

Every dock can be hidden, floated or rearranged; **View → Panels** toggles them and the menu always reflects the current layout. The window layout is remembered between sessions.

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

**Undo** (++ctrl+z++) and **Redo** (++ctrl+shift+z++) cover every project mutation, and name the action they will reverse — *Undo Remove sections*, *Redo Set duration*. Multi-section operations undo as one step. **Preferences…** is described below.

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

**New Project…** opens the full project settings editor in a dialog, so all project-wide settings are decided before the project exists:

- **Project** — name, version, description.
- **CVBS Presets** — video standard, sample encoding, signal state, and the standard-specific options (PAL pilot burst, NTSC VBI burst, NTSC black setup override, SECAM field identification). The signal state follows the standard: `STANDARD_STABLE_UNLOCKED` for [SECAM](video-signal.md#secam-chroma), `STANDARD_STABLE_LOCKED` otherwise. Options that do not apply to the selected standard are disabled rather than hidden.
- **Output** — video path and signal type, plus the optional EFM digital audio channel-pair selection.
- **Laserdisc & VITS** — disc format (CAV/CLV), VITS placement policy and the VITS set. SECAM projects have no laserdisc format, so the disc format and laserdisc placement are disabled.

Clicking **Create** asks where to save the project. The file is written as part of creation, so `{project}`-relative paths and the preview always have a real anchor.

!!! warning "The video standard is fixed after creation"
    The standard can be changed freely inside the New Project dialog, but the control is disabled once the project exists. Signal geometry, VBI line allocation and code validity all depend on it. Changing the standard in the dialog also remaps the seeded bundled source to the matching raster, so the new project still previews.

A new project starts with one progressive section sourced from the bundled colour-bar still for the chosen standard.

**Project → Edit Project…** reopens the same editor for an existing project.

## The Sections dock

The section list shows the sections in output order, with buttons for **Add**, **Remove**, **Duplicate**, **Up** and **Down**.

- **Add** appends a plain progressive section using the bundled colour bars for the project's raster.
- **Duplicate** copies the selection, naming the copies *"<name> (copy)"*, and inserts them as a contiguous block after the last selected row.
- Multi-selection (++ctrl++ / ++shift++ click) puts the editor into **batch mode**: an edit made to the current section is mirrored onto every selected section. The editor says so explicitly, and the laserdisc code checklist narrows to codes valid for *all* the selected section types.

Selecting a section moves the preview to that section's first output frame. Scrubbing the preview into another section selects it in the list, so the editor always describes the frame on screen.

## The section editor

The centre pane is a set of collapsible groups covering everything a section holds.

**Section** — name, disc section type (`(none)`, `lead_in`, `programme_area`, `lead_out`), source frames, duration and frame range.

- **Source frames** selects the run of a video's frames the section plays: a start frame, and an end frame or **To end of source**. The fields are enabled for video sources only, and the probed frame count caps them. Moving the start past a set end moves the end with it.
- **Duration** is shown in frames with the equivalent time at the project's frame rate beside it. **All source frames** plays the selected range, with a **repeat** count and a summary such as *10 frames x 3 = 30 total*. A fixed duration longer than the range loops it, and the summary says so. A still has no length of its own, so **All source frames** is unavailable for it.
- Choosing a different video sets the duration to **All source frames** and resets the range to the whole clip. Choosing a still clears the range and replaces an **All source frames** duration with a frame count. Opening a project never rewrites a duration.
- **Frame range** is the section's span in the output, counted across the preceding sections.

**Section Source** — a two-way choice:

- **Built-in asset** — pick an asset type (*Still image (EXR)* or *Video*) and a file name from a dropdown of the shipped assets. The raster is derived from the project standard and is not a field you set, so changing the standard self-heals the path.
- **Local file** — a browsed `.mkv`, `.mp4` or `.exr` file, stored relative to the project when **Relative to project** is ticked, absolute otherwise.

**Source Profile** — the result of probing the chosen file: whether it matches a supported profile, its raster, rate, codec, pixel format and frame count, the colour conversion a video will receive (or that it is used unconverted), and any warnings such as an opposite field order. Probing runs in the background; the editor shows *Probing source…* until it completes.

**Line injections** — the laserdisc code checklist for this section. See [Laserdisc discs](laserdisc.md).

**Noise injection** — noise floor (Black PSNR), white spread and an optional fixed seed. See [Impairments](impairments.md).

**Dropouts** — random (Poisson) and scratch severity scales, each with an optional fixed seed.

**On-screen display** — the overlay list, with text, position, scale and foreground/background luma per overlay. The available substitution tokens are listed beneath the text field.

**Audio channel pairs** — one entry per stereo pair, with a description and either one tone for both channels or independent left/right tones, each optionally a frequency ramp.

Every editor is a view onto the project document, so every change is undoable and every change marks the project dirty.

## The Preview and Line Scope

The preview synthesises frames on demand through the same generation code the pipeline uses, so what you see is what will be written.

**Navigator** — a frame spinner over the whole output, showing which section the current frame belongs to. Sections that contribute no output frames say so rather than showing a stale picture.

**Mode**

- **Source** — the decoded source frame, before encoding.
- **Encoded** — the woven full-frame composite raster including the VBI. This is the default.

**Trace** — for encoded mode, **Composite** or a **Y+C overlay**.

**Zoom** — **Fit**, **100%** or **200%**.

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

Double-clicking an issue selects the section it relates to, or reports that it is a project-level issue.

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
