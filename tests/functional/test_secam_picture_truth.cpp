/*
 * File:        test_secam_picture_truth.cpp
 * Module:      picture_truth_tests
 * Purpose:     Checks the SECAM picture truth against the generated signal:
 *              its Y equals the PAL rendering's, and re-encoding its D'R/D'B
 *              reproduces the transmitted chroma.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

#include "fixture_paths.h"
#include "lattice_delay_reference.h"
#include "videosynth/fixed_point.h"
#include "videosynth/frame_line_layout.h"
#include "videosynth/generation_stage.h"
#include "videosynth/path_resolution.h"
#include "videosynth/picture_truth.h"
#include "videosynth/project_validator.h"
#include "videosynth/secam_chroma_encoder.h"
#include "videosynth/timing_constants.h"
#include "videosynth/yaml_project_parser.h"

namespace videosynth {
namespace {

constexpr char kFixture[] = "general/secam_picture_truth.yaml";
// The fixture's first section: two frames of 75% colour bars, whose white
// first bar leaves no colour difference spilling out of the active window.
constexpr std::size_t kBarsFrames = 2;

Project LoadFixture() {
  YamlProjectParser parser;
  const ParseResult parsed =
      parser.ParseFile(test_support::FixturePath(kFixture));
  EXPECT_TRUE(parsed.ok) << (parsed.errors.empty() ? "" : parsed.errors[0]);
  AssetRootMap roots = DefaultAssetRoots();
  roots.roots["output"] =
      (std::filesystem::temp_directory_path() / "videosynth_secam_truth")
          .string();
  return ResolveProjectPaths(parsed.project, roots,
                             test_support::FixtureProjectDir(kFixture),
                             /*anchor_unset=*/true);
}

struct Render {
  std::vector<SampleFixed> y;
  std::vector<SampleFixed> c;
  std::vector<PictureTruthFrame> truth;
};

Render RenderWithTruth(const Project& project) {
  const ValidationResult validation = ProjectValidator().Validate(project);
  EXPECT_TRUE(validation.is_valid)
      << (validation.errors.empty() ? "" : validation.errors[0]);
  GenerationStage stage;
  std::vector<IGenerationStage::FrameScheduleItem> schedule;
  std::vector<std::string> errors;
  EXPECT_TRUE(stage.BuildFrameSchedule(project, &schedule, &errors))
      << (errors.empty() ? "" : errors[0]);
  Render render;
  EXPECT_TRUE(stage.GenerateFrameBatchWithTruth(
      project, schedule, 0, schedule.size(), &render.y, &render.c,
      &render.truth, &errors))
      << (errors.empty() ? "" : errors[0]);
  return render;
}

TEST(SecamPictureTruthTest, RecordsBothColourDifferencesAndTheLineRule) {
  const Project project = LoadFixture();
  const Render render = RenderWithTruth(project);
  const PictureTruthLayout layout =
      BuildPictureTruthLayout(project.cvbs_presets);
  ASSERT_EQ(render.truth.size(), 6U);
  for (std::size_t frame = 0; frame < render.truth.size(); ++frame) {
    const PictureTruthFrame& truth = render.truth[frame];
    ASSERT_EQ(truth.dr.size(), layout.PlaneSamples());
    ASSERT_EQ(truth.db.size(), layout.PlaneSamples());
    EXPECT_TRUE(truth.u_mv.empty());
    // BT.1700 Part C Figure 16: line 7 of the first frame is a D'R line, and
    // every line swaps component each frame.
    EXPECT_EQ(truth.dr_line_parity, (frame % 2U) == 0U ? 1 : 0) << frame;
  }
  // The green bar (fifth of eight) has R'-Y' and B'-Y' both negative at 75%:
  // D'R = -1.902 (R'-Y') > 0 and D'B = 1.505 (B'-Y') < 0, on every row,
  // whichever component the row's line transmits.
  const int green = (4 * layout.samples_per_line / 8) - 40;
  for (const int row : {100, 101, 300, 301}) {
    const std::size_t index =
        (static_cast<std::size_t>(row) * layout.samples_per_line) + green;
    EXPECT_GT(render.truth[0].dr[index], 0.5F) << row;
    EXPECT_LT(render.truth[0].db[index], -0.5F) << row;
  }
}

