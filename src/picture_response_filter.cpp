/*
 * File:        picture_response_filter.cpp
 * Module:      generation
 * Purpose:     Separable linear-phase FIRs for the per-section picture
 *              response: a horizontal shaping of each active line and a
 *              vertical shaping across the rows of a progressive picture,
 *              both on fixed-point mV luma.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "videosynth/picture_response_filter.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <stdexcept>

#include "videosynth/noise_spectrum_filter.h"

namespace videosynth {

namespace {

constexpr double kTwoPi = 2.0 * 3.14159265358979323846;

// Frequency-sampled, Hann-windowed, zero-phase kernel of `taps` (odd) taps
// whose amplitude follows amplitude_db(cycles per sample), normalised to unit
// DC gain.
std::vector<double> DesignLinearPhaseKernel(
    const std::function<double(double)>& amplitude_db, int taps) {
  const int centre = (taps - 1) / 2;
  std::vector<double> amplitude(static_cast<std::size_t>(centre) + 1U);
  for (int k = 0; k <= centre; ++k) {
    const double cycles_per_sample =
        static_cast<double>(k) / static_cast<double>(taps);
    amplitude[static_cast<std::size_t>(k)] =
        std::pow(10.0, amplitude_db(cycles_per_sample) / 20.0);
  }

  std::vector<double> kernel(static_cast<std::size_t>(taps));
  double dc_gain = 0.0;
  for (int n = 0; n < taps; ++n) {
    const int offset = n - centre;
    double sum = amplitude[0];
    for (int k = 1; k <= centre; ++k) {
      sum += 2.0 * amplitude[static_cast<std::size_t>(k)] *
             std::cos(kTwoPi * static_cast<double>(k) *
                      static_cast<double>(offset) / static_cast<double>(taps));
    }
    const double window =
        0.5 - (0.5 * std::cos(kTwoPi * static_cast<double>(n + 1) /
                              static_cast<double>(taps + 1)));
    const double tap = (sum / static_cast<double>(taps)) * window;
    kernel[static_cast<std::size_t>(n)] = tap;
    dc_gain += tap;
  }
  for (double& tap : kernel) {
    tap /= dc_gain;
  }
  return kernel;
}

}  // namespace

std::vector<NoiseSpectrumPoint> DomesdayHorizontalResponseDb() {
  // Measured disc/bench band ratios (dB): 0-2.5 MHz 0, 2.5-3.0 -2.1,
  // 3.0-3.5 -2.2, 3.5-4.0 -2.2, 4.0-4.5 -3.6, 4.5-5.0 -7.7, 5.0-5.5 -9.8.
  // The breakpoints are fitted so that the designed filter's band-mean power
  // gain matches each figure to within 0.1 dB.
  return {{0.0, 0.0},   {2.25, 0.0},  {2.75, -2.2}, {3.75, -2.2},
          {4.25, -3.1}, {4.75, -8.4}, {5.25, -9.9}, {5.75, -11.0}};
}

std::vector<VerticalResponsePoint> DomesdayVerticalResponseDb() {
  // Measured disc/bench band ratios (dB): 1/4-5/16 -4.2, 5/16-3/8 -1.6,
  // 3/8-7/16 -9.5, 7/16-1/2 -10.4. A camera aperture or flicker filter falls
  // monotonically, so the curve is a monotonic fit: it meets the first, third
  // and fourth bands within 0.5 dB and treats the 5/16-3/8 figure as content
  // scatter of the redrawn card (the filter falls between its neighbours
  // there: -6.3 dB on a flat spectrum, -8.2 dB on Test Card F).
  return {{0.0, 0.0},   {0.2, 0.0},    {0.27, -4.5},
          {0.34, -5.0}, {0.38, -10.0}, {0.5, -10.4}};
}

std::vector<NoiseSpectrumPoint> ResolvedHorizontalResponseDb(
    const PictureResponseParameters& response) {
  switch (response.preset) {
    case PictureResponsePreset::kDomesday:
      return DomesdayHorizontalResponseDb();
    case PictureResponsePreset::kCustom:
      break;
  }
  return response.horizontal_db;
}

std::vector<VerticalResponsePoint> ResolvedVerticalResponseDb(
    const PictureResponseParameters& response) {
  switch (response.preset) {
    case PictureResponsePreset::kDomesday:
      return DomesdayVerticalResponseDb();
    case PictureResponsePreset::kCustom:
      break;
  }
  return response.vertical_db;
}

double InterpolateVerticalResponseDb(
    const std::vector<VerticalResponsePoint>& curve, double cycles_per_row) {
  if (curve.empty()) {
    return 0.0;
  }
  if (cycles_per_row <= curve.front().cycles_per_row) {
    return curve.front().relative_db;
  }
  for (std::size_t i = 1; i < curve.size(); ++i) {
    const VerticalResponsePoint& lower = curve[i - 1];
    const VerticalResponsePoint& upper = curve[i];
    if (cycles_per_row <= upper.cycles_per_row) {
      const double fraction = (cycles_per_row - lower.cycles_per_row) /
                              (upper.cycles_per_row - lower.cycles_per_row);
      return lower.relative_db +
             (fraction * (upper.relative_db - lower.relative_db));
    }
  }
  return curve.back().relative_db;
}

PictureResponseFilter::PictureResponseFilter(
    const PictureResponseParameters& response, double sample_rate_hz)
    : response_(response), sample_rate_hz_(sample_rate_hz) {
  if (!(sample_rate_hz > 0.0)) {
    throw std::invalid_argument(
        "PictureResponseFilter: sample rate must be positive");
  }
  const std::vector<NoiseSpectrumPoint> horizontal =
      ResolvedHorizontalResponseDb(response);
  if (!horizontal.empty()) {
    const double sample_rate_mhz = sample_rate_hz / 1.0e6;
    horizontal_kernel_ = DesignLinearPhaseKernel(
        [&](double cycles_per_sample) {
          return InterpolateNoiseSpectrumDb(
              horizontal, cycles_per_sample * sample_rate_mhz);
        },
        kPictureResponseHorizontalTaps);
  }
  const std::vector<VerticalResponsePoint> vertical =
      ResolvedVerticalResponseDb(response);
  if (!vertical.empty()) {
    vertical_kernel_ = DesignLinearPhaseKernel(
        [&](double cycles_per_row) {
          return InterpolateVerticalResponseDb(vertical, cycles_per_row);
        },
        kPictureResponseVerticalTaps);
  }
}

void PictureResponseFilter::ApplyHorizontal(const SampleFixed* in,
                                            std::size_t count,
                                            SampleFixed* out) const {
  if (count == 0U || horizontal_kernel_.empty()) {
    return;
  }
  const std::vector<double>& kernel = horizontal_kernel_;
  const auto half_taps = static_cast<std::ptrdiff_t>(kernel.size() / 2U);
  const auto last = static_cast<std::ptrdiff_t>(count) - 1;
  for (std::ptrdiff_t n = 0; n <= last; ++n) {
    double acc = 0.0;
    if (n >= half_taps && n + half_taps <= last) {
      const SampleFixed* const window = in + (n - half_taps);
      for (std::size_t k = 0; k < kernel.size(); ++k) {
        acc += kernel[k] * static_cast<double>(window[k]);
      }
    } else {
      for (std::size_t k = 0; k < kernel.size(); ++k) {
        const std::ptrdiff_t index = std::clamp<std::ptrdiff_t>(
            n - half_taps + static_cast<std::ptrdiff_t>(k), 0, last);
        acc += kernel[k] * static_cast<double>(in[index]);
      }
    }
    out[n] = static_cast<SampleFixed>(std::llround(acc));
  }
}

void PictureResponseFilter::ApplyVertical(const SampleFixed* in,
                                          std::size_t rows, std::size_t width,
                                          SampleFixed* out) const {
  if (rows == 0U || width == 0U || vertical_kernel_.empty()) {
    return;
  }
  const std::vector<double>& kernel = vertical_kernel_;
  const auto half_taps = static_cast<std::ptrdiff_t>(kernel.size() / 2U);
  const auto last = static_cast<std::ptrdiff_t>(rows) - 1;
  std::vector<double> acc(width);
  for (std::ptrdiff_t row = 0; row <= last; ++row) {
    std::fill(acc.begin(), acc.end(), 0.0);
    for (std::size_t k = 0; k < kernel.size(); ++k) {
      const std::ptrdiff_t source_row = std::clamp<std::ptrdiff_t>(
          row - half_taps + static_cast<std::ptrdiff_t>(k), 0, last);
      const SampleFixed* const source =
          in + (static_cast<std::size_t>(source_row) * width);
      const double tap = kernel[k];
      for (std::size_t x = 0; x < width; ++x) {
        acc[x] += tap * static_cast<double>(source[x]);
      }
    }
    SampleFixed* const destination =
        out + (static_cast<std::size_t>(row) * width);
    for (std::size_t x = 0; x < width; ++x) {
      destination[x] = static_cast<SampleFixed>(std::llround(acc[x]));
    }
  }
}

}  // namespace videosynth
