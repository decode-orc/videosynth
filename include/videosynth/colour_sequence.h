/*
 * File:        colour_sequence.h
 * Module:      colour_sequence
 * Purpose:     The colour subcarrier lattice and the PAL burst / V-switch
 *              sequence of the 4fsc synthesiser, shared by generation and by
 *              the stages that demodulate its chroma.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <cstddef>

#include "videosynth/model.h"
#include "videosynth/signal_timing_model.h"

namespace videosynth {

// Thread-safety: all functions in this module are thread-safe pure functions.

// Samples per subcarrier cycle at 4fsc. The carrier advances exactly π/2 per
// sample, so its phase is a pure function of the sample index modulo this
// value; see docs-tech/design/high-level-design.md, "Subcarrier Lock
// Implementation".
constexpr std::size_t kSubcarrierLatticeSamples = 4U;

// ITU-R BT.1700 Annex 1 Part B Figure 8 with Table 1 item 10f: the 8-field
// PAL sequence pairs each burst-blanking meander position with a specific
// subcarrier-to-frame phase. Rotating the subcarrier lattice by 270° anchors
// disc frame 0 (meander parity 0, fields I/II) to the subcarrier phase that
// decoders identify as colour fields 1/2. Validated against the
// ld-decode/decode-orc field-phase detection: without this anchor the two
// fields of a frame decode as non-consecutive field IDs. The anchor applies
// to 625-line PAL only; PAL-M keeps a zero anchor.
constexpr double kPalSubcarrierAnchorRad = 3.0 * 3.14159265358979323846 / 2.0;

// Number of frames after which a clean synthesised frame repeats exactly for
// identical source content. The subcarrier lattice position advances by
// (samples per frame mod 4) each frame — period 4 for PAL (709,379 ≡ 3) and
// PAL-M (477,225 ≡ 1), period 2 for NTSC (477,750 ≡ 2) — and the PAL/PAL-M
// burst meander and V-switch have period 2, which divides 4. The LaserDisc
// pilot burst is period 1 (17,734,475 = 25 × 709,379). SECAM chroma is
// rendered from 0H of every line, independent of the lattice position, and
// its D'R/D'B sequence and phase inversion repeat every 2 frames (ITU-R
// BT.1700 Annex 1 Part C Figure 16, Table 4 Note 1). Every use of the disc
// frame index in clean synthesis therefore depends only on the index modulo
// this period.
inline std::size_t ColourSequencePeriodFrames(Standard standard) {
  return (standard == Standard::kNtsc || standard == Standard::kSecam) ? 2U
                                                                       : 4U;
}

// Phase of the colour subcarrier at an absolute sample index, exactly.
//
// Reducing the index onto the 4-sample lattice before the multiply keeps the
// result exact for the life of the render: sin/cos see one of four bounded
// arguments rather than (π/2) × an index that reaches 1.9 × 10^11 on a
// three-hour disc.
inline double SubcarrierPhaseRad(std::size_t absolute_sample_index) {
  constexpr double kQuarterWaveRad = 3.14159265358979323846 / 2.0;
  return kQuarterWaveRad *
         static_cast<double>(absolute_sample_index % kSubcarrierLatticeSamples);
}

inline int PalBurstSequenceIndex(std::size_t frame_index,
                                 int field_index_1based) {
  // ITU-R BT.1700 Annex 1 Part B Table 1 item 10f: defines sequence I/II/III/IV
  // repeating over colour fields. A frame contains two consecutive fields.
  return static_cast<int>(
      ((2U * frame_index) + static_cast<std::size_t>(field_index_1based - 1)) %
      4U);
}

inline bool PalBurstPositiveOnOddLine(int burst_sequence_index) {
  // Sequence I/II: odd lines use +135 deg, even lines use -135 deg.
  // Sequence III/IV: odd lines use -135 deg, even lines use +135 deg.
  return burst_sequence_index == 0 || burst_sequence_index == 1;
}

inline double PalBurstPhaseRadForLine(std::size_t frame_index,
                                      const LineTimingPrimitive& line) {
  constexpr double kPi = 3.14159265358979323846;
  const int burst_sequence_index =
      PalBurstSequenceIndex(frame_index, line.field_index_1based);
  const bool line_is_odd = (line.line_number_1based % 2) == 1;
  const bool positive_on_odd = PalBurstPositiveOnOddLine(burst_sequence_index);
  const bool positive_phase = line_is_odd ? positive_on_odd : !positive_on_odd;
  return positive_phase ? (3.0 * kPi / 4.0) : (-3.0 * kPi / 4.0);
}

// True on the PAL and PAL-M lines whose V axis is inverted (the V-switch).
inline bool PalInvertVAxisForLine(std::size_t frame_index,
                                  const LineTimingPrimitive& line) {
  return PalBurstPhaseRadForLine(frame_index, line) < 0.0;
}

// The offset generation adds to the subcarrier lattice phase when it
// modulates a line's picture chroma: U (NTSC b-y) rides sin(lattice +
// offset) and V (r-y) cos(lattice + offset). SMPTE 170M-2004 Section 10
// places NTSC picture chroma at burst + 180°; PAL carries its subcarrier
// anchor; PAL-M has none.
inline double PictureChromaPhaseOffsetRad(Standard standard,
                                          const LineTimingPrimitive& line) {
  constexpr double kPi = 3.14159265358979323846;
  if (standard == Standard::kNtsc) {
    return line.burst_phase_rad + kPi;
  }
  return standard == Standard::kPal ? kPalSubcarrierAnchorRad : 0.0;
}

}  // namespace videosynth
