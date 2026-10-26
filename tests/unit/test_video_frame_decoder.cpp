/*
 * File:        test_video_frame_decoder.cpp
 * Module:      video_frame_decoder_tests
 * Purpose:     Unit tests for yuv422p10le unpacking and the ffmpeg decode
 *              command line, using in-memory buffers only.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "videosynth/progressive_frame_source.h"
#include "videosynth/video_frame_decoder.h"

namespace videosynth {
namespace {

// Planar yuv422p10le samples for a width x height frame where each plane
// sample encodes its own position, so siting errors show up as wrong codes.
std::vector<std::uint16_t> MakePositionCodedFrame(int width, int height) {
  std::vector<std::uint16_t> samples;
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      samples.push_back(static_cast<std::uint16_t>(100 + (y * width) + x));
    }
  }
  for (int plane = 0; plane < 2; ++plane) {
    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width / 2; ++x) {
        samples.push_back(static_cast<std::uint16_t>((plane == 0 ? 300 : 600) +
                                                     (y * (width / 2)) + x));
      }
    }
  }
  return samples;
}

TEST(VideoFrameDecoderTest, UnpackRepeatsEachChromaSampleForItsLumaPair) {
  constexpr int kWidth = 4;
  constexpr int kHeight = 2;
  const std::vector<std::uint16_t> samples =
      MakePositionCodedFrame(kWidth, kHeight);

  FrameSourceImage image;
  std::string error;
  ASSERT_TRUE(UnpackYuv422p10leFrame(samples.data(), samples.size(), kWidth,
                                     kHeight, &image, &error))
      << error;

  EXPECT_EQ(image.width, kWidth);
  EXPECT_EQ(image.height, kHeight);
  EXPECT_EQ(image.active_width, kWidth);
  EXPECT_EQ(image.active_height, kHeight);
  for (int y = 0; y < kHeight; ++y) {
    for (int x = 0; x < kWidth; ++x) {
      const YCbCr444Pixel& pixel = image.PixelAt(x, y);
      const int chroma_index = (y * (kWidth / 2)) + (x / 2);
      EXPECT_EQ(pixel.y, 100 + (y * kWidth) + x);
      EXPECT_EQ(pixel.cb, 300 + chroma_index) << "x=" << x << " y=" << y;
      EXPECT_EQ(pixel.cr, 600 + chroma_index) << "x=" << x << " y=" << y;
    }
  }
}

TEST(VideoFrameDecoderTest, UnpackKeepsOnlyTheLowTenBits) {
  constexpr int kWidth = 2;
  constexpr int kHeight = 1;
  // Y0, Y1, Cb, Cr with junk in the upper six bits.
  const std::vector<std::uint16_t> samples = {0xFC40, 0x03AC, 0xFE00, 0x0400};

  FrameSourceImage image;
  std::string error;
  ASSERT_TRUE(UnpackYuv422p10leFrame(samples.data(), samples.size(), kWidth,
                                     kHeight, &image, &error));
  EXPECT_EQ(image.PixelAt(0, 0).y, 0x040);
  EXPECT_EQ(image.PixelAt(1, 0).y, 0x3AC);
  EXPECT_EQ(image.PixelAt(0, 0).cb, 0x200);
  EXPECT_EQ(image.PixelAt(0, 0).cr, 0x000);
}

TEST(VideoFrameDecoderTest, UnpackRejectsMismatchedPayloadOrGeometry) {
  const std::vector<std::uint16_t> samples(16, 512);
  FrameSourceImage image;
  std::string error;
  EXPECT_FALSE(UnpackYuv422p10leFrame(samples.data(), samples.size() - 1, 4, 2,
                                      &image, &error));
  EXPECT_FALSE(error.empty());
  error.clear();
  EXPECT_FALSE(UnpackYuv422p10leFrame(samples.data(), samples.size(), 3, 2,
                                      &image, &error));
  EXPECT_FALSE(error.empty());
  EXPECT_FALSE(UnpackYuv422p10leFrame(samples.data(), samples.size(), 4, 2,
                                      nullptr, &error));
}

TEST(VideoFrameDecoderTest, CommandFromFrameZeroHasNoSeekOrFilter) {
  VideoDecodeRequest request;
  request.source = "/media/clip.mkv";
  request.standard = Standard::kPal;
  const std::string command = FfmpegVideoFrameDecoder::BuildCommand(request);
  EXPECT_EQ(command.find("-ss"), std::string::npos);
  EXPECT_EQ(command.find("-vf"), std::string::npos);
  EXPECT_NE(command.find("-i '/media/clip.mkv'"), std::string::npos);
  EXPECT_NE(command.find("-map 0:v:0"), std::string::npos);
  EXPECT_NE(command.find("-pix_fmt yuv422p10le"), std::string::npos);
  EXPECT_NE(command.find("-f rawvideo -"), std::string::npos);
}

TEST(VideoFrameDecoderTest, SeekLandsHalfAFrameBeforeTheWantedFrame) {
  VideoDecodeRequest request;
  request.source = "clip.mp4";
  request.standard = Standard::kPal;
  request.first_frame = 10;
  // (10 - 0.5) / 25 = 0.38 s.
  EXPECT_NE(
      FfmpegVideoFrameDecoder::BuildCommand(request).find("-ss 0.380000 -i"),
      std::string::npos);

  request.standard = Standard::kNtsc;
  request.first_frame = 1;
  // 0.5 / (30000 / 1001) = 0.016683 s.
  EXPECT_NE(
      FfmpegVideoFrameDecoder::BuildCommand(request).find("-ss 0.016683 -i"),
      std::string::npos);
}

TEST(VideoFrameDecoderTest, SeekIncludesVideoStartOffset) {
  VideoDecodeRequest request;
  request.source = "clip.mp4";
  request.standard = Standard::kPal;
  request.first_frame = 1;
  request.video_start_offset_seconds = 0.1;
  EXPECT_NE(
      FfmpegVideoFrameDecoder::BuildCommand(request).find("-ss 0.120000 -i"),
      std::string::npos);
}

TEST(VideoFrameDecoderTest, FilterChainAndQuotesAreShellEscaped) {
  VideoDecodeRequest request;
  request.source = "/media/it's here.mkv";
  request.standard = Standard::kPal;
  request.filter_chain = "scale=in_range=pc:out_range=tv,format=yuv422p10le";
  const std::string command = FfmpegVideoFrameDecoder::BuildCommand(request);
  EXPECT_NE(command.find("-i '/media/it'\\''s here.mkv'"), std::string::npos);
  EXPECT_NE(
      command.find("-vf 'scale=in_range=pc:out_range=tv,format=yuv422p10le'"),
      std::string::npos);
}

TEST(VideoFrameDecoderTest, ReadBeforeOpenIsAnError) {
  FfmpegVideoFrameDecoder decoder;
  FrameSourceImage image;
  std::string error;
  EXPECT_EQ(decoder.ReadFrame(&image, &error), VideoFrameReadStatus::kError);
  EXPECT_FALSE(error.empty());
}

}  // namespace
}  // namespace videosynth
