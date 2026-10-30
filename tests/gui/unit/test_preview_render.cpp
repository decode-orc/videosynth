/*
 * File:        test_preview_render.cpp
 * Module:      gui_tests
 * Purpose:     Unit tests for preview raster rendering: field geometry,
 *              10-bit code-space grayscale mapping, and BT.601 source
 *              conversion
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <QImage>
#include <algorithm>
#include <vector>

#include "preview_render.h"
#include "videosynth/fixed_point.h"
#include "videosynth/model.h"
#include "videosynth/timing_constants.h"

namespace videosynth::gui {
namespace {

std::vector<SampleFixed> MakeFrameBuffer(Standard standard, double millivolts) {
  return std::vector<SampleFixed>(
      static_cast<std::size_t>(SamplesPerFrame4fsc(standard)),
      MillivoltsToSampleFixed(millivolts));
}

TEST(PreviewRenderTest, FieldLineRangesCoverPalFrameIncludingVbi) {
  // ITU-R BT.1700 Annex 1 Part B Table 1 item 1: 625 lines, field 1 modelled
  // as lines 1-312. Full-field rasters keep the VBI region visible.
  const FieldLineRange field1 = GetFieldLineRange(Standard::kPal, 1);
  const FieldLineRange field2 = GetFieldLineRange(Standard::kPal, 2);
  EXPECT_EQ(field1.first_line_1based, 1);
  EXPECT_EQ(field1.line_count, 312);
  EXPECT_EQ(field2.first_line_1based, 313);
  EXPECT_EQ(field2.line_count, 313);
}

TEST(PreviewRenderTest, FieldLineRangesCoverNtscFrameIncludingVbi) {
  // SMPTE 170M-2004 Section 11.3: 525 lines, field 1 modelled as lines 1-262.
  const FieldLineRange field1 = GetFieldLineRange(Standard::kNtsc, 1);
  const FieldLineRange field2 = GetFieldLineRange(Standard::kNtsc, 2);
  EXPECT_EQ(field1.first_line_1based, 1);
  EXPECT_EQ(field1.line_count, 262);
  EXPECT_EQ(field2.first_line_1based, 263);
  EXPECT_EQ(field2.line_count, 263);
}

TEST(PreviewRenderTest, PictureRowLineMappingRoundTrips) {
  EXPECT_EQ(PictureRowToLineNumber(Standard::kPal, 1, 0), 1);
  EXPECT_EQ(PictureRowToLineNumber(Standard::kPal, 1, 311), 312);
  EXPECT_EQ(PictureRowToLineNumber(Standard::kPal, 2, 0), 313);
  EXPECT_EQ(PictureRowToLineNumber(Standard::kNtsc, 2, 5), 268);

  EXPECT_EQ(LineNumberToField(Standard::kPal, 312), 1);
  EXPECT_EQ(LineNumberToField(Standard::kPal, 313), 2);
  EXPECT_EQ(LineNumberToPictureRow(Standard::kPal, 313), 0);
  EXPECT_EQ(LineNumberToPictureRow(Standard::kNtsc, 22), 21);
}

TEST(PreviewRenderTest, InterlacedLineOrderWeavesBothFieldsOnce) {
  const std::vector<int> pal = InterlacedLineOrder(Standard::kPal);
  ASSERT_EQ(pal.size(), 625U);
  // Woven as the generation stage maps source rows: field 2 line L + 313
  // above field 1 line L, with field 2's unpaired line 313 on top.
  EXPECT_EQ(pal[0], 313);
  EXPECT_EQ(pal[1], 314);
  EXPECT_EQ(pal[2], 1);
  EXPECT_EQ(pal[3], 315);
  EXPECT_EQ(pal[4], 2);
  EXPECT_EQ(pal[624], 312);
  // Every frame line appears exactly once.
  std::vector<int> sorted = pal;
  std::sort(sorted.begin(), sorted.end());
  sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
  EXPECT_EQ(sorted.size(), 625U);
  EXPECT_EQ(sorted.front(), 1);
  EXPECT_EQ(sorted.back(), 625);

  // NTSC pairs field 2 line L + 262 above field 1 line L, with field 2's
  // unpaired line 525 at the bottom.
  const std::vector<int> ntsc = InterlacedLineOrder(Standard::kNtsc);
  ASSERT_EQ(ntsc.size(), 525U);
  EXPECT_EQ(ntsc[0], 263);
  EXPECT_EQ(ntsc[1], 1);
  EXPECT_EQ(ntsc[2], 264);
  EXPECT_EQ(ntsc[523], 262);
  EXPECT_EQ(ntsc[524], 525);
}

TEST(PreviewRenderTest, InterlacedLineOrderFollowsSourceRows) {
  // The first picture line of field 2 carries source row 0 and the first of
  // field 1 row 1 (generation stage), so they must be woven in that order on
  // adjacent rows.
  const auto expect_adjacent = [](Standard standard, int upper, int lower) {
    EXPECT_EQ(LineNumberToFrameRow(standard, lower),
              LineNumberToFrameRow(standard, upper) + 1)
        << upper << " above " << lower;
  };
  expect_adjacent(Standard::kPal, 336, 23);
  expect_adjacent(Standard::kPal, 23, 337);
  expect_adjacent(Standard::kNtsc, 284, 22);
  expect_adjacent(Standard::kNtsc, 22, 285);
  expect_adjacent(Standard::kPalM, 284, 22);
}

TEST(PreviewRenderTest, FrameRowLineMappingRoundTrips) {
  for (int line : {1, 2, 312, 313, 314, 625}) {
    const int row = LineNumberToFrameRow(Standard::kPal, line);
    EXPECT_EQ(FrameRowToLineNumber(Standard::kPal, row), line)
        << "line " << line;
  }
  // Out-of-range rows clamp to the frame edges rather than crashing.
  EXPECT_EQ(FrameRowToLineNumber(Standard::kPal, -5), 313);
  EXPECT_EQ(FrameRowToLineNumber(Standard::kPal, 100000), 312);
}

TEST(PreviewRenderTest, EncodedFrameImageCoversWholeFrame) {
  const std::vector<SampleFixed> y_mv = MakeFrameBuffer(Standard::kPal, 0.0);
  const std::vector<SampleFixed> c_mv = MakeFrameBuffer(Standard::kPal, 0.0);
  const QImage frame = RenderEncodedFrameImage(y_mv, c_mv, Standard::kPal,
                                               EncodedImageMode::kComposite);
  ASSERT_FALSE(frame.isNull());
  EXPECT_EQ(frame.width(), 1135);
  EXPECT_EQ(frame.height(), 625);

  const std::vector<SampleFixed> ntsc = MakeFrameBuffer(Standard::kNtsc, 0.0);
  const QImage ntsc_frame = RenderEncodedFrameImage(
      ntsc, ntsc, Standard::kNtsc, EncodedImageMode::kComposite);
  ASSERT_FALSE(ntsc_frame.isNull());
  EXPECT_EQ(ntsc_frame.height(), 525);
}

TEST(PreviewRenderTest, EncodedFrameImageIsNullForUndersizedBuffers) {
  const std::vector<SampleFixed> too_small(100, 0);
  const QImage frame = RenderEncodedFrameImage(
      too_small, too_small, Standard::kPal, EncodedImageMode::kComposite);
  EXPECT_TRUE(frame.isNull());
}

TEST(PreviewRenderTest, EncodedPalFieldImagesHaveCorrectGeometry) {
  const std::vector<SampleFixed> y_mv = MakeFrameBuffer(Standard::kPal, 0.0);
  const std::vector<SampleFixed> c_mv = MakeFrameBuffer(Standard::kPal, 0.0);

  const QImage field1 = RenderEncodedFieldImage(y_mv, c_mv, Standard::kPal, 1,
                                                EncodedImageMode::kComposite);
  const QImage field2 = RenderEncodedFieldImage(y_mv, c_mv, Standard::kPal, 2,
                                                EncodedImageMode::kComposite);
  ASSERT_FALSE(field1.isNull());
  ASSERT_FALSE(field2.isNull());
  EXPECT_EQ(field1.width(), 1135);
  EXPECT_EQ(field1.height(), 312);
  EXPECT_EQ(field2.width(), 1135);
  EXPECT_EQ(field2.height(), 313);
}

TEST(PreviewRenderTest, EncodedNtscFieldImagesHaveCorrectGeometry) {
  const std::vector<SampleFixed> y_mv = MakeFrameBuffer(Standard::kNtsc, 0.0);
  const std::vector<SampleFixed> c_mv = MakeFrameBuffer(Standard::kNtsc, 0.0);

  const QImage field1 = RenderEncodedFieldImage(y_mv, c_mv, Standard::kNtsc, 1,
                                                EncodedImageMode::kComposite);
  const QImage field2 = RenderEncodedFieldImage(y_mv, c_mv, Standard::kNtsc, 2,
                                                EncodedImageMode::kComposite);
  ASSERT_FALSE(field1.isNull());
  ASSERT_FALSE(field2.isNull());
  EXPECT_EQ(field1.width(), 910);
  EXPECT_EQ(field1.height(), 262);
  EXPECT_EQ(field2.width(), 910);
  EXPECT_EQ(field2.height(), 263);
}

TEST(PreviewRenderTest, CompositeGrayscaleUsesTenBitCodeSpace) {
  // CVBS quantisation: PAL blanking (0 mV) maps to code 256 → gray 64;
  // NTSC blanking maps to code 240 → gray 60.
  const std::vector<SampleFixed> pal_y = MakeFrameBuffer(Standard::kPal, 0.0);
  const std::vector<SampleFixed> pal_c = MakeFrameBuffer(Standard::kPal, 0.0);
  const QImage pal_image = RenderEncodedFieldImage(
      pal_y, pal_c, Standard::kPal, 1, EncodedImageMode::kComposite);
  ASSERT_FALSE(pal_image.isNull());
  EXPECT_EQ(qGray(pal_image.pixel(100, 100)), 64);

  const std::vector<SampleFixed> ntsc_y = MakeFrameBuffer(Standard::kNtsc, 0.0);
  const std::vector<SampleFixed> ntsc_c = MakeFrameBuffer(Standard::kNtsc, 0.0);
  const QImage ntsc_image = RenderEncodedFieldImage(
      ntsc_y, ntsc_c, Standard::kNtsc, 1, EncodedImageMode::kComposite);
  ASSERT_FALSE(ntsc_image.isNull());
  EXPECT_EQ(qGray(ntsc_image.pixel(100, 100)), 60);
}

TEST(PreviewRenderTest, LumaWhiteAndChromaZeroMapToExpectedGray) {
  // PAL white (700 mV): code 256 + 700/1.1905 ≈ 844 → gray 211. Chroma zero
  // centres at code 512 → gray 128 (CVBS file format spec §3.2).
  const std::vector<SampleFixed> y_mv = MakeFrameBuffer(Standard::kPal, 700.0);
  const std::vector<SampleFixed> c_mv = MakeFrameBuffer(Standard::kPal, 0.0);

  const QImage luma = RenderEncodedFieldImage(y_mv, c_mv, Standard::kPal, 1,
                                              EncodedImageMode::kLuma);
  ASSERT_FALSE(luma.isNull());
  EXPECT_EQ(qGray(luma.pixel(0, 0)), 211);

  const QImage chroma = RenderEncodedFieldImage(y_mv, c_mv, Standard::kPal, 1,
                                                EncodedImageMode::kChroma);
  ASSERT_FALSE(chroma.isNull());
  EXPECT_EQ(qGray(chroma.pixel(0, 0)), 128);
}

TEST(PreviewRenderTest, UndersizedBuffersYieldNullImage) {
  const std::vector<SampleFixed> too_small(100, 0);
  const QImage image = RenderEncodedFieldImage(
      too_small, too_small, Standard::kPal, 1, EncodedImageMode::kComposite);
  EXPECT_TRUE(image.isNull());
}

TEST(PreviewRenderTest, ExtractLineMillivoltsFollowsPalLineLayout) {
  // EBU Tech. 3280-E Section 1.2: PAL lines 313 and 625 carry two extra
  // samples; all other lines are 1135 samples.
  std::vector<SampleFixed> y_mv = MakeFrameBuffer(Standard::kPal, 0.0);
  // Line 1 occupies samples [0, 1135); mark its last sample.
  y_mv[1134] = MillivoltsToSampleFixed(700.0);
  // Line 2 starts at sample 1135; mark its first sample.
  y_mv[1135] = MillivoltsToSampleFixed(-300.0);

  const std::vector<double> line1 =
      ExtractLineMillivolts(y_mv, Standard::kPal, 1);
  ASSERT_EQ(line1.size(), 1135U);
  EXPECT_NEAR(line1.back(), 700.0, 0.01);

  const std::vector<double> line2 =
      ExtractLineMillivolts(y_mv, Standard::kPal, 2);
  ASSERT_EQ(line2.size(), 1135U);
  EXPECT_NEAR(line2.front(), -300.0, 0.01);

  const std::vector<double> line313 =
      ExtractLineMillivolts(y_mv, Standard::kPal, 313);
  EXPECT_EQ(line313.size(), 1137U);

  EXPECT_TRUE(ExtractLineMillivolts(y_mv, Standard::kPal, 0).empty());
  EXPECT_TRUE(ExtractLineMillivolts(y_mv, Standard::kPal, 626).empty());
}

TEST(PreviewRenderTest, SourceImageConvertsBt601ToRgb) {
  FrameSourceImage source;
  source.width = 4;
  source.height = 2;
  source.active_x = 1;
  source.active_y = 0;
  source.active_width = 3;
  source.active_height = 2;
  source.pixels.assign(static_cast<std::size_t>(source.width) *
                           static_cast<std::size_t>(source.height),
                       YCbCr444Pixel{64, 512, 512});  // black

  // Active pixel (0,0): studio white.
  source.pixels[1] = YCbCr444Pixel{940, 512, 512};
  // Active pixel (1,0): mid grey pushed red by Cr = +0.5.
  source.pixels[2] = YCbCr444Pixel{502, 512, 960};

  const QImage image = RenderSourceImage(source);
  ASSERT_FALSE(image.isNull());
  EXPECT_EQ(image.width(), 3);
  EXPECT_EQ(image.height(), 2);

  const QRgb white = image.pixel(0, 0);
  EXPECT_EQ(qRed(white), 255);
  EXPECT_EQ(qGreen(white), 255);
  EXPECT_EQ(qBlue(white), 255);

  // ITU-R BT.601-7 Section 2.5.1: R' = Y' + 1.402 Cr' saturates at 1.0;
  // G' = 0.5 − 0.714136 × 0.5 ≈ 0.143; B' = Y' = 0.5.
  const QRgb red = image.pixel(1, 0);
  EXPECT_EQ(qRed(red), 255);
  EXPECT_NEAR(qGreen(red), 36, 1);
  EXPECT_NEAR(qBlue(red), 128, 1);

  const QRgb black = image.pixel(2, 1);
  EXPECT_EQ(qRed(black), 0);
  EXPECT_EQ(qGreen(black), 0);
  EXPECT_EQ(qBlue(black), 0);
}

TEST(PreviewRenderTest, EmptySourceYieldsNullImage) {
  EXPECT_TRUE(RenderSourceImage(FrameSourceImage{}).isNull());
}

}  // namespace
}  // namespace videosynth::gui
