/*
 * File:        biphase_encoder.h
 * Module:      biphase_encoder
 * Purpose:     24-bit biphase (Manchester) signal generation for LaserDisc
 *              VBI injection per IEC 60856 (PAL) and IEC 60857 (NTSC).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "videosynth/fixed_point.h"
#include "videosynth/model.h"

namespace videosynth {

// Encodes 24-bit biphase (Manchester) signals for LaserDisc VBI injection.
//
// Encoding rules (IEC 60856/60857):
//   - Bit cell duration: 2.0 µs ± 0.01 µs. The 2.0 µs cell is not a whole
//     number of 4fsc samples, so cell boundaries and centres sit on the exact
//     grid rounded to the nearest sample (BitCellStart): each transition is
//     within half a sample of its nominal time and the code length does not
//     drift.
//   - Positive (rising) transition at center of bit cell = logic '1'
//   - Negative (falling) transition at center of bit cell = logic '0'
//   - Inter-bit transitions occur between consecutive identical bits
//   - Transition time: 225 ns ± 25 ns (10%-90%), shaped with S-curve
//   - Key nibble (first 4 bits of 24-bit code) always begins with logic '1'
//
// Signal levels are caller-supplied in millivolts:
//   - PAL:  baseline = 0 mV (blanking), peak = 700 mV (100% white; IEC 60856
//           §10.1 "30%–100%" is the allowed range for the high level)
//   - NTSC: baseline = 0 mV (0 IRE),    peak = 714.3 mV (100 IRE)
//
// Thread-safety: BiphaseEncoder is immutable after construction and may be
// called concurrently from multiple threads.
class BiphaseEncoder {
 public:
  // Constructs a BiphaseEncoder for the given sample rate.
  //
  // Args:
  //   sample_rate_hz:        Output sample rate in Hz (e.g. 17734475 for PAL
  //                          4fsc or 14318180 for NTSC 4fsc).
  //   bit_cell_duration_us:  Bit cell duration in microseconds (default 2.0).
  //   transition_duration_ns: 10%-90% transition time in nanoseconds
  //                            (default 225.0 for 24-bit biphase).
  explicit BiphaseEncoder(double sample_rate_hz,
                          double bit_cell_duration_us = 2.0,
                          double transition_duration_ns = 225.0);

  // Returns samples for a full video line with the 24-bit biphase signal
  // placed at the start of the returned buffer. The remainder of the buffer
  // is filled with baseline_level_mv.
  //
  // The returned vector length equals
  // GetTimingConstants(standard).samples_per_line_4fsc.
  //
  // Args:
  //   hex_code:           6-character hex string (e.g. "88FFFF") optionally
  //                       prefixed with "0x".
  //   standard:           PAL or NTSC (determines line buffer length).
  //   baseline_level_mv:  Signal low level in millivolts.
  //   peak_level_mv:      Signal high level in millivolts.
  //
  // Throws std::invalid_argument if hex_code is not a valid 6-digit hex code.
  std::vector<SampleFixed> GenerateLine(const std::string& hex_code,
                                        Standard standard,
                                        double baseline_level_mv,
                                        double peak_level_mv) const;

  // Returns samples for the 24-bit biphase waveform.
  //
  // The returned vector length is BitCellStart(24).
  // Bits are transmitted MSB first; the first bit corresponds to bit 23
  // of code_value.
  //
  // Args:
  //   code_value:         24-bit code (bits 23..0).
  //   baseline_level_mv:  Signal low level in millivolts.
  //   peak_level_mv:      Signal high level in millivolts.
  std::vector<SampleFixed> Generate24BitCode(uint32_t code_value,
                                             double baseline_level_mv,
                                             double peak_level_mv) const;

  // Returns samples for a single bit cell.
  //
  // The returned vector length is bit_cell_samples().
  // A '1' bit starts at baseline and rises to peak at the cell centre.
  // A '0' bit starts at peak and falls to baseline at the cell centre.
  // No inter-bit transition is included; Generate24BitCode handles those.
  //
  // Args:
  //   bit_value:          Logic value to encode (true = '1', false = '0').
  //   baseline_level_mv:  Signal low level in millivolts.
  //   peak_level_mv:      Signal high level in millivolts.
  std::vector<SampleFixed> GenerateBit(bool bit_value, double baseline_level_mv,
                                       double peak_level_mv) const;

  // Bit cell length rounded to whole samples; the length of GenerateBit's
  // single cell. Cells inside a code are this length ± 1 (see BitCellStart).
  int bit_cell_samples() const { return bit_cell_samples_; }

  // Exact bit cell length in samples (may be fractional).
  double bit_cell_exact() const { return bit_cell_exact_; }

  // First sample of bit cell bit_index within a code: bit_index × the exact
  // cell length, rounded to the nearest sample. BitCellStart(24) is the code
  // length.
  int BitCellStart(int bit_index) const;

  // Sample of bit cell bit_index's centre transition, rounded the same way.
  int BitCellCentre(int bit_index) const;

  // Number of samples spanning the full 0%-100% S-curve transition.
  // The 10%-90% measured transition time equals transition_duration_ns.
  int ramp_samples() const { return ramp_samples_; }

 private:
  int ramp_samples_;
  double bit_cell_exact_;
  int bit_cell_samples_;

  // Applies a single shaped step transition centred at center_sample.
  // rising == true:  baseline_level_mv → peak_level_mv (positive step)
  // rising == false: peak_level_mv → baseline_level_mv (negative step)
  // Uses ShapedPulseLevel from signal_shaping.h for the S-curve edge.
  void ApplyStepTransition(std::vector<double>& samples, int center_sample,
                           bool rising, double baseline_level_mv,
                           double peak_level_mv) const;
};

}  // namespace videosynth
