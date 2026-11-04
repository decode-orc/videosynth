/*
 * File:        test_picture_response_filter.cpp
 * Module:      generation_tests
 * Purpose:     Unit tests for the picture response FIR design (amplitude
 *              against the curve, linear phase, unit DC gain, edge
 *              replication), its presets, and the `picture_response:` parser.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <vector>

#include "videosynth/fixed_point.h"
#include "videosynth/noise_spectrum_filter.h"
#include "videosynth/picture_response_filter.h"
#include "videosynth/timing_constants.h"
#include "videosynth/yaml_project_parser.h"

namespace videosynth {
namespace {

constexpr double kPi = 3.14159265358979323846;

// Amplitude response in dB of a symmetric kernel at cycles_per_sample.
double ResponseDb(const std::vector<double>& kernel, double cycles_per_sample) {
  const double centre = static_cast<double>(kernel.size() - 1U) / 2.0;
  double sum = 0.0;
  for (std::size_t n = 0; n < kernel.size(); ++n) {
    sum += kernel[n] * std::cos(2.0 * kPi * cycles_per_sample *
                                (static_cast<double>(n) - centre));
  }
  return 20.0 * std::log10(std::abs(sum));
}

double PalSampleRateHz() {
  return GetTimingConstants(Standard::kPal).sample_rate_4fsc_hz;
}

PictureResponseParameters Custom(std::vector<NoiseSpectrumPoint> horizontal,
                                 std::vector<VerticalResponsePoint> vertical) {
  PictureResponseParameters response;
  response.enabled = true;
  response.preset = PictureResponsePreset::kCustom;
  response.horizontal_db = std::move(horizontal);
  response.vertical_db = std::move(vertical);
  return response;
}

PictureResponseParameters Domesday() {
  PictureResponseParameters response;
  response.enabled = true;
  response.preset = PictureResponsePreset::kDomesday;
  return response;
}

TEST(PictureResponseFilterTest, FlatCurvesDesignUnitDeltas) {
  const PictureResponseFilter filter(Custom({{0.0, 0.0}}, {{0.0, 0.0}}),
                                     PalSampleRateHz());
  for (const std::vector<double>* kernel :
       {&filter.horizontal_kernel(), &filter.vertical_kernel()}) {
    const std::size_t centre = kernel->size() / 2U;
    for (std::size_t n = 0; n < kernel->size(); ++n) {
      EXPECT_NEAR((*kernel)[n], n == centre ? 1.0 : 0.0, 1e-12) << n;
    }
  }
}

TEST(PictureResponseFilterTest, EmptyCurveLeavesThatDirectionUnfiltered) {
  const PictureResponseFilter horizontal_only(
      Custom({{0.0, 0.0}, {5.0, -10.0}}, {}), PalSampleRateHz());
  EXPECT_TRUE(horizontal_only.has_horizontal());
  EXPECT_FALSE(horizontal_only.has_vertical());
  const PictureResponseFilter vertical_only(
      Custom({}, {{0.0, 0.0}, {0.5, -10.0}}), PalSampleRateHz());
  EXPECT_FALSE(vertical_only.has_horizontal());
  EXPECT_TRUE(vertical_only.has_vertical());
}

TEST(PictureResponseFilterTest, KernelsAreSymmetricWithUnitDcGain) {
  const PictureResponseFilter filter(Domesday(), PalSampleRateHz());
  ASSERT_EQ(filter.horizontal_kernel().size(),
            static_cast<std::size_t>(kPictureResponseHorizontalTaps));
  ASSERT_EQ(filter.vertical_kernel().size(),
            static_cast<std::size_t>(kPictureResponseVerticalTaps));
  for (const std::vector<double>* kernel :
       {&filter.horizontal_kernel(), &filter.vertical_kernel()}) {
    double sum = 0.0;
    for (std::size_t n = 0; n < kernel->size(); ++n) {
      sum += (*kernel)[n];
      EXPECT_NEAR((*kernel)[n], (*kernel)[kernel->size() - 1U - n], 1e-15);
    }
    EXPECT_NEAR(sum, 1.0, 1e-12);
  }
}

TEST(PictureResponseFilterTest, CurveIsNormalisedToItsZeroFrequencyLevel) {
  // A curve 6 dB down everywhere is flat in shape: it passes picture levels.
  const PictureResponseFilter filter(Custom({{0.0, -6.0}, {8.0, -6.0}}, {}),
                                     PalSampleRateHz());
  const std::size_t centre = filter.horizontal_kernel().size() / 2U;
  EXPECT_NEAR(filter.horizontal_kernel()[centre], 1.0, 1e-12);
}

TEST(PictureResponseFilterTest, DomesdayHorizontalResponseFollowsItsCurve) {
  const double fs = PalSampleRateHz();
  const PictureResponseFilter filter(Domesday(), fs);
  const std::vector<NoiseSpectrumPoint> curve = DomesdayHorizontalResponseDb();
  // Mid-segment and plateau frequencies, away from the window's smoothing of
  // the breakpoint corners.
  for (const double mhz : {0.5, 1.5, 3.25, 4.5, 5.0, 7.0}) {
    EXPECT_NEAR(ResponseDb(filter.horizontal_kernel(), mhz * 1.0e6 / fs),
                InterpolateNoiseSpectrumDb(curve, mhz), 0.35)
        << mhz << " MHz";
  }
}

TEST(PictureResponseFilterTest, DomesdayVerticalResponseFollowsItsCurve) {
  const PictureResponseFilter filter(Domesday(), PalSampleRateHz());
  const std::vector<VerticalResponsePoint> curve = DomesdayVerticalResponseDb();
  for (const double cycles : {0.05, 0.1, 0.305, 0.44, 0.5}) {
    EXPECT_NEAR(ResponseDb(filter.vertical_kernel(), cycles),
                InterpolateVerticalResponseDb(curve, cycles), 0.6)
        << cycles << " cycles/row";
  }
}

TEST(PictureResponseFilterTest, FlatLinesAndColumnsPassUnchanged) {
  const PictureResponseFilter filter(Domesday(), PalSampleRateHz());
  constexpr std::size_t kWidth = 300;
  constexpr std::size_t kRows = 40;
  const SampleFixed level = MillivoltsToSampleFixed(412.5);
  const std::vector<SampleFixed> line(kWidth, level);
  std::vector<SampleFixed> out(kWidth);
  filter.ApplyHorizontal(line.data(), kWidth, out.data());
  EXPECT_EQ(out, line);

  // Each row constant along its length but different from its neighbours:
  // the horizontal filter is an identity on every row.
  std::vector<SampleFixed> plane(kWidth * kRows);
  for (std::size_t row = 0; row < kRows; ++row) {
    for (std::size_t x = 0; x < kWidth; ++x) {
      plane[(row * kWidth) + x] =
          MillivoltsToSampleFixed((row % 2U == 0U) ? 100.0 : 600.0);
    }
  }
  for (std::size_t row = 0; row < kRows; ++row) {
    filter.ApplyHorizontal(plane.data() + (row * kWidth), kWidth, out.data());
    EXPECT_TRUE(
        std::equal(out.begin(), out.end(),
                   plane.begin() + static_cast<std::ptrdiff_t>(row * kWidth)))
        << row;
  }

  // Each column constant: the vertical filter is an identity.
  std::vector<SampleFixed> columns(kWidth * kRows);
  for (std::size_t row = 0; row < kRows; ++row) {
    for (std::size_t x = 0; x < kWidth; ++x) {
      columns[(row * kWidth) + x] =
          MillivoltsToSampleFixed(static_cast<double>(x));
    }
  }
  std::vector<SampleFixed> filtered(columns.size());
  filter.ApplyVertical(columns.data(), kRows, kWidth, filtered.data());
  EXPECT_EQ(filtered, columns);
}

TEST(PictureResponseFilterTest, VerticalEdgesAreReplicatedNotZeroPadded) {
  const PictureResponseFilter filter(Domesday(), PalSampleRateHz());
  constexpr std::size_t kWidth = 3;
  constexpr std::size_t kRows = 100;
  std::vector<SampleFixed> plane(kWidth * kRows);
  for (std::size_t row = 0; row < kRows; ++row) {
    for (std::size_t x = 0; x < kWidth; ++x) {
      plane[(row * kWidth) + x] =
          MillivoltsToSampleFixed(row < kRows / 2U ? 100.0 : 600.0);
    }
  }
  std::vector<SampleFixed> out(plane.size());
  filter.ApplyVertical(plane.data(), kRows, kWidth, out.data());
  EXPECT_NEAR(SampleFixedToMillivolts(out.front()), 100.0, 1e-3);
  EXPECT_NEAR(SampleFixedToMillivolts(out.back()), 600.0, 1e-3);
}

TEST(PictureResponseFilterTest, VerticalFilterAttenuatesRowNyquist) {
  // Alternate rows at the row Nyquist frequency, -20 dB in the curve.
  const PictureResponseFilter filter(Custom({}, {{0.0, 0.0}, {0.5, -20.0}}),
                                     PalSampleRateHz());
  constexpr std::size_t kWidth = 2;
  constexpr std::size_t kRows = 200;
  std::vector<SampleFixed> plane(kWidth * kRows);
  for (std::size_t row = 0; row < kRows; ++row) {
    for (std::size_t x = 0; x < kWidth; ++x) {
      plane[(row * kWidth) + x] =
          MillivoltsToSampleFixed(row % 2U == 0U ? 300.0 : 500.0);
    }
  }
  std::vector<SampleFixed> out(plane.size());
  filter.ApplyVertical(plane.data(), kRows, kWidth, out.data());
  const double swing = SampleFixedToMillivolts(out[100 * kWidth]) -
                       SampleFixedToMillivolts(out[101 * kWidth]);
  // The kernel's own response at the row Nyquist frequency (the window
  // softens the curve's -20 dB end point to about -18.6 dB).
  const double designed_db = ResponseDb(filter.vertical_kernel(), 0.5);
  EXPECT_NEAR(designed_db, -20.0, 2.0);
  EXPECT_NEAR(20.0 * std::log10(std::abs(swing) / 200.0), designed_db, 0.01);
}

TEST(PictureResponseFilterTest, PresetIgnoresAnyCustomCurves) {
  PictureResponseParameters response = Domesday();
  response.horizontal_db = {{0.0, 0.0}, {1.0, -40.0}};
  EXPECT_EQ(ResolvedHorizontalResponseDb(response),
            DomesdayHorizontalResponseDb());
  EXPECT_EQ(ResolvedVerticalResponseDb(response), DomesdayVerticalResponseDb());
  const PictureResponseParameters custom = Custom({{0.0, 0.0}}, {});
  EXPECT_EQ(ResolvedHorizontalResponseDb(custom), custom.horizontal_db);
  EXPECT_TRUE(ResolvedVerticalResponseDb(custom).empty());
}

TEST(PictureResponseFilterTest, VerticalCurveInterpolatesAndHoldsEnds) {
  const std::vector<VerticalResponsePoint> curve = {{0.1, -1.0}, {0.3, -5.0}};
  EXPECT_DOUBLE_EQ(InterpolateVerticalResponseDb(curve, 0.0), -1.0);
  EXPECT_DOUBLE_EQ(InterpolateVerticalResponseDb(curve, 0.2), -3.0);
  EXPECT_DOUBLE_EQ(InterpolateVerticalResponseDb(curve, 0.5), -5.0);
  EXPECT_DOUBLE_EQ(InterpolateVerticalResponseDb({}, 0.2), 0.0);
}

TEST(PictureResponseFilterTest, RejectsNonPositiveSampleRate) {
  EXPECT_THROW(PictureResponseFilter(Domesday(), 0.0), std::invalid_argument);
}

std::string SectionYaml(const std::string& picture_response) {
  return "cvbs_presets:\n"
         "  video_standard_preset: PAL\n"
         "output:\n"
         "  video_path: out.cvbs\n"
         "sections:\n"
         "  - name: S\n"
         "    type: progressive\n"
         "    source: f.exr\n"
         "    duration_frames: 1\n"
         "    picture_response:" +
         picture_response;
}

TEST(PictureResponseParserTest, ReadsThePresetName) {
  const ParseResult result =
      YamlProjectParser().ParseString(SectionYaml(" domesday\n"));
  ASSERT_TRUE(result.ok) << result.errors[0];
  const PictureResponseParameters& response =
      result.project.sections[0].picture_response;
  EXPECT_TRUE(response.enabled);
  EXPECT_EQ(response.preset, PictureResponsePreset::kDomesday);
  EXPECT_TRUE(response.horizontal_db.empty());
  EXPECT_TRUE(response.vertical_db.empty());
}

TEST(PictureResponseParserTest, ReadsCustomCurves) {
  const ParseResult result = YamlProjectParser().ParseString(
      SectionYaml("\n"
                  "      horizontal_db: [[0, 0], [2.5, -2], [5.25, -10]]\n"
                  "      vertical_db: [[0, 0], [0.28, -4], [0.375, -10]]\n"));
  ASSERT_TRUE(result.ok) << result.errors[0];
  const PictureResponseParameters& response =
      result.project.sections[0].picture_response;
  EXPECT_TRUE(response.enabled);
  EXPECT_EQ(response.preset, PictureResponsePreset::kCustom);
  ASSERT_EQ(response.horizontal_db.size(), 3U);
  EXPECT_DOUBLE_EQ(response.horizontal_db[2].frequency_mhz, 5.25);
  EXPECT_DOUBLE_EQ(response.horizontal_db[2].relative_db, -10.0);
  ASSERT_EQ(response.vertical_db.size(), 3U);
  EXPECT_DOUBLE_EQ(response.vertical_db[1].cycles_per_row, 0.28);
  EXPECT_DOUBLE_EQ(response.vertical_db[1].relative_db, -4.0);
}

TEST(PictureResponseParserTest, AbsentBlockLeavesTheResponseDisabled) {
  const ParseResult result = YamlProjectParser().ParseString(
      "cvbs_presets:\n"
      "  video_standard_preset: PAL\n"
      "output:\n"
      "  video_path: out.cvbs\n"
      "sections:\n"
      "  - name: S\n"
      "    type: progressive\n"
      "    source: f.exr\n"
      "    duration_frames: 1\n");
  ASSERT_TRUE(result.ok) << result.errors[0];
  EXPECT_FALSE(result.project.sections[0].picture_response.enabled);
}

TEST(PictureResponseParserTest, RejectsBadPresetsKeysAndCurves) {
  const struct {
    const char* yaml;
    const char* error;
  } cases[] = {
      {" sharp\n", "preset name 'domesday'"},
      {" [1, 2]\n", "must be a preset name or a map"},
      {"\n      gain: 3\n", "gain"},
      {" {}\n", "at least one of: horizontal_db, vertical_db"},
      {"\n      horizontal_db: 3\n", "horizontal_db must be a sequence"},
      {"\n      vertical_db: [[0.1]]\n",
       "entries must be [cycles_per_row, relative_db] pairs"},
      {"\n      vertical_db: [[a, b]]\n", "entries must be numeric pairs"},
  };
  for (const auto& test_case : cases) {
    const ParseResult result =
        YamlProjectParser().ParseString(SectionYaml(test_case.yaml));
    ASSERT_FALSE(result.ok) << test_case.yaml;
    ASSERT_FALSE(result.errors.empty());
    EXPECT_NE(result.errors[0].find(test_case.error), std::string::npos)
        << result.errors[0];
  }
}

}  // namespace
}  // namespace videosynth
