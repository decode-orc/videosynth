/*
 * File:        variable_delay_interpolator.cpp
 * Module:      line_timing
 * Purpose:     Band-limited reading of a fixed-point mV signal at arbitrary
 *              fractional positions, for delays that vary sample by sample.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "videosynth/variable_delay_interpolator.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "videosynth/kaiser_window.h"

namespace videosynth {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kTaps = 2 * kVariableDelayHalfTaps;

// Exact at the integers, so a whole-sample position reads the sample itself
// rather than carrying sin(pi m) rounding residue from every tap.
double Sinc(double x) {
  if (x == std::round(x)) {
    return x == 0.0 ? 1.0 : 0.0;
  }
  return std::sin(kPi * x) / (kPi * x);
}

}  // namespace

VariableDelayInterpolator::VariableDelayInterpolator()
    : kernels_(static_cast<std::size_t>(kVariableDelayPhases + 1) * kTaps) {
  constexpr int kHalf = kVariableDelayHalfTaps;
  for (int phase = 0; phase <= kVariableDelayPhases; ++phase) {
    const double fraction =
        static_cast<double>(phase) / static_cast<double>(kVariableDelayPhases);
    double* kernel = &kernels_[static_cast<std::size_t>(phase) * kTaps];
    double dc_gain = 0.0;
    for (int m = -kHalf + 1; m <= kHalf; ++m) {
      // Tap at distance t from the read position, windowed over the
      // kernel's half-length.
      const double t = static_cast<double>(m) - fraction;
      const double tap = Sinc(t) * KaiserWindow(t / static_cast<double>(kHalf),
                                                kVariableDelayKaiserBeta);
      kernel[m + kHalf - 1] = tap;
      dc_gain += tap;
    }
    for (int i = 0; i < kTaps; ++i) {
      kernel[i] /= dc_gain;
    }
  }
}

double VariableDelayInterpolator::At(const SampleFixed* input,
                                     std::size_t count, double position) const {
  if (count == 0U) {
    return 0.0;
  }
  constexpr int kHalf = kVariableDelayHalfTaps;
  const double floor_position = std::floor(position);
  const auto base = static_cast<std::int64_t>(floor_position);
  const double scaled =
      (position - floor_position) * static_cast<double>(kVariableDelayPhases);
  const int phase = std::min(static_cast<int>(scaled), kVariableDelayPhases);
  const double blend = scaled - static_cast<double>(phase);
  const double* lower = &kernels_[static_cast<std::size_t>(phase) * kTaps];
  const double* upper = &kernels_[static_cast<std::size_t>(std::min(
                                      phase + 1, kVariableDelayPhases)) *
                                  kTaps];

  const auto last = static_cast<std::int64_t>(count) - 1;
  const std::int64_t first_index = base - kHalf + 1;
  double lower_sum = 0.0;
  double upper_sum = 0.0;
  if (first_index >= 0 && first_index + kTaps - 1 <= last) {
    const SampleFixed* window = input + first_index;
    for (int i = 0; i < kTaps; ++i) {
      const auto sample = static_cast<double>(window[i]);
      lower_sum += lower[i] * sample;
      upper_sum += upper[i] * sample;
    }
  } else {
    for (int i = 0; i < kTaps; ++i) {
      const std::int64_t index =
          std::clamp<std::int64_t>(first_index + i, 0, last);
      const auto sample =
          static_cast<double>(input[static_cast<std::size_t>(index)]);
      lower_sum += lower[i] * sample;
      upper_sum += upper[i] * sample;
    }
  }
  return lower_sum + (blend * (upper_sum - lower_sum));
}

}  // namespace videosynth
