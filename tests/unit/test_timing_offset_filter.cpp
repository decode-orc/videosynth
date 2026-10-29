/*
 * File:        test_timing_offset_filter.cpp
 * Module:      channel_tests
 * Purpose:     Verifies the band-limited circular fractional delay: integer
 *              shifts, sub-sample phase accuracy, invertibility and error
 *              floor.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "videosynth/fixed_point.h"
#include "videosynth/model.h"
#include "videosynth/timing_constants.h"
#include "videosynth/timing_offset_filter.h"

namespace videosynth {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 2.0 * kPi;
// Buffer length for the tone tests; each tone is placed on a DFT bin so it is
// exactly periodic in the circular buffer.
constexpr std::size_t kToneLength = 8192;

double PalSampleRateHz() {
  return GetTimingConstants(Standard::kPal).sample_rate_4fsc_hz;
}

// Angular frequency (radians/sample) of the DFT bin nearest frequency_mhz.
double BinOmega(double frequency_mhz) {
  const double bin = std::round(frequency_mhz * 1.0e6 / PalSampleRateHz() *
                                static_cast<double>(kToneLength));
  return kTwoPi * bin / static_cast<double>(kToneLength);
}

// Sum of cosines with the given amplitudes (mV) and angular frequencies,
// each delayed by delay_samples (ideal, evaluated analytically).
std::vector<double> TonesMv(const std::vector<double>& omegas,
                            const std::vector<double>& amplitudes_mv,
                            double delay_samples) {
  std::vector<double> tone(kToneLength, 0.0);
  for (std::size_t k = 0; k < omegas.size(); ++k) {
    for (std::size_t n = 0; n < kToneLength; ++n) {
      tone[n] += amplitudes_mv[k] *
                 std::cos(omegas[k] * (static_cast<double>(n) - delay_samples));
    }
  }
  return tone;
}

std::vector<SampleFixed> ToFixed(const std::vector<double>& mv) {
  std::vector<SampleFixed> fixed(mv.size());
  for (std::size_t n = 0; n < mv.size(); ++n) {
    fixed[n] = MillivoltsToSampleFixed(mv[n]);
  }
  return fixed;
}

std::vector<SampleFixed> Delay(const std::vector<SampleFixed>& input,
                               double offset_samples) {
  std::vector<SampleFixed> output(input.size());
  TimingOffsetFilter(offset_samples)
      .Apply(input.data(), input.size(), output.data());
  return output;
}

// Delay of a single tone in samples, measured from its phase against the
// undelayed reference cosine and unwrapped towards expected_samples.
double MeasuredDelay(const std::vector<SampleFixed>& delayed, double omega,
                     double expected_samples) {
  double c = 0.0;
  double s = 0.0;
  for (std::size_t n = 0; n < delayed.size(); ++n) {
    const double value = SampleFixedToMillivolts(delayed[n]);
    c += value * std::cos(omega * static_cast<double>(n));
    s += value * std::sin(omega * static_cast<double>(n));
  }
  const double phase = std::atan2(s, c);
  const double residual =
      std::remainder(phase - (omega * expected_samples), kTwoPi);
  return expected_samples + (residual / omega);
}

// Power ratio, in dB, of (measured - reference) to reference.
double ErrorDb(const std::vector<SampleFixed>& measured,
               const std::vector<double>& reference_mv) {
  double error = 0.0;
  double signal = 0.0;
  for (std::size_t n = 0; n < measured.size(); ++n) {
    const double diff = SampleFixedToMillivolts(measured[n]) - reference_mv[n];
    error += diff * diff;
    signal += reference_mv[n] * reference_mv[n];
  }
  return 10.0 * std::log10(error / signal);
}

TEST(TimingOffsetFilterTest, SplitsOffsetIntoFloorAndFraction) {
  const TimingOffsetFilter negative(-0.3);
  EXPECT_EQ(negative.integer_shift(), -1);
  EXPECT_NEAR(negative.fraction(), 0.7, 1e-12);

  const TimingOffsetFilter positive(2.25);
  EXPECT_EQ(positive.integer_shift(), 2);
  EXPECT_DOUBLE_EQ(positive.fraction(), 0.25);
  ASSERT_EQ(positive.kernel().size(), 2U * kTimingOffsetHalfTaps);

  double dc_gain = 0.0;
  for (double tap : positive.kernel()) {
    dc_gain += tap;
  }
  EXPECT_NEAR(dc_gain, 1.0, 1e-12);

  EXPECT_TRUE(TimingOffsetFilter(-3.0).kernel().empty());
}

TEST(TimingOffsetFilterTest, IntegerOffsetIsExactCircularShift) {
  std::vector<SampleFixed> input(101);
  for (std::size_t n = 0; n < input.size(); ++n) {
    input[n] = static_cast<SampleFixed>((n * 7919U) % 1009U) - 500;
  }
  for (int shift : {1, 3, -5, 100, -101}) {
    const std::vector<SampleFixed> output =
        Delay(input, static_cast<double>(shift));
    for (std::size_t n = 0; n < input.size(); ++n) {
      const auto length = static_cast<std::int64_t>(input.size());
      const std::int64_t source =
          (((static_cast<std::int64_t>(n) - shift) % length) + length) % length;
      ASSERT_EQ(output[n], input[static_cast<std::size_t>(source)])
          << "shift " << shift << " sample " << n;
    }
  }
}

TEST(TimingOffsetFilterTest, ToneDelayIsAccurateFrom1To6Mhz) {
  for (double offset : {0.3, -0.3, 0.5, 2.73, -4.1}) {
    for (double frequency_mhz : {1.0, 2.0, 3.0, 4.0, 4.43, 5.0, 6.0}) {
      const double omega = BinOmega(frequency_mhz);
      const std::vector<SampleFixed> delayed =
          Delay(ToFixed(TonesMv({omega}, {300.0}, 0.0)), offset);
      EXPECT_NEAR(MeasuredDelay(delayed, omega, offset), offset, 0.01)
          << frequency_mhz << " MHz, offset " << offset;
    }
  }
}

TEST(TimingOffsetFilterTest, ForwardThenBackwardRestoresBandLimitedSignal) {
  const std::vector<double> omegas = {BinOmega(0.5), BinOmega(2.0),
                                      BinOmega(3.58), BinOmega(4.43),
                                      BinOmega(6.0)};
  const std::vector<double> amplitudes(omegas.size(), 100.0);
  const std::vector<double> original = TonesMv(omegas, amplitudes, 0.0);
  for (double offset : {0.3, 0.5, 1.7}) {
    const std::vector<SampleFixed> restored =
        Delay(Delay(ToFixed(original), offset), -offset);
    // Each pass contributes an error below -76 dB up to 6 MHz.
    EXPECT_LT(ErrorDb(restored, original), -70.0) << "offset " << offset;
  }
}

TEST(TimingOffsetFilterTest, FiveMhzToneHasNoProductsAboveMinus60Db) {
  const double omega = BinOmega(5.0);
  for (double offset : {0.25, 0.5, -0.3}) {
    const std::vector<SampleFixed> delayed =
        Delay(ToFixed(TonesMv({omega}, {300.0}, 0.0)), offset);
    EXPECT_LT(ErrorDb(delayed, TonesMv({omega}, {300.0}, offset)), -60.0)
        << "offset " << offset;
  }
}

TEST(TimingOffsetFilterTest, FlatLevelPassesUnchanged) {
  const std::vector<SampleFixed> input(64, MillivoltsToSampleFixed(-300.0));
  EXPECT_EQ(Delay(input, 0.37), input);
}

}  // namespace
}  // namespace videosynth
