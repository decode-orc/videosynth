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
      "  pal_laserdisc_pilot_burst: true\n"
      "  ntsc_laserdisc_vbi_burst: false\n"
      "output:\n"
      "  video_path: out.cvbs\n"
      "line_injections:\n"
      "  disc_type: CAV\n"
      "  vits:\n"
      "    - vits_type: virs\n"
      "      target_lines: [19, 20]\n"
      "sections:\n"
      "  - name: ProgressiveWithInjections\n"
      "    type: progressive\n"
      "    source: fixture.exr\n"
      "    duration_frames: 4\n"
      "    line_injections:\n"
      "      - type: laserdisc\n"
      "        codes:\n"
      "          - code_type: picture_number\n"
      "            start_value: 1\n";

  YamlProjectParser parser;
  const ParseResult result = parser.ParseString(yaml);

  ASSERT_TRUE(result.ok);
  ASSERT_EQ(result.project.sections.size(), 1U);
  EXPECT_TRUE(result.project.cvbs_presets.pal_laserdisc_pilot_burst);
  EXPECT_FALSE(result.project.cvbs_presets.ntsc_laserdisc_vbi_burst);

  // Project-wide line injections: disc_type + VITS set.
  EXPECT_EQ(result.project.line_injections.disc_type, "CAV");
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
      "    section_type: lead_in\n";

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
      "    section_type: programme_area\n";

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
      "    section_type: lead_out\n";

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
      "    section_type: programme\n";

  YamlProjectParser parser;
  const ParseResult result = parser.ParseString(yaml);

  EXPECT_FALSE(result.ok);
  ASSERT_FALSE(result.errors.empty());
  EXPECT_NE(result.errors[0].find("section_type"), std::string::npos);
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

// Minimal project text with an `output` block whose body is supplied by the
// caller, used by the EFM digital audio output cases below.
std::string MakeProjectWithOutputBlock(const std::string& output_body) {
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
         output_body +
         "sections:\n"
         "  - name: Programme\n"
         "    type: progressive\n"
         "    source: fixture.exr\n"
         "    duration_frames: 10\n";
}

TEST(YamlProjectParserTest, ParsesEfmAudioOutputPair) {
  const std::string yaml = MakeProjectWithOutputBlock(
      "  efm_audio:\n"
      "    pair: 3\n");

  YamlProjectParser parser;
  const ParseResult result = parser.ParseString(yaml);

  ASSERT_TRUE(result.ok);
  EXPECT_TRUE(result.project.output.efm_audio.enabled);
  EXPECT_EQ(result.project.output.efm_audio.pair, 3);
}

TEST(YamlProjectParserTest, EfmAudioDisabledWhenOutputBlockAbsent) {
  YamlProjectParser parser;
  const ParseResult result = parser.ParseString(MakeProjectWithOutputBlock(""));

  ASSERT_TRUE(result.ok);
  EXPECT_FALSE(result.project.output.efm_audio.enabled);
  EXPECT_EQ(result.project.output.efm_audio.pair, 0);
}

TEST(YamlProjectParserTest, RejectsEfmAudioWithoutPair) {
  const std::string yaml = MakeProjectWithOutputBlock(
      "  efm_audio:\n"
      "    {}\n");

  YamlProjectParser parser;
  const ParseResult result = parser.ParseString(yaml);

  EXPECT_FALSE(result.ok);
  ASSERT_FALSE(result.errors.empty());
}

TEST(YamlProjectParserTest, RejectsUnsupportedEfmAudioField) {
  const std::string yaml = MakeProjectWithOutputBlock(
      "  efm_audio:\n"
      "    pair: 0\n"
      "    channel: left\n");

  YamlProjectParser parser;
  const ParseResult result = parser.ParseString(yaml);

  EXPECT_FALSE(result.ok);
  ASSERT_FALSE(result.errors.empty());
}

TEST(YamlProjectParserTest, RejectsNonIntegerEfmAudioPair) {
  const std::string yaml = MakeProjectWithOutputBlock(
      "  efm_audio:\n"
      "    pair: first\n");

  YamlProjectParser parser;
  const ParseResult result = parser.ParseString(yaml);

  EXPECT_FALSE(result.ok);
  ASSERT_FALSE(result.errors.empty());
}

}  // namespace
}  // namespace videosynth
