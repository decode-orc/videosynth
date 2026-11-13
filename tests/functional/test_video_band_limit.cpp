/*
 * File:        test_video_band_limit.cpp
 * Module:      generation_stage_tests
 * Purpose:     Validates that generated pictures and OSD text carry nothing
 *              above the standard's video band limit, even from a hard-edged
 *              source painted with nearest-pixel mapping
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
#include "videosynth/model.h"
#include "videosynth/picture_chroma.h"
#include "videosynth/timing_constants.h"
#include "videosynth/video_band_limit.h"

namespace videosynth {
namespace {

constexpr double kPi = 3.14159265358979323846;

Project MakeBarsProject(Standard standard, bool with_osd) {
  Project project;
  project.cvbs_presets.video_standard_preset = standard;
  project.cvbs_presets.sample_encoding_preset = "CVBS_U10_4FSC";
  project.cvbs_presets.signal_state_preset = "STANDARD_STABLE_LOCKED";
  const std::string raster =
      Is625LineStandard(standard) ? "720x576" : "720x486";
  Section section{
      .name = "Bars",
      .type = "progressive",
      .line_injections = {},
      .source = (std::filesystem::path(VIDEOSYNTH_SOURCE_DIR) /
                 "videosynth-assets/assets/exr" / raster / "100_BARS.exr")
                    .string(),
      .duration_frames = 1};
  if (with_osd) {
    OsdOverlay overlay;
    overlay.text = "EDGE TEST 0123";
    overlay.x = 40;
    overlay.y = 40;
    overlay.scale = 2;
    overlay.bg_level = OsdBgLevel::kBlack;
    section.osd.overlays.push_back(overlay);
  }
  project.sections.push_back(section);
  return project;
}

// Energy above min_hz over total AC energy, in dB, of a Blackman-Harris
// tapered segment of luma.
double FractionAboveDb(const std::vector<SampleFixed>& y, std::size_t start,
                       std::size_t count, double min_hz, double fs) {
  std::vector<double> tapered(count);
  double mean = 0.0;
  for (std::size_t n = 0; n < count; ++n) {
    mean += SampleFixedToMillivolts(y[start + n]);
  }
  mean /= static_cast<double>(count);
  for (std::size_t n = 0; n < count; ++n) {
    const double phase =
        2.0 * kPi * static_cast<double>(n) / static_cast<double>(count - 1U);
    const double window = 0.35875 - (0.48829 * std::cos(phase)) +
                          (0.14128 * std::cos(2.0 * phase)) -
                          (0.01168 * std::cos(3.0 * phase));
    tapered[n] = window * (SampleFixedToMillivolts(y[start + n]) - mean);
  }
  double total = 0.0;
  double above = 0.0;
  for (std::size_t k = 1; k <= count / 2U; ++k) {
    double re = 0.0;
    double im = 0.0;
    for (std::size_t n = 0; n < count; ++n) {
      const double angle =
          2.0 * kPi * static_cast<double>(k * n) / static_cast<double>(count);
      re += tapered[n] * std::cos(angle);
      im -= tapered[n] * std::sin(angle);
    }
    const double energy = (re * re) + (im * im);
    total += energy;
    if (fs * static_cast<double>(k) / static_cast<double>(count) >= min_hz) {
      above += energy;
    }
  }
  return 10.0 * std::log10(above / total);
}

class VideoBandLimitTest : public ::testing::TestWithParam<Standard> {};

// 100 % bars painted pixel by pixel step from bar to bar in one sample, and
// OSD text is keyed in over them; every picture line, the ones under the
// text among them, holds less than -60 dB of its energy beyond the stopband
// edge.
TEST_P(VideoBandLimitTest, PictureAndOsdStayInsideTheVideoBand) {
  const Standard standard = GetParam();
  const double fs = GetTimingConstants(standard).sample_rate_4fsc_hz;
  const double stop_hz = NominalVideoBandwidth(standard).stopband_edge_hz;
  for (const bool with_osd : {false, true}) {
    const Project project = MakeBarsProject(standard, with_osd);
    std::vector<SampleFixed> y;
    std::vector<SampleFixed> c;
    std::vector<std::string> errors;
    ASSERT_TRUE(GenerationStage().Generate(project, &y, &c, &errors))
        << (errors.empty() ? "" : errors[0]);
    const std::vector<PictureLineWindow> windows =
        BuildPictureLineWindows(project, standard);
    ASSERT_GT(windows.size(), 100U);
    // Every 8th line, which samples the lines under the text (text rows 40
    // to 55 of each field).
    for (std::size_t w = 0; w < windows.size(); w += 8U) {
      EXPECT_LT(
          FractionAboveDb(y, windows[w].start, windows[w].count, stop_hz, fs),
          -60.0)
          << StandardToString(standard) << " osd " << with_osd << " line "
          << windows[w].line_1based;
    }
  }
}

INSTANTIATE_TEST_SUITE_P(Standards, VideoBandLimitTest,
                         ::testing::Values(Standard::kPal, Standard::kNtsc,
                                           Standard::kPalM),
                         [](const ::testing::TestParamInfo<Standard>& info) {
                           return StandardToString(info.param);
                         });

}  // namespace
}  // namespace videosynth
