/*
 * File:        luma_bandwidth_filter.h
 * Module:      generation
 * Purpose:     Kaiser-windowed-sinc luma low-pass for the per-section luma
 *              bandwidth limit, applied to one active line of fixed-point mV
 *              luma before it is painted.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <cstddef>
#include <vector>

#include "videosynth/fixed_point.h"

namespace videosynth {

// Design stopband attenuation in dB. Above the 60 dB the limit promises, so
// the fixed-point rounding of the output stays clear of it.
constexpr double kLumaBandwidthStopbandDb = 70.0;

// Symmetric low-pass FIR with an odd number of taps and unit DC gain.
//
// The passband edge is cutoff - transition / 2 and the stopband edge is
// cutoff + transition / 2; the response is -6 dB at the cutoff. The Kaiser
// window (beta from the stopband attenuation) gives a stopband at least
// kLumaBandwidthStopbandDb down and a passband ripple below 0.003 dB.
class LumaBandwidthFilter {
 public:
  LumaBandwidthFilter(double cutoff_hz, double transition_hz,
                      double sample_rate_hz);

  // Filters count samples of in into out (which must not alias in). Samples
  // beyond either end of the input are taken as copies of the end samples
  // (edge replication), so a flat line passes unchanged. Results are rounded
  // to the nearest fixed-point step.
  //
  // Thread-safety: const; safe to call concurrently on distinct buffers.
  void Apply(const SampleFixed* in, std::size_t count, SampleFixed* out) const;

  double cutoff_hz() const { return cutoff_hz_; }
  double transition_hz() const { return transition_hz_; }
  const std::vector<double>& kernel() const { return kernel_; }

 private:
  double cutoff_hz_;
  double transition_hz_;
  std::vector<double> kernel_;
};

}  // namespace videosynth