TEST(SecamPictureTruthTest, LumaEqualsThePalRendering) {
  const Project secam = LoadFixture();
  Project pal = secam;
  pal.cvbs_presets.video_standard_preset = Standard::kPal;
  pal.cvbs_presets.signal_state_preset =
      GeneratedSignalStatePreset(Standard::kPal);
  const Render secam_render = RenderWithTruth(secam);
  const Render pal_render = RenderWithTruth(pal);
  ASSERT_EQ(secam_render.truth.size(), pal_render.truth.size());
  for (std::size_t frame = 0; frame < secam_render.truth.size(); ++frame) {
    EXPECT_EQ(secam_render.truth[frame].y_mv, pal_render.truth[frame].y_mv)
        << frame;
  }
}

TEST(SecamPictureTruthTest, ReEncodingTheTruthReproducesTheChroma) {
  const Project project = LoadFixture();
  const Render render = RenderWithTruth(project);
  const PictureTruthLayout layout =
      BuildPictureTruthLayout(project.cvbs_presets);
  ASSERT_GE(render.truth.size(), kBarsFrames);
  const std::size_t frame_samples = render.c.size() / render.truth.size();
  const TimingConstants timing = GetTimingConstants(Standard::kSecam);
  const SecamChromaEncoder encoder(timing.sample_rate_4fsc_hz);
  const std::vector<int> offsets = BuildLineSampleOffsets(BuildLineSampleCounts(
      Standard::kSecam, timing.lines_per_frame, timing.samples_per_line_4fsc));
  constexpr int kLineSamples = 1135;

  // The transmitted line's D'R or D'B, zero outside the active window, put
  // back through the precorrection, FM, bell filter and gate (ITU-R BT.1700
  // Annex 1 Part C Table 4 items 10f, 5, 10e, 10d).
  std::vector<double> colour_difference;
  std::vector<SampleFixed> encoded;
  double worst_mv = 0.0;
  int lines_checked = 0;
  for (std::size_t frame = 0; frame < kBarsFrames; ++frame) {
    const PictureTruthFrame& truth = render.truth[frame];
    const int frame_parity = static_cast<int>(frame % 2U);
    for (int line = 1; line <= 625; ++line) {
      const int row = layout.RowForLine(line);
      if (row < 0 || line == layout.half_picture_line) {
        continue;
      }
      const SecamLineComponent component =
          SecamComponentForLine(line, frame_parity);
      ASSERT_EQ(component == SecamLineComponent::kDr,
                (line % 2) == truth.dr_line_parity);
      const std::vector<float>& plane =
          component == SecamLineComponent::kDr ? truth.dr : truth.db;
      colour_difference.assign(kLineSamples, 0.0);
      const std::size_t row_base =
          static_cast<std::size_t>(row) * layout.samples_per_line;
      std::copy_n(plane.begin() + static_cast<std::ptrdiff_t>(row_base),
                  layout.samples_per_line,
                  colour_difference.begin() + layout.first_sample);
      encoder.EncodeColourDifferenceLine(
          component, SecamInitialPhaseRadForLine(line, frame_parity),
          colour_difference, kLineSamples, &encoded);
      // The truth and the encoder are 0H-relative; the stored line is that
      // render delayed onto the PAL 4fsc lattice.
      const std::vector<double> expected = test_support::LatticeDelayedLine(
          Standard::kSecam, line, layout.first_sample, layout.samples_per_line,
          [&](int j) {
            return (j >= 0 && j < kLineSamples)
                       ? SampleFixedToMillivolts(
                             encoded[static_cast<std::size_t>(j)])
                       : 0.0;
          });
      const std::size_t line_base =
          (frame * frame_samples) +
          static_cast<std::size_t>(
              offsets[static_cast<std::size_t>(line - 1)]) +
          static_cast<std::size_t>(layout.first_sample);
      for (int x = 0; x < layout.samples_per_line; ++x) {
        const std::size_t slot = static_cast<std::size_t>(x);
        worst_mv = std::max(
            worst_mv,
            std::abs(expected[slot] -
                     SampleFixedToMillivolts(render.c[line_base + slot])));
      }
      ++lines_checked;
    }
  }
  EXPECT_EQ(lines_checked, static_cast<int>(kBarsFrames) * (layout.rows - 1));
  // The truth is float32 and the stored signal fixed point: the re-encoded
  // subcarrier agrees to a fraction of a millivolt of its 80.5 mV peak.
  EXPECT_LT(worst_mv, 0.5);
}

}  // namespace
}  // namespace videosynth
