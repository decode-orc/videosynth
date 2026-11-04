/*
 * File:        variable_delay_interpolator.h
 * Module:      line_timing
 * Purpose:     Band-limited reading of a fixed-point mV signal at arbitrary
 *              fractional positions, for delays that vary sample by sample.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <cstddef>
#include <vector>

#include "videosynth/fixed_point.h"

namespace videosynth {

// Half-length of the interpolation kernel (2 * this taps), and its Kaiser
// beta: the same kernel TimingOffsetFilter uses for a fixed delay, so a
// varying delay is as band-limited as a constant one.
constexpr int kVariableDelayHalfTaps = 16;
constexpr double kVariableDelayKaiserBeta = 8.0;
// Fractional positions tabulated between two samples. Kernels between two
// tabulated phases are interpolated linearly, which keeps the error of a
// band-limited signal below 1e-5 of its amplitude.
constexpr int kVariableDelayPhases = 512;

// Reads a signal at fractional sample positions through a Kaiser-windowed
// sinc kernel:
//   x(t) = sum_m h_a[m] x[floor(t) + m],  m = -15 .. 16,  a = t - floor(t),
// with h_a[m] = w(m - a) sinc(m - a) normalised to unit DC gain, so flat
// levels pass unchanged and an integer position returns the sample itself.
// Samples beyond [0, count) read as the nearest end sample.
//
// Complexity: O(kVariableDelayHalfTaps) per read; the table holds
// (kVariableDelayPhases + 1) x 2 * kVariableDelayHalfTaps doubles.
//
// Thread-safety: immutable after construction; At may be called
// concurrently.
class VariableDelayInterpolator {
 public:
  VariableDelayInterpolator();

  // The value of input[0, count) at position (in samples).
  double At(const SampleFixed* input, std::size_t count, double position) const;

 private:
  // kernels_[phase * taps + (m + half - 1)] is h_{phase / phases}[m].
  std::vector<double> kernels_;
};

}  // namespace videosynth
