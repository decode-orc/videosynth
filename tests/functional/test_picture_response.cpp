/*
 * File:        test_picture_response.cpp
 * Module:      generation_tests
 * Purpose:     End-to-end checks of the per-section picture response on the
 *              TESTCARD-F still: the domesday preset gives the measured
 *              Domesday disc band ratios in the picture truth's luma, and
 *              chroma and everything outside the picture are untouched.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

#include "videosynth/fixed_point.h"
#include "videosynth/generation_stage.h"
#include "videosynth/picture_truth.h"
#include "videosynth/project_validator.h"
#include "videosynth/timing_constants.h"

namespace videosynth {
namespace {

constexpr double kPi = 3.14159265358979323846;

// Rows and samples kept clear of the picture's edges, where the half-picture
// lines and the window ends lie.
constexpr int kRowMargin = 4;
constexpr int kSampleMargin = 16;

Project MakeTestCardProject() {
  Project project;
  project.cvbs_presets.video_standard_preset = Standard::kPal;
  project.cvbs_presets.sample_encoding_preset = "CVBS_U10_4FSC";
  project.cvbs_presets.signal_state_preset = "STANDARD_STABLE_LOCKED";
  project.output.video_path = "out.cvbs";
  project.output.metadata_path = "out.meta";
  project.sections.push_back(
      Section{.name = "Card",
              .type = "progressive",
              .line_injections = {},
              .source = (std::filesystem::path(VIDEOSYNTH_SOURCE_DIR) /
                         "videosynth-assets/assets/exr/720x576/TESTCARD-F.exr")
                            .string(),
              .duration_frames = 1});
  return project;
}

struct Render {
  std::vector<SampleFixed> y;
  std::vector<SampleFixed> c;
  PictureTruthFrame truth;
  PictureTruthLayout layout;
};

Render RenderFrame(const Project& project) {
  Render render;
  GenerationStage stage;
  std::vector<IGenerationStage::FrameScheduleItem> schedule;
  std::vector<std::string> errors;
  EXPECT_TRUE(stage.BuildFrameSchedule(project, &schedule, &errors))
      << (errors.empty() ? "" : errors[0]);
  std::vector<PictureTruthFrame> truth;
  EXPECT_TRUE(stage.GenerateFrameBatchWithTruth(
      project, schedule, 0, 1, &render.y, &render.c, &truth, &errors))
      << (errors.empty() ? "" : errors[0]);
  if (!truth.empty()) {
    render.truth = truth[0];
  }
  render.layout = BuildPictureTruthLayout(project.cvbs_presets);
  return render;
}

// Power of the Hann-tapered, mean-removed sequence at each DFT bin from 0 to
// the Nyquist bin.
std::vector<double> PowerSpectrum(const std::vector<double>& values) {
  const std::size_t count = values.size();
  double mean = 0.0;
  for (const double v : values) {
    mean += v;
  }
  mean /= static_cast<double>(count);
  std::vector<double> tapered(count);
  for (std::size_t n = 0; n < count; ++n) {
    const double window =
        0.5 - (0.5 * std::cos(2.0 * kPi * static_cast<double>(n) /
                              static_cast<double>(count - 1U)));
    tapered[n] = window * (values[n] - mean);
  }
  std::vector<double> power((count / 2U) + 1U);
  for (std::size_t k = 0; k < power.size(); ++k) {
    double re = 0.0;
    double im = 0.0;
    for (std::size_t n = 0; n < count; ++n) {
      const double angle =
          2.0 * kPi * static_cast<double>(k * n) / static_cast<double>(count);
      re += tapered[n] * std::cos(angle);
      im -= tapered[n] * std::sin(angle);
    }
    power[k] = (re * re) + (im * im);
  }
  return power;
}

// Luma power summed over every row, per horizontal DFT bin.
std::vector<double> HorizontalSpectrum(const Render& render) {
  const PictureTruthLayout& layout = render.layout;
  const int width = layout.samples_per_line - (2 * kSampleMargin);
  std::vector<double> total;
  for (int row = kRowMargin; row < layout.rows - kRowMargin; ++row) {
    std::vector<double> values(static_cast<std::size_t>(width));
    for (int x = 0; x < width; ++x) {
      values[static_cast<std::size_t>(x)] =
          render.truth
              .y_mv[(static_cast<std::size_t>(row) *
                     static_cast<std::size_t>(layout.samples_per_line)) +
                    static_cast<std::size_t>(x + kSampleMargin)];
    }
    const std::vector<double> power = PowerSpectrum(values);
    total.resize(power.size(), 0.0);
    for (std::size_t k = 0; k < power.size(); ++k) {
      total[k] += power[k];
    }
  }
  return total;
}

// Luma power summed over every fourth column, per vertical DFT bin.
std::vector<double> VerticalSpectrum(const Render& render) {
  const PictureTruthLayout& layout = render.layout;
  const int rows = layout.rows - (2 * kRowMargin);
  std::vector<double> total;
  for (int x = kSampleMargin; x < layout.samples_per_line - kSampleMargin;
       x += 4) {
    std::vector<double> values(static_cast<std::size_t>(rows));
    for (int row = 0; row < rows; ++row) {
      values[static_cast<std::size_t>(row)] =
          render.truth
              .y_mv[(static_cast<std::size_t>(row + kRowMargin) *
                     static_cast<std::size_t>(layout.samples_per_line)) +
                    static_cast<std::size_t>(x)];
    }
    const std::vector<double> power = PowerSpectrum(values);
    total.resize(power.size(), 0.0);
    for (std::size_t k = 0; k < power.size(); ++k) {
      total[k] += power[k];
    }
  }
  return total;
}

// Ratio in dB of the shaped to the plain power in the bins whose frequency
// (bin index * bin_width) lies in [low, high).
double BandRatioDb(const std::vector<double>& shaped,
                   const std::vector<double>& plain, double bin_width,
                   double low, double high) {
  double shaped_sum = 0.0;
  double plain_sum = 0.0;
  for (std::size_t k = 1; k < plain.size(); ++k) {
    const double frequency = static_cast<double>(k) * bin_width;
    if (frequency >= low && frequency < high) {
      shaped_sum += shaped[k];
      plain_sum += plain[k];
    }
  }
  return 10.0 * std::log10(shaped_sum / plain_sum);
}

struct Band {
  double low;
  double high;
  double measured_db;
  double tolerance_db;
};

class PictureResponseTestCardTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    Project project = MakeTestCardProject();
    plain_ = new Render(RenderFrame(project));
    project.sections[0].picture_response = PictureResponseParameters{
        .enabled = true, .preset = PictureResponsePreset::kDomesday};
    const ValidationResult validation = ProjectValidator().Validate(project);
    ASSERT_TRUE(validation.is_valid) << validation.errors[0];
    shaped_ = new Render(RenderFrame(project));
  }
  static void TearDownTestSuite() {
    delete plain_;
    delete shaped_;
    plain_ = nullptr;
    shaped_ = nullptr;
  }

  static Render* plain_;
  static Render* shaped_;
};

Render* PictureResponseTestCardTest::plain_ = nullptr;
Render* PictureResponseTestCardTest::shaped_ = nullptr;

// videosynth issue #18: Domesday's Test Card F against the bench's, in luma
// power per horizontal band (MHz).
TEST_F(PictureResponseTestCardTest, DomesdayHorizontalBandRatiosMatchTheDisc) {
  ASSERT_FALSE(shaped_->truth.y_mv.empty());
  const std::vector<double> plain = HorizontalSpectrum(*plain_);
  const std::vector<double> shaped = HorizontalSpectrum(*shaped_);
  const double bin_mhz = (plain_->layout.sample_rate_hz / 1.0e6) /
                         static_cast<double>(plain_->layout.samples_per_line -
                                             (2 * kSampleMargin));
  const Band bands[] = {
      {0.5, 2.5, 0.0, 0.5},  {2.5, 3.0, -2.1, 1.0}, {3.0, 3.5, -2.2, 1.0},
      {3.5, 4.0, -2.2, 1.0}, {4.0, 4.5, -3.6, 1.0}, {4.5, 5.0, -7.7, 1.0},
      {5.0, 5.5, -9.8, 1.0},
  };
  for (const Band& band : bands) {
    const double ratio =
        BandRatioDb(shaped, plain, bin_mhz, band.low, band.high);
    EXPECT_NEAR(ratio, band.measured_db, band.tolerance_db)
        << band.low << "-" << band.high << " MHz";
  }
}

// Vertical bands in cycles per frame row. The measured 5/16-3/8 figure
// (-1.6 dB) breaks the monotonic fall of the bands either side of it, so the
// preset (a monotonic camera / flicker-filter roll-off) is only required to
// fall between those neighbours there.
TEST_F(PictureResponseTestCardTest, DomesdayVerticalBandRatiosMatchTheDisc) {
  const std::vector<double> plain = VerticalSpectrum(*plain_);
  const std::vector<double> shaped = VerticalSpectrum(*shaped_);
  const double bin_cycles =
      1.0 / static_cast<double>(plain_->layout.rows - (2 * kRowMargin));
  const Band bands[] = {
      {0.25, 0.3125, -4.2, 1.0},
      {0.375, 0.4375, -9.5, 1.0},
      {0.4375, 0.5001, -10.4, 1.0},
  };
  for (const Band& band : bands) {
    const double ratio =
        BandRatioDb(shaped, plain, bin_cycles, band.low, band.high);
    EXPECT_NEAR(ratio, band.measured_db, band.tolerance_db)
        << band.low << "-" << band.high << " cycles/row";
  }
  const double middle = BandRatioDb(shaped, plain, bin_cycles, 0.3125, 0.375);
  EXPECT_LT(middle, -4.2);
  EXPECT_GT(middle, -9.5);
}

// The response shapes the picture luma only: chroma, and every luma sample
// the truth does not cover (sync, blanking, VBI), are bit-identical.
TEST_F(PictureResponseTestCardTest, OnlyThePictureLumaChanges) {
  EXPECT_EQ(shaped_->c, plain_->c);
  EXPECT_EQ(shaped_->truth.u_mv, plain_->truth.u_mv);
  EXPECT_EQ(shaped_->truth.v_mv, plain_->truth.v_mv);
  ASSERT_EQ(shaped_->y.size(), plain_->y.size());
  std::size_t changed = 0;
  for (std::size_t s = 0; s < plain_->y.size(); ++s) {
    if (shaped_->y[s] != plain_->y[s]) {
      ++changed;
    }
  }
  EXPECT_GT(changed, 100000U);
  EXPECT_NE(shaped_->truth.y_mv, plain_->truth.y_mv);
}

TEST(PictureResponseValidationTest, CustomCurveRangesAreEnforced) {
  struct Case {
    std::vector<NoiseSpectrumPoint> horizontal;
    std::vector<VerticalResponsePoint> vertical;
    bool valid;
    const char* error;
  };
  // 2 f_sc on PAL: 8.867 MHz.
  const Case cases[] = {
      {{{0.0, 0.0}, {5.25, -10.0}}, {{0.0, 0.0}, {0.5, -10.0}}, true, ""},
      {{{0.0, 0.0}, {8.8, -60.0}}, {}, true, ""},
      {{}, {{0.25, 20.0}}, true, ""},
      {{}, {}, false, "at least one of horizontal_db, vertical_db"},
      {{{0.0, 0.0}, {9.0, -10.0}}, {}, false, "frequency_mhz must be in"},
      {{{3.0, 0.0}, {2.0, -10.0}}, {}, false, "strictly ascending"},
      {{{0.0, 0.0}, {5.0, -61.0}}, {}, false, "relative_db must be in"},
      {{}, {{0.0, 0.0}, {0.51, -10.0}}, false, "cycles_per_row must be in"},
      {{}, {{0.0, 21.0}}, false, "relative_db must be in"},
  };
  for (const Case& test_case : cases) {
    Project project = MakeTestCardProject();
    PictureResponseParameters& response = project.sections[0].picture_response;
    response.enabled = true;
    response.horizontal_db = test_case.horizontal;
    response.vertical_db = test_case.vertical;
    const ValidationResult result = ProjectValidator().Validate(project);
    EXPECT_EQ(result.is_valid, test_case.valid) << test_case.error;
    if (!test_case.valid && !result.errors.empty()) {
      EXPECT_NE(result.errors[0].find("Picture response validation error"),
                std::string::npos)
          << result.errors[0];
      EXPECT_NE(result.errors[0].find(test_case.error), std::string::npos)
          << result.errors[0];
    }
  }
}

TEST(PictureResponseValidationTest, PresetIsValidOnEveryStandard) {
  for (const Standard standard :
       {Standard::kPal, Standard::kNtsc, Standard::kPalM}) {
    Project project = MakeTestCardProject();
    project.cvbs_presets.video_standard_preset = standard;
    if (!Is625LineStandard(standard)) {
      project.sections[0].source =
          (std::filesystem::path(VIDEOSYNTH_SOURCE_DIR) /
           "videosynth-assets/assets/exr/720x486/MULTIBURST.exr")
              .string();
    }
    project.sections[0].picture_response = PictureResponseParameters{
        .enabled = true, .preset = PictureResponsePreset::kDomesday};
    const ValidationResult result = ProjectValidator().Validate(project);
    EXPECT_TRUE(result.is_valid)
        << StandardToString(standard) << ": "
        << (result.errors.empty() ? "" : result.errors[0]);
  }
}

}  // namespace
}  // namespace videosynth
