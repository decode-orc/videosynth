/*
 * File:        test_chroma_encoder.cpp
 * Module:      chroma_encoder_tests
 * Purpose:     Validates standard-specific chroma encoder dispatch and neutral
 * behavior.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "videosynth/chroma_encoder.h"
#include "videosynth/fixed_point.h"
#include "videosynth/timing_constants.h"

namespace videosynth {
namespace {

double RootMeanSquare(const std::vector<SampleFixed>& values) {
  double square_sum = 0.0;
  for (SampleFixed value : values) {
    const double value_mv = SampleFixedToMillivolts(value);
    square_sum += value_mv * value_mv;
  }
  return values.empty()
             ? 0.0
             : std::sqrt(square_sum / static_cast<double>(values.size()));
}

std::vector<YCbCr444Pixel> MakeCbSinusoidLine(std::size_t sample_count,
                                              double amplitude_norm,
                                              double cycles_per_sample) {
  std::vector<YCbCr444Pixel> line(
      sample_count, YCbCr444Pixel{.y = 512, .cb = 512, .cr = 512});
  for (std::size_t index = 0; index < sample_count; ++index) {
    const double angle =
        2.0 * M_PI * cycles_per_sample * static_cast<double>(index);
    const double cb_norm = amplitude_norm * std::sin(angle);
    line[index].cb =
        static_cast<std::int16_t>(std::lround(512.0 + (448.0 * cb_norm)));
  }
  return line;
}

std::vector<YCbCr444Pixel> MakeCrSinusoidLine(std::size_t sample_count,
                                              double amplitude_norm,
                                              double cycles_per_sample) {
  std::vector<YCbCr444Pixel> line(
      sample_count, YCbCr444Pixel{.y = 512, .cb = 512, .cr = 512});
  for (std::size_t index = 0; index < sample_count; ++index) {
    const double angle =
        2.0 * M_PI * cycles_per_sample * static_cast<double>(index);
    const double cr_norm = amplitude_norm * std::sin(angle);
    line[index].cr =
        static_cast<std::int16_t>(std::lround(512.0 + (448.0 * cr_norm)));
  }
  return line;
}

TEST(ChromaEncoderTest, NeutralChromaProducesNoSubcarrierEnergy) {
  const std::vector<YCbCr444Pixel> neutral_line(
      64, YCbCr444Pixel{.y = 512, .cb = 512, .cr = 512});
  const auto pal = CreateChromaEncoder(
      Standard::kPal, GetTimingConstants(Standard::kPal).sample_rate_4fsc_hz);
  const auto ntsc = CreateChromaEncoder(
      Standard::kNtsc, GetTimingConstants(Standard::kNtsc).sample_rate_4fsc_hz);
  std::vector<SampleFixed> pal_output;
  std::vector<SampleFixed> ntsc_output;

  ASSERT_NE(pal, nullptr);
  ASSERT_NE(ntsc, nullptr);

  pal->EncodeLineFromPhaseStart(neutral_line, 0.0, &pal_output);
  ntsc->EncodeLineFromPhaseStart(neutral_line, 0.0, &ntsc_output);

  for (SampleFixed value : pal_output) {
    EXPECT_NEAR(SampleFixedToMillivolts(value), 0.0, 1e-12);
  }
  for (SampleFixed value : ntsc_output) {
    EXPECT_NEAR(SampleFixedToMillivolts(value), 0.0, 1e-12);
  }
}

TEST(ChromaEncoderTest, NtscCrAxisIsScaledLargerThanCbAxisPerSmpte170M) {
  // SMPTE 170M-2004 Annex A eqs (4)/(5)/(10):
  //   Cb (b-y) axis peak scale = 0.925 × 0.492111 × 0.886 × 100 × (1000/140)
  //   Cr (r-y) axis peak scale = 0.925 × 0.877283 × 0.701 × 100 × (1000/140)
  // Cr/Cb ratio ≈ 1.411 at equal cb_norm / cr_norm input magnitudes.
  const TimingConstants ntsc_timing = GetTimingConstants(Standard::kNtsc);
  const auto ntsc =
      CreateChromaEncoder(Standard::kNtsc, ntsc_timing.sample_rate_4fsc_hz);
  ASSERT_NE(ntsc, nullptr);

  // Full positive excursion on each axis in isolation (phase chosen to land
  // the axis squarely on the carrier at sample 0).
  const std::vector<YCbCr444Pixel> max_cb_line(
      64, YCbCr444Pixel{.y = 512, .cb = 960, .cr = 512});
  const std::vector<YCbCr444Pixel> max_cr_line(
      64, YCbCr444Pixel{.y = 512, .cb = 512, .cr = 960});

  std::vector<SampleFixed> cb_output;
  std::vector<SampleFixed> cr_output;
  ntsc->EncodeLineFromPhaseStart(max_cb_line, M_PI / 2.0, &cb_output);
  ntsc->EncodeLineFromPhaseStart(max_cr_line, 0.0, &cr_output);

  const double cb_peak = SampleFixedToMillivolts(cb_output[0]);
  const double cr_peak = SampleFixedToMillivolts(cr_output[0]);

  // Both axes must have non-trivial amplitude.
  EXPECT_GT(std::abs(cb_peak), 100.0);
  EXPECT_GT(std::abs(cr_peak), 100.0);

  // Cr peak must be ~41 % larger than Cb peak (ratio ≈ 1.411, ±5 %).
  const double expected_ratio = (0.877283 * 0.701) / (0.492111 * 0.886);
  EXPECT_NEAR(std::abs(cr_peak) / std::abs(cb_peak), expected_ratio,
              expected_ratio * 0.05);
}

TEST(ChromaEncoderTest,
     PalLowPassAttenuatesHighFrequencyChromaMoreThanLowFrequency) {
  const TimingConstants pal_timing = GetTimingConstants(Standard::kPal);
  const auto pal =
      CreateChromaEncoder(Standard::kPal, pal_timing.sample_rate_4fsc_hz);
  std::vector<SampleFixed> low_output;
  std::vector<SampleFixed> high_output;

  ASSERT_NE(pal, nullptr);

  const auto low_line = MakeCbSinusoidLine(256, 0.35, 0.01);
  const auto high_line = MakeCbSinusoidLine(256, 0.35, 0.18);

  // PAL uses E'U on the sin() axis, so use +90 deg carrier phase to isolate Cb.
  pal->EncodeLineFromPhaseStart(low_line, M_PI / 2.0, &low_output);
  pal->EncodeLineFromPhaseStart(high_line, M_PI / 2.0, &high_output);

  EXPECT_GT(RootMeanSquare(low_output), RootMeanSquare(high_output) * 3.0);
}

TEST(ChromaEncoderTest, NtscCbAndCrAxesUseSymmetricBandwidth) {
  // Both NTSC axes share the same low-pass filter (kNtscCbCrCutoffHz), so a
  // sinusoid of the same normalised frequency and amplitude should be
  // attenuated identically on each axis. The output RMS ratio must therefore
  // equal the per-axis voltage scale ratio (Cr/Cb ≈ 1.411) within ±10 %.
  // SMPTE 170M-2004 Annex A eqs (4)/(5)/(10).
  const TimingConstants ntsc_timing = GetTimingConstants(Standard::kNtsc);
  const auto ntsc =
      CreateChromaEncoder(Standard::kNtsc, ntsc_timing.sample_rate_4fsc_hz);
  std::vector<SampleFixed> cb_output;
  std::vector<SampleFixed> cr_output;

  ASSERT_NE(ntsc, nullptr);

  const auto cb_line = MakeCbSinusoidLine(256, 0.35, 0.06);
  const auto cr_line = MakeCrSinusoidLine(256, 0.35, 0.06);

  ntsc->EncodeLineFromPhaseStart(cb_line, M_PI / 2.0, &cb_output);
  ntsc->EncodeLineFromPhaseStart(cr_line, 0.0, &cr_output);

  const double cb_rms = RootMeanSquare(cb_output);
  const double cr_rms = RootMeanSquare(cr_output);
  EXPECT_GT(cb_rms, 1.0);
  EXPECT_GT(cr_rms, 1.0);

  const double expected_ratio = (0.877283 * 0.701) / (0.492111 * 0.886);
  EXPECT_NEAR(cr_rms / cb_rms, expected_ratio, expected_ratio * 0.1);
}

// The colour-difference axes and the Q30 filter taps are held as int32 so the
// FIR accumulates through a widening 32×32→64 multiply. These two tests walk
// the whole legal input range to show that narrowing changed nothing: the
// accumulator never overflows or saturates, and the fixed-point result still
// tracks the analytic chroma amplitude to well inside one output LSB.
TEST(ChromaEncoderTest, FixedPointEncodingIsExactAcrossTheFullChromaCodeRange) {
  const TimingConstants pal_timing = GetTimingConstants(Standard::kPal);
  const auto pal =
      CreateChromaEncoder(Standard::kPal, pal_timing.sample_rate_4fsc_hz);
  ASSERT_NE(pal, nullptr);

  // ITU-R BT.1700 Part B Table 1 item 9: E'_V = 0.877 × (R−Y), scaled to the
  // 700 mV PAL white level. At carrier phase 0 the sin (U) axis contributes
  // nothing and the cos (V) axis lands squarely on the carrier, so sample 0 of
  // a constant line is the full V amplitude for that Cr code.
  constexpr double kPalVScaleMv = 0.877 * 0.701 * 700.0;

  // Quantising the unity-DC-gain taps to Q30 leaves a relative DC-gain error of
  // at most one half-LSB per tap; over 33 PAL taps and the full ±2^20 axis
  // range that is a few millionths of a millivolt. Allow 1e-3 mV, still three
  // orders of magnitude below the ~1.19 mV output code step.
  constexpr double kToleranceMv = 1e-3;

  for (int code = 0; code <= 1023; ++code) {
    const std::vector<YCbCr444Pixel> line(
        64, YCbCr444Pixel{
                .y = 512, .cb = 512, .cr = static_cast<std::int16_t>(code)});
    std::vector<SampleFixed> output;
    pal->EncodeLineFromPhaseStart(line, 0.0, &output);
    ASSERT_EQ(output.size(), line.size());

    // The encoder clamps to the BT.601 colour-difference range and quantises
    // the axis to Q20 with truncating division, so the reference value must do
    // the same before scaling.
    const int clamped = std::max(64, std::min(code, 960));
    const std::int64_t axis_fixed =
        static_cast<std::int64_t>(clamped - 512) * (1LL << 20) / 448;
    const double expected_mv =
        (static_cast<double>(axis_fixed) / static_cast<double>(1LL << 20)) *
        kPalVScaleMv;

    EXPECT_NEAR(SampleFixedToMillivolts(output[0]), expected_mv, kToleranceMv)
        << "Cr code " << code;
  }
}

TEST(ChromaEncoderTest,
     FullScaleChromaAlternationStaysWithinTheAmplitudeBound) {
  // Worst case for the fixed-point accumulator: both axes swinging the full
  // legal excursion every sample, which drives the filter transient to its
  // largest magnitude. An overflow or a narrowing mistake would show up as a
  // sign flip or a wildly out-of-range amplitude rather than a subtle error.
  const TimingConstants pal_timing = GetTimingConstants(Standard::kPal);
  const auto pal =
      CreateChromaEncoder(Standard::kPal, pal_timing.sample_rate_4fsc_hz);
  ASSERT_NE(pal, nullptr);

  std::vector<YCbCr444Pixel> line(512, YCbCr444Pixel{});
  for (std::size_t index = 0; index < line.size(); ++index) {
    const bool high = (index % 2U) == 0U;
    line[index] =
        YCbCr444Pixel{.y = 512,
                      .cb = static_cast<std::int16_t>(high ? 960 : 64),
                      .cr = static_cast<std::int16_t>(high ? 64 : 960)};
  }

  std::vector<SampleFixed> output;
  pal->EncodeLineFromPhaseStart(line, 0.0, &output);
  ASSERT_EQ(output.size(), line.size());

  // Vector sum of both axes at full excursion, before any low-pass
  // attenuation: no sample can legitimately exceed it.
  constexpr double kPalUScaleMv = 0.493 * 0.886 * 700.0;
  constexpr double kPalVScaleMv = 0.877 * 0.701 * 700.0;
  const double amplitude_bound_mv = kPalUScaleMv + kPalVScaleMv;

  for (SampleFixed value : output) {
    EXPECT_LE(std::abs(SampleFixedToMillivolts(value)), amplitude_bound_mv);
  }
}

TEST(ChromaEncoderTest, PhaseStartEncodingIsDeterministic) {
  const TimingConstants ntsc_timing = GetTimingConstants(Standard::kNtsc);
  const auto ntsc =
      CreateChromaEncoder(Standard::kNtsc, ntsc_timing.sample_rate_4fsc_hz);
  ASSERT_NE(ntsc, nullptr);

  std::vector<YCbCr444Pixel> line(
      128, YCbCr444Pixel{.y = 512, .cb = 760, .cr = 300});
  const double phase_start = M_PI / 6.0;

  std::vector<SampleFixed> first_run;
  std::vector<SampleFixed> second_run;
  ntsc->EncodeLineFromPhaseStart(line, phase_start, &first_run);
  ntsc->EncodeLineFromPhaseStart(line, phase_start, &second_run);

  ASSERT_EQ(first_run.size(), second_run.size());
  for (std::size_t i = 0; i < first_run.size(); ++i) {
    EXPECT_EQ(first_run[i], second_run[i]);
  }
}

TEST(ChromaEncoderTest, MirroredCodesGiveExactlyNegatedAxisSamples) {
  for (int code = 0; code <= 1024; ++code) {
    ASSERT_EQ(ChromaAxisFromCode(1024 - code), -ChromaAxisFromCode(code))
        << code;
  }
  EXPECT_EQ(ChromaAxisFromCode(512), 0);
  EXPECT_EQ(ChromaAxisFromCode(960), kChromaAxisScale);
  EXPECT_EQ(ChromaAxisFromCode(1023), kChromaAxisScale);
  EXPECT_EQ(ChromaAxisFromCode(0), -kChromaAxisScale);
}

TEST(ChromaEncoderTest, AxisEncodingMatchesPixelEncoding) {
  for (const Standard standard :
       {Standard::kPal, Standard::kNtsc, Standard::kPalM}) {
    const auto encoder = CreateChromaEncoder(
        standard, GetTimingConstants(standard).sample_rate_4fsc_hz);
    ASSERT_NE(encoder, nullptr);
    std::vector<YCbCr444Pixel> line(200);
    std::vector<std::int32_t> sin_axis(line.size());
    std::vector<std::int32_t> cos_axis(line.size());
    for (std::size_t i = 0; i < line.size(); ++i) {
      line[i].cb = static_cast<std::int16_t>(64 + ((i * 37) % 897));
      line[i].cr = static_cast<std::int16_t>(960 - ((i * 53) % 897));
      sin_axis[i] = ChromaAxisFromCode(line[i].cb);
      cos_axis[i] = ChromaAxisFromCode(line[i].cr);
    }
    std::vector<SampleFixed> from_pixels;
    std::vector<SampleFixed> from_axes;
    encoder->EncodeLineFromPhaseStart(line, 0.3, &from_pixels);
    const std::vector<std::int32_t> filtered_cos_pixels =
        encoder->filtered_cos_axis();
    encoder->EncodeAxesFromPhaseStart(sin_axis, cos_axis, 0.3, &from_axes);
    EXPECT_EQ(from_pixels, from_axes) << StandardToString(standard);
    EXPECT_EQ(filtered_cos_pixels, encoder->filtered_cos_axis());
    EXPECT_EQ(encoder->filtered_sin_axis().size(), line.size());
  }
}

TEST(ChromaEncoderTest, AxisEncodingAcceptsResamplerOvershoot) {
  const auto encoder = CreateChromaEncoder(
      Standard::kPal, GetTimingConstants(Standard::kPal).sample_rate_4fsc_hz);
  const std::vector<std::int32_t> sin_axis(64, 2 * kChromaAxisScale);
  const std::vector<std::int32_t> cos_axis(64, -2 * kChromaAxisScale);
  std::vector<SampleFixed> chroma;
  encoder->EncodeAxesFromPhaseStart(sin_axis, cos_axis, 0.0, &chroma);
  // Flat axes pass the unity-DC-gain low-pass unchanged.
  EXPECT_EQ(encoder->filtered_sin_axis()[32], 2 * kChromaAxisScale);
  EXPECT_EQ(encoder->filtered_cos_axis()[32], -2 * kChromaAxisScale);
}

}  // namespace
}  // namespace videosynth