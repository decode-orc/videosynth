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

#include "lattice_delay_reference.h"
#include "videosynth/fixed_point.h"
#include "videosynth/frame_line_layout.h"
#include "videosynth/generation_stage.h"
#include "videosynth/picture_response_filter.h"
#include "videosynth/picture_truth.h"
#include "videosynth/progressive_frame_source.h"
#include "videosynth/timing_constants.h"
#include "videosynth/timing_offset_filter.h"

namespace videosynth {
namespace {

constexpr double kPi = 3.14159265358979323846;

// How closely the stored signal matches the picture truth: fixed-point
// rounding and float truth, and on PAL, where the truth is delayed onto the
// 4fsc lattice before the comparison, the delay of a modulated carrier
// against the modulation of delayed axes (under -80 dB of a 300 mV carrier).
constexpr double kTruthLumaToleranceMv = 1e-3;
constexpr double kTruthChromaResidualMv = 1e-3;
constexpr double kTruthDelayedChromaResidualMv = 0.05;
constexpr double kTruthChromaGainTolerance = 1e-6;
constexpr double kTruthDelayedChromaGainTolerance = 1e-4;

// Source pictures by name. "flat_v.mkv" is a uniform grey with +V (Cr above
// 512); "edge.mkv" is a luma and chroma step at the raster's centre column;
// "rows.mkv" alternates dark and light grey rows (the row Nyquist frequency).
YCbCr444Pixel PixelFor(const std::string& source, int x, int y) {
  if (source == "rows.mkv") {
    return (y % 2 == 0) ? YCbCr444Pixel{.y = 300, .cb = 512, .cr = 512}
                        : YCbCr444Pixel{.y = 700, .cb = 512, .cr = 512};
  }
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
            PixelFor(source_, x, y);
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
        const int count = layout.samples_per_line;
        // The truth is the line's 0H-relative picture. On PAL the stored line
        // is that picture delayed onto the 4fsc lattice, so the truth is
        // delayed the same way before it is compared. The truth ends at the
        // window's ends, where chroma is cut off and the band-limited luma
        // runs on into the porches, so on PAL both comparisons leave the
        // delay kernel's reach at each end out.
        auto delayed = [&](const float* plane, double outside) {
          return test_support::LatticeDelayedLine(
              standard, line, layout.first_sample, count, [&](int j) {
                const int x = j - layout.first_sample;
                return (x >= 0 && x < count)
                           ? static_cast<double>(plane[row_offset + x]) +
                                 outside
                           : outside;
              });
        };
        // The kernel reads up to kTimingOffsetHalfTaps samples either side,
        // plus the whole-sample part of the delay.
        const int margin =
            Is625LineStandard(standard) ? kTimingOffsetHalfTaps + 2 : 0;
        const std::vector<double> expected_y = delayed(truth.y_mv.data(), 0.0);
        for (int x = margin; x < count - margin; ++x) {
          ASSERT_NEAR(expected_y[static_cast<std::size_t>(x)],
                      SampleFixedToMillivolts(frames.y_mv[base + x] - blanking),
                      kTruthLumaToleranceMv)
              << StandardToString(standard) << " line " << line;
        }
        const std::vector<double> u_delayed = delayed(truth.u_mv.data(), 0.0);
        const std::vector<double> v_delayed = delayed(truth.v_mv.data(), 0.0);
        const std::vector<float> u_fit(u_delayed.begin(), u_delayed.end());
        const std::vector<float> v_fit(v_delayed.begin(), v_delayed.end());
        const LineFit plus =
            FitLine(&frames.c_mv[base + margin], &u_fit[margin], &v_fit[margin],
                    count - (2 * margin), 1);
        const LineFit minus =
            FitLine(&frames.c_mv[base + margin], &u_fit[margin], &v_fit[margin],
                    count - (2 * margin), -1);
        const LineFit& best =
            plus.max_residual_mv <= minus.max_residual_mv ? plus : minus;
        const bool lattice_delayed = Is625LineStandard(standard);
        ASSERT_LT(best.max_residual_mv, lattice_delayed
                                            ? kTruthDelayedChromaResidualMv
                                            : kTruthChromaResidualMv)
            << StandardToString(standard) << " line " << line;
        ASSERT_NEAR(best.amplitude, 1.0,
                    lattice_delayed ? kTruthDelayedChromaGainTolerance
                                    : kTruthChromaGainTolerance);
        if (standard == Standard::kNtsc) {
          // NTSC has no V-switch.
          ASSERT_LT(plus.max_residual_mv, 1e-3) << "line " << line;
        }
      }
    }
  }
}

