/*
 * File:        test_frame_line_layout.cpp
 * Module:      frame_line_layout_tests
 * Purpose:     Validates the storage-line layout of a 4fsc frame and the
 *              lattice delay of each line's 0H against the PAL 4fsc lattice
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include "videosynth/frame_line_layout.h"
#include "videosynth/model.h"
#include "videosynth/timing_constants.h"

namespace videosynth {
namespace {

std::vector<int> LineOffsets(Standard standard) {
  const TimingConstants timing = GetTimingConstants(standard);
  return BuildLineSampleOffsets(BuildLineSampleCounts(
      standard, timing.lines_per_frame, timing.samples_per_line_4fsc));
}

TEST(FrameLineLayoutTest, PalFrameHolds709379Samples) {
  const TimingConstants timing = GetTimingConstants(Standard::kPal);
  const std::vector<int> counts = BuildLineSampleCounts(
      Standard::kPal, timing.lines_per_frame, timing.samples_per_line_4fsc);
  std::int64_t total = 0;
  for (int count : counts) {
    total += count;
  }
  EXPECT_EQ(total, kPalLatticeSamplesPerFrame);
  EXPECT_EQ(counts[312], 1137);
  EXPECT_EQ(counts[624], 1137);
}

TEST(FrameLineLayoutTest, LatticeLineDelayMatchesTheSawtoothEndpoints) {
  EXPECT_EQ(LatticeLineDelay(Standard::kPal, 1).numerator, 0);
  EXPECT_EQ(LatticeLineDelay(Standard::kPal, 313).numerator, 1248);
  EXPECT_EQ(LatticeLineDelay(Standard::kPal, 314).numerator, 2);
  EXPECT_EQ(LatticeLineDelay(Standard::kPal, 625).numerator, 1246);
  EXPECT_DOUBLE_EQ(LatticeLineDelay(Standard::kPal, 313).Samples(),
                   1248.0 / 625.0);
}

TEST(FrameLineLayoutTest, LatticeLineDelayPlacesEveryPalLineOnTheLattice) {
  for (Standard standard : {Standard::kPal, Standard::kSecam}) {
    const std::vector<int> offsets = LineOffsets(standard);
    ASSERT_EQ(offsets.size(), 625U);
    for (std::size_t k = 0; k < offsets.size(); ++k) {
      const LatticeDelay delay =
          LatticeLineDelay(standard, static_cast<int>(k) + 1);
      EXPECT_EQ(
          (static_cast<std::int64_t>(offsets[k]) * kLatticeDelayDenominator) +
              delay.numerator,
          static_cast<std::int64_t>(k) * kPalLatticeSamplesPerFrame)
          << "line " << (k + 1);
      EXPECT_GE(delay.Samples(), 0.0) << "line " << (k + 1);
      EXPECT_LT(delay.Samples(), 2.0) << "line " << (k + 1);
    }
  }
}

TEST(FrameLineLayoutTest, LatticeLineDelayIsZeroOnOrthogonalStandards) {
  for (Standard standard : {Standard::kNtsc, Standard::kPalM}) {
    const int lines = GetTimingConstants(standard).lines_per_frame;
    for (int line = 1; line <= lines; ++line) {
      EXPECT_EQ(LatticeLineDelay(standard, line).numerator, 0)
          << "line " << line;
    }
  }
}

}  // namespace
}  // namespace videosynth
