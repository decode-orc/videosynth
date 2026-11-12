/*
 * File:        test_horizontal_sync_timing.cpp
 * Module:      generation_stage_tests
 * Purpose:     Measures where the half-amplitude point (0H) of every line's
 *              horizontal sync falls in synthesised frames of each standard,
 *              relative to the storage line and to the 4fsc lattice
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "sync_edge_measurement.h"
#include "videosynth/fixed_point.h"
#include "videosynth/frame_line_layout.h"
#include "videosynth/generation_stage.h"
#include "videosynth/model.h"
#include "videosynth/timing_constants.h"

namespace videosynth {
namespace {

// Where the generator currently places 0H after a storage line's first
// sample: its sync ramp starts at the line's first sample, so the
// half-amplitude point sits half a rise time later (EBU/ITU 625-line 250 ns
// and System M 140 ns edges at 4fsc).
constexpr double kCurrent625LineOriginSamples = 3.0;
constexpr double kCurrent525LineOriginSamples = 1.5;
constexpr double kOriginTolerance = 0.01;

Project MakeBarsProject(Standard standard) {
  Project project;
  project.cvbs_presets.video_standard_preset = standard;
  project.cvbs_presets.sample_encoding_preset =
      standard == Standard::kSecam ? "CVBS_TPG21_4FSC" : "CVBS_U10_4FSC";
  project.cvbs_presets.signal_state_preset = standard == Standard::kSecam
                                                 ? "STANDARD_STABLE_UNLOCKED"
                                                 : "STANDARD_STABLE_LOCKED";
  const char* raster = Is625LineStandard(standard) ? "720x576" : "720x486";
  project.sections.push_back(Section{
      .name = "Bars",
      .type = "progressive",
      .line_injections = {},
      .source = (std::filesystem::path(VIDEOSYNTH_SOURCE_DIR) /
                 "videosynth-assets/assets/exr" / raster / "100_BARS.exr")
                    .string(),
      .duration_frames = 1});
  return project;
}

struct MeasuredFrame {
  std::vector<int> line_offsets;
  // 0H of each line, in samples after its storage line's first sample.
  std::vector<double> origin_after_line_start;
  // 0H of each line, in samples from the start of the frame.
  std::vector<double> absolute;
};

MeasuredFrame MeasureFrame(Standard standard) {
  std::vector<SampleFixed> y;
  std::vector<SampleFixed> c;
  std::vector<std::string> errors;
  MeasuredFrame frame;
  const bool generated =
      GenerationStage().Generate(MakeBarsProject(standard), &y, &c, &errors);
  EXPECT_TRUE(generated) << (errors.empty() ? "" : errors[0]);
  if (!generated) {
    return frame;
  }

  const TimingConstants timing = GetTimingConstants(standard);
  const SignalLevels levels = GetSignalLevels(standard);
  frame.line_offsets = BuildLineSampleOffsets(BuildLineSampleCounts(
      standard, timing.lines_per_frame, timing.samples_per_line_4fsc));
  // One frame only: the measurement treats the frame as periodic.
  const std::size_t frame_samples =
      static_cast<std::size_t>(frame.line_offsets.back()) +
      static_cast<std::size_t>(
          BuildLineSampleCounts(standard, timing.lines_per_frame,
                                timing.samples_per_line_4fsc)
              .back());
  EXPECT_EQ(y.size(), frame_samples);

  const std::vector<std::optional<double>> edges =
      test_support::MeasureLineSyncEdges(
          y, frame.line_offsets, levels.blanking_mv, levels.sync_tip_mv);
  for (std::size_t i = 0; i < edges.size(); ++i) {
    EXPECT_TRUE(edges[i].has_value()) << "line " << (i + 1);
    const double edge = edges[i].value_or(0.0);
    frame.absolute.push_back(edge);
    frame.origin_after_line_start.push_back(edge - frame.line_offsets[i]);
  }
  return frame;
}

// Every line's 0H sits the same distance after its storage line's first
// sample: the orthogonal 525-line standards are already on their lattice.
TEST(HorizontalSyncTimingTest, OrthogonalStandardsPlace0HAtAFixedLineOffset) {
  for (Standard standard : {Standard::kNtsc, Standard::kPalM}) {
    const MeasuredFrame frame = MeasureFrame(standard);
    ASSERT_FALSE(frame.origin_after_line_start.empty());
    const auto [lowest, highest] =
        std::minmax_element(frame.origin_after_line_start.begin(),
                            frame.origin_after_line_start.end());
    EXPECT_NEAR(*lowest, kCurrent525LineOriginSamples, kOriginTolerance);
    EXPECT_NEAR(*highest, kCurrent525LineOriginSamples, kOriginTolerance);
    EXPECT_LT(*highest - *lowest, kOriginTolerance);
  }
}

// On the 625-line standards every line's sync also starts at its storage
// line's first sample, so 0H runs behind the 1135.0064-sample lattice by
// exactly the lattice delay d_k: a sawtooth reaching 1.997 samples on line
// 313 and resetting on lines 314 and 1.
TEST(HorizontalSyncTimingTest, Pal625LineSyncLagsTheLatticeByTheLatticeDelay) {
  for (Standard standard : {Standard::kPal, Standard::kSecam}) {
    const MeasuredFrame frame = MeasureFrame(standard);
    ASSERT_EQ(frame.absolute.size(), 625U);
    EXPECT_NEAR(frame.origin_after_line_start[0], kCurrent625LineOriginSamples,
                kOriginTolerance);

    for (std::size_t k = 0; k < frame.absolute.size(); ++k) {
      const int line = static_cast<int>(k) + 1;
      EXPECT_NEAR(frame.origin_after_line_start[k],
                  kCurrent625LineOriginSamples, kOriginTolerance)
          << "line " << line;
      const double lattice =
          frame.absolute[0] + (static_cast<double>(k) *
                               static_cast<double>(kPalLatticeSamplesPerFrame) /
                               kLatticeDelayDenominator);
      EXPECT_NEAR(lattice - frame.absolute[k],
                  LatticeLineDelay(standard, line).Samples(), kOriginTolerance)
          << "line " << line;
    }
    EXPECT_NEAR(
        frame.absolute[0] + (312.0 * 709379.0 / 625.0) - frame.absolute[312],
        1.997, kOriginTolerance);
  }
}

}  // namespace
}  // namespace videosynth
