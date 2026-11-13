/*
 * File:        video_band_limit.cpp
 * Module:      generation
 * Purpose:     Always-on band limit of active picture luma and OSD overlays
 *              to the standard's nominal video bandwidth.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "videosynth/video_band_limit.h"

#include <cmath>

namespace videosynth {

VideoBandwidth NominalVideoBandwidth(Standard standard) {
  if (standard == Standard::kNtsc || standard == Standard::kPalM) {
    return VideoBandwidth{.passband_edge_hz = 4.2e6, .stopband_edge_hz = 6.5e6};
  }
  return VideoBandwidth{.passband_edge_hz = 6.0e6, .stopband_edge_hz = 8.0e6};
}

namespace {

LumaBandwidthFilter DesignFilter(Standard standard, double sample_rate_hz) {
  const VideoBandwidth band = NominalVideoBandwidth(standard);
  return LumaBandwidthFilter(
      0.5 * (band.passband_edge_hz + band.stopband_edge_hz),
      band.stopband_edge_hz - band.passband_edge_hz, sample_rate_hz);
}

}  // namespace

VideoBandLimiter::VideoBandLimiter(Standard standard, double sample_rate_hz)
    : filter_(DesignFilter(standard, sample_rate_hz)),
      reach_(static_cast<int>(filter_.kernel().size() / 2U)) {}

void VideoBandLimiter::Apply(const SampleFixed* in, std::size_t count,
                             SampleFixed background, Workspace* workspace,
                             SampleFixed* out) const {
  std::vector<double>* const scratch = &workspace->padded;
  std::vector<double>* const accumulator = &workspace->accumulator;
  const auto reach = static_cast<std::size_t>(reach_);
  const std::size_t out_count = count + (2U * reach);
  // The input less the background, with 2 * reach zeros either side, so
  // every output sums whole kernels without bounds checks.
  scratch->assign(out_count + (2U * reach), 0.0);
  double* const padded = scratch->data() + (2U * reach);
  for (std::size_t i = 0; i < count; ++i) {
    padded[i] = static_cast<double>(in[i] - background);
  }
  // out[k] is centred on input position k - reach, whose padded index is
  // k + reach, so its kernel spans padded indices k .. k + 2 * reach. Taking
  // the taps in the outer loop leaves a contiguous inner loop over k that
  // the compiler vectorises, while each output still sums its taps in order.
  const std::vector<double>& kernel = filter_.kernel();
  std::vector<double>& acc = *accumulator;
  acc.assign(out_count, 0.0);
  for (std::size_t tap = 0; tap < kernel.size(); ++tap) {
    const double weight = kernel[tap];
    const double* const window = scratch->data() + tap;
    for (std::size_t k = 0; k < out_count; ++k) {
      acc[k] += weight * window[k];
    }
  }
  for (std::size_t k = 0; k < out_count; ++k) {
    out[k] = background + static_cast<SampleFixed>(std::llround(acc[k]));
  }
}

}  // namespace videosynth
