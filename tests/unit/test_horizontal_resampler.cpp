/*
 * File:        test_horizontal_resampler.cpp
 * Module:      generation_tests
 * Purpose:     Validates the band-limited horizontal mapping: unit DC gain,
 *              edge alignment with the nearest mapping, suppression of the
 *              source-spectrum images a zero-order hold leaves, edge
 *              replication, and the horizontal_resample YAML key.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "videosynth/active_sample_mapping.h"
#include "videosynth/fixed_point.h"
#include "videosynth/horizontal_resampler.h"
#include "videosynth/model.h"
#include "videosynth/yaml_project_emitter.h"
#include "videosynth/yaml_project_parser.h"

namespace videosynth {
namespace {

constexpr double kPi = 3.14159265358979323846;
// PAL active window: 52 us at 4fsc, and the 720-pixel BT.601 source raster.
constexpr int kPalWindowSamples = 922;
constexpr int kSourceWidth = 720;

// Amplitude of the component at frequency (cycles per sample) of a real
// signal, by single-bin DFT with a Hann window (normalised so a sinusoid of
// amplitude A reads about A).
double ComponentAmplitude(const std::vector<double>& signal, double frequency) {
  double re = 0.0;
  double im = 0.0;
  double window_sum = 0.0;
  const auto count = static_cast<double>(signal.size());
  for (std::size_t n = 0; n < signal.size(); ++n) {
    const double window =
        0.5 - (0.5 * std::cos(2.0 * kPi * static_cast<double>(n) / count));
    const double angle = 2.0 * kPi * frequency * static_cast<double>(n);
    re += window * signal[n] * std::cos(angle);
    im -= window * signal[n] * std::sin(angle);
    window_sum += window;
  }
  return 2.0 * std::hypot(re, im) / window_sum;
}

// A source row of a sinusoid at cycles_per_pixel, in mV about 350 mV.
std::vector<SampleFixed> SineRow(double cycles_per_pixel, double amplitude_mv) {
  std::vector<SampleFixed> row(kSourceWidth);
  for (int x = 0; x < kSourceWidth; ++x) {
    row[static_cast<std::size_t>(x)] = MillivoltsToSampleFixed(
        350.0 + (amplitude_mv * std::sin(2.0 * kPi * cycles_per_pixel * x)));
  }
  return row;
}

std::vector<double> ToMillivolts(const std::vector<SampleFixed>& samples) {
  std::vector<double> out(samples.size());
  for (std::size_t i = 0; i < samples.size(); ++i) {
    out[i] = SampleFixedToMillivolts(samples[i]) - 350.0;
  }
  return out;
}

std::vector<SampleFixed> NearestMap(const std::vector<SampleFixed>& row) {
  std::vector<SampleFixed> out(kPalWindowSamples);
  for (int x = 0; x < kPalWindowSamples; ++x) {
    out[static_cast<std::size_t>(x)] = row[static_cast<std::size_t>(
        MapActiveSampleToSourcePixel(x, kPalWindowSamples, kSourceWidth, 0))];
  }
  return out;
}

TEST(BandlimitedHorizontalResamplerTest, EveryPhaseHasUnitDcGain) {
  const BandlimitedHorizontalResampler resampler(kPalWindowSamples,
                                                 kSourceWidth, 0, kSourceWidth);
  ASSERT_GT(resampler.taps_per_sample(), 0);
  for (int x = 0; x < kPalWindowSamples; ++x) {
    double sum = 0.0;
    for (int k = 0; k < resampler.taps_per_sample(); ++k) {
      sum += resampler.weight(x, k);
      ASSERT_GE(resampler.column(x, k), 0);
      ASSERT_LT(resampler.column(x, k), kSourceWidth);
    }
    ASSERT_NEAR(sum, 1.0, 1e-12) << x;
  }
}

TEST(BandlimitedHorizontalResamplerTest, FlatRowPassesUnchanged) {
  const BandlimitedHorizontalResampler resampler(kPalWindowSamples,
                                                 kSourceWidth, 0, kSourceWidth);
  const std::vector<SampleFixed> row(kSourceWidth,
                                     MillivoltsToSampleFixed(523.25));
  std::vector<SampleFixed> out(kPalWindowSamples);
  resampler.Apply(row.data(), out.data());
  for (const SampleFixed sample : out) {
    ASSERT_EQ(sample, row.front());
  }

  const std::vector<std::int32_t> axis_row(kSourceWidth, -123456);
  std::vector<std::int32_t> axis_out(kPalWindowSamples);
  resampler.Apply(axis_row.data(), axis_out.data());
  for (const std::int32_t sample : axis_out) {
    ASSERT_EQ(sample, -123456);
  }
}

TEST(BandlimitedHorizontalResamplerTest, StepEdgeLandsWhereTheNearestEdgeDoes) {
  const BandlimitedHorizontalResampler resampler(kPalWindowSamples,
                                                 kSourceWidth, 0, kSourceWidth);
  constexpr int kEdgePixel = 360;
  std::vector<SampleFixed> row(kSourceWidth);
  for (int x = 0; x < kSourceWidth; ++x) {
    row[static_cast<std::size_t>(x)] =
        MillivoltsToSampleFixed(x < kEdgePixel ? 100.0 : 600.0);
  }
  std::vector<SampleFixed> out(kPalWindowSamples);
  resampler.Apply(row.data(), out.data());
  const std::vector<SampleFixed> nearest = NearestMap(row);

  // The nearest mapping switches at the first sample mapped to the edge
  // pixel; the band-limited step crosses its midpoint within a sample of it.
  int nearest_edge = 0;
  while (nearest[static_cast<std::size_t>(nearest_edge)] == row.front()) {
    ++nearest_edge;
  }
  double crossing = -1.0;
  for (int x = 1; x < kPalWindowSamples; ++x) {
    const double previous =
        SampleFixedToMillivolts(out[static_cast<std::size_t>(x - 1)]);
    const double current =
        SampleFixedToMillivolts(out[static_cast<std::size_t>(x)]);
    if (previous < 350.0 && current >= 350.0) {
      crossing = (x - 1) + ((350.0 - previous) / (current - previous));
      break;
    }
  }
  ASSERT_GE(crossing, 0.0);
  // The hold changes level between samples nearest_edge - 1 and nearest_edge.
  EXPECT_NEAR(crossing, nearest_edge - 0.5, 1.0);
  // Both ends settle on the row's own levels (edge replication).
  EXPECT_NEAR(SampleFixedToMillivolts(out.front()), 100.0, 1e-3);
  EXPECT_NEAR(SampleFixedToMillivolts(out.back()), 600.0, 1e-3);
}

TEST(BandlimitedHorizontalResamplerTest, SuppressesTheZeroOrderHoldImages) {
  const BandlimitedHorizontalResampler resampler(kPalWindowSamples,
                                                 kSourceWidth, 0, kSourceWidth);
  // Source pixels per output sample: the 720-pixel row spans the 922-sample
  // window.
  constexpr double kPixelsPerSample =
      static_cast<double>(kSourceWidth) / kPalWindowSamples;
  // Content at c cycles/pixel leaves an image at 1 - c cycles/pixel. In
  // output samples that is (1 - c) * W / N, above the output Nyquist, so it
  // aliases to 1 - (1 - c) * W / N: for c = 0.02 (0.27 MHz on a 13.5 MHz
  // raster), about 4.65 MHz in the PAL output, beside the colour subcarrier.
  for (const double cycles_per_pixel : {0.02, 0.07, 0.2}) {
    const double tone = cycles_per_pixel * kPixelsPerSample;
    const double image = 1.0 - ((1.0 - cycles_per_pixel) * kPixelsPerSample);
    const std::vector<SampleFixed> row = SineRow(cycles_per_pixel, 200.0);

    std::vector<SampleFixed> bandlimited(kPalWindowSamples);
    resampler.Apply(row.data(), bandlimited.data());
    const std::vector<double> bandlimited_mv = ToMillivolts(bandlimited);
    const std::vector<double> nearest_mv = ToMillivolts(NearestMap(row));

    // The tone itself passes at full amplitude.
    EXPECT_NEAR(ComponentAmplitude(bandlimited_mv, tone), 200.0, 2.0)
        << cycles_per_pixel;
    // The hold leaves a clearly measurable image; the band-limited mapping
    // leaves it at the window's leakage floor.
    const double nearest_image = ComponentAmplitude(nearest_mv, image);
    const double bandlimited_image = ComponentAmplitude(bandlimited_mv, image);
    EXPECT_GT(nearest_image, 2.0) << cycles_per_pixel;
    EXPECT_LT(bandlimited_image, 0.05) << cycles_per_pixel;
  }
}

TEST(BandlimitedHorizontalResamplerTest, TapsNeverReadOutsideTheActiveRaster) {
  // A source whose active area is a sub-rectangle of its raster.
  const BandlimitedHorizontalResampler resampler(900, 700, 10, 720);
  for (int x = 0; x < 900; ++x) {
    for (int k = 0; k < resampler.taps_per_sample(); ++k) {
      ASSERT_GE(resampler.column(x, k), 10);
      ASSERT_LE(resampler.column(x, k), 709);
    }
  }
}

std::string SectionYaml(const std::string& extra) {
  return "cvbs_presets:\n"
         "  video_standard_preset: PAL\n"
         "output:\n"
         "  video_path: out.cvbs\n"
         "sections:\n"
         "  - name: S\n"
         "    type: progressive\n"
         "    source: f.exr\n"
         "    duration_frames: 1\n" +
         extra;
}

TEST(HorizontalResampleParserTest, DefaultsToNearestAndReadsBandlimited) {
  YamlProjectParser parser;
  const ParseResult defaulted = parser.ParseString(SectionYaml(""));
  ASSERT_TRUE(defaulted.ok);
  EXPECT_EQ(defaulted.project.sections[0].horizontal_resample,
            HorizontalResample::kNearest);

  const ParseResult bandlimited =
      parser.ParseString(SectionYaml("    horizontal_resample: bandlimited\n"));
  ASSERT_TRUE(bandlimited.ok) << bandlimited.errors[0];
  EXPECT_EQ(bandlimited.project.sections[0].horizontal_resample,
            HorizontalResample::kBandlimited);

  const ParseResult nearest =
      parser.ParseString(SectionYaml("    horizontal_resample: nearest\n"));
  ASSERT_TRUE(nearest.ok);
  EXPECT_EQ(nearest.project.sections[0].horizontal_resample,
            HorizontalResample::kNearest);
}

TEST(HorizontalResampleParserTest, RejectsUnknownValues) {
  YamlProjectParser parser;
  const ParseResult result =
      parser.ParseString(SectionYaml("    horizontal_resample: lanczos\n"));
  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.errors[0].find("horizontal_resample"), std::string::npos);
  EXPECT_NE(result.errors[0].find("nearest, bandlimited"), std::string::npos);
}

TEST(HorizontalResampleEmitterTest, OmitsTheDefaultAndRoundTripsBandlimited) {
  YamlProjectParser parser;
  YamlProjectEmitter emitter;
  const ParseResult nearest = parser.ParseString(SectionYaml(""));
  ASSERT_TRUE(nearest.ok);
  EXPECT_EQ(emitter.EmitString(nearest.project).find("horizontal_resample"),
            std::string::npos);

  const ParseResult bandlimited =
      parser.ParseString(SectionYaml("    horizontal_resample: bandlimited\n"));
  ASSERT_TRUE(bandlimited.ok);
  const ParseResult reparsed =
      parser.ParseString(emitter.EmitString(bandlimited.project));
  ASSERT_TRUE(reparsed.ok);
  EXPECT_EQ(reparsed.project.sections[0].horizontal_resample,
            HorizontalResample::kBandlimited);
}

}  // namespace
}  // namespace videosynth
