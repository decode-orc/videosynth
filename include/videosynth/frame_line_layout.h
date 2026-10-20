/*
 * File:        frame_line_layout.h
 * Module:      frame_line_layout
 * Purpose:     Per-line sample counts and offsets within a synthesised frame,
 *              shared by the generation stage and the GUI preview.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <algorithm>
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

}  // namespace videosynth
