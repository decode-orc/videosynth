/*
 * File:        test_source_pixel_grid.cpp
 * Module:      generation_tests
 * Purpose:     Validates the source pixel grids of the horizontal_mapping
 *              key: the active-window stretch, the ITU-R BT.601 sampling grid
 *              anchored to 0H, the band-limited resampler on that grid, and
 *              the YAML parsing and emission of the key.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

#include "videosynth/fixed_point.h"
#include "videosynth/frame_line_layout.h"
#include "videosynth/horizontal_resampler.h"
#include "videosynth/model.h"
#include "videosynth/source_pixel_grid.h"
#include "videosynth/timing_constants.h"
#include "videosynth/yaml_project_emitter.h"
#include "videosynth/yaml_project_parser.h"

namespace videosynth {
namespace {

constexpr int kSourceWidth = 720;

struct WindowForTest {
  double sample_rate_hz = 0.0;
  int start = 0;
  int samples = 0;
};

WindowForTest ActiveWindowFor(Standard standard) {
  const double rate = GetTimingConstants(standard).sample_rate_4fsc_hz;
  const ActiveRasterGeometry geometry = GetActiveRasterGeometry(standard, rate);
  return WindowForTest{
      .sample_rate_hz = rate,
      .start = geometry.active_window_start_samples,
      .samples = geometry.active_window_end_samples -
                 geometry.active_window_start_samples,
  };
}

// The 4fsc sample offset after 0H (fractional) at which the grid centres
// source pixel n.
double PixelCentreSamplesAfterZeroH(const SourcePixelGrid& grid,
                                    int window_start, int n) {
  return static_cast<double>(window_start) +
         (((static_cast<double>(n) + 0.5) - grid.origin_pixels) /
          grid.pixels_per_sample);
}

TEST(SourcePixelGridTest, ActiveWindowStretchFillsTheWindow) {
  const SourcePixelGrid grid = ActiveWindowSourcePixelGrid(922, kSourceWidth);
  EXPECT_DOUBLE_EQ(grid.origin_pixels, 0.0);
  EXPECT_DOUBLE_EQ(SourcePixelCoordinate(grid, 922), 720.0);
  EXPECT_EQ(SourcePixelGridInRangeSamples(grid, 922, kSourceWidth), 922);
}

TEST(SourcePixelGridTest, Bt601PalCentresPixelZero132PeriodsAfterZeroH) {
  const WindowForTest window = ActiveWindowFor(Standard::kPal);
  const SourcePixelGrid grid =
      Bt601SourcePixelGrid(Standard::kPal, window.sample_rate_hz, window.start);
  EXPECT_DOUBLE_EQ(grid.pixels_per_sample, 13.5e6 / 17734475.0);
  // ITU-R BT.601-5 Part A Appendix 1 Fig. 1: 132 T = 9.778 us after 0H.
  const double expected_first = 132.0 * 17734475.0 / 13.5e6;
  EXPECT_NEAR(PixelCentreSamplesAfterZeroH(grid, window.start, 0),
              expected_first, 1e-9);
  // 720 pixels span 53.33 us: about 945.8 4fsc samples, not the 922 of the
  // 52 us active window.
  const double span =
      PixelCentreSamplesAfterZeroH(grid, window.start, kSourceWidth) -
      PixelCentreSamplesAfterZeroH(grid, window.start, 0);
  EXPECT_NEAR(span, 945.84, 0.01);
}

TEST(SourcePixelGridTest, Bt601525LineCentresPixelZero122PeriodsAfterZeroH) {
  for (const Standard standard : {Standard::kNtsc, Standard::kPalM}) {
    const WindowForTest window = ActiveWindowFor(standard);
    const SourcePixelGrid grid =
        Bt601SourcePixelGrid(standard, window.sample_rate_hz, window.start);
    EXPECT_NEAR(PixelCentreSamplesAfterZeroH(grid, window.start, 0),
                122.0 * window.sample_rate_hz / 13.5e6, 1e-9);
  }
}

TEST(SourcePixelGridTest, Bt601LineStartsBeforeTheActiveWindowOnEveryStandard) {
  // The column map relies on there being no window samples before pixel 0.
  for (const Standard standard :
       {Standard::kPal, Standard::kNtsc, Standard::kPalM, Standard::kSecam}) {
    const WindowForTest window = ActiveWindowFor(standard);
    const SourcePixelGrid grid =
        Bt601SourcePixelGrid(standard, window.sample_rate_hz, window.start);
    EXPECT_GE(SourcePixelCoordinate(grid, 0), 0.0);
  }
}

TEST(SourcePixelGridTest, Bt601PalCoversTheWholeWindowAndNtscLeavesATail) {
  const WindowForTest pal = ActiveWindowFor(Standard::kPal);
  const SourcePixelGrid pal_grid =
      Bt601SourcePixelGrid(Standard::kPal, pal.sample_rate_hz, pal.start);
  // The BT.601 line ends 63.1 us after 0H, after the 62.0 us window end.
  EXPECT_EQ(SourcePixelGridInRangeSamples(pal_grid, pal.samples, kSourceWidth),
            pal.samples);

  const WindowForTest ntsc = ActiveWindowFor(Standard::kNtsc);
  const SourcePixelGrid ntsc_grid =
      Bt601SourcePixelGrid(Standard::kNtsc, ntsc.sample_rate_hz, ntsc.start);
  // The BT.601 line ends 62.37 us after 0H, ahead of the 62.5 us window end.
  const int in_range =
      SourcePixelGridInRangeSamples(ntsc_grid, ntsc.samples, kSourceWidth);
  EXPECT_LT(in_range, ntsc.samples);
  EXPECT_GT(in_range, ntsc.samples - 4);
  EXPECT_LT(SourcePixelCoordinate(ntsc_grid, in_range - 1), 720.0);
  EXPECT_GE(SourcePixelCoordinate(ntsc_grid, in_range), 720.0);
}

TEST(SourcePixelGridTest, ResolveSelectsTheSectionMapping) {
  const WindowForTest pal = ActiveWindowFor(Standard::kPal);
  EXPECT_EQ(ResolveSourcePixelGrid(HorizontalMapping::kActiveWindow,
                                   Standard::kPal, pal.sample_rate_hz,
                                   pal.start, pal.samples, kSourceWidth),
            ActiveWindowSourcePixelGrid(pal.samples, kSourceWidth));
  EXPECT_EQ(
      ResolveSourcePixelGrid(HorizontalMapping::kBt601, Standard::kPal,
                             pal.sample_rate_hz, pal.start, pal.samples,
                             kSourceWidth),
      Bt601SourcePixelGrid(Standard::kPal, pal.sample_rate_hz, pal.start));
}

TEST(SourcePixelGridTest, ActiveWindowResamplerIsUnchangedByTheGrid) {
  const BandlimitedHorizontalResampler legacy(922, kSourceWidth, 0,
                                              kSourceWidth);
  const BandlimitedHorizontalResampler gridded(
      922, kSourceWidth, 0, kSourceWidth,
      ActiveWindowSourcePixelGrid(922, kSourceWidth));
  ASSERT_EQ(legacy.taps_per_sample(), gridded.taps_per_sample());
  for (int x = 0; x < 922; ++x) {
    for (int k = 0; k < legacy.taps_per_sample(); ++k) {
      ASSERT_EQ(legacy.column(x, k), gridded.column(x, k));
      ASSERT_EQ(legacy.weight(x, k), gridded.weight(x, k));
    }
  }
}

TEST(SourcePixelGridTest, Bt601ResamplerPlacesAStepAtTheBt601Instant) {
  const WindowForTest pal = ActiveWindowFor(Standard::kPal);
  const SourcePixelGrid grid =
      Bt601SourcePixelGrid(Standard::kPal, pal.sample_rate_hz, pal.start);
  const BandlimitedHorizontalResampler resampler(pal.samples, kSourceWidth, 0,
                                                 kSourceWidth, grid);
  // Black to white at pixel 360: the edge lies between the centres of pixels
  // 359 and 360, at coordinate u = 360.
  constexpr int kEdgePixel = 360;
  std::vector<SampleFixed> row(kSourceWidth);
  for (int x = 0; x < kSourceWidth; ++x) {
    row[static_cast<std::size_t>(x)] =
        MillivoltsToSampleFixed(x < kEdgePixel ? 0.0 : 700.0);
  }
  std::vector<SampleFixed> out(static_cast<std::size_t>(pal.samples));
  resampler.Apply(row.data(), out.data());

  // Linear interpolation of the 350 mV crossing.
  double crossing = -1.0;
  for (int x = 1; x < pal.samples; ++x) {
    const double previous =
        SampleFixedToMillivolts(out[static_cast<std::size_t>(x - 1)]);
    const double current =
        SampleFixedToMillivolts(out[static_cast<std::size_t>(x)]);
    if (previous < 350.0 && current >= 350.0) {
      crossing = (x - 1) + ((350.0 - previous) / (current - previous));
      break;
    }
  }
  const double expected =
      (static_cast<double>(kEdgePixel) - grid.origin_pixels) /
      grid.pixels_per_sample;
  EXPECT_NEAR(crossing, expected, 0.05);

  // A flat row stays flat over the window.
  std::vector<SampleFixed> flat(kSourceWidth, MillivoltsToSampleFixed(350.0));
  resampler.Apply(flat.data(), out.data());
  for (const SampleFixed sample : out) {
    ASSERT_EQ(sample, flat[0]);
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

TEST(HorizontalMappingParserTest, DefaultsToActiveWindowAndReadsBt601) {
  YamlProjectParser parser;
  const ParseResult defaulted = parser.ParseString(SectionYaml(""));
  ASSERT_TRUE(defaulted.ok);
  EXPECT_EQ(defaulted.project.sections[0].horizontal_mapping,
            HorizontalMapping::kActiveWindow);

  const ParseResult bt601 =
      parser.ParseString(SectionYaml("    horizontal_mapping: bt601\n"));
  ASSERT_TRUE(bt601.ok) << bt601.errors[0];
  EXPECT_EQ(bt601.project.sections[0].horizontal_mapping,
            HorizontalMapping::kBt601);

  const ParseResult active = parser.ParseString(
      SectionYaml("    horizontal_mapping: active_window\n"));
  ASSERT_TRUE(active.ok);
  EXPECT_EQ(active.project.sections[0].horizontal_mapping,
            HorizontalMapping::kActiveWindow);
}

TEST(HorizontalMappingParserTest, RejectsUnknownValues) {
  YamlProjectParser parser;
  const ParseResult result =
      parser.ParseString(SectionYaml("    horizontal_mapping: smpte\n"));
  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.errors[0].find("horizontal_mapping"), std::string::npos);
  EXPECT_NE(result.errors[0].find("active_window, bt601"), std::string::npos);
}

TEST(HorizontalMappingEmitterTest, OmitsTheDefaultAndRoundTripsBt601) {
  YamlProjectParser parser;
  YamlProjectEmitter emitter;
  const ParseResult active = parser.ParseString(SectionYaml(""));
  ASSERT_TRUE(active.ok);
  EXPECT_EQ(emitter.EmitString(active.project).find("horizontal_mapping"),
            std::string::npos);

  const ParseResult bt601 =
      parser.ParseString(SectionYaml("    horizontal_mapping: bt601\n"));
  ASSERT_TRUE(bt601.ok);
  const ParseResult reparsed =
      parser.ParseString(emitter.EmitString(bt601.project));
  ASSERT_TRUE(reparsed.ok);
  EXPECT_EQ(reparsed.project.sections[0].horizontal_mapping,
            HorizontalMapping::kBt601);
}

TEST(ActiveExtentParserTest, DefaultsToWindowAndReadsSource) {
  YamlProjectParser parser;
  const ParseResult defaulted = parser.ParseString(SectionYaml(""));
  ASSERT_TRUE(defaulted.ok);
  EXPECT_EQ(defaulted.project.sections[0].active_extent, ActiveExtent::kWindow);

  const ParseResult source = parser.ParseString(SectionYaml(
      "    horizontal_mapping: bt601\n    active_extent: source\n"));
  ASSERT_TRUE(source.ok) << source.errors[0];
  EXPECT_EQ(source.project.sections[0].active_extent, ActiveExtent::kSource);
}

TEST(ActiveExtentParserTest, RejectsUnknownValues) {
  YamlProjectParser parser;
  const ParseResult result =
      parser.ParseString(SectionYaml("    active_extent: full\n"));
  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.errors[0].find("window, source"), std::string::npos);
}

TEST(ActiveExtentEmitterTest, OmitsTheDefaultAndRoundTripsSource) {
  YamlProjectParser parser;
  YamlProjectEmitter emitter;
  const ParseResult window = parser.ParseString(SectionYaml(""));
  ASSERT_TRUE(window.ok);
  EXPECT_EQ(emitter.EmitString(window.project).find("active_extent"),
            std::string::npos);

  const ParseResult source = parser.ParseString(SectionYaml(
      "    horizontal_mapping: bt601\n    active_extent: source\n"));
  ASSERT_TRUE(source.ok);
  const ParseResult reparsed =
      parser.ParseString(emitter.EmitString(source.project));
  ASSERT_TRUE(reparsed.ok);
  EXPECT_EQ(reparsed.project.sections[0].active_extent, ActiveExtent::kSource);
}

// PAL 4fsc: one BT.601 pixel is 1.3137 samples. Pixel 0 starts at
// 131.5 periods (172.75 samples) and pixel 719 ends at 851.5 periods
// (1118.6 samples), so the span is samples 173 to 1118 inclusive.
TEST(ActiveSampleSpanTest, PalSourceExtentCoversTheWholeBt601Line) {
  const ActiveSampleSpan span =
      ResolveActiveSampleSpan(HorizontalMapping::kBt601, ActiveExtent::kSource,
                              Standard::kPal, 17734475.0, 177, 177 + 922, 1137);
  EXPECT_EQ(span.start, 173);
  EXPECT_EQ(span.end(), 1119);

  const SourcePixelGrid grid =
      Bt601SourcePixelGrid(Standard::kPal, 17734475.0, span.start);
  EXPECT_GE(SourcePixelCoordinate(grid, 0), 0.0);
  EXPECT_EQ(SourcePixelGridInRangeSamples(grid, span.samples, 720),
            span.samples);
}

// NTSC: the BT.601 line starts before the window but ends inside it, so the
// span is the window extended at its start only.
TEST(ActiveSampleSpanTest, NtscSourceExtentExtendsOnlyTheStart) {
  const double rate = 14318181.818181818;
  const int window_start = 150;
  const int window_end = 895;
  const ActiveSampleSpan span = ResolveActiveSampleSpan(
      HorizontalMapping::kBt601, ActiveExtent::kSource, Standard::kNtsc, rate,
      window_start, window_end, 910);
  EXPECT_EQ(span.start, 129);
  EXPECT_EQ(span.end(), window_end);
}

TEST(ActiveSampleSpanTest, WindowExtentAndActiveWindowMappingKeepTheWindow) {
  const ActiveSampleSpan window =
      ResolveActiveSampleSpan(HorizontalMapping::kBt601, ActiveExtent::kWindow,
                              Standard::kPal, 17734475.0, 177, 1099, 1137);
  EXPECT_EQ(window, (ActiveSampleSpan{.start = 177, .samples = 922}));
  const ActiveSampleSpan stretched = ResolveActiveSampleSpan(
      HorizontalMapping::kActiveWindow, ActiveExtent::kSource, Standard::kPal,
      17734475.0, 177, 1099, 1137);
  EXPECT_EQ(stretched, window);
}

}  // namespace
}  // namespace videosynth
