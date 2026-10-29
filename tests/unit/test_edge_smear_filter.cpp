/*
 * File:        test_edge_smear_filter.cpp
 * Module:      channel_tests
 * Purpose:     Validates the channel edge smear: step response, DC gain and
 *              circular warm-up within the frame.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <utility>
#include <vector>

#include "videosynth/edge_smear_filter.h"
#include "videosynth/fixed_point.h"

namespace videosynth {
namespace {

TEST(EdgeSmearFilterTest, StepResponseIsOneMinusFractionTimesPolePower) {
  constexpr double kStepMv = 10000.0;
  constexpr std::size_t kStepAt = 1000;
  for (const auto& [fraction, tau] :
       {std::pair{0.3, 5.0}, std::pair{0.05, 1.0}, std::pair{0.5, 40.0}}) {
    // A zero tail, so the circular warm-up primes a zero state.
    std::vector<SampleFixed> frame(20000, 0);
    for (std::size_t n = kStepAt; n < 15000U; ++n) {
      frame[n] = MillivoltsToSampleFixed(kStepMv);
    }
    ApplyEdgeSmear(fraction, tau, frame.data(), frame.size());

    const double pole = std::exp(-1.0 / tau);
    for (std::size_t n = 0; n < kStepAt; ++n) {
      ASSERT_EQ(frame[n], 0) << n;
    }
    for (std::size_t n = 0; n < 4000U; ++n) {
      const double expected =
          1.0 - (fraction * std::pow(pole, static_cast<double>(n + 1U)));
      ASSERT_NEAR(SampleFixedToMillivolts(frame[kStepAt + n]) / kStepMv,
                  expected, 1e-9)
          << fraction << " " << tau << " n=" << n;
    }
  }
}

TEST(EdgeSmearFilterTest, DcGainIsOne) {
  const SampleFixed level = MillivoltsToSampleFixed(123.456);
  std::vector<SampleFixed> frame(50000, level);
  ApplyEdgeSmear(0.4, 25.0, frame.data(), frame.size());
  for (SampleFixed sample : frame) {
    ASSERT_LE(std::llabs(sample - level), 1) << sample;
  }
}

TEST(EdgeSmearFilterTest, WarmsUpOverTheFramesOwnTail) {
  // A level held over the end of the frame carries into its start, as if the
  // frame were followed by itself.
  const SampleFixed high = MillivoltsToSampleFixed(500.0);
  std::vector<SampleFixed> frame(10000, 0);
  for (std::size_t n = 9000; n < frame.size(); ++n) {
    frame[n] = high;
  }
  constexpr double kFraction = 0.3;
  constexpr double kTau = 10.0;
  ApplyEdgeSmear(kFraction, kTau, frame.data(), frame.size());
  // At sample 0 the input falls to zero: the output is the smeared fraction
  // of a fully charged state, decaying one step.
  const double pole = std::exp(-1.0 / kTau);
  EXPECT_NEAR(SampleFixedToMillivolts(frame[0]), 500.0 * kFraction * pole,
              1e-3);
}

TEST(EdgeSmearFilterTest, ZeroFractionLeavesTheFrameUntouched) {
  std::vector<SampleFixed> frame(1000);
  for (std::size_t n = 0; n < frame.size(); ++n) {
    frame[n] = static_cast<SampleFixed>(n * 7919U);
  }
  const std::vector<SampleFixed> before = frame;
  ApplyEdgeSmear(0.0, 5.0, frame.data(), frame.size());
  EXPECT_EQ(frame, before);
}

}  // namespace
}  // namespace videosynth
