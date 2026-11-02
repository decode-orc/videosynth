/*
 * File:        test_field_offset_filter.cpp
 * Module:      channel_tests
 * Purpose:     Verifies the picture-domain source offset used for the field-2
 *              misregistration: whole offsets are exact shifts, fractional
 *              offsets interpolate, edges hold and the border is untouched.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "videosynth/field_offset_filter.h"
#include "videosynth/progressive_frame_source.h"

namespace videosynth {
namespace {

// A 40x20 image whose 32x16 active region starts at (4, 2). Inside it Y is a
// horizontal ramp, Cb a vertical ramp and Cr constant; the border is marked.
FrameSourceImage MakeRampImage() {
  FrameSourceImage image;
  image.width = 40;
  image.height = 20;
  image.active_x = 4;
  image.active_y = 2;
  image.active_width = 32;
  image.active_height = 16;
  image.pixels.resize(static_cast<std::size_t>(image.width) *
                      static_cast<std::size_t>(image.height));
  for (int y = 0; y < image.height; ++y) {
    for (int x = 0; x < image.width; ++x) {
      YCbCr444Pixel& pixel =
          image.pixels[(static_cast<std::size_t>(y) * image.width) + x];
      const bool active = x >= 4 && x < 36 && y >= 2 && y < 18;
      pixel = active ? YCbCr444Pixel{.y = static_cast<std::int16_t>(
                                         100 + (10 * (x - 4))),
                                     .cb = static_cast<std::int16_t>(
                                         300 + (20 * (y - 2))),
                                     .cr = 600}
                     : YCbCr444Pixel{.y = 7, .cb = 8, .cr = 9};
    }
  }
  return image;
}

TEST(FieldOffsetFilterTest, ZeroOffsetIsTheIdentity) {
  const FrameSourceImage image = MakeRampImage();
  const FrameSourceImage moved = OffsetSourceImage(image, 0.0, 0.0);
  for (std::size_t i = 0; i < image.pixels.size(); ++i) {
    ASSERT_EQ(moved.pixels[i].y, image.pixels[i].y) << i;
    ASSERT_EQ(moved.pixels[i].cb, image.pixels[i].cb) << i;
    ASSERT_EQ(moved.pixels[i].cr, image.pixels[i].cr) << i;
  }
}

TEST(FieldOffsetFilterTest, WholeOffsetsAreExactShiftsThatHoldTheEdges) {
  const FrameSourceImage image = MakeRampImage();
  const FrameSourceImage moved = OffsetSourceImage(image, 2.0, -1.0);
  for (int y = 2; y < 18; ++y) {
    for (int x = 4; x < 36; ++x) {
      const int source_x = std::clamp(x - 2, 4, 35);
      const int source_y = std::clamp(y + 1, 2, 17);
      ASSERT_EQ(moved.PixelAt(x, y).y, image.PixelAt(source_x, source_y).y);
      ASSERT_EQ(moved.PixelAt(x, y).cb, image.PixelAt(source_x, source_y).cb);
    }
  }
  // The border outside the active region is copied unchanged.
  EXPECT_EQ(moved.PixelAt(0, 0).y, 7);
  EXPECT_EQ(moved.PixelAt(39, 19).cr, 9);
}

TEST(FieldOffsetFilterTest, FractionalOffsetsInterpolateRampsAndKeepFlats) {
  const FrameSourceImage image = MakeRampImage();
  const FrameSourceImage moved = OffsetSourceImage(image, 0.5, 0.25);
  // Interior pixels, clear of the edges the kernel would reach.
  for (int y = 11; y < 13; ++y) {
    for (int x = 16; x < 24; ++x) {
      // Y ramps 10 codes per pixel, so half a pixel right lowers it by 5; Cb
      // ramps 20 per row, so a quarter row down lowers it by 5.
      EXPECT_NEAR(moved.PixelAt(x, y).y, image.PixelAt(x, y).y - 5, 1);
      EXPECT_NEAR(moved.PixelAt(x, y).cb, image.PixelAt(x, y).cb - 5, 1);
      EXPECT_EQ(moved.PixelAt(x, y).cr, 600);
    }
  }
}

}  // namespace
}  // namespace videosynth
