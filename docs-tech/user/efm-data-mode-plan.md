# EFM Data Mode — Implementation Plan

Encode a 2,048-byte-per-sector image (AIV / LV-ROM data, e.g. a BBC Domesday
VFS image) into the LaserDisc EFM channel as ECMA-130 Mode 1 sectors, locked
to the video by the AIV placement rule. Source: GitHub issue #21.

References:

- ECMA-130 — [`ECMA-130.md`](../analogue-video-specifications/docs/efm/ECMA-130/ECMA-130.md)
  (§14 sector layout, §14.2 header, §14.3 EDC, §15 + Annex B scrambling,
  §16 sector-to-frame mapping, Annex A RSPC).
- IEC 60908-1999 (subcode, CIRC, EFM) — already implemented in `videosynth_efm`.
- HLD — [`high-level-design.md`](../design/high-level-design.md),
  "LaserDisc Digital Audio (EFM) Output".
- Reference decoder — decode-orc `orc/plugins/stages/common/efm-decode`
  (`dec_f1sectiontodata24section`, `dec_data24torawsector`,
  `dec_rawsectortosector`, `dec_sectorcorrection`).

## Design decisions

These are fixed for every phase.

| Topic | Decision |
|-------|----------|
| Sector format | ECMA-130 Mode 1: 12-byte sync, header (BCD MIN/SEC/FRAC + mode `01`), 2,048 user bytes, EDC (CRC-32, §14.3) over bytes 0–2063 stored LSB first, 8 zero bytes, 172 P + 104 Q RSPC bytes (Annex A). Bytes 12–2351 scrambled (Annex B). |
| Sector → F1 | One sector per subcode section (98 F1 frames × 24 bytes). Sector bytes are consecutive (§16), carried as little-endian 16-bit words: F1 byte `2w` (WmA) = sector byte `2w+1`, F1 byte `2w+1` (WmB) = sector byte `2w`. This is the pair swap that decode-orc undoes. |
| Header address | Image sector *n* carries address `first_address + n` (default 0). |
| Placement | Header time = picture time + `picture_offset_sectors` (default 150 = 2 s). Picture *p* carries addresses `floor(p × 75 / frame_rate) + offset` onward; PAL picture *p* = image sectors 3*p*+150 … 3*p*+152. The rule is anchored on the first frame with a CAV picture number (or, with none, the first programme-area frame as picture 1). Addresses then follow the continuous subcode timeline, so address − Q time stays constant, which the decoder's sector placement requires. A picture number that disagrees with the anchor is reported. |
| Position within frame | A sector's 98 F1 frames enter the CIRC encoder at the start of its subcode section. The CIRC delay (108 F1 frames) is **not** pre-compensated, which matches the audio path, so a sector finishes decoding about ⅓ frame after its picture starts. This still needs checking against a capture (acceptance 3). |
| Frames without EFM | Sections that do not declare `laserdisc.efm_data` carry **no t-values** (zero-length `efm_frame` rows). The CIRC encoder and modulator restart after each gap. A restarted encoder is identical to one flushed with silence, so the interleave is seamless. Q time keeps counting across the gap. Sectors whose 98 + 111 F1 frames are cut by a gap cannot be recovered, and they are reported. |
| Fill before the image | Mode 1 sectors with zero user data and ECMA-130 §14.2 lead-in headers (MIN + A0, time since the start of the lead-in). decode-orc discards them, so they keep sector sync without adding output. |
| Fill after the image | ECMA-130 §14.1 Mode 0 sectors with addresses continuing past the image. The decode stays clean, but the decoded file grows by those zero sectors (the image prefix stays byte-identical). |
| Track layout | One data track for the whole programme area: the 2 s INDEX 00 pause of track 1 is kept, and the lead-in TOC lists track 1. Lead-in and lead-out follow `laserdisc.area` as for audio. |
| Q control | `q_control: data` → `0100` (data, copy prohibited), or `data_copy_permitted` → `0110` (IEC 60908-1999 §17.5 / ECMA-130 §22.3.2). |
| Q mode (ADR) | `q_mode` defaults to `4`, the LaserDisc mode of IEC 60856/60857:1986 Amd 2 §13.5.1.1 / §13.6.1.1. `q_mode: 1` (IEC 60908-1999 §17.5) is an override for AIV discs such as Domesday, which predate that amendment and record ADR 1. Only the ADR nibble differs: the DATA-Q layout is the same in both modes. |
| Output files | Same `<basename>.efm` + `<basename>.efm.meta` pair as `efm_audio`, so the file and sidecar code is shared. |
| Module boundary | Sector building (EDC, RSPC, scrambling, sector → F1) lives in `videosynth_efm` with no core dependency. Placement and file I/O live in core. |

