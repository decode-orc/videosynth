/*
 * File:        test_bt601_horizontal_mapping.cpp
 * Module:      generation_stage_tests
 * Purpose:     Validates that a section with horizontal_mapping: bt601 paints
 *              each active line from the ITU-R BT.601 sampling grid, leaves
 *              the samples beyond the 720-pixel span at blanking, and keeps
 *              its frames apart from an active-window section of the same
 *              source in the frame template cache.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "videosynth/active_sample_mapping.h"
#include "videosynth/fixed_point.h"
#include "videosynth/frame_line_layout.h"
#include "videosynth/generation_stage.h"
#include "videosynth/progressive_frame_source.h"
#include "videosynth/source_pixel_grid.h"
#include "videosynth/timing_constants.h"

namespace videosynth {
namespace {

std::string BarsExrPath(Standard standard) {
  return (std::filesystem::path(VIDEOSYNTH_SOURCE_DIR) /
          (Is625LineStandard(standard)
               ? "videosynth-assets/assets/exr/720x576/100_BARS.exr"
               : "videosynth-assets/assets/exr/720x486/100_BARS.exr"))
      .string();
}

Section BarsSection(Standard standard, HorizontalMapping mapping,
                    int duration_frames) {
  Section section{.name = "Bars",
                  .type = "progressive",
                  .line_injections = {},
                  .source = BarsExrPath(standard),
                  .duration_frames = duration_frames};
  section.horizontal_mapping = mapping;
  return section;
}

Project MakeProject(Standard standard) {
  Project project;
  project.cvbs_presets.video_standard_preset = standard;
  project.cvbs_presets.sample_encoding_preset = "CVBS_U10_4FSC";
  project.cvbs_presets.signal_state_preset = "STANDARD_STABLE_LOCKED";
  return project;
}

double LumaMillivoltsFromCodeForTest(int y_code, const SignalLevels& levels) {
  const int clamped = std::max(48, std::min(940, y_code));
  const double y_norm = static_cast<double>(clamped - 64) / 876.0;
  return levels.black_mv + (y_norm * (levels.white_mv - levels.black_mv));
}

// Checks every active-window sample of one field-1 line of frame
// frame_index against the nearest-pixel BT.601 mapping of its source row.
void ExpectLineFollowsBt601Grid(const Project& project,
                                const std::vector<SampleFixed>& y,
                                int frame_index, int line_1based) {
  const Standard standard = project.cvbs_presets.video_standard_preset;
  const TimingConstants timing = GetTimingConstants(standard);
  const ActiveRasterGeometry geometry =
      GetActiveRasterGeometry(standard, timing.sample_rate_4fsc_hz);
  const int window_start = geometry.active_window_start_samples;
  const int window_samples = geometry.active_window_end_samples - window_start;
  const SourcePixelGrid grid =
      Bt601SourcePixelGrid(standard, timing.sample_rate_4fsc_hz, window_start);
  const int in_range = SourcePixelGridInRangeSamples(grid, window_samples, 720);

  ProgressiveFrameSource frame_source;
  std::shared_ptr<const FrameSourceImage> source;
  std::string error;
  ASSERT_TRUE(frame_source.GenerateFrame(
      BarsSection(standard, HorizontalMapping::kBt601, 1), 0, standard, &source,
      &error))
      << error;

  // Field 1 is the upper field on 625-line systems (even rows) and the lower
  // field on 525-line systems (odd rows).
  const int field_line = line_1based - geometry.first_active_line_field1;
  const int source_row = source->active_y + (2 * field_line) +
                         (Is625LineStandard(standard) ? 0 : 1);

  const std::vector<int> counts = BuildLineSampleCounts(
      standard, timing.lines_per_frame, timing.samples_per_line_4fsc);
  const std::vector<int> offsets = BuildLineSampleOffsets(counts);
  const std::size_t frame_samples = static_cast<std::size_t>(offsets.back()) +
                                    static_cast<std::size_t>(counts.back());
  const std::size_t line_base =
      (static_cast<std::size_t>(frame_index) * frame_samples) +
      static_cast<std::size_t>(
          offsets[static_cast<std::size_t>(line_1based - 1)]) +
      static_cast<std::size_t>(window_start);

  const SignalLevels levels = GetSignalLevels(project.cvbs_presets);
  for (int x = 0; x < window_samples; ++x) {
    const double actual =
        SampleFixedToMillivolts(y[line_base + static_cast<std::size_t>(x)]);
    if (x >= in_range) {
      EXPECT_NEAR(actual, levels.blanking_mv, 1e-6) << "sample " << x;
      continue;
    }
    const int column =
        static_cast<int>(std::floor(SourcePixelCoordinate(grid, x)));
    const double expected = LumaMillivoltsFromCodeForTest(
        source->PixelAt(source->active_x + column, source_row).y, levels);
    ASSERT_NEAR(actual, expected, 1.0) << "sample " << x;
  }
}

TEST(Bt601HorizontalMappingTest, PalLinePaintsFromTheBt601Grid) {
  Project project = MakeProject(Standard::kPal);
  project.sections.push_back(
      BarsSection(Standard::kPal, HorizontalMapping::kBt601, 1));
  GenerationStage generation;
  std::vector<SampleFixed> y;
  std::vector<SampleFixed> c;
  std::vector<std::string> errors;
  ASSERT_TRUE(generation.Generate(project, &y, &c, &errors));
  ExpectLineFollowsBt601Grid(project, y, 0, 100);
}

TEST(Bt601HorizontalMappingTest, NtscLineLeavesTheTailBeyondTheSpanBlank) {
  Project project = MakeProject(Standard::kNtsc);
  project.sections.push_back(
      BarsSection(Standard::kNtsc, HorizontalMapping::kBt601, 1));
  GenerationStage generation;
  std::vector<SampleFixed> y;
  std::vector<SampleFixed> c;
  std::vector<std::string> errors;
  ASSERT_TRUE(generation.Generate(project, &y, &c, &errors));
  ExpectLineFollowsBt601Grid(project, y, 0, 100);
}

TEST(Bt601HorizontalMappingTest, TemplateCacheKeepsMappingsApart) {
  // The active-window section's frames 4-7 are admitted to the template
  // cache (second request of each colour-sequence key); the BT.601 section
  // then requests the same source and sequence phases and must not reuse
  // them.
  Project project = MakeProject(Standard::kPal);
  project.sections.push_back(
      BarsSection(Standard::kPal, HorizontalMapping::kActiveWindow, 8));
  project.sections.push_back(
      BarsSection(Standard::kPal, HorizontalMapping::kBt601, 4));
  GenerationStage generation;
  std::vector<SampleFixed> y;
  std::vector<SampleFixed> c;
  std::vector<std::string> errors;
  ASSERT_TRUE(generation.Generate(project, &y, &c, &errors));
  ExpectLineFollowsBt601Grid(project, y, 8, 100);
}

}  // namespace
}  // namespace videosynth
