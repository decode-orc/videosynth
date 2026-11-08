/*
 * File:        horizontal_resampler.h
 * Module:      generation
 * Purpose:     Polyphase Kaiser-windowed-sinc resampler that maps one row of
 *              source pixels onto the 4fsc active window without the images
 *              a zero-order hold leaves behind.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "videosynth/source_pixel_grid.h"

namespace videosynth {

// Design stopband attenuation in dB, matching the luma bandwidth limit so the
// fixed-point rounding of the output stays clear of it.
constexpr double kHorizontalResampleStopbandDb = 70.0;

// -6 dB point of the interpolation kernel in cycles per source pixel: the
// source Nyquist frequency (6.75 MHz for a 13.5 MHz BT.601 raster).
constexpr double kHorizontalResampleCutoffCyclesPerPixel = 0.5;

// Full width of the transition band in cycles per source pixel, centred on
// the cutoff. The image of every passband frequency f lands at 1 - f, which
// is then inside the stopband, so only transition-band content leaves an
// image (at most -6 dB, at the source Nyquist frequency itself).
constexpr double kHorizontalResampleTransitionCyclesPerPixel = 0.1;

// Band-limited source-row to active-window mapping.
//
// Output sample x of an N-sample active window reads the source row at pixel
// position u(x) - 0.5, where u is the section's SourcePixelGrid coordinate
// (measured from the source's active_x). The nearest mapping holds pixel k for
// the samples with k <= u(x) < k + 1, so pixel k's value is centred on the
// middle of that span and edges land on the same samples under both mappings.
// For the active-window stretch u(x) = x * W / N (W = source active width).
//
// Each output sample has its own phase of a Kaiser-windowed sinc kernel,
// designed once at construction and normalised to unit DC gain, so a flat
// row passes unchanged. Taps that fall outside the source's active raster
// read the nearest edge pixel (edge replication), as the other line filters
// do.
//
// Complexity: construction O(N × taps); Apply O(N × taps).
//
// Thread-safety: immutable after construction; Apply is const and safe to
// call concurrently on distinct output buffers.
class BandlimitedHorizontalResampler {
 public:
  // Args:
  //   active_window_samples: N, the number of active-window output samples.
  //   source_active_width:   W, the source's active width in pixels.
  //   source_active_x:       First active source column.
  //   source_width:          Raster width; taps never read beyond it.
  //   grid:                  Source pixel position of every sample.
  BandlimitedHorizontalResampler(int active_window_samples,
                                 int source_active_width, int source_active_x,
                                 int source_width, const SourcePixelGrid& grid);

  // The active-window stretch (ActiveWindowSourcePixelGrid).
  BandlimitedHorizontalResampler(int active_window_samples,
                                 int source_active_width, int source_active_x,
                                 int source_width)
      : BandlimitedHorizontalResampler(
            active_window_samples, source_active_width, source_active_x,
            source_width,
            ActiveWindowSourcePixelGrid(active_window_samples,
                                        source_active_width)) {}

  bool Matches(int active_window_samples, int source_active_width,
               int source_active_x, int source_width,
               const SourcePixelGrid& grid) const {
    return active_window_samples == active_window_samples_ &&
           source_active_width == source_active_width_ &&
           source_active_x == source_active_x_ &&
           source_width == source_width_ && grid == grid_;
  }

  // Resamples one row. row is indexed by absolute source column and holds at
  // least source_width values; out receives active_window_samples values,
  // each rounded to the nearest integer step of T.
  template <typename T>
  void Apply(const T* row, T* out) const {
    const std::size_t taps = static_cast<std::size_t>(taps_per_sample_);
    for (std::size_t x = 0;
         x < static_cast<std::size_t>(active_window_samples_); ++x) {
      const double* const weights = weights_.data() + (x * taps);
      const int* const columns = columns_.data() + (x * taps);
      double acc = 0.0;
      for (std::size_t k = 0; k < taps; ++k) {
        acc += weights[k] * static_cast<double>(row[columns[k]]);
      }
      out[x] = static_cast<T>(std::llround(acc));
    }
  }

  int taps_per_sample() const { return taps_per_sample_; }

  // The source column and weight of tap k of output sample x; for tests.
  int column(int x, int k) const {
    return columns_[(static_cast<std::size_t>(x) * taps_per_sample_) +
                    static_cast<std::size_t>(k)];
  }
  double weight(int x, int k) const {
    return weights_[(static_cast<std::size_t>(x) * taps_per_sample_) +
                    static_cast<std::size_t>(k)];
  }

 private:
  int active_window_samples_;
  int source_active_width_;
  int source_active_x_;
  int source_width_;
  SourcePixelGrid grid_;
  int taps_per_sample_ = 0;
  // Row-major [sample][tap] weights and the (edge-clamped) source columns
  // they apply to.
  std::vector<double> weights_;
  std::vector<int> columns_;
};

}  // namespace videosynth
