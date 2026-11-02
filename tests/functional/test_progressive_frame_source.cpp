/*
 * File:        test_progressive_frame_source.cpp
 * Module:      progressive_frame_source_tests
 * Purpose:     Validates progressive frame-source decoding and normalization.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "videosynth/progressive_frame_source.h"
#include "videosynth/progressive_frame_source_probe.h"
#include "videosynth/video_frame_decoder.h"

namespace videosynth {
namespace {

void ExpectCodesWithinStudioRange(const FrameSourceImage& image) {
  for (int y = image.active_y; y < image.active_y + image.active_height; ++y) {
    for (int x = image.active_x; x < image.active_x + image.active_width; ++x) {
      const YCbCr444Pixel& pixel = image.PixelAt(x, y);
      EXPECT_GE(pixel.y, 64) << "x=" << x << " y=" << y;
      EXPECT_LE(pixel.y, 940) << "x=" << x << " y=" << y;
      EXPECT_GE(pixel.cb, 64) << "x=" << x << " y=" << y;
      EXPECT_LE(pixel.cb, 960) << "x=" << x << " y=" << y;
      EXPECT_GE(pixel.cr, 64) << "x=" << x << " y=" << y;
      EXPECT_LE(pixel.cr, 960) << "x=" << x << " y=" << y;
    }
  }
}

TEST(FrameSourceTest, DecodesProgressivePalExrSource) {
  ProgressiveFrameSource frame_source;
  std::shared_ptr<const FrameSourceImage> image;
  std::string error;

  Section section;
  section.type = "progressive";
  section.source = (std::filesystem::path(VIDEOSYNTH_SOURCE_DIR) /
                    "videosynth-assets/assets/exr/720x576/100_BARS.exr")
                       .string();

  ASSERT_TRUE(
      frame_source.GenerateFrame(section, 0, Standard::kPal, &image, &error));
  EXPECT_TRUE(error.empty());
  EXPECT_EQ(image->width, 720);
  EXPECT_EQ(image->height, 576);
  EXPECT_EQ(image->active_x, 0);
  EXPECT_EQ(image->active_y, 0);
  EXPECT_EQ(image->active_width, 720);
  EXPECT_EQ(image->active_height, 576);
  const std::size_t center_x = image->active_x + (image->active_width / 2);
  const std::size_t center_y = image->active_y + (image->active_height / 2);
  EXPECT_NE(image->PixelAt(center_x, center_y).y, 64);
}

TEST(FrameSourceTest, DecodesProgressiveNtscExrSource) {
  ProgressiveFrameSource frame_source;
  std::shared_ptr<const FrameSourceImage> image;
  std::string error;

  Section section;
  section.type = "progressive";
  section.source = (std::filesystem::path(VIDEOSYNTH_SOURCE_DIR) /
                    "videosynth-assets/assets/exr/720x486/100_BARS.exr")
                       .string();

  ASSERT_TRUE(
      frame_source.GenerateFrame(section, 0, Standard::kNtsc, &image, &error));
  EXPECT_TRUE(error.empty());
  EXPECT_EQ(image->width, 720);
  EXPECT_EQ(image->height, 486);
  EXPECT_EQ(image->active_x, 0);
  EXPECT_EQ(image->active_y, 0);
  EXPECT_EQ(image->active_width, 720);
  EXPECT_EQ(image->active_height, 486);
  const std::size_t center_x = image->active_x + (image->active_width / 2);
  const std::size_t center_y = image->active_y + (image->active_height / 2);
  EXPECT_NE(image->PixelAt(center_x, center_y).y, 64);
}

TEST(FrameSourceTest, RejectsProgressiveRawSourceFamily) {
  ProgressiveFrameSource frame_source;
  std::shared_ptr<const FrameSourceImage> image;
  std::string error;

  Section section;
  section.type = "progressive";
  section.source = (std::filesystem::path(VIDEOSYNTH_SOURCE_DIR) /
                    "videosynth-assets/assets/exr/720x576/100_BARS.exr")
                       .string();
  section.source.replace(section.source.size() - 4, 4, ".raw");

  EXPECT_FALSE(
      frame_source.GenerateFrame(section, 0, Standard::kPal, &image, &error));
  EXPECT_FALSE(error.empty());
}

TEST(FrameSourceTest, DecodesProgressivePalMkvSourceFrames) {
  ProgressiveFrameSource frame_source;
  std::string error;

  Section section;
  section.type = "progressive";
  section.source = (std::filesystem::path(VIDEOSYNTH_SOURCE_DIR) /
                    "videosynth-assets/assets/mkv/720x576/MOVING_ZONE_2H.mkv")
                       .string();

  int frame_count = 0;
  ASSERT_TRUE(frame_source.ResolveFrameCount(section, Standard::kPal,
                                             &frame_count, &error));
  ASSERT_GT(frame_count, 0);

  std::shared_ptr<const FrameSourceImage> first_frame;
  ASSERT_TRUE(frame_source.GenerateFrame(section, 0, Standard::kPal,
                                         &first_frame, &error));
  EXPECT_EQ(first_frame->width, 720);
  EXPECT_EQ(first_frame->height, 576);
  EXPECT_EQ(first_frame->active_x, 0);
  EXPECT_EQ(first_frame->active_y, 0);
  EXPECT_EQ(first_frame->active_width, 720);
  EXPECT_EQ(first_frame->active_height, 576);
}

TEST(FrameSourceTest, DecodesProgressiveNtscMkvSourceFrames) {
  ProgressiveFrameSource frame_source;
  std::string error;

  Section section;
  section.type = "progressive";
  section.source = (std::filesystem::path(VIDEOSYNTH_SOURCE_DIR) /
                    "videosynth-assets/assets/mkv/720x486/MOVING_ZONE_2H.mkv")
                       .string();

  int frame_count = 0;
  ASSERT_TRUE(frame_source.ResolveFrameCount(section, Standard::kNtsc,
                                             &frame_count, &error));
  ASSERT_GT(frame_count, 0);

  std::shared_ptr<const FrameSourceImage> first_frame;
  ASSERT_TRUE(frame_source.GenerateFrame(section, 0, Standard::kNtsc,
                                         &first_frame, &error));
  EXPECT_EQ(first_frame->width, 720);
  EXPECT_EQ(first_frame->height, 486);
  EXPECT_EQ(first_frame->active_x, 0);
  EXPECT_EQ(first_frame->active_y, 0);
  EXPECT_EQ(first_frame->active_width, 720);
  EXPECT_EQ(first_frame->active_height, 486);
  EXPECT_NE(first_frame->PixelAt(0, 0).y, first_frame->PixelAt(8, 0).y);
}

TEST(FrameSourceTest, NtscMkvPreservesFullRasterActiveGeometry) {
  ProgressiveFrameSource frame_source;
  std::string error;

  Section section;
  section.type = "progressive";
  section.source = (std::filesystem::path(VIDEOSYNTH_SOURCE_DIR) /
                    "videosynth-assets/assets/mkv/720x486/MOVING_ZONE_2H.mkv")
                       .string();

  std::shared_ptr<const FrameSourceImage> frame;
  ASSERT_TRUE(
      frame_source.GenerateFrame(section, 0, Standard::kNtsc, &frame, &error));
  EXPECT_TRUE(error.empty());

  EXPECT_EQ(frame->width, 720);
  EXPECT_EQ(frame->height, 486);
  EXPECT_EQ(frame->active_x, 0);
  EXPECT_EQ(frame->active_width, 720);
  EXPECT_EQ(frame->active_y, 0);
  EXPECT_EQ(frame->active_height, 486);
}

TEST(FrameSourceTest, PalAndNtscMkvDecodedPixelsStayWithinStudioCodeRange) {
  ProgressiveFrameSource frame_source;
  std::string error;

  Section pal_section;
  pal_section.type = "progressive";
  pal_section.source =
      (std::filesystem::path(VIDEOSYNTH_SOURCE_DIR) /
       "videosynth-assets/assets/mkv/720x576/MOVING_ZONE_2H.mkv")
          .string();
  std::shared_ptr<const FrameSourceImage> pal_frame;
  ASSERT_TRUE(frame_source.GenerateFrame(pal_section, 0, Standard::kPal,
                                         &pal_frame, &error));
  EXPECT_TRUE(error.empty());
  ExpectCodesWithinStudioRange(*pal_frame);

  Section ntsc_section;
  ntsc_section.type = "progressive";
  ntsc_section.source =
      (std::filesystem::path(VIDEOSYNTH_SOURCE_DIR) /
       "videosynth-assets/assets/mkv/720x486/MOVING_ZONE_2H.mkv")
          .string();
  std::shared_ptr<const FrameSourceImage> ntsc_frame;
  ASSERT_TRUE(frame_source.GenerateFrame(ntsc_section, 0, Standard::kNtsc,
                                         &ntsc_frame, &error));
  EXPECT_TRUE(error.empty());
  ExpectCodesWithinStudioRange(*ntsc_frame);
}

TEST(FrameSourceTest, RejectsProgressiveMkvFrameIndexOutOfRange) {
  ProgressiveFrameSource frame_source;
  std::string error;

  Section section;
  section.type = "progressive";
  section.source = (std::filesystem::path(VIDEOSYNTH_SOURCE_DIR) /
                    "videosynth-assets/assets/mkv/720x486/MOVING_ZONE_2H.mkv")
                       .string();

  int frame_count = 0;
  ASSERT_TRUE(frame_source.ResolveFrameCount(section, Standard::kNtsc,
                                             &frame_count, &error));
  ASSERT_GT(frame_count, 0);

  std::shared_ptr<const FrameSourceImage> out_of_range;
  EXPECT_FALSE(frame_source.GenerateFrame(section, frame_count, Standard::kNtsc,
                                          &out_of_range, &error));
  EXPECT_FALSE(error.empty());
}

TEST(FrameSourceTest, RepeatedRequestsShareOneDecodedImage) {
  ProgressiveFrameSource frame_source;
  std::string error;

  Section section;
  section.type = "progressive";
  section.source = (std::filesystem::path(VIDEOSYNTH_SOURCE_DIR) /
                    "videosynth-assets/assets/exr/720x576/100_BARS.exr")
                       .string();

  std::shared_ptr<const FrameSourceImage> first;
  std::shared_ptr<const FrameSourceImage> second;
  ASSERT_TRUE(
      frame_source.GenerateFrame(section, 0, Standard::kPal, &first, &error));
  ASSERT_TRUE(
      frame_source.GenerateFrame(section, 0, Standard::kPal, &second, &error));

  // A cache hit hands back the same image rather than a fresh copy of it.
  EXPECT_EQ(first.get(), second.get());
}

TEST(FrameSourceTest, DeliveredImageOutlivesTheCache) {
  ProgressiveFrameSource frame_source;
  std::string error;

  Section section;
  section.type = "progressive";
  section.source = (std::filesystem::path(VIDEOSYNTH_SOURCE_DIR) /
                    "videosynth-assets/assets/exr/720x576/100_BARS.exr")
                       .string();

  std::shared_ptr<const FrameSourceImage> frame;
  ASSERT_TRUE(
      frame_source.GenerateFrame(section, 0, Standard::kPal, &frame, &error));
  const std::int16_t centre_luma =
      frame->PixelAt(frame->width / 2, frame->height / 2).y;

  frame_source.ClearCache();

  // The caller's reference keeps the image alive and unchanged.
  ASSERT_NE(frame, nullptr);
  EXPECT_EQ(frame->PixelAt(frame->width / 2, frame->height / 2).y, centre_luma);
}

TEST(FrameSourceTest, PrefetchedSourceIsServedFromTheCache) {
  ProgressiveFrameSource frame_source;
  std::string error;

  Section section;
  section.type = "progressive";
  section.source = (std::filesystem::path(VIDEOSYNTH_SOURCE_DIR) /
                    "videosynth-assets/assets/exr/720x576/75_BARS.exr")
                       .string();

  frame_source.PrefetchSection(section, 0, Standard::kPal);

  // The request is served from the cache once the prefetch lands, and from a
  // decode of its own if it arrives first — either way it returns the same
  // image the next request sees, which is what makes the prefetch invisible.
  std::shared_ptr<const FrameSourceImage> first;
  std::shared_ptr<const FrameSourceImage> second;
  ASSERT_TRUE(
      frame_source.GenerateFrame(section, 0, Standard::kPal, &first, &error))
      << error;
  ASSERT_TRUE(
      frame_source.GenerateFrame(section, 0, Standard::kPal, &second, &error));
  EXPECT_EQ(first.get(), second.get());
  EXPECT_EQ(first->width, 720);
  EXPECT_EQ(first->height, 576);
}

TEST(FrameSourceTest, PrefetchOfAnUnreadableSourceLeavesTheRequestPathIntact) {
  ProgressiveFrameSource frame_source;
  std::string error;

  Section missing;
  missing.type = "progressive";
  missing.source = (std::filesystem::path(VIDEOSYNTH_SOURCE_DIR) /
                    "videosynth-assets/assets/exr/720x576/DOES_NOT_EXIST.exr")
                       .string();

  // A failed prefetch is dropped silently; the real request still reports the
  // failure itself.
  frame_source.PrefetchSection(missing, 0, Standard::kPal);
  std::shared_ptr<const FrameSourceImage> image;
  EXPECT_FALSE(
      frame_source.GenerateFrame(missing, 0, Standard::kPal, &image, &error));
  EXPECT_FALSE(error.empty());

  // An unsupported section is not queued at all.
  Section unsupported;
  unsupported.type = "progressive";
  unsupported.source = "not-a-media-file.txt";
  frame_source.PrefetchSection(unsupported, 0, Standard::kPal);
}

TEST(FrameSourceTest, TwoSourcesStayCachedAcrossAlternatingRequests) {
  ProgressiveFrameSource frame_source;
  std::string error;

  Section pal_section;
  pal_section.type = "progressive";
  pal_section.source = (std::filesystem::path(VIDEOSYNTH_SOURCE_DIR) /
                        "videosynth-assets/assets/exr/720x576/100_BARS.exr")
                           .string();

  Section second_section;
  second_section.type = "progressive";
  second_section.source = (std::filesystem::path(VIDEOSYNTH_SOURCE_DIR) /
                           "videosynth-assets/assets/exr/720x576/75_BARS.exr")
                              .string();

  std::shared_ptr<const FrameSourceImage> first_a;
  std::shared_ptr<const FrameSourceImage> b;
  std::shared_ptr<const FrameSourceImage> second_a;
  ASSERT_TRUE(frame_source.GenerateFrame(pal_section, 0, Standard::kPal,
                                         &first_a, &error));
  ASSERT_TRUE(
      frame_source.GenerateFrame(second_section, 0, Standard::kPal, &b, &error))
      << error;
  ASSERT_TRUE(frame_source.GenerateFrame(pal_section, 0, Standard::kPal,
                                         &second_a, &error));

  // Both sources fit the cache, so straddling a section boundary does not
  // force a re-decode of the section still in use.
  EXPECT_EQ(first_a.get(), second_a.get());
  EXPECT_NE(first_a.get(), b.get());
}

// A decoder opened part-way into a 24000/1001 film clip on an NTSC project
// must seek with the clip's own rate: the standard's 30000/1001 rate would
// land on frame 0.8 × N. The window capacity of 1 makes the seeked source
// reopen its decoder at the requested frame.
TEST(FrameSourceTest, SeekIntoFilmClipReturnsTheRequestedFrame) {
  Section section;
  section.type = "progressive";
  section.source = (std::filesystem::path(VIDEOSYNTH_SOURCE_DIR) /
                    "videosynth-assets/assets/mkv/720x486/FILM_COUNTER_24P.mkv")
                       .string();
  section.cadence.pattern = CadencePattern::kPulldown23;
  constexpr int kFrame = 40;

  ProgressiveFrameSource sequential;
  std::string error;
  std::shared_ptr<const FrameSourceImage> expected;
  for (int frame = 0; frame <= kFrame; ++frame) {
    ASSERT_TRUE(sequential.GenerateFrame(section, frame, Standard::kNtsc,
                                         &expected, &error))
        << error;
  }

  ProgressiveFrameSource seeking(
      std::make_shared<ProgressiveFrameSourceProbe>(),
      []() { return std::make_unique<FfmpegVideoFrameDecoder>(); }, 1U);
  section.start_frame = kFrame;
  std::shared_ptr<const FrameSourceImage> sought;
  ASSERT_TRUE(
      seeking.GenerateFrame(section, kFrame, Standard::kNtsc, &sought, &error))
      << error;
  ASSERT_EQ(sought->pixels.size(), expected->pixels.size());
  for (std::size_t i = 0; i < sought->pixels.size(); ++i) {
    const YCbCr444Pixel& a = sought->pixels[i];
    const YCbCr444Pixel& b = expected->pixels[i];
    ASSERT_TRUE(a.y == b.y && a.cb == b.cb && a.cr == b.cr) << "pixel " << i;
  }
}

}  // namespace
}  // namespace videosynth