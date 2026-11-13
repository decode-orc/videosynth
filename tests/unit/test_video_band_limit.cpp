/*
 * File:        test_video_band_limit.cpp
 * Module:      generation_tests
 * Purpose:     Unit tests for the standard's video band limit: band edges,
 *              passband flatness, stopband depth, the blanking-padded run
 *              and the overshoot of a hard step
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <vector>

#include "videosynth/fixed_point.h"
#include "videosynth/model.h"
#include "videosynth/timing_constants.h"
#include "videosynth/video_band_limit.h"

namespace videosynth {
namespace {

constexpr double kPi = 3.14159265358979323846;

double SampleRate(Standard standard) {
  return GetTimingConstants(standard).sample_rate_4fsc_hz;
}

// |H(f)| in dB of a symmetric kernel centred on its middle tap.
double ResponseDb(const std::vector<double>& kernel, double f_hz,
                  double sample_rate_hz) {
  const double centre = static_cast<double>(kernel.size() - 1U) / 2.0;
  std::complex<double> sum = 0.0;
  for (std::size_t i = 0; i < kernel.size(); ++i) {
    const double angle =
        -2.0 * kPi * f_hz / sample_rate_hz * (static_cast<double>(i) - centre);
    sum += kernel[i] * std::polar(1.0, angle);
  }
  return 20.0 * std::log10(std::abs(sum));
}

TEST(VideoBandLimitTest, BandEdgesFollowTheWidestBroadcastSystem) {
  for (const Standard standard : {Standard::kPal, Standard::kSecam}) {
    const VideoBandwidth band = NominalVideoBandwidth(standard);
    EXPECT_DOUBLE_EQ(band.passband_edge_hz, 6.0e6);
    EXPECT_DOUBLE_EQ(band.stopband_edge_hz, 8.0e6);
  }
  for (const Standard standard : {Standard::kNtsc, Standard::kPalM}) {
    const VideoBandwidth band = NominalVideoBandwidth(standard);
    EXPECT_DOUBLE_EQ(band.passband_edge_hz, 4.2e6);
    EXPECT_DOUBLE_EQ(band.stopband_edge_hz, 6.5e6);
  }
}

TEST(VideoBandLimitTest, PassbandIsFlatAndStopbandIsNearlySeventyDbDown) {
  for (const Standard standard :
       {Standard::kPal, Standard::kSecam, Standard::kNtsc, Standard::kPalM}) {
    const double fs = SampleRate(standard);
    const VideoBandLimiter limiter(standard, fs);
    const VideoBandwidth band = NominalVideoBandwidth(standard);
    for (double f = 0.0; f <= band.passband_edge_hz; f += 0.1e6) {
      EXPECT_NEAR(ResponseDb(limiter.kernel(), f, fs), 0.0, 0.003)
          << StandardToString(standard) << " " << f;
    }
    for (double f = band.stopband_edge_hz; f <= fs / 2.0; f += 0.05e6) {
      EXPECT_LT(ResponseDb(limiter.kernel(), f, fs), -69.5)
          << StandardToString(standard) << " " << f;
    }
    EXPECT_EQ(limiter.reach(), static_cast<int>(limiter.kernel().size() / 2U));
  }
}

TEST(VideoBandLimitTest, RunIsPaddedWithBackground) {
  const VideoBandLimiter limiter(Standard::kPal, SampleRate(Standard::kPal));
  const int reach = limiter.reach();
  const SampleFixed background = MillivoltsToSampleFixed(-12.5);
  VideoBandLimiter::Workspace scratch;

  // A run at background throughout comes out at background, spill included.
  const std::vector<SampleFixed> flat(64, background);
  std::vector<SampleFixed> out(flat.size() +
                               (2U * static_cast<std::size_t>(reach)));
  limiter.Apply(flat.data(), flat.size(), background, &scratch, out.data());
  for (const SampleFixed sample : out) {
    EXPECT_EQ(sample, background);
  }

  // A long run at another level: its middle keeps the level, and the run's
  // edges against the background spread exactly reach samples either side.
  const SampleFixed level = MillivoltsToSampleFixed(500.0);
  const std::vector<SampleFixed> run(400, level);
  out.assign(run.size() + (2U * static_cast<std::size_t>(reach)), 0);
  limiter.Apply(run.data(), run.size(), background, &scratch, out.data());
  EXPECT_NEAR(SampleFixedToMillivolts(out[out.size() / 2U]), 500.0, 1e-3);
  EXPECT_NE(out.front(), background);
  EXPECT_NE(out.back(), background);
}

TEST(VideoBandLimitTest, HardStepOvershootsByLessThanTenPercent) {
  // Ringing is the Gibbs overshoot of a flat passband; it stays below 10 % of
  // the step on every standard.
  for (const Standard standard : {Standard::kPal, Standard::kNtsc}) {
    const VideoBandLimiter limiter(standard, SampleRate(standard));
    const std::vector<SampleFixed> run(200, MillivoltsToSampleFixed(700.0));
    std::vector<SampleFixed> out(
        run.size() + (2U * static_cast<std::size_t>(limiter.reach())));
    VideoBandLimiter::Workspace scratch;
    limiter.Apply(run.data(), run.size(), 0, &scratch, out.data());
    const auto [low, high] = std::minmax_element(out.begin(), out.end());
    EXPECT_LT(SampleFixedToMillivolts(*high), 770.0);
    EXPECT_GT(SampleFixedToMillivolts(*low), -70.0);
    EXPECT_GT(SampleFixedToMillivolts(*high), 700.0) << "a flat filter rings";
  }
}

}  // namespace
}  // namespace videosynth