TEST(PictureTruthGenerationTest, PalTruthIsTheUndelayedLine) {
  // The truth is taken from the 0H-relative render, before the lattice line
  // shifter. On line 310 (d = 1236/625 samples) it lags the stored line at
  // the edge by about two samples, so read as stored samples from
  // first_sample it misses the edge by far more than rounding.
  const Project project = MakeProject(Standard::kPal, "edge.mkv");
  const GeneratedFrames frames = Generate(project, 1, true);
  ASSERT_EQ(frames.truth.size(), 1U);
  const PictureTruthLayout layout =
      BuildPictureTruthLayout(project.cvbs_presets);
  const TimingConstants timing = GetTimingConstants(Standard::kPal);
  const std::vector<int> offsets = BuildLineSampleOffsets(BuildLineSampleCounts(
      Standard::kPal, timing.lines_per_frame, timing.samples_per_line_4fsc));
  const SampleFixed blanking = MillivoltsToSampleFixed(
      GetSignalLevels(project.cvbs_presets).blanking_mv);
  constexpr int kLine = 310;
  const int row = layout.RowForLine(kLine);
  ASSERT_GE(row, 0);
  const std::size_t base = static_cast<std::size_t>(offsets[kLine - 1]) +
                           static_cast<std::size_t>(layout.first_sample);
  const std::size_t row_offset =
      static_cast<std::size_t>(row) * layout.samples_per_line;
  double worst_mv = 0.0;
  for (int x = 0; x < layout.samples_per_line; ++x) {
    worst_mv = std::max(
        worst_mv,
        std::abs(static_cast<double>(frames.truth[0].y_mv[row_offset + x]) -
                 SampleFixedToMillivolts(frames.y_mv[base + x] - blanking)));
  }
  EXPECT_GT(worst_mv, 50.0);
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

PictureResponseParameters CustomResponse(
    std::vector<NoiseSpectrumPoint> horizontal,
    std::vector<VerticalResponsePoint> vertical) {
  PictureResponseParameters response;
  response.enabled = true;
  response.horizontal_db = std::move(horizontal);
  response.vertical_db = std::move(vertical);
  return response;
}

// The designed vertical kernel's gain in dB at the row Nyquist frequency:
// alternate taps summed with alternating signs.
double RowNyquistResponseDb(const PictureResponseParameters& response) {
  const PictureResponseFilter filter(response, 1.0);
  double sum = 0.0;
  const std::vector<double>& kernel = filter.vertical_kernel();
  for (std::size_t n = 0; n < kernel.size(); ++n) {
    sum += (n % 2U == 0U) ? kernel[n] : -kernel[n];
  }
  return 20.0 * std::log10(std::abs(sum));
}

// Swing in mV between truth rows row and row + 1 at the centre column.
double TruthRowSwingMv(const GeneratedFrames& frames,
                       const PictureTruthLayout& layout, int row) {
  const std::size_t width = static_cast<std::size_t>(layout.samples_per_line);
  const std::size_t column = width / 2U;
  const std::vector<float>& y = frames.truth[0].y_mv;
  return static_cast<double>(
             y[(static_cast<std::size_t>(row) * width) + column]) -
         static_cast<double>(
             y[(static_cast<std::size_t>(row + 1) * width) + column]);
}

TEST(PictureResponseGenerationTest, FlatResponseIsBitIdenticalToNoBlock) {
  Project project = MakeProject(Standard::kPal, "edge.mkv");
  const GeneratedFrames plain = Generate(project, 1, true);
  project.sections[0].picture_response =
      CustomResponse({{0.0, 0.0}}, {{0.0, 0.0}});
  const GeneratedFrames flat = Generate(project, 1, true);
  EXPECT_EQ(flat.y_mv, plain.y_mv);
  EXPECT_EQ(flat.c_mv, plain.c_mv);
  EXPECT_EQ(flat.truth[0].y_mv, plain.truth[0].y_mv);
}

TEST(PictureResponseGenerationTest, HorizontalResponseLeavesRowDetailAlone) {
  // Every row of rows.mkv is flat along its length.
  Project project = MakeProject(Standard::kPal, "rows.mkv");
  const GeneratedFrames plain = Generate(project, 1, false);
  project.sections[0].picture_response =
      CustomResponse({{0.0, 0.0}, {2.0, 0.0}, {5.0, -20.0}}, {});
  const GeneratedFrames shaped = Generate(project, 1, false);
  EXPECT_EQ(shaped.y_mv, plain.y_mv);
}

TEST(PictureResponseGenerationTest, VerticalResponseLeavesColumnDetailAlone) {
  // Every column of edge.mkv is flat down its height.
  Project project = MakeProject(Standard::kPal, "edge.mkv");
  const GeneratedFrames plain = Generate(project, 1, false);
  project.sections[0].picture_response =
      CustomResponse({}, {{0.0, 0.0}, {0.5, -20.0}});
  const GeneratedFrames shaped = Generate(project, 1, false);
  EXPECT_EQ(shaped.y_mv, plain.y_mv);
  EXPECT_EQ(shaped.c_mv, plain.c_mv);
}

// The vertical response filters the progressive picture's rows before the
// fields are split, so alternate frame rows (one field dark, the other light)
// are pulled together in both fields, and the truth carries it.
TEST(PictureResponseGenerationTest, VerticalResponseActsOnFrameRowsInTruth) {
  for (const Standard standard : {Standard::kPal, Standard::kNtsc}) {
    for (const HorizontalResample resample :
         {HorizontalResample::kNearest, HorizontalResample::kBandlimited}) {
      Project project = MakeProject(standard, "rows.mkv");
      project.sections[0].horizontal_resample = resample;
      const PictureTruthLayout layout =
          BuildPictureTruthLayout(project.cvbs_presets);
      const GeneratedFrames plain = Generate(project, 1, true);
      project.sections[0].picture_response =
          CustomResponse({}, {{0.0, 0.0}, {0.5, -20.0}});
      const GeneratedFrames shaped = Generate(project, 1, true);
      const int row = layout.rows / 2;
      const double plain_swing = TruthRowSwingMv(plain, layout, row);
      const double shaped_swing = TruthRowSwingMv(shaped, layout, row);
      ASSERT_GT(std::abs(plain_swing), 100.0);
      EXPECT_NEAR(20.0 * std::log10(shaped_swing / plain_swing),
                  RowNyquistResponseDb(project.sections[0].picture_response),
                  0.01)
          << StandardToString(standard);
      // The mean level is unchanged.
      const std::size_t width =
          static_cast<std::size_t>(layout.samples_per_line);
      const std::size_t at = (static_cast<std::size_t>(row) * width) + 300U;
      EXPECT_NEAR(shaped.truth[0].y_mv[at] + shaped.truth[0].y_mv[at + width],
                  plain.truth[0].y_mv[at] + plain.truth[0].y_mv[at + width],
                  0.01);
    }
  }
}

}  // namespace
}  // namespace videosynth
