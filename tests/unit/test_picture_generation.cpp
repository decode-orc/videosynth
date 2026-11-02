/*
 * File:        test_picture_generation.cpp
 * Module:      generation_stage_tests
 * Purpose:     Validates the active-picture options of the generation stage —
 *              picture truth, chroma_line_sign and the band-limited horizontal
 *              mapping — using a fake probe and a fake pattern decoder.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "videosynth/fixed_point.h"
#include "videosynth/frame_line_layout.h"
#include "videosynth/generation_stage.h"
#include "videosynth/picture_truth.h"
#include "videosynth/progressive_frame_source.h"
#include "videosynth/timing_constants.h"

namespace videosynth {
namespace {

constexpr double kPi = 3.14159265358979323846;

// Source pictures by name. "flat_v.mkv" is a uniform grey with +V (Cr above
// 512); "edge.mkv" is a luma and chroma step at the raster's centre column.
YCbCr444Pixel PixelFor(const std::string& source, int x) {
  if (source == "edge.mkv") {
    return x < 360 ? YCbCr444Pixel{.y = 300, .cb = 400, .cr = 600}
                   : YCbCr444Pixel{.y = 700, .cb = 620, .cr = 450};
  }
  return YCbCr444Pixel{.y = 500, .cb = 512, .cr = 600};
}

class PatternFrameDecoder final : public IVideoFrameDecoder {
 public:
  explicit PatternFrameDecoder(int height) : height_(height) {}

  bool Open(const VideoDecodeRequest& request, std::string*) override {
    source_ = request.source;
    return true;
  }

  VideoFrameReadStatus ReadFrame(FrameSourceImage* out_image,
                                 std::string*) override {
    out_image->width = 720;
    out_image->height = height_;
    out_image->active_width = 720;
    out_image->active_height = height_;
    out_image->pixels.resize(static_cast<std::size_t>(720) * height_);
    for (int y = 0; y < height_; ++y) {
      for (int x = 0; x < 720; ++x) {
        out_image->pixels[(static_cast<std::size_t>(y) * 720) + x] =
            PixelFor(source_, x);
      }
    }
    return VideoFrameReadStatus::kFrame;
  }

 private:
  int height_;
  std::string source_;
};

class PatternProbe final : public IProgressiveFrameSourceProbe {
 public:
  explicit PatternProbe(Standard standard) : standard_(standard) {}

  bool Probe(const Section&, ProgressiveFrameSourceProfile* out_profile,
             std::string*) override {
    ProgressiveFrameSourceProfile profile;
    profile.container = "matroska,webm";
    profile.codec = "ffv1";
    profile.pixel_format = "yuv422p10le";
    profile.color_range = "tv";
    profile.width = 720;
    const bool is_625 = Is625LineStandard(standard_);
    profile.height = is_625 ? 576 : 486;
    profile.frame_rate_hz = is_625 ? 25.0 : 30000.0 / 1001.0;
    profile.frame_count = 8;
    *out_profile = profile;
    return true;
  }

 private:
  Standard standard_;
};

std::unique_ptr<GenerationStage> MakeStage(Standard standard) {
  const int height = Is625LineStandard(standard) ? 576 : 486;
  return std::make_unique<GenerationStage>(
      nullptr, nullptr, nullptr,
      std::make_unique<ProgressiveFrameSource>(
          std::make_shared<PatternProbe>(standard),
          [height]() { return std::make_unique<PatternFrameDecoder>(height); },
          32U));
}

Project MakeProject(Standard standard, const std::string& source) {
  Project project;
  project.cvbs_presets.video_standard_preset = standard;
  project.cvbs_presets.sample_encoding_preset = "CVBS_U10_4FSC";
  project.cvbs_presets.signal_state_preset = "STANDARD_STABLE_LOCKED";
  Section section;
  section.name = "Picture";
  section.type = "progressive";
  section.source = source;
  section.duration_frames = 4;
  project.sections.push_back(section);
  return project;
}

struct GeneratedFrames {
  std::vector<SampleFixed> y_mv;
  std::vector<SampleFixed> c_mv;
  std::vector<PictureTruthFrame> truth;
};

GeneratedFrames Generate(const Project& project, std::size_t frame_count,
                         bool with_truth) {
  auto stage = MakeStage(project.cvbs_presets.video_standard_preset);
  std::vector<IGenerationStage::FrameScheduleItem> schedule;
  std::vector<std::string> errors;
  EXPECT_TRUE(stage->BuildFrameSchedule(project, &schedule, &errors));
  GeneratedFrames frames;
  const bool ok =
      with_truth
          ? stage->GenerateFrameBatchWithTruth(
                project, schedule, 0, frame_count, &frames.y_mv, &frames.c_mv,
                &frames.truth, &errors)
          : stage->GenerateFrameBatch(project, schedule, 0, frame_count,
                                      &frames.y_mv, &frames.c_mv, &errors);
  EXPECT_TRUE(ok) << (errors.empty() ? "" : errors[0]);
  return frames;
}

// Largest residual, in mV, of fitting c = a*P + b*Q over one line, where
// P/Q are the truth's quadrature basis for V-switch sign s and the phase
// (a, b) = (cos, sin) of the line's carrier start is free. Also returns the
// fitted carrier amplitude, which must be 1.
struct LineFit {
  double max_residual_mv = 0.0;
  double amplitude = 0.0;
};

LineFit FitLine(const SampleFixed* c_line, const float* u, const float* v,
                int count, int s) {
  double ppt = 0.0;
  double pqt = 0.0;
  double qqt = 0.0;
  double pct = 0.0;
  double qct = 0.0;
  std::vector<double> p(static_cast<std::size_t>(count));
  std::vector<double> q(static_cast<std::size_t>(count));
  for (int k = 0; k < count; ++k) {
    const double sk = std::sin(k * kPi / 2.0);
    const double ck = std::cos(k * kPi / 2.0);
    const double u_k = u[k];
    const double v_k = static_cast<double>(s) * v[k];
    p[k] = (u_k * sk) + (v_k * ck);
    q[k] = (u_k * ck) - (v_k * sk);
    const double c = SampleFixedToMillivolts(c_line[k]);
    ppt += p[k] * p[k];
    pqt += p[k] * q[k];
    qqt += q[k] * q[k];
    pct += p[k] * c;
    qct += q[k] * c;
  }
  const double det = (ppt * qqt) - (pqt * pqt);
  const double a = ((qqt * pct) - (pqt * qct)) / det;
  const double b = ((ppt * qct) - (pqt * pct)) / det;
  LineFit fit;
  fit.amplitude = std::hypot(a, b);
  for (int k = 0; k < count; ++k) {
    const double residual =
        SampleFixedToMillivolts(c_line[k]) - ((a * p[k]) + (b * q[k]));
    fit.max_residual_mv = std::max(fit.max_residual_mv, std::abs(residual));
  }
  return fit;
}

TEST(PictureTruthGenerationTest, TruthReproducesTheModulatedPicture) {
  for (const Standard standard : {Standard::kPal, Standard::kNtsc}) {
    const Project project = MakeProject(standard, "edge.mkv");
    const GeneratedFrames frames = Generate(project, 2, true);
    ASSERT_EQ(frames.truth.size(), 2U);
    const PictureTruthLayout layout =
        BuildPictureTruthLayout(project.cvbs_presets);
    const TimingConstants timing = GetTimingConstants(standard);
    const std::vector<int> offsets =
        BuildLineSampleOffsets(BuildLineSampleCounts(
            standard, timing.lines_per_frame, timing.samples_per_line_4fsc));
    const SampleFixed blanking = MillivoltsToSampleFixed(
        GetSignalLevels(project.cvbs_presets).blanking_mv);
    const std::size_t frame_samples =
        static_cast<std::size_t>(SamplesPerFrame4fsc(standard));

    for (std::size_t frame = 0; frame < 2U; ++frame) {
      const PictureTruthFrame& truth = frames.truth[frame];
      for (int line = 1; line <= timing.lines_per_frame; ++line) {
        const int row = layout.RowForLine(line);
        if (row < 0 || line == layout.half_picture_line) {
          continue;
        }
        const std::size_t base = (frame * frame_samples) +
                                 static_cast<std::size_t>(offsets[line - 1]) +
                                 static_cast<std::size_t>(layout.first_sample);
        const std::size_t row_offset =
            static_cast<std::size_t>(row) * layout.samples_per_line;
        for (int x = 0; x < layout.samples_per_line; ++x) {
          ASSERT_FLOAT_EQ(truth.y_mv[row_offset + x],
                          static_cast<float>(SampleFixedToMillivolts(
                              frames.y_mv[base + x] - blanking)))
              << StandardToString(standard) << " line " << line;
        }
        const LineFit plus =
            FitLine(&frames.c_mv[base], &truth.u_mv[row_offset],
                    &truth.v_mv[row_offset], layout.samples_per_line, 1);
        const LineFit minus =
            FitLine(&frames.c_mv[base], &truth.u_mv[row_offset],
                    &truth.v_mv[row_offset], layout.samples_per_line, -1);
        const LineFit& best =
            plus.max_residual_mv <= minus.max_residual_mv ? plus : minus;
        ASSERT_LT(best.max_residual_mv, 1e-3)
            << StandardToString(standard) << " line " << line;
        ASSERT_NEAR(best.amplitude, 1.0, 1e-6);
        if (standard == Standard::kNtsc) {
          // NTSC has no V-switch.
          ASSERT_LT(plus.max_residual_mv, 1e-3) << "line " << line;
        }
      }
    }
  }
}

TEST(PictureTruthGenerationTest, TruthRequestDoesNotChangeTheSignal) {
  const Project project = MakeProject(Standard::kPal, "edge.mkv");
  const GeneratedFrames plain = Generate(project, 4, false);
  const GeneratedFrames with_truth = Generate(project, 4, true);
  EXPECT_EQ(plain.y_mv, with_truth.y_mv);
  EXPECT_EQ(plain.c_mv, with_truth.c_mv);
}

TEST(PictureTruthGenerationTest, SecamHasNoTruth) {
  const Project project = MakeProject(Standard::kSecam, "flat_v.mkv");
  auto stage = MakeStage(Standard::kSecam);
  std::vector<IGenerationStage::FrameScheduleItem> schedule;
  std::vector<std::string> errors;
  ASSERT_TRUE(stage->BuildFrameSchedule(project, &schedule, &errors));
  std::vector<SampleFixed> y_mv;
  std::vector<SampleFixed> c_mv;
  std::vector<PictureTruthFrame> truth;
  EXPECT_FALSE(stage->GenerateFrameBatchWithTruth(
      project, schedule, 0, 1, &y_mv, &c_mv, &truth, &errors));
  ASSERT_FALSE(errors.empty());
}

// Counts the rows (sampled at column x) whose truth value is positive and
// negative, requiring every value to have the same magnitude.
struct SignCount {
  int positive = 0;
  int negative = 0;
};

SignCount CountSigns(const std::vector<float>& plane,
                     const PictureTruthLayout& layout, int x, float magnitude) {
  SignCount count;
  // Rows 2..rows-3 avoid the half-picture line.
  for (int row = 2; row < layout.rows - 2; ++row) {
    const float value =
        plane[(static_cast<std::size_t>(row) * layout.samples_per_line) + x];
    EXPECT_NEAR(std::abs(value), magnitude, 1e-3) << "row " << row;
    (value > 0.0F ? count.positive : count.negative)++;
  }
  return count;
}

TEST(ChromaLineSignGenerationTest, FollowSwitchAlternatesTheDecodedComponent) {
  Project project = MakeProject(Standard::kPal, "flat_v.mkv");
  const PictureTruthLayout layout =
      BuildPictureTruthLayout(project.cvbs_presets);
  constexpr int kMidSample = 461;

  // Without regions the decoded V is the source's steady +V.
  const GeneratedFrames plain = Generate(project, 1, true);
  const float v_magnitude =
      plain.truth[0].v_mv[(10U * layout.samples_per_line) + kMidSample];
  ASSERT_GT(v_magnitude, 10.0F);
  const SignCount plain_v =
      CountSigns(plain.truth[0].v_mv, layout, kMidSample, v_magnitude);
  EXPECT_EQ(plain_v.negative, 0);

  // v: follow_switch over the whole raster: the transmitted V is unswitched,
  // so the decoded V alternates with the switch, half the lines each way.
  project.sections[0].chroma_line_sign.push_back(
      ChromaLineSignRegion{.x = 0,
                           .y = 0,
                           .width = 720,
                           .height = 576,
                           .v = ChromaLineSignMode::kFollowSwitch});
  const GeneratedFrames following = Generate(project, 1, true);
  const SignCount follow_v =
      CountSigns(following.truth[0].v_mv, layout, kMidSample, v_magnitude);
  EXPECT_GT(follow_v.negative, 0);
  EXPECT_EQ(follow_v.positive, follow_v.negative);
  // U is untouched (zero in this source).
  for (const float u : following.truth[0].u_mv) {
    ASSERT_EQ(u, 0.0F);
  }
}

TEST(ChromaLineSignGenerationTest, FollowSwitchOnUAlternatesU) {
  Project project = MakeProject(Standard::kPal, "edge.mkv");
  const PictureTruthLayout layout =
      BuildPictureTruthLayout(project.cvbs_presets);
  constexpr int kLeftSample = 100;
  const GeneratedFrames plain = Generate(project, 1, true);
  const float u_left =
      plain.truth[0].u_mv[(10U * layout.samples_per_line) + kLeftSample];
  ASSERT_LT(u_left, -10.0F);

  project.sections[0].chroma_line_sign.push_back(
      ChromaLineSignRegion{.x = 0,
                           .y = 0,
                           .width = 360,
                           .height = 576,
                           .u = ChromaLineSignMode::kFollowSwitch});
  const GeneratedFrames following = Generate(project, 1, true);
  const SignCount follow_u = CountSigns(following.truth[0].u_mv, layout,
                                        kLeftSample, std::abs(u_left));
  EXPECT_EQ(follow_u.positive, follow_u.negative);
  // V and the right half (outside the region) match the plain picture.
  EXPECT_EQ(following.truth[0].v_mv, plain.truth[0].v_mv);
  constexpr int kRightSample = 800;
  for (int row = 0; row < layout.rows; ++row) {
    const std::size_t index =
        (static_cast<std::size_t>(row) * layout.samples_per_line) +
        kRightSample;
    ASSERT_EQ(following.truth[0].u_mv[index], plain.truth[0].u_mv[index]);
  }
}

TEST(ChromaLineSignGenerationTest, RowsOutsideTheRegionAreUnchanged) {
  Project project = MakeProject(Standard::kPal, "flat_v.mkv");
  const GeneratedFrames plain = Generate(project, 1, false);
  // A region on source rows 100-109 only.
  project.sections[0].chroma_line_sign.push_back(
      ChromaLineSignRegion{.x = 0,
                           .y = 100,
                           .width = 720,
                           .height = 10,
                           .v = ChromaLineSignMode::kFollowSwitch});
  const GeneratedFrames following = Generate(project, 1, false);
  EXPECT_EQ(plain.y_mv, following.y_mv);
  const TimingConstants timing = GetTimingConstants(Standard::kPal);
  const std::vector<int> offsets = BuildLineSampleOffsets(BuildLineSampleCounts(
      Standard::kPal, timing.lines_per_frame, timing.samples_per_line_4fsc));
  const PictureTruthLayout layout =
      BuildPictureTruthLayout(project.cvbs_presets);
  int changed_lines = 0;
  for (int line = 1; line <= timing.lines_per_frame; ++line) {
    const int start = offsets[line - 1];
    const int end = line < timing.lines_per_frame
                        ? offsets[line]
                        : static_cast<int>(plain.c_mv.size());
    const bool same =
        std::equal(plain.c_mv.begin() + start, plain.c_mv.begin() + end,
                   following.c_mv.begin() + start);
    const int row = layout.RowForLine(line);
    if (row < 100 || row >= 110) {
      ASSERT_TRUE(same) << "line " << line;
    } else if (!same) {
      ++changed_lines;
    }
  }
  // About half of the ten region rows have an inverted V axis (the switch
  // alternates line by line within each field).
  EXPECT_GE(changed_lines, 4);
  EXPECT_LE(changed_lines, 6);
}

TEST(BandlimitedGenerationTest, FlatPictureMatchesTheNearestMapping) {
  Project project = MakeProject(Standard::kPal, "flat_v.mkv");
  const GeneratedFrames nearest = Generate(project, 1, false);
  project.sections[0].horizontal_resample = HorizontalResample::kBandlimited;
  const GeneratedFrames bandlimited = Generate(project, 1, false);
  EXPECT_EQ(nearest.y_mv, bandlimited.y_mv);
  EXPECT_EQ(nearest.c_mv, bandlimited.c_mv);
}

TEST(BandlimitedGenerationTest, EdgePictureIsResampled) {
  for (const Standard standard :
       {Standard::kPal, Standard::kNtsc, Standard::kSecam}) {
    Project project = MakeProject(standard, "edge.mkv");
    const GeneratedFrames nearest = Generate(project, 1, false);
    project.sections[0].horizontal_resample = HorizontalResample::kBandlimited;
    const GeneratedFrames bandlimited = Generate(project, 1, false);
    EXPECT_NE(nearest.y_mv, bandlimited.y_mv) << StandardToString(standard);
    EXPECT_NE(nearest.c_mv, bandlimited.c_mv) << StandardToString(standard);
  }
}

}  // namespace
}  // namespace videosynth
