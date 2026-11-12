# 4fsc Horizontal Timing — Implementation Plan

Bring the horizontal timing of generated output into line with CVBS File
Format Specification v1.8.0. This plan fixes two problems:

- **0H origin, all standards:** 0H of stored line 0 must sit at −0.5 samples,
  midway before the first stored sample.
- **PAL lattice, PAL and SECAM:** 0H of frame line *k* (0-based) must fall
  *k* × 1135.0064 samples after 0H of line 0, on the subcarrier-locked 4fsc
  lattice.

References:

- CVBS spec, [signal-state-presets](../cvbs-file-format-specification/docs/signal-state-presets.md)
  (time-base stability; "a grid with a whole number of samples on every line
  … is not the PAL 4×fsc lattice") and
  [video-standard-presets](../cvbs-file-format-specification/docs/video-standard-presets.md)
  (Stored Frame and Line Origin; Preset `PAL`, "non-orthogonal 4fsc sampling
  structure"; Preset `SECAM`).
- [EBU Tech. 3280-E](../analogue-video-specifications/docs/video_formats/EBU-Tech-3280-E/EBU-Tech-3280-E.md)
  §1.2 (1135.0064 samples per line, 709,379 per frame; 0° Sc/H sampling
  phase).
- SMPTE 244M-2003 (NTSC 4fsc: 0H between digital-line samples 784 and 785).
- HLD — [`high-level-design.md`](../design/high-level-design.md), 4fsc frame
  cadence (line 158), PAL chroma path, horizontal mapping (line 398).
- decode-orc `orc/sdk/include/orc/support/pal_line_locked_resample.h`
  (issue #369): decode-orc's model of the same lattice. It keeps 1135-sample
  storage lines with two extra samples on lines 313 and 625
  (`kPalExtraSampleLines`), and puts 0H of row *k* at *k* × 709379 / 625.

## Current behaviour

Measured on generated frames (100 % bars):

- **Origin:** the sync ramp *starts* at line sample 0, so the
  half-amplitude point, which is the spec's 0H, sits at **+3.0** on PAL/SECAM
  and **+1.5** on NTSC/PAL-M. Every other feature (burst gate, active window,
  BT.601 *t*₀, biphase/FM/VITS start times) is already placed as a time after
  0H with sample 0 taken as 0H, as BT.1700 and SMPTE 170M define. Only the
  sync edges are misplaced.
- **Lattice:** storage lines are 1135 samples, except lines 313 and 625, which
  are 1137. Every line's sync starts at the start of its storage line, so on
  PAL/SECAM 0H runs behind the lattice in a sawtooth. The lag grows by 4/625
  sample per line, reaches 1.997 samples (113 ns) on line 313, and resets on
  lines 314 and 1.
- The subcarrier is computed from the absolute sample index, so burst and
  chroma phase are already lattice-locked. Sc/H was set against the misplaced
  0H.
- NTSC and PAL-M are orthogonal (910 and 909 samples per line), so the lattice
  problem does not affect them.

## Design decisions

| Topic | Decision |
|-------|----------|
| 0H origin | 0H of every frame's line 0 is at −0.5 samples on every standard. Sync edges move earlier by 3.5 samples (PAL/SECAM) or 2 samples (NTSC/PAL-M). Burst, active window and VBI/OSD positions stay where they are, which puts them 0.5 sample after their nominal time. That is the spec's midway convention, not an error. |
| Sync edges | Every falling edge (horizontal, equalising, broad and the mid-line pulses) is drawn with its half-amplitude point at the pulse's nominal time − 0.5. `ShapedPulseLevel` is evaluated at that fractional position. Pulse widths stay nominal, measured at half amplitude (BT.1700 / SMPTE 170M). The pilot-burst gate moves with the sync. |
| Edge before sample 0 | The part of a line-start edge that falls before the line's first sample is added into the previous storage line's front porch. For line 1 that is the same frame's last line, because the edge is the same in every frame. |
| Storage layout | Unchanged: 1135 samples per storage line, 1137 on lines 313 and 625 (`BuildLineSampleCounts`). It matches decode-orc's `kPalExtraSampleLines`, and the spec does not constrain which samples a stored "line" holds. |
| Lattice delay | On PAL/SECAM, storage line *k* (0-based) carries its waveform delayed by *d*ₖ = 4*k*/625 for *k* ≤ 312 and (4*k* − 1250)/625 for *k* ≥ 313, so *d*ₖ ∈ [0, 2). This is exact: storage offset + *d*ₖ = *k* × 709379/625. *d*ₖ = 0 on NTSC and PAL-M. |
| How the delay is applied | Render each line 0H-relative, then delay the whole line (Y and C) by *d*ₖ with the Kaiser-sinc kernel `VariableDelayInterpolator` already uses. Each line reads only its own render, with blanking guard samples either side, so no line leaks into its neighbour. The leading guard holds the pre-0H half of the sync edge, so the edge is delayed with its line before it is added into the previous line. The alternative, rendering every feature (burst gate, picture grid, biphase, FM, VITS, OSD, SECAM gate) at a fractional origin, touches about ten modules. The delay touches one. |
| Lattice-locked carriers | The PAL colour subcarrier (burst and picture chroma) and the PAL LaserDisc pilot burst are tied to absolute time, not to 0H. They are rendered with their phase advanced by ω·*d*ₖ, so after the delay they land on the lattice. |
| SECAM chroma | The SECAM FM carrier is 0H-locked ("same phase at every 0H"), so it is delayed with the line and gets no phase advance. |
| Sc/H | `kPalSubcarrierAnchorRad` and the NTSC/PAL-M reference are worked out again from EBU Tech. 3280-E / SMPTE 244M, so frame 1 is at 0° Sc/H against the corrected 0H. They are not tuned to a decoder. |
| Picture truth | Stays 0H-relative and is not delayed: row samples start `first_sample` after 0H. Decoders return 0H-relative pictures, so this is the useful frame of reference. The truth bytes and schema stay unchanged. The docs state where 0H sits in the stored samples: −0.5 + *d*ₖ after the storage line's first sample. |
| Output format | No change to `.cvbs`/`.meta` schemas or `user_version`. Sample data changes on all four standards. |
| decode-orc | Used to check the result, not to define it. If it relied on the old timing, report that as a decode-orc issue; videosynth follows the spec. |

Known risk: the 32-tap Kaiser kernel (β = 8) has to pass the PAL chroma band
(4.43 MHz ± 1.3 MHz, up to 0.33 fs) without measurable gain or phase error.
Task 2.2 measures this. If it fails, add a longer kernel for this use instead
of changing `VariableDelayInterpolator`, whose output the channel and
line-timing stages' hashes depend on.

## Phase 1 — Timing model and measurement

1. **Lattice delay function.** Add `LatticeLineDelay(Standard, int line_1based)`
   to `include/videosynth/frame_line_layout.h`. It returns *d*ₖ as an exact
   rational (numerator over 625) with a `double` accessor.
   *Accept:* unit tests (`tests/unit/`): line 1 → 0, line 313 → 1248/625,
   line 314 → 2/625, line 625 → 1246/625. For every PAL line, storage offset ×
   625 + numerator = *k* × 709379. NTSC and PAL-M return 0 on every line.
2. **Sync-edge measurement helper.** Add a test-support function that returns
   the absolute half-amplitude falling-edge position of every line's
   horizontal sync in a synthesised frame: the crossing of (blanking + sync
   tip) / 2, with linear interpolation between the two samples either side.
   *Accept:* functional test that pins today's output: 0H at +1.5 on every
   NTSC and PAL-M line (spread < 0.01 sample), at +3.0 on PAL line 1, and the
   PAL sawtooth (line 313 lag 1.997 ± 0.01). Phase 2 replaces its expected
   values.
3. **Hash baseline.** Run `scripts/output-hashes.sh --record` on the
   unchanged build.
   *Accept:* `build/output-hashes/{general,stacking}.sha256` exist.
4. **HLD line 158.** Correct "four lines per frame at 1134" to the
   implemented storage layout (two extra samples on each of lines 313 and 625).
   *Accept:* the HLD matches `BuildLineSampleCounts`.

## Phase 2 — Spec-compliant horizontal timing in the generator

1. **Centred sync edges.** Draw every sync falling edge with its
   half-amplitude point at nominal − 0.5 sample, on all standards. Rebuild the
   pulse-waveform tables and the pilot-burst gate table at the new positions,
   once per run. Add the pre-0H half of each line-start edge into the previous
   storage line; for line 1, the frame's last line.
   *Accept:* unit test: each pulse table's half-amplitude falling crossing is
   at −0.5 ± 0.01. The half-amplitude pulse widths match BT.1700 / SMPTE 170M
   within 0.5 sample.
2. **Line delay stage.** Add `LatticeLineShifter`
   (`include/videosynth/lattice_line_shifter.h`, `src/lattice_line_shifter.cpp`).
   It delays each storage line of a Y or C frame by *d*ₖ, reading its own line
   and leading guard, which holds the pre-0H edge, and is a no-op when every
   *d*ₖ is 0. Call it in `GenerationStage` after all line content (sync,
   burst, picture, biphase/FM, VITS, OSD) is written, on both planes.
   *Accept:* unit tests: a line of constant blanking passes unchanged. A
   4.43 MHz tone delayed by any *d*ₖ keeps amplitude within 0.1 % and phase
   within 0.1° of the analytic delayed tone. A sync edge's half-amplitude
   point moves by *d*ₖ ± 0.01.
3. **Carrier phase advance.** In the burst, picture-chroma and pilot-burst
   paths (the three `SubcarrierPhaseRad` callers and the pilot table in
   `generation_stage.cpp`), advance the carrier phase by ω·*d*ₖ for PAL. Leave
   SECAM C alone.
   *Accept:* test: burst phase measured at a fixed absolute sample on lines 1,
   157, 313 and 625 is the same function of the absolute sample index on every
   line, within 0.1°.
4. **Re-derive Sc/H.** Set `kPalSubcarrierAnchorRad` and the NTSC/PAL-M
   reference from EBU Tech. 3280-E / SMPTE 244M for 0H at −0.5.
   *Accept:* a unit test that computes Sc/H from the measured 0H and the
   subcarrier phase gives 0° ± 1° on frame 1 of each standard. The PAL 8-field
   and NTSC 4-field sequence tests pass.
5. **Compliance test.** Change the Phase 1 functional test to the spec
   positions.
   *Accept:* every line's 0H is at −0.5 ± 0.02 (NTSC/PAL-M) or
   −0.5 + *d*ₖ ± 0.02 after its storage line's first sample (PAL/SECAM). The
   burst start and active window start are at their specified times after the
   measured 0H, within 0.5 sample. Frame sizes are unchanged.
6. **Cost.** Run `scripts/benchmark.sh`.
   *Accept:* the throughput change per standard is reported to the user before
   continuing.

## Phase 3 — Consumers of line positions

1. **Picture windows.** `picture_chroma.cpp` (`ChromaErrorStage`'s picture and
   burst windows) and `line_timing_stage.cpp` (line start and slope pivot) add
   *d*ₖ, rounded where they work in whole samples, to the storage-line
   offset.
   *Accept:* functional tests: on PAL, the chroma-error window starts within
   0.5 sample of the first delayed picture sample on lines 23, 157 and 310.
   The line-timing stage's existing tests pass.
2. **Picture truth.** Confirm the truth path takes the 0H-relative render, not
   the delayed planes.
   *Accept:* `.truth` and `.truth.meta` hashes are unchanged from the Phase 1
   baseline.
3. **GUI preview.** In `src/gui/preview_render.cpp`, offset each line of the
   encoded raster and the line scope by round(*d*ₖ), so sync lines up
   vertically.
   *Accept:* unit test of the render helper: line 313's first plotted sample is
   2 samples into its storage line.
4. **Audit.** Check every other user of `BuildLineSampleOffsets`
   (`BiphaseInjectionManager`, `OsdRenderer`, VITS, the dropout active range)
   runs before the shifter or needs no sub-sample accuracy, and note the
   reason in a comment where it isn't obvious.
   *Accept:* a list of call sites with their status in the task report.

## Phase 4 — Documentation, decode-orc check and baselines

1. **HLD.** Add a "4fsc horizontal timing" subsection covering the 0H origin,
   the centred sync edges, the storage layout, *d*ₖ, the line delay, the
   carrier phase advance, the SECAM exception and Sc/H. Update line 158, line
   398, the sync-generation section and the PAL chroma and SECAM sections.
   *Accept:* every behaviour from Phases 2–3 is described. No text says
   sample 0 is 0H, or that 0H sits at a fixed position on every PAL line.
2. **Published docs.** `docs/user-manual/video-signal.md` (4fsc paragraph and
   0H-relative timings), `docs/user-manual/outputs.md` (frame-size table:
   storage lines and 0H), `docs/reference/sections.md` (BT.601 *t*₀, painted
   span), `docs/reference/output.md` (truth `first_sample`).
   *Accept:* `nix develop "path:$PWD" --command mkdocs build --strict` is
   clean, and every "after 0H" figure matches a measurement on generated
   output.
3. **decode-orc.** Decode the fixtures of all four standards, including PAL
   LaserDisc (biphase, FM, pilot), with decode-orc before and after the change.
   Check colour-frame detection, chroma decode and the VBI readers. Compare the
   PAL sync drift with an ld-decode `.tbc` converted through decode-orc's #369
   path.
   *Accept:* VBI codes decode identically, chroma decodes with no new errors,
   and the per-line 0H drift agrees within 0.05 sample. Any decode-orc
   dependence on the old timing is reported, not worked around.
4. **Baselines.** Re-record `scripts/output-hashes.sh --record`.
   *Accept:* `scripts/output-hashes.sh` passes.
