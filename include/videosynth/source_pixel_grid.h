/*
 * File:        source_pixel_grid.h
 * Module:      generation
 * Purpose:     Positions of source pixels along the 4fsc active window, for
 *              the active-window stretch and the ITU-R BT.601 sampling grid.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <algorithm>
#include <cmath>

#include "videosynth/model.h"

namespace videosynth {

// ITU-R BT.601-5 Part A Table 2 item 4: 13.5 MHz luminance sampling for both
// line standards.
constexpr double kBt601SampleRateHz = 13.5e6;

// ITU-R BT.601-5 Part A Table 2 items 2, 6 and 7, and Appendix 1 Figs. 1-2:
// the digital active line is 720 samples of an 864-sample (625-line) or
// 858-sample (525-line) line, ending 12 or 16 samples before 0H, so its first
// sample is 864 - 720 - 12 = 132 or 858 - 720 - 16 = 122 samples after 0H.
constexpr int kBt601ZeroHToActiveSamples625 = 132;
constexpr int kBt601ZeroHToActiveSamples525 = 122;

// ITU-R BT.601-5 Part A Table 2 item 6: 720 luminance samples per digital
// active line on both line standards.
constexpr int kBt601ActiveLineSamples = 720;

// Affine map from an active-window sample index x to a source pixel
// coordinate u(x) = origin_pixels + x * pixels_per_sample, measured from the
// source's active_x, in which pixel k covers [k, k + 1) and is centred on
// k + 0.5. The nearest mapping holds pixel floor(u); the band-limited
// resampler interpolates at u - 0.5. Samples with u outside [0, W) lie
// beyond the source's active width W.
//
// Thread-safety: plain value type.
struct SourcePixelGrid {
  double origin_pixels = 0.0;
  double pixels_per_sample = 0.0;

  bool operator==(const SourcePixelGrid& other) const {
    return origin_pixels == other.origin_pixels &&
           pixels_per_sample == other.pixels_per_sample;
  }
};

// The active-window stretch: the source's active width fills the window, so
// u(x) = x * W / N.
inline SourcePixelGrid ActiveWindowSourcePixelGrid(int active_sample_count,
                                                   int source_active_width) {
  if (active_sample_count <= 0 || source_active_width <= 0) {
    return {};
  }
  return SourcePixelGrid{
      .origin_pixels = 0.0,
      .pixels_per_sample = static_cast<double>(source_active_width) /
                           static_cast<double>(active_sample_count),
  };
}

// The BT.601 grid: source pixel n is centred on the BT.601 sampling instant
// t0 + n / 13.5 MHz, where t0 is the start of the digital active line after
// 0H. Sample 0 of every 4fsc line sits at 0H, so active-window sample x is
// (active_window_start_samples + x) / sample_rate_hz after 0H.
inline SourcePixelGrid Bt601SourcePixelGrid(Standard standard,
                                            double sample_rate_hz,
                                            int active_window_start_samples) {
  if (sample_rate_hz <= 0.0) {
    return {};
  }
  const int zero_h_to_active = Is625LineStandard(standard)
                                   ? kBt601ZeroHToActiveSamples625
                                   : kBt601ZeroHToActiveSamples525;
  const double pixels_per_sample = kBt601SampleRateHz / sample_rate_hz;
  // Pixel 0 is centred (u = 0.5) on 0H + zero_h_to_active BT.601 periods.
  const double window_start_pixels =
      static_cast<double>(active_window_start_samples) * pixels_per_sample;
  return SourcePixelGrid{
      .origin_pixels =
          window_start_pixels - static_cast<double>(zero_h_to_active) + 0.5,
      .pixels_per_sample = pixels_per_sample,
  };
}

// The grid a section's horizontal_mapping selects.
inline SourcePixelGrid ResolveSourcePixelGrid(HorizontalMapping mapping,
                                              Standard standard,
                                              double sample_rate_hz,
                                              int active_window_start_samples,
                                              int active_sample_count,
                                              int source_active_width) {
  if (mapping == HorizontalMapping::kBt601) {
    return Bt601SourcePixelGrid(standard, sample_rate_hz,
                                active_window_start_samples);
  }
  return ActiveWindowSourcePixelGrid(active_sample_count, source_active_width);
}

// Source pixel coordinate of active-window sample x.
inline double SourcePixelCoordinate(const SourcePixelGrid& grid, int x) {
  return grid.origin_pixels + (static_cast<double>(x) * grid.pixels_per_sample);
}

// Number of leading active-window samples whose coordinate lies before the
// source's active width ends: the samples a source pixel paints. The grid is
// increasing, so the samples beyond the width form a suffix.
inline int SourcePixelGridInRangeSamples(const SourcePixelGrid& grid,
                                         int active_sample_count,
                                         int source_active_width) {
  if (grid.pixels_per_sample <= 0.0) {
    return active_sample_count;
  }
  for (int x = 0; x < active_sample_count; ++x) {
    if (SourcePixelCoordinate(grid, x) >=
        static_cast<double>(source_active_width)) {
      return x;
    }
  }
  return active_sample_count;
}

// The run of 4fsc samples, within a line, that a section paints with
// picture: start is the sample offset after 0H and samples the count.
//
// Thread-safety: plain value type.
struct ActiveSampleSpan {
  int start = 0;
  int samples = 0;

  int end() const { return start + samples; }
  bool operator==(const ActiveSampleSpan& other) const {
    return start == other.start && samples == other.samples;
  }
};

// The span a section paints. Every combination except bt601 with
// ActiveExtent::kSource paints the active window [window_start, window_end).
// That one paints the union of the window and the BT.601 digital active line,
// from the first sample inside pixel 0 (whose span starts half a pixel before
// t0) to the last sample inside pixel 719, so no source pixel the grid places
// on the line is cut. The span is clamped to [0, line_limit).
inline ActiveSampleSpan ResolveActiveSampleSpan(
    HorizontalMapping mapping, ActiveExtent extent, Standard standard,
    double sample_rate_hz, int window_start, int window_end, int line_limit) {
  int start = window_start;
  int end = window_end;
  if (mapping == HorizontalMapping::kBt601 && extent == ActiveExtent::kSource &&
      sample_rate_hz > 0.0) {
    const int zero_h_to_active = Is625LineStandard(standard)
                                     ? kBt601ZeroHToActiveSamples625
                                     : kBt601ZeroHToActiveSamples525;
    const double samples_per_pixel = sample_rate_hz / kBt601SampleRateHz;
    // Pixel n covers [t0 + n - 0.5, t0 + n + 0.5) BT.601 periods after 0H.
    const double first_edge =
        (static_cast<double>(zero_h_to_active) - 0.5) * samples_per_pixel;
    const double last_edge =
        (static_cast<double>(zero_h_to_active + kBt601ActiveLineSamples) -
         0.5) *
        samples_per_pixel;
    start = std::min(start, static_cast<int>(std::ceil(first_edge)));
    end = std::max(end, static_cast<int>(std::ceil(last_edge)));
  }
  start = std::clamp(start, 0, std::max(0, line_limit - 1));
  end = std::clamp(end, start + 1, std::max(start + 1, line_limit));
  return ActiveSampleSpan{.start = start, .samples = end - start};
}

}  // namespace videosynth
