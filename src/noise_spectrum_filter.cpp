/*
 * File:        noise_spectrum_filter.cpp
 * Module:      noise_injection
 * Purpose:     Designs the linear-phase FIR that shapes injected noise to a
 *              configured power-spectrum table.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "videosynth/noise_spectrum_filter.h"

#include <cmath>
#include <stdexcept>

namespace videosynth {

double InterpolateNoiseSpectrumDb(const std::vector<NoiseSpectrumPoint>& table,
                                  double frequency_mhz) {
  if (table.empty()) {
    return 0.0;
  }
  if (frequency_mhz <= table.front().frequency_mhz) {
    return table.front().relative_db;
  }
  for (std::size_t i = 1; i < table.size(); ++i) {
    const NoiseSpectrumPoint& lower = table[i - 1];
    const NoiseSpectrumPoint& upper = table[i];
    if (frequency_mhz <= upper.frequency_mhz) {
      const double span = upper.frequency_mhz - lower.frequency_mhz;
      const double fraction = (frequency_mhz - lower.frequency_mhz) / span;
      return lower.relative_db +
             (fraction * (upper.relative_db - lower.relative_db));
    }
  }
  return table.back().relative_db;
}

NoiseSpectrumFilter::NoiseSpectrumFilter(
    const std::vector<NoiseSpectrumPoint>& table, double sample_rate_hz) {
  if (!(sample_rate_hz > 0.0)) {
    throw std::invalid_argument(
        "NoiseSpectrumFilter: sample rate must be positive");
  }

  constexpr int kTaps = kNoiseSpectrumTaps;
  constexpr int kCentre = (kTaps - 1) / 2;
  constexpr double kTwoPi = 2.0 * 3.14159265358979323846;

  // Zero-phase amplitude on the DFT grid; bins 1..kCentre mirror onto
  // kTaps-1..kCentre+1, so only the lower half is stored.
  std::vector<double> amplitude(static_cast<std::size_t>(kCentre) + 1U);
  for (int k = 0; k <= kCentre; ++k) {
    const double frequency_mhz =
        (static_cast<double>(k) * sample_rate_hz / static_cast<double>(kTaps)) /
        1.0e6;
    amplitude[static_cast<std::size_t>(k)] =
        std::pow(10.0, InterpolateNoiseSpectrumDb(table, frequency_mhz) / 20.0);
  }

  kernel_.assign(static_cast<std::size_t>(kTaps), 0.0);
  double energy = 0.0;
  for (int n = 0; n < kTaps; ++n) {
    const int offset = n - kCentre;
    double sum = amplitude[0];
    for (int k = 1; k <= kCentre; ++k) {
      sum += 2.0 * amplitude[static_cast<std::size_t>(k)] *
             std::cos(kTwoPi * static_cast<double>(k) *
                      static_cast<double>(offset) / static_cast<double>(kTaps));
    }
    // Hann window with non-zero end taps, unity at the centre.
    const double window =
        0.5 - (0.5 * std::cos(kTwoPi * static_cast<double>(n + 1) /
                              static_cast<double>(kTaps + 1)));
    const double tap = (sum / static_cast<double>(kTaps)) * window;
    kernel_[static_cast<std::size_t>(n)] = tap;
    energy += tap * tap;
  }

  const double scale = 1.0 / std::sqrt(energy);
  for (double& tap : kernel_) {
    tap *= scale;
  }
}

void NoiseSpectrumFilter::Apply(const std::vector<double>& input,
                                std::size_t count,
                                std::vector<double>* output) const {
  const std::size_t taps = kernel_.size();
  if (input.size() + 1U < count + taps) {
    throw std::invalid_argument(
        "NoiseSpectrumFilter::Apply: input shorter than count + taps - 1");
  }
  output->resize(count);
  const double* kernel = kernel_.data();
  for (std::size_t s = 0; s < count; ++s) {
    const double* window = input.data() + s;
    double sum = 0.0;
    for (std::size_t j = 0; j < taps; ++j) {
      sum += kernel[j] * window[j];
    }
    (*output)[s] = sum;
  }
}

}  // namespace videosynth
