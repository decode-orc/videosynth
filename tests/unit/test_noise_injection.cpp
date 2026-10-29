/*
 * File:        test_noise_injection.cpp
 * Module:      noise_injection_tests
 * Purpose:     Validates YAML parsing, project validation, and statistical
 *              correctness of NoiseInjectionStage.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <numeric>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "videosynth/fixed_point.h"
#include "videosynth/frame_enrichment.h"
#include "videosynth/interfaces.h"
#include "videosynth/model.h"
#include "videosynth/noise_injection_stage.h"
#include "videosynth/noise_spectrum_filter.h"
#include "videosynth/project_validator.h"
#include "videosynth/sample_clamp.h"
#include "videosynth/timing_constants.h"
#include "videosynth/yaml_project_parser.h"

namespace videosynth {
namespace {

// ---------------------------------------------------------------------------
// Shared YAML helpers
// ---------------------------------------------------------------------------

const std::string kMinimalYamlPrefix =
    "project:\n"
    "  name: NoiseTest\n"
    "  version: \"1.0\"\n"
    "cvbs_presets:\n"
    "  video_standard_preset: PAL\n"
    "  sample_encoding_preset: CVBS_U10_4FSC\n"
    "  signal_state_preset: STANDARD_STABLE_LOCKED\n"
    "output:\n"
    "  video_path: out.cvbs\n";

// ---------------------------------------------------------------------------
// Parser tests
// ---------------------------------------------------------------------------

TEST(NoiseInjectionTest, ParserNoNoiseSectionHasDisabledNoise) {
  const std::string yaml = kMinimalYamlPrefix +
                           "sections:\n"
                           "  - name: S\n"
                           "    type: progressive\n"
                           "    source: f.exr\n"
                           "    duration_frames: 8\n";

  YamlProjectParser parser;
  const ParseResult result = parser.ParseString(yaml);

  ASSERT_TRUE(result.ok);
  ASSERT_EQ(result.project.sections.size(), 1U);
  EXPECT_FALSE(result.project.sections[0].noise.enabled);
}

TEST(NoiseInjectionTest, ParserNoiseDbOnlyEnablesNoise) {
  const std::string yaml = kMinimalYamlPrefix +
                           "sections:\n"
                           "  - name: S\n"
                           "    type: progressive\n"
                           "    source: f.exr\n"
                           "    duration_frames: 8\n"
                           "    noise:\n"
                           "      noise_db: 48.0\n";

  YamlProjectParser parser;
  const ParseResult result = parser.ParseString(yaml);

  ASSERT_TRUE(result.ok);
  ASSERT_EQ(result.project.sections.size(), 1U);
  const NoiseParameters& noise = result.project.sections[0].noise;
  EXPECT_TRUE(noise.enabled);
  EXPECT_DOUBLE_EQ(noise.noise_db, 48.0);
  EXPECT_DOUBLE_EQ(noise.noise_spread_db, 0.0);
}

TEST(NoiseInjectionTest, ParserNoiseDbAndSpreadParsedCorrectly) {
  const std::string yaml = kMinimalYamlPrefix +
                           "sections:\n"
                           "  - name: S\n"
                           "    type: progressive\n"
                           "    source: f.exr\n"
                           "    duration_frames: 8\n"
                           "    noise:\n"
                           "      noise_db: 52.0\n"
                           "      noise_spread_db: 4.0\n";

  YamlProjectParser parser;
  const ParseResult result = parser.ParseString(yaml);

  ASSERT_TRUE(result.ok);
  ASSERT_EQ(result.project.sections.size(), 1U);
  const NoiseParameters& noise = result.project.sections[0].noise;
  EXPECT_TRUE(noise.enabled);
  EXPECT_DOUBLE_EQ(noise.noise_db, 52.0);
  EXPECT_DOUBLE_EQ(noise.noise_spread_db, 4.0);
}

TEST(NoiseInjectionTest, ParserRejectsSpreadWithoutNoiseDb) {
  const std::string yaml = kMinimalYamlPrefix +
                           "sections:\n"
                           "  - name: S\n"
                           "    type: progressive\n"
                           "    source: f.exr\n"
                           "    duration_frames: 8\n"
                           "    noise:\n"
                           "      noise_spread_db: 4.0\n";

  YamlProjectParser parser;
  const ParseResult result = parser.ParseString(yaml);

  EXPECT_FALSE(result.ok);
  ASSERT_FALSE(result.errors.empty());
  EXPECT_NE(result.errors[0].find("noise_spread_db"), std::string::npos);
}

TEST(NoiseInjectionTest, ParserNoiseSeedParsedCorrectly) {
  const std::string yaml = kMinimalYamlPrefix +
                           "sections:\n"
                           "  - name: S\n"
                           "    type: progressive\n"
                           "    source: f.exr\n"
                           "    duration_frames: 8\n"
                           "    noise:\n"
                           "      noise_db: 48.0\n"
                           "      noise_seed: 12345\n";

  YamlProjectParser parser;
  const ParseResult result = parser.ParseString(yaml);

  ASSERT_TRUE(result.ok);
  ASSERT_EQ(result.project.sections.size(), 1U);
  const NoiseParameters& noise = result.project.sections[0].noise;
  EXPECT_TRUE(noise.noise_seed_specified);
  EXPECT_EQ(noise.noise_seed, 12345);
}

TEST(NoiseInjectionTest, ParserRejectsUnknownNoiseKey) {
  const std::string yaml = kMinimalYamlPrefix +
                           "sections:\n"
                           "  - name: S\n"
                           "    type: progressive\n"
                           "    source: f.exr\n"
                           "    duration_frames: 8\n"
                           "    noise:\n"
                           "      noise_db: 48.0\n"
                           "      unknown_key: 1.0\n";

  YamlProjectParser parser;
  const ParseResult result = parser.ParseString(yaml);

  EXPECT_FALSE(result.ok);
}

std::string NoiseSectionYaml(const std::string& noise_block) {
  return kMinimalYamlPrefix +
         "sections:\n"
         "  - name: S\n"
         "    type: progressive\n"
         "    source: f.exr\n"
         "    duration_frames: 8\n"
         "    noise:\n" +
         noise_block;
}

TEST(NoiseInjectionTest, ParserReadsCaptureSpectrum) {
  const ParseResult result = YamlProjectParser().ParseString(
      NoiseSectionYaml("      noise_db: 46.0\n"
                       "      spectrum:\n"
                       "        - [0.0, 0.0]\n"
                       "        - [3.5, 4.0]\n"
                       "        - [6.0, 10.0]\n"));
  ASSERT_TRUE(result.ok) << result.errors.front();
  const NoiseParameters& noise = result.project.sections[0].noise;
  ASSERT_EQ(noise.spectrum.size(), 3U);
  EXPECT_DOUBLE_EQ(noise.spectrum[1].frequency_mhz, 3.5);
  EXPECT_DOUBLE_EQ(noise.spectrum[1].relative_db, 4.0);
  EXPECT_FALSE(noise.shared.enabled);
}

TEST(NoiseInjectionTest, ParserReadsSharedLayer) {
  const ParseResult result = YamlProjectParser().ParseString(
      NoiseSectionYaml("      noise_db: 46.0\n"
                       "      shared:\n"
                       "        noise_db: 44.0\n"
                       "        noise_seed: 77\n"
                       "        spectrum: [[0.0, 0.0], [5.0, 6.0]]\n"));
  ASSERT_TRUE(result.ok) << result.errors.front();
  const SharedNoiseParameters& shared = result.project.sections[0].noise.shared;
  EXPECT_TRUE(shared.enabled);
  EXPECT_DOUBLE_EQ(shared.noise_db, 44.0);
  EXPECT_TRUE(shared.noise_seed_specified);
  EXPECT_EQ(shared.noise_seed, 77);
  ASSERT_EQ(shared.spectrum.size(), 2U);
}

TEST(NoiseInjectionTest, ParserSharedOnlyBlockHasNoCaptureLayer) {
  const ParseResult result = YamlProjectParser().ParseString(
      NoiseSectionYaml("      shared:\n"
                       "        noise_db: 44.0\n"
                       "        noise_seed: 77\n"));
  ASSERT_TRUE(result.ok) << result.errors.front();
  const NoiseParameters& noise = result.project.sections[0].noise;
  EXPECT_FALSE(noise.enabled);
  EXPECT_TRUE(noise.shared.enabled);
}

TEST(NoiseInjectionTest, ParserRejectsSpectrumWithoutNoiseDb) {
  const ParseResult result = YamlProjectParser().ParseString(
      NoiseSectionYaml("      spectrum: [[0.0, 0.0]]\n"));
  EXPECT_FALSE(result.ok);
  ASSERT_FALSE(result.errors.empty());
  EXPECT_NE(result.errors[0].find("spectrum requires noise_db"),
            std::string::npos);
}

TEST(NoiseInjectionTest, ParserRejectsMalformedSpectrumEntry) {
  const ParseResult result = YamlProjectParser().ParseString(
      NoiseSectionYaml("      noise_db: 46.0\n"
                       "      spectrum: [[0.0, 0.0, 1.0]]\n"));
  EXPECT_FALSE(result.ok);
  ASSERT_FALSE(result.errors.empty());
  EXPECT_NE(result.errors[0].find("[frequency_mhz, relative_db]"),
            std::string::npos);
}

TEST(NoiseInjectionTest, ParserRejectsSharedNoiseSpread) {
  const ParseResult result = YamlProjectParser().ParseString(
      NoiseSectionYaml("      shared:\n"
                       "        noise_db: 44.0\n"
                       "        noise_spread_db: 2.0\n"
                       "        noise_seed: 77\n"));
  EXPECT_FALSE(result.ok);
  ASSERT_FALSE(result.errors.empty());
  EXPECT_NE(result.errors[0].find("does not take noise_spread_db"),
            std::string::npos);
}

TEST(NoiseInjectionTest, ParserRejectsSharedWithoutNoiseDb) {
  const ParseResult result = YamlProjectParser().ParseString(
      NoiseSectionYaml("      shared:\n"
                       "        noise_seed: 77\n"));
  EXPECT_FALSE(result.ok);
  ASSERT_FALSE(result.errors.empty());
  EXPECT_NE(result.errors[0].find("noise.shared requires noise_db"),
            std::string::npos);
}

// ---------------------------------------------------------------------------
// Validator tests (noise-specific rules)
// ---------------------------------------------------------------------------

// Probe is not needed for noise validation tests — pass nullptr to skip the
// progressive source profile checks, matching the pattern used in
// test_project_validator.cpp for configuration-only assertions.
Project MakeValidNoiseProject(double noise_db, double noise_spread_db) {
  Project project;
  project.cvbs_presets.video_standard_preset = Standard::kPal;
  project.cvbs_presets.sample_encoding_preset = "CVBS_U10_4FSC";
  project.cvbs_presets.signal_state_preset = "STANDARD_STABLE_LOCKED";
  project.output.video_path = "/tmp/noise_test.cvbs";
  project.output.metadata_path = "/tmp/noise_test.meta";

  Section section;
  section.name = "Noisy";
  section.type = "progressive";
  section.source = "f.exr";
  section.duration_frames = 8;
  section.noise.enabled = true;
  section.noise.noise_db = noise_db;
  section.noise.noise_spread_db = noise_spread_db;

  project.sections.push_back(section);
  return project;
}

TEST(NoiseInjectionTest, ValidatorAcceptsValidNoiseDb) {
  ProjectValidator validator;
  const ValidationResult result =
      validator.Validate(MakeValidNoiseProject(48.0, 0.0));
  EXPECT_TRUE(result.is_valid);
}

TEST(NoiseInjectionTest, ValidatorAcceptsValidNoiseDbAndSpread) {
  ProjectValidator validator;
  const ValidationResult result =
      validator.Validate(MakeValidNoiseProject(48.0, 4.0));
  EXPECT_TRUE(result.is_valid);
}

TEST(NoiseInjectionTest, ValidatorRejectsNoiseDbTooLow) {
  ProjectValidator validator;
  const ValidationResult result =
      validator.Validate(MakeValidNoiseProject(19.9, 0.0));
  EXPECT_FALSE(result.is_valid);
  ASSERT_FALSE(result.errors.empty());
  EXPECT_NE(result.errors[0].find("noise_db"), std::string::npos);
}

TEST(NoiseInjectionTest, ValidatorRejectsNoiseDbTooHigh) {
  ProjectValidator validator;
  const ValidationResult result =
      validator.Validate(MakeValidNoiseProject(61.1, 0.0));
  EXPECT_FALSE(result.is_valid);
}

TEST(NoiseInjectionTest, ValidatorRejectsNegativeNoiseSpread) {
  ProjectValidator validator;
  const ValidationResult result =
      validator.Validate(MakeValidNoiseProject(48.0, -1.0));
  EXPECT_FALSE(result.is_valid);
  ASSERT_FALSE(result.errors.empty());
  EXPECT_NE(result.errors[0].find("noise_spread_db"), std::string::npos);
}

TEST(NoiseInjectionTest, ValidatorRejectsSpreadDrivingWhiteSnrBelowFloor) {
  ProjectValidator validator;
  // noise_db=30.0, spread=11.0 → white_snr=19.0 < 20.0
  const ValidationResult result =
      validator.Validate(MakeValidNoiseProject(30.0, 11.0));
  EXPECT_FALSE(result.is_valid);
  ASSERT_FALSE(result.errors.empty());
  EXPECT_NE(result.errors[0].find("White SNR floor"), std::string::npos);
}

TEST(NoiseInjectionTest, ValidatorWarnsWhenNoVitsForWhiteSnrCheck) {
  ProjectValidator validator;
  // Valid noise_spread_db > 0 but no VITS on line 19 → warning
  const ValidationResult result =
      validator.Validate(MakeValidNoiseProject(48.0, 4.0));
  EXPECT_TRUE(result.is_valid);
  ASSERT_FALSE(result.warnings.empty());
  EXPECT_NE(result.warnings[0].find("White SNR"), std::string::npos);
}

TEST(NoiseInjectionTest, ValidatorNoWarnWhenVitsOnWhiteSnrLine) {
  ProjectValidator validator;
  Project project = MakeValidNoiseProject(48.0, 4.0);
  // Add a project-wide VITS injection on PAL line 19 to suppress the warning.
  project.line_injections.vits.push_back(VitsInjection{"uk-national", {19}});

  const ValidationResult result = validator.Validate(project);
  EXPECT_TRUE(result.is_valid);
  EXPECT_TRUE(result.warnings.empty());
}

TEST(NoiseInjectionTest, ValidatorNoWarnWhenSpreadIsZero) {
  ProjectValidator validator;
  // spread=0 → white SNR check not applicable, no warning expected
  const ValidationResult result =
      validator.Validate(MakeValidNoiseProject(48.0, 0.0));
  EXPECT_TRUE(result.is_valid);
  EXPECT_TRUE(result.warnings.empty());
}

Project MakeSharedNoiseProject(double shared_db) {
  Project project = MakeValidNoiseProject(48.0, 0.0);
  SharedNoiseParameters& shared = project.sections[0].noise.shared;
  shared.enabled = true;
  shared.noise_db = shared_db;
  shared.noise_seed = 77;
  shared.noise_seed_specified = true;
  return project;
}

std::string FirstError(const ValidationResult& result) {
  return result.errors.empty() ? std::string() : result.errors.front();
}

TEST(NoiseInjectionTest, ValidatorAcceptsSpectrumAndSharedLayer) {
  Project project = MakeSharedNoiseProject(44.0);
  project.sections[0].noise.spectrum = {{0.0, 0.0}, {3.5, 4.0}, {6.0, 10.0}};
  project.sections[0].noise.shared.spectrum = {{0.0, -60.0}, {8.8, 40.0}};
  const ValidationResult result = ProjectValidator().Validate(project);
  EXPECT_TRUE(result.is_valid) << FirstError(result);
  EXPECT_TRUE(result.warnings.empty());
}

TEST(NoiseInjectionTest, ValidatorAcceptsSharedOnlyLayer) {
  Project project = MakeSharedNoiseProject(44.0);
  project.sections[0].noise.enabled = false;
  const ValidationResult result = ProjectValidator().Validate(project);
  EXPECT_TRUE(result.is_valid) << FirstError(result);
}

TEST(NoiseInjectionTest, ValidatorRejectsSpectrumOutOfRangeValues) {
  struct Case {
    std::vector<NoiseSpectrumPoint> spectrum;
    std::string expected;
  };
  const std::vector<Case> cases = {
      {{{-0.1, 0.0}}, "frequency must be in the range"},
      // PAL 4fsc Nyquist is 2 x 4.43361875 MHz = 8.8672375 MHz.
      {{{8.9, 0.0}}, "frequency must be in the range"},
      {{{1.0, 0.0}, {1.0, 3.0}}, "strictly ascending"},
      {{{2.0, 0.0}, {1.0, 3.0}}, "strictly ascending"},
      {{{1.0, -60.1}}, "relative_db must be in the range"},
      {{{1.0, 40.1}}, "relative_db must be in the range"},
  };
  for (const Case& test_case : cases) {
    Project capture = MakeValidNoiseProject(48.0, 0.0);
    capture.sections[0].noise.spectrum = test_case.spectrum;
    const ValidationResult capture_result =
        ProjectValidator().Validate(capture);
    EXPECT_FALSE(capture_result.is_valid);
    EXPECT_NE(FirstError(capture_result).find("': spectrum"), std::string::npos)
        << FirstError(capture_result);
    EXPECT_NE(FirstError(capture_result).find(test_case.expected),
              std::string::npos)
        << FirstError(capture_result);

    Project shared = MakeSharedNoiseProject(44.0);
    shared.sections[0].noise.shared.spectrum = test_case.spectrum;
    const ValidationResult shared_result = ProjectValidator().Validate(shared);
    EXPECT_FALSE(shared_result.is_valid);
    EXPECT_NE(FirstError(shared_result).find("shared.spectrum"),
              std::string::npos)
        << FirstError(shared_result);
    EXPECT_NE(FirstError(shared_result).find(test_case.expected),
              std::string::npos)
        << FirstError(shared_result);
  }
}

TEST(NoiseInjectionTest, ValidatorAcceptsSpectrumUpToNyquist) {
  Project project = MakeValidNoiseProject(48.0, 0.0);
  project.sections[0].noise.spectrum = {{0.0, 0.0}, {8.867, 6.0}};
  const ValidationResult result = ProjectValidator().Validate(project);
  EXPECT_TRUE(result.is_valid) << FirstError(result);
}

TEST(NoiseInjectionTest, ValidatorBoundsSpectrumBy4fscNyquistForRawPresets) {
  // Noise is shaped on the 4fsc signal before the output stage resamples, so
  // a 28 MHz encoding (Nyquist 14 MHz) still allows only up to 8.867 MHz.
  for (const char* preset : {"RAW_S16_28M", "RAW_S16_40M"}) {
    Project project = MakeValidNoiseProject(48.0, 0.0);
    project.cvbs_presets.sample_encoding_preset = preset;
    project.sections[0].noise.spectrum = {{0.0, 0.0}, {8.867, 6.0}};
    ValidationResult result = ProjectValidator().Validate(project);
    for (const std::string& error : result.errors) {
      EXPECT_EQ(error.find("spectrum"), std::string::npos) << error;
    }

    project.sections[0].noise.spectrum = {{0.0, 0.0}, {10.0, 6.0}};
    result = ProjectValidator().Validate(project);
    EXPECT_FALSE(result.is_valid);
    bool found = false;
    for (const std::string& error : result.errors) {
      found =
          found || error.find("frequency must be in the range [0.0, 8.867") !=
                       std::string::npos;
    }
    EXPECT_TRUE(found) << preset << ": " << FirstError(result);
  }
}

TEST(NoiseInjectionTest, ValidatorRejectsSharedNoiseDbOutOfRange) {
  for (double noise_db : {19.9, 61.1}) {
    const ValidationResult result =
        ProjectValidator().Validate(MakeSharedNoiseProject(noise_db));
    EXPECT_FALSE(result.is_valid);
    EXPECT_NE(FirstError(result).find("shared.noise_db must be in the range"),
              std::string::npos)
        << FirstError(result);
  }
}

TEST(NoiseInjectionTest, ValidatorRequiresSharedNoiseSeed) {
  Project project = MakeSharedNoiseProject(44.0);
  project.sections[0].noise.shared.noise_seed_specified = false;
  const ValidationResult result = ProjectValidator().Validate(project);
  EXPECT_FALSE(result.is_valid);
  EXPECT_NE(FirstError(result).find("shared.noise_seed is required"),
            std::string::npos)
      << FirstError(result);
}

// A CAV lead-in section: valid laserdisc structure, but no picture numbers.
Project MakeCavSharedNoiseProject(bool with_picture_number) {
  Project project = MakeSharedNoiseProject(44.0);
  project.line_injections.disc_type = "CAV";
  Section& section = project.sections[0];
  Section::LineInjection laserdisc;
  laserdisc.type = "laserdisc";
  Section::LineInjectionCode code;
  if (with_picture_number) {
    section.section_type = SectionType::kProgrammeArea;
    code.code_type = "picture_number";
    code.start_value = 1;
    code.start_value_specified = true;
  } else {
    section.section_type = SectionType::kLeadIn;
    code.code_type = "lead_in";
  }
  laserdisc.codes.push_back(code);
  section.line_injections.push_back(laserdisc);
  return project;
}

TEST(NoiseInjectionTest, ValidatorWarnsSharedNoiseOnCavWithoutPictureNumber) {
  const ValidationResult result =
      ProjectValidator().Validate(MakeCavSharedNoiseProject(false));
  EXPECT_TRUE(result.is_valid) << FirstError(result);
  const bool warned = std::any_of(
      result.warnings.begin(), result.warnings.end(),
      [](const std::string& warning) {
        return warning.find("pressing noise falls back to output position") !=
               std::string::npos;
      });
  EXPECT_TRUE(warned);
}

TEST(NoiseInjectionTest, ValidatorNoSharedNoiseWarningWithPictureNumber) {
  const ValidationResult result =
      ProjectValidator().Validate(MakeCavSharedNoiseProject(true));
  EXPECT_TRUE(result.is_valid) << FirstError(result);
  for (const std::string& warning : result.warnings) {
    EXPECT_EQ(warning.find("pressing noise"), std::string::npos) << warning;
  }
}

// ---------------------------------------------------------------------------
// NoiseInjectionStage unit tests
// ---------------------------------------------------------------------------

class NullLogger final : public ILogger {
 public:
  void Info(const std::string&) override {}
  void Warning(const std::string&) override {}
  void Error(const std::string&) override {}
  void Debug(const std::string&) override {}
  void Trace(const std::string&) override {}
};

// Constructs a minimal project and a one-frame schedule for the given section.
struct TestContext {
  Project project;
  std::vector<IGenerationStage::FrameScheduleItem> schedule;
};

TestContext MakeTestContext(Standard standard, bool noise_enabled,
                            double noise_db, double noise_spread_db) {
  TestContext ctx;
  ctx.project.cvbs_presets.video_standard_preset = standard;
  ctx.project.cvbs_presets.sample_encoding_preset = "CVBS_U10_4FSC";
  ctx.project.cvbs_presets.signal_state_preset = "STANDARD_STABLE_LOCKED";
  ctx.project.output.video_path = "/tmp/noise_stage_test.cvbs";
  ctx.project.output.metadata_path = "/tmp/noise_stage_test.meta";

  Section section;
  section.name = "Test";
  section.type = "progressive";
  section.source = "f.exr";
  section.duration_frames = 1;
  section.noise.enabled = noise_enabled;
  section.noise.noise_db = noise_db;
  section.noise.noise_spread_db = noise_spread_db;
  ctx.project.sections.push_back(section);

  IGenerationStage::FrameScheduleItem item;
  item.section = &ctx.project.sections[0];
  item.source_frame_index = 0;
  ctx.schedule.push_back(item);

  return ctx;
}

TEST(NoiseInjectionTest, RawEncodingPresetsUse4fscFramesAndSpectrum) {
  // Generation always hands the stage 4fsc frames; the output stage resamples
  // RAW encodings later. A RAW preset must therefore produce exactly the noise
  // of a 4fsc preset, inside the 4fsc buffer, on every frame of a batch.
  constexpr std::size_t kFrames = 3;
  for (Standard standard : {Standard::kPal, Standard::kNtsc}) {
    auto Run = [&](const std::string& preset) {
      auto ctx = MakeTestContext(standard, true, 30.0, 4.0);
      ctx.project.cvbs_presets.sample_encoding_preset = preset;
      ctx.project.sections[0].noise.noise_seed = 99;
      ctx.project.sections[0].noise.noise_seed_specified = true;
      ctx.project.sections[0].noise.spectrum = {{0.0, 0.0}, {5.0, 8.0}};
      ctx.schedule.assign(kFrames, ctx.schedule[0]);
      for (IGenerationStage::FrameScheduleItem& item : ctx.schedule) {
        item.section = &ctx.project.sections[0];
      }
      const std::size_t n =
          static_cast<std::size_t>(SamplesPerFrame4fsc(standard)) * kFrames;
      std::vector<SampleFixed> y(n, MillivoltsToSampleFixed(300.0));
      std::vector<SampleFixed> c(n, 0);
      NullLogger logger;
      NoiseInjectionStage(&logger).InjectNoise(ctx.project, ctx.schedule, 0,
                                               kFrames, &y, &c);
      return std::pair{y, c};
    };

    const auto reference = Run("CVBS_U10_4FSC");
    const std::size_t spf =
        static_cast<std::size_t>(SamplesPerFrame4fsc(standard));
    // Every frame of the batch, including the last sample, carries noise.
    for (std::size_t f = 0; f < kFrames; ++f) {
      EXPECT_NE(reference.second[(f * spf) + spf - 1U], 0) << f;
    }
    for (const char* preset : {"RAW_S16_28M", "RAW_S16_40M"}) {
      const auto raw = Run(preset);
      EXPECT_EQ(raw.first, reference.first)
          << StandardToString(standard) << " " << preset;
      EXPECT_EQ(raw.second, reference.second)
          << StandardToString(standard) << " " << preset;
    }
  }
}

TEST(NoiseInjectionTest, DisabledNoiseDoesNotChangeBuffers) {
  NullLogger logger;
  NoiseInjectionStage stage(&logger);

  auto ctx = MakeTestContext(Standard::kPal, false, 48.0, 0.0);
  const std::size_t n =
      static_cast<std::size_t>(SamplesPerFrame4fsc(Standard::kPal));
  std::vector<SampleFixed> y(n, MillivoltsToSampleFixed(0.0));
  std::vector<SampleFixed> c(n, MillivoltsToSampleFixed(0.0));
  const std::vector<SampleFixed> y_orig = y;
  const std::vector<SampleFixed> c_orig = c;

  stage.InjectNoise(ctx.project, ctx.schedule, 0, 1, &y, &c);

  EXPECT_EQ(y, y_orig);
  EXPECT_EQ(c, c_orig);
}

// Computes the sample standard deviation of a vector of SampleFixed values
// converted to millivolts.
double ComputeStdDevMv(const std::vector<SampleFixed>& samples,
                       double reference_mv) {
  if (samples.empty()) {
    return 0.0;
  }
  double sum_sq = 0.0;
  for (SampleFixed s : samples) {
    const double delta = SampleFixedToMillivolts(s) - reference_mv;
    sum_sq += delta * delta;
  }
  return std::sqrt(sum_sq / static_cast<double>(samples.size()));
}

TEST(NoiseInjectionTest, FloorNoiseStdDevMatchesTargetAtBlanking) {
  NullLogger logger;
  NoiseInjectionStage stage(&logger);

  // PAL, noise_db=40, spread=0 → sigma_f_ire=100/100=1 IRE at 40 dB,
  // sigma_f_mV=1*7.0=7.0 mV
  constexpr double kNoiseDd = 40.0;
  auto ctx = MakeTestContext(Standard::kPal, true, kNoiseDd, 0.0);

  const std::size_t n =
      static_cast<std::size_t>(SamplesPerFrame4fsc(Standard::kPal));
  // Blanking level: 0 mV → proportional component is zero.
  std::vector<SampleFixed> y(n, MillivoltsToSampleFixed(0.0));
  std::vector<SampleFixed> c(n, MillivoltsToSampleFixed(0.0));

  stage.InjectNoise(ctx.project, ctx.schedule, 0, 1, &y, &c);

  // sigma_f_ire = 100 / 10^(40/20) = 100 / 100 = 1 IRE
  // sigma_f_mV  = 1 * (700/100) = 7.0 mV
  const double expected_sigma_mv = 7.0;
  const double measured_y = ComputeStdDevMv(y, 0.0);
  const double measured_c = ComputeStdDevMv(c, 0.0);

  // Accept ±5 % at ~709 k samples.
  EXPECT_NEAR(measured_y, expected_sigma_mv, expected_sigma_mv * 0.05);
  EXPECT_NEAR(measured_c, expected_sigma_mv, expected_sigma_mv * 0.05);
}

TEST(NoiseInjectionTest, YAndCNoiseAreEqualPerSampleAtBlanking) {
  NullLogger logger;
  NoiseInjectionStage stage(&logger);

  auto ctx = MakeTestContext(Standard::kPal, true, 40.0, 0.0);

  // Use a small count — just verify sample-level equality (correlated noise).
  constexpr std::size_t kSamples = 1024;
  std::vector<SampleFixed> y(kSamples, MillivoltsToSampleFixed(0.0));
  std::vector<SampleFixed> c(kSamples, MillivoltsToSampleFixed(0.0));

  // Override schedule so only one "frame" with exactly kSamples is processed.
  // We exploit that InjectNoise uses samples_per_frame from the project preset,
  // so we directly verify the correlated property via a full PAL frame instead.
  const std::size_t n =
      static_cast<std::size_t>(SamplesPerFrame4fsc(Standard::kPal));
  std::vector<SampleFixed> y_full(n, MillivoltsToSampleFixed(0.0));
  std::vector<SampleFixed> c_full(n, MillivoltsToSampleFixed(0.0));

  stage.InjectNoise(ctx.project, ctx.schedule, 0, 1, &y_full, &c_full);

  // Every sample must have the same noise added to Y and C.
  for (std::size_t i = 0; i < n; ++i) {
    ASSERT_EQ(y_full[i], c_full[i]) << "Mismatch at sample " << i;
  }
}

TEST(NoiseInjectionTest, ProportionalNoiseIncreasesNoiseAtWhiteLevel) {
  NullLogger logger;
  NoiseInjectionStage stage(&logger);

  // PAL, noise_db=48, spread=4 → white_snr_target=44
  // sigma_f_mV = 100/10^(48/20) * 7.0 = 100/251.2 * 7.0 ≈ 2.786 mV
  // sigma_w_ire = 100/10^(44/20) = 100/158.5 ≈ 0.631 IRE
  // sigma_w_mV = 0.631 * 7.0 ≈ 4.416 mV
  auto ctx = MakeTestContext(Standard::kPal, true, 48.0, 4.0);

  const std::size_t n =
      static_cast<std::size_t>(SamplesPerFrame4fsc(Standard::kPal));
  // Set all Y samples to white level (700 mV) to drive proportional component.
  std::vector<SampleFixed> y(n, MillivoltsToSampleFixed(700.0));
  std::vector<SampleFixed> c(n, MillivoltsToSampleFixed(0.0));

  stage.InjectNoise(ctx.project, ctx.schedule, 0, 1, &y, &c);

  // Expected sigma_total at white: sqrt(sigma_f^2 + (k*700)^2) = sigma_w_mV
  const double sigma_w_mv =
      (100.0 / std::pow(10.0, 44.0 / 20.0)) * (700.0 / 100.0);
  // After noise injection the Y distribution has mean ≈ 700 mV.
  const double measured_y = ComputeStdDevMv(y, 700.0);
  EXPECT_NEAR(measured_y, sigma_w_mv, sigma_w_mv * 0.05);
}

TEST(NoiseInjectionTest, OutputIsDeterministicForSameFrameAndSection) {
  // Within a single run, the same (frame, section) always produces the same
  // noise regardless of how many times InjectNoise is called.
  NullLogger logger;
  NoiseInjectionStage stage(&logger);

  auto ctx = MakeTestContext(Standard::kPal, true, 48.0, 4.0);

  const std::size_t n =
      static_cast<std::size_t>(SamplesPerFrame4fsc(Standard::kPal));
  std::vector<SampleFixed> y1(n, MillivoltsToSampleFixed(350.0));
  std::vector<SampleFixed> c1(n, MillivoltsToSampleFixed(0.0));
  std::vector<SampleFixed> y2 = y1;
  std::vector<SampleFixed> c2 = c1;

  stage.InjectNoise(ctx.project, ctx.schedule, 0, 1, &y1, &c1);
  stage.InjectNoise(ctx.project, ctx.schedule, 0, 1, &y2, &c2);

  EXPECT_EQ(y1, y2);
  EXPECT_EQ(c1, c2);
}

TEST(NoiseInjectionTest, FixedSeedProducesSameOutputAcrossInstances) {
  // Two separately constructed stages with the same noise_seed produce
  // identical output — the user-supplied seed overrides the random base seed.
  NullLogger logger;

  auto ctx1 = MakeTestContext(Standard::kPal, true, 48.0, 4.0);
  ctx1.project.sections[0].noise.noise_seed = 99999;
  ctx1.project.sections[0].noise.noise_seed_specified = true;
  ctx1.schedule[0].section = &ctx1.project.sections[0];

  auto ctx2 = MakeTestContext(Standard::kPal, true, 48.0, 4.0);
  ctx2.project.sections[0].noise.noise_seed = 99999;
  ctx2.project.sections[0].noise.noise_seed_specified = true;
  ctx2.schedule[0].section = &ctx2.project.sections[0];

  const std::size_t n =
      static_cast<std::size_t>(SamplesPerFrame4fsc(Standard::kPal));
  std::vector<SampleFixed> y1(n, MillivoltsToSampleFixed(350.0));
  std::vector<SampleFixed> c1(n, MillivoltsToSampleFixed(0.0));
  std::vector<SampleFixed> y2 = y1;
  std::vector<SampleFixed> c2 = c1;

  NoiseInjectionStage stage1(&logger);
  NoiseInjectionStage stage2(&logger);

  stage1.InjectNoise(ctx1.project, ctx1.schedule, 0, 1, &y1, &c1);
  stage2.InjectNoise(ctx2.project, ctx2.schedule, 0, 1, &y2, &c2);

  EXPECT_EQ(y1, y2);
  EXPECT_EQ(c1, c2);
}

TEST(NoiseInjectionTest, RandomSeedProducesDifferentOutputAcrossInstances) {
  // Two separately constructed stages without a fixed noise_seed should
  // produce different output (each captures a distinct random_device seed).
  // The probability of a false failure is 1/2^64 per sample — negligible.
  NullLogger logger;

  auto ctx1 = MakeTestContext(Standard::kPal, true, 48.0, 4.0);
  auto ctx2 = MakeTestContext(Standard::kPal, true, 48.0, 4.0);

  const std::size_t n =
      static_cast<std::size_t>(SamplesPerFrame4fsc(Standard::kPal));
  std::vector<SampleFixed> y1(n, MillivoltsToSampleFixed(350.0));
  std::vector<SampleFixed> c1(n, MillivoltsToSampleFixed(0.0));
  std::vector<SampleFixed> y2 = y1;
  std::vector<SampleFixed> c2 = c1;

  NoiseInjectionStage stage1(&logger);
  NoiseInjectionStage stage2(&logger);

  stage1.InjectNoise(ctx1.project, ctx1.schedule, 0, 1, &y1, &c1);
  stage2.InjectNoise(ctx2.project, ctx2.schedule, 0, 1, &y2, &c2);

  EXPECT_NE(y1, y2);
}

TEST(NoiseInjectionTest, ClampingKeepsLumaInLegalRangeAndLeavesChroma) {
  NullLogger logger;
  NoiseInjectionStage stage(&logger);

  // Use maximum noise (20 dB → sigma ≈ 70 mV) to stress the clamping path.
  auto ctx = MakeTestContext(Standard::kPal, true, 20.0, 0.0);

  const std::size_t n =
      static_cast<std::size_t>(SamplesPerFrame4fsc(Standard::kPal));
  // Y at white stresses the upper clamp. C sits below the composite floor, as
  // saturated chroma does, and must not be clamped to it.
  std::vector<SampleFixed> y(n, MillivoltsToSampleFixed(700.0));
  std::vector<SampleFixed> c(n, MillivoltsToSampleFixed(-440.0));
  const std::vector<SampleFixed> y_in = y;
  const std::vector<SampleFixed> c_in = c;

  stage.InjectNoise(ctx.project, ctx.schedule, 0, 1, &y, &c);

  // PAL legal range: approx -300.0 to +908.5 mV.
  const SampleFixed min_legal = MillivoltsToSampleFixed(-300.1);
  const SampleFixed max_legal = MillivoltsToSampleFixed(908.5);

  bool y_clamped = false;
  for (std::size_t i = 0; i < n; ++i) {
    ASSERT_GE(y[i], min_legal) << "Y sample " << i << " below legal floor";
    ASSERT_LE(y[i], max_legal) << "Y sample " << i << " above legal ceiling";
    // The same noise value lands on Y and C, so where Y was not clamped the
    // C change equals the Y change, and C always carries it unclamped.
    if (y[i] != SampleClampBoundsFor(Standard::kPal).max) {
      ASSERT_EQ(c[i] - c_in[i], y[i] - y_in[i]) << "C sample " << i;
    } else {
      y_clamped = true;
    }
  }
  EXPECT_TRUE(y_clamped);
  EXPECT_LT(*std::min_element(c.begin(), c.end()), min_legal);
}

// ---------------------------------------------------------------------------
// Shaped and shared noise layers
// ---------------------------------------------------------------------------

std::size_t PalSamplesPerFrame() {
  return static_cast<std::size_t>(SamplesPerFrame4fsc(Standard::kPal));
}

// Builds a schedule item whose enrichment carries the given disc position.
IGenerationStage::FrameScheduleItem DiscFrame(const Section* section,
                                              std::size_t disc_frame_index) {
  IGenerationStage::FrameScheduleItem item;
  item.section = section;
  auto enrichment = std::make_shared<FrameEnrichment>();
  enrichment->disc_frame_index = disc_frame_index;
  item.enrichment = enrichment;
  return item;
}

// The capture-layer algorithm as it stood before spectrum and shared layers
// existed, kept here as the golden reference for the unchanged white path.
void LegacyCaptureNoise(const NoiseParameters& noise, std::size_t section_index,
                        std::size_t global_frame, std::vector<SampleFixed>* y,
                        std::vector<SampleFixed>* c) {
  const double white_mv = 700.0;
  const double sigma_f_ire = 100.0 / std::pow(10.0, noise.noise_db / 20.0);
  const double sigma_f_mv = sigma_f_ire * (white_mv / 100.0);
  double k = 0.0;
  if (noise.noise_spread_db > 0.0) {
    const double sigma_w_ire =
        100.0 / std::pow(10.0, (noise.noise_db - noise.noise_spread_db) / 20.0);
    k = std::sqrt((sigma_w_ire * sigma_w_ire) - (sigma_f_ire * sigma_f_ire)) /
        100.0;
  }
  const std::uint64_t seed =
      (static_cast<std::uint64_t>(noise.noise_seed) ^
       (static_cast<std::uint64_t>(section_index) * 2654435761ULL)) ^
      (static_cast<std::uint64_t>(global_frame) * 2246822519ULL);
  std::mt19937_64 rng(seed);
  std::normal_distribution<double> unit_normal(0.0, 1.0);
  const SampleFixed clamp_min = MillivoltsToSampleFixed(-300.006);
  const SampleFixed clamp_max = MillivoltsToSampleFixed(908.452);
  for (std::size_t s = 0; s < y->size(); ++s) {
    double sigma = sigma_f_mv;
    if (k != 0.0) {
      const double prop = k * SampleFixedToMillivolts((*y)[s]);
      sigma = std::sqrt((sigma_f_mv * sigma_f_mv) + (prop * prop));
    }
    const SampleFixed n = MillivoltsToSampleFixed(sigma * unit_normal(rng));
    (*y)[s] = std::clamp((*y)[s] + n, clamp_min, clamp_max);
    (*c)[s] += n;
  }
}

TEST(NoiseInjectionTest, WhiteCaptureNoiseMatchesLegacyImplementation) {
  NullLogger logger;
  NoiseInjectionStage stage(&logger);

  Project project;
  project.cvbs_presets.video_standard_preset = Standard::kPal;
  project.cvbs_presets.sample_encoding_preset = "CVBS_U10_4FSC";
  for (int s = 0; s < 2; ++s) {
    Section section;
    section.name = "S" + std::to_string(s);
    section.noise.enabled = true;
    section.noise.noise_db = 30.0;
    section.noise.noise_spread_db = 6.0;
    section.noise.noise_seed = 4242;
    section.noise.noise_seed_specified = true;
    project.sections.push_back(section);
  }
  // Frame 2 of the schedule sits in section 1, exercising both seed axes.
  std::vector<IGenerationStage::FrameScheduleItem> schedule;
  schedule.push_back(DiscFrame(&project.sections[0], 0));
  schedule.push_back(DiscFrame(&project.sections[1], 1));
  schedule.push_back(DiscFrame(&project.sections[1], 2));

  const std::size_t n = PalSamplesPerFrame();
  std::vector<SampleFixed> y(n);
  for (std::size_t s = 0; s < n; ++s) {
    // A ramp past both clamp limits, so level dependence and clamping are
    // both covered.
    y[s] = MillivoltsToSampleFixed(
        -320.0 + (1250.0 * static_cast<double>(s) / static_cast<double>(n)));
  }
  std::vector<SampleFixed> c(n, MillivoltsToSampleFixed(100.0));
  std::vector<SampleFixed> y_expected = y;
  std::vector<SampleFixed> c_expected = c;

  stage.InjectNoise(project, schedule, 2, 1, &y, &c);
  LegacyCaptureNoise(project.sections[1].noise, 1, 2, &y_expected, &c_expected);

  EXPECT_EQ(y, y_expected);
  EXPECT_EQ(c, c_expected);
}

// Welch power spectral density estimate at one frequency: Hann-windowed
// segments with 50 % overlap, evaluated directly (no FFT).
double WelchPower(const std::vector<double>& signal, double frequency_mhz,
                  double sample_rate_hz) {
  constexpr std::size_t kSegment = 256;
  constexpr double kTwoPi = 2.0 * 3.14159265358979323846;
  std::vector<double> cos_table(kSegment);
  std::vector<double> sin_table(kSegment);
  const double omega = kTwoPi * frequency_mhz * 1.0e6 / sample_rate_hz;
  for (std::size_t n = 0; n < kSegment; ++n) {
    const double window =
        0.5 - (0.5 * std::cos(kTwoPi * static_cast<double>(n) /
                              static_cast<double>(kSegment)));
    cos_table[n] = window * std::cos(omega * static_cast<double>(n));
    sin_table[n] = window * std::sin(omega * static_cast<double>(n));
  }
  double total = 0.0;
  std::size_t segments = 0;
  for (std::size_t start = 0; start + kSegment <= signal.size();
       start += kSegment / 2U) {
    double re = 0.0;
    double im = 0.0;
    for (std::size_t n = 0; n < kSegment; ++n) {
      re += signal[start + n] * cos_table[n];
      im += signal[start + n] * sin_table[n];
    }
    total += (re * re) + (im * im);
    ++segments;
  }
  return total / static_cast<double>(segments);
}

// Mean Welch power over a +/-0.2 MHz band around centre_mhz, in dB.
double BandPowerDb(const std::vector<double>& signal, double centre_mhz,
                   double sample_rate_hz) {
  double sum = 0.0;
  for (double delta : {-0.2, -0.1, 0.0, 0.1, 0.2}) {
    sum += WelchPower(signal, centre_mhz + delta, sample_rate_hz);
  }
  return 10.0 * std::log10(sum / 5.0);
}

TEST(NoiseInjectionTest, ShapedCaptureNoiseFollowsSpectrumAndKeepsLevel) {
  NullLogger logger;
  NoiseInjectionStage stage(&logger);

  const std::vector<NoiseSpectrumPoint> table = {
      {0.0, 0.0}, {3.5, 4.0}, {6.0, 10.0}};
  constexpr std::size_t kFrames = 2;
  auto ctx = MakeTestContext(Standard::kPal, true, 40.0, 0.0);
  ctx.project.sections[0].noise.spectrum = table;
  ctx.project.sections[0].noise.noise_seed = 31;
  ctx.project.sections[0].noise.noise_seed_specified = true;
  ctx.schedule.push_back(ctx.schedule[0]);

  const std::size_t n = PalSamplesPerFrame();
  constexpr double kMidGreyMv = 350.0;
  std::vector<SampleFixed> y(n * kFrames, MillivoltsToSampleFixed(kMidGreyMv));
  std::vector<SampleFixed> c(n * kFrames, MillivoltsToSampleFixed(0.0));
  stage.InjectNoise(ctx.project, ctx.schedule, 0, kFrames, &y, &c);

  std::vector<double> noise_mv(y.size());
  for (std::size_t s = 0; s < y.size(); ++s) {
    noise_mv[s] = SampleFixedToMillivolts(y[s]) - kMidGreyMv;
  }

  // 40 dB at a 700 mV white level: sigma = 1 IRE = 7.0 mV.
  EXPECT_NEAR(ComputeStdDevMv(y, kMidGreyMv), 7.0, 7.0 * 0.02);

  const double sample_rate_hz =
      GetTimingConstants(Standard::kPal).sample_rate_4fsc_hz;
  constexpr double kReferenceMhz = 1.0;
  const double reference_db =
      BandPowerDb(noise_mv, kReferenceMhz, sample_rate_hz);
  for (double centre_mhz : {2.0, 2.8, 4.5, 5.3, 7.0, 8.0}) {
    const double measured =
        BandPowerDb(noise_mv, centre_mhz, sample_rate_hz) - reference_db;
    const double expected = InterpolateNoiseSpectrumDb(table, centre_mhz) -
                            InterpolateNoiseSpectrumDb(table, kReferenceMhz);
    EXPECT_NEAR(measured, expected, 1.0) << "band at " << centre_mhz << " MHz";
  }
}

// One section carrying both layers, shaped, with fixed seeds.
TestContext MakeLayeredContext(std::size_t frames) {
  auto ctx = MakeTestContext(Standard::kPal, true, 40.0, 4.0);
  NoiseParameters& noise = ctx.project.sections[0].noise;
  noise.noise_seed = 1001;
  noise.noise_seed_specified = true;
  noise.spectrum = {{0.0, 0.0}, {5.0, 8.0}};
  noise.shared.enabled = true;
  noise.shared.noise_db = 42.0;
  noise.shared.noise_seed = 77;
  noise.shared.noise_seed_specified = true;
  noise.shared.spectrum = {{1.0, 6.0}, {4.0, -3.0}};
  ctx.schedule.clear();
  for (std::size_t f = 0; f < frames; ++f) {
    ctx.schedule.push_back(DiscFrame(&ctx.project.sections[0], 100U + f));
  }
  return ctx;
}

TEST(NoiseInjectionTest, LayeredNoiseIsIndependentOfBatchSize) {
  NullLogger logger;
  NoiseInjectionStage stage(&logger);
  constexpr std::size_t kFrames = 3;
  auto ctx = MakeLayeredContext(kFrames);

  const std::size_t n = PalSamplesPerFrame();
  const std::vector<SampleFixed> input(n * kFrames,
                                       MillivoltsToSampleFixed(400.0));
  std::vector<SampleFixed> y_batch = input;
  std::vector<SampleFixed> c_batch = input;
  stage.InjectNoise(ctx.project, ctx.schedule, 0, kFrames, &y_batch, &c_batch);

  for (std::size_t f = 0; f < kFrames; ++f) {
    std::vector<SampleFixed> y_single(n, MillivoltsToSampleFixed(400.0));
    std::vector<SampleFixed> c_single = y_single;
    stage.InjectNoise(ctx.project, ctx.schedule, f, 1, &y_single, &c_single);
    EXPECT_TRUE(std::equal(y_single.begin(), y_single.end(),
                           y_batch.begin() + static_cast<long>(f * n)))
        << "frame " << f;
    EXPECT_TRUE(std::equal(c_single.begin(), c_single.end(),
                           c_batch.begin() + static_cast<long>(f * n)))
        << "frame " << f;
  }
}

// Runs the shared layer alone on one zero frame of schedule[frame].
std::vector<SampleFixed> SharedLayerFrame(
    const NoiseInjectionStage& stage, const Project& project,
    const std::vector<IGenerationStage::FrameScheduleItem>& schedule,
    std::size_t frame) {
  std::vector<SampleFixed> y(PalSamplesPerFrame(), 0);
  std::vector<SampleFixed> c(PalSamplesPerFrame(), 0);
  stage.InjectNoise(project, schedule, frame, 1, &y, &c);
  EXPECT_EQ(y, c);
  return y;
}

Project MakeSharedOnlyProject(std::size_t section_count) {
  Project project;
  project.cvbs_presets.video_standard_preset = Standard::kPal;
  project.cvbs_presets.sample_encoding_preset = "CVBS_U10_4FSC";
  for (std::size_t s = 0; s < section_count; ++s) {
    Section section;
    section.name = "S" + std::to_string(s);
    section.noise.shared.enabled = true;
    section.noise.shared.noise_db = 44.0;
    section.noise.shared.noise_seed = 77;
    section.noise.shared.noise_seed_specified = true;
    section.noise.shared.spectrum = {{0.0, 0.0}, {6.0, 10.0}};
    project.sections.push_back(section);
  }
  return project;
}

TEST(NoiseInjectionTest, SharedNoiseDependsOnlyOnDiscPosition) {
  NullLogger logger;
  // Separate stage instances, as for two separate captures.
  NoiseInjectionStage first_capture(&logger);
  NoiseInjectionStage second_capture(&logger);

  // Capture A: one section playing disc frames 10..13.
  const Project project_a = MakeSharedOnlyProject(1);
  std::vector<IGenerationStage::FrameScheduleItem> schedule_a;
  for (std::size_t d = 10; d <= 13; ++d) {
    schedule_a.push_back(DiscFrame(&project_a.sections[0], d));
  }

  // Capture B: starts earlier, splits into three sections and replays 11..12.
  const Project project_b = MakeSharedOnlyProject(3);
  std::vector<IGenerationStage::FrameScheduleItem> schedule_b = {
      DiscFrame(&project_b.sections[0], 8),
      DiscFrame(&project_b.sections[0], 9),
      DiscFrame(&project_b.sections[1], 10),
      DiscFrame(&project_b.sections[1], 11),
      DiscFrame(&project_b.sections[2], 12),
      DiscFrame(&project_b.sections[2], 11),
      DiscFrame(&project_b.sections[2], 12),
      DiscFrame(&project_b.sections[2], 13),
  };

  for (std::size_t b = 2; b < schedule_b.size(); ++b) {
    const std::size_t disc = schedule_b[b].enrichment->disc_frame_index;
    const std::vector<SampleFixed> expected =
        SharedLayerFrame(first_capture, project_a, schedule_a, disc - 10U);
    EXPECT_EQ(SharedLayerFrame(second_capture, project_b, schedule_b, b),
              expected)
        << "capture B frame " << b << " (disc frame " << disc << ")";
  }
}

TEST(NoiseInjectionTest, SharedNoiseDiffersBetweenConsecutiveDiscFrames) {
  // A held-still picture repeated over consecutive picture numbers still gets
  // fresh pressing noise on each picture.
  NullLogger logger;
  NoiseInjectionStage stage(&logger);
  const Project project = MakeSharedOnlyProject(1);
  const std::vector<IGenerationStage::FrameScheduleItem> schedule = {
      DiscFrame(&project.sections[0], 20), DiscFrame(&project.sections[0], 21)};
  EXPECT_NE(SharedLayerFrame(stage, project, schedule, 0),
            SharedLayerFrame(stage, project, schedule, 1));
}

TEST(NoiseInjectionTest, SharedNoiseFallsBackToSchedulePosition) {
  NullLogger logger;
  NoiseInjectionStage stage(&logger);
  const Project project = MakeSharedOnlyProject(1);
  IGenerationStage::FrameScheduleItem bare;
  bare.section = &project.sections[0];
  const std::vector<IGenerationStage::FrameScheduleItem> hand_built = {bare,
                                                                       bare};
  const std::vector<IGenerationStage::FrameScheduleItem> enriched = {
      DiscFrame(&project.sections[0], 0), DiscFrame(&project.sections[0], 1)};
  EXPECT_EQ(SharedLayerFrame(stage, project, hand_built, 1),
            SharedLayerFrame(stage, project, enriched, 1));
}

double Correlation(const std::vector<SampleFixed>& a,
                   const std::vector<SampleFixed>& b) {
  double sum_ab = 0.0;
  double sum_aa = 0.0;
  double sum_bb = 0.0;
  for (std::size_t s = 0; s < a.size(); ++s) {
    const double x = SampleFixedToMillivolts(a[s]);
    const double y = SampleFixedToMillivolts(b[s]);
    sum_ab += x * y;
    sum_aa += x * x;
    sum_bb += y * y;
  }
  return sum_ab / std::sqrt(sum_aa * sum_bb);
}

// White capture-only and shared-only versions of one section, both seeded
// with `seed`, run on frame 0 / disc frame 0.
struct LayerPair {
  std::vector<SampleFixed> capture;
  std::vector<SampleFixed> shared;
  std::vector<SampleFixed> both;
};

LayerPair RunLayersSeparately(std::int64_t seed, double capture_db,
                              double shared_db) {
  NullLogger logger;
  NoiseInjectionStage stage(&logger);
  auto ctx = MakeTestContext(Standard::kPal, true, capture_db, 0.0);
  NoiseParameters& noise = ctx.project.sections[0].noise;
  noise.noise_seed = seed;
  noise.noise_seed_specified = true;
  noise.shared.noise_db = shared_db;
  noise.shared.noise_seed = seed;
  noise.shared.noise_seed_specified = true;
  ctx.schedule = {DiscFrame(&ctx.project.sections[0], 0)};

  const std::size_t n = PalSamplesPerFrame();
  LayerPair result;
  auto run = [&](bool capture, bool shared, std::vector<SampleFixed>* out) {
    noise.enabled = capture;
    noise.shared.enabled = shared;
    out->assign(n, 0);
    std::vector<SampleFixed> c(n, 0);
    stage.InjectNoise(ctx.project, ctx.schedule, 0, 1, out, &c);
  };
  run(true, false, &result.capture);
  run(false, true, &result.shared);
  run(true, true, &result.both);
  return result;
}

TEST(NoiseInjectionTest, SharedSeedEqualToCaptureSeedGivesUncorrelatedLayers) {
  const LayerPair layers = RunLayersSeparately(0, 40.0, 40.0);
  EXPECT_LT(std::abs(Correlation(layers.capture, layers.shared)), 0.01);
}

TEST(NoiseInjectionTest, LayerVariancesAdd) {
  const LayerPair layers = RunLayersSeparately(12345, 40.0, 44.0);
  // Both layers together are exactly the sum of each alone (no clamping at
  // these levels).
  for (std::size_t s = 0; s < layers.both.size(); ++s) {
    ASSERT_EQ(layers.both[s], layers.capture[s] + layers.shared[s]);
  }
  const double capture_sigma = 7.0;  // 40 dB at 700 mV white.
  const double shared_sigma = 7.0 / std::pow(10.0, 4.0 / 20.0);
  const double expected = std::sqrt((capture_sigma * capture_sigma) +
                                    (shared_sigma * shared_sigma));
  EXPECT_NEAR(ComputeStdDevMv(layers.both, 0.0), expected, expected * 0.02);
}

}  // namespace
}  // namespace videosynth
