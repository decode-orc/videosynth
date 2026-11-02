/*
 * File:        test_replay_channel_filter.cpp
 * Module:      channel_tests
 * Purpose:     Verifies the LaserVision replay channel FIR: the group delay
 *              follows the IEC 60856 curve, the amplitude follows the replay
 *              response, flat levels keep their DC gain, the identity is a
 *              unit tap, and responses interpolate by picture number.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <cmath>
#include <complex>
#include <cstddef>
#include <vector>

#include "videosynth/fixed_point.h"
#include "videosynth/model.h"
#include "videosynth/noise_spectrum_filter.h"
#include "videosynth/replay_channel_filter.h"
#include "videosynth/timing_constants.h"

namespace videosynth {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kCentre = (kReplayChannelTaps - 1) / 2;

double PalRateHz() {
  return GetTimingConstants(Standard::kPal).sample_rate_4fsc_hz;
}

// Frequency response of the kernel, h[m] for m = -kCentre..kCentre, at
// frequency_mhz.
std::complex<double> Response(const ReplayChannelFilter& filter,
                              double frequency_mhz) {
  const double omega = 2.0 * kPi * frequency_mhz * 1.0e6 / PalRateHz();
  std::complex<double> sum = 0.0;
  const std::vector<double>& kernel = filter.kernel();
  for (std::size_t i = 0; i < kernel.size(); ++i) {
    const double m = static_cast<double>(i) - kCentre;
    sum += kernel[i] * std::polar(1.0, -omega * m);
  }
  return sum;
}

// Group delay in ns at frequency_mhz, from the phase slope over +-10 kHz.
double GroupDelayNs(const ReplayChannelFilter& filter, double frequency_mhz) {
  constexpr double kStepMhz = 0.01;
  const double phase = std::arg(Response(filter, frequency_mhz + kStepMhz) /
                                Response(filter, frequency_mhz - kStepMhz));
  return -phase / (2.0 * kPi * 2.0 * kStepMhz * 1.0e6) * 1.0e9;
}

double AmplitudeDb(const ReplayChannelFilter& filter, double frequency_mhz) {
  return 20.0 * std::log10(std::abs(Response(filter, frequency_mhz)));
}

ReplayChannelFilter IecGroupDelayFilter(double scale) {
  const std::vector<GroupDelayPoint> curve = Iec60856NominalGroupDelay();
  return ReplayChannelFilter([](double) { return 0.0; },
                             [&curve, scale](double mhz) {
                               return scale *
                                      InterpolateGroupDelayNs(curve, mhz);
                             },
                             PalRateHz());
}

TEST(ReplayChannelFilterTest, GroupDelayFollowsTheIec60856Curve) {
  const ReplayChannelFilter filter = IecGroupDelayFilter(1.0);
  const std::vector<GroupDelayPoint> curve = Iec60856NominalGroupDelay();
  for (double mhz : {0.3, 1.0, 2.0, 2.5, 3.0, 3.5, 4.0, 4.2, 4.43, 4.6}) {
    EXPECT_NEAR(GroupDelayNs(filter, mhz), InterpolateGroupDelayNs(curve, mhz),
                3.0)
        << mhz << " MHz";
  }
  // An all-pass: the amplitude stays flat across the video band.
  for (double mhz : {0.0, 1.0, 3.0, 4.43, 5.0, 5.5}) {
    EXPECT_NEAR(AmplitudeDb(filter, mhz), 0.0, 0.02) << mhz << " MHz";
  }
}

TEST(ReplayChannelFilterTest, ScaleScalesTheGroupDelay) {
  const ReplayChannelFilter half = IecGroupDelayFilter(0.5);
  const ReplayChannelFilter inverse = IecGroupDelayFilter(-1.0);
  EXPECT_NEAR(GroupDelayNs(half, 4.43), -67.5, 2.0);
  EXPECT_NEAR(GroupDelayNs(inverse, 4.43), 135.0, 3.0);
}

TEST(ReplayChannelFilterTest, AmplitudeFollowsTheResponseWithLinearPhase) {
  const std::vector<NoiseSpectrumPoint> response = {
      {0.0, 0.0}, {3.0, -0.5}, {4.4, -2.0}, {5.0, -6.0}, {5.5, -20.0}};
  const ReplayChannelFilter filter(
      [&response](double mhz) {
        return InterpolateNoiseSpectrumDb(response, mhz);
      },
      [](double) { return 0.0; }, PalRateHz());
  // Away from the breakpoints the Hann-windowed design follows the table to
  // 0.1 dB; at a breakpoint the window rounds the corner slightly more.
  for (double mhz : {0.5, 2.0, 3.0, 3.7, 4.7}) {
    EXPECT_NEAR(AmplitudeDb(filter, mhz),
                InterpolateNoiseSpectrumDb(response, mhz), 0.1)
        << mhz << " MHz";
  }
  EXPECT_NEAR(AmplitudeDb(filter, 4.4), -2.0, 0.15);
  for (double mhz : {1.0, 3.0, 4.43}) {
    EXPECT_NEAR(GroupDelayNs(filter, mhz), 0.0, 0.5) << mhz << " MHz";
  }
}

TEST(ReplayChannelFilterTest, DcGainIsExactlyTheResponseAtZero) {
  const ReplayChannelFilter filter(
      [](double mhz) { return mhz < 2.0 ? -1.0 : -3.0; },
      [](double mhz) { return -20.0 * mhz; }, PalRateHz());
  double dc = 0.0;
  for (double tap : filter.kernel()) {
    dc += tap;
  }
  EXPECT_NEAR(dc, std::pow(10.0, -1.0 / 20.0), 1e-12);
}

TEST(ReplayChannelFilterTest, IdentityIsAUnitTap) {
  const ReplayChannelFilter filter([](double) { return 0.0; },
                                   [](double) { return 0.0; }, PalRateHz());
  const std::vector<double>& kernel = filter.kernel();
  ASSERT_EQ(kernel.size(), static_cast<std::size_t>(kReplayChannelTaps));
  for (std::size_t i = 0; i < kernel.size(); ++i) {
    EXPECT_NEAR(kernel[i], i == static_cast<std::size_t>(kCentre) ? 1.0 : 0.0,
                1e-12)
        << i;
  }

  std::vector<SampleFixed> frame(1000);
  for (std::size_t n = 0; n < frame.size(); ++n) {
    frame[n] = MillivoltsToSampleFixed(std::sin(0.37 * static_cast<double>(n)) *
                                       300.0);
  }
  std::vector<SampleFixed> filtered = frame;
  filter.Apply(filtered.data(), filtered.size());
  EXPECT_EQ(filtered, frame);
}

TEST(ReplayChannelFilterTest, ApplyIsCircularAndKeepsFlatLevels) {
  const ReplayChannelFilter filter = IecGroupDelayFilter(1.0);
  std::vector<SampleFixed> flat(600, MillivoltsToSampleFixed(-300.0));
  filter.Apply(flat.data(), flat.size());
  for (SampleFixed sample : flat) {
    ASSERT_EQ(sample, MillivoltsToSampleFixed(-300.0));
  }

  // An impulse at sample 0 spreads over both ends of the frame as the kernel
  // does about its centre.
  std::vector<SampleFixed> impulse(600, 0);
  impulse[0] = MillivoltsToSampleFixed(100.0);
  filter.Apply(impulse.data(), impulse.size());
  const std::vector<double>& kernel = filter.kernel();
  for (int m = -kCentre; m <= kCentre; ++m) {
    const std::size_t n = static_cast<std::size_t>((m + 600) % 600);
    EXPECT_NEAR(SampleFixedToMillivolts(impulse[n]),
                100.0 * kernel[static_cast<std::size_t>(m + kCentre)], 1e-5)
        << m;
  }
}

TEST(ReplayChannelFilterTest, ResponseInterpolatesByPictureNumber) {
  LaserdiscChannelParameters channel;
  channel.response_by_picture = {{1, {{0.0, 0.0}, {5.0, -4.0}}},
                                 {101, {{0.0, 0.0}, {5.0, -8.0}}}};
  EXPECT_DOUBLE_EQ(ReplayResponseDb(channel, 1.0, 5.0), -4.0);
  EXPECT_DOUBLE_EQ(ReplayResponseDb(channel, 51.0, 5.0), -6.0);
  EXPECT_DOUBLE_EQ(ReplayResponseDb(channel, 101.0, 5.0), -8.0);
  // The end entries are held beyond them.
  EXPECT_DOUBLE_EQ(ReplayResponseDb(channel, 0.0, 5.0), -4.0);
  EXPECT_DOUBLE_EQ(ReplayResponseDb(channel, 5000.0, 5.0), -8.0);
  EXPECT_DOUBLE_EQ(ReplayResponseDb(channel, 51.0, 2.5), -3.0);

  LaserdiscChannelParameters fixed;
  fixed.response_db = {{0.0, 0.0}, {4.0, -2.0}};
  EXPECT_DOUBLE_EQ(ReplayResponseDb(fixed, 999.0, 2.0), -1.0);
  EXPECT_DOUBLE_EQ(ReplayResponseDb(LaserdiscChannelParameters{}, 1.0, 2.0),
                   0.0);
}

TEST(ReplayChannelFilterTest, GroupDelayCurveInterpolatesAndHoldsItsEnds) {
  const std::vector<GroupDelayPoint> curve = Iec60856NominalGroupDelay();
  EXPECT_DOUBLE_EQ(InterpolateGroupDelayNs(curve, 0.1), 0.0);
  EXPECT_DOUBLE_EQ(InterpolateGroupDelayNs(curve, 2.5), -22.5);
  EXPECT_DOUBLE_EQ(InterpolateGroupDelayNs(curve, 4.43), -135.0);
  EXPECT_DOUBLE_EQ(InterpolateGroupDelayNs(curve, 7.0), -200.0);
  EXPECT_DOUBLE_EQ(InterpolateGroupDelayNs({}, 3.0), 0.0);
}

}  // namespace
}  // namespace videosynth
