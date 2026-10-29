/*
 * File:        test_fm_beat_model.cpp
 * Module:      channel_tests
 * Purpose:     Validates FmBeatModel against the reference beat model
 *              (deherring.py _beat_basis): golden vectors, the beat
 *              frequency of flat colours and the zero-chroma case.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <vector>

#include "fm_beat_reference_vectors.h"
#include "videosynth/fixed_point.h"
#include "videosynth/fm_beat_model.h"
#include "videosynth/model.h"
#include "videosynth/timing_constants.h"

namespace videosynth {
namespace {

constexpr double kPi = 3.14159265358979323846;

double PalSampleRate() {
  return GetTimingConstants(Standard::kPal).sample_rate_4fsc_hz;
}

FmBeatModel MakePalModel() {
  return FmBeatModel(PalSampleRate(), kFmBeatCarrierBlackDefaultHz, 0.0, 700.0);
}

// Raised-cosine envelope: zero outside [start, end), rising and falling over
// ramp samples.
double Envelope(std::size_t n, std::size_t start, std::size_t end,
                std::size_t ramp) {
  if (n < start || n >= end) {
    return 0.0;
  }
  const std::size_t rise = std::min(n - start, end - 1U - n);
  if (rise >= ramp) {
    return 1.0;
  }
  return 0.5 - 0.5 * std::cos(kPi * static_cast<double>(rise) /
                              static_cast<double>(ramp));
}

// The signal of scripts/generate_fm_beat_reference.py (make_signal). Luma
// rises from black at both ends, so the reference's zero-padded FFTs see no
// step there.
void MakeReferenceSignal(std::vector<SampleFixed>* y,
                         std::vector<SampleFixed>* c,
                         PictureLineWindow* window) {
  constexpr std::size_t kCount = 1400;
  constexpr std::size_t kWindowStart = 150;
  constexpr std::size_t kWindowEnd = 1250;
  constexpr std::size_t kRamp = 40;
  constexpr std::size_t kLumaRamp = 100;
  y->resize(kCount);
  c->resize(kCount);
  for (std::size_t n = 0; n < kCount; ++n) {
    const auto t = static_cast<double>(n);
    (*y)[n] = MillivoltsToSampleFixed(
        Envelope(n, 0U, kCount, kLumaRamp) *
        (350.0 +
         150.0 * std::sin(2.0 * kPi * t / static_cast<double>(kCount))));
    (*c)[n] = MillivoltsToSampleFixed(
        200.0 * Envelope(n, kWindowStart, kWindowEnd, kRamp) *
        std::cos((0.5 * kPi * t) + 0.7));
  }
  *window = PictureLineWindow{.start = kWindowStart,
                              .count = kWindowEnd - kWindowStart};
}

// Frequency of a sampled tone, from its lag-one autocorrelation.
double ToneFrequencyHz(const std::vector<double>& signal, std::size_t start,
                       std::size_t end, double sample_rate_hz) {
  double energy = 0.0;
  double neighbours = 0.0;
  for (std::size_t n = start + 1U; n + 1U < end; ++n) {
    energy += signal[n] * signal[n];
    neighbours += signal[n] * (signal[n - 1U] + signal[n + 1U]);
  }
  return sample_rate_hz / (2.0 * kPi) * std::acos(neighbours / (2.0 * energy));
}

TEST(FmBeatModelTest, MatchesReferenceBasis) {
  std::vector<SampleFixed> y;
  std::vector<SampleFixed> c;
  PictureLineWindow window;
  MakeReferenceSignal(&y, &c, &window);
  std::vector<double> basis;
  MakePalModel().ComputeBasis(y.data(), c.data(), y.size(), {window}, &basis);

  const std::size_t start = fm_beat_reference::kCompareStart;
  ASSERT_EQ(std::size(fm_beat_reference::kBasisMv),
            fm_beat_reference::kCompareEnd - start);
  double peak = 0.0;
  for (double value : fm_beat_reference::kBasisMv) {
    peak = std::max(peak, std::abs(value));
  }
  ASSERT_GT(peak, 0.01);
  // The reference forms its analytic chroma and its band with FFT masks; the
  // port uses a Hilbert FIR and a Kaiser-windowed FIR, which agree with them
  // to about 2e-4 of the beat's peak here.
  for (std::size_t i = 0; i < std::size(fm_beat_reference::kBasisMv); ++i) {
    ASSERT_NEAR(basis[start + i], fm_beat_reference::kBasisMv[i], 0.001 * peak)
        << "sample " << start + i;
  }
}

TEST(FmBeatModelTest, FlatColourBeatIsCarrierMinusSubcarrier) {
  // 75% bars luma (BT.601 weights): blue 0.114 * 75 IRE, yellow 0.886 * 75.
  // The reference measures 2.72 MHz on blue and 3.19 MHz on yellow.
  const struct {
    double luma_ire;
    double beat_hz;
  } cases[] = {{0.114 * 75.0, 2.72e6}, {0.886 * 75.0, 3.19e6}};
  constexpr std::size_t kCount = 6000;
  const PictureLineWindow window{.start = 500, .count = 5000};
  const FmBeatModel model = MakePalModel();
  const double fs = PalSampleRate();
  for (const auto& test_case : cases) {
    std::vector<SampleFixed> y(
        kCount, MillivoltsToSampleFixed(7.0 * test_case.luma_ire));
    std::vector<SampleFixed> c(kCount);
    for (std::size_t n = 0; n < kCount; ++n) {
      c[n] = MillivoltsToSampleFixed(
          200.0 * Envelope(n, window.start, window.start + window.count, 40U) *
          std::cos(0.5 * kPi * static_cast<double>(n)));
    }
    std::vector<double> basis;
    model.ComputeBasis(y.data(), c.data(), kCount, {window}, &basis);

    const double measured = ToneFrequencyHz(basis, 1000U, 5000U, fs);
    EXPECT_NEAR(measured, test_case.beat_hz, 20.0e3) << test_case.luma_ire;
    const double predicted = kFmBeatCarrierBlackDefaultHz +
                             (kFmBeatHzPerIre * test_case.luma_ire) -
                             (fs / 4.0);
    EXPECT_NEAR(measured, predicted, 5.0e3) << test_case.luma_ire;
  }
}

TEST(FmBeatModelTest, ZeroChromaGivesExactlyZeroBasis) {
  constexpr std::size_t kCount = 3000;
  std::vector<SampleFixed> y(kCount);
  for (std::size_t n = 0; n < kCount; ++n) {
    y[n] = MillivoltsToSampleFixed(static_cast<double>(n % 700U));
  }
  const std::vector<SampleFixed> c(kCount, 0);
  std::vector<double> basis;
  MakePalModel().ComputeBasis(y.data(), c.data(), kCount,
                              {PictureLineWindow{.start = 100, .count = 2800}},
                              &basis);
  ASSERT_EQ(basis.size(), kCount);
  for (double value : basis) {
    ASSERT_EQ(value, 0.0);
  }
}

TEST(FmBeatModelTest, BasisIsZeroOutsideWindows) {
  std::vector<SampleFixed> y;
  std::vector<SampleFixed> c;
  PictureLineWindow window;
  MakeReferenceSignal(&y, &c, &window);
  std::vector<double> basis;
  MakePalModel().ComputeBasis(y.data(), c.data(), y.size(), {window}, &basis);
  for (std::size_t n = 0; n < basis.size(); ++n) {
    if (n < window.start || n >= window.start + window.count) {
      ASSERT_EQ(basis[n], 0.0) << n;
    }
  }
}

TEST(FmBeatModelTest, EmphasisFiltersHaveUnityDcAndInverseShelves) {
  const double fs = PalSampleRate();
  const FirstOrderIir pre =
      DesignEmphasisFilter(kFmBeatEmphasisSlowS, kFmBeatEmphasisFastS, fs);
  const FirstOrderIir de =
      DesignEmphasisFilter(kFmBeatEmphasisFastS, kFmBeatEmphasisSlowS, fs);
  EXPECT_NEAR(std::abs(pre.Response(0.0, fs)), 1.0, 1e-12);
  EXPECT_NEAR(std::abs(de.Response(0.0, fs)), 1.0, 1e-12);
  // The bilinear transform pre-warps both corners, so the Nyquist gain is
  // the ratio of the warped corners rather than exactly 400 / 100 ns.
  const double warped_ratio =
      std::tan(1.0 / (kFmBeatEmphasisFastS * 2.0 * fs)) /
      std::tan(1.0 / (kFmBeatEmphasisSlowS * 2.0 * fs));
  EXPECT_NEAR(std::abs(pre.Response(fs / 2.0, fs)), warped_ratio, 1e-9);
  EXPECT_NEAR(std::abs(de.Response(fs / 2.0, fs)), 1.0 / warped_ratio, 1e-9);
  for (double f : {1.0e6, 3.0e6, 6.0e6}) {
    EXPECT_NEAR(std::abs(pre.Response(f, fs) * de.Response(f, fs)), 1.0, 1e-12)
        << f;
  }
}

TEST(FmBeatModelTest, LevelScaleNormalisesTheFiftyIreBeat) {
  const FmBeatModel model = MakePalModel();
  const double carrier_50 =
      kFmBeatCarrierBlackDefaultHz + (50.0 * kFmBeatHzPerIre);
  for (double level_db : {-60.0, -20.0, 0.0}) {
    EXPECT_NEAR(model.LevelScale(level_db) * model.FlatColourGain(carrier_50),
                std::pow(10.0, level_db / 20.0), 1e-12);
  }
}

}  // namespace
}  // namespace videosynth
