/*
 * File:        horizontal_resampler.cpp
 * Module:      generation
 * Purpose:     Polyphase Kaiser-windowed-sinc resampler that maps one row of
 *              source pixels onto the 4fsc active window without the images
 *              a zero-order hold leaves behind.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "videosynth/horizontal_resampler.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

#include "videosynth/kaiser_window.h"

namespace videosynth {

namespace {

constexpr double kPi = 3.14159265358979323846;

}  // namespace

BandlimitedHorizontalResampler::BandlimitedHorizontalResampler(
    int active_window_samples, int source_active_width, int source_active_x,
    int source_width)
    : active_window_samples_(active_window_samples),
      source_active_width_(source_active_width),
      source_active_x_(source_active_x),
      source_width_(source_width) {
  const int first_column = std::max(0, source_active_x);
  const int last_column =
      std::min(source_active_x + source_active_width, source_width) - 1;
  if (active_window_samples <= 0 || last_column < first_column) {
    return;
  }

  // Kaiser's design formulas for the window shape and the half-length, with
  // the transition width expressed in radians per source pixel.
  const double attenuation_db = kHorizontalResampleStopbandDb;
  const double beta = 0.1102 * (attenuation_db - 8.7);
  const double transition_rad =
      2.0 * kPi * kHorizontalResampleTransitionCyclesPerPixel;
  const double half_length_pixels =
      std::ceil((attenuation_db - 7.95) / (2.285 * transition_rad) / 2.0);
  // A window of half-length L pixels spans 2L + 1 pixel positions around any
  // fractional centre, rounded up to whole columns on each side.
  taps_per_sample_ = (2 * static_cast<int>(half_length_pixels)) + 2;

  const std::size_t taps = static_cast<std::size_t>(taps_per_sample_);
  weights_.assign(static_cast<std::size_t>(active_window_samples) * taps, 0.0);
  columns_.assign(static_cast<std::size_t>(active_window_samples) * taps,
                  first_column);

  const double pixels_per_sample = static_cast<double>(source_active_width) /
                                   static_cast<double>(active_window_samples);
  const double cutoff = kHorizontalResampleCutoffCyclesPerPixel;
  for (int x = 0; x < active_window_samples; ++x) {
    // Pixel-index position of this sample, relative to source_active_x.
    const double position = (static_cast<double>(x) * pixels_per_sample) - 0.5;
    const int first_tap_pixel =
        static_cast<int>(std::floor(position)) - (taps_per_sample_ / 2) + 1;
    double sum = 0.0;
    for (int k = 0; k < taps_per_sample_; ++k) {
      const int pixel = first_tap_pixel + k;
      const double distance = position - static_cast<double>(pixel);
      const double ideal =
          (distance == 0.0)
              ? 2.0 * cutoff
              : std::sin(2.0 * kPi * cutoff * distance) / (kPi * distance);
      const double ratio = distance / half_length_pixels;
      const double weight =
          (std::abs(ratio) >= 1.0) ? 0.0 : ideal * KaiserWindow(ratio, beta);
      const std::size_t slot =
          (static_cast<std::size_t>(x) * taps) + static_cast<std::size_t>(k);
      weights_[slot] = weight;
      columns_[slot] =
          std::clamp(source_active_x + pixel, first_column, last_column);
      sum += weight;
    }
    for (int k = 0; k < taps_per_sample_; ++k) {
      weights_[(static_cast<std::size_t>(x) * taps) +
               static_cast<std::size_t>(k)] /= sum;
    }
  }
}

}  // namespace videosynth
