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

// Sample `index` of a periodic frame, in millivolts.
inline double PeriodicSampleMv(const std::vector<SampleFixed>& frame,
                               std::int64_t index) {
  const auto count = static_cast<std::int64_t>(frame.size());
  std::int64_t wrapped = index % count;
  if (wrapped < 0) {
    wrapped += count;
  }
  return SampleFixedToMillivolts(frame[static_cast<std::size_t>(wrapped)]);
}

// Position of the first crossing of `threshold_mv`, falling or rising, between
// samples `from` and `to` of a periodic frame, linearly interpolated between
// the two samples either side of it. Empty when there is none.
// Complexity O(to - from). Thread-safety: pure function.
inline std::optional<double> FindLevelCrossing(
    const std::vector<SampleFixed>& frame, std::int64_t from, std::int64_t to,
    double threshold_mv, bool falling) {
  for (std::int64_t i = from; i < to; ++i) {
    const double before = PeriodicSampleMv(frame, i);
    const double after = PeriodicSampleMv(frame, i + 1);
    const bool crosses = falling
                             ? (before >= threshold_mv && after < threshold_mv)
                             : (before < threshold_mv && after >= threshold_mv);
    if (crosses) {
      return static_cast<double>(i) +
             ((before - threshold_mv) / (before - after));
    }
  }
  return std::nullopt;
}

// Position of the half-amplitude falling crossing of the horizontal sync that
// starts each line, in samples from the start of the frame.
//
// `luma` holds one frame's Y plane in millivolts (SampleFixed), and
// `line_offsets` the first-sample offset of each storage line
// (BuildLineSampleOffsets). The threshold is (blanking + sync tip) / 2, as
// ITU-R BT.1700 and SMPTE 170M define 0H. The frame is treated as periodic,
// so the edge of line 1 may be found before sample 0 and is then returned as
// a negative position.
//
// Returns one entry per line; an entry is empty when no falling crossing lies
// in the search window. Complexity O(lines x window).
// Thread-safety: pure function.
inline std::vector<std::optional<double>> MeasureLineSyncEdges(
    const std::vector<SampleFixed>& luma, const std::vector<int>& line_offsets,
    double blanking_mv, double sync_tip_mv) {
  const double threshold = 0.5 * (blanking_mv + sync_tip_mv);
  std::vector<std::optional<double>> edges;
  edges.reserve(line_offsets.size());
  for (int offset : line_offsets) {
    edges.push_back(FindLevelCrossing(luma, offset - kSyncEdgeSearchBefore,
                                      offset + kSyncEdgeSearchAfter, threshold,
                                      /*falling=*/true));
  }
  return edges;
}

}  // namespace videosynth::test_support
