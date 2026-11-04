/*
 * File:        test_line_timing_stage.cpp
 * Module:      line_timing_tests
 * Purpose:     Validates the `line_timing:` block (parsing, validation and
 *              round trip), the variable-delay interpolator, the per-line
 *              timing error process and the LineTimingStage on the active
 *              picture.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "videosynth/fixed_point.h"
#include "videosynth/frame_enrichment.h"
#include "videosynth/frame_line_layout.h"
#include "videosynth/line_timing_stage.h"
#include "videosynth/model.h"
#include "videosynth/picture_chroma.h"
#include "videosynth/project_validator.h"
#include "videosynth/signal_timing_model.h"
#include "videosynth/timing_constants.h"
#include "videosynth/variable_delay_interpolator.h"
#include "videosynth/yaml_project_emitter.h"
#include "videosynth/yaml_project_parser.h"

namespace videosynth {
namespace {

constexpr double kPi = 3.14159265358979323846;

const std::string kYamlPrefix =
    "cvbs_presets:\n"
    "  video_standard_preset: PAL\n"
    "  sample_encoding_preset: CVBS_U10_4FSC\n"
    "  signal_state_preset: STANDARD_STABLE_LOCKED\n"
    "output:\n"
    "  video_path: out.cvbs\n";

const std::string kYamlSections =
    "sections:\n"
    "  - name: S\n"
    "    type: progressive\n"
    "    source: f.exr\n"
    "    duration_frames: 8\n";

ParseResult ParseWithLineTiming(const std::string& block) {
  return YamlProjectParser().ParseString(kYamlPrefix + block + kYamlSections);
}

Project MakeLineTimingProject(Standard standard) {
  Project project;
  project.cvbs_presets.video_standard_preset = standard;
  project.cvbs_presets.sample_encoding_preset = "CVBS_U10_4FSC";
  project.cvbs_presets.signal_state_preset =
      GeneratedSignalStatePreset(standard);
  project.output.video_path = "/tmp/line_timing_test.cvbs";
  project.output.metadata_path = "/tmp/line_timing_test.meta";
  project.line_timing.enabled = true;

  Section section;
  section.name = "S";
  section.type = "progressive";
  section.source = "f.exr";
  section.duration_frames = 8;
  project.sections.push_back(section);
  return project;
}

// A schedule whose frame i shows disc frame disc_frames[i].
std::vector<IGenerationStage::FrameScheduleItem> MakeSchedule(
    const Project& project, const std::vector<std::size_t>& disc_frames) {
  std::vector<IGenerationStage::FrameScheduleItem> schedule(disc_frames.size());
  for (std::size_t i = 0; i < disc_frames.size(); ++i) {
    schedule[i].section = &project.sections[0];
    schedule[i].section_frame_index = static_cast<int>(i);
    schedule[i].source_frame_index = static_cast<int>(i);
    auto enrichment = std::make_shared<FrameEnrichment>();
    enrichment->disc_frame_index = disc_frames[i];
    schedule[i].enrichment = std::move(enrichment);
  }
  return schedule;
}

std::vector<IGenerationStage::FrameScheduleItem> MakeSchedule(
    const Project& project, std::size_t frames) {
  std::vector<std::size_t> disc_frames(frames);
  for (std::size_t i = 0; i < frames; ++i) {
    disc_frames[i] = i;
  }
  return MakeSchedule(project, disc_frames);
}

LineTimingLayer Layer(double line_ns, double correlation, double field_ns,
                      double slope_ns, std::int64_t seed) {
  LineTimingLayer layer;
  layer.enabled = true;
  layer.line_ns = line_ns;
  layer.line_correlation = correlation;
  layer.field_ns = field_ns;
  layer.slope_ns = slope_ns;
  layer.seed_specified = true;
  layer.seed = seed;
  return layer;
}

std::size_t FrameSamples(Standard standard) {
  return static_cast<std::size_t>(SamplesPerFrame4fsc(standard));
}

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------

TEST(LineTimingStageTest, ParserLeavesLineTimingDisabledWhenAbsent) {
  const ParseResult result = ParseWithLineTiming("");
  ASSERT_TRUE(result.ok);
  EXPECT_FALSE(result.project.line_timing.enabled);
}

TEST(LineTimingStageTest, ParserReadsFullBlock) {
  const ParseResult result = ParseWithLineTiming(
      "line_timing:\n"
      "  line_ns: 4.9\n"
      "  line_correlation: 0.6\n"
      "  field_ns: 3.1\n"
      "  slope_ns: 3.4\n"
      "  seed: 7\n"
      "  include_burst: true\n"
      "  shared:\n"
      "    line_ns: 3.0\n"
      "    line_correlation: 0.5\n"
      "    field_ns: 2.0\n"
      "    slope_ns: 1.5\n"
      "    seed: 7001\n");
  ASSERT_TRUE(result.ok) << result.errors.front();
  const LineTimingParameters& line_timing = result.project.line_timing;
  EXPECT_TRUE(line_timing.enabled);
  EXPECT_TRUE(line_timing.include_burst);
  EXPECT_EQ(line_timing.capture, Layer(4.9, 0.6, 3.1, 3.4, 7));
  EXPECT_EQ(line_timing.shared, Layer(3.0, 0.5, 2.0, 1.5, 7001));
}

TEST(LineTimingStageTest, ParserDefaultsOmittedKeys) {
  const ParseResult result =
      ParseWithLineTiming("line_timing:\n  line_ns: 5.0\n");
  ASSERT_TRUE(result.ok);
  const LineTimingParameters& line_timing = result.project.line_timing;
  EXPECT_TRUE(line_timing.capture.enabled);
  EXPECT_DOUBLE_EQ(line_timing.capture.line_correlation, 0.0);
  EXPECT_DOUBLE_EQ(line_timing.capture.field_ns, 0.0);
  EXPECT_FALSE(line_timing.capture.seed_specified);
  EXPECT_FALSE(line_timing.shared.enabled);
  EXPECT_FALSE(line_timing.include_burst);
}

TEST(LineTimingStageTest, ParserAcceptsSharedLayerAlone) {
  const ParseResult result = ParseWithLineTiming(
      "line_timing:\n  shared:\n    field_ns: 2.0\n    seed: 3\n");
  ASSERT_TRUE(result.ok) << result.errors.front();
  EXPECT_FALSE(result.project.line_timing.capture.enabled);
  EXPECT_TRUE(result.project.line_timing.shared.enabled);
}

TEST(LineTimingStageTest, ParserRejectsEmptyAndMalformedBlocks) {
  struct Case {
    std::string block;
    std::string error;
  };
  const Case cases[] = {
      {"line_timing: {}\n",
       "line_timing block must set at least one of: line_ns, field_ns, "
       "slope_ns, shared."},
      {"line_timing:\n  line_correlation: 0.5\n",
       "line_timing block must set at least one of: line_ns, field_ns, "
       "slope_ns, shared."},
      {"line_timing:\n  shared: { seed: 4 }\n",
       "line_timing.shared must set at least one of: line_ns, field_ns, "
       "slope_ns."},
      {"line_timing:\n  seed: 4\n  shared: { line_ns: 1.0, seed: 5 }\n",
       "line_timing.seed seeds the capture layer, which sets none of: "
       "line_ns, field_ns, slope_ns."},
      {"line_timing:\n  line_ns: fast\n",
       "line_timing.line_ns must be a number."},
      {"line_timing:\n  line_ns: 1.0\n  seed: x\n",
       "line_timing.seed must be an integer."},
      {"line_timing:\n  line_ns: 1.0\n  include_burst: maybe\n",
       "line_timing.include_burst must be true or false."},
      {"line_timing: 3\n", "Top-level 'line_timing' must be a map/object."},
      {"line_timing:\n  line_ns: 1.0\n  shared: 2\n",
       "line_timing.shared must be a map/object."},
  };
  for (const Case& c : cases) {
    const ParseResult result = ParseWithLineTiming(c.block);
    ASSERT_FALSE(result.ok) << c.block;
    EXPECT_EQ(result.errors[0], c.error) << c.block;
  }
  EXPECT_FALSE(
      ParseWithLineTiming("line_timing:\n  line_ns: 1.0\n  jitter: 2\n").ok);
  EXPECT_FALSE(
      ParseWithLineTiming(
          "line_timing:\n  shared: { line_ns: 1.0, seed: 2, burst: true }\n")
          .ok);
}

TEST(LineTimingStageTest, EmitterRoundTripsBlock) {
  for (const std::string& block :
       {std::string("line_timing:\n  line_ns: 4.9\n  line_correlation: 0.6\n"
                    "  field_ns: 3.1\n  slope_ns: 3.4\n  seed: 7\n"
                    "  include_burst: true\n"
                    "  shared: { line_ns: 2.5, field_ns: 1.0, seed: 9 }\n"),
        std::string("line_timing:\n  slope_ns: 2.0\n"),
        std::string("line_timing:\n  shared: { slope_ns: 2.0, seed: 1 }\n")}) {
    const ParseResult parsed = ParseWithLineTiming(block);
    ASSERT_TRUE(parsed.ok) << block;
    const std::string emitted = YamlProjectEmitter().EmitString(parsed.project);
    const ParseResult reparsed = YamlProjectParser().ParseString(emitted);
    ASSERT_TRUE(reparsed.ok) << emitted;
    EXPECT_EQ(reparsed.project.line_timing, parsed.project.line_timing)
        << emitted;
  }
}

// ---------------------------------------------------------------------------
// Validator
// ---------------------------------------------------------------------------

TEST(LineTimingStageTest, ValidatorAcceptsRangeLimits) {
  Project project = MakeLineTimingProject(Standard::kNtsc);
  project.line_timing.capture = Layer(50.0, 0.99, 50.0, 50.0, 1);
  project.line_timing.shared = Layer(0.0, 0.0, 0.0, 50.0, 2);
  project.line_timing.include_burst = true;
  const ValidationResult result = ProjectValidator().Validate(project);
  EXPECT_TRUE(result.is_valid)
      << (result.errors.empty() ? "" : result.errors.front());
}

TEST(LineTimingStageTest, ValidatorRejectsOutOfRangeValues) {
  struct Case {
    LineTimingLayer capture;
    LineTimingLayer shared;
    std::string message_prefix;
  };
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const LineTimingLayer valid = Layer(1.0, 0.5, 1.0, 1.0, 1);
  const Case cases[] = {
      {Layer(50.1, 0.5, 1.0, 1.0, 1), valid,
       "line_ns must be in the range [0.0, 50.0]"},
      {Layer(-0.1, 0.5, 1.0, 1.0, 1), valid,
       "line_ns must be in the range [0.0, 50.0]"},
      {Layer(1.0, 1.0, 1.0, 1.0, 1), valid,
       "line_correlation must be in the range [0.0, 0.99]"},
      {Layer(1.0, -0.1, 1.0, 1.0, 1), valid,
       "line_correlation must be in the range [0.0, 0.99]"},
      {Layer(1.0, 0.5, nan, 1.0, 1), valid,
       "field_ns must be in the range [0.0, 50.0]"},
      {Layer(1.0, 0.5, 1.0, 51.0, 1), valid,
       "slope_ns must be in the range [0.0, 50.0]"},
      {valid, Layer(1.0, 0.5, 1.0, 60.0, 1),
       "shared.slope_ns must be in the range [0.0, 50.0]"},
  };
  for (const Case& c : cases) {
    Project project = MakeLineTimingProject(Standard::kPal);
    project.line_timing.capture = c.capture;
    project.line_timing.shared = c.shared;
    const ValidationResult result = ProjectValidator().Validate(project);
    ASSERT_FALSE(result.is_valid) << c.message_prefix;
    ASSERT_FALSE(result.errors.empty());
    EXPECT_EQ(result.errors[0].rfind(
                  "Line timing validation error: " + c.message_prefix, 0),
              0U)
        << result.errors[0];
  }
}

TEST(LineTimingStageTest, ValidatorRequiresSharedSeed) {
  Project project = MakeLineTimingProject(Standard::kPal);
  project.line_timing.shared = Layer(2.0, 0.5, 1.0, 1.0, 0);
  project.line_timing.shared.seed_specified = false;
  const ValidationResult result = ProjectValidator().Validate(project);
  ASSERT_FALSE(result.is_valid);
  EXPECT_EQ(result.errors[0],
            "Line timing validation error: shared.seed is required; the "
            "pressing layer must be identical in every capture of the "
            "pressing.");

  // The capture layer may draw a fresh seed every run.
  project.line_timing.shared = {};
  project.line_timing.capture = Layer(2.0, 0.5, 1.0, 1.0, 0);
  project.line_timing.capture.seed_specified = false;
  EXPECT_TRUE(ProjectValidator().Validate(project).is_valid);
}

TEST(LineTimingStageTest, ValidatorRejectsIncludeBurstOnSecam) {
  Project project = MakeLineTimingProject(Standard::kSecam);
  project.line_timing.capture = Layer(2.0, 0.5, 1.0, 1.0, 1);
  EXPECT_TRUE(ProjectValidator().Validate(project).is_valid);
  project.line_timing.include_burst = true;
  const ValidationResult result = ProjectValidator().Validate(project);
  ASSERT_FALSE(result.is_valid);
  bool found = false;
  for (const std::string& error : result.errors) {
    found = found || error ==
                         "Line timing validation error: include_burst needs a "
                         "colour burst; SECAM has none.";
  }
  EXPECT_TRUE(found);
}

// ---------------------------------------------------------------------------
// Variable-delay interpolator
// ---------------------------------------------------------------------------

TEST(LineTimingStageTest, InterpolatorReturnsSamplesAtIntegerPositions) {
  const VariableDelayInterpolator interpolator;
  std::mt19937_64 rng(11U);
  std::uniform_int_distribution<SampleFixed> value(-50000, 50000);
  std::vector<SampleFixed> signal(200);
  for (SampleFixed& sample : signal) {
    sample = value(rng);
  }
  for (std::size_t n = 0; n < signal.size(); ++n) {
    EXPECT_DOUBLE_EQ(
        interpolator.At(signal.data(), signal.size(), static_cast<double>(n)),
        static_cast<double>(signal[n]));
  }
}

TEST(LineTimingStageTest, InterpolatorPassesFlatLevels) {
  const VariableDelayInterpolator interpolator;
  const std::vector<SampleFixed> flat(100, 123456);
  for (double position : {-3.7, 0.25, 17.5, 50.001, 99.9, 104.2}) {
    EXPECT_NEAR(interpolator.At(flat.data(), flat.size(), position), 123456.0,
                1e-6);
  }
}

TEST(LineTimingStageTest, InterpolatorReadsBandLimitedSignalAtFractions) {
  // 0.5-5 MHz at the 625-line 4fsc rate, amplitude 100 mV.
  const VariableDelayInterpolator interpolator;
  const double rate = GetTimingConstants(Standard::kPal).sample_rate_4fsc_hz;
  const double amplitude = 100.0 * static_cast<double>(kSampleScale);
  for (double mhz : {0.5, 2.0, 5.0}) {
    const double omega = 2.0 * kPi * mhz * 1e6 / rate;
    std::vector<SampleFixed> signal(400);
    for (std::size_t n = 0; n < signal.size(); ++n) {
      signal[n] = static_cast<SampleFixed>(
          std::llround(amplitude * std::sin(omega * static_cast<double>(n))));
    }
    double worst = 0.0;
    for (double position = 100.0; position < 300.0; position += 0.137) {
      const double expected = amplitude * std::sin(omega * position);
      worst = std::max(
          worst,
          std::abs(interpolator.At(signal.data(), signal.size(), position) -
                   expected));
    }
    // Within 0.05 % of the amplitude.
    EXPECT_LT(worst, 5e-4 * amplitude) << mhz << " MHz";
  }
}

// ---------------------------------------------------------------------------
// Timing error process
// ---------------------------------------------------------------------------

struct ProcessStatistics {
  double within_rms = 0.0;
  double field_rms = 0.0;
  double slope_rms = 0.0;
  double lag1 = 0.0;
  double lag4 = 0.0;
  double field_pair = 0.0;
};

// The error of 0-based frame line `index`.
const LineTimingError& ErrorAt(const std::vector<LineTimingError>& errors,
                               int index) {
  return errors[static_cast<std::size_t>(index)];
}

// Statistics of the drawn errors over the picture lines of `frames` frames.
ProcessStatistics MeasureProcess(const LineTimingLayer& layer,
                                 Standard standard, std::size_t frames) {
  const TimingConstants timing = GetTimingConstants(standard);
  const ActiveRasterGeometry geometry =
      GetActiveRasterGeometry(standard, timing.sample_rate_4fsc_hz);
  const int field1_first = geometry.first_active_line_field1;
  const int field2_first = geometry.first_active_line_field2;
  const int rows = geometry.active_lines_per_field - 1;

  double within = 0.0;
  double field = 0.0;
  double slope = 0.0;
  double lag1 = 0.0;
  double lag4 = 0.0;
  double pair = 0.0;
  double pair_norm1 = 0.0;
  double pair_norm2 = 0.0;
  std::size_t within_count = 0;
  std::size_t lag1_count = 0;
  std::size_t lag4_count = 0;
  std::size_t field_count = 0;
  for (std::size_t f = 0; f < frames; ++f) {
    const std::vector<LineTimingError> errors =
        DrawLineTimingErrors(layer, standard, 1000U + f);
    std::vector<double> means;
    for (int first : {field1_first, field2_first}) {
      double mean = 0.0;
      for (int r = 0; r < rows; ++r) {
        mean += ErrorAt(errors, first + r - 1).offset_ns;
      }
      mean /= rows;
      means.push_back(mean);
      field += mean * mean;
      ++field_count;
      for (int r = 0; r < rows; ++r) {
        const LineTimingError& e = ErrorAt(errors, first + r - 1);
        const double x = e.offset_ns - mean;
        within += x * x;
        slope += e.slope_ns * e.slope_ns;
        ++within_count;
        if (r + 1 < rows) {
          lag1 += x * (ErrorAt(errors, first + r).offset_ns - mean);
          ++lag1_count;
        }
        if (r + 4 < rows) {
          lag4 += x * (ErrorAt(errors, first + r + 3).offset_ns - mean);
          ++lag4_count;
        }
      }
    }
    for (int r = 0; r < rows; ++r) {
      const double a =
          ErrorAt(errors, field1_first + r - 1).offset_ns - means[0];
      const double b =
          ErrorAt(errors, field2_first + r - 1).offset_ns - means[1];
      pair += a * b;
      pair_norm1 += a * a;
      pair_norm2 += b * b;
    }
  }
  ProcessStatistics stats;
  const double within_variance = within / static_cast<double>(within_count);
  stats.within_rms = std::sqrt(within_variance);
  stats.field_rms = std::sqrt(field / static_cast<double>(field_count));
  stats.slope_rms = std::sqrt(slope / static_cast<double>(within_count));
  stats.lag1 = (lag1 / static_cast<double>(lag1_count)) / within_variance;
  stats.lag4 = (lag4 / static_cast<double>(lag4_count)) / within_variance;
  stats.field_pair = pair / std::sqrt(pair_norm1 * pair_norm2);
  return stats;
}

TEST(LineTimingStageTest, ProcessMatchesConfiguredFigures) {
  const LineTimingLayer layer = Layer(4.9, 0.6, 3.1, 3.4, 0);
  for (Standard standard : {Standard::kPal, Standard::kNtsc}) {
    const ProcessStatistics stats = MeasureProcess(layer, standard, 400U);
    EXPECT_NEAR(stats.within_rms, 4.9, 0.05 * 4.9);
    EXPECT_NEAR(stats.field_rms, 3.1, 0.1 * 3.1);
    EXPECT_NEAR(stats.slope_rms, 3.4, 0.05 * 3.4);
    EXPECT_NEAR(stats.lag1, 0.6, 0.03);
    EXPECT_NEAR(stats.lag4, std::pow(0.6, 4.0), 0.03);
    EXPECT_NEAR(stats.field_pair, 0.0, 0.03);
  }
}

TEST(LineTimingStageTest, ProcessIsDeterministicAndFiguresAreIndependent) {
  const LineTimingLayer layer = Layer(4.9, 0.6, 3.1, 3.4, 0);
  const auto first = DrawLineTimingErrors(layer, Standard::kPal, 42U);
  const auto again = DrawLineTimingErrors(layer, Standard::kPal, 42U);
  const auto other = DrawLineTimingErrors(layer, Standard::kPal, 43U);
  LineTimingLayer no_slope = layer;
  no_slope.slope_ns = 0.0;
  const auto unsloped = DrawLineTimingErrors(no_slope, Standard::kPal, 42U);
  bool differs = false;
  for (std::size_t i = 0; i < first.size(); ++i) {
    EXPECT_EQ(first[i].offset_ns, again[i].offset_ns);
    EXPECT_EQ(first[i].slope_ns, again[i].slope_ns);
    // Removing the slope leaves the shifts as they were.
    EXPECT_EQ(unsloped[i].offset_ns, first[i].offset_ns);
    EXPECT_EQ(unsloped[i].slope_ns, 0.0);
    differs = differs || other[i].offset_ns != first[i].offset_ns;
  }
  EXPECT_TRUE(differs);
}

// ---------------------------------------------------------------------------
// Stage
// ---------------------------------------------------------------------------

// A still frame: band-limited luma texture (0.3-2.5 MHz, mean 350 mV) on
// every line and a 4.43 MHz tone in C, identical on every call.
void FillStillFrame(Standard standard, SampleFixed* y, SampleFixed* c) {
  const TimingConstants timing = GetTimingConstants(standard);
  const std::size_t samples = FrameSamples(standard);
  std::mt19937_64 rng(5U);
  std::uniform_real_distribution<double> phase(0.0, 2.0 * kPi);
  constexpr double kMhz[] = {0.3, 0.7, 1.1, 1.6, 2.1, 2.5};
  double phases[6];
  for (double& p : phases) {
    p = phase(rng);
  }
  const double scale = static_cast<double>(kSampleScale);
  const double fsc_omega = 2.0 * kPi / 4.0;
  for (std::size_t n = 0; n < samples; ++n) {
    double value = 350.0;
    for (int k = 0; k < 6; ++k) {
      value += 40.0 *
               std::sin((2.0 * kPi * kMhz[k] * 1e6 /
                         timing.sample_rate_4fsc_hz * static_cast<double>(n)) +
                        phases[k]);
    }
    y[n] = static_cast<SampleFixed>(std::llround(value * scale));
    c[n] = static_cast<SampleFixed>(std::llround(
        150.0 * scale * std::sin(fsc_omega * static_cast<double>(n))));
  }
}

TEST(LineTimingStageTest, AbsentOrZeroBlockLeavesBuffersBitIdentical) {
  for (bool enabled : {false, true}) {
    Project project = MakeLineTimingProject(Standard::kPal);
    project.line_timing.enabled = enabled;
    project.line_timing.capture = Layer(0.0, 0.6, 0.0, 0.0, 3);
    project.line_timing.shared = Layer(0.0, 0.6, 0.0, 0.0, 4);
    project.line_timing.include_burst = true;
    EXPECT_FALSE(LineTimingStage::IsActive(project.line_timing));
    const auto schedule = MakeSchedule(project, 1U);
    const std::size_t samples = FrameSamples(Standard::kPal);
    std::vector<SampleFixed> y(samples);
    std::vector<SampleFixed> c(samples);
    FillStillFrame(Standard::kPal, y.data(), c.data());
    const std::vector<SampleFixed> y_before = y;
    const std::vector<SampleFixed> c_before = c;
    LineTimingStage(1U).ApplyLineTiming(project, schedule, 0U, 1U, &y, &c);
    EXPECT_EQ(y, y_before);
    EXPECT_EQ(c, c_before);
  }
}

TEST(LineTimingStageTest, ShiftsPictureByDrawnErrorAndLeavesTheRestAlone) {
  for (Standard standard : {Standard::kPal, Standard::kNtsc, Standard::kPalM}) {
    for (bool include_burst : {false, true}) {
      Project project = MakeLineTimingProject(standard);
      project.line_injections.vits.push_back(
          VitsInjection{.vits_type = "uk-national", .target_lines = {19}});
      project.line_timing.capture = Layer(20.0, 0.6, 10.0, 15.0, 3);
      project.line_timing.include_burst = include_burst;
      const auto schedule = MakeSchedule(project, 1U);
      const std::size_t samples = FrameSamples(standard);
      std::vector<SampleFixed> y(samples);
      std::vector<SampleFixed> c(samples);
      FillStillFrame(standard, y.data(), c.data());
      const std::vector<SampleFixed> y_before = y;
      const std::vector<SampleFixed> c_before = c;
      const LineTimingStage stage(1U);
      stage.ApplyLineTiming(project, schedule, 0U, 1U, &y, &c);

      const TimingConstants timing = GetTimingConstants(standard);
      const std::vector<int> offsets =
          BuildLineSampleOffsets(BuildLineSampleCounts(
              standard, timing.lines_per_frame, timing.samples_per_line_4fsc));
      const ActiveRasterGeometry geometry =
          GetActiveRasterGeometry(standard, timing.sample_rate_4fsc_hz);
      const double width = geometry.active_window_end_samples -
                           geometry.active_window_start_samples;
      const double centre = geometry.active_window_start_samples + width / 2.0;
      const double samples_per_ns = timing.sample_rate_4fsc_hz * 1e-9;
      const std::vector<LineTimingError> errors =
          stage.FrameErrors(project, schedule, 0U);

      // Inside the picture windows, Y reads the still at n - d(n).
      std::vector<bool> touched_y(samples, false);
      std::vector<bool> touched_c(samples, false);
      const VariableDelayInterpolator interpolator;
      const std::vector<PictureLineWindow> windows =
          BuildPictureLineWindows(project, standard);
      const std::vector<PictureLineWindow> bursts =
          BuildBurstLineWindows(standard);
      double worst = 0.0;
      for (const PictureLineWindow& window : windows) {
        EXPECT_NE(window.line_1based, 19);
        const auto index = static_cast<std::size_t>(window.line_1based - 1);
        const LineTimingError& error = errors[index];
        for (std::size_t n = window.start; n < window.start + window.count;
             ++n) {
          touched_y[n] = true;
          touched_c[n] = true;
          const double relative =
              static_cast<double>(n) - offsets[index] - centre;
          const double delay =
              samples_per_ns *
              (error.offset_ns + error.slope_ns * relative / (width / 2.0));
          const double expected = interpolator.At(
              y_before.data(), samples, static_cast<double>(n) - delay);
          worst =
              std::max(worst, std::abs(static_cast<double>(y[n]) - expected));
        }
        if (include_burst) {
          const PictureLineWindow& burst = bursts[index];
          bool burst_moved = false;
          for (std::size_t n = burst.start; n < burst.start + burst.count;
               ++n) {
            touched_c[n] = true;
            burst_moved = burst_moved || c[n] != c_before[n];
          }
          EXPECT_TRUE(burst_moved) << window.line_1based;
        }
      }
      EXPECT_LE(worst, 0.5) << static_cast<int>(standard);

      // Everything else (sync, burst, VBI, VITS line 19) is bit-identical.
      std::size_t untouched_changes = 0;
      for (std::size_t n = 0; n < samples; ++n) {
        if (!touched_y[n] && y[n] != y_before[n]) {
          ++untouched_changes;
        }
        if (!touched_c[n] && c[n] != c_before[n]) {
          ++untouched_changes;
        }
      }
      EXPECT_EQ(untouched_changes, 0U) << static_cast<int>(standard);
    }
  }
}

TEST(LineTimingStageTest, CaptureLayerFollowsOutputFrameSharedFollowsDisc) {
  Project project = MakeLineTimingProject(Standard::kPal);
  project.line_timing.shared = Layer(4.0, 0.6, 2.0, 2.0, 7001);
  // Output frames 0 and 1 both show disc frame 5; frame 2 shows disc frame 6.
  const auto schedule =
      MakeSchedule(project, std::vector<std::size_t>{5, 5, 6});
  const LineTimingStage stage(1U);
  const auto a = stage.FrameErrors(project, schedule, 0U);
  const auto b = stage.FrameErrors(project, schedule, 1U);
  const auto other = stage.FrameErrors(project, schedule, 2U);
  // Another capture (other run seed, other capture layer) shares the
  // pressing layer.
  const auto replay = LineTimingStage(99U).FrameErrors(project, schedule, 0U);
  bool differs = false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    EXPECT_EQ(a[i].offset_ns, b[i].offset_ns);
    EXPECT_EQ(a[i].offset_ns, replay[i].offset_ns);
    differs = differs || a[i].offset_ns != other[i].offset_ns;
  }
  EXPECT_TRUE(differs);

  // The capture layer differs between output frames of one disc frame, and
  // follows its seed.
  project.line_timing.shared = {};
  project.line_timing.capture = Layer(4.0, 0.6, 2.0, 2.0, 7);
  const auto capture_a = stage.FrameErrors(project, schedule, 0U);
  const auto capture_b = stage.FrameErrors(project, schedule, 1U);
  const auto capture_again =
      LineTimingStage(99U).FrameErrors(project, schedule, 0U);
  differs = false;
  for (std::size_t i = 0; i < capture_a.size(); ++i) {
    EXPECT_EQ(capture_a[i].offset_ns, capture_again[i].offset_ns);
    differs = differs || capture_a[i].offset_ns != capture_b[i].offset_ns;
  }
  EXPECT_TRUE(differs);
}

TEST(LineTimingStageTest, UnseededCaptureLayerUsesRunBaseSeed) {
  Project project = MakeLineTimingProject(Standard::kPal);
  project.line_timing.capture = Layer(4.0, 0.6, 2.0, 2.0, 0);
  project.line_timing.capture.seed_specified = false;
  const auto schedule = MakeSchedule(project, 1U);
  const auto a = LineTimingStage(5U).FrameErrors(project, schedule, 0U);
  const auto b = LineTimingStage(5U).FrameErrors(project, schedule, 0U);
  const auto c = LineTimingStage(6U).FrameErrors(project, schedule, 0U);
  bool differs = false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    EXPECT_EQ(a[i].offset_ns, b[i].offset_ns);
    differs = differs || a[i].offset_ns != c[i].offset_ns;
  }
  EXPECT_TRUE(differs);
}

// Least-squares shift (ns) of frame against reference over [start, end),
// from the reference's five-point slope: frame - reference ~ -shift *
// reference'.
double EstimateShiftNs(const std::vector<double>& frame,
                       const std::vector<double>& reference, std::size_t start,
                       std::size_t end, double samples_per_ns) {
  double numerator = 0.0;
  double denominator = 0.0;
  for (std::size_t n = start; n < end; ++n) {
    const double slope = (reference[n - 2] - (8.0 * reference[n - 1]) +
                          (8.0 * reference[n + 1]) - reference[n + 2]) /
                         12.0;
    numerator += (frame[n] - reference[n]) * slope;
    denominator += slope * slope;
  }
  return -(numerator / denominator) / samples_per_ns;
}

TEST(LineTimingStageTest, HeldStillMeasuresAsConfigured) {
  // The issue's acceptance measurement: 20 frames of one still, each frame's
  // luma compared line by line with the mean of the other frames by a
  // least-squares shift.
  constexpr std::size_t kFrames = 20;
  const Standard standard = Standard::kPal;
  Project project = MakeLineTimingProject(standard);
  project.line_timing.capture = Layer(4.9, 0.6, 3.1, 3.4, 7);
  const auto schedule = MakeSchedule(project, kFrames);
  const std::size_t samples = FrameSamples(standard);
  std::vector<SampleFixed> still_y(samples);
  std::vector<SampleFixed> still_c(samples);
  FillStillFrame(standard, still_y.data(), still_c.data());
  std::vector<SampleFixed> y;
  std::vector<SampleFixed> c;
  for (std::size_t f = 0; f < kFrames; ++f) {
    y.insert(y.end(), still_y.begin(), still_y.end());
    c.insert(c.end(), still_c.begin(), still_c.end());
  }
  LineTimingStage(1U).ApplyLineTiming(project, schedule, 0U, kFrames, &y, &c);

  std::vector<double> sum(samples, 0.0);
  for (std::size_t f = 0; f < kFrames; ++f) {
    for (std::size_t n = 0; n < samples; ++n) {
      sum[n] += static_cast<double>(y[f * samples + n]);
    }
  }
  const TimingConstants timing = GetTimingConstants(standard);
  const double samples_per_ns = timing.sample_rate_4fsc_hz * 1e-9;
  const std::vector<PictureLineWindow> windows =
      BuildPictureLineWindows(project, standard);

  double line_sq = 0.0;
  double field_sq = 0.0;
  double within_sq = 0.0;
  double lag1 = 0.0;
  double slope_sq = 0.0;
  std::size_t lines = 0;
  std::size_t lag_pairs = 0;
  std::size_t fields = 0;
  std::vector<double> frame(samples);
  std::vector<double> reference(samples);
  for (std::size_t f = 0; f < kFrames; ++f) {
    for (std::size_t n = 0; n < samples; ++n) {
      frame[n] = static_cast<double>(y[f * samples + n]);
      reference[n] = (sum[n] - frame[n]) / static_cast<double>(kFrames - 1);
    }
    for (int field = 1; field <= 2; ++field) {
      std::vector<double> shifts;
      for (const PictureLineWindow& window : windows) {
        if (GetFieldIndex(standard, window.line_1based) != field ||
            window.count < 900U) {
          continue;
        }
        const std::size_t start = window.start + 20U;
        const std::size_t end = window.start + window.count - 20U;
        const std::size_t middle = (start + end) / 2U;
        shifts.push_back(
            EstimateShiftNs(frame, reference, start, end, samples_per_ns));
        const double left =
            EstimateShiftNs(frame, reference, start, middle, samples_per_ns);
        const double right =
            EstimateShiftNs(frame, reference, middle, end, samples_per_ns);
        slope_sq += (right - left) * (right - left);
      }
      double mean = 0.0;
      for (double shift : shifts) {
        mean += shift;
        line_sq += shift * shift;
      }
      mean /= static_cast<double>(shifts.size());
      field_sq += mean * mean;
      ++fields;
      for (std::size_t k = 0; k < shifts.size(); ++k) {
        within_sq += (shifts[k] - mean) * (shifts[k] - mean);
        if (k + 1 < shifts.size()) {
          lag1 += (shifts[k] - mean) * (shifts[k + 1] - mean);
          ++lag_pairs;
        }
      }
      lines += shifts.size();
    }
  }
  const double within_variance = within_sq / static_cast<double>(lines);
  const double within_rms = std::sqrt(within_variance);
  const double field_rms = std::sqrt(field_sq / static_cast<double>(fields));
  const double line_rms = std::sqrt(line_sq / static_cast<double>(lines));
  const double slope_rms = std::sqrt(slope_sq / static_cast<double>(lines));
  const double correlation =
      (lag1 / static_cast<double>(lag_pairs)) / within_variance;
  // The leave-one-out reference carries 1/19 of the other frames' jitter.
  const double reference_gain =
      std::sqrt(1.0 + 1.0 / static_cast<double>(kFrames - 1));
  EXPECT_NEAR(within_rms / reference_gain, 4.9, 0.15 * 4.9);
  EXPECT_NEAR(field_rms / reference_gain, 3.1, 0.15 * 3.1);
  EXPECT_NEAR(line_rms / reference_gain, std::hypot(4.9, 3.1),
              0.15 * std::hypot(4.9, 3.1));
  EXPECT_NEAR(slope_rms / reference_gain, 3.4, 0.15 * 3.4);
  EXPECT_NEAR(correlation, 0.6, 0.15 * 0.6);
}

}  // namespace
}  // namespace videosynth
