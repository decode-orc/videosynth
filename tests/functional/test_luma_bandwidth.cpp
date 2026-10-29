/*
 * File:        test_luma_bandwidth.cpp
 * Module:      generation_tests
 * Purpose:     End-to-end checks of the per-section luma bandwidth limit on
 *              generated multiburst frames in PAL, NTSC and PAL-M: content
 *              above the stopband edge is at least 60 dB down, and sync,
 *              blanking, VBI, VITS and chroma are bit-identical.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

#include "videosynth/fixed_point.h"
#include "videosynth/generation_stage.h"
#include "videosynth/picture_chroma.h"
#include "videosynth/project_validator.h"
#include "videosynth/timing_constants.h"

namespace videosynth {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kCutoffMhz = 2.0;
constexpr double kTransitionMhz = 0.4;

Project MakeProject(Standard standard) {
  Project project;
  project.cvbs_presets.video_standard_preset = standard;
  project.cvbs_presets.sample_encoding_preset = "CVBS_U10_4FSC";
  project.cvbs_presets.signal_state_preset = "STANDARD_STABLE_LOCKED";
  project.output.video_path = "out.cvbs";
  project.output.metadata_path = "out.meta";
  project.line_injections.vits.push_back(VitsInjection{
      .vits_type = (standard == Standard::kPal) ? "uk-national" : "virs",
      .target_lines = {standard == Standard::kPal ? 19 : 20}});
  const std::string raster =
      Is625LineStandard(standard) ? "720x576" : "720x486";
  project.sections.push_back(Section{
      .name = "S",
      .type = "progressive",
      .line_injections = {},
      .source = (std::filesystem::path(VIDEOSYNTH_SOURCE_DIR) /
                 "videosynth-assets/assets/exr" / raster / "MULTIBURST.exr")
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

// Energy of the Blackman-Harris tapered segment at DFT bins whose frequency
// is at least min_hz.
double EnergyAbove(const std::vector<SampleFixed>& signal, std::size_t start,
                   std::size_t count, double min_hz, double sample_rate_hz) {
  std::vector<double> tapered(count);
  double mean = 0.0;
  for (std::size_t n = 0; n < count; ++n) {
    mean += SampleFixedToMillivolts(signal[start + n]);
  }
  mean /= static_cast<double>(count);
  for (std::size_t n = 0; n < count; ++n) {
    const double phase =
        2.0 * kPi * static_cast<double>(n) / static_cast<double>(count - 1U);
    const double window = 0.35875 - (0.48829 * std::cos(phase)) +
                          (0.14128 * std::cos(2.0 * phase)) -
                          (0.01168 * std::cos(3.0 * phase));
    tapered[n] = window * (SampleFixedToMillivolts(signal[start + n]) - mean);
  }
  double energy = 0.0;
  for (std::size_t k = 0; k <= count / 2U; ++k) {
    const double f =
        sample_rate_hz * static_cast<double>(k) / static_cast<double>(count);
    if (f < min_hz) {
      continue;
    }
    double re = 0.0;
    double im = 0.0;
    for (std::size_t n = 0; n < count; ++n) {
      const double angle =
          2.0 * kPi * static_cast<double>(k * n) / static_cast<double>(count);
      re += tapered[n] * std::cos(angle);
      im -= tapered[n] * std::sin(angle);
    }
    energy += (re * re) + (im * im);
  }
  return energy;
}

class LumaBandwidthTest : public ::testing::TestWithParam<Standard> {};

TEST_P(LumaBandwidthTest, StopbandIsSixtyDbDownAndBlankingIsUntouched) {
  const Standard standard = GetParam();
  Project project = MakeProject(standard);
  ASSERT_TRUE(ProjectValidator().Validate(project).is_valid);
  std::vector<SampleFixed> clean_y;
  std::vector<SampleFixed> clean_c;
  Generate(project, &clean_y, &clean_c);

  project.sections[0].luma_bandwidth =
      LumaBandwidthParameters{.enabled = true,
                              .cutoff_mhz = kCutoffMhz,
                              .transition_mhz = kTransitionMhz};
  const ValidationResult validation = ProjectValidator().Validate(project);
  ASSERT_TRUE(validation.is_valid) << validation.errors[0];
  std::vector<SampleFixed> y;
  std::vector<SampleFixed> c;
  Generate(project, &y, &c);
  ASSERT_EQ(y.size(), clean_y.size());

  // Chroma is never filtered; luma changes only inside the picture windows.
  EXPECT_EQ(c, clean_c);
  const std::vector<PictureLineWindow> windows =
      BuildPictureLineWindows(project, standard);
  std::vector<bool> in_window(y.size(), false);
  for (const PictureLineWindow& window : windows) {
    for (std::size_t s = window.start; s < window.start + window.count; ++s) {
      in_window[s] = true;
    }
  }
  std::size_t changed = 0;
  for (std::size_t s = 0; s < y.size(); ++s) {
    if (y[s] != clean_y[s]) {
      ASSERT_TRUE(in_window[s]) << "sample " << s << " outside the picture";
      ++changed;
    }
  }
  EXPECT_GT(changed, 1000U);

  // Measured just clear of the stopband edge, beyond the taper's main lobe.
  const double fs = GetTimingConstants(standard).sample_rate_4fsc_hz;
  const double stop_hz = ((kCutoffMhz + (kTransitionMhz / 2.0)) * 1e6) + 0.1e6;
  double clean_energy = 0.0;
  double filtered_energy = 0.0;
  for (std::size_t w = 0; w < windows.size(); w += 16U) {
    clean_energy +=
        EnergyAbove(clean_y, windows[w].start, windows[w].count, stop_hz, fs);
    filtered_energy +=
        EnergyAbove(y, windows[w].start, windows[w].count, stop_hz, fs);
  }
  ASSERT_GT(clean_energy, 0.0);
  EXPECT_LT(10.0 * std::log10(filtered_energy / clean_energy), -60.0);
}

INSTANTIATE_TEST_SUITE_P(Standards, LumaBandwidthTest,
                         ::testing::Values(Standard::kPal, Standard::kNtsc,
                                           Standard::kPalM),
                         [](const ::testing::TestParamInfo<Standard>& info) {
                           return StandardToString(info.param);
                         });

TEST(LumaBandwidthValidationTest, RangesAreEnforced) {
  struct Case {
    Standard standard;
    double cutoff_mhz;
    double transition_mhz;
    bool valid;
  };
  // 2 f_sc: PAL 8.867 MHz, NTSC 7.159 MHz.
  const Case cases[] = {
      {Standard::kPal, 0.5, 0.4, true},   {Standard::kPal, 0.49, 0.4, false},
      {Standard::kPal, 8.66, 0.4, true},  {Standard::kPal, 8.68, 0.4, false},
      {Standard::kNtsc, 6.95, 0.4, true}, {Standard::kNtsc, 6.97, 0.4, false},
      {Standard::kPal, 3.0, 0.1, true},   {Standard::kPal, 3.0, 0.09, false},
      {Standard::kPal, 3.0, 2.0, true},   {Standard::kPal, 3.0, 2.01, false},
  };
  for (const Case& test_case : cases) {
    Project project = MakeProject(test_case.standard);
    project.sections[0].luma_bandwidth =
        LumaBandwidthParameters{.enabled = true,
                                .cutoff_mhz = test_case.cutoff_mhz,
                                .transition_mhz = test_case.transition_mhz};
    const ValidationResult result = ProjectValidator().Validate(project);
    EXPECT_EQ(result.is_valid, test_case.valid)
        << test_case.cutoff_mhz << " / " << test_case.transition_mhz;
    if (!test_case.valid && !result.errors.empty()) {
      EXPECT_NE(result.errors[0].find("Luma bandwidth validation error"),
                std::string::npos)
          << result.errors[0];
    }
  }
}

}  // namespace
}  // namespace videosynth
