/*
 * File:        sync_edge_measurement.h
 * Module:      test_support
 * Purpose:     Measures the half-amplitude falling edge (0H) of every line's
 *              horizontal sync in a synthesised luma frame
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "videosynth/fixed_point.h"

namespace videosynth::test_support {

// Samples searched either side of a storage line's first sample for its
// line-start sync edge. The lattice delay is under 2 samples and the edge's
// half-amplitude point stays within a few samples of the line start, while
// the nearest other falling edge (a mid-line equalising or broad pulse) is
// half a line away.
constexpr int kSyncEdgeSearchBefore = 8;
constexpr int kSyncEdgeSearchAfter = 16;

// Position of the half-amplitude falling crossing of the horizontal sync that
// starts each line, in samples from the start of the frame.
//
// `luma` holds one frame's Y plane in millivolts (SampleFixed), and
// `line_offsets` the first-sample offset of each storage line
// (BuildLineSampleOffsets). The threshold is (blanking + sync tip) / 2, as
// ITU-R BT.1700 and SMPTE 170M define 0H, and the crossing is linearly
// interpolated between the two samples either side of it. The frame is treated
// as periodic, so the edge of line 1 may be found before sample 0 and is then
// returned as a negative position.
//
// Returns one entry per line; an entry is empty when no falling crossing lies
// in the search window. Complexity O(lines x window).
// Thread-safety: pure function.
inline std::vector<std::optional<double>> MeasureLineSyncEdges(
    const std::vector<SampleFixed>& luma, const std::vector<int>& line_offsets,
    double blanking_mv, double sync_tip_mv) {
  const double threshold = 0.5 * (blanking_mv + sync_tip_mv);
  const std::int64_t frame_samples = static_cast<std::int64_t>(luma.size());
  auto sample_mv = [&](std::int64_t index) {
    std::int64_t wrapped = index % frame_samples;
    if (wrapped < 0) {
      wrapped += frame_samples;
    }
    return SampleFixedToMillivolts(luma[static_cast<std::size_t>(wrapped)]);
  };

  std::vector<std::optional<double>> edges;
  edges.reserve(line_offsets.size());
  for (int offset : line_offsets) {
    std::optional<double> edge;
    for (std::int64_t i = offset - kSyncEdgeSearchBefore;
         i < offset + kSyncEdgeSearchAfter; ++i) {
      const double before = sample_mv(i);
      const double after = sample_mv(i + 1);
      if (before >= threshold && after < threshold) {
        edge =
            static_cast<double>(i) + ((before - threshold) / (before - after));
        break;
      }
    }
    edges.push_back(edge);
  }
  return edges;
}

}  // namespace videosynth::test_support
