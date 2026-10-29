/*
 * File:        frame_line_layout.h
 * Module:      frame_line_layout
 * Purpose:     Per-line sample counts and offsets, and the active picture
 *              raster, within a synthesised frame; shared by the generation
 *              stage, the impairment stages and the GUI preview.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

#include "videosynth/model.h"

namespace videosynth {

// Thread-safety: all functions in this module are thread-safe pure functions.

// Sample count of each line (0-based index = line number - 1) in a frame.
inline std::vector<int> BuildLineSampleCounts(Standard standard,
                                              int lines_per_frame,
                                              int nominal_samples) {
  std::vector<int> counts(static_cast<std::size_t>(lines_per_frame),
                          nominal_samples);
  if (Is625LineStandard(standard)) {
    // EBU Tech. 3280-E Section 1.2: 625-line PAL at 4fsc has 1135.0064
    // samples/line average, i.e., 709,379 samples/frame. The normative
    // placement of the four extra samples per frame is two on line 313 and two
    // on line 625.
    constexpr int kLongLines[] = {313, 625};
    for (int line_1based : kLongLines) {
      counts[static_cast<std::size_t>(line_1based - 1)] += 2;
    }
  }
  return counts;
}

// Sample offset of each line's first sample within the frame buffer.
inline std::vector<int> BuildLineSampleOffsets(
    const std::vector<int>& line_samples) {
  std::vector<int> offsets(line_samples.size(), 0);
  int running = 0;
  for (std::size_t i = 0; i < line_samples.size(); ++i) {
    offsets[i] = running;
    running += line_samples[i];
  }
  return offsets;
}

inline int MaxLineSamples(const std::vector<int>& line_samples) {
  int max_samples = 0;
  for (int samples : line_samples) {
    max_samples = std::max(max_samples, samples);
  }
  return max_samples;
}

// The active picture raster of a 4fsc frame: which frame lines carry picture
// and the sample window within each of them.
struct ActiveRasterGeometry {
  int first_active_line_field1 = 0;
  int first_active_line_field2 = 0;
  int active_lines_per_field = 0;
  int active_window_start_samples = 0;
  int active_window_end_samples = 0;
  int active_width_pixels = 720;
  // Frame line whose picture stops part-way along the line (0 when every
  // active line carries a full line of picture), and the sample offset
  // (exclusive) at which that picture ends.
  int half_picture_line = 0;
  int half_picture_end_samples = 0;
};

inline ActiveRasterGeometry GetActiveRasterGeometry(Standard standard,
                                                    double sample_rate_hz) {
  if (Is625LineStandard(standard)) {
    // ITU-R BT.1700 Annex 1 Part B Table 3: PAL line timing, mapped through
    // BT.601's 13.5 MHz sampling model. Keep the established PAL line start
    // anchor at +177 4fsc samples, but limit active-picture synthesis to
    // the 52.0 us visible-aperture duration.
    const int active_window_start = 177;
    const int active_window_end =
        active_window_start +
        std::max(1, static_cast<int>(std::lround(sample_rate_hz * 52.0e-6)));
    // ITU-R BT.1700 Annex 1 Part B Table 2 item j: the field-blanking
    // interval is 25H + a. Before field 2 it starts at line 311, so field 2
    // picture resumes on line 336; before field 1 it starts at mid-line 623,
    // so field 2 picture runs to the first half of line 623 (288 lines per
    // field, Table 1 item 1a). Item K: blanking leads the first equalizing
    // pulse (at half-line) by 3 us, so line 623 picture ends 29 us after 0H.
    constexpr double kHalfPictureEndSeconds = 32.0e-6 - 3.0e-6;
    return ActiveRasterGeometry{
        .first_active_line_field1 = 23,
        .first_active_line_field2 = 336,
        .active_lines_per_field = 288,
        .active_window_start_samples = active_window_start,
        .active_window_end_samples = active_window_end,
        .active_width_pixels = 720,
        .half_picture_line = 623,
        .half_picture_end_samples = static_cast<int>(
            std::lround(sample_rate_hz * kHalfPictureEndSeconds)),
    };
  }

  const int active_window_start =
      std::max(0, static_cast<int>(std::lround(sample_rate_hz * 10.5e-6)));
  const int active_window_end =
      std::max(active_window_start + 1,
               static_cast<int>(std::lround(sample_rate_hz * 62.5e-6)));

  return ActiveRasterGeometry{
      .first_active_line_field1 = 22,
      .first_active_line_field2 = 284,
      .active_lines_per_field = 240,
      .active_window_start_samples = active_window_start,
      .active_window_end_samples = active_window_end,
      .active_width_pixels = 720,
  };
}

// Sample offset (exclusive) within the line at which its picture ends.
inline int ActivePictureEndSamples(const ActiveRasterGeometry& geometry,
                                   int line_1based) {
  if (geometry.half_picture_line != 0 &&
      line_1based == geometry.half_picture_line) {
    return geometry.half_picture_end_samples;
  }
  return geometry.active_window_end_samples;
}

// Index of a frame line within the active raster (field 1 rows first), or -1
// when the line carries no picture.
inline int ActiveFrameLineIndex(const ActiveRasterGeometry& geometry,
                                int line_1based) {
  if (line_1based >= geometry.first_active_line_field1 &&
      line_1based < (geometry.first_active_line_field1 +
                     geometry.active_lines_per_field)) {
    return line_1based - geometry.first_active_line_field1;
  }
  if (line_1based >= geometry.first_active_line_field2 &&
      line_1based < (geometry.first_active_line_field2 +
                     geometry.active_lines_per_field)) {
    return geometry.active_lines_per_field +
           (line_1based - geometry.first_active_line_field2);
  }

  return -1;
}

}  // namespace videosynth
