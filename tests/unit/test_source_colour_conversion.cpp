/*
 * File:        test_source_colour_conversion.cpp
 * Module:      source_colour_conversion_tests
 * Purpose:     Unit tests for the BT.601 studio-swing colour normalisation
 *              plan derived from a probed video profile.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "videosynth/source_colour_conversion.h"

namespace videosynth {
namespace {

ProgressiveFrameSourceProfile MakeConformantProfile() {
  ProgressiveFrameSourceProfile profile;
  profile.pixel_format = "yuv422p10le";
  profile.color_space = "smpte170m";
  profile.color_range = "tv";
  profile.color_primaries = "bt470bg";
  profile.color_transfer = "bt709";
  return profile;
}

bool Contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

bool ListContains(const std::vector<std::string>& list,
                  const std::string& needle) {
  return std::any_of(list.begin(), list.end(), [&](const std::string& item) {
    return Contains(item, needle);
  });
}

TEST(SourceColourConversionTest, ConformantSourcePassesThroughWithoutFilter) {
  const SourceColourConversion plan =
      PlanSourceColourConversion(MakeConformantProfile(), Standard::kPal);
  EXPECT_TRUE(plan.IsPassThrough());
  EXPECT_TRUE(plan.filter_chain.empty());
  EXPECT_TRUE(plan.conversions.empty());
  EXPECT_TRUE(plan.unconverted_notes.empty());
}

TEST(SourceColourConversionTest, UnknownRangeAndMatrixAreTakenAsBt601Studio) {
  for (const char* tag : {"", "unknown"}) {
    ProgressiveFrameSourceProfile profile = MakeConformantProfile();
    profile.color_range = tag;
    profile.color_space = tag;
    EXPECT_TRUE(
        PlanSourceColourConversion(profile, Standard::kPal).IsPassThrough())
        << "tag='" << tag << "'";
  }
}

TEST(SourceColourConversionTest, Bt470bgMatrixIsBt601) {
  ProgressiveFrameSourceProfile profile = MakeConformantProfile();
  profile.color_space = "bt470bg";
  EXPECT_TRUE(
      PlanSourceColourConversion(profile, Standard::kPal).IsPassThrough());
}

TEST(SourceColourConversionTest, FullRangeIsScaledToStudioSwing) {
  for (const char* range : {"pc", "jpeg"}) {
    ProgressiveFrameSourceProfile profile = MakeConformantProfile();
    profile.color_range = range;
    const SourceColourConversion plan =
        PlanSourceColourConversion(profile, Standard::kPal);
    EXPECT_FALSE(plan.IsPassThrough());
    EXPECT_TRUE(Contains(plan.filter_chain, "in_range=pc"));
    EXPECT_TRUE(Contains(plan.filter_chain, "out_range=tv"));
    EXPECT_TRUE(ListContains(plan.conversions, "full range -> studio swing"));
  }
}

TEST(SourceColourConversionTest, YuvjPixelFormatImpliesFullRange) {
  ProgressiveFrameSourceProfile profile = MakeConformantProfile();
  profile.pixel_format = "yuvj420p";
  profile.color_range = "";
  const SourceColourConversion plan =
      PlanSourceColourConversion(profile, Standard::kPal);
  EXPECT_TRUE(Contains(plan.filter_chain, "in_range=pc"));
}

TEST(SourceColourConversionTest, KnownNonBt601MatricesAreConverted) {
  struct Case {
    const char* color_space;
    const char* swscale_matrix;
    const char* display;
  };
  for (const Case& c :
       {Case{"bt709", "bt709", "BT.709"}, Case{"fcc", "fcc", "FCC"},
        Case{"smpte240m", "smpte240m", "SMPTE 240M"},
        Case{"bt2020nc", "bt2020", "BT.2020"},
        Case{"bt2020c", "bt2020", "BT.2020"}}) {
    ProgressiveFrameSourceProfile profile = MakeConformantProfile();
    profile.color_space = c.color_space;
    const SourceColourConversion plan =
        PlanSourceColourConversion(profile, Standard::kPal);
    EXPECT_TRUE(Contains(plan.filter_chain,
                         std::string("in_color_matrix=") + c.swscale_matrix))
        << c.color_space;
    EXPECT_TRUE(Contains(plan.filter_chain, "out_color_matrix=bt601"));
    EXPECT_TRUE(ListContains(plan.conversions,
                             std::string(c.display) + " matrix -> BT.601"))
        << c.color_space;
  }
}

TEST(SourceColourConversionTest, RgbSourcesConvertToBt601Ycbcr) {
  for (const char* pixel_format : {"rgb24", "bgr0", "gbrp10le", "rgba"}) {
    ProgressiveFrameSourceProfile profile = MakeConformantProfile();
    profile.pixel_format = pixel_format;
    profile.color_space = "";
    const SourceColourConversion plan =
        PlanSourceColourConversion(profile, Standard::kPal);
    EXPECT_FALSE(Contains(plan.filter_chain, "in_range")) << pixel_format;
    EXPECT_TRUE(Contains(plan.filter_chain, "out_range=tv"));
    EXPECT_TRUE(Contains(plan.filter_chain, "out_color_matrix=bt601"));
    EXPECT_TRUE(Contains(plan.filter_chain, "format=yuv422p10le"));
    // A single stage: swscale's RGB path is exact at the 0/255 extremes.
    EXPECT_FALSE(Contains(plan.filter_chain, "scale=flags")) << pixel_format;
    EXPECT_TRUE(ListContains(plan.conversions, "RGB")) << pixel_format;
  }
}

TEST(SourceColourConversionTest, GbrColourSpaceTagMarksRgbSource) {
  ProgressiveFrameSourceProfile profile = MakeConformantProfile();
  profile.color_space = "gbr";
  const SourceColourConversion plan =
      PlanSourceColourConversion(profile, Standard::kPal);
  EXPECT_TRUE(ListContains(plan.conversions, "RGB"));
}

TEST(SourceColourConversionTest, OtherPixelFormatsConvertToYuv422p10le) {
  for (const char* pixel_format :
       {"yuv420p", "yuv422p", "yuv420p10le", "yuv444p12le", "gray"}) {
    ProgressiveFrameSourceProfile profile = MakeConformantProfile();
    profile.pixel_format = pixel_format;
    const SourceColourConversion plan =
        PlanSourceColourConversion(profile, Standard::kPal);
    EXPECT_FALSE(plan.IsPassThrough()) << pixel_format;
    EXPECT_TRUE(Contains(plan.filter_chain, "in_range=tv"));
    EXPECT_TRUE(Contains(plan.filter_chain, "in_color_matrix=bt601"));
    EXPECT_TRUE(Contains(plan.filter_chain, "format=yuv422p10le"));
    EXPECT_TRUE(ListContains(plan.conversions,
                             std::string(pixel_format) + " -> yuv422p10le"));
  }
}

TEST(SourceColourConversionTest, CombinedConversionsAreAllListed) {
  ProgressiveFrameSourceProfile profile = MakeConformantProfile();
  profile.pixel_format = "yuv420p";
  profile.color_range = "pc";
  profile.color_space = "bt709";
  const SourceColourConversion plan =
      PlanSourceColourConversion(profile, Standard::kNtsc);
  EXPECT_EQ(plan.conversions.size(), 3U);
  EXPECT_EQ(plan.filter_chain,
            "scale=in_range=pc:out_range=tv:in_color_matrix=bt709:"
            "out_color_matrix=bt601:flags=accurate_rnd+full_chroma_int+"
            "bitexact,format=yuv420p,scale=flags=accurate_rnd+full_chroma_int+"
            "bitexact,format=yuv422p10le");
}

TEST(SourceColourConversionTest, StudioRangeConversionIsOneStage) {
  ProgressiveFrameSourceProfile profile = MakeConformantProfile();
  profile.pixel_format = "yuv420p";
  profile.color_space = "bt709";
  const SourceColourConversion plan =
      PlanSourceColourConversion(profile, Standard::kNtsc);
  EXPECT_EQ(plan.conversions.size(), 2U);
  EXPECT_EQ(plan.filter_chain,
            "scale=in_range=tv:out_range=tv:in_color_matrix=bt709:"
            "out_color_matrix=bt601:flags=accurate_rnd+full_chroma_int+"
            "bitexact,format=yuv422p10le");
}

TEST(SourceColourConversionTest, FullRangeConvertsAtSourceDepthThenWidens) {
  ProgressiveFrameSourceProfile profile = MakeConformantProfile();
  profile.pixel_format = "yuvj420p";
  const SourceColourConversion plan =
      PlanSourceColourConversion(profile, Standard::kPal);
  EXPECT_EQ(plan.filter_chain,
            "scale=in_range=pc:out_range=tv:in_color_matrix=bt601:"
            "out_color_matrix=bt601:flags=accurate_rnd+full_chroma_int+"
            "bitexact,format=yuv420p,scale=flags=accurate_rnd+full_chroma_int+"
            "bitexact,format=yuv422p10le");
}

TEST(SourceColourConversionTest, FullRangeTargetFormatNeedsNoWideningStage) {
  ProgressiveFrameSourceProfile profile = MakeConformantProfile();
  profile.color_range = "pc";
  const SourceColourConversion plan =
      PlanSourceColourConversion(profile, Standard::kPal);
  EXPECT_EQ(plan.filter_chain.find("scale=flags"), std::string::npos);
  EXPECT_TRUE(Contains(plan.filter_chain, ",format=yuv422p10le"));
}

TEST(SourceColourConversionTest, MismatchedPrimariesAreNotedNotConverted) {
  ProgressiveFrameSourceProfile profile = MakeConformantProfile();
  profile.color_primaries = "bt709";
  const SourceColourConversion plan =
      PlanSourceColourConversion(profile, Standard::kPal);
  EXPECT_TRUE(plan.IsPassThrough());
  ASSERT_EQ(plan.unconverted_notes.size(), 1U);
  EXPECT_TRUE(Contains(plan.unconverted_notes[0], "primaries 'bt709'"));
}

TEST(SourceColourConversionTest, PrimariesExpectationFollowsStandard) {
  ProgressiveFrameSourceProfile profile = MakeConformantProfile();
  profile.color_primaries = "smpte170m";
  EXPECT_EQ(PlanSourceColourConversion(profile, Standard::kNtsc)
                .unconverted_notes.size(),
            0U);
  EXPECT_EQ(PlanSourceColourConversion(profile, Standard::kPalM)
                .unconverted_notes.size(),
            0U);
  EXPECT_EQ(PlanSourceColourConversion(profile, Standard::kPal)
                .unconverted_notes.size(),
            1U);
}

TEST(SourceColourConversionTest, UnusualTransferIsNoted) {
  ProgressiveFrameSourceProfile profile = MakeConformantProfile();
  profile.color_transfer = "smpte2084";
  const SourceColourConversion plan =
      PlanSourceColourConversion(profile, Standard::kPal);
  ASSERT_EQ(plan.unconverted_notes.size(), 1U);
  EXPECT_TRUE(Contains(plan.unconverted_notes[0], "transfer"));
}

}  // namespace
}  // namespace videosynth
