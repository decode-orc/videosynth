/*
 * File:        luma_bandwidth_filter.cpp
 * Module:      generation
 * Purpose:     Kaiser-windowed-sinc luma low-pass for the per-section luma
 *              bandwidth limit, applied to one active line of fixed-point mV
 *              luma before it is painted.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "videosynth/luma_bandwidth_filter.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

#include "videosynth/kaiser_window.h"

namespace videosynth {

namespace {

constexpr double kPi = 3.14159265358979323846;

}  // namespace

LumaBandwidthFilter::LumaBandwidthFilter(double cutoff_hz, double transition_hz,
                                         double sample_rate_hz)
    : cutoff_hz_(cutoff_hz), transition_hz_(transition_hz) {
  // Kaiser's design formulas for the window shape and the length.
  const double attenuation_db = kLumaBandwidthStopbandDb;
  const double beta = 0.1102 * (attenuation_db - 8.7);
  const double transition_rad = 2.0 * kPi * transition_hz / sample_rate_hz;
  const auto half_taps = static_cast<std::size_t>(
      std::ceil((attenuation_db - 7.95) / (2.285 * transition_rad) / 2.0));

  const double normalised_cutoff = cutoff_hz / sample_rate_hz;
  kernel_.resize((2U * half_taps) + 1U);
  double sum = 0.0;
  for (std::size_t i = 0; i < kernel_.size(); ++i) {
    const double n = static_cast<double>(i) - static_cast<double>(half_taps);
    const double ideal =
        (n == 0.0) ? 2.0 * normalised_cutoff
                   : std::sin(2.0 * kPi * normalised_cutoff * n) / (kPi * n);
    const double ratio =
        (half_taps == 0U) ? 0.0 : n / static_cast<double>(half_taps);
    kernel_[i] = ideal * KaiserWindow(ratio, beta);
    sum += kernel_[i];
  }
  for (double& tap : kernel_) {
    tap /= sum;
  }
}

void LumaBandwidthFilter::Apply(const SampleFixed* in, std::size_t count,
                                SampleFixed* out) const {
  if (count == 0U) {
    return;
  }
  const auto half_taps = static_cast<std::ptrdiff_t>(kernel_.size() / 2U);
  const auto last = static_cast<std::ptrdiff_t>(count) - 1;
  for (std::ptrdiff_t n = 0; n <= last; ++n) {
    double acc = 0.0;
    if (n >= half_taps && n + half_taps <= last) {
      const SampleFixed* const window = in + (n - half_taps);
      for (std::size_t k = 0; k < kernel_.size(); ++k) {
        acc += kernel_[k] * static_cast<double>(window[k]);
      }
    } else {
      for (std::size_t k = 0; k < kernel_.size(); ++k) {
        const std::ptrdiff_t index = std::clamp<std::ptrdiff_t>(
            n - half_taps + static_cast<std::ptrdiff_t>(k), 0, last);
        acc += kernel_[k] * static_cast<double>(in[index]);
      }
    }
    out[n] = static_cast<SampleFixed>(std::llround(acc));
  }
}

}  // namespace videosynth
