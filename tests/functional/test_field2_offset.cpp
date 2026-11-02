/*
 * File:        test_field2_offset.cpp
 * Module:      channel_tests
 * Purpose:     End-to-end check of laserdisc.channel.field2_offset: field 2
 *              is misregistered in the picture domain while field 1 is left
 *              bit-identical.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

#include "videosynth/fixed_point.h"
#include "videosynth/frame_line_layout.h"
#include "videosynth/generation_stage.h"
#include "videosynth/signal_timing_model.h"
#include "videosynth/timing_constants.h"

namespace videosynth {
namespace {

Project MakeTestCardProject() {
  Project project;
  project.cvbs_presets.video_standard_preset = Standard::kPal;
  project.cvbs_presets.sample_encoding_preset = "CVBS_U10_4FSC";
  project.cvbs_presets.signal_state_preset = "STANDARD_STABLE_LOCKED";
  project.sections.push_back(
      Section{.name = "Card",
              .type = "progressive",
              .line_injections = {},
              .source = (std::filesystem::path(VIDEOSYNTH_SOURCE_DIR) /
                         "videosynth-assets/assets/exr/720x576/TESTCARD-F.exr")
                            .string(),
              .duration_frames = 1});
  return project;
}

void Generate(const Project& project, std::vector<SampleFixed>* y,
              std::vector<SampleFixed>* c) {
  std::vector<std::string> errors;
  ASSERT_TRUE(GenerationStage().Generate(project, y, c, &errors))
      << (errors.empty() ? "" : errors[0]);
}

// Luma of the active window of frame line `line_1based`.
std::vector<SampleFixed> ActiveLuma(const std::vector<SampleFixed>& y,
                                    int line_1based) {
  const TimingConstants timing = GetTimingConstants(Standard::kPal);
  const std::vector<int> offsets = BuildLineSampleOffsets(BuildLineSampleCounts(
      Standard::kPal, timing.lines_per_frame, timing.samples_per_line_4fsc));
  const ActiveRasterGeometry geometry =
      GetActiveRasterGeometry(Standard::kPal, timing.sample_rate_4fsc_hz);
  const int start = offsets[static_cast<std::size_t>(line_1based - 1)] +
                    geometry.active_window_start_samples;
  const int end = offsets[static_cast<std::size_t>(line_1based - 1)] +
                  geometry.active_window_end_samples;
  return {y.begin() + start, y.begin() + end};
}

TEST(Field2OffsetFunctionalTest, WholeFieldLineOffsetMovesOnlyField2) {
  Project clean_project = MakeTestCardProject();
  std::vector<SampleFixed> clean_y;
  std::vector<SampleFixed> clean_c;
  Generate(clean_project, &clean_y, &clean_c);

  // Two frame lines is one field line: each field-2 line shows the picture
  // the field-2 line above it showed.
  Project project = clean_project;
  project.laserdisc.enabled = true;
  project.laserdisc.channel.enabled = true;
  project.laserdisc.channel.field2_offset = {
      .enabled = true, .x_samples = 0.0, .y_lines = 2.0};
  std::vector<SampleFixed> y;
  std::vector<SampleFixed> c;
  Generate(project, &y, &c);

  // PAL field 1 picture lines 23-310, field 2 picture lines 336-623.
  for (int line = 40; line < 300; ++line) {
    ASSERT_EQ(ActiveLuma(y, line), ActiveLuma(clean_y, line)) << line;
  }
  for (int line = 350; line < 610; ++line) {
    ASSERT_EQ(ActiveLuma(y, line), ActiveLuma(clean_y, line - 1)) << line;
  }
  EXPECT_NE(y, clean_y);
}

TEST(Field2OffsetFunctionalTest, ZeroOffsetIsBitIdentical) {
  Project clean_project = MakeTestCardProject();
  std::vector<SampleFixed> clean_y;
  std::vector<SampleFixed> clean_c;
  Generate(clean_project, &clean_y, &clean_c);

  Project project = clean_project;
  project.laserdisc.enabled = true;
  project.laserdisc.channel.enabled = true;
  project.laserdisc.channel.field2_offset = {
      .enabled = true, .x_samples = 0.0, .y_lines = 0.0};
  std::vector<SampleFixed> y;
  std::vector<SampleFixed> c;
  Generate(project, &y, &c);
  EXPECT_EQ(y, clean_y);
  EXPECT_EQ(c, clean_c);
}

}  // namespace
}  // namespace videosynth
