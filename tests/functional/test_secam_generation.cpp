/*
 * File:        test_secam_generation.cpp
 * Module:      generation_tests
 * Purpose:     Verifies end-to-end SECAM chroma generation on the 625-line PAL
 * preset from the committed SECAM fixture project.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 itewreed
 */

#include <gtest/gtest.h>

#include <cmath>
#include <complex>
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

#include "fixture_paths.h"
#include "videosynth/fixed_point.h"
#include "videosynth/generation_stage.h"
#include "videosynth/path_resolution.h"
#include "videosynth/project_validator.h"
#include "videosynth/timing_constants.h"
#include "videosynth/yaml_project_parser.h"

namespace videosynth {
namespace {

constexpr char kFixture[] = "general/secam_progressive_exr.yaml";
constexpr double kForHz = 4406250.0;
constexpr double kFobHz = 4250000.0;
constexpr double kPalFscHz = 4433618.75;

// Resolves the fixture's asset paths and sends its output to a scratch
// directory, as the application does.
Project LoadSecamFixture() {
  YamlProjectParser parser;
  const ParseResult parsed =
      parser.ParseFile(test_support::FixturePath(kFixture));
  EXPECT_TRUE(parsed.ok);
  Project project = parsed.project;

  AssetRootMap roots = DefaultAssetRoots();
  roots.roots["output"] =
      (std::filesystem::temp_directory_path() / "videosynth_secam_generation")
          .string();
  const std::string project_dir = test_support::FixtureProjectDir(kFixture);
  for (Section& section : project.sections) {
    section.source = ResolveAssetPath(section.source, roots, project_dir,
                                      /*anchor_unset=*/true);
  }
  project.output.video_path = ResolveAssetPath(
      project.output.video_path, roots, project_dir, /*anchor_unset=*/true);
  return project;
}

// Start sample of a 625-line frame line within its frame (lines 1-312 and
// 314-625 are 1135 samples, line 313 is 1137).
std::size_t LineOffset(int line_1based) {
  if (line_1based <= 313) {
    return static_cast<std::size_t>(line_1based - 1) * 1135U;
  }
  return 312U * 1135U + 1137U +
         static_cast<std::size_t>(line_1based - 314) * 1135U;
}

// Magnitude of the single-bin DFT of a chroma window at frequency_hz.
double ToneMagnitude(const std::vector<SampleFixed>& c_mv, std::size_t first,
                     std::size_t count, double frequency_hz,
                     double sample_rate_hz) {
  std::complex<double> acc(0.0, 0.0);
  for (std::size_t i = 0; i < count; ++i) {
    const double angle =
        -2.0 * M_PI * frequency_hz * static_cast<double>(i) / sample_rate_hz;
    acc += SampleFixedToMillivolts(c_mv[first + i]) *
           std::complex<double>(std::cos(angle), std::sin(angle));
  }
  return std::abs(acc) / static_cast<double>(count);
}

// Mean frequency of a chroma window from its first and last rising zero
// crossings, linearly interpolated.
double MeanFrequencyHz(const std::vector<SampleFixed>& c_mv, std::size_t first,
                       std::size_t count, double sample_rate_hz) {
  double first_crossing = -1.0;
  double last_crossing = -1.0;
  int cycles = -1;
  for (std::size_t i = first + 1; i < first + count; ++i) {
    const double a = SampleFixedToMillivolts(c_mv[i - 1]);
    const double b = SampleFixedToMillivolts(c_mv[i]);
    if (a < 0.0 && b >= 0.0) {
      const double crossing = static_cast<double>(i - 1) + (a / (a - b));
      if (first_crossing < 0.0) {
        first_crossing = crossing;
      }
      last_crossing = crossing;
      ++cycles;
    }
  }
  if (cycles < 1) {
    return 0.0;
  }
  return static_cast<double>(cycles) * sample_rate_hz /
         (last_crossing - first_crossing);
}

class SecamGenerationTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    Project project = LoadSecamFixture();
    ProjectValidator validator;
    const ValidationResult validation = validator.Validate(project);
    ASSERT_TRUE(validation.is_valid)
        << (validation.errors.empty() ? "" : validation.errors[0]);

