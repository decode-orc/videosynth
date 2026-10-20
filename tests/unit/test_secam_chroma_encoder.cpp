/*
 * File:        test_secam_chroma_encoder.cpp
 * Module:      chroma_encoder_tests
 * Purpose:     Validates the 625-line SECAM FM chroma encoder against ITU-R
 * BT.1700 Annex 1 Part C.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 itewreed
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

#include "videosynth/fixed_point.h"
#include "videosynth/secam_chroma_encoder.h"

namespace videosynth {
namespace {

// EBU Tech. 3280-E Section 1.2: 4fsc PAL sampling, on which SECAM is carried.
constexpr double kSampleRateHz = 17734475.0;
constexpr int kLineSamples = 1135;
constexpr int kActiveStart = 177;
constexpr int kActiveSamples = 922;

constexpr double kForHz = 4406250.0;
constexpr double kFobHz = 4250000.0;

int SecondsToSample(double seconds) {
  return static_cast<int>(std::lround(seconds * kSampleRateHz));
}

// Mean frequency over [first, last) from linearly interpolated zero crossings.
double MeasureFrequencyHz(const std::vector<SampleFixed>& line, int first,
                          int last) {
  double first_crossing = -1.0;
  double last_crossing = -1.0;
  int crossings = 0;
  for (int i = first + 1; i < last; ++i) {
    const double a = SampleFixedToMillivolts(line[i - 1]);
    const double b = SampleFixedToMillivolts(line[i]);
    if ((a < 0.0 && b >= 0.0) || (a >= 0.0 && b < 0.0)) {
      const double crossing = (i - 1) + (a / (a - b));
      if (first_crossing < 0.0) {
        first_crossing = crossing;
      }
      last_crossing = crossing;
      ++crossings;
    }
  }
  if (crossings < 2) {
    return 0.0;
  }
  const double half_cycles = static_cast<double>(crossings - 1);
  return (half_cycles / 2.0) /
         ((last_crossing - first_crossing) / kSampleRateHz);
}

double PeakMillivolts(const std::vector<SampleFixed>& line, int first,
                      int last) {
  double peak = 0.0;
  for (int i = first; i < last; ++i) {
    peak = std::max(peak, std::abs(SampleFixedToMillivolts(line[i])));
  }
  return peak;
}

std::vector<YCbCr444Pixel> UniformActiveLine(int cb, int cr) {
  return std::vector<YCbCr444Pixel>(
      kActiveSamples, YCbCr444Pixel{.y = 512,
                                    .cb = static_cast<std::int16_t>(cb),
                                    .cr = static_cast<std::int16_t>(cr)});
}

TEST(SecamLineSequenceTest, FollowsFigure16ComponentOrder) {
  // ITU-R BT.1700 Annex 1 Part C Figure 16.
  EXPECT_EQ(SecamComponentForLine(7, 0), SecamLineComponent::kDr);
  EXPECT_EQ(SecamComponentForLine(8, 0), SecamLineComponent::kDb);
  EXPECT_EQ(SecamComponentForLine(320, 0), SecamLineComponent::kDb);
  EXPECT_EQ(SecamComponentForLine(321, 0), SecamLineComponent::kDr);
  EXPECT_EQ(SecamComponentForLine(7, 1), SecamLineComponent::kDb);
  EXPECT_EQ(SecamComponentForLine(8, 1), SecamLineComponent::kDr);
  EXPECT_EQ(SecamComponentForLine(320, 1), SecamLineComponent::kDr);
}

TEST(SecamLineSequenceTest, AlternatesContinuouslyAcrossFrameBoundary) {
  // Line 625 of one frame and line 1 of the next are consecutive lines.
  EXPECT_NE(SecamComponentForLine(625, 0), SecamComponentForLine(1, 1));
  EXPECT_NE(SecamComponentForLine(625, 1), SecamComponentForLine(1, 0));
}

TEST(SecamLineSequenceTest, InitialPhaseFollowsZeroZeroPiPattern) {
  // ITU-R BT.1700 Annex 1 Part C Table 4 Note 1.
  EXPECT_DOUBLE_EQ(SecamInitialPhaseRadForLine(1, 0), 0.0);
  EXPECT_DOUBLE_EQ(SecamInitialPhaseRadForLine(2, 0), 0.0);
  EXPECT_DOUBLE_EQ(SecamInitialPhaseRadForLine(3, 0), M_PI);
  EXPECT_DOUBLE_EQ(SecamInitialPhaseRadForLine(4, 0), 0.0);
  EXPECT_DOUBLE_EQ(SecamInitialPhaseRadForLine(1, 1), M_PI);
  EXPECT_DOUBLE_EQ(SecamInitialPhaseRadForLine(3, 1), 0.0);
}

TEST(SecamLineSequenceTest, IdentificationLinesAre7To15And320To328) {
  EXPECT_FALSE(IsSecamFieldIdentificationLine(6));
  EXPECT_TRUE(IsSecamFieldIdentificationLine(7));
  EXPECT_TRUE(IsSecamFieldIdentificationLine(15));
  EXPECT_FALSE(IsSecamFieldIdentificationLine(16));
  EXPECT_FALSE(IsSecamFieldIdentificationLine(319));
  EXPECT_TRUE(IsSecamFieldIdentificationLine(320));
  EXPECT_TRUE(IsSecamFieldIdentificationLine(328));
  EXPECT_FALSE(IsSecamFieldIdentificationLine(329));
}

TEST(SecamChromaEncoderTest, NeutralLineCarriesUndeviatedSubcarriers) {
  const SecamChromaEncoder encoder(kSampleRateHz);
  std::vector<SampleFixed> line;
  const int first = SecondsToSample(7.0e-6);
  const int last = kLineSamples - SecondsToSample(3.0e-6);

  encoder.EncodeLine(SecamLineComponent::kDr, 0.0, UniformActiveLine(512, 512),
                     kActiveStart, kLineSamples, &line);
  ASSERT_EQ(line.size(), static_cast<std::size_t>(kLineSamples));
  EXPECT_NEAR(MeasureFrequencyHz(line, first, last), kForHz, 2.0e3);

  encoder.EncodeLine(SecamLineComponent::kDb, 0.0, UniformActiveLine(512, 512),
                     kActiveStart, kLineSamples, &line);
  EXPECT_NEAR(MeasureFrequencyHz(line, first, last), kFobHz, 2.0e3);
}

TEST(SecamChromaEncoderTest, SubcarrierBlankedDuringLineBlanking) {
  // ITU-R BT.470-6 Table 2 item 2.17a: blanked up to 5.6 µs after 0H and
  // from the leading edge of line blanking (front porch, 1.5 µs before 0H).
  const SecamChromaEncoder encoder(kSampleRateHz);
  std::vector<SampleFixed> line;
  encoder.EncodeLine(SecamLineComponent::kDb, 0.0, UniformActiveLine(512, 512),
                     kActiveStart, kLineSamples, &line);

  const int gate_start = SecondsToSample(5.6e-6);
  const int gate_end = kLineSamples - SecondsToSample(1.5e-6);
  EXPECT_EQ(PeakMillivolts(line, 0, gate_start), 0.0);
  EXPECT_EQ(PeakMillivolts(line, gate_end, kLineSamples), 0.0);
  EXPECT_GT(PeakMillivolts(line, gate_start, gate_start + 20), 10.0);
}

TEST(SecamChromaEncoderTest, BellFilterSetsSubcarrierAmplitude) {
  // ITU-R BT.1700 Annex 1 Part C Table 4 items 10d/10g: 2·M0 = 23 % of
  // 700 mV at f0 = 4.286 MHz, scaled by |1 + j16F| / |1 + j1.26F|:
  //   f_OB: F = −0.01692 → gain 1.0360 → 83.40 mV peak.
  //   f_OR: F = +0.05526 → gain 1.3343 → 107.41 mV peak.
  const SecamChromaEncoder encoder(kSampleRateHz);
  std::vector<SampleFixed> line;
  const int first = SecondsToSample(20.0e-6);
  const int last = SecondsToSample(60.0e-6);

  encoder.EncodeLine(SecamLineComponent::kDb, 0.0, UniformActiveLine(512, 512),
                     kActiveStart, kLineSamples, &line);
  EXPECT_NEAR(PeakMillivolts(line, first, last), 83.40, 1.5);

  encoder.EncodeLine(SecamLineComponent::kDr, 0.0, UniformActiveLine(512, 512),
                     kActiveStart, kLineSamples, &line);
  EXPECT_NEAR(PeakMillivolts(line, first, last), 107.41, 2.0);
}

TEST(SecamChromaEncoderTest, InitialPhaseInvertsSubcarrier) {
  const SecamChromaEncoder encoder(kSampleRateHz);
  std::vector<SampleFixed> normal;
  std::vector<SampleFixed> inverted;
  encoder.EncodeLine(SecamLineComponent::kDr, 0.0, UniformActiveLine(512, 512),
                     kActiveStart, kLineSamples, &normal);
  encoder.EncodeLine(SecamLineComponent::kDr, M_PI, UniformActiveLine(512, 512),
                     kActiveStart, kLineSamples, &inverted);
  for (int i = 0; i < kLineSamples; ++i) {
    EXPECT_NEAR(SampleFixedToMillivolts(normal[i]),
                -SampleFixedToMillivolts(inverted[i]), 1e-6);
  }
}

TEST(SecamChromaEncoderTest, BlueDeviatesDbLineUpwards) {
  // Cb = 960 → (B−Y) = 0.886 → D'B = 1.505 × 0.886 = 1.3334. The settled
  // deviation is ΔfOB × D'B = 230 kHz × 1.3334 = 306.7 kHz (item 10e).
  const SecamChromaEncoder encoder(kSampleRateHz);
  std::vector<SampleFixed> line;
  encoder.EncodeLine(SecamLineComponent::kDb, 0.0, UniformActiveLine(960, 512),
                     kActiveStart, kLineSamples, &line);
  const int first = SecondsToSample(50.0e-6);
  const int last = SecondsToSample(60.0e-6);
  EXPECT_NEAR(MeasureFrequencyHz(line, first, last), kFobHz + 306.7e3, 15.0e3);
}

TEST(SecamChromaEncoderTest, RedDeviatesDrLineDownwards) {
  // Cr = 960 → (R−Y) = 0.701 → D'R = −1.902 × 0.701 = −1.3333. The settled
  // deviation is ΔfOR × D'R = 280 kHz × −1.3333 = −373.3 kHz (item 10e).
  const SecamChromaEncoder encoder(kSampleRateHz);
  std::vector<SampleFixed> line;
  encoder.EncodeLine(SecamLineComponent::kDr, 0.0, UniformActiveLine(512, 960),
                     kActiveStart, kLineSamples, &line);
  const int first = SecondsToSample(50.0e-6);
  const int last = SecondsToSample(60.0e-6);
  EXPECT_NEAR(MeasureFrequencyHz(line, first, last), kForHz - 373.3e3, 15.0e3);
}

TEST(SecamChromaEncoderTest, PrecorrectionOvershootIsLimitedToMaxDeviation) {
  // Item 10f: the low-frequency precorrection boosts a step up to 3×, so the
  // red step at the start of the picture is clipped at the −506 kHz limit of
  // item 10e and the overshoot exceeds the settled deviation.
  const SecamChromaEncoder encoder(kSampleRateHz);
  std::vector<SampleFixed> line;
  encoder.EncodeLine(SecamLineComponent::kDr, 0.0, UniformActiveLine(512, 960),
                     kActiveStart, kLineSamples, &line);
  const int step = kActiveStart + 10;
  const double overshoot_hz = MeasureFrequencyHz(line, step, step + 30);
  EXPECT_LT(overshoot_hz, kForHz - 420.0e3);
  EXPECT_GT(overshoot_hz, kForHz - 506.0e3 - 15.0e3);
}

TEST(SecamChromaEncoderTest, IdentificationLinesReachMaximumDeviation) {
  // ITU-R BT.470-6 Table 2 item 2.18: maximum deviation during identification
  // is +350 kHz for D'R lines and −350 kHz for D'B lines.
  const SecamChromaEncoder encoder(kSampleRateHz);
  std::vector<SampleFixed> line;
  const int first = SecondsToSample(40.0e-6);
  const int last = SecondsToSample(60.0e-6);
  const int porch_first = SecondsToSample(6.5e-6);
  const int porch_last = SecondsToSample(10.5e-6);

  encoder.EncodeIdentificationLine(SecamLineComponent::kDr, 0.0, kLineSamples,
                                   &line);
  EXPECT_NEAR(MeasureFrequencyHz(line, first, last), kForHz + 350.0e3, 5.0e3);
  EXPECT_NEAR(MeasureFrequencyHz(line, porch_first, porch_last), kForHz,
              20.0e3);

  encoder.EncodeIdentificationLine(SecamLineComponent::kDb, 0.0, kLineSamples,
                                   &line);
  EXPECT_NEAR(MeasureFrequencyHz(line, first, last), kFobHz - 350.0e3, 5.0e3);
  EXPECT_NEAR(MeasureFrequencyHz(line, porch_first, porch_last), kFobHz,
              20.0e3);
}

TEST(SecamChromaEncoderTest, EmptyActiveLineEncodesUndeviatedSubcarrier) {
  // A colourless line (no source pixels) is the undeviated subcarrier
  // throughout the gate; the OSD renderer writes it under SECAM text. Used
  // through the interface, as the generation stage does.
  const SecamChromaEncoder concrete(kSampleRateHz);
  const ISecamChromaEncoder& encoder = concrete;
  std::vector<SampleFixed> empty_line;
  std::vector<SampleFixed> neutral_line;
  encoder.EncodeLine(SecamLineComponent::kDb, 0.0, {}, 0, kLineSamples,
                     &empty_line);
  encoder.EncodeLine(SecamLineComponent::kDb, 0.0, UniformActiveLine(512, 512),
                     kActiveStart, kLineSamples, &neutral_line);

  ASSERT_EQ(empty_line.size(), static_cast<std::size_t>(kLineSamples));
  EXPECT_NEAR(MeasureFrequencyHz(empty_line, SecondsToSample(7.0e-6),
                                 kLineSamples - SecondsToSample(3.0e-6)),
              kFobHz, 2.0e3);
  EXPECT_EQ(empty_line, neutral_line);
}

TEST(SecamChromaEncoderTest, RejectsNullOutput) {
  const SecamChromaEncoder encoder(kSampleRateHz);
  EXPECT_THROW(encoder.EncodeLine(SecamLineComponent::kDr, 0.0,
                                  UniformActiveLine(512, 512), kActiveStart,
                                  kLineSamples, nullptr),
               std::invalid_argument);
}

}  // namespace
}  // namespace videosynth
