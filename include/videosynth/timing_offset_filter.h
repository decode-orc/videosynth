/*
 * File:        timing_offset_filter.h
 * Module:      channel
 * Purpose:     Band-limited circular fractional delay of one frame of
 *              fixed-point mV samples (Kaiser-windowed sinc).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "videosynth/fixed_point.h"

namespace videosynth {

// Half-length of the fractional-delay kernel; the kernel has 2 * this taps.
// With kTimingOffsetKaiserBeta below, the delayed response stays within
// 0.001 dB and 0.0002 sample of ideal, with a complex error below -76 dB, up
// to 6 MHz at both the 625-line (17.73 MHz) and 525-line (14.32 MHz) 4fsc
// rates.
constexpr int kTimingOffsetHalfTaps = 16;
constexpr double kTimingOffsetKaiserBeta = 8.0;

// Delays a signal by offset_samples, positive meaning the signal arrives
// later: output[n] = input(n - offset_samples), wrapping circularly within the
// buffer. The integer part of the offset is an exact index shift; the
// fractional part is a Kaiser-windowed-sinc FIR whose taps are normalised to
// unit DC gain, so flat levels pass unchanged.
//
// Thread-safety: immutable after construction; Apply may be called
// concurrently on distinct buffers.
class TimingOffsetFilter {
 public:
  explicit TimingOffsetFilter(double offset_samples);

  // floor(offset_samples).
  std::int64_t integer_shift() const { return integer_shift_; }
  // offset_samples - floor(offset_samples), in [0, 1).
  double fraction() const { return fraction_; }

  // Taps h[m] for m = -kTimingOffsetHalfTaps + 1 .. kTimingOffsetHalfTaps,
  // applied as output[n] = sum h[m] * input[n - integer_shift - m]. Empty when
  // the fraction is zero.
  const std::vector<double>& kernel() const { return kernel_; }

  // Writes the delayed copy of input[0, count) to output[0, count). The
  // buffers must not overlap. Results are rounded to the nearest fixed-point
  // step and are not clamped.
  void Apply(const SampleFixed* input, std::size_t count,
             SampleFixed* output) const;

 private:
  std::int64_t integer_shift_;
  double fraction_;
  std::vector<double> kernel_;
};

}  // namespace videosynth