    GenerationStage generation;
    std::vector<std::string> errors;
    ASSERT_TRUE(generation.Generate(project, &y_mv_, &c_mv_, &errors));
    ASSERT_TRUE(errors.empty());
  }

  static std::size_t FrameLine(int frame, int line_1based) {
    return static_cast<std::size_t>(frame) *
               static_cast<std::size_t>(SamplesPerFrame4fsc(Standard::kPal)) +
           LineOffset(line_1based);
  }

  static std::size_t Us(double microseconds) {
    return static_cast<std::size_t>(
        std::lround(microseconds * 1e-6 * SampleRate()));
  }

  static double SampleRate() {
    return GetTimingConstants(Standard::kPal).sample_rate_4fsc_hz;
  }

  static double Tone(int frame, int line, double from_us, double to_us,
                     double frequency_hz) {
    return ToneMagnitude(c_mv_, FrameLine(frame, line) + Us(from_us),
                         Us(to_us) - Us(from_us), frequency_hz, SampleRate());
  }

  static double Frequency(int frame, int line, double from_us, double to_us) {
    return MeanFrequencyHz(c_mv_, FrameLine(frame, line) + Us(from_us),
                           Us(to_us) - Us(from_us), SampleRate());
  }

  static std::vector<SampleFixed> y_mv_;
  static std::vector<SampleFixed> c_mv_;
};

std::vector<SampleFixed> SecamGenerationTest::y_mv_;
std::vector<SampleFixed> SecamGenerationTest::c_mv_;

TEST_F(SecamGenerationTest, GeneratesAllFixtureFrames) {
  EXPECT_EQ(
      c_mv_.size(),
      static_cast<std::size_t>(SamplesPerFrame4fsc(Standard::kPal)) * 16U);
}

TEST_F(SecamGenerationTest, BackPorchCarriesLineReferenceInsteadOfPalBurst) {
  // ITU-R BT.1700 Annex 1 Part C Table 4 item 13 / Figure 16: frame 1 line
  // 100 is a D'B line and line 101 a D'R line; the back porch carries the
  // undeviated subcarrier, not the 4.43 MHz PAL burst.
  EXPECT_NEAR(Frequency(0, 100, 6.0, 9.0), kFobHz, 10.0e3);
  EXPECT_NEAR(Frequency(0, 101, 6.0, 9.0), kForHz, 10.0e3);
  EXPECT_GT(std::abs(Frequency(0, 100, 6.0, 9.0) - kPalFscHz), 150.0e3);
}

TEST_F(SecamGenerationTest, LineSequenceSwapsEveryFrame) {
  EXPECT_NEAR(Frequency(1, 100, 6.0, 9.0), kForHz, 10.0e3);
  EXPECT_NEAR(Frequency(1, 101, 6.0, 9.0), kFobHz, 10.0e3);
}

TEST_F(SecamGenerationTest, FieldIdentificationLinesReachMaximumDeviation) {
  // ITU-R BT.470-6 Table 2 item 2.18: +350 kHz on D'R lines and −350 kHz on
  // D'B lines once the trapezoid has settled.
  EXPECT_GT(Tone(0, 7, 40.0, 60.0, kForHz + 350.0e3),
            4.0 * Tone(0, 7, 40.0, 60.0, kForHz));
  EXPECT_GT(Tone(0, 8, 40.0, 60.0, kFobHz - 350.0e3),
            4.0 * Tone(0, 8, 40.0, 60.0, kFobHz));
  EXPECT_GT(Tone(0, 320, 40.0, 60.0, kFobHz - 350.0e3),
            4.0 * Tone(0, 320, 40.0, 60.0, kFobHz));
}

TEST_F(SecamGenerationTest, FieldBlankingLinesWithoutIdentificationAreBlank) {
  // ITU-R BT.470-6 Table 2 item 2.17b: subcarrier blanked in field blanking
  // outside the identification lines.
  for (const int line : {1, 6, 16, 22, 311, 319, 329}) {
    for (std::size_t i = 0; i < 1135U; ++i) {
      ASSERT_EQ(c_mv_[FrameLine(0, line) + i], SampleFixed{0})
          << "line " << line << " sample " << i;
    }
  }
}

}  // namespace
}  // namespace videosynth