Known risk: decode-orc's `F2SectionCorrection` skips lead-in (TNO 00)
sections until its timeline has settled, so image sectors that fall in the
lead-in may not decode. The round trip in phase 5 measures this. The issue
reports that Domesday sectors 143–179 decode from captures.

---

## Phase 1 — ECMA-130 sector encoder (`videosynth_efm`)

1. **Scrambler and EDC.** `efm/data_sector.h`: the Annex B scramble sequence
   (x¹⁵+x+1, preset 0x0001, LSB first) over bytes 12–2351, and the §14.3 EDC
   CRC-32 (x³²+x³¹+x¹⁶+x¹⁵+x⁴+x³+x+1).
   *Accept:* unit tests: the first scramble bytes match decode-orc's
   `unscrambleTable` (`01 80 00 60 00 28 …`), and the EDC of a known sector
   matches the reference value.
2. **RSPC.** P parity (86 × (26,24)) and Q parity (52 × (45,43)) over GF(2⁸)
   with the Annex A generator and addressing.
   *Accept:* unit test: every P and Q codeword of an encoded sector has zero
   syndromes, and flipping one byte makes one P and one Q syndrome non-zero.
3. **Sector builders.** `EncodeMode1Sector(address, user_data)`,
   `EncodeMode0Sector(address)`, `EncodeLeadInSector(relative_time)` (MIN + A0
   header), each returning a scrambled 2,352-byte sector.
   *Accept:* unit tests on the header BCD, the mode byte, the zero field, and
   descrambling to the expected layout.
4. **Sector → F1 frames.** `SectorToF1Frames` with the WmA/WmB pair swap.
   *Accept:* unit test: frame 0 bytes 0/1 = sector bytes 1/0, and 98 frames
   cover 2,352 bytes.
5. **Stream encoder F1 input and gaps.** `EfmStreamEncoder::PushF1Frames` and
   `SkipFrames(count)`. `SkipFrames` ends the current run (flushes the last
   T value), advances the subcode frame index without emitting, and restarts
   CIRC, modulator and T-value state.
   *Accept:* unit tests: PCM and F1 input give identical t-values, a skip
   keeps the Q time continuous, and a restarted stream decodes with the
   test-side decoder.

## Phase 2 — Subcode for a data track

1. **Configurable control nibble and ADR.** `TrackTable::control` (default
   audio `0000`) and `TrackTable::adr` (default mode 4), used by every Q
   payload.
   *Accept:* unit test: a data table yields control `0100` in lead-in,
   programme and lead-out payloads; ADR 1 changes only the ADR nibble; the
   CRC stays valid; the audio defaults are unchanged.
2. **Single-track layout option.** `BuildEfmTrackLayout` gains a
   single-programme-track mode: all programme runs merge into track 1.
   *Accept:* unit test: three programme sections → one track, with the TOC
   A0/A1 = 01.

## Phase 3 — Project model, parser, emitter, validator

1. **Model.** `EfmDataOutput { enabled, image, first_address,
   picture_offset_sectors, q_control, q_mode }` on `LaserdiscParameters`, plus
   `Section::carries_efm_data`.
   *Accept:* equality operators updated, and `LaserdiscBlockPresent` covers
   it.
2. **YAML parse/emit.** `laserdisc.efm_data` (keys `image`, `first_address`,
   `picture_offset_sectors`, `q_control`, `q_mode`) and `section.laserdisc.efm_data: {}`.
   `{project}`/asset-root path resolution for `image`.
   *Accept:* parser/emitter unit tests, including round trips and unknown-key
   errors.
3. **Validation.** Errors for: `efm_data` together with `efm_audio`; a
   non-PAL/NTSC/PAL-M standard; a section `efm_data` without a project
   `efm_data`; a PAL section that carries both EFM data and an audio channel
   pair; `first_address` / `picture_offset_sectors` out of range; a bad
   `q_control`; a `q_mode` other than 1 or 4. A warning when no section carries EFM data.
   *Accept:* validator unit tests for each rule, and the messages are listed
   in `docs/user-manual/validation.md`.

## Phase 4 — Placement and writer (core)

1. **Placement layout.** `efm_data_layout.h`: from the standard, the per-frame
   section, the `carries_efm_data` flag, the picture number and the settings,
   compute for every output frame its F1-frame span, whether it carries EFM,
   and for every subcode section its slot (image sector *n*, lead-in fill,
   Mode 0 fill, or silent). The function is pure.
   *Accept:* unit tests: PAL picture *p* → sectors 3*p*+150…3*p*+152, the
   no-picture-number fallback, NTSC fractional sections, slots cut by a gap,
   and the count of omitted image sectors with the first omitted address.
