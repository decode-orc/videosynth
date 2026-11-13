/*
 * File:        lattice_delay_reference.h
 * Module:      test_support
 * Purpose:     Predicts the stored samples of a synthesised line from its
 *              0H-relative render, as the lattice line shifter delays it
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "videosynth/frame_line_layout.h"
#include "videosynth/model.h"
#include "videosynth/timing_offset_filter.h"

namespace videosynth::test_support {

// Stored samples first .. first + count - 1 (offsets from the start of frame
// line `line_1based`'s storage line) of a line whose 0H-relative render is
// render(j), in millivolts, for any line offset j. The render is delayed by
// the line's lattice delay through TimingOffsetFilter's kernel, as
// LatticeLineShifter does, so the prediction matches the generator to within
// fixed-point rounding wherever the line's neighbours add nothing (more than
// 16 samples from either end of the storage line). On NTSC and PAL-M it is
// render(j) itself.
//
// Complexity O(count x 32). Thread-safety: pure function.
template <typename Render>
std::vector<double> LatticeDelayedLine(Standard standard, int line_1based,
                                       int first, int count, Render render) {
  const TimingOffsetFilter filter(
      LatticeLineDelay(standard, line_1based).Samples());
  const auto shift = static_cast<int>(filter.integer_shift());
  const std::vector<double>& kernel = filter.kernel();
  std::vector<double> stored;
  stored.reserve(static_cast<std::size_t>(count));
  for (int n = first; n < first + count; ++n) {
    if (kernel.empty()) {
      stored.push_back(render(n - shift));
      continue;
    }
    // kernel[i] is h[m] for m = i - kTimingOffsetHalfTaps + 1.
    double value = 0.0;
    for (std::size_t i = 0; i < kernel.size(); ++i) {
      const int m = static_cast<int>(i) - kTimingOffsetHalfTaps + 1;
      value += kernel[i] * render(n - shift - m);
    }
    stored.push_back(value);
  }
  return stored;
}

}  // namespace videosynth::test_support
