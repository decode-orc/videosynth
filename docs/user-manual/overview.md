# Overview

This page explains how videosynth is put together and the vocabulary the rest of the manual uses. If you have not yet generated anything, work through the [Quick Start](../getting-started/quick-start.md) first — it is easier to read this with a concrete project in front of you.

## The idea

videosynth synthesises analogue video signals from first principles. Nothing is sampled from a real capture: every sync pulse, burst, VBI code and active picture line is built from the timing and level figures published in the standards, on the sample lattice the output format demands.

Two consequences follow, and they are the reason the tool exists:

- **You know the right answer.** Every sample of the output is a deliberate choice traceable to a clause in a specification, so a decoder's output can be compared against ground truth rather than against another decoder.
- **The output is reproducible.** Generation is deterministic. The same project produces byte-identical files on any machine, at any thread count, on any run. Where randomness is wanted — noise, dropouts — it is seedable.

## The project

Everything videosynth does is described by a **project**: a YAML file naming one output format and an ordered list of **sections**. Both front ends read and write the same file, losslessly — a project saved by the GUI parses identically in the CLI, and vice versa.

A project has exactly one video standard, one sample encoding and one signal state, because the artefact it produces is one capture file with one format. Everything that can legitimately vary along the timeline lives in the sections.

```yaml
project:          # Name, version, description
cvbs_presets:     # The signal format — one per project
output:           # Where the media goes, and in what form
line_injections:  # Project-wide VITS set
laserdisc:        # LaserDisc only: disc format, bursts, EFM audio, LaserVision channel
channel:          # Pressing impairments
chroma_error:     # Capture chroma error
line_timing:      # Per-line timing error after time-base correction
sections:         # The ordered timeline
```

Only `cvbs_presets`, `output` and `sections` are required. Everything specific to LaserDisc is in the `laserdisc:` block, and in a `laserdisc:` block on each section that belongs to the disc, so a project that is not a disc never needs either.

## Sections

A section is a contiguous run of output frames drawn from one source file, with its own impairments, audio, on-screen display and VBI codes. Sections play back to back in the order they are declared, and their frame counts add up to the length of the output.

Sections are how a project expresses change over time — a different picture, a noisier passage, a new chapter, a scratch that appears halfway through. They are also how a laserdisc's structure is expressed: giving a section a `laserdisc:` block with an `area` of `lead_in`, `programme_area` or `lead_out` turns the project into a disc, with all the validation that implies. See [Laserdisc discs](laserdisc.md).

## The pipeline

A run is a fixed sequence of stages:

```text
[YAML project]
      ↓
  Parse            Read the file; reject unknown keys outright
      ↓
  Validate         Check every rule; probe the source files
      ↓
  Generate         Synthesise Y and C for each frame, in millivolts
      ↓
  Channel          Pressing and replay impairments (LaserVision channel, edge smear, timing offset)
      ↓
  Chroma error     The capture's chroma gain, phase, differential and crosstalk
      ↓
  Line timing      The per-line picture timing error left after time-base correction
      ↓
  Noise            Add per-section Gaussian noise, before quantisation
      ↓
  Dropouts         Punch per-section dropout events; record them in a sidecar
      ↓
  Output           Combine Y and C, quantise, write the sample and metadata files
      ↓
[.cvbs + .meta (+ .dropouts.meta, .wav, .efm)]
```

Some points worth knowing:

- **Luma and chroma are generated independently** and combined only at the output stage. That is what makes Y/C output a format choice rather than a separate code path.
- **Noise is injected before quantisation**, on the internal fixed-point millivolt representation, so it lands on every synthesised region — VBI lines, blanking and active picture alike — and survives into the final sample codes.
- **Audio runs alongside**, driven by output position rather than by source frames, so every track stays sample-accurately locked to the video.
- **Cancellation is clean.** A cancelled run aborts every artefact it was writing, so it never leaves a half-written file behind.

## Determinism and threading

Frame synthesis is spread across worker threads and the results reassembled in order, so output does not depend on the thread count. `--threads 1` selects a pure sequential path; `--threads auto` (the default) uses the available hardware threads. Both produce the same bytes.

A frame template cache holds the parts of a frame that do not change between frames (sync structure, burst, static VBI content), patched per frame with what does. It is a speed optimisation only — `--template-cache-mb 0` disables it and the output is unchanged.

## Where things live

| Concept | Scope | Declared in |
|---------|-------|-------------|
| Video standard, sample encoding, signal state | Project | [`cvbs_presets`](../reference/cvbs-presets.md) |
| Output paths, signal type, picture truth | Project | [`output`](../reference/output.md) |
| VITS placement, VITS set | Project | [`line_injections`](../reference/line-injections.md) |
| Pressing timing offset, edge smear | Project | [`channel`](../reference/channel.md) |
| Capture chroma gain, phase, differential and crosstalk | Project | [`chroma_error`](../reference/chroma-error.md) |
| Per-line picture timing error (capture and pressing parts) | Project | [`line_timing`](../reference/line-timing.md) |
| Picture source, duration, cadence | Section | [`sections[]`](../reference/sections.md) |
| *LaserDisc:* disc format, pilot/VBI burst, EFM audio | Project | [`laserdisc`](../reference/laserdisc.md) |
| *LaserDisc:* FM beat, group delay, replay response, field-2 offset | Project | [`laserdisc.channel`](../reference/laserdisc-channel.md) |
| *LaserDisc:* disc area, biphase codes | Section | [`sections[].laserdisc`](../reference/section-laserdisc.md) |
| Noise, dropouts, luma bandwidth | Section | [`noise`](../reference/section-noise.md), [`dropouts`](../reference/section-dropouts.md), [`luma_bandwidth`](../reference/sections.md#luma_bandwidth) |
| Camera-chain picture softening | Section | [`picture_response`](../reference/sections.md#picture_response) |
| Audio channel pairs | Section | [`audio`](../reference/section-audio.md) |
| On-screen display overlays | Section | [`osd`](../reference/section-osd.md) |

The split is not arbitrary. Anything that describes the *file* is project-level; anything that can legitimately differ between one part of the timeline and another is section-level. A disc is entirely CAV or entirely CLV, so `laserdisc.disc_type` is project-level; a disc's picture numbers restart or jump between sections, so the codes are section-level.

## Reading on

- [The GUI](gui.md) and [the CLI](cli.md) — the two ways to drive it.
- [Video signal](video-signal.md) — standards, sample encodings, composite and Y/C.
- [Picture sources](sources.md) — what videosynth will accept as content.
- [VITS](vits.md), [Laserdisc discs](laserdisc.md) — VBI content.
- [Impairments](impairments.md), [Audio](audio.md), [On-screen display](osd.md).
- [Output files](outputs.md) and [Validation](validation.md).
