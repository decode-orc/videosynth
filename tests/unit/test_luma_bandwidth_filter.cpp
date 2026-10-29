/*
 * File:        test_luma_bandwidth_filter.cpp
 * Module:      generation_tests
 * Purpose:     Validates the luma bandwidth low-pass: -6 dB at the cutoff,
 *              passband ripple, stopband attenuation, unit DC gain and edge
 *              replication.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

#include "videosynth/fixed_point.h"
#include "videosynth/luma_bandwidth_filter.h"
#include "videosynth/model.h"
#include "videosynth/timing_constants.h"
#include "videosynth/yaml_project_parser.h"

namespace videosynth {
namespace {

constexpr double kPi = 3.14159265358979323846;

// Magnitude of the filter's response at frequency_hz, in dB.
double ResponseDb(const LumaBandwidthFilter& filter, double frequency_hz,
                  double sample_rate_hz) {
  const std::vector<double>& kernel = filter.kernel();
  const auto half_taps = kernel.size() / 2U;
  const auto centre = static_cast<double>(half_taps);
  double sum = 0.0;
  for (std::size_t i = 0; i < kernel.size(); ++i) {
    sum += kernel[i] *
           std::cos(2.0 * kPi * frequency_hz *
                    (static_cast<double>(i) - centre) / sample_rate_hz);
  }
  return 20.0 * std::log10(std::max(std::abs(sum), 1e-30));
}

struct Design {
  Standard standard;
  double cutoff_mhz;
  double transition_mhz;
};

TEST(LumaBandwidthFilterTest, ResponseMeetsPassbandAndStopbandLimits) {
  const Design designs[] = {
      {Standard::kPal, 3.0, kLumaBandwidthTransitionDefaultMhz},
      {Standard::kPal, 0.5, 0.1},
      {Standard::kPal, 8.0, 1.0},
      {Standard::kNtsc, 2.5, kLumaBandwidthTransitionDefaultMhz},
      {Standard::kNtsc, 4.2, 2.0},
      {Standard::kPalM, 3.0, 0.2},
  };
  for (const Design& design : designs) {
    const double fs = GetTimingConstants(design.standard).sample_rate_4fsc_hz;
    const double cutoff_hz = design.cutoff_mhz * 1e6;
    const double half_transition_hz = design.transition_mhz * 0.5e6;
    const LumaBandwidthFilter filter(cutoff_hz, design.transition_mhz * 1e6,
                                     fs);
    ASSERT_EQ(filter.kernel().size() % 2U, 1U);
    EXPECT_NEAR(ResponseDb(filter, cutoff_hz, fs), -6.02, 0.1)
        << design.cutoff_mhz;

    double passband_max = -1e9;
    double passband_min = 1e9;
    double stopband_max = -1e9;
    constexpr int kPoints = 4000;
    for (int p = 0; p <= kPoints; ++p) {
      const double f = (fs / 2.0) * static_cast<double>(p) / kPoints;
      const double db = ResponseDb(filter, f, fs);
      if (f <= cutoff_hz - half_transition_hz) {
        passband_max = std::max(passband_max, db);
        passband_min = std::min(passband_min, db);
      } else if (f >= cutoff_hz + half_transition_hz) {
        stopband_max = std::max(stopband_max, db);
      }
    }
    EXPECT_LT(passband_max - passband_min, 0.1) << design.cutoff_mhz;
    EXPECT_LT(std::abs(passband_max), 0.05) << design.cutoff_mhz;
    EXPECT_LT(stopband_max, -60.0) << design.cutoff_mhz;
  }
}

TEST(LumaBandwidthFilterTest, FlatLinePassesUnchanged) {
  const double fs = GetTimingConstants(Standard::kPal).sample_rate_4fsc_hz;
  const LumaBandwidthFilter filter(1.5e6, 0.4e6, fs);
  // Shorter than the kernel, so every output sample uses replicated edges.
  for (std::size_t count :
       {std::size_t{1}, std::size_t{20}, std::size_t{900}}) {
    const std::vector<SampleFixed> in(count, MillivoltsToSampleFixed(523.25));
    std::vector<SampleFixed> out(count);
    filter.Apply(in.data(), count, out.data());
    for (std::size_t n = 0; n < count; ++n) {
      ASSERT_LE(std::abs(out[n] - in[n]), 1) << count << " " << n;
    }
  }
}

TEST(LumaBandwidthFilterTest, EdgesAreReplicatedNotZeroPadded) {
  // A step in the middle of a line: both ends settle to the line's own end
  // levels rather than ringing towards zero.
  const double fs = GetTimingConstants(Standard::kPal).sample_rate_4fsc_hz;
  const LumaBandwidthFilter filter(2.0e6, 0.4e6, fs);
  constexpr std::size_t kCount = 900;
  std::vector<SampleFixed> in(kCount);
  for (std::size_t n = 0; n < kCount; ++n) {
    in[n] = MillivoltsToSampleFixed(n < kCount / 2U ? 100.0 : 600.0);
  }
  std::vector<SampleFixed> out(kCount);
  filter.Apply(in.data(), kCount, out.data());
  EXPECT_NEAR(SampleFixedToMillivolts(out.front()), 100.0, 1e-3);
  EXPECT_NEAR(SampleFixedToMillivolts(out.back()), 600.0, 1e-3);
  // -6 dB point: the step's midpoint lands on the step.
  EXPECT_NEAR(SampleFixedToMillivolts(out[kCount / 2U]) +
                  SampleFixedToMillivolts(out[(kCount / 2U) - 1U]),
              700.0, 1.0);
}

std::string SectionYaml(const std::string& luma_bandwidth) {
  return "cvbs_presets:\n"
         "  video_standard_preset: PAL\n"
         "output:\n"
         "  video_path: out.cvbs\n"
         "sections:\n"
         "  - name: S\n"
         "    type: progressive\n"
         "    source: f.exr\n"
         "    duration_frames: 1\n"
         "    luma_bandwidth:\n" +
         luma_bandwidth;
}

TEST(LumaBandwidthParserTest, ReadsCutoffAndDefaultsTheTransition) {
  YamlProjectParser parser;
  const ParseResult result =
      parser.ParseString(SectionYaml("      cutoff_mhz: 3.2\n"));
  ASSERT_TRUE(result.ok) << result.errors[0];
  const LumaBandwidthParameters& bandwidth =
      result.project.sections[0].luma_bandwidth;
  EXPECT_TRUE(bandwidth.enabled);
  EXPECT_DOUBLE_EQ(bandwidth.cutoff_mhz, 3.2);
  EXPECT_DOUBLE_EQ(bandwidth.transition_mhz,
                   kLumaBandwidthTransitionDefaultMhz);
}

TEST(LumaBandwidthParserTest, RejectsMissingBadAndUnknownKeys) {
  YamlProjectParser parser;
  const struct {
    const char* yaml;
    const char* error;
  } cases[] = {
      {"      transition_mhz: 0.5\n", "missing required field: 'cutoff_mhz'"},
      {"      cutoff_mhz: fast\n", "cutoff_mhz must be a number"},
      {"      cutoff_mhz: 3.0\n      transition_mhz: [1]\n",
       "transition_mhz must be a number"},
      {"      cutoff_mhz: 3.0\n      order: 4\n", "order"},
  };
  for (const auto& test_case : cases) {
    const ParseResult result = parser.ParseString(SectionYaml(test_case.yaml));
    ASSERT_FALSE(result.ok) << test_case.yaml;
    ASSERT_FALSE(result.errors.empty());
    EXPECT_NE(result.errors[0].find(test_case.error), std::string::npos)
        << result.errors[0];
  }
}

}  // namespace
}  // namespace videosynth
