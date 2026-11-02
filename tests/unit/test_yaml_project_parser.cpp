/*
 * File:        test_yaml_project_parser.cpp
 * Module:      yaml_project_parser_tests
 * Purpose:     Validates progressive section parsing and duration semantics.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

#include "videosynth/yaml_project_parser.h"

namespace videosynth {
namespace {

TEST(YamlProjectParserTest, ParsesProgressiveSectionWithAllDuration) {
  const std::string yaml =
      "project:\n"
      "  name: ProgressiveAll\n"
      "  version: \"1.0\"\n"
      "cvbs_presets:\n"
      "  video_standard_preset: PAL\n"
      "  sample_encoding_preset: CVBS_U10_4FSC\n"
      "  signal_state_preset: STANDARD_STABLE_LOCKED\n"
      "output:\n"
      "  video_path: out.cvbs\n"
      "sections:\n"
      "  - name: ProgressiveVideo\n"
      "    type: progressive\n"
      "    source: fixture.mkv\n"
      "    duration_frames: all\n";

  YamlProjectParser parser;
  const ParseResult result = parser.ParseString(yaml);

  ASSERT_TRUE(result.ok);
  ASSERT_EQ(result.project.sections.size(), 1U);
  EXPECT_TRUE(result.project.sections[0].duration_frames_all);
  EXPECT_EQ(result.project.sections[0].duration_frames, 0);
}

// A section-only YAML document around `section_body` (indented keys).
std::string MakeVideoSectionYaml(const std::string& section_body) {
  return "cvbs_presets:\n"
         "  video_standard_preset: PAL\n"
         "output:\n"
         "  video_path: out.cvbs\n"
         "sections:\n"
         "  - name: Excerpt\n"
         "    type: progressive\n"
         "    source: capture.mp4\n" +
         section_body;
}

TEST(YamlProjectParserTest, OmittedDurationFramesMeansAll) {
  YamlProjectParser parser;
  const ParseResult result =
      parser.ParseString(MakeVideoSectionYaml("    start_frame: 4\n"));

  ASSERT_TRUE(result.ok) << (result.errors.empty() ? "" : result.errors[0]);
  EXPECT_TRUE(result.project.sections[0].duration_frames_all);
  EXPECT_EQ(result.project.sections[0].duration_frames_repeat, 1);
}

TEST(YamlProjectParserTest, ParsesEndFrame) {
  YamlProjectParser parser;
  const ParseResult result = parser.ParseString(
      MakeVideoSectionYaml("    start_frame: 1500\n    end_frame: 2999\n"));

  ASSERT_TRUE(result.ok) << (result.errors.empty() ? "" : result.errors[0]);
  const Section& section = result.project.sections[0];
  EXPECT_EQ(section.start_frame, 1500);
  EXPECT_TRUE(section.end_frame_specified);
  EXPECT_EQ(section.end_frame, 2999);
  EXPECT_TRUE(section.duration_frames_all);
}

TEST(YamlProjectParserTest, EndFrameIsUnsetWhenAbsent) {
  YamlProjectParser parser;
  const ParseResult result =
      parser.ParseString(MakeVideoSectionYaml("    duration_frames: 10\n"));

  ASSERT_TRUE(result.ok);
  EXPECT_FALSE(result.project.sections[0].end_frame_specified);
}

TEST(YamlProjectParserTest, RejectsNonIntegerEndFrame) {
  YamlProjectParser parser;
  const ParseResult result =
      parser.ParseString(MakeVideoSectionYaml("    end_frame: last\n"));

  EXPECT_FALSE(result.ok);
  ASSERT_FALSE(result.errors.empty());
  EXPECT_NE(result.errors[0].find("end_frame"), std::string::npos)
      << result.errors[0];
}

TEST(YamlProjectParserTest, CadenceDefaultsToVideoWhenAbsent) {
  YamlProjectParser parser;
  const ParseResult result =
      parser.ParseString(MakeVideoSectionYaml("    duration_frames: 10\n"));

  ASSERT_TRUE(result.ok);
  const CadenceParameters& cadence = result.project.sections[0].cadence;
  EXPECT_EQ(cadence.pattern, CadencePattern::kVideo);
  EXPECT_EQ(cadence.phase, CadencePhase::kA);
  EXPECT_FALSE(cadence.phase_specified);
  EXPECT_EQ(cadence.continuity, CadenceContinuity::kRestart);
}

TEST(YamlProjectParserTest, ParsesPulldownCadence) {
  YamlProjectParser parser;
  const ParseResult result =
      parser.ParseString(MakeVideoSectionYaml("    cadence:\n"
                                              "      pattern: \"2:3\"\n"
                                              "      phase: C\n"
                                              "      continuity: continue\n"));

  ASSERT_TRUE(result.ok) << (result.errors.empty() ? "" : result.errors[0]);
  const CadenceParameters& cadence = result.project.sections[0].cadence;
  EXPECT_EQ(cadence.pattern, CadencePattern::kPulldown23);
  EXPECT_EQ(cadence.phase, CadencePhase::kC);
  EXPECT_TRUE(cadence.phase_specified);
  EXPECT_EQ(cadence.continuity, CadenceContinuity::kContinue);
}

TEST(YamlProjectParserTest, ParsesUnquotedPulldownPattern) {
  YamlProjectParser parser;
  const ParseResult result = parser.ParseString(
      MakeVideoSectionYaml("    cadence:\n      pattern: 2:3\n"));

  ASSERT_TRUE(result.ok) << (result.errors.empty() ? "" : result.errors[0]);
  EXPECT_EQ(result.project.sections[0].cadence.pattern,
            CadencePattern::kPulldown23);
}

// Each unknown cadence value is an error naming its key.
TEST(YamlProjectParserTest, RejectsUnknownCadenceValues) {
  const std::vector<std::pair<std::string, std::string>> cases = {
      {"      pattern: \"3:2\"\n", "cadence.pattern"},
      {"      phase: E\n", "cadence.phase"},
      {"      continuity: resume\n", "cadence.continuity"},
      {"      cycle: 10\n", "cycle"},
      {"      phase: [A]\n", "cadence.phase"},
  };
  YamlProjectParser parser;
  for (const auto& [body, key] : cases) {
    const ParseResult result =
        parser.ParseString(MakeVideoSectionYaml("    cadence:\n" + body));
    EXPECT_FALSE(result.ok) << body;
    ASSERT_FALSE(result.errors.empty()) << body;
    EXPECT_NE(result.errors[0].find(key), std::string::npos)
        << result.errors[0];
  }
}

TEST(YamlProjectParserTest, RejectsNonMapCadence) {
  YamlProjectParser parser;
  const ParseResult result =
      parser.ParseString(MakeVideoSectionYaml("    cadence: \"2:3\"\n"));

  EXPECT_FALSE(result.ok);
  ASSERT_FALSE(result.errors.empty());
  EXPECT_NE(result.errors[0].find("section.cadence"), std::string::npos)
      << result.errors[0];
}

TEST(YamlProjectParserTest, ParsesDurationRepeatWithAllDuration) {
  const std::string yaml =
      "project:\n"
      "  name: ProgressiveAllRepeat\n"
      "  version: \"1.0\"\n"
      "cvbs_presets:\n"
      "  video_standard_preset: PAL\n"
      "  sample_encoding_preset: CVBS_U10_4FSC\n"
      "  signal_state_preset: STANDARD_STABLE_LOCKED\n"
      "output:\n"
      "  video_path: out.cvbs\n"
      "sections:\n"
      "  - name: ProgressiveVideo\n"
      "    type: progressive\n"
      "    source: fixture.mkv\n"
      "    duration_frames: all\n"
      "    duration_repeat: 4\n";

  YamlProjectParser parser;
  const ParseResult result = parser.ParseString(yaml);

  ASSERT_TRUE(result.ok);
  ASSERT_EQ(result.project.sections.size(), 1U);
  EXPECT_TRUE(result.project.sections[0].duration_frames_all);
  EXPECT_EQ(result.project.sections[0].duration_frames_repeat, 4);
}

TEST(YamlProjectParserTest, DurationRepeatDefaultsToOneWhenAbsent) {
  const std::string yaml =
      "project:\n"
      "  name: ProgressiveAllDefault\n"
      "  version: \"1.0\"\n"
      "cvbs_presets:\n"
      "  video_standard_preset: PAL\n"
      "  sample_encoding_preset: CVBS_U10_4FSC\n"
      "  signal_state_preset: STANDARD_STABLE_LOCKED\n"
      "output:\n"
      "  video_path: out.cvbs\n"
      "sections:\n"
      "  - name: ProgressiveVideo\n"
      "    type: progressive\n"
      "    source: fixture.mkv\n"
      "    duration_frames: all\n";

  YamlProjectParser parser;
  const ParseResult result = parser.ParseString(yaml);

  ASSERT_TRUE(result.ok);
  ASSERT_EQ(result.project.sections.size(), 1U);
  EXPECT_EQ(result.project.sections[0].duration_frames_repeat, 1);
}

TEST(YamlProjectParserTest, RejectsNonIntegerDurationRepeat) {
  const std::string yaml =
      "project:\n"
      "  name: BadRepeat\n"
      "  version: \"1.0\"\n"
      "cvbs_presets:\n"
      "  video_standard_preset: PAL\n"
      "  sample_encoding_preset: CVBS_U10_4FSC\n"
      "  signal_state_preset: STANDARD_STABLE_LOCKED\n"
      "output:\n"
      "  video_path: out.cvbs\n"
      "sections:\n"
      "  - name: ProgressiveVideo\n"
      "    type: progressive\n"
      "    source: fixture.mkv\n"
      "    duration_frames: all\n"
      "    duration_repeat: twice\n";

  YamlProjectParser parser;
  const ParseResult result = parser.ParseString(yaml);

  EXPECT_FALSE(result.ok);
}

TEST(YamlProjectParserTest, ParsesProgressiveSectionWithIntegerDuration) {
  const std::string yaml =
      "project:\n"
      "  name: ProgressiveFixed\n"
      "  version: \"1.0\"\n"
      "cvbs_presets:\n"
      "  video_standard_preset: NTSC\n"
      "  sample_encoding_preset: CVBS_U10_4FSC\n"
      "  signal_state_preset: STANDARD_STABLE_LOCKED\n"
      "output:\n"
      "  video_path: out.cvbs\n"
      "sections:\n"
      "  - name: ProgressiveStill\n"
      "    type: progressive\n"
      "    source: fixture.exr\n"
      "    duration_frames: 8\n";

  YamlProjectParser parser;
  const ParseResult result = parser.ParseString(yaml);

  ASSERT_TRUE(result.ok);
  ASSERT_EQ(result.project.sections.size(), 1U);
  EXPECT_FALSE(result.project.sections[0].duration_frames_all);
  EXPECT_EQ(result.project.sections[0].duration_frames, 8);
}

TEST(YamlProjectParserTest, ParsesOptionalNtscBlackSetupIre) {
  const std::string yaml =
      "project:\n"
      "  name: NtscZeroSetup\n"
      "  version: \"1.0\"\n"
      "cvbs_presets:\n"
      "  video_standard_preset: NTSC\n"
      "  sample_encoding_preset: CVBS_U10_4FSC\n"
      "  signal_state_preset: STANDARD_STABLE_LOCKED\n"
      "  ntsc_black_setup_ire: 0.0\n"
      "output:\n"
      "  video_path: out.cvbs\n"
      "sections:\n"
      "  - name: ProgressiveStill\n"
      "    type: progressive\n"
      "    source: fixture.exr\n"
      "    duration_frames: 8\n";

  YamlProjectParser parser;
  const ParseResult result = parser.ParseString(yaml);

  ASSERT_TRUE(result.ok);
  EXPECT_TRUE(result.project.cvbs_presets.ntsc_black_setup_ire_specified);
  EXPECT_DOUBLE_EQ(result.project.cvbs_presets.ntsc_black_setup_ire, 0.0);
}

TEST(YamlProjectParserTest, RejectsInvalidDurationFramesScalar) {
  const std::string yaml =
      "project:\n"
      "  name: ProgressiveInvalid\n"
      "  version: \"1.0\"\n"
      "cvbs_presets:\n"
      "  video_standard_preset: PAL\n"
      "  sample_encoding_preset: CVBS_U10_4FSC\n"
      "  signal_state_preset: STANDARD_STABLE_LOCKED\n"
      "output:\n"
      "  video_path: out.cvbs\n"
      "sections:\n"
      "  - name: ProgressiveBad\n"
      "    type: progressive\n"
      "    source: fixture.mkv\n"
      "    duration_frames: forever\n";

  YamlProjectParser parser;
  const ParseResult result = parser.ParseString(yaml);

  EXPECT_FALSE(result.ok);
  EXPECT_FALSE(result.errors.empty());
}

std::string PresetYaml(const std::string& presets) {
  return "project:\n"
         "  name: PresetParse\n"
         "  version: \"1.0\"\n"
         "cvbs_presets:\n" +
         presets +
         "output:\n"
         "  video_path: out.cvbs\n"
         "sections:\n"
         "  - name: Still\n"
         "    type: progressive\n"
         "    source: fixture.exr\n"
         "    duration_frames: 2\n";
}

TEST(YamlProjectParserTest, ParsesSecamPresetAndFieldIdentification) {
  YamlProjectParser parser;
  const ParseResult result = parser.ParseString(
      PresetYaml("  video_standard_preset: SECAM\n"
                 "  sample_encoding_preset: CVBS_U10_4FSC\n"
                 "  signal_state_preset: STANDARD_STABLE_UNLOCKED\n"
                 "  secam_field_identification: false\n"));

  ASSERT_TRUE(result.ok);
  EXPECT_EQ(result.project.cvbs_presets.video_standard_preset,
            Standard::kSecam);
  EXPECT_EQ(result.project.cvbs_presets.signal_state_preset,
            "STANDARD_STABLE_UNLOCKED");
  EXPECT_FALSE(result.project.cvbs_presets.secam_field_identification);
}

TEST(YamlProjectParserTest, OmittedSignalStateDefaultsPerStandard) {
  YamlProjectParser parser;
  const ParseResult secam = parser.ParseString(
      PresetYaml("  video_standard_preset: SECAM\n"
                 "  sample_encoding_preset: CVBS_U10_4FSC\n"));
  ASSERT_TRUE(secam.ok);
  EXPECT_EQ(secam.project.cvbs_presets.signal_state_preset,
            "STANDARD_STABLE_UNLOCKED");
  EXPECT_TRUE(secam.project.cvbs_presets.secam_field_identification);

  const ParseResult pal = parser.ParseString(
      PresetYaml("  video_standard_preset: PAL\n"
                 "  sample_encoding_preset: CVBS_U10_4FSC\n"));
  ASSERT_TRUE(pal.ok);
  EXPECT_EQ(pal.project.cvbs_presets.signal_state_preset,
            "STANDARD_STABLE_LOCKED");
}

TEST(YamlProjectParserTest, RejectsChromaEncodingKey) {
  // SECAM is a video standard preset (CVBS File Format Specification v1.7.0),
  // not a colour-encoding option on PAL.
  YamlProjectParser parser;
  const ParseResult result = parser.ParseString(
      PresetYaml("  video_standard_preset: PAL\n"
                 "  sample_encoding_preset: CVBS_U10_4FSC\n"
                 "  signal_state_preset: STANDARD_STABLE_LOCKED\n"
                 "  chroma_encoding: SECAM\n"));

  EXPECT_FALSE(result.ok);
  ASSERT_FALSE(result.errors.empty());
  EXPECT_NE(result.errors.front().find("chroma_encoding"), std::string::npos);
}

TEST(YamlProjectParserTest, ParsesLineInjectionsAndLaserdiscPresetFlags) {
  const std::string yaml =
      "project:\n"
      "  name: InjectionParse\n"
      "  version: \"1.0\"\n"
      "cvbs_presets:\n"
      "  video_standard_preset: PAL\n"
      "  sample_encoding_preset: CVBS_U10_4FSC\n"
      "  signal_state_preset: STANDARD_STABLE_LOCKED\n"
      "output:\n"
      "  video_path: out.cvbs\n"
      "line_injections:\n"
      "  vits:\n"
      "    - vits_type: virs\n"
      "      target_lines: [19, 20]\n"
      "laserdisc:\n"
      "  disc_type: CAV\n"
      "  pilot_burst: true\n"
      "  vbi_burst: false\n"
      "sections:\n"
      "  - name: ProgressiveWithInjections\n"
      "    type: progressive\n"
      "    source: fixture.exr\n"
      "    duration_frames: 4\n"
      "    laserdisc:\n"
      "      area: programme_area\n"
      "      codes:\n"
      "        - code_type: picture_number\n"
      "          start_value: 1\n";

  YamlProjectParser parser;
  const ParseResult result = parser.ParseString(yaml);

  ASSERT_TRUE(result.ok);
  ASSERT_EQ(result.project.sections.size(), 1U);
  EXPECT_TRUE(result.project.laserdisc.pilot_burst);
  EXPECT_FALSE(result.project.laserdisc.vbi_burst);

  // The laserdisc block's disc_type and the project-wide VITS set.
  EXPECT_EQ(result.project.laserdisc.disc_type, "CAV");
  ASSERT_EQ(result.project.line_injections.vits.size(), 1U);
  EXPECT_EQ(result.project.line_injections.vits[0].vits_type, "virs");
  ASSERT_EQ(result.project.line_injections.vits[0].target_lines.size(), 2U);
  EXPECT_EQ(result.project.line_injections.vits[0].target_lines[0], 19);
  EXPECT_EQ(result.project.line_injections.vits[0].target_lines[1], 20);

  // Section-level injections carry only laserdisc codes now.
  ASSERT_EQ(result.project.sections[0].line_injections.size(), 1U);
  EXPECT_EQ(result.project.sections[0].line_injections[0].type, "laserdisc");
  ASSERT_EQ(result.project.sections[0].line_injections[0].codes.size(), 1U);
  EXPECT_EQ(result.project.sections[0].line_injections[0].codes[0].code_type,
            "picture_number");
  EXPECT_TRUE(result.project.sections[0]
                  .line_injections[0]
                  .codes[0]
                  .start_value_specified);
  EXPECT_EQ(result.project.sections[0].line_injections[0].codes[0].start_value,
            1);
}

TEST(YamlProjectParserTest, DefaultsVitsPlacementToStandardWhenAbsent) {
  const std::string yaml =
      "cvbs_presets:\n"
      "  video_standard_preset: PAL\n"
      "output:\n"
      "  video_path: out.cvbs\n"
      "line_injections:\n"
      "  vits:\n"
      "    - vits_type: vits17\n"
      "      target_lines: [17]\n"
      "sections:\n"
      "  - name: Bars\n"
      "    type: progressive\n"
      "    source: fixture.exr\n"
      "    duration_frames: 4\n";

  YamlProjectParser parser;
  const ParseResult result = parser.ParseString(yaml);

  ASSERT_TRUE(result.ok);
  EXPECT_EQ(result.project.line_injections.placement, VitsPlacement::kStandard);
}

TEST(YamlProjectParserTest, ParsesLaserdiscVitsPlacement) {
  const std::string yaml =
      "cvbs_presets:\n"
      "  video_standard_preset: PAL\n"
      "output:\n"
      "  video_path: out.cvbs\n"
      "line_injections:\n"
      "  placement: laserdisc\n"
      "  vits:\n"
      "    - vits_type: uk-national\n"
      "      target_lines: [19, 332]\n"
      "sections:\n"
      "  - name: Bars\n"
      "    type: progressive\n"
      "    source: fixture.exr\n"
      "    duration_frames: 4\n";

  YamlProjectParser parser;
  const ParseResult result = parser.ParseString(yaml);

  ASSERT_TRUE(result.ok);
  EXPECT_EQ(result.project.line_injections.placement,
            VitsPlacement::kLaserdisc);
}

TEST(YamlProjectParserTest, RejectsUnknownVitsPlacement) {
  const std::string yaml =
      "cvbs_presets:\n"
      "  video_standard_preset: PAL\n"
      "output:\n"
      "  video_path: out.cvbs\n"
      "line_injections:\n"
      "  placement: broadcast\n"
      "sections:\n"
      "  - name: Bars\n"
      "    type: progressive\n"
      "    source: fixture.exr\n"
      "    duration_frames: 4\n";

  YamlProjectParser parser;
  const ParseResult result = parser.ParseString(yaml);

  EXPECT_FALSE(result.ok);
  ASSERT_FALSE(result.errors.empty());
  EXPECT_EQ(result.errors[0],
            "line_injections.placement must be 'standard', 'laserdisc', or "
            "'custom'.");
}

TEST(YamlProjectParserTest, RejectsUnsupportedLineInjectionField) {
  const std::string yaml =
      "project:\n"
      "  name: InjectionInvalid\n"
      "  version: \"1.0\"\n"
      "cvbs_presets:\n"
      "  video_standard_preset: PAL\n"
      "  sample_encoding_preset: CVBS_U10_4FSC\n"
      "  signal_state_preset: STANDARD_STABLE_LOCKED\n"
      "output:\n"
      "  video_path: out.cvbs\n"
      "sections:\n"
      "  - name: ProgressiveWithInjections\n"
      "    type: progressive\n"
      "    source: fixture.exr\n"
      "    duration_frames: 4\n"
      "    line_injections:\n"
      "      - type: vits\n"
      "        unsupported_field: true\n";

  YamlProjectParser parser;
  const ParseResult result = parser.ParseString(yaml);

  EXPECT_FALSE(result.ok);
  ASSERT_FALSE(result.errors.empty());
}

TEST(YamlProjectParserTest, RejectsDeprecatedSourcePixelFormatField) {
  const std::string yaml =
      "project:\n"
      "  name: DeprecatedField\n"
      "  version: \"1.0\"\n"
      "cvbs_presets:\n"
      "  video_standard_preset: PAL\n"
      "  sample_encoding_preset: CVBS_U10_4FSC\n"
      "  signal_state_preset: STANDARD_STABLE_LOCKED\n"
      "output:\n"
      "  video_path: out.cvbs\n"
      "sections:\n"
      "  - name: ProgressiveStill\n"
      "    type: progressive\n"
      "    source: fixture.exr\n"
      "    source_pixel_format: yuv422p10le\n"
      "    duration_frames: 8\n";

  YamlProjectParser parser;
  const ParseResult result = parser.ParseString(yaml);

  EXPECT_FALSE(result.ok);
  ASSERT_FALSE(result.errors.empty());
  EXPECT_NE(result.errors[0].find("unsupported field"), std::string::npos);
  EXPECT_NE(result.errors[0].find("source_pixel_format"), std::string::npos);
}

TEST(YamlProjectParserTest, ParsesSectionTypeLeadIn) {
  const std::string yaml =
      "project:\n"
      "  name: SectionTypeTest\n"
      "  version: \"1.0\"\n"
      "cvbs_presets:\n"
      "  video_standard_preset: PAL\n"
      "  sample_encoding_preset: CVBS_U10_4FSC\n"
      "  signal_state_preset: STANDARD_STABLE_LOCKED\n"
      "output:\n"
      "  video_path: out.cvbs\n"
      "sections:\n"
      "  - name: LeadInSection\n"
      "    type: progressive\n"
      "    source: fixture.exr\n"
      "    duration_frames: 8\n"
      "    laserdisc:\n"
      "      area: lead_in\n";

  YamlProjectParser parser;
  const ParseResult result = parser.ParseString(yaml);

  ASSERT_TRUE(result.ok);
  ASSERT_EQ(result.project.sections.size(), 1U);
  EXPECT_EQ(result.project.sections[0].section_type, SectionType::kLeadIn);
}

TEST(YamlProjectParserTest, ParsesSectionTypeProgrammeArea) {
  const std::string yaml =
      "project:\n"
      "  name: SectionTypeTest\n"
      "  version: \"1.0\"\n"
      "cvbs_presets:\n"
      "  video_standard_preset: PAL\n"
      "  sample_encoding_preset: CVBS_U10_4FSC\n"
      "  signal_state_preset: STANDARD_STABLE_LOCKED\n"
      "output:\n"
      "  video_path: out.cvbs\n"
      "sections:\n"
      "  - name: ProgrammeSection\n"
      "    type: progressive\n"
      "    source: fixture.exr\n"
      "    duration_frames: 8\n"
      "    laserdisc:\n"
      "      area: programme_area\n";

  YamlProjectParser parser;
  const ParseResult result = parser.ParseString(yaml);

  ASSERT_TRUE(result.ok);
  ASSERT_EQ(result.project.sections.size(), 1U);
  EXPECT_EQ(result.project.sections[0].section_type,
            SectionType::kProgrammeArea);
}

TEST(YamlProjectParserTest, ParsesSectionTypeLeadOut) {
  const std::string yaml =
      "project:\n"
      "  name: SectionTypeTest\n"
      "  version: \"1.0\"\n"
      "cvbs_presets:\n"
      "  video_standard_preset: PAL\n"
      "  sample_encoding_preset: CVBS_U10_4FSC\n"
      "  signal_state_preset: STANDARD_STABLE_LOCKED\n"
      "output:\n"
      "  video_path: out.cvbs\n"
      "sections:\n"
      "  - name: LeadOutSection\n"
      "    type: progressive\n"
      "    source: fixture.exr\n"
      "    duration_frames: 8\n"
      "    laserdisc:\n"
      "      area: lead_out\n";

  YamlProjectParser parser;
  const ParseResult result = parser.ParseString(yaml);

  ASSERT_TRUE(result.ok);
  ASSERT_EQ(result.project.sections.size(), 1U);
  EXPECT_EQ(result.project.sections[0].section_type, SectionType::kLeadOut);
}

TEST(YamlProjectParserTest, SectionWithNoSectionTypeDefaultsToUnknown) {
  const std::string yaml =
      "project:\n"
      "  name: SectionTypeTest\n"
      "  version: \"1.0\"\n"
      "cvbs_presets:\n"
      "  video_standard_preset: PAL\n"
      "  sample_encoding_preset: CVBS_U10_4FSC\n"
      "  signal_state_preset: STANDARD_STABLE_LOCKED\n"
      "output:\n"
      "  video_path: out.cvbs\n"
      "sections:\n"
      "  - name: DefaultSection\n"
      "    type: progressive\n"
      "    source: fixture.exr\n"
      "    duration_frames: 8\n";

  YamlProjectParser parser;
  const ParseResult result = parser.ParseString(yaml);

  ASSERT_TRUE(result.ok);
  ASSERT_EQ(result.project.sections.size(), 1U);
  EXPECT_EQ(result.project.sections[0].section_type, SectionType::kUnknown);
}

TEST(YamlProjectParserTest, RejectsUnrecognisedSectionTypeValue) {
  const std::string yaml =
      "project:\n"
      "  name: SectionTypeTest\n"
      "  version: \"1.0\"\n"
      "cvbs_presets:\n"
      "  video_standard_preset: PAL\n"
      "  sample_encoding_preset: CVBS_U10_4FSC\n"
      "  signal_state_preset: STANDARD_STABLE_LOCKED\n"
      "output:\n"
      "  video_path: out.cvbs\n"
      "sections:\n"
      "  - name: BadSection\n"
      "    type: progressive\n"
      "    source: fixture.exr\n"
      "    duration_frames: 8\n"
      "    laserdisc:\n"
      "      area: programme\n";

  YamlProjectParser parser;
  const ParseResult result = parser.ParseString(yaml);

  EXPECT_FALSE(result.ok);
  ASSERT_FALSE(result.errors.empty());
  EXPECT_NE(result.errors[0].find("laserdisc.area"), std::string::npos);
  EXPECT_NE(result.errors[0].find("programme"), std::string::npos);
}

TEST(YamlProjectParserTest, ParsesOsdOverlayBlock) {
  const std::string yaml =
      "project:\n"
      "  name: OsdTest\n"
      "  version: \"1.0\"\n"
      "cvbs_presets:\n"
      "  video_standard_preset: PAL\n"
      "  sample_encoding_preset: CVBS_U10_4FSC\n"
      "  signal_state_preset: STANDARD_STABLE_LOCKED\n"
      "output:\n"
      "  video_path: out.cvbs\n"
      "sections:\n"
      "  - name: OsdSection\n"
      "    type: progressive\n"
      "    source: fixture.exr\n"
      "    duration_frames: 4\n"
      "    osd:\n"
      "      overlays:\n"
      "        - text: \"LABEL\"\n"
      "          x: 10\n"
      "          y: 5\n"
      "          scale: 2\n"
      "          fg_luma: white\n"
      "          bg_luma: black\n"
      "        - text: \"PN:{picture_number}\"\n"
      "          x: 0\n"
      "          y: 20\n"
      "          scale: 1\n"
      "          fg_luma: light_grey\n"
      "          bg_luma: transparent\n";

  YamlProjectParser parser;
  const ParseResult result = parser.ParseString(yaml);

  ASSERT_TRUE(result.ok);
  ASSERT_EQ(result.project.sections.size(), 1U);
  const OsdConfig& osd = result.project.sections[0].osd;
  ASSERT_EQ(osd.overlays.size(), 2U);

  EXPECT_EQ(osd.overlays[0].text, "LABEL");
  EXPECT_EQ(osd.overlays[0].x, 10);
  EXPECT_EQ(osd.overlays[0].y, 5);
  EXPECT_EQ(osd.overlays[0].scale, 2);
  EXPECT_EQ(osd.overlays[0].fg_level, OsdFgLevel::kWhite);
  EXPECT_EQ(osd.overlays[0].bg_level, OsdBgLevel::kBlack);

  EXPECT_EQ(osd.overlays[1].text, "PN:{picture_number}");
  EXPECT_EQ(osd.overlays[1].x, 0);
  EXPECT_EQ(osd.overlays[1].y, 20);
  EXPECT_EQ(osd.overlays[1].scale, 1);
  EXPECT_EQ(osd.overlays[1].fg_level, OsdFgLevel::kLightGrey);
  EXPECT_EQ(osd.overlays[1].bg_level, OsdBgLevel::kTransparent);
}

TEST(YamlProjectParserTest, OsdOverlayDefaultsApplied) {
  const std::string yaml =
      "project:\n"
      "  name: OsdDefaults\n"
      "  version: \"1.0\"\n"
      "cvbs_presets:\n"
      "  video_standard_preset: PAL\n"
      "  sample_encoding_preset: CVBS_U10_4FSC\n"
      "  signal_state_preset: STANDARD_STABLE_LOCKED\n"
      "output:\n"
      "  video_path: out.cvbs\n"
      "sections:\n"
      "  - name: OsdSection\n"
      "    type: progressive\n"
      "    source: fixture.exr\n"
      "    duration_frames: 4\n"
      "    osd:\n"
      "      overlays:\n"
      "        - text: \"ONLY TEXT\"\n";

  YamlProjectParser parser;
  const ParseResult result = parser.ParseString(yaml);

  ASSERT_TRUE(result.ok);
  ASSERT_EQ(result.project.sections.size(), 1U);
  const OsdConfig& osd = result.project.sections[0].osd;
  ASSERT_EQ(osd.overlays.size(), 1U);
  EXPECT_EQ(osd.overlays[0].x, 0);
  EXPECT_EQ(osd.overlays[0].y, 0);
  EXPECT_EQ(osd.overlays[0].scale, 1);
  EXPECT_EQ(osd.overlays[0].fg_level, OsdFgLevel::kWhite);
  EXPECT_EQ(osd.overlays[0].bg_level, OsdBgLevel::kTransparent);
}

TEST(YamlProjectParserTest, OsdOverlayNumericFgLumaParsesAsUnknownLevel) {
  const std::string yaml =
      "project:\n"
      "  name: OsdLegacyFg\n"
      "  version: \"1.0\"\n"
      "cvbs_presets:\n"
      "  video_standard_preset: PAL\n"
      "  sample_encoding_preset: CVBS_U10_4FSC\n"
      "  signal_state_preset: STANDARD_STABLE_LOCKED\n"
      "output:\n"
      "  video_path: out.cvbs\n"
      "sections:\n"
      "  - name: OsdSection\n"
      "    type: progressive\n"
      "    source: fixture.exr\n"
      "    duration_frames: 4\n"
      "    osd:\n"
      "      overlays:\n"
      "        - text: \"ONLY TEXT\"\n"
      "          fg_luma: 0.8\n"
      "          bg_luma: -1.0\n";

  YamlProjectParser parser;
  const ParseResult result = parser.ParseString(yaml);

  // Legacy numeric fg_luma/bg_luma values parse (for a clear validation
  // message) but map to kUnknown so the validator rejects them.
  ASSERT_TRUE(result.ok);
  ASSERT_EQ(result.project.sections.size(), 1U);
  const OsdConfig& osd = result.project.sections[0].osd;
  ASSERT_EQ(osd.overlays.size(), 1U);
  EXPECT_EQ(osd.overlays[0].fg_level, OsdFgLevel::kUnknown);
  EXPECT_EQ(osd.overlays[0].fg_level_text, "0.8");
  EXPECT_EQ(osd.overlays[0].bg_level, OsdBgLevel::kUnknown);
  EXPECT_EQ(osd.overlays[0].bg_level_text, "-1.0");
}

TEST(YamlProjectParserTest, OsdBlockNotAMapReturnsError) {
  const std::string yaml =
      "project:\n"
      "  name: OsdBad\n"
      "  version: \"1.0\"\n"
      "cvbs_presets:\n"
      "  video_standard_preset: PAL\n"
      "  sample_encoding_preset: CVBS_U10_4FSC\n"
      "  signal_state_preset: STANDARD_STABLE_LOCKED\n"
      "output:\n"
      "  video_path: out.cvbs\n"
      "sections:\n"
      "  - name: OsdSection\n"
      "    type: progressive\n"
      "    source: fixture.exr\n"
      "    duration_frames: 4\n"
      "    osd: \"not a map\"\n";

  YamlProjectParser parser;
  const ParseResult result = parser.ParseString(yaml);

  EXPECT_FALSE(result.ok);
  ASSERT_FALSE(result.errors.empty());
  EXPECT_NE(result.errors[0].find("osd"), std::string::npos);
}

TEST(YamlProjectParserTest, OsdOverlaysNotAListReturnsError) {
  const std::string yaml =
      "project:\n"
      "  name: OsdBad\n"
      "  version: \"1.0\"\n"
      "cvbs_presets:\n"
      "  video_standard_preset: PAL\n"
      "  sample_encoding_preset: CVBS_U10_4FSC\n"
      "  signal_state_preset: STANDARD_STABLE_LOCKED\n"
      "output:\n"
      "  video_path: out.cvbs\n"
      "sections:\n"
      "  - name: OsdSection\n"
      "    type: progressive\n"
      "    source: fixture.exr\n"
      "    duration_frames: 4\n"
      "    osd:\n"
      "      overlays: scalar\n";

  YamlProjectParser parser;
  const ParseResult result = parser.ParseString(yaml);

  EXPECT_FALSE(result.ok);
  ASSERT_FALSE(result.errors.empty());
  EXPECT_NE(result.errors[0].find("overlays"), std::string::npos);
}

TEST(YamlProjectParserTest, RejectsRemovedDiscSkipsTopLevelKey) {
  const std::string yaml =
      "cvbs_presets:\n"
      "  video_standard_preset: PAL\n"
      "output:\n"
      "  video_path: out.cvbs\n"
      "sections:\n"
      "  - name: S\n"
      "    type: progressive\n"
      "    source: fixture.exr\n"
      "    duration_frames: 10\n"
      "disc_skips:\n"
      "  - at_frame: 3\n"
      "    direction: forward\n"
      "    count: 2\n";

  YamlProjectParser parser;
  const ParseResult result = parser.ParseString(yaml);

  EXPECT_FALSE(result.ok);
  ASSERT_FALSE(result.errors.empty());
  EXPECT_NE(result.errors[0].find("disc_skips"), std::string::npos);
}

TEST(YamlProjectParserTest, ParsesFixedFrequencyChannelPair) {
  const std::string yaml =
      "cvbs_presets:\n"
      "  video_standard_preset: PAL\n"
      "output:\n"
      "  video_path: out.cvbs\n"
      "sections:\n"
      "  - name: Tone\n"
      "    type: progressive\n"
      "    source: fixture.exr\n"
      "    duration_frames: 10\n"
      "    audio:\n"
      "      channel_pairs:\n"
      "        - pair: 0\n"
      "          description: Analogue stereo\n"
      "          left:\n"
      "            waveform: square\n"
      "            frequency: 440.0\n"
      "            amplitude: 0.8\n"
      "          right:\n"
      "            waveform: sine\n"
      "            frequency: 1000.0\n";

  YamlProjectParser parser;
  const ParseResult result = parser.ParseString(yaml);

  ASSERT_TRUE(result.ok);
  ASSERT_EQ(result.project.sections.size(), 1U);
  ASSERT_EQ(result.project.sections[0].audio_channel_pairs.size(), 1U);
  const AudioChannelPair& cp =
      result.project.sections[0].audio_channel_pairs[0];
  EXPECT_EQ(cp.pair, 0);
  EXPECT_TRUE(cp.pair_specified);
  EXPECT_EQ(cp.description, "Analogue stereo");
  EXPECT_TRUE(cp.left.enabled);
  EXPECT_EQ(cp.left.waveform, AudioWaveform::kSquare);
  EXPECT_DOUBLE_EQ(cp.left.frequency_hz, 440.0);
  EXPECT_DOUBLE_EQ(cp.left.amplitude, 0.8);
  EXPECT_FALSE(cp.left.ramp_enabled);
  EXPECT_TRUE(cp.right.enabled);
  EXPECT_EQ(cp.right.waveform, AudioWaveform::kSine);
}

TEST(YamlProjectParserTest, ChannelWithOmittedChannelIsSilent) {
  const std::string yaml =
      "cvbs_presets:\n"
      "  video_standard_preset: PAL\n"
      "output:\n"
      "  video_path: out.cvbs\n"
      "sections:\n"
      "  - name: Tone\n"
      "    type: progressive\n"
      "    source: fixture.exr\n"
      "    duration_frames: 10\n"
      "    audio:\n"
      "      channel_pairs:\n"
      "        - pair: 2\n"
      "          left: {}\n";

  YamlProjectParser parser;
  const ParseResult result = parser.ParseString(yaml);

  ASSERT_TRUE(result.ok);
  const AudioChannelPair& cp =
      result.project.sections[0].audio_channel_pairs[0];
  EXPECT_EQ(cp.pair, 2);
  // Omitted fields fall back to defaults; the left channel is active.
  EXPECT_TRUE(cp.left.enabled);
  EXPECT_EQ(cp.left.waveform, AudioWaveform::kSine);
  EXPECT_DOUBLE_EQ(cp.left.frequency_hz, 1000.0);
  EXPECT_DOUBLE_EQ(cp.left.amplitude, 0.5);
  // The omitted right channel stays silent.
  EXPECT_FALSE(cp.right.enabled);
}

TEST(YamlProjectParserTest, ParsesSectionSpanningRampChannel) {
  const std::string yaml =
      "cvbs_presets:\n"
      "  video_standard_preset: PAL\n"
      "output:\n"
      "  video_path: out.cvbs\n"
      "sections:\n"
      "  - name: Sweep\n"
      "    type: progressive\n"
      "    source: fixture.exr\n"
      "    duration_frames: 10\n"
      "    audio:\n"
      "      channel_pairs:\n"
      "        - pair: 0\n"
      "          left:\n"
      "            waveform: sine\n"
      "            ramp:\n"
      "              start: 100.0\n"
      "              end: 2000.0\n"
      "              mode: up\n";

  YamlProjectParser parser;
  const ParseResult result = parser.ParseString(yaml);

  ASSERT_TRUE(result.ok);
  const AudioParameters& audio =
      result.project.sections[0].audio_channel_pairs[0].left;
  EXPECT_TRUE(audio.enabled);
  EXPECT_TRUE(audio.ramp_enabled);
  EXPECT_TRUE(audio.ramp_start_specified);
  EXPECT_TRUE(audio.ramp_end_specified);
  EXPECT_DOUBLE_EQ(audio.ramp_start_hz, 100.0);
  EXPECT_DOUBLE_EQ(audio.ramp_end_hz, 2000.0);
  EXPECT_EQ(audio.ramp_mode, AudioRampMode::kUp);
  EXPECT_DOUBLE_EQ(audio.ramp_period_seconds, 0.0);
}

TEST(YamlProjectParserTest, ParsesPeriodicRampChannel) {
  const std::string yaml =
      "cvbs_presets:\n"
      "  video_standard_preset: PAL\n"
      "output:\n"
      "  video_path: out.cvbs\n"
      "sections:\n"
      "  - name: Sweep\n"
      "    type: progressive\n"
      "    source: fixture.exr\n"
      "    duration_frames: 25\n"
      "    audio:\n"
      "      channel_pairs:\n"
      "        - pair: 0\n"
      "          left:\n"
      "            ramp:\n"
      "              start: 500.0\n"
      "              end: 50.0\n"
      "              mode: bounce\n"
      "              period: 0.5\n";

  YamlProjectParser parser;
  const ParseResult result = parser.ParseString(yaml);

  ASSERT_TRUE(result.ok);
  const AudioParameters& audio =
      result.project.sections[0].audio_channel_pairs[0].left;
  EXPECT_TRUE(audio.ramp_enabled);
  EXPECT_EQ(audio.ramp_mode, AudioRampMode::kBounce);
  EXPECT_DOUBLE_EQ(audio.ramp_period_seconds, 0.5);
}

TEST(YamlProjectParserTest, ParsesMultipleChannelPairs) {
  const std::string yaml =
      "cvbs_presets:\n"
      "  video_standard_preset: PAL\n"
      "output:\n"
      "  video_path: out.cvbs\n"
      "sections:\n"
      "  - name: Tone\n"
      "    type: progressive\n"
      "    source: fixture.exr\n"
      "    duration_frames: 10\n"
      "    audio:\n"
      "      channel_pairs:\n"
      "        - pair: 0\n"
      "          left: { waveform: sine }\n"
      "          right: { waveform: sine }\n"
      "        - pair: 7\n"
      "          description: Commentary\n"
      "          left: { waveform: square, frequency: 440.0 }\n";

  YamlProjectParser parser;
  const ParseResult result = parser.ParseString(yaml);

  ASSERT_TRUE(result.ok);
  ASSERT_EQ(result.project.sections[0].audio_channel_pairs.size(), 2U);
  EXPECT_EQ(result.project.sections[0].audio_channel_pairs[0].pair, 0);
  EXPECT_EQ(result.project.sections[0].audio_channel_pairs[1].pair, 7);
  EXPECT_FALSE(result.project.sections[0].audio_channel_pairs[1].right.enabled);
}

TEST(YamlProjectParserTest, ParsesFileAndVideoAudioChannelPairs) {
  YamlProjectParser parser;
  const ParseResult result = parser.ParseString(MakeVideoSectionYaml(
      "    audio:\n"
      "      channel_pairs:\n"
      "        - pair: 0\n"
      "          video_audio: { stream: 1 }\n"
      "        - pair: 1\n"
      "          file: { path: \"{project}/narration.flac\", repeat: true }\n"
      "        - pair: 2\n"
      "          file: { path: music.wav }\n"
      "        - pair: 3\n"
      "          video_audio: {}\n"));

  ASSERT_TRUE(result.ok) << (result.errors.empty() ? "" : result.errors[0]);
  const std::vector<AudioChannelPair>& pairs =
      result.project.sections[0].audio_channel_pairs;
  ASSERT_EQ(pairs.size(), 4U);
  EXPECT_EQ(pairs[0].content, AudioPairContent::kVideo);
  EXPECT_EQ(pairs[0].video_audio.stream, 1);
  EXPECT_EQ(pairs[1].content, AudioPairContent::kFile);
  EXPECT_EQ(pairs[1].file.path, "{project}/narration.flac");
  EXPECT_TRUE(pairs[1].file.repeat);
  EXPECT_EQ(pairs[2].content, AudioPairContent::kFile);
  EXPECT_FALSE(pairs[2].file.repeat);
  EXPECT_EQ(pairs[3].content, AudioPairContent::kVideo);
  EXPECT_EQ(pairs[3].video_audio.stream, 0);
  EXPECT_FALSE(pairs[0].left.enabled);
}

TEST(YamlProjectParserTest, RejectsMixingTonesWithFileOrVideoAudio) {
  YamlProjectParser parser;
  for (const std::string& pair_body :
       {std::string("          file: { path: a.wav }\n"
                    "          left: { waveform: sine }\n"),
        std::string("          video_audio: {}\n"
                    "          right: { waveform: sine }\n"),
        std::string("          video_audio: {}\n"
                    "          file: { path: a.wav }\n")}) {
    const ParseResult result =
        parser.ParseString(MakeVideoSectionYaml("    audio:\n"
                                                "      channel_pairs:\n"
                                                "        - pair: 0\n" +
                                                pair_body));
    EXPECT_FALSE(result.ok) << pair_body;
    ASSERT_FALSE(result.errors.empty());
    EXPECT_NE(result.errors[0].find("only one of"), std::string::npos)
        << result.errors[0];
  }
}

TEST(YamlProjectParserTest, RejectsMalformedFileAudio) {
  YamlProjectParser parser;
  for (const std::string& file_value :
       {std::string("narration.wav"), std::string("{ repeat: true }"),
        std::string("{ path: a.wav, loop: true }")}) {
    const ParseResult result =
        parser.ParseString(MakeVideoSectionYaml("    audio:\n"
                                                "      channel_pairs:\n"
                                                "        - pair: 0\n"
                                                "          file: " +
                                                file_value + "\n"));
    EXPECT_FALSE(result.ok) << file_value;
  }
}

TEST(YamlProjectParserTest, RejectsUnknownVideoAudioKey) {
  YamlProjectParser parser;
  const ParseResult result = parser.ParseString(
      MakeVideoSectionYaml("    audio:\n"
                           "      channel_pairs:\n"
                           "        - pair: 0\n"
                           "          video_audio: { track: 1 }\n"));
  EXPECT_FALSE(result.ok);
}

TEST(YamlProjectParserTest, RejectsUnknownAudioKey) {
  const std::string yaml =
      "cvbs_presets:\n"
      "  video_standard_preset: PAL\n"
      "output:\n"
      "  video_path: out.cvbs\n"
      "sections:\n"
      "  - name: Tone\n"
      "    type: progressive\n"
      "    source: fixture.exr\n"
      "    duration_frames: 10\n"
      "    audio:\n"
      "      waveform: square\n";

  YamlProjectParser parser;
  const ParseResult result = parser.ParseString(yaml);

  EXPECT_FALSE(result.ok);
  ASSERT_FALSE(result.errors.empty());
}

TEST(YamlProjectParserTest, RejectsUnknownChannelKey) {
  const std::string yaml =
      "cvbs_presets:\n"
      "  video_standard_preset: PAL\n"
      "output:\n"
      "  video_path: out.cvbs\n"
      "sections:\n"
      "  - name: Tone\n"
      "    type: progressive\n"
      "    source: fixture.exr\n"
      "    duration_frames: 10\n"
      "    audio:\n"
      "      channel_pairs:\n"
      "        - pair: 0\n"
      "          left:\n"
      "            frequency: 440.0\n"
      "            gain: 0.8\n";

  YamlProjectParser parser;
  const ParseResult result = parser.ParseString(yaml);

  EXPECT_FALSE(result.ok);
  ASSERT_FALSE(result.errors.empty());
}

TEST(YamlProjectParserTest, RejectsUnknownChannelRampKey) {
  const std::string yaml =
      "cvbs_presets:\n"
      "  video_standard_preset: PAL\n"
      "output:\n"
      "  video_path: out.cvbs\n"
      "sections:\n"
      "  - name: Sweep\n"
      "    type: progressive\n"
      "    source: fixture.exr\n"
      "    duration_frames: 10\n"
      "    audio:\n"
      "      channel_pairs:\n"
      "        - pair: 0\n"
      "          left:\n"
      "            ramp:\n"
      "              start: 100.0\n"
      "              end: 200.0\n"
      "              slope: fast\n";

  YamlProjectParser parser;
  const ParseResult result = parser.ParseString(yaml);

  EXPECT_FALSE(result.ok);
  ASSERT_FALSE(result.errors.empty());
}

// Minimal project text with a `laserdisc` block whose body is supplied by the
// caller (no block when empty), used by the EFM digital audio cases below.
std::string MakeProjectWithLaserdiscBlock(const std::string& laserdisc_body) {
  return std::string(
             "project:\n"
             "  name: EfmAudio\n"
             "  version: \"1.0\"\n"
             "cvbs_presets:\n"
             "  video_standard_preset: PAL\n"
             "  sample_encoding_preset: CVBS_U10_4FSC\n"
             "  signal_state_preset: STANDARD_STABLE_LOCKED\n"
             "output:\n"
             "  video_path: out.cvbs\n") +
         (laserdisc_body.empty() ? "" : "laserdisc:\n" + laserdisc_body) +
         "sections:\n"
         "  - name: Programme\n"
         "    type: progressive\n"
         "    source: fixture.exr\n"
         "    duration_frames: 10\n";
}

TEST(YamlProjectParserTest, ParsesEfmAudioOutputPair) {
  const std::string yaml = MakeProjectWithLaserdiscBlock(
      "  efm_audio:\n"
      "    pair: 3\n");

  YamlProjectParser parser;
  const ParseResult result = parser.ParseString(yaml);

  ASSERT_TRUE(result.ok);
  EXPECT_TRUE(result.project.laserdisc.efm_audio.enabled);
  EXPECT_EQ(result.project.laserdisc.efm_audio.pair, 3);
}

TEST(YamlProjectParserTest, EfmAudioDisabledWhenLaserdiscBlockAbsent) {
  YamlProjectParser parser;
  const ParseResult result =
      parser.ParseString(MakeProjectWithLaserdiscBlock(""));

  ASSERT_TRUE(result.ok);
  EXPECT_FALSE(result.project.laserdisc.efm_audio.enabled);
  EXPECT_EQ(result.project.laserdisc.efm_audio.pair, 0);
}

TEST(YamlProjectParserTest, RejectsEfmAudioWithoutPair) {
  const std::string yaml = MakeProjectWithLaserdiscBlock(
      "  efm_audio:\n"
      "    {}\n");

  YamlProjectParser parser;
  const ParseResult result = parser.ParseString(yaml);

  EXPECT_FALSE(result.ok);
  ASSERT_FALSE(result.errors.empty());
}

TEST(YamlProjectParserTest, RejectsUnsupportedEfmAudioField) {
  const std::string yaml = MakeProjectWithLaserdiscBlock(
      "  efm_audio:\n"
      "    pair: 0\n"
      "    channel: left\n");

  YamlProjectParser parser;
  const ParseResult result = parser.ParseString(yaml);

  EXPECT_FALSE(result.ok);
  ASSERT_FALSE(result.errors.empty());
}

TEST(YamlProjectParserTest, RejectsNonIntegerEfmAudioPair) {
  const std::string yaml = MakeProjectWithLaserdiscBlock(
      "  efm_audio:\n"
      "    pair: first\n");

  YamlProjectParser parser;
  const ParseResult result = parser.ParseString(yaml);

  EXPECT_FALSE(result.ok);
  ASSERT_FALSE(result.errors.empty());
}

// Every LaserDisc key that used to live elsewhere is rejected at its old
// place with an error naming its new one.
TEST(YamlProjectParserTest, NamesTheNewHomeOfEveryMovedLaserdiscKey) {
  const std::string presets =
      "cvbs_presets:\n"
      "  video_standard_preset: PAL\n";
  const std::string output = "output:\n  video_path: out.cvbs\n";
  const std::string section =
      "sections:\n"
      "  - name: S\n"
      "    type: progressive\n"
      "    source: fixture.exr\n"
      "    duration_frames: 4\n";
  const struct {
    std::string yaml;
    const char* error;
  } cases[] = {
      {presets + "  pal_laserdisc_pilot_burst: true\n" + output + section,
       "cvbs_presets.pal_laserdisc_pilot_burst has moved to "
       "laserdisc.pilot_burst."},
      {presets + "  ntsc_laserdisc_vbi_burst: true\n" + output + section,
       "cvbs_presets.ntsc_laserdisc_vbi_burst has moved to "
       "laserdisc.vbi_burst."},
      {presets + output + "  efm_audio:\n    pair: 0\n" + section,
       "output.efm_audio has moved to laserdisc.efm_audio."},
      {presets + output + "line_injections:\n  disc_type: CAV\n" + section,
       "line_injections.disc_type has moved to laserdisc.disc_type."},
      {presets + output + section + "    section_type: lead_in\n",
       "section.section_type has moved to the section's laserdisc.area."},
      {presets + output + section +
           "    line_injections:\n"
           "      - type: laserdisc\n",
       "line_injections[] type 'laserdisc' has moved: laserdisc codes go in "
       "the section's laserdisc.codes list."},
  };
  for (const auto& test_case : cases) {
    const ParseResult result = YamlProjectParser().ParseString(test_case.yaml);
    ASSERT_FALSE(result.ok) << test_case.yaml;
    ASSERT_FALSE(result.errors.empty());
    EXPECT_EQ(result.errors[0], test_case.error);
  }
}

TEST(YamlProjectParserTest, ParsesTheLaserVisionChannel) {
  const ParseResult result =
      YamlProjectParser().ParseString(MakeProjectWithLaserdiscBlock(
          "  channel:\n"
          "    group_delay: { preset: iec60856_nominal, scale: 0.5 }\n"
          "    response_by_picture:\n"
          "      - {picture: 1, response_db: [[0, 0], [5.0, -4.0]]}\n"
          "      - {picture: 54000, response_db: [[0, 0], [5.0, -9.0]]}\n"
          "    field2_offset: { x_samples: 0.25, y_lines: -0.5 }\n"));
  ASSERT_TRUE(result.ok) << result.errors[0];
  const LaserdiscChannelParameters& channel = result.project.laserdisc.channel;
  EXPECT_TRUE(result.project.laserdisc.enabled);
  EXPECT_TRUE(channel.enabled);
  EXPECT_TRUE(channel.group_delay.enabled);
  EXPECT_EQ(channel.group_delay.preset, GroupDelayPreset::kIec60856Nominal);
  EXPECT_DOUBLE_EQ(channel.group_delay.scale, 0.5);
  ASSERT_EQ(channel.response_by_picture.size(), 2U);
  EXPECT_EQ(channel.response_by_picture[1].picture, 54000);
  EXPECT_DOUBLE_EQ(channel.response_by_picture[1].response_db[1].relative_db,
                   -9.0);
  EXPECT_TRUE(channel.field2_offset.enabled);
  EXPECT_DOUBLE_EQ(channel.field2_offset.x_samples, 0.25);
  EXPECT_DOUBLE_EQ(channel.field2_offset.y_lines, -0.5);

  const ParseResult custom =
      YamlProjectParser().ParseString(MakeProjectWithLaserdiscBlock(
          "  channel:\n"
          "    group_delay:\n"
          "      points_ns: [[0.5, 0], [4.43, -135]]\n"
          "    response_db: [[0, 0], [4.4, -2.0]]\n"));
  ASSERT_TRUE(custom.ok) << custom.errors[0];
  const LaserdiscChannelParameters& custom_channel =
      custom.project.laserdisc.channel;
  EXPECT_EQ(custom_channel.group_delay.preset, GroupDelayPreset::kCustom);
  ASSERT_EQ(custom_channel.group_delay.points_ns.size(), 2U);
  EXPECT_DOUBLE_EQ(custom_channel.group_delay.points_ns[1].delay_ns, -135.0);
  EXPECT_DOUBLE_EQ(custom_channel.group_delay.scale, 1.0);
  ASSERT_EQ(custom_channel.response_db.size(), 2U);
}

TEST(YamlProjectParserTest, RejectsMalformedLaserdiscBlocks) {
  const struct {
    const char* body;
    const char* error;
  } cases[] = {
      {"  disc_type: [CAV]\n", "laserdisc.disc_type must be a string."},
      {"  pilot_burst: maybe\n",
       "laserdisc.pilot_burst must be true or false."},
      {"  efm_audio:\n    pair: first\n",
       "laserdisc.efm_audio.pair must be an integer."},
      {"  channel: {}\n",
       "laserdisc.channel must set at least one of: fm_beat, group_delay, "
       "response_db, response_by_picture, field2_offset."},
      {"  channel:\n    group_delay: { scale: 1 }\n",
       "laserdisc.channel.group_delay must set exactly one of: preset, "
       "points_ns."},
      {"  channel:\n    group_delay: { preset: iec60857 }\n",
       "laserdisc.channel.group_delay.preset has unrecognised value "
       "'iec60857'. Expected: iec60856_nominal."},
      {"  channel:\n    group_delay: { points_ns: [[1, 2, 3]] }\n",
       "laserdisc.channel.group_delay.points_ns entries must be "
       "[frequency_mhz, delay_ns] pairs."},
      {"  channel:\n    response_db: [[0, flat]]\n",
       "laserdisc.channel.response_db entries must be numeric pairs."},
      {"  channel:\n    response_by_picture:\n      - {picture: 1}\n",
       "laserdisc.channel.response_by_picture[] must set both of: picture, "
       "response_db."},
      {"  channel:\n    field2_offset: {}\n",
       "laserdisc.channel.field2_offset must set at least one of: x_samples, "
       "y_lines."},
      {"  channel:\n    field2_offset: { x_samples: left }\n",
       "laserdisc.channel.field2_offset.x_samples must be a number."},
      {"  colour: true\n", "laserdisc contains unsupported field: 'colour'."},
  };
  for (const auto& test_case : cases) {
    const ParseResult result = YamlProjectParser().ParseString(
        MakeProjectWithLaserdiscBlock(test_case.body));
    ASSERT_FALSE(result.ok) << test_case.body;
    ASSERT_FALSE(result.errors.empty());
    EXPECT_EQ(result.errors[0], test_case.error) << test_case.body;
  }
}

TEST(YamlProjectParserTest, RejectsMalformedSectionLaserdiscBlocks) {
  const std::string head =
      "cvbs_presets:\n"
      "  video_standard_preset: PAL\n"
      "output:\n"
      "  video_path: out.cvbs\n"
      "laserdisc:\n"
      "  disc_type: CAV\n"
      "sections:\n"
      "  - name: S\n"
      "    type: progressive\n"
      "    source: fixture.exr\n"
      "    duration_frames: 4\n"
      "    laserdisc:\n";
  const struct {
    const char* body;
    const char* error;
  } cases[] = {
      {"      {}\n",
       "section.laserdisc must set at least one of: area, codes."},
      {"      area: [lead_in]\n", "section.laserdisc.area must be a string."},
      {"      codes: picture_number\n",
       "section.laserdisc.codes must be a list/sequence."},
      {"      codes:\n        - start_value: 1\n",
       "laserdisc.codes[] is missing required field: 'code_type'."},
      {"      codes:\n        - code_type: picture_number\n"
       "          start_value: one\n",
       "laserdisc.codes[].start_value must be an integer."},
      {"      target_lines: [16]\n",
       "section.laserdisc contains unsupported field: 'target_lines'."},
  };
  for (const auto& test_case : cases) {
    const ParseResult result =
        YamlProjectParser().ParseString(head + test_case.body);
    ASSERT_FALSE(result.ok) << test_case.body;
    ASSERT_FALSE(result.errors.empty());
    EXPECT_EQ(result.errors[0], test_case.error) << test_case.body;
  }
}

TEST(YamlProjectParserTest, ParsesDifferentialAndCrosstalk) {
  const std::string yaml =
      "cvbs_presets:\n"
      "  video_standard_preset: PAL\n"
      "output:\n"
      "  video_path: out.cvbs\n"
      "chroma_error:\n"
      "  differential:\n"
      "    gain: [[0, 1.0], [100, 0.95]]\n"
      "    phase_deg: [[0, 0.0], [100, 2.5]]\n"
      "  crosstalk: { v_to_u: 0.3, alternate_with_switch: true }\n"
      "sections:\n"
      "  - name: S\n"
      "    type: progressive\n"
      "    source: fixture.exr\n"
      "    duration_frames: 4\n";
  const ParseResult result = YamlProjectParser().ParseString(yaml);
  ASSERT_TRUE(result.ok) << result.errors[0];
  const ChromaErrorParameters& chroma_error = result.project.chroma_error;
  EXPECT_TRUE(chroma_error.enabled);
  EXPECT_DOUBLE_EQ(chroma_error.gain, 1.0);
  ASSERT_EQ(chroma_error.differential.gain.size(), 2U);
  EXPECT_DOUBLE_EQ(chroma_error.differential.gain[1].level_ire, 100.0);
  EXPECT_DOUBLE_EQ(chroma_error.differential.phase_deg[1].value, 2.5);
  EXPECT_TRUE(chroma_error.crosstalk.enabled);
  EXPECT_DOUBLE_EQ(chroma_error.crosstalk.v_to_u, 0.3);
  EXPECT_DOUBLE_EQ(chroma_error.crosstalk.u_to_v, 0.0);
  EXPECT_TRUE(chroma_error.crosstalk.alternate_with_switch);
}

}  // namespace
}  // namespace videosynth