2. **Shared EFM file writer.** Extract the `.efm` + `.efm.meta` handling from
   `AudioEfmWriter` into a reusable `EfmStreamFile`. `AudioEfmWriter` keeps
   its API.
   *Accept:* the existing audio EFM unit and functional tests pass unchanged.
3. **`DataEfmWriter`.** Reads the image sector by sector through an injected
   reader interface and drives the stream encoder per output frame from the
   layout. It logs at info level the image sectors placed and omitted (from
   which address), the unrecoverable sectors at gap cuts, and any non-blank
   image sector that falls on a frame without EFM (warning).
   *Accept:* unit tests with an in-memory image reader. Functional test: the
   image length check rejects a size that is not a multiple of 2,048.
4. **Pipeline wiring.** `VideoSynthPipeline` drives the writer per stored
   frame (Begin/Append/Finalize/Abort), alongside the audio generator.
   *Accept:* the functional pipeline test writes `.efm` + `.efm.meta`, and
   cancellation removes both.

## Phase 5 — Round trip and fixtures

1. **Test-side sector decoder.** Extend `tests/support/efm_channel_decoder.h`
   with CIRC de-interleave plus descramble, so a synthetic image (each sector
   filled with its own address) decodes back sector-exact.
   *Accept:* functional test: byte-identical over every placed sector, and
   the placement matches the rule.
2. **Fixture project.** `projects/general/pal_efm_data.yaml` with a small
   synthetic image generated at test time under `{output}`.
   *Accept:* the fixture-project test runs it.
   *Status:* not done. The fixture runners have no step that generates an
   image, and a committed image would be a binary asset. The functional test
   `test_efm_data_pipeline.cpp` writes its own image instead.
3. **decode-orc round trip (manual).** Decode the generated `.efm` with
   `orc-cli` EFM sink, `decode_mode: data`.
   *Accept:* byte-identical over the placed sectors and an empty bad-sector
   map. The result is recorded in this plan, including the lead-in risk
   above.
   *Result* (decode-orc `dbd9f771`, PAL, 700-sector synthetic image, a
   60-frame lead-in, data sections and one 20-frame section without EFM):
   every sector placed in the programme area decodes byte-identical, with
   0 C1/C2 errors, 0 RSPC corrections and 0 address discontinuities. The
   decoder resyncs on the first sector after a gap. Missing in the decode,
   as the design expects: the sectors on the frames without EFM, and about
   one sector at each cut (the layout warns about two; its 111-frame window
   is conservative). Also missing: the 153 image sectors placed in the
   lead-in (0–152), because `F2SectionCorrection` skips TNO 00 sections,
   whether ADR is 4 or 1. This is still open: either decode-orc decodes
   data in the lead-in, or the Domesday lead-in differs from the lead-in
   generated here (the issue reports that sectors 143–152 decode from
   captures). The Mode 0 fill after the image decodes as zero-filled sectors
   past the image's end, as chosen. The decode-orc build of 14 Sep crashes
   in `Data24ToRawSector::waitingForSync` on any stream that reaches
   sector sync; the current source does not.

## Phase 6 — Documentation and GUI

1. **Docs.** `docs/reference/laserdisc.md`, `docs/reference/section-laserdisc.md`
   (new keys), `docs/user-manual/laserdisc.md` (EFM data section),
   `docs/user-manual/validation.md`, `docs/misc/troubleshooting.md`, and the
   HLD EFM section (data mode, placement, gaps, fill).
   *Accept:* `mkdocs build --strict` passes.
2. **GUI.** Project settings: an `efm_data` group (image, first address,
   offset, Q control, Q mode) that is mutually exclusive with `efm_audio`, and a
   per-section "carries EFM data" toggle.
   *Accept:* presenter unit tests.
   *Status:* done. The project settings **EFM data (AIV / LV-ROM)** group
   edits every `efm_data` key; each EFM use disables the other's enable
   control, and unticking it clears the section flags in the same undo step.
   The section editor's LaserDisc group has **Carries EFM data**, with a note
   for the PAL analogue-audio conflict. Validation issues about `efm_data`
   navigate to these controls.
3. **Real-disc acceptance (manual, outside CI).** Domesday Community North
   image (SHA-256 `08594e1d…a157`) → `.efm` → decode-orc → `vfs-verifier`,
   and a VP415e data read and picture check.
