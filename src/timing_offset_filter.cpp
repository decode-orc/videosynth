/*
 * File:        timing_offset_filter.cpp
 * Module:      channel
 * Purpose:     Band-limited circular fractional delay of one frame of
 *              fixed-point mV samples (Kaiser-windowed sinc).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "videosynth/timing_offset_filter.h"

#include <algorithm>
#include <cmath>

#include "videosynth/kaiser_window.h"

namespace videosynth {

namespace {

constexpr double kPi = 3.14159265358979323846;

double Sinc(double x) {
  if (x == 0.0) {
    return 1.0;
  }
  return std::sin(kPi * x) / (kPi * x);
}

// Index into a circular buffer of length count, for any signed position.
std::size_t WrapIndex(std::int64_t position, std::size_t count) {
  const auto length = static_cast<std::int64_t>(count);
  std::int64_t wrapped = position % length;
  if (wrapped < 0) {
    wrapped += length;
  }
  return static_cast<std::size_t>(wrapped);
}

}  // namespace

TimingOffsetFilter::TimingOffsetFilter(double offset_samples)
    : integer_shift_(static_cast<std::int64_t>(std::floor(offset_samples))),
      fraction_(offset_samples - std::floor(offset_samples)) {
  if (fraction_ == 0.0) {
    return;
  }

  constexpr int kHalf = kTimingOffsetHalfTaps;
  kernel_.reserve(2U * static_cast<std::size_t>(kHalf));
  double dc_gain = 0.0;
  for (int m = -kHalf + 1; m <= kHalf; ++m) {
    // Ideal delayed-sinc tap at distance t from the delayed sample instant,
    // windowed over the kernel's half-length.
    const double t = static_cast<double>(m) - fraction_;
    const double ratio = t / static_cast<double>(kHalf);
    const double window = KaiserWindow(ratio, kTimingOffsetKaiserBeta);
    kernel_.push_back(Sinc(t) * window);
    dc_gain += kernel_.back();
  }
  for (double& tap : kernel_) {
    tap /= dc_gain;
  }
}

void TimingOffsetFilter::Apply(const SampleFixed* input, std::size_t count,
                               SampleFixed* output) const {
  if (count == 0U) {
    return;
  }

  if (kernel_.empty()) {
    for (std::size_t n = 0; n < count; ++n) {
      output[n] = input[WrapIndex(static_cast<std::int64_t>(n) - integer_shift_,
                                  count)];
    }
    return;
  }

  // ext[j] = input[(j - integer_shift - kHalf) mod count], so output[n] reads
  // the contiguous window ext[n .. n + 2*kHalf - 1] with the kernel reversed:
  // output[n] = sum_r h[kHalf - r] * ext[n + r].
  constexpr int kHalf = kTimingOffsetHalfTaps;
  const std::size_t taps = kernel_.size();
  std::vector<double> ext(count + taps - 1U);
  for (std::size_t j = 0; j < ext.size(); ++j) {
    ext[j] = static_cast<double>(input[WrapIndex(
        static_cast<std::int64_t>(j) - integer_shift_ - kHalf, count)]);
  }
  std::vector<double> reversed(kernel_.rbegin(), kernel_.rend());
  // kernel_[i] is h[i - kHalf + 1]; reversed[r] = h[kHalf - r].
  for (std::size_t n = 0; n < count; ++n) {
    const double* window = &ext[n];
    double acc = 0.0;
    for (std::size_t r = 0; r < taps; ++r) {
      acc += reversed[r] * window[r];
    }
    output[n] = static_cast<SampleFixed>(std::llround(acc));
  }
}

}  // namespace videosynth
