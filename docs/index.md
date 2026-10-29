# Home

Welcome to the videosynth documentation. Here you will find the user manual and the complete project-file reference for videosynth.

# What is videosynth?

**videosynth** is a video signal synthesizer for PAL, PAL-M, NTSC and SECAM. It generates analogue composite (CVBS) and Y/C video signals as sample files, built in the time domain from the published analogue video standards — ITU-R BT.470/BT.1700, SMPTE 170M/244M and IEC 60856/60857.

It exists to produce **known-good, reproducible test signals**: material whose every sample you can predict, so that decoders, time base correctors and restoration tools can be exercised against a signal whose correct answer is already known. That includes laserdisc-style discs carrying VBI address codes, deliberately degraded signals with noise and dropouts, and multi-source "stacking" sets that model repeated captures of the same disc.

videosynth ships two front ends built from the same pipeline:

- **`videosynth-gui`** — a Qt 6 desktop application for authoring projects, previewing frames and running generation.
- **`videosynth`** — the command-line generator, for scripted and batch use.

Both read and write the same YAML project file, and both produce byte-identical output from the same project.

# What it can generate

- **Standards-based signals** for PAL, PAL-M, NTSC and SECAM, in composite or Y/C form, at 4fsc or raw high sample rates.
- **Picture content** from validated progressive sources — OpenEXR stills and MKV/MP4 video clips in any FFmpeg-decodable codec, colour-normalised to BT.601.
- **Vertical interval test signals** (VITS) — the EBU, ITU, NTC-7, FCC and VIRS families.
- **Laserdisc VBI metadata** — IEC 60856/60857 24-bit biphase codes and NTSC 40-bit FM codes: picture numbers, programme time codes, chapters, picture stops, programme status, lead-in and lead-out.
- **Impairments** — two-component Gaussian noise with optional spectral shaping and a shared pressing layer, dropout simulation (random surface events and persistent scratches, flagged in a sidecar database or left unflagged), pressing channel effects (sub-sample timing offset, edge smear, PAL FM-beat herringbone), chroma gain and phase error, and a per-section luma bandwidth limit.
- **Audio** — up to eight stereo channel pairs at 48 kHz/24-bit, plus laserdisc EFM digital audio.
- **On-screen display** burn-in text with substitution tokens for picture number, timecode and frame number.

# Where to start

- New to videosynth? Read [Installation](getting-started/installation.md), then work through the [Quick Start](getting-started/quick-start.md) — it builds a complete PAL CAV laserdisc with VITS and IEC-compliant biphase metadata from scratch.
- Looking for what a particular feature does? The [User Manual](user-manual/overview.md) covers each subsystem in turn.
- Looking for the exact meaning of a YAML key? The [Project File Reference](reference/overview.md) documents the project file block by block.

# Development status

videosynth is under active development, and some parts of the intended design are not yet implemented — VITC injection and custom per-line content in particular. This documentation describes the behaviour that is actually implemented; where something is planned but not yet available, it is called out explicitly.

If you spot a mistake, or would like to report an issue or contribute, please use the main repository:

[videosynth GitHub repository](https://github.com/decode-orc/videosynth){target="_blank"}

videosynth is designed to be used alongside decoding and restoration tooling such as [decode-orc](https://decode-orc.github.io/decode-orc/){target="_blank"} and [ld-decode](https://happycube.github.io/ld-decode-docs/){target="_blank"}.
