/*
 * File:        test_audio_container_option.cpp
 * Module:      audio_container_tests
 * Purpose:     Validates the parsing, emission and validation of
 *              output.audio_container (auto / forced RF64 channel pair files)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "videosynth/project_validator.h"
#include "videosynth/yaml_project_emitter.h"
#include "videosynth/yaml_project_parser.h"

namespace videosynth {
namespace {

bool AnyContains(const std::vector<std::string>& messages,
                 const std::string& text) {
  for (const std::string& message : messages) {
    if (message.find(text) != std::string::npos) {
      return true;
    }
  }
  return false;
}

std::string ProjectYaml(const std::string& output_extra) {
  return "cvbs_presets:\n"
         "  video_standard_preset: PAL\n"
         "output:\n"
         "  video_path: out.cvbs\n" +
         output_extra +
         "sections:\n"
         "  - name: S\n"
         "    type: progressive\n"
         "    source: f.exr\n"
         "    duration_frames: 1\n";
}

TEST(AudioContainerParserTest, DefaultsToAuto) {
  const ParseResult result = YamlProjectParser().ParseString(ProjectYaml(""));
  ASSERT_TRUE(result.ok) << result.errors[0];
  EXPECT_EQ(result.project.output.audio_container, AudioContainerMode::kAuto);
}

TEST(AudioContainerParserTest, ReadsAutoAndRf64) {
  YamlProjectParser parser;
  const ParseResult automatic =
      parser.ParseString(ProjectYaml("  audio_container: auto\n"));
  ASSERT_TRUE(automatic.ok) << automatic.errors[0];
  EXPECT_EQ(automatic.project.output.audio_container,
            AudioContainerMode::kAuto);
  const ParseResult forced =
      parser.ParseString(ProjectYaml("  audio_container: rf64\n"));
  ASSERT_TRUE(forced.ok) << forced.errors[0];
  EXPECT_EQ(forced.project.output.audio_container, AudioContainerMode::kRf64);
}

TEST(AudioContainerParserTest, RejectsUnknownValues) {
  // RIFF cannot describe audio past its size limit, so it is not offered.
  for (const char* value : {"riff", "RF64", "wav"}) {
    const ParseResult result = YamlProjectParser().ParseString(
        ProjectYaml(std::string("  audio_container: ") + value + "\n"));
    ASSERT_FALSE(result.ok) << value;
    EXPECT_NE(result.errors[0].find("output.audio_container"),
              std::string::npos)
        << result.errors[0];
    EXPECT_NE(result.errors[0].find("auto, rf64"), std::string::npos);
  }
}

TEST(AudioContainerEmitterTest, WritesRf64AndOmitsTheAutoDefault) {
  YamlProjectParser parser;
  YamlProjectEmitter emitter;
  const ParseResult forced =
      parser.ParseString(ProjectYaml("  audio_container: rf64\n"));
  ASSERT_TRUE(forced.ok);
  const std::string emitted = emitter.EmitString(forced.project);
  EXPECT_NE(emitted.find("audio_container: rf64"), std::string::npos)
      << emitted;
  const ParseResult reparsed = parser.ParseString(emitted);
  ASSERT_TRUE(reparsed.ok) << emitted;
  EXPECT_EQ(reparsed.project, forced.project);

  const ParseResult automatic =
      parser.ParseString(ProjectYaml("  audio_container: auto\n"));
  ASSERT_TRUE(automatic.ok);
  EXPECT_EQ(emitter.EmitString(automatic.project).find("audio_container"),
            std::string::npos);
}

TEST(AudioContainerEmitterTest, ModeTakesPartInProjectEquality) {
  OutputTargets a;
  OutputTargets b;
  EXPECT_EQ(a, b);
  b.audio_container = AudioContainerMode::kRf64;
  EXPECT_FALSE(a == b);
}

Project ValidProject() {
  Project project;
  project.cvbs_presets.video_standard_preset = Standard::kPal;
  project.cvbs_presets.sample_encoding_preset = "CVBS_U10_4FSC";
  project.cvbs_presets.signal_state_preset =
      GeneratedSignalStatePreset(Standard::kPal);
  project.output.video_path = "out.cvbs";
  Section section;
  section.name = "Card";
  section.type = "progressive";
  section.source = "card.exr";
  section.duration_frames = 4;
  project.sections.push_back(section);
  return project;
}

TEST(AudioContainerValidationTest, WarnsWhenRf64IsSetWithoutAudio) {
  Project project = ValidProject();
  project.output.audio_container = AudioContainerMode::kRf64;
  const ValidationResult result = ProjectValidator().Validate(project);
  EXPECT_TRUE(result.is_valid)
      << (result.errors.empty() ? "" : result.errors[0]);
  EXPECT_TRUE(AnyContains(result.warnings, "output.audio_container"));
}

TEST(AudioContainerValidationTest, QuietWhenAutoOrWhenAudioIsDeclared) {
  Project automatic = ValidProject();
  EXPECT_FALSE(AnyContains(ProjectValidator().Validate(automatic).warnings,
                           "output.audio_container"));

  Project with_audio = ValidProject();
  with_audio.output.audio_container = AudioContainerMode::kRf64;
  AudioChannelPair pair;
  pair.pair = 0;
  pair.pair_specified = true;
  pair.left.enabled = true;
  pair.left.frequency_hz = 1000.0;
  pair.left.amplitude = 0.5;
  with_audio.sections[0].audio_channel_pairs = {pair};
  EXPECT_FALSE(AnyContains(ProjectValidator().Validate(with_audio).warnings,
                           "output.audio_container"));
}

}  // namespace
}  // namespace videosynth
