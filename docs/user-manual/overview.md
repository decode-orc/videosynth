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
line_injections:  # Project-wide: disc format and the VITS set
sections:         # The ordered timeline
```

## Sections

A section is a contiguous run of output frames drawn from one source file, with its own impairments, audio, on-screen display and VBI codes. Sections play back to back in the order they are declared, and their frame counts add up to the length of the output.

Sections are how a project expresses change over time — a different picture, a noisier passage, a new chapter, a scratch that appears halfway through. They are also how a laserdisc's structure is expressed: giving a section a `section_type` of `lead_in`, `programme_area` or `lead_out` turns the project into a disc, with all the validation that implies.

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
| Output paths, signal type, EFM selection | Project | [`output`](../reference/output.md) |
| Disc format (CAV/CLV), VITS placement, VITS set | Project | [`line_injections`](../reference/line-injections.md) |
| Pressing timing offset, edge smear, FM beat | Project | [`channel`](../reference/channel.md) |
| Capture chroma gain and phase error | Project | [`chroma_error`](../reference/chroma-error.md) |
| Picture source, duration, disc section type | Section | [`sections[]`](../reference/sections.md) |
| Laserdisc biphase codes | Section | [`sections[].line_injections`](../reference/section-line-injections.md) |
| Noise, dropouts, luma bandwidth | Section | [`noise`](../reference/section-noise.md), [`dropouts`](../reference/section-dropouts.md), [`luma_bandwidth`](../reference/sections.md#luma_bandwidth) |
| Audio channel pairs | Section | [`audio`](../reference/section-audio.md) |
| On-screen display overlays | Section | [`osd`](../reference/section-osd.md) |

The split is not arbitrary. Anything that describes the *file* is project-level; anything that can legitimately differ between one part of the timeline and another is section-level. A disc is entirely CAV or entirely CLV, so `disc_type` is project-level; a disc's picture numbers restart or jump between sections, so the codes are section-level.

## Reading on

- [The GUI](gui.md) and [the CLI](cli.md) — the two ways to drive it.
- [Video signal](video-signal.md) — standards, sample encodings, composite and Y/C.
- [Picture sources](sources.md) — what videosynth will accept as content.
- [VITS](vits.md), [Laserdisc discs](laserdisc.md) — VBI content.
- [Impairments](impairments.md), [Audio](audio.md), [On-screen display](osd.md).
- [Output files](outputs.md) and [Validation](validation.md).
