/*
 * File:        noise_spectrum_filter.h
 * Module:      noise_injection
 * Purpose:     Designs the linear-phase FIR that shapes injected noise to a
 *              configured power-spectrum table.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <cstddef>
#include <vector>

#include "videosynth/model.h"

namespace videosynth {

// Kernel length of every noise spectrum filter. Odd, so the kernel is
// linear-phase with an integer centre. At 4fsc the frequency-sampling grid is
// fs / 255: about 70 kHz for PAL (17.73 MHz) and 56 kHz for NTSC
// (14.32 MHz). The Hann window widens each breakpoint transition to roughly
// four grid steps, which is far finer than any channel response.
constexpr int kNoiseSpectrumTaps = 255;

// Returns the table's relative level in dB at frequency_mhz. Breakpoints are
// joined by straight lines in dB against MHz; the first and last values are
// held below the first and above the last breakpoint. An empty table is
// flat (0 dB).
//
// Thread-safety: Stateless; safe to call concurrently.
double InterpolateNoiseSpectrumDb(const std::vector<NoiseSpectrumPoint>& table,
                                  double frequency_mhz);

// A linear-phase FIR whose amplitude response follows a spectrum table, for
// shaping unit-variance white noise.
//
// Design (once, at construction):
//   1. Sample the amplitude 10^(dB/20) of the interpolated table on the
//      kNoiseSpectrumTaps-point DFT grid from 0 Hz to Nyquist.
//   2. Inverse real DFT of that zero-phase response, centred on the middle
//      tap (direct evaluation; the kernel is small).
//   3. Hann window, to suppress the ripple of the frequency-sampled design.
//   4. Normalise to sum(h^2) = 1, so filtered unit-variance white noise keeps
//      unit variance and the layer's noise_db is unchanged by the shape.
//
// A flat table therefore yields a unit delta at the centre tap.
//
// Complexity: construction is O(taps^2); Apply is O(count * taps).
//
// Thread-safety: Immutable after construction; Apply and kernel() are safe to
// call concurrently.
class NoiseSpectrumFilter {
 public:
  // Throws std::invalid_argument when sample_rate_hz is not positive.
  NoiseSpectrumFilter(const std::vector<NoiseSpectrumPoint>& table,
                      double sample_rate_hz);

  const std::vector<double>& kernel() const { return kernel_; }

  // Valid convolution: writes count outputs from count + taps - 1 inputs, so
  // every output sees a full kernel and there are no edge effects.
  //
  // Args:
  //   input:  At least count + kNoiseSpectrumTaps - 1 samples.
  //   count:  Number of outputs.
  //   output: Receives count samples (resized).
  void Apply(const std::vector<double>& input, std::size_t count,
             std::vector<double>* output) const;

 private:
  std::vector<double> kernel_;
};

}  // namespace videosynth
