/*
 * File:        test_noise_spectrum_filter.cpp
 * Module:      noise_injection_tests
 * Purpose:     Verifies the noise spectrum FIR design: interpolation, delta
 *              kernel for flat tables, frequency response, normalisation and
 *              determinism.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <cmath>
#include <stdexcept>
#include <vector>

#include "videosynth/model.h"
#include "videosynth/noise_spectrum_filter.h"
#include "videosynth/timing_constants.h"

namespace videosynth {
namespace {

constexpr double kTwoPi = 2.0 * 3.14159265358979323846;
constexpr std::size_t kCentreTap = (kNoiseSpectrumTaps - 1) / 2;

double PalSampleRateHz() {
  return GetTimingConstants(Standard::kPal).sample_rate_4fsc_hz;
}

// Magnitude response of a kernel in dB at frequency_mhz.
double ResponseDb(const std::vector<double>& kernel, double frequency_mhz,
                  double sample_rate_hz) {
  const double omega = kTwoPi * frequency_mhz * 1.0e6 / sample_rate_hz;
  double re = 0.0;
  double im = 0.0;
  for (std::size_t n = 0; n < kernel.size(); ++n) {
    re += kernel[n] * std::cos(omega * static_cast<double>(n));
    im -= kernel[n] * std::sin(omega * static_cast<double>(n));
  }
  return 10.0 * std::log10((re * re) + (im * im));
}

void ExpectDeltaKernel(const std::vector<double>& kernel) {
  ASSERT_EQ(kernel.size(), static_cast<std::size_t>(kNoiseSpectrumTaps));
  for (std::size_t n = 0; n < kernel.size(); ++n) {
    if (n == kCentreTap) {
      EXPECT_NEAR(kernel[n], 1.0, 1e-12);
    } else {
      EXPECT_NEAR(kernel[n], 0.0, 1e-12) << "tap " << n;
    }
  }
}

TEST(NoiseSpectrumFilterTest, InterpolationIsLinearInDbAndHoldsEnds) {
  const std::vector<NoiseSpectrumPoint> table = {{1.0, -2.0}, {3.0, 6.0}};
  EXPECT_DOUBLE_EQ(InterpolateNoiseSpectrumDb(table, 0.0), -2.0);
  EXPECT_DOUBLE_EQ(InterpolateNoiseSpectrumDb(table, 1.0), -2.0);
  EXPECT_DOUBLE_EQ(InterpolateNoiseSpectrumDb(table, 2.0), 2.0);
  EXPECT_DOUBLE_EQ(InterpolateNoiseSpectrumDb(table, 3.0), 6.0);
  EXPECT_DOUBLE_EQ(InterpolateNoiseSpectrumDb(table, 8.0), 6.0);
  EXPECT_DOUBLE_EQ(InterpolateNoiseSpectrumDb({}, 4.0), 0.0);
}

TEST(NoiseSpectrumFilterTest, FlatTableGivesDeltaKernel) {
  const NoiseSpectrumFilter filter({{0.0, 0.0}, {4.0, 0.0}, {8.0, 0.0}},
                                   PalSampleRateHz());
  ExpectDeltaKernel(filter.kernel());
}

TEST(NoiseSpectrumFilterTest, OnePointTableIsFlat) {
  // Any constant level is a pure shape-free gain, removed by normalisation.
  const NoiseSpectrumFilter filter({{2.5, 12.0}}, PalSampleRateHz());
  ExpectDeltaKernel(filter.kernel());
}

TEST(NoiseSpectrumFilterTest, KernelHasUnitEnergy) {
  const NoiseSpectrumFilter filter({{0.0, 0.0}, {3.5, 4.0}, {6.0, 10.0}},
                                   PalSampleRateHz());
  double energy = 0.0;
  for (double tap : filter.kernel()) {
    energy += tap * tap;
  }
  EXPECT_NEAR(energy, 1.0, 1e-12);
}

TEST(NoiseSpectrumFilterTest, KernelIsLinearPhaseSymmetric) {
  const NoiseSpectrumFilter filter({{0.0, -6.0}, {5.0, 8.0}},
                                   PalSampleRateHz());
  const std::vector<double>& kernel = filter.kernel();
  for (std::size_t n = 0; n < kCentreTap; ++n) {
    EXPECT_NEAR(kernel[n], kernel[kernel.size() - 1U - n], 1e-15);
  }
}

TEST(NoiseSpectrumFilterTest, ResponseFollowsTableAwayFromBreakpoints) {
  const std::vector<NoiseSpectrumPoint> table = {
      {0.0, 0.0}, {3.5, 4.0}, {6.0, 10.0}};
  const double sample_rate_hz = PalSampleRateHz();
  const NoiseSpectrumFilter filter(table, sample_rate_hz);

  // Normalisation sets the absolute gain; compare the shape against a
  // reference frequency.
  constexpr double kReferenceMhz = 1.0;
  const double offset_db =
      ResponseDb(filter.kernel(), kReferenceMhz, sample_rate_hz) -
      InterpolateNoiseSpectrumDb(table, kReferenceMhz);

  for (double frequency_mhz :
       {0.3, 1.5, 2.0, 2.5, 3.0, 4.0, 4.75, 5.5, 6.5, 7.0, 8.0, 8.5}) {
    const double measured =
        ResponseDb(filter.kernel(), frequency_mhz, sample_rate_hz) - offset_db;
    EXPECT_NEAR(measured, InterpolateNoiseSpectrumDb(table, frequency_mhz), 1.0)
        << "at " << frequency_mhz << " MHz";
  }
}

TEST(NoiseSpectrumFilterTest, DesignIsBitIdenticalAcrossCalls) {
  const std::vector<NoiseSpectrumPoint> table = {
      {0.0, 0.0}, {2.0, -20.0}, {7.0, 30.0}};
  const NoiseSpectrumFilter first(table, PalSampleRateHz());
  const NoiseSpectrumFilter second(table, PalSampleRateHz());
  EXPECT_EQ(first.kernel(), second.kernel());
}

TEST(NoiseSpectrumFilterTest, ApplyIsValidConvolution) {
  const NoiseSpectrumFilter filter({{0.0, 0.0}, {6.0, 10.0}},
                                   PalSampleRateHz());
  const std::vector<double>& kernel = filter.kernel();

  // An impulse at the end of the input window reads the kernel out in order
  // across the outputs.
  const std::size_t count = kernel.size();
  std::vector<double> input(count + kernel.size() - 1U, 0.0);
  input[kernel.size() - 1U] = 1.0;
  std::vector<double> output;
  filter.Apply(input, count, &output);
  ASSERT_EQ(output.size(), count);
  for (std::size_t s = 0; s < count; ++s) {
    EXPECT_DOUBLE_EQ(output[s], kernel[kernel.size() - 1U - s]);
  }
}

TEST(NoiseSpectrumFilterTest, ApplyRejectsShortInput) {
  const NoiseSpectrumFilter filter({{0.0, 0.0}}, PalSampleRateHz());
  std::vector<double> output;
  EXPECT_THROW(filter.Apply(std::vector<double>(10, 0.0), 10, &output),
               std::invalid_argument);
}

TEST(NoiseSpectrumFilterTest, RejectsNonPositiveSampleRate) {
  EXPECT_THROW(NoiseSpectrumFilter({{0.0, 0.0}}, 0.0), std::invalid_argument);
}

}  // namespace
}  // namespace videosynth
