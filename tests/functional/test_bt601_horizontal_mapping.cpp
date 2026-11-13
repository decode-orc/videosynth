/*
 * File:        test_bt601_horizontal_mapping.cpp
 * Module:      generation_stage_tests
 * Purpose:     Validates that a section with horizontal_mapping: bt601 paints
 *              each active line from the ITU-R BT.601 sampling grid, leaves
 *              the samples beyond the 720-pixel span at blanking, and keeps
 *              its frames apart from an active-window section of the same
 *              source in the frame template cache; with active_extent:
 *              source it paints the whole BT.601 span past the window.
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

#include "lattice_delay_reference.h"
#include "video_band_limit_reference.h"
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

// Checks every painted sample of one field-1 line of frame frame_index, and
// the samples just outside the painted span, against the nearest-pixel
// BT.601 mapping of its source row with blanking either side, band-limited
// and delayed onto the 4fsc lattice as the generator stores it.
void ExpectLineFollowsBt601Grid(const Project& project,
                                const std::vector<SampleFixed>& y,
                                int frame_index, int line_1based,
                                ActiveExtent extent = ActiveExtent::kWindow) {
  const Standard standard = project.cvbs_presets.video_standard_preset;
  const TimingConstants timing = GetTimingConstants(standard);
  const ActiveRasterGeometry geometry =
      GetActiveRasterGeometry(standard, timing.sample_rate_4fsc_hz);
  const ActiveSampleSpan span = ResolveActiveSampleSpan(
      HorizontalMapping::kBt601, extent, standard, timing.sample_rate_4fsc_hz,
      geometry.active_window_start_samples, geometry.active_window_end_samples,
      timing.samples_per_line_4fsc);
  const int window_start = span.start;
  const int window_samples = span.samples;
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
  // The line's 0H-relative render: the nearest BT.601 pixel inside the
  // painted span, blanking either side of it and past the source raster.
  auto render = [&](int j) {
    const int x = j - window_start;
    if (x < 0 || x >= window_samples || x >= in_range) {
      return levels.blanking_mv;
    }
    const int column =
        static_cast<int>(std::floor(SourcePixelCoordinate(grid, x)));
    return LumaMillivoltsFromCodeForTest(
        source->PixelAt(source->active_x + column, source_row).y, levels);
  };
  // The generator band-limits that render to the standard's video bandwidth,
  // and on PAL then delays it onto the 4fsc lattice; the samples either side
  // of the span carry only the shaped picture edge.
  const std::vector<double> expected = test_support::LatticeDelayedLine(
      standard, line_1based, window_start - 2, window_samples + 4,
      test_support::BandLimitedRender(standard, render));
  for (int x = -2; x < window_samples + 2; ++x) {
    const double actual = SampleFixedToMillivolts(y[static_cast<std::size_t>(
        static_cast<std::ptrdiff_t>(line_base) + x)]);
    ASSERT_NEAR(actual, expected[static_cast<std::size_t>(x + 2)], 1.0)
        << "sample " << x;
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

TEST(Bt601HorizontalMappingTest, PalSourceExtentPaintsPastTheWindow) {
  Project project = MakeProject(Standard::kPal);
  Section section = BarsSection(Standard::kPal, HorizontalMapping::kBt601, 1);
  section.active_extent = ActiveExtent::kSource;
  project.sections.push_back(section);
  GenerationStage generation;
  std::vector<SampleFixed> y;
  std::vector<SampleFixed> c;
  std::vector<std::string> errors;
  ASSERT_TRUE(generation.Generate(project, &y, &c, &errors));
  ExpectLineFollowsBt601Grid(project, y, 0, 100, ActiveExtent::kSource);
}

TEST(Bt601HorizontalMappingTest, NtscSourceExtentPaintsBeforeTheWindow) {
  Project project = MakeProject(Standard::kNtsc);
  Section section = BarsSection(Standard::kNtsc, HorizontalMapping::kBt601, 1);
  section.active_extent = ActiveExtent::kSource;
  project.sections.push_back(section);
  GenerationStage generation;
  std::vector<SampleFixed> y;
  std::vector<SampleFixed> c;
  std::vector<std::string> errors;
  ASSERT_TRUE(generation.Generate(project, &y, &c, &errors));
  ExpectLineFollowsBt601Grid(project, y, 0, 100, ActiveExtent::kSource);
}

TEST(Bt601HorizontalMappingTest, TemplateCacheKeepsExtentsApart) {
  // As below, with the window-extent frames cached before the source-extent
  // section requests the same keys.
  Project project = MakeProject(Standard::kPal);
  project.sections.push_back(
      BarsSection(Standard::kPal, HorizontalMapping::kBt601, 8));
  Section wide = BarsSection(Standard::kPal, HorizontalMapping::kBt601, 4);
  wide.active_extent = ActiveExtent::kSource;
  project.sections.push_back(wide);
  GenerationStage generation;
  std::vector<SampleFixed> y;
  std::vector<SampleFixed> c;
  std::vector<std::string> errors;
  ASSERT_TRUE(generation.Generate(project, &y, &c, &errors));
  ExpectLineFollowsBt601Grid(project, y, 8, 100, ActiveExtent::kSource);
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
