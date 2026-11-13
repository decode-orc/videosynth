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

// Phase added to the subcarrier lattice so that disc frame 0 is the first
// frame of the colour sequence at 0 degrees Sc/H, with 0H at
// ZeroHOffsetSamples before the frame's first sample (CVBS File Format
// Specification, Stored Frame and Line Origin). The lattice phase of absolute
// sample n is (pi/2) n; the quadrature encoder carries U on
// sin(lattice + offset) and V on cos.
//
// - PAL: EBU Tech. 3280-E §1.1.1 and Fig. 1 put the +U axis at zero phase at
//   0H of line 1, field 1. 0H is at sample -0.5, where the lattice is at
//   -pi/4, so the anchor is pi/4. The samples then fall at 45, 135, 225 and
//   315 degrees of the U axis, as §1.2 requires. Disc frame 0 has burst
//   meander parity 0 (fields 1 and 2, ITU-R BT.1700 Part B Figure 8).
// - PAL-M: the PAL convention on the 525-line structure (BT.470-6 Table 2),
//   with the same anchor.
// - NTSC: SMPTE 170M-2004 §13.2. 0H coincides with a zero crossing of the
//   burst-locked sine wave, and on line 10 of field I (colour frame A) that
//   crossing is positive-going. The burst is sin(lattice + anchor + pi/4)
//   (BurstPhaseRad). 0H of line 10 is lattice sample 8190 + z, z =
//   ZeroHOffsetSamples = -57/90, at phase pi + (pi/2) z, so the anchor is
//   3pi/4 - (pi/2) z = 192 degrees. The samples then fall on the I and Q axes
//   (SMPTE 244M-2003 Section 3.1): stored sample 125 of stored line 9 in
//   colour frame A, SMPTE 244M's sample 0 of line 10, is a +I sample.
constexpr double SubcarrierAnchorRad(Standard standard) {
  constexpr double kPi = 3.14159265358979323846;
  switch (standard) {
    case Standard::kPal:
    case Standard::kPalM:
      return kPi / 4.0;
    case Standard::kNtsc:
      return (3.0 * kPi / 4.0) -
             ((kPi / 2.0) * ZeroHOffsetSamples(Standard::kNtsc));
    default:
      return 0.0;
  }
}

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
// offset) and V (r-y) cos(lattice + offset). Every standard carries its
// subcarrier anchor. SMPTE 170M-2004 Section 10 also places NTSC picture
// chroma at burst + 180°.
inline double PictureChromaPhaseOffsetRad(Standard standard,
                                          const LineTimingPrimitive& line) {
  constexpr double kPi = 3.14159265358979323846;
  if (standard == Standard::kNtsc) {
    return line.burst_phase_rad + kPi + SubcarrierAnchorRad(standard);
  }
  return SubcarrierAnchorRad(standard);
}

}  // namespace videosynth
