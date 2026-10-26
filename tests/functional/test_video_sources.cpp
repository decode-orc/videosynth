/*
 * File:        test_video_sources.cpp
 * Module:      progressive_frame_source_tests
 * Purpose:     Functional tests for general MKV/MP4 ingestion, colour
 *              normalisation, exact seeking and the bundled BLACK.exr, over
 *              clips generated with ffmpeg at test time.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>
#include <unistd.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "videosynth/fixed_point.h"
#include "videosynth/generation_stage.h"
#include "videosynth/progressive_frame_source.h"
#include "videosynth/progressive_frame_source_probe.h"
#include "videosynth/source_colour_conversion.h"
#include "videosynth/timing_constants.h"
#include "videosynth/video_frame_decoder.h"

namespace videosynth {
namespace {

// Generated media goes to the {output} root (a scratch directory), never into
// the source tree.
std::filesystem::path ClipDirectory() {
  return std::filesystem::temp_directory_path() / "videosynth_video_sources";
}

// Generates `name` once per test process with `ffmpeg -v error -y <args>`.
// ctest runs each test in its own process, possibly in parallel, so the clip
// is written under a process-unique name and renamed into place: a concurrent
// reader sees either a complete earlier copy or the complete new one.
std::string Clip(const std::string& name, const std::string& ffmpeg_args) {
  static std::mutex mutex;
  static std::set<std::string> generated;
  const std::lock_guard<std::mutex> lock(mutex);
  const std::filesystem::path path = ClipDirectory() / name;
  if (generated.count(name) == 0U) {
    std::filesystem::create_directories(ClipDirectory());
    const std::filesystem::path staging =
        ClipDirectory() / (std::to_string(getpid()) + "_" + name);
    const std::string command = "ffmpeg -v error -y -nostdin " + ffmpeg_args +
                                " '" + staging.string() + "'";
    EXPECT_EQ(std::system(command.c_str()), 0) << command;
    std::filesystem::rename(staging, path);
    generated.insert(name);
  }
  return path.string();
}

std::string PalTestPatternArgs() {
  return "-f lavfi -i testsrc2=s=720x576:r=25:d=4";
}

std::string NtscTestPatternArgs() {
  return "-f lavfi -i testsrc2=s=720x486:r=30000/1001:d=4";
}

// 100 PAL frames, long GOP with B-frames, plus a mono AAC soundtrack.
std::string H264Mp4WithAudio() {
  return Clip("h264_pal_audio.mp4",
              PalTestPatternArgs() +
                  " -f lavfi -i sine=f=1000:d=4:sample_rate=48000"
                  " -c:v libx264 -g 12 -bf 2 -pix_fmt yuv420p -c:a aac");
}

// 120 NTSC frames of H.264 in Matroska, no audio.
std::string H264NtscMkv() {
  return Clip(
      "h264_ntsc.mkv",
      NtscTestPatternArgs() + " -c:v libx264 -g 12 -bf 2 -pix_fmt yuv420p");
}

// 100 PAL frames of FFV1 already at the BT.601 decode target.
std::string Ffv1PalMkv() {
  return Clip("ffv1_pal.mkv",
              PalTestPatternArgs() +
                  " -c:v ffv1 -pix_fmt yuv422p10le -color_range tv"
                  " -colorspace smpte170m");
}

// Full-range 8-bit luma bars: Y 0 | 128 | 255 across thirds of the line.
std::string FullRangeLumaBars() {
  return Clip(
      "full_range_bars.mkv",
      "-f lavfi -i \"nullsrc=s=720x576:r=25:d=0.2,format=yuv420p,"
      "geq=lum=if(lt(X\\,240)\\,0\\,if(lt(X\\,480)\\,128\\,255)):"
      "cb=128:cr=128\" -c:v ffv1 -color_range pc -colorspace smpte170m");
}

// Red | green | blue | white bars coded with the BT.709 matrix at 10 bits.
std::string Bt709Bars() {
  return Clip("bt709_bars.mkv",
              "-f lavfi -i \"color=c=red:s=180x576:r=25:d=0.2[a];"
              "color=c=lime:s=180x576:r=25:d=0.2[b];"
              "color=c=blue:s=180x576:r=25:d=0.2[c];"
              "color=c=white:s=180x576:r=25:d=0.2[d];[a][b][c][d]hstack=4,"
              "scale=out_color_matrix=bt709:out_range=tv:flags=accurate_rnd+"
              "full_chroma_int,format=yuv444p10le\" -c:v ffv1 -colorspace bt709"
              " -color_range tv -color_primaries bt709 -color_trc bt709");
}

// A solid red RGB-coded clip (PNG in Matroska). The colour source is asked
// for rgb24 directly so the clip holds exactly R=255, G=B=0.
std::string RgbPngMkv() {
  return Clip("rgb_png.mkv",
              "-f lavfi -i color=c=red:s=720x576:r=25:d=0.2,format=rgb24 "
              "-c:v png "
              "-pix_fmt rgb24");
}

Section VideoSection(const std::string& source) {
  Section section;
  section.name = "Clip";
  section.type = "progressive";
  section.source = source;
  section.duration_frames_all = true;
  return section;
}

std::string BlackExrPath(Standard standard) {
  return (std::filesystem::path(VIDEOSYNTH_SOURCE_DIR) /
          (Is625LineStandard(standard)
               ? "videosynth-assets/assets/exr/720x576/BLACK.exr"
               : "videosynth-assets/assets/exr/720x486/BLACK.exr"))
      .string();
}

// ITU-R BT.601-5 Section 3.5.1-3.5.3: 10-bit studio coding of normalised
// R'G'B' (Y' = 219 E_Y x 4 + 64, colour difference 224 E_C x 4 + 512).
YCbCr444Pixel Bt601Codes(double r, double g, double b) {
  const double y = (0.299 * r) + (0.587 * g) + (0.114 * b);
  const double cb = (-0.168736 * r) - (0.331264 * g) + (0.5 * b);
  const double cr = (0.5 * r) - (0.418688 * g) - (0.081312 * b);
  return YCbCr444Pixel{
      .y = static_cast<std::int16_t>(std::lround(64.0 + (876.0 * y))),
      .cb = static_cast<std::int16_t>(std::lround(512.0 + (896.0 * cb))),
      .cr = static_cast<std::int16_t>(std::lround(512.0 + (896.0 * cr))),
  };
}

std::shared_ptr<const FrameSourceImage> DecodeFrame(const std::string& source,
                                                    Standard standard,
                                                    int frame_index) {
  ProgressiveFrameSource frame_source;
  std::shared_ptr<const FrameSourceImage> image;
  std::string error;
  EXPECT_TRUE(frame_source.GenerateFrame(VideoSection(source), frame_index,
                                         standard, &image, &error))
      << error;
  return image;
}

void ExpectPixelNear(const YCbCr444Pixel& actual, const YCbCr444Pixel& expected,
                     int tolerance, const std::string& label) {
  EXPECT_NEAR(actual.y, expected.y, tolerance) << label << " Y";
  EXPECT_NEAR(actual.cb, expected.cb, tolerance) << label << " Cb";
  EXPECT_NEAR(actual.cr, expected.cr, tolerance) << label << " Cr";
}

// --- BLACK.exr ------------------------------------------------------------

TEST(VideoSourcesTest, BlackExrDecodesToStudioBlackInEveryPixel) {
  for (Standard standard : {Standard::kPal, Standard::kNtsc}) {
    ProgressiveFrameSource frame_source;
    Section section = VideoSection(BlackExrPath(standard));
    section.duration_frames_all = false;
    section.duration_frames = 1;
    std::shared_ptr<const FrameSourceImage> image;
    std::string error;
    ASSERT_TRUE(
        frame_source.GenerateFrame(section, 0, standard, &image, &error))
        << error;
    ASSERT_EQ(image->pixels.size(),
              720U * (standard == Standard::kPal ? 576U : 486U));
    int mismatches = 0;
    for (const YCbCr444Pixel& pixel : image->pixels) {
      if (pixel.y != 64 || pixel.cb != 512 || pixel.cr != 512) {
        ++mismatches;
      }
    }
    EXPECT_EQ(mismatches, 0);
  }
}

// Mean of the luma signal over the middle of an active line.
double MidLineLumaMillivolts(const std::vector<SampleFixed>& y_mv,
                             Standard standard, int line_1based) {
  const TimingConstants timing = GetTimingConstants(standard);
  const std::size_t line_start =
      static_cast<std::size_t>(line_1based - 1) *
      static_cast<std::size_t>(timing.samples_per_line_4fsc);
  // 30-45 µs after 0H sits well inside the active line in both systems.
  const auto first = static_cast<std::size_t>(
      std::lround(timing.sample_rate_4fsc_hz * 30.0e-6));
  const auto last = static_cast<std::size_t>(
      std::lround(timing.sample_rate_4fsc_hz * 45.0e-6));
  double sum = 0.0;
  for (std::size_t i = first; i < last; ++i) {
    sum += SampleFixedToMillivolts(y_mv[line_start + i]);
  }
  return sum / static_cast<double>(last - first);
}

Project MakeBlackProject(Standard standard) {
  Project project;
  project.cvbs_presets.video_standard_preset = standard;
  project.cvbs_presets.sample_encoding_preset = "CVBS_U10_4FSC";
  project.cvbs_presets.signal_state_preset = "STANDARD_STABLE_LOCKED";
  project.sections.push_back(Section{.name = "Black",
                                     .type = "progressive",
                                     .line_injections = {},
                                     .source = BlackExrPath(standard),
                                     .duration_frames = 1});
  return project;
}

TEST(VideoSourcesTest, PalBlackExrEmitsBlankingLevelOnActiveLines) {
  GenerationStage generation;
  std::vector<SampleFixed> y;
  std::vector<SampleFixed> c;
  std::vector<std::string> errors;
  ASSERT_TRUE(
      generation.Generate(MakeBlackProject(Standard::kPal), &y, &c, &errors));
  // ITU-R BT.1700 Annex 1 Part B Table 2 item 4: 625-line black = blanking.
  for (int line : {100, 400}) {
    EXPECT_NEAR(MidLineLumaMillivolts(y, Standard::kPal, line), 0.0, 1.0)
        << "line " << line;
  }
}

TEST(VideoSourcesTest, NtscBlackExrEmitsConfiguredSetupLevel) {
  for (double setup_ire : {7.5, 0.0}) {
    Project project = MakeBlackProject(Standard::kNtsc);
    project.cvbs_presets.ntsc_black_setup_ire = setup_ire;
    GenerationStage generation;
    std::vector<SampleFixed> y;
    std::vector<SampleFixed> c;
    std::vector<std::string> errors;
    ASSERT_TRUE(generation.Generate(project, &y, &c, &errors));
    const double black_mv = GetSignalLevels(project.cvbs_presets).black_mv;
    EXPECT_NEAR(black_mv, setup_ire == 0.0 ? 0.0 : 53.6, 0.1);
    EXPECT_NEAR(MidLineLumaMillivolts(y, Standard::kNtsc, 100), black_mv, 1.0)
        << "setup " << setup_ire;
  }
}

// --- Probe ----------------------------------------------------------------

TEST(VideoSourcesTest, ProbeReportsH264Mp4WithAudio) {
  ProgressiveFrameSourceProbe probe;
  ProgressiveFrameSourceProfile profile;
  std::string error;
  ASSERT_TRUE(probe.Probe(VideoSection(H264Mp4WithAudio()), &profile, &error))
      << error;
  EXPECT_NE(profile.container.find("mp4"), std::string::npos);
  EXPECT_EQ(profile.codec, "h264");
  EXPECT_EQ(profile.pixel_format, "yuv420p");
  EXPECT_EQ(profile.width, 720);
  EXPECT_EQ(profile.height, 576);
  EXPECT_NEAR(profile.frame_rate_hz, 25.0, 1e-6);
  EXPECT_NEAR(profile.average_frame_rate_hz, 25.0, 1e-6);
  // MP4 declares nb_frames, so the count needs no decoding pass.
  EXPECT_EQ(profile.frame_count, 100);
  ASSERT_EQ(profile.audio_streams.size(), 1U);
  EXPECT_EQ(profile.audio_streams[0].stream_index, 1);
  EXPECT_EQ(profile.audio_streams[0].codec, "aac");
  EXPECT_EQ(profile.audio_streams[0].channels, 1);
  EXPECT_EQ(profile.audio_streams[0].sample_rate_hz, 48000);
  EXPECT_NEAR(profile.audio_streams[0].duration_seconds, 4.0, 0.05);
}

TEST(VideoSourcesTest, ProbeReportsH264MkvWithoutAudio) {
  ProgressiveFrameSourceProbe probe;
  ProgressiveFrameSourceProfile profile;
  std::string error;
  ASSERT_TRUE(probe.Probe(VideoSection(H264NtscMkv()), &profile, &error))
      << error;
  EXPECT_NE(profile.container.find("matroska"), std::string::npos);
  EXPECT_EQ(profile.codec, "h264");
  EXPECT_EQ(profile.height, 486);
  EXPECT_NEAR(profile.frame_rate_hz, 30000.0 / 1001.0, 1e-6);
  EXPECT_EQ(profile.frame_count, 120);
  EXPECT_TRUE(profile.audio_streams.empty());
}

TEST(VideoSourcesTest, ProbeReportsRangeMatrixAndRgbTags) {
  ProgressiveFrameSourceProbe probe;
  std::string error;

  ProgressiveFrameSourceProfile full;
  ASSERT_TRUE(probe.Probe(VideoSection(FullRangeLumaBars()), &full, &error))
      << error;
  EXPECT_EQ(full.color_range, "pc");
  EXPECT_EQ(full.pixel_format, "yuv420p");
  EXPECT_EQ(full.bit_depth, 8);

  ProgressiveFrameSourceProfile bt709;
  ASSERT_TRUE(probe.Probe(VideoSection(Bt709Bars()), &bt709, &error)) << error;
  EXPECT_EQ(bt709.color_space, "bt709");

  ProgressiveFrameSourceProfile rgb;
  ASSERT_TRUE(probe.Probe(VideoSection(RgbPngMkv()), &rgb, &error)) << error;
  EXPECT_EQ(rgb.codec, "png");
  EXPECT_EQ(rgb.pixel_format, "rgb24");
}

TEST(VideoSourcesTest, ProbedFrameCountMatchesDecodedFrames) {
  struct Case {
    std::string source;
    Standard standard;
  };
  for (const Case& c : {Case{H264Mp4WithAudio(), Standard::kPal},
                        Case{H264NtscMkv(), Standard::kNtsc},
                        Case{Ffv1PalMkv(), Standard::kPal}}) {
    ProgressiveFrameSourceProbe probe;
    ProgressiveFrameSourceProfile profile;
    std::string error;
    ASSERT_TRUE(probe.Probe(VideoSection(c.source), &profile, &error));

    FfmpegVideoFrameDecoder decoder;
    VideoDecodeRequest request;
    request.source = c.source;
    request.standard = c.standard;
    request.filter_chain =
        PlanSourceColourConversion(profile, c.standard).filter_chain;
    ASSERT_TRUE(decoder.Open(request, &error)) << error;
    FrameSourceImage image;
    int decoded = 0;
    VideoFrameReadStatus status = VideoFrameReadStatus::kFrame;
    while ((status = decoder.ReadFrame(&image, &error)) ==
           VideoFrameReadStatus::kFrame) {
      ++decoded;
    }
    EXPECT_EQ(status, VideoFrameReadStatus::kEndOfStream) << error;
    EXPECT_EQ(decoded, profile.frame_count) << c.source;
  }
}

// --- Exact seek -----------------------------------------------------------

TEST(VideoSourcesTest, OpeningAtFrameNMatchesSequentialReadToFrameN) {
  struct Case {
    std::string source;
    Standard standard;
  };
  for (const Case& c : {Case{H264Mp4WithAudio(), Standard::kPal},
                        Case{H264NtscMkv(), Standard::kNtsc},
                        Case{Ffv1PalMkv(), Standard::kPal}}) {
    const std::vector<int> targets = {1, 13, 37, 60};
    std::vector<FrameSourceImage> sequential;
    {
      FfmpegVideoFrameDecoder decoder;
      VideoDecodeRequest request;
      request.source = c.source;
      request.standard = c.standard;
      std::string error;
      ASSERT_TRUE(decoder.Open(request, &error)) << error;
      for (int i = 0; i <= targets.back(); ++i) {
        FrameSourceImage image;
        ASSERT_EQ(decoder.ReadFrame(&image, &error),
                  VideoFrameReadStatus::kFrame)
            << error;
        sequential.push_back(std::move(image));
      }
    }

    for (int target : targets) {
      FfmpegVideoFrameDecoder decoder;
      VideoDecodeRequest request;
      request.source = c.source;
      request.standard = c.standard;
      request.first_frame = target;
      std::string error;
      ASSERT_TRUE(decoder.Open(request, &error)) << error;
      FrameSourceImage image;
      ASSERT_EQ(decoder.ReadFrame(&image, &error), VideoFrameReadStatus::kFrame)
          << error;
      const std::vector<YCbCr444Pixel>& expected =
          sequential[static_cast<std::size_t>(target)].pixels;
      ASSERT_EQ(image.pixels.size(), expected.size());
      int mismatches = 0;
      for (std::size_t i = 0; i < expected.size(); ++i) {
        if (image.pixels[i].y != expected[i].y ||
            image.pixels[i].cb != expected[i].cb ||
            image.pixels[i].cr != expected[i].cr) {
          ++mismatches;
        }
      }
      EXPECT_EQ(mismatches, 0) << c.source << " frame " << target;
    }
  }
}

TEST(VideoSourcesTest, FrameSourceServesH264Mp4FramesAcrossTheClip) {
  ProgressiveFrameSource frame_source;
  const Section section = VideoSection(H264Mp4WithAudio());
  int frame_count = 0;
  std::string error;
  ASSERT_TRUE(frame_source.ResolveFrameCount(section, Standard::kPal,
                                             &frame_count, &error))
      << error;
  ASSERT_EQ(frame_count, 100);
  for (int i : {0, 1, 50, 99, 3}) {
    std::shared_ptr<const FrameSourceImage> image;
    ASSERT_TRUE(
        frame_source.GenerateFrame(section, i, Standard::kPal, &image, &error))
        << "frame " << i << ": " << error;
    EXPECT_EQ(image->width, 720);
    EXPECT_EQ(image->height, 576);
  }
  std::shared_ptr<const FrameSourceImage> past_end;
  EXPECT_FALSE(frame_source.GenerateFrame(section, 100, Standard::kPal,
                                          &past_end, &error));
}

// --- Colour normalisation -------------------------------------------------

TEST(VideoSourcesTest, FullRangeLumaBarsDecodeToStudioSwing) {
  const std::shared_ptr<const FrameSourceImage> image =
      DecodeFrame(FullRangeLumaBars(), Standard::kPal, 0);
  ASSERT_NE(image, nullptr);
  // Bar centres, away from the transitions.
  EXPECT_NEAR(image->PixelAt(120, 288).y, 64, 1);
  EXPECT_NEAR(image->PixelAt(360, 288).y, 512, 1);
  EXPECT_NEAR(image->PixelAt(600, 288).y, 940, 1);
  EXPECT_NEAR(image->PixelAt(600, 288).cb, 512, 1);
  EXPECT_NEAR(image->PixelAt(600, 288).cr, 512, 1);
}

TEST(VideoSourcesTest, Bt709BarsDecodeWithinTwoCodesOfBt601Values) {
  const std::shared_ptr<const FrameSourceImage> image =
      DecodeFrame(Bt709Bars(), Standard::kPal, 0);
  ASSERT_NE(image, nullptr);
  ExpectPixelNear(image->PixelAt(90, 288), Bt601Codes(1, 0, 0), 2, "red");
  ExpectPixelNear(image->PixelAt(270, 288), Bt601Codes(0, 1, 0), 2, "green");
  ExpectPixelNear(image->PixelAt(450, 288), Bt601Codes(0, 0, 1), 2, "blue");
  ExpectPixelNear(image->PixelAt(630, 288), Bt601Codes(1, 1, 1), 2, "white");
}

TEST(VideoSourcesTest, RgbCodedClipDecodesToBt601Ycbcr) {
  const std::shared_ptr<const FrameSourceImage> image =
      DecodeFrame(RgbPngMkv(), Standard::kPal, 0);
  ASSERT_NE(image, nullptr);
  ExpectPixelNear(image->PixelAt(360, 288), Bt601Codes(1, 0, 0), 2, "red");
}

TEST(VideoSourcesTest, ConformantFfv1ClipPassesThroughUnconverted) {
  ProgressiveFrameSourceProbe probe;
  ProgressiveFrameSourceProfile profile;
  std::string error;
  ASSERT_TRUE(probe.Probe(VideoSection(Ffv1PalMkv()), &profile, &error));
  EXPECT_TRUE(
      PlanSourceColourConversion(profile, Standard::kPal).IsPassThrough());
}

}  // namespace
}  // namespace videosynth
