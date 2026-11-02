/*
 * File:        test_picture_truth.cpp
 * Module:      picture_truth_tests
 * Purpose:     Validates the picture truth layout and file naming, and the
 *              parsing, emission and validation of output.truth and
 *              section chroma_line_sign.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "videosynth/picture_truth.h"
#include "videosynth/picture_truth_writer.h"
#include "videosynth/project_validator.h"
#include "videosynth/yaml_project_emitter.h"
#include "videosynth/yaml_project_parser.h"

namespace videosynth {
namespace {

CvbsPresets PresetsFor(Standard standard) {
  CvbsPresets presets;
  presets.video_standard_preset = standard;
  return presets;
}

bool AnyContains(const std::vector<std::string>& messages,
                 const std::string& text) {
  for (const std::string& message : messages) {
    if (message.find(text) != std::string::npos) {
      return true;
    }
  }
  return false;
}

TEST(PictureTruthLayoutTest, PalLayoutCoversTheActiveWindow) {
  const PictureTruthLayout layout =
      BuildPictureTruthLayout(PresetsFor(Standard::kPal));
  EXPECT_EQ(layout.samples_per_line, 922);
  EXPECT_EQ(layout.first_sample, 177);
  EXPECT_EQ(layout.rows, 576);
  EXPECT_EQ(layout.upper_field, 1);
  EXPECT_EQ(layout.half_picture_line, 623);
  EXPECT_DOUBLE_EQ(layout.black_mv, 0.0);
  EXPECT_DOUBLE_EQ(layout.white_mv, 700.0);
  EXPECT_EQ(layout.PlaneSamples(), 922U * 576U);
  // Field 1 opens the picture on line 23 (row 0), field 2 on line 336.
  EXPECT_EQ(layout.RowForLine(23), 0);
  EXPECT_EQ(layout.RowForLine(336), 1);
  EXPECT_EQ(layout.RowForLine(24), 2);
  EXPECT_EQ(layout.RowForLine(310), 574);
  EXPECT_EQ(layout.RowForLine(623), 575);
  EXPECT_EQ(layout.RowForLine(22), -1);
  EXPECT_EQ(layout.RowForLine(311), -1);
  EXPECT_EQ(layout.RowForLine(624), -1);
}

TEST(PictureTruthLayoutTest, NtscEvenRowsComeFromField2) {
  const PictureTruthLayout layout =
      BuildPictureTruthLayout(PresetsFor(Standard::kNtsc));
  EXPECT_EQ(layout.rows, 480);
  EXPECT_EQ(layout.samples_per_line, 745);
  EXPECT_EQ(layout.first_sample, 150);
  EXPECT_EQ(layout.upper_field, 2);
  EXPECT_EQ(layout.half_picture_line, 0);
  const PictureTruthLayout pal_m =
      BuildPictureTruthLayout(PresetsFor(Standard::kPalM));
  EXPECT_EQ(pal_m.samples_per_line, 745);
  EXPECT_EQ(pal_m.first_sample, 150);
  EXPECT_EQ(pal_m.rows, 480);
  EXPECT_EQ(layout.RowForLine(284), 0);
  EXPECT_EQ(layout.RowForLine(22), 1);
  // NTSC black carries the 7.5 IRE setup above blanking.
  EXPECT_GT(layout.black_mv, 50.0);
}

TEST(PictureTruthLayoutTest, SecamHasNoLayout) {
  EXPECT_EQ(BuildPictureTruthLayout(PresetsFor(Standard::kSecam)).rows, 0);
}

TEST(PictureTruthLayoutTest, ResetSizesAndZeroesThePlanes) {
  const PictureTruthLayout layout =
      BuildPictureTruthLayout(PresetsFor(Standard::kPalM));
  PictureTruthFrame frame;
  frame.y_mv.assign(3, 5.0F);
  frame.Reset(layout);
  ASSERT_EQ(frame.y_mv.size(), layout.PlaneSamples());
  ASSERT_EQ(frame.u_mv.size(), layout.PlaneSamples());
  ASSERT_EQ(frame.v_mv.size(), layout.PlaneSamples());
  for (const float value : frame.y_mv) {
    ASSERT_EQ(value, 0.0F);
  }
}

TEST(PictureTruthPathTest, SitsBesideTheVideoOutput) {
  EXPECT_EQ(DerivePictureTruthPath("out/disc.cvbs"), "out/disc.truth");
  EXPECT_EQ(DerivePictureTruthSidecarPath("out/disc.cvbs"),
            "out/disc.truth.meta");
  EXPECT_EQ(DerivePictureTruthPath("out/disc.cvbsy"), "out/disc.truth");
  EXPECT_EQ(DerivePictureTruthPath("out/disc.raw"), "out/disc.raw.truth");
}

std::string ProjectYaml(const std::string& output_extra,
                        const std::string& section_extra,
                        const std::string& standard = "PAL") {
  return "cvbs_presets:\n"
         "  video_standard_preset: " +
         standard +
         "\n"
         "output:\n"
         "  video_path: out.cvbs\n" +
         output_extra +
         "sections:\n"
         "  - name: S\n"
         "    type: progressive\n"
         "    source: f.exr\n"
         "    duration_frames: 1\n" +
         section_extra;
}

TEST(PictureTruthParserTest, ReadsOutputTruth) {
  YamlProjectParser parser;
  EXPECT_FALSE(parser.ParseString(ProjectYaml("", "")).project.output.truth);
  const ParseResult enabled =
      parser.ParseString(ProjectYaml("  truth: true\n", ""));
  ASSERT_TRUE(enabled.ok) << enabled.errors[0];
  EXPECT_TRUE(enabled.project.output.truth);
  const ParseResult bad =
      parser.ParseString(ProjectYaml("  truth: sometimes\n", ""));
  ASSERT_FALSE(bad.ok);
  EXPECT_NE(bad.errors[0].find("output.truth"), std::string::npos);
}

constexpr const char* kTwoRegions =
    "    chroma_line_sign:\n"
    "      - region: {x: 48, y: 96, width: 96, height: 144}\n"
    "        v: follow_switch\n"
    "      - region: {x: 576, y: 96, width: 96, height: 144}\n"
    "        u: follow_switch\n"
    "        v: normal\n";

TEST(ChromaLineSignParserTest, ReadsRegionsAndModes) {
  YamlProjectParser parser;
  const ParseResult result = parser.ParseString(ProjectYaml("", kTwoRegions));
  ASSERT_TRUE(result.ok) << result.errors[0];
  const std::vector<ChromaLineSignRegion>& regions =
      result.project.sections[0].chroma_line_sign;
  ASSERT_EQ(regions.size(), 2U);
  EXPECT_EQ(regions[0],
            (ChromaLineSignRegion{.x = 48,
                                  .y = 96,
                                  .width = 96,
                                  .height = 144,
                                  .u = ChromaLineSignMode::kNormal,
                                  .v = ChromaLineSignMode::kFollowSwitch}));
  EXPECT_EQ(regions[1].x, 576);
  EXPECT_EQ(regions[1].u, ChromaLineSignMode::kFollowSwitch);
  EXPECT_EQ(regions[1].v, ChromaLineSignMode::kNormal);
}

TEST(ChromaLineSignParserTest, RejectsMalformedEntries) {
  YamlProjectParser parser;
  const struct {
    const char* yaml;
    const char* error;
  } cases[] = {
      {"    chroma_line_sign: {x: 1}\n", "must be a list"},
      {"    chroma_line_sign:\n      - v: follow_switch\n",
       "missing required field: 'region'"},
      {"    chroma_line_sign:\n"
       "      - region: {x: 1, y: 2, width: 3}\n"
       "        v: follow_switch\n",
       "missing required field: 'height'"},
      {"    chroma_line_sign:\n"
       "      - region: {x: a, y: 2, width: 3, height: 4}\n"
       "        v: follow_switch\n",
       "x must be an integer"},
      {"    chroma_line_sign:\n"
       "      - region: {x: 1, y: 2, width: 3, height: 4}\n"
       "        v: invert\n",
       "normal, follow_switch"},
      {"    chroma_line_sign:\n"
       "      - region: {x: 1, y: 2, width: 3, height: 4}\n"
       "        w: follow_switch\n",
       "w"},
  };
  for (const auto& test_case : cases) {
    const ParseResult result =
        parser.ParseString(ProjectYaml("", test_case.yaml));
    ASSERT_FALSE(result.ok) << test_case.yaml;
    ASSERT_FALSE(result.errors.empty());
    EXPECT_NE(result.errors[0].find(test_case.error), std::string::npos)
        << result.errors[0];
  }
}

TEST(PictureSettingsEmitterTest, RoundTripsTruthAndChromaLineSign) {
  YamlProjectParser parser;
  YamlProjectEmitter emitter;
  const ParseResult first =
      parser.ParseString(ProjectYaml("  truth: true\n", kTwoRegions));
  ASSERT_TRUE(first.ok);
  const std::string emitted = emitter.EmitString(first.project);
  const ParseResult second = parser.ParseString(emitted);
  ASSERT_TRUE(second.ok) << emitted;
  EXPECT_EQ(first.project, second.project) << emitted;
  EXPECT_EQ(emitter.EmitString(second.project), emitted);
  // A component left normal is not written.
  EXPECT_EQ(emitted.find("normal"), std::string::npos) << emitted;

  const ParseResult plain = parser.ParseString(ProjectYaml("", ""));
  const std::string plain_emitted = emitter.EmitString(plain.project);
  EXPECT_EQ(plain_emitted.find("truth"), std::string::npos);
  EXPECT_EQ(plain_emitted.find("chroma_line_sign"), std::string::npos);
}

Project ValidProject(Standard standard) {
  Project project;
  project.cvbs_presets.video_standard_preset = standard;
  project.cvbs_presets.sample_encoding_preset = "CVBS_U10_4FSC";
  project.cvbs_presets.signal_state_preset =
      GeneratedSignalStatePreset(standard);
  project.output.video_path = "out.cvbs";
  Section section;
  section.name = "Card";
  section.type = "progressive";
  section.source = "card.exr";
  section.duration_frames = 4;
  project.sections.push_back(section);
  return project;
}

ChromaLineSignRegion VRegion(int x, int y, int width, int height) {
  return ChromaLineSignRegion{.x = x,
                              .y = y,
                              .width = width,
                              .height = height,
                              .v = ChromaLineSignMode::kFollowSwitch};
}

TEST(ChromaLineSignValidationTest, AcceptsRegionsInsideThePalAndPalMRasters) {
  for (const auto& [standard, height] :
       {std::pair{Standard::kPal, 576}, std::pair{Standard::kPalM, 486}}) {
    Project project = ValidProject(standard);
    project.sections[0].chroma_line_sign = {VRegion(0, 0, 720, height)};
    const ValidationResult result = ProjectValidator().Validate(project);
    EXPECT_TRUE(result.is_valid)
        << (result.errors.empty() ? "" : result.errors[0]);
  }
}

TEST(ChromaLineSignValidationTest, RejectsStandardsWithoutAVSwitch) {
  for (const Standard standard : {Standard::kNtsc, Standard::kSecam}) {
    Project project = ValidProject(standard);
    project.sections[0].chroma_line_sign = {VRegion(0, 0, 10, 10)};
    const ValidationResult result = ProjectValidator().Validate(project);
    EXPECT_FALSE(result.is_valid);
    ASSERT_FALSE(result.errors.empty());
    EXPECT_TRUE(AnyContains(result.errors, "requires a PAL or PAL-M project"))
        << result.errors[0];
    EXPECT_TRUE(AnyContains(result.errors, "Picture validation error"));
  }
}

TEST(ChromaLineSignValidationTest, RejectsRegionsOutsideTheRaster) {
  const ChromaLineSignRegion bad_regions[] = {
      VRegion(-1, 0, 10, 10), VRegion(0, -1, 10, 10),  VRegion(0, 0, 0, 10),
      VRegion(0, 0, 10, 0),   VRegion(711, 0, 10, 10), VRegion(0, 567, 10, 10),
  };
  for (const ChromaLineSignRegion& region : bad_regions) {
    Project project = ValidProject(Standard::kPal);
    project.sections[0].chroma_line_sign = {region};
    const ValidationResult result = ProjectValidator().Validate(project);
    EXPECT_FALSE(result.is_valid) << region.x << "," << region.y;
    EXPECT_TRUE(AnyContains(result.errors, "chroma_line_sign[0].region"));
  }
  // PAL-M rows end at 486.
  Project pal_m = ValidProject(Standard::kPalM);
  pal_m.sections[0].chroma_line_sign = {VRegion(0, 480, 10, 10)};
  EXPECT_FALSE(ProjectValidator().Validate(pal_m).is_valid);
}

TEST(ChromaLineSignValidationTest, RejectsARegionThatChangesNothing) {
  Project project = ValidProject(Standard::kPal);
  ChromaLineSignRegion region = VRegion(0, 0, 10, 10);
  region.v = ChromaLineSignMode::kNormal;
  project.sections[0].chroma_line_sign = {region};
  const ValidationResult result = ProjectValidator().Validate(project);
  EXPECT_FALSE(result.is_valid);
  EXPECT_TRUE(AnyContains(result.errors, "changes nothing"));
}

TEST(PictureTruthValidationTest, AcceptsQuadratureStandards) {
  for (const Standard standard :
       {Standard::kPal, Standard::kNtsc, Standard::kPalM}) {
    Project project = ValidProject(standard);
    project.output.truth = true;
    const ValidationResult result = ProjectValidator().Validate(project);
    EXPECT_TRUE(result.is_valid)
        << (result.errors.empty() ? "" : result.errors[0]);
    EXPECT_FALSE(AnyContains(result.warnings, "output.truth"));
  }
}

TEST(PictureTruthValidationTest, RejectsSecam) {
  Project project = ValidProject(Standard::kSecam);
  project.output.truth = true;
  const ValidationResult result = ProjectValidator().Validate(project);
  EXPECT_FALSE(result.is_valid);
  EXPECT_TRUE(AnyContains(result.errors, "output.truth requires a PAL"));
}

TEST(PictureTruthValidationTest, WarnsThatOsdIsExcluded) {
  Project project = ValidProject(Standard::kPal);
  project.output.truth = true;
  OsdOverlay overlay;
  overlay.text = "hello";
  project.sections[0].osd.overlays.push_back(overlay);
  const ValidationResult result = ProjectValidator().Validate(project);
  EXPECT_TRUE(result.is_valid)
      << (result.errors.empty() ? "" : result.errors[0]);
  EXPECT_TRUE(AnyContains(result.warnings, "excludes OSD overlays"));
}

}  // namespace
}  // namespace videosynth
