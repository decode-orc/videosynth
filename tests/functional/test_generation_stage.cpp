/*
 * File:        test_generation_stage.cpp
 * Module:      generation_stage_tests
 * Purpose:     Validates generated sync, burst, and picture timing behavior.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "videosynth/active_sample_mapping.h"
#include "videosynth/chroma_encoder.h"
#include "videosynth/fixed_point.h"
#include "videosynth/generation_stage.h"
#include "videosynth/progressive_frame_source.h"
#include "videosynth/signal_shaping.h"
#include "videosynth/signal_timing_model.h"
#include "videosynth/timing_constants.h"
#include "videosynth/vits_definition_provider.h"
#include "videosynth/vits_generator.h"

namespace videosynth {
namespace {

int BurstStartSamples(double sample_rate_hz) {
  return static_cast<int>(std::lround(sample_rate_hz * 5.6e-6));
}

int BurstEndSamples(double sample_rate_hz) {
  return static_cast<int>(std::lround(sample_rate_hz * 8.0e-6));
}

// Matches kPalSubcarrierAnchorRad in generation_stage.cpp: the 625-line PAL
// subcarrier lattice is rotated 270° so the burst-blanking meander pairs with
// the subcarrier phase of colour fields 1/2 (ITU-R BT.1700 Annex 1 Part B
// Figure 8 with Table 1 item 10f).
constexpr double kPalSubcarrierAnchorRad = 3.0 * 3.14159265358979323846 / 2.0;

std::string DefaultBarsExrPath(Standard standard) {
  return (std::filesystem::path(VIDEOSYNTH_SOURCE_DIR) /
          (standard == Standard::kPal
               ? "videosynth-assets/assets/exr/720x576/100_BARS.exr"
               : "videosynth-assets/assets/exr/720x486/100_BARS.exr"))
      .string();
}

Project MakeProject(Standard standard, int duration_frames = 1) {
  Project project;
  project.cvbs_presets.video_standard_preset = standard;
  project.cvbs_presets.sample_encoding_preset = "CVBS_U10_4FSC";
  project.cvbs_presets.signal_state_preset = "STANDARD_STABLE_LOCKED";
  project.sections.push_back(Section{.name = "SignalTiming",
                                     .type = "progressive",
                                     .line_injections = {},
                                     .source = DefaultBarsExrPath(standard),
                                     .duration_frames = duration_frames});
  return project;
}

Project MakeProgressiveSourceProject(Standard standard,
                                     const std::string& source_path) {
  Project project;
  project.cvbs_presets.video_standard_preset = standard;
  project.cvbs_presets.sample_encoding_preset = "CVBS_U10_4FSC";
  project.cvbs_presets.signal_state_preset = "STANDARD_STABLE_LOCKED";
  project.sections.push_back(Section{.name = "ProgressiveImport",
                                     .type = "progressive",
                                     .line_injections = {},
                                     .source = source_path,
                                     .duration_frames = 1});
  return project;
}

Project MakeProjectWithSectionSpans(Standard standard) {
  Project project;
  project.cvbs_presets.video_standard_preset = standard;
  project.cvbs_presets.sample_encoding_preset = "CVBS_U10_4FSC";
  project.cvbs_presets.signal_state_preset = "STANDARD_STABLE_LOCKED";
  project.sections.push_back(Section{.name = "InjectedSection",
                                     .type = "progressive",
                                     .line_injections = {},
                                     .source = DefaultBarsExrPath(standard),
                                     .duration_frames = 1});
  project.sections.push_back(Section{.name = "PlainSection",
                                     .type = "progressive",
                                     .line_injections = {},
                                     .source = DefaultBarsExrPath(standard),
                                     .duration_frames = 1});
  return project;
}

class FakeVitsDefinitionProvider final : public IVitsDefinitionProvider {
 public:
  bool TryGetDefinition(Standard standard, const std::string& vits_type,
                        VitsDefinition* out_definition,
                        std::string* error) const override {
    if (out_definition != nullptr) {
      out_definition->standard = standard;
      out_definition->vits_type = vits_type;
      out_definition->primitives.push_back(
          VitsPrimitiveDefinition{.id = "placeholder", .continuity_group = ""});
      out_definition->render_order = {"placeholder"};
    }
    if (error != nullptr) {
      error->clear();
    }
    return true;
  }
};

class FakeVitsGenerator final : public IVitsGenerator {
 public:
  bool BuildSynthesisPlan(const VitsDefinition& definition,
                          VitsSynthesisPlan* out_plan,
                          std::string* error) const override {
    if (out_plan == nullptr) {
      if (error != nullptr) {
        *error = "Missing VITS synthesis plan output.";
      }
      return false;
    }

    out_plan->standard = definition.standard;
    out_plan->vits_type = definition.vits_type;
    out_plan->primitives.clear();
    out_plan->render_order = {"fake-render"};
    if (error != nullptr) {
      error->clear();
    }
    return true;
  }

  bool RenderLine(const VitsSynthesisPlan& plan, double sample_rate_hz,
                  int sample_count, VitsRenderedLine* out_line,
                  std::string* error) const override {
    if (out_line == nullptr) {
      if (error != nullptr) {
        *error = "Missing rendered line output.";
      }
      return false;
    }

    out_line->standard = plan.standard;
    out_line->vits_type = plan.vits_type;
    out_line->sample_rate_hz = sample_rate_hz;
    out_line->y_samples_mv.assign(static_cast<std::size_t>(sample_count),
                                  MillivoltsToSampleFixed(0.0));
    out_line->c_samples_mv.assign(static_cast<std::size_t>(sample_count),
                                  MillivoltsToSampleFixed(0.0));

    const int start = static_cast<int>(std::lround(sample_rate_hz * 12.0e-6));
    const int end = std::min(
        sample_count, static_cast<int>(std::lround(sample_rate_hz * 20.0e-6)));
    for (int i = start; i < end; ++i) {
      out_line->y_samples_mv[static_cast<std::size_t>(i)] =
          MillivoltsToSampleFixed(123.0);
      out_line->c_samples_mv[static_cast<std::size_t>(i)] =
          MillivoltsToSampleFixed(45.0);
    }

    if (error != nullptr) {
      error->clear();
    }
    return true;
  }
};

double WindowMeanMillivolts(const std::vector<SampleFixed>& samples, int start,
                            int end) {
  double sum = 0.0;
  int count = 0;
  for (int i = start; i < end; ++i) {
    sum += SampleFixedToMillivolts(samples[static_cast<std::size_t>(i)]);
    ++count;
  }
  return count > 0 ? (sum / static_cast<double>(count)) : 0.0;
}

double LumaMillivoltsFromCodeForTest(int y_code, const SignalLevels& levels) {
  const int clamped = std::max(48, std::min(940, y_code));
  const double y_norm = static_cast<double>(clamped - 64) / 876.0;
  return levels.black_mv + (y_norm * (levels.white_mv - levels.black_mv));
}

int ActiveWindowStartSamples(Standard standard, double sample_rate_hz) {
  if (standard == Standard::kPal) {
    return 177;
  }
  return static_cast<int>(std::lround(sample_rate_hz * 10.5e-6));
}

int ActiveWindowEndSamples(Standard standard, double sample_rate_hz) {
  if (standard == Standard::kPal) {
    return ActiveWindowStartSamples(standard, sample_rate_hz) +
           static_cast<int>(std::lround(sample_rate_hz * 52.0e-6));
  }
  return static_cast<int>(std::lround(sample_rate_hz * 62.5e-6));
}

int CountSyncSamplesOnLine(const std::vector<SampleFixed>& y_mv,
                           int line_1based, const TimingConstants& timing,
                           double sync_tip_mv) {
  const int line_start = (line_1based - 1) * timing.samples_per_line_4fsc;
  const int line_end = line_start + timing.samples_per_line_4fsc;

  int count = 0;
  const SampleFixed sync_tip_fixed = MillivoltsToSampleFixed(sync_tip_mv);
  for (int i = line_start; i < line_end; ++i) {
    if (y_mv[i] == sync_tip_fixed) {
      ++count;
    }
  }
  return count;
}

int CountSyncSamplesInHalfLine(const std::vector<SampleFixed>& y_mv,
                               int line_1based, int half_index,
                               const TimingConstants& timing,
                               double sync_tip_mv) {
  const int half_size = timing.samples_per_line_4fsc / 2;
  const int line_start = (line_1based - 1) * timing.samples_per_line_4fsc;
  const int start = line_start + (half_index * half_size);
  const int end =
      std::min(start + half_size, line_start + timing.samples_per_line_4fsc);

  int count = 0;
  const SampleFixed sync_tip_fixed = MillivoltsToSampleFixed(sync_tip_mv);
  for (int i = start; i < end; ++i) {
    if (y_mv[i] == sync_tip_fixed) {
      ++count;
    }
  }
  return count;
}

int QuantizeCompositeCodeForStandard(double composite_mv, Standard standard) {
  if (standard == Standard::kPal) {
    constexpr double kMillivoltsPerCode = 1.1905;
    constexpr int kBlankingCode = 256;
    constexpr int kMinCode = 4;
    constexpr int kMaxCode = 1019;
    const int mapped =
        static_cast<int>(std::lround(composite_mv / kMillivoltsPerCode)) +
        kBlankingCode;
    return std::max(kMinCode, std::min(kMaxCode, mapped));
  }

  constexpr double kMillivoltsPerCode = 1.2755;
  constexpr int kBlankingCode = 240;
  constexpr int kMinCode = 16;
  constexpr int kMaxCode = 1019;
  const int mapped =
      static_cast<int>(std::lround(composite_mv / kMillivoltsPerCode)) +
      kBlankingCode;
  return std::max(kMinCode, std::min(kMaxCode, mapped));
}

double BurstWindowMean(const std::vector<SampleFixed>& c_mv, int line_1based,
                       const TimingConstants& timing) {
  const int line_start = (line_1based - 1) * timing.samples_per_line_4fsc;
  const int start = line_start + BurstStartSamples(timing.sample_rate_4fsc_hz);
  const int end = line_start + BurstEndSamples(timing.sample_rate_4fsc_hz);

  double sum = 0.0;
  int count = 0;
  for (int i = start; i < end; ++i) {
    sum += SampleFixedToMillivolts(c_mv[i]);
    ++count;
  }
  return count > 0 ? (sum / static_cast<double>(count)) : 0.0;
}

// Estimates PAL burst phase relative to the anchored subcarrier lattice
// (carrier plus kPalSubcarrierAnchorRad), returning the ±135° swinging-burst
// offset.
double EstimateBurstPhaseRad(const std::vector<SampleFixed>& c_mv,
                             int line_1based, const TimingConstants& timing) {
  constexpr double kPi = 3.14159265358979323846;
  const int line_start = (line_1based - 1) * timing.samples_per_line_4fsc;
  const int start = line_start + BurstStartSamples(timing.sample_rate_4fsc_hz);
  const int end = line_start + BurstEndSamples(timing.sample_rate_4fsc_hz);
  const double subcarrier_hz = timing.sample_rate_4fsc_hz / 4.0;

  double sum_sin = 0.0;
  double sum_cos = 0.0;
  for (int i = start; i < end; ++i) {
    const double wt = (2.0 * kPi * subcarrier_hz *
                       (static_cast<double>(i) / timing.sample_rate_4fsc_hz)) +
                      kPalSubcarrierAnchorRad;
    const double c_mv_double =
        SampleFixedToMillivolts(c_mv[static_cast<std::size_t>(i)]);
    sum_sin += c_mv_double * std::sin(wt);
    sum_cos += c_mv_double * std::cos(wt);
  }
  return std::atan2(sum_cos, sum_sin);
}

double WrappedPhaseDeltaAbs(double a_rad, double b_rad) {
  constexpr double kPi = 3.14159265358979323846;
  const double two_pi = 2.0 * kPi;
  double delta = std::fmod((b_rad - a_rad) + kPi, two_pi);
  if (delta < 0.0) {
    delta += two_pi;
  }
  delta -= kPi;
  return std::abs(delta);
}

struct DecodedPalChromaSample {
  double u = 0.0;
  double v_switched = 0.0;
  double burst_phase_rad = 0.0;
};

DecodedPalChromaSample DecodePalChromaWindowBurstLocked(
    const std::vector<SampleFixed>& c_mv, int line_1based, int sample_start,
    int sample_end, const TimingConstants& timing) {
  constexpr double kPi = 3.14159265358979323846;
  constexpr double kCompositeChromaScaleMillivolts = 350.0;

  const double burst_phase_rad =
      EstimateBurstPhaseRad(c_mv, line_1based, timing);
  const double burst_nominal =
      burst_phase_rad >= 0.0 ? (3.0 * kPi / 4.0) : (-3.0 * kPi / 4.0);
  const double phase_correction = burst_phase_rad - burst_nominal;
  const double subcarrier_hz = timing.sample_rate_4fsc_hz / 4.0;

  double sum_u = 0.0;
  double sum_v = 0.0;
  int count = 0;
  for (int sample_index = sample_start; sample_index < sample_end;
       ++sample_index) {
    const double t =
        static_cast<double>(sample_index) / timing.sample_rate_4fsc_hz;
    const double wt = (2.0 * kPi * subcarrier_hz * t) +
                      kPalSubcarrierAnchorRad + phase_correction;
    const double chroma_norm =
        SampleFixedToMillivolts(c_mv[static_cast<std::size_t>(sample_index)]) /
        kCompositeChromaScaleMillivolts;
    sum_u += chroma_norm * std::sin(wt);
    sum_v += chroma_norm * std::cos(wt);
    ++count;
  }

  return DecodedPalChromaSample{
      .u = count > 0 ? (2.0 * sum_u / static_cast<double>(count)) : 0.0,
      .v_switched =
          count > 0 ? (2.0 * sum_v / static_cast<double>(count)) : 0.0,
      .burst_phase_rad = burst_phase_rad,
  };
}

TEST(GenerationStageTimingTest, ProducesDeterministicSampleCounts) {
  GenerationStage generation;
  std::vector<std::string> errors;

  std::vector<SampleFixed> y_pal;
  std::vector<SampleFixed> c_pal;
  ASSERT_TRUE(generation.Generate(MakeProject(Standard::kPal), &y_pal, &c_pal,
                                  &errors));
  EXPECT_EQ(y_pal.size(),
            static_cast<std::size_t>(SamplesPerFrame4fsc(Standard::kPal)));
  EXPECT_EQ(c_pal.size(), y_pal.size());

  std::vector<SampleFixed> y_ntsc;
  std::vector<SampleFixed> c_ntsc;
  ASSERT_TRUE(generation.Generate(MakeProject(Standard::kNtsc), &y_ntsc,
                                  &c_ntsc, &errors));
  const TimingConstants ntsc = GetTimingConstants(Standard::kNtsc);
  EXPECT_EQ(ntsc.samples_per_line_4fsc, 910);
  EXPECT_EQ(y_ntsc.size(),
            static_cast<std::size_t>(SamplesPerFrame4fsc(Standard::kNtsc)));
  EXPECT_EQ(c_ntsc.size(), y_ntsc.size());
}

TEST(GenerationStageTimingTest,
     FixedPointModeMatchesFloatingReferenceAtCodeLevel) {
  GenerationStage generation;
  std::vector<std::string> errors;
  const Project project = MakeProject(Standard::kPal, 1);

  std::vector<SampleFixed> y_float;
  std::vector<SampleFixed> c_float;
  unsetenv("VIDEOSYNTH_GENERATION_FIXED");
  ASSERT_TRUE(generation.Generate(project, &y_float, &c_float, &errors));

  std::vector<SampleFixed> y_fixed;
  std::vector<SampleFixed> c_fixed;
  ASSERT_EQ(setenv("VIDEOSYNTH_GENERATION_FIXED", "1", 1), 0);
  ASSERT_TRUE(generation.Generate(project, &y_fixed, &c_fixed, &errors));
  unsetenv("VIDEOSYNTH_GENERATION_FIXED");

  ASSERT_EQ(y_float.size(), y_fixed.size());
  ASSERT_EQ(c_float.size(), c_fixed.size());

  int max_abs_code_delta = 0;
  double code_error_power = 0.0;
  int float_clipped = 0;
  int fixed_clipped = 0;

  for (std::size_t i = 0; i < y_float.size(); ++i) {
    const int code_float = QuantizeCompositeCodeForStandard(
        static_cast<double>(y_float[i]) + static_cast<double>(c_float[i]),
        Standard::kPal);
    const int code_fixed = QuantizeCompositeCodeForStandard(
        static_cast<double>(y_fixed[i]) + static_cast<double>(c_fixed[i]),
        Standard::kPal);
    const int delta = code_fixed - code_float;

    max_abs_code_delta = std::max(max_abs_code_delta, std::abs(delta));
    code_error_power += static_cast<double>(delta * delta);

    if (code_float == 4 || code_float == 1019) {
      ++float_clipped;
    }
    if (code_fixed == 4 || code_fixed == 1019) {
      ++fixed_clipped;
    }
  }

  const double rms_code_delta =
      std::sqrt(code_error_power / static_cast<double>(y_float.size()));
  EXPECT_LE(max_abs_code_delta, 1);
  EXPECT_LT(rms_code_delta, 0.5);
  EXPECT_EQ(float_clipped, fixed_clipped);
}

TEST(GenerationStageTimingTest, ReportsProgressiveSourceReadError) {
  Project project = MakeProject(Standard::kPal);
  project.sections[0].source = "fixture.exr";
  project.sections[0].duration_frames = 1;

  GenerationStage generation;
  std::vector<std::string> errors;
  std::vector<SampleFixed> y;
  std::vector<SampleFixed> c;

  EXPECT_FALSE(generation.Generate(project, &y, &c, &errors));
  ASSERT_FALSE(errors.empty());
}

TEST(GenerationStageTimingTest, AppliesVitsInjectionOnlyToRequestedLines) {
  Project project = MakeProject(Standard::kPal, 1);
  // VITS injections are now project-wide (Project::line_injections.vits).
  VitsInjection injection;
  injection.vits_type = "fake-vits";
  injection.target_lines = {17, 18};
  project.line_injections.vits.push_back(injection);

  FakeVitsDefinitionProvider provider;
  FakeVitsGenerator fake_generator;
  GenerationStage generation(nullptr, &provider, &fake_generator);
  std::vector<std::string> errors;
  std::vector<SampleFixed> y;
  std::vector<SampleFixed> c;

  ASSERT_TRUE(generation.Generate(project, &y, &c, &errors));
  ASSERT_TRUE(errors.empty());

  const TimingConstants pal = GetTimingConstants(Standard::kPal);
  const int line17 = (17 - 1) * pal.samples_per_line_4fsc;
  const int line18 = (18 - 1) * pal.samples_per_line_4fsc;
  const int line19 = (19 - 1) * pal.samples_per_line_4fsc;
  const int start =
      static_cast<int>(std::lround(pal.sample_rate_4fsc_hz * 12.0e-6));
  const int end =
      static_cast<int>(std::lround(pal.sample_rate_4fsc_hz * 20.0e-6));

  EXPECT_GT(WindowMeanMillivolts(y, line17 + start, line17 + end), 100.0);
  EXPECT_GT(WindowMeanMillivolts(y, line18 + start, line18 + end), 100.0);
  EXPECT_LT(WindowMeanMillivolts(y, line19 + start, line19 + end), 10.0);
}

TEST(GenerationStageTimingTest, AppliesProjectWideVitsToEverySectionFrame) {
  // VITS injections are now a project-wide decision
  // (Project::line_injections.vits) rather than per-section, so the injected
  // VITS line must be rendered on the frames of every section in the project.
  Project project = MakeProjectWithSectionSpans(Standard::kPal);
  VitsInjection injection;
  injection.vits_type = "fake-vits";
  injection.target_lines = {17};
  project.line_injections.vits.push_back(injection);

  FakeVitsDefinitionProvider provider;
  FakeVitsGenerator fake_generator;
  GenerationStage generation(nullptr, &provider, &fake_generator);
  std::vector<std::string> errors;
  std::vector<SampleFixed> y;
  std::vector<SampleFixed> c;

  ASSERT_TRUE(generation.Generate(project, &y, &c, &errors));
  ASSERT_TRUE(errors.empty());

  const int frame_samples = SamplesPerFrame4fsc(Standard::kPal);
  const TimingConstants pal = GetTimingConstants(Standard::kPal);
  const int line17 = (17 - 1) * pal.samples_per_line_4fsc;
  const int start =
      static_cast<int>(std::lround(pal.sample_rate_4fsc_hz * 12.0e-6));
  const int end =
      static_cast<int>(std::lround(pal.sample_rate_4fsc_hz * 20.0e-6));

  // First section's frame carries the project VITS.
  EXPECT_GT(WindowMeanMillivolts(y, line17 + start, line17 + end), 100.0);
  // Second section's frame carries the same project-wide VITS.
  EXPECT_GT(WindowMeanMillivolts(y, frame_samples + line17 + start,
                                 frame_samples + line17 + end),
            100.0);
}

TEST(GenerationStageTimingTest,
     AppliesBuiltInVitsDefinitionThroughDefaultRuntimePath) {
  Project project = MakeProject(Standard::kPal, 1);
  // VITS injections are now project-wide (Project::line_injections.vits).
  VitsInjection injection;
  injection.vits_type = "vits17";
  injection.target_lines = {17};
  project.line_injections.vits.push_back(injection);

  GenerationStage generation;
  std::vector<std::string> errors;
  std::vector<SampleFixed> y;
  std::vector<SampleFixed> c;

  ASSERT_TRUE(generation.Generate(project, &y, &c, &errors));
  ASSERT_TRUE(errors.empty());

  const TimingConstants pal = GetTimingConstants(Standard::kPal);
  const int line17 = (17 - 1) * pal.samples_per_line_4fsc;
  const int line19 = (19 - 1) * pal.samples_per_line_4fsc;
  const int start =
      static_cast<int>(std::lround(pal.sample_rate_4fsc_hz * 12.0e-6));
  const int end =
      static_cast<int>(std::lround(pal.sample_rate_4fsc_hz * 20.0e-6));

  EXPECT_GT(WindowMeanMillivolts(y, line17 + start, line17 + end), 150.0);
  EXPECT_LT(WindowMeanMillivolts(y, line19 + start, line19 + end), 10.0);
}

TEST(GenerationStageTimingTest,
     BuildsDifferentPulseWidthsForEqualizingAndBroadPulses) {
  GenerationStage generation;
  std::vector<std::string> errors;
  std::vector<SampleFixed> y;
  std::vector<SampleFixed> c;
  ASSERT_TRUE(
      generation.Generate(MakeProject(Standard::kNtsc), &y, &c, &errors));

  const TimingConstants ntsc = GetTimingConstants(Standard::kNtsc);
  const SignalLevels levels = GetSignalLevels(Standard::kNtsc);

  const int horizontal_sync =
      CountSyncSamplesOnLine(y, 20, ntsc, levels.sync_tip_mv);
  const int equalizing_sync =
      CountSyncSamplesOnLine(y, 2, ntsc, levels.sync_tip_mv);
  const int broad_sync = CountSyncSamplesOnLine(y, 5, ntsc, levels.sync_tip_mv);

  EXPECT_GT(horizontal_sync, equalizing_sync);
  EXPECT_GT(broad_sync, horizontal_sync);
}

TEST(GenerationStageTimingTest,
     AppliesTwoHalfLinePulsesToEqualizingAndVerticalSyncLines) {
  GenerationStage generation;
  std::vector<std::string> errors;
  std::vector<SampleFixed> y;
  std::vector<SampleFixed> c;
  ASSERT_TRUE(
      generation.Generate(MakeProject(Standard::kPal), &y, &c, &errors));

  const TimingConstants pal = GetTimingConstants(Standard::kPal);
  const SignalLevels levels = GetSignalLevels(Standard::kPal);

  const int eq_first_half =
      CountSyncSamplesInHalfLine(y, 4, 0, pal, levels.sync_tip_mv);
  const int eq_second_half =
      CountSyncSamplesInHalfLine(y, 4, 1, pal, levels.sync_tip_mv);
  EXPECT_GT(eq_first_half, 0);
  EXPECT_GT(eq_second_half, 0);

  const int vs_first_half =
      CountSyncSamplesInHalfLine(y, 1, 0, pal, levels.sync_tip_mv);
  const int vs_second_half =
      CountSyncSamplesInHalfLine(y, 1, 1, pal, levels.sync_tip_mv);
  EXPECT_GT(vs_first_half, 0);
  EXPECT_GT(vs_second_half, 0);

  const int h_second_half =
      CountSyncSamplesInHalfLine(y, 23, 1, pal, levels.sync_tip_mv);
  EXPECT_EQ(h_second_half, 0);
}

TEST(GenerationStageTimingTest, KeepsEndOfFrameNtscLinesAsHorizontalSync) {
  GenerationStage generation;
  std::vector<std::string> errors;
  std::vector<SampleFixed> y;
  std::vector<SampleFixed> c;
  ASSERT_TRUE(
      generation.Generate(MakeProject(Standard::kNtsc), &y, &c, &errors));

  const TimingConstants ntsc = GetTimingConstants(Standard::kNtsc);
  const SignalLevels levels = GetSignalLevels(Standard::kNtsc);

  const int line520 = CountSyncSamplesOnLine(y, 520, ntsc, levels.sync_tip_mv);
  const int line523 = CountSyncSamplesOnLine(y, 523, ntsc, levels.sync_tip_mv);
  const int line525 = CountSyncSamplesOnLine(y, 525, ntsc, levels.sync_tip_mv);

  // Frame-tail lines should remain horizontal-like in this line-granular model;
  // the vertical interval wraps across frame boundaries at sub-line positions.
  EXPECT_NEAR(line523, line520, 2);
  EXPECT_NEAR(line525, line520, 2);
}

TEST(GenerationStageTimingTest,
     IncludesSecondFieldNtscVerticalTransitionBlock) {
  GenerationStage generation;
  std::vector<std::string> errors;
  std::vector<SampleFixed> y;
  std::vector<SampleFixed> c;
  ASSERT_TRUE(
      generation.Generate(MakeProject(Standard::kNtsc), &y, &c, &errors));

  const TimingConstants ntsc = GetTimingConstants(Standard::kNtsc);
  const SignalLevels levels = GetSignalLevels(Standard::kNtsc);

  const int line263 = CountSyncSamplesOnLine(y, 263, ntsc, levels.sync_tip_mv);
  const int line264 = CountSyncSamplesOnLine(y, 264, ntsc, levels.sync_tip_mv);
  const int line522 = CountSyncSamplesOnLine(y, 522, ntsc, levels.sync_tip_mv);
  const int line267 = CountSyncSamplesOnLine(y, 267, ntsc, levels.sync_tip_mv);

  EXPECT_GT(line263, 0);
  EXPECT_GT(line264, 0);
  EXPECT_GT(line267, line264);
  EXPECT_GT(line522, 0);
}

TEST(GenerationStageTimingTest, NtscBroadSyncKeepsIntervalWithinEachHalfLine) {
  GenerationStage generation;
  std::vector<std::string> errors;
  std::vector<SampleFixed> y;
  std::vector<SampleFixed> c;
  ASSERT_TRUE(
      generation.Generate(MakeProject(Standard::kNtsc), &y, &c, &errors));

  const TimingConstants ntsc = GetTimingConstants(Standard::kNtsc);
  const SignalLevels levels = GetSignalLevels(Standard::kNtsc);
  const int half_line_samples = ntsc.samples_per_line_4fsc / 2;

  const int vs_first_half =
      CountSyncSamplesInHalfLine(y, 5, 0, ntsc, levels.sync_tip_mv);
  const int vs_second_half =
      CountSyncSamplesInHalfLine(y, 5, 1, ntsc, levels.sync_tip_mv);

  EXPECT_GT(vs_first_half, 0);
  EXPECT_GT(vs_second_half, 0);
  EXPECT_LT(vs_first_half, half_line_samples);
  EXPECT_LT(vs_second_half, half_line_samples);
}

TEST(GenerationStageTimingTest, PalBroadSyncKeepsIntervalWithinEachHalfLine) {
  GenerationStage generation;
  std::vector<std::string> errors;
  std::vector<SampleFixed> y;
  std::vector<SampleFixed> c;
  ASSERT_TRUE(
      generation.Generate(MakeProject(Standard::kPal), &y, &c, &errors));

  const TimingConstants pal = GetTimingConstants(Standard::kPal);
  const SignalLevels levels = GetSignalLevels(Standard::kPal);
  const int half_line_samples = pal.samples_per_line_4fsc / 2;

  const int vs_first_half =
      CountSyncSamplesInHalfLine(y, 1, 0, pal, levels.sync_tip_mv);
  const int vs_second_half =
      CountSyncSamplesInHalfLine(y, 1, 1, pal, levels.sync_tip_mv);

  EXPECT_GT(vs_first_half, 0);
  EXPECT_GT(vs_second_half, 0);
  EXPECT_LT(vs_first_half, half_line_samples);
  EXPECT_LT(vs_second_half, half_line_samples);
}

TEST(GenerationStageTimingTest, EmitsBurstOnHorizontalButNotBroadSyncLines) {
  GenerationStage generation;
  std::vector<std::string> errors;
  std::vector<SampleFixed> y;
  std::vector<SampleFixed> c;
  ASSERT_TRUE(
      generation.Generate(MakeProject(Standard::kNtsc), &y, &c, &errors));

  const TimingConstants ntsc = GetTimingConstants(Standard::kNtsc);

  const double normal_line_mean = std::abs(BurstWindowMean(c, 20, ntsc));
  const double broad_line_mean = std::abs(BurstWindowMean(c, 5, ntsc));

  EXPECT_GT(normal_line_mean, 0.1);
  EXPECT_LT(broad_line_mean, 1e-9);
}

TEST(GenerationStageTimingTest,
     UsesContinuousSubcarrierBurstPhaseProgressionForNtscAndPal) {
  constexpr double kPi = 3.14159265358979323846;
  GenerationStage generation;
  std::vector<std::string> errors;

  // NTSC: 910 samples/line × π/2 rad/sample = π rad/line, so adjacent lines
  // maintain a strong burst with stable magnitude under continuous subcarrier
  // timing.
  std::vector<SampleFixed> y_ntsc;
  std::vector<SampleFixed> c_ntsc;
  ASSERT_TRUE(generation.Generate(MakeProject(Standard::kNtsc), &y_ntsc,
                                  &c_ntsc, &errors));
  const TimingConstants ntsc = GetTimingConstants(Standard::kNtsc);

  const double ntsc_line20 = BurstWindowMean(c_ntsc, 20, ntsc);
  const double ntsc_line21 = BurstWindowMean(c_ntsc, 21, ntsc);
  EXPECT_GT(std::abs(ntsc_line20), 0.1);
  EXPECT_GT(std::abs(ntsc_line21), 0.1);

  std::vector<SampleFixed> y_pal;
  std::vector<SampleFixed> c_pal;
  ASSERT_TRUE(generation.Generate(MakeProject(Standard::kPal), &y_pal, &c_pal,
                                  &errors));
  const TimingConstants pal = GetTimingConstants(Standard::kPal);

  // ITU-R BT.1700 Annex 1 Part B Table 1 item 10f: PAL burst at ±135° relative
  // to EU axis, alternating per line. For Seq I (frame 0, field 1): odd lines
  // carry +135°, even lines carry -135°. Item 10h: the EV' component of the
  // burst signals V-switching direction to the decoder; this requires ±135°.
  const double pal_odd_phase = EstimateBurstPhaseRad(c_pal, 23, pal);
  const double pal_even_phase = EstimateBurstPhaseRad(c_pal, 24, pal);
  const double pal_odd2_phase = EstimateBurstPhaseRad(c_pal, 25, pal);
  constexpr double k135Deg = 3.0 * kPi / 4.0;
  EXPECT_NEAR(pal_odd_phase, k135Deg, 0.15);
  EXPECT_NEAR(pal_even_phase, -k135Deg, 0.15);
  EXPECT_NEAR(pal_odd2_phase, k135Deg, 0.15);
}

TEST(GenerationStageTimingTest, PalBurstPhaseFollowsFourFrameSequence) {
  // ITU-R BT.1700 Annex 1 Part B Table 1 item 10f: burst polarity on a fixed
  // odd line alternates +135° (Seq I/II) and -135° (Seq III/IV) across frames.
  // The burst total phase (carrier + ±135° offset) groups consecutive frame
  // pairs: frames {0,1} share one total phase and frames {2,3} share another
  // π-apart. These two measures together uniquely identify all 4 frames of the
  // PAL colour sequence, enabling ld-decode to track the 8-field cycle.
  constexpr double kPi = 3.14159265358979323846;
  GenerationStage generation;
  std::vector<std::string> errors;
  std::vector<SampleFixed> y_pal;
  std::vector<SampleFixed> c_pal;
  ASSERT_TRUE(generation.Generate(MakeProject(Standard::kPal, 4), &y_pal,
                                  &c_pal, &errors));

  const TimingConstants pal = GetTimingConstants(Standard::kPal);
  const int frame_samples = SamplesPerFrame4fsc(Standard::kPal);
  const int line_1based = 23;
  const int burst_offset_start = BurstStartSamples(pal.sample_rate_4fsc_hz);
  const int burst_offset_end = BurstEndSamples(pal.sample_rate_4fsc_hz);

  // Burst polarity: correlation against the anchored subcarrier lattice
  // returns ±135°.
  auto burst_polarity = [&](int frame_index) {
    const int frame_base = frame_index * frame_samples;
    const int line_start =
        frame_base + ((line_1based - 1) * pal.samples_per_line_4fsc);
    const int start = line_start + burst_offset_start;
    const int end = line_start + burst_offset_end;
    double sum_sin = 0.0;
    double sum_cos = 0.0;
    for (int i = start; i < end; ++i) {
      const double wt =
          ((kPi / 2.0) * static_cast<double>(i)) + kPalSubcarrierAnchorRad;
      sum_sin += static_cast<double>(c_pal[static_cast<std::size_t>(i)]) *
                 std::sin(wt);
      sum_cos += static_cast<double>(c_pal[static_cast<std::size_t>(i)]) *
                 std::cos(wt);
    }
    return std::atan2(sum_cos, sum_sin);
  };

  // Burst total phase: local-reference correlation returns carrier + offset.
  auto burst_total_phase = [&](int frame_index) {
    const int frame_base = frame_index * frame_samples;
    const int line_start =
        frame_base + ((line_1based - 1) * pal.samples_per_line_4fsc);
    const int start = line_start + burst_offset_start;
    const int end = line_start + burst_offset_end;
    double sum_sin = 0.0;
    double sum_cos = 0.0;
    for (int i = start; i < end; ++i) {
      const double wt_local = (kPi / 2.0) * static_cast<double>(i - start);
      sum_sin += static_cast<double>(c_pal[static_cast<std::size_t>(i)]) *
                 std::sin(wt_local);
      sum_cos += static_cast<double>(c_pal[static_cast<std::size_t>(i)]) *
                 std::cos(wt_local);
    }
    return std::atan2(sum_cos, sum_sin);
  };

  // Odd line 23 burst polarity: +135° for Seq I (frames 0,2), -135° for Seq III
  // (frames 1,3). Each polarity switch signals a V-axis inversion change (item
  // 10h).
  constexpr double k135Deg = 3.0 * kPi / 4.0;
  EXPECT_NEAR(burst_polarity(0), k135Deg, 0.15);
  EXPECT_NEAR(burst_polarity(1), -k135Deg, 0.15);
  EXPECT_NEAR(burst_polarity(2), k135Deg, 0.15);
  EXPECT_NEAR(burst_polarity(3), -k135Deg, 0.15);

  // Burst total phase: frames {0,1} share one total phase, frames {2,3} share
  // a total phase π apart. All four frames are uniquely identifiable.
  const double tp0 = burst_total_phase(0);
  const double tp1 = burst_total_phase(1);
  const double tp2 = burst_total_phase(2);
  const double tp3 = burst_total_phase(3);
  EXPECT_NEAR(WrappedPhaseDeltaAbs(tp0, tp1), 0.0, 0.15);
  EXPECT_NEAR(WrappedPhaseDeltaAbs(tp2, tp3), 0.0, 0.15);
  EXPECT_NEAR(WrappedPhaseDeltaAbs(tp0, tp2), kPi, 0.15);
}

int PalLineSampleOffset(int line_1based);

// Peak chroma magnitude (mV) within the burst window of a line, addressed by
// an absolute line-start sample offset (frame base plus line offset).
double BurstWindowPeakMv(const std::vector<SampleFixed>& c_mv,
                         int line_start_sample, Standard standard,
                         double sample_rate_hz) {
  const int start =
      line_start_sample + BurstStartSamples(standard, sample_rate_hz);
  const int end = line_start_sample + BurstEndSamples(standard, sample_rate_hz);
  double peak = 0.0;
  for (int i = start; i < end; ++i) {
    peak = std::max(
        peak,
        std::abs(SampleFixedToMillivolts(c_mv[static_cast<std::size_t>(i)])));
  }
  return peak;
}

TEST(GenerationStageTimingTest, PalBurstBlankingMeanderAlternatesPerFrame) {
  // ITU-R BT.1700 Annex 1 Part B Figure 8: the burst-blanking windows are
  //   I: lines 623-006, II: 310-318, III: 622-005, IV: 311-319,
  // spanning frame boundaries. The meander edge lines (6, 310, 319, 622) must
  // therefore alternate between consecutive frames; decoders identify the
  // 4-field position of the 8-field sequence from this alternation (ld-decode
  // checks burst presence on field line 6 of each field).
  GenerationStage generation;
  std::vector<std::string> errors;
  std::vector<SampleFixed> y;
  std::vector<SampleFixed> c;
  ASSERT_TRUE(
      generation.Generate(MakeProject(Standard::kPal, 2), &y, &c, &errors));

  const TimingConstants pal = GetTimingConstants(Standard::kPal);
  const int frame_samples = SamplesPerFrame4fsc(Standard::kPal);
  const auto burst_present = [&](int frame_index, int line_1based) {
    const double peak = BurstWindowPeakMv(
        c, frame_index * frame_samples + PalLineSampleOffset(line_1based),
        Standard::kPal, pal.sample_rate_4fsc_hz);
    return peak > 50.0;
  };

  // Frame 0 carries fields I/II: windows 1-6, 310-318, and 622-625 (start of
  // window III, which precedes the next frame's field III).
  EXPECT_FALSE(burst_present(0, 6));
  EXPECT_FALSE(burst_present(0, 310));
  EXPECT_TRUE(burst_present(0, 319));
  EXPECT_FALSE(burst_present(0, 622));

  // Frame 1 carries fields III/IV: windows 1-5, 311-319, and 623-625.
  EXPECT_TRUE(burst_present(1, 6));
  EXPECT_TRUE(burst_present(1, 310));
  EXPECT_FALSE(burst_present(1, 319));
  EXPECT_TRUE(burst_present(1, 622));

  // Lines outside all windows carry burst in both frames; lines covered by
  // overlapping windows carry burst in neither.
  for (int frame_index = 0; frame_index < 2; ++frame_index) {
    EXPECT_TRUE(burst_present(frame_index, 7));
    EXPECT_TRUE(burst_present(frame_index, 320));
    EXPECT_TRUE(burst_present(frame_index, 621));
    EXPECT_FALSE(burst_present(frame_index, 310 + 8));
  }
}

TEST(GenerationStageTimingTest, PalMBurstBlankingMeanderAlternatesPerFrame) {
  // ITU-R BT.1700 Annex 1 Part B Figure 9: the 525-line M/PAL burst-blanking
  // windows are I: lines 523-008, II: 260-270, III: 522-007, IV: 259-269,
  // spanning frame boundaries. Windows II and IV overlap on lines 260-269 and
  // line 270 falls in the System M equalizing block, so the meander edges
  // observable on horizontal lines are 259 and 522; they must alternate
  // between consecutive frames.
  GenerationStage generation;
  std::vector<std::string> errors;
  std::vector<SampleFixed> y;
  std::vector<SampleFixed> c;
  ASSERT_TRUE(
      generation.Generate(MakeProject(Standard::kPalM, 2), &y, &c, &errors));

  const TimingConstants pal_m = GetTimingConstants(Standard::kPalM);
  const int frame_samples = SamplesPerFrame4fsc(Standard::kPalM);
  const auto burst_present = [&](int frame_index, int line_1based) {
    const int line_start = frame_index * frame_samples +
                           (line_1based - 1) * pal_m.samples_per_line_4fsc;
    const double peak = BurstWindowPeakMv(c, line_start, Standard::kPalM,
                                          pal_m.sample_rate_4fsc_hz);
    return peak > 50.0;
  };

  // Frame 0 carries fields I/II: windows 1-8, 260-270, and 522-525 (start of
  // window III, which precedes the next frame's field III).
  EXPECT_TRUE(burst_present(0, 259));
  EXPECT_FALSE(burst_present(0, 522));

  // Frame 1 carries fields III/IV: windows 1-7, 259-269, and 523-525.
  EXPECT_FALSE(burst_present(1, 259));
  EXPECT_TRUE(burst_present(1, 522));

  // Lines 260-263 lie in both windows II and IV (always blanked); lines 258
  // and 521 lie outside all windows (always burst).
  for (int frame_index = 0; frame_index < 2; ++frame_index) {
    EXPECT_FALSE(burst_present(frame_index, 260));
    EXPECT_FALSE(burst_present(frame_index, 263));
    EXPECT_TRUE(burst_present(frame_index, 258));
    EXPECT_TRUE(burst_present(frame_index, 521));
  }
}

TEST(GenerationStageTimingTest, ShapesSyncEdgesInsteadOfHardSteps) {
  GenerationStage generation;
  std::vector<std::string> errors;
  std::vector<SampleFixed> y;
  std::vector<SampleFixed> c;
  ASSERT_TRUE(
      generation.Generate(MakeProject(Standard::kNtsc), &y, &c, &errors));

  const TimingConstants ntsc = GetTimingConstants(Standard::kNtsc);
  const SignalLevels levels = GetSignalLevels(Standard::kNtsc);
  const int line_1based = 20;
  const int line_start = (line_1based - 1) * ntsc.samples_per_line_4fsc;

  ASSERT_LT(line_start + 3, static_cast<int>(y.size()));
  EXPECT_DOUBLE_EQ(y[line_start - 1], levels.blanking_mv);

  // SMPTE 170M-2004 specifies finite sync rise/fall times, so the pulse edge
  // should move through intermediate levels rather than a single hard step.
  const double first_pulse_sample = SampleFixedToMillivolts(y[line_start]);
  const double second_pulse_sample = SampleFixedToMillivolts(y[line_start + 1]);
  const double third_pulse_sample = SampleFixedToMillivolts(y[line_start + 2]);
  EXPECT_DOUBLE_EQ(first_pulse_sample, levels.blanking_mv);
  EXPECT_LT(second_pulse_sample, levels.blanking_mv);
  EXPECT_GT(second_pulse_sample, levels.sync_tip_mv);
  EXPECT_LT(third_pulse_sample, second_pulse_sample);
}

TEST(GenerationStageTimingTest, AppliesBurstEnvelopeRampAtBurstWindowEdges) {
  GenerationStage generation;
  std::vector<std::string> errors;
  std::vector<SampleFixed> y;
  std::vector<SampleFixed> c;
  ASSERT_TRUE(
      generation.Generate(MakeProject(Standard::kNtsc), &y, &c, &errors));

  const TimingConstants ntsc = GetTimingConstants(Standard::kNtsc);
  const int line_1based = 20;
  const int line_start = (line_1based - 1) * ntsc.samples_per_line_4fsc;
  const int burst_start =
      line_start + BurstStartSamples(Standard::kNtsc, ntsc.sample_rate_4fsc_hz);
  const int burst_end =
      line_start + BurstEndSamples(Standard::kNtsc, ntsc.sample_rate_4fsc_hz);
  auto MaxAbsInRange = [&](int start_sample, int end_sample) {
    double max_abs = 0.0;
    for (int i = start_sample; i < end_sample; ++i) {
      max_abs = std::max(max_abs, std::abs(SampleFixedToMillivolts(c[i])));
    }
    return max_abs;
  };

  const int burst_width = burst_end - burst_start;
  ASSERT_GT(burst_width, 6);
  const int center_start = burst_start + (burst_width / 3);
  const int center_end = burst_start + ((2 * burst_width) / 3);

  const double edge_start_max = MaxAbsInRange(burst_start, burst_start + 3);
  const double center_max = MaxAbsInRange(center_start, center_end);
  const double edge_end_max = MaxAbsInRange(burst_end - 3, burst_end);

  EXPECT_DOUBLE_EQ(c[burst_start], 0.0);
  EXPECT_DOUBLE_EQ(c[burst_end - 1], 0.0);

  // SMPTE 170M-2004 Table 2 defines a finite burst envelope rise time.
  EXPECT_LT(edge_start_max, center_max);
  EXPECT_LT(edge_end_max, center_max);
  EXPECT_GT(center_max, 100.0);
}

TEST(GenerationStageTimingTest,
     ShapesBothPositiveAndNegativeBurstLobesAtEdges) {
  GenerationStage generation;
  std::vector<std::string> errors;
  std::vector<SampleFixed> y;
  std::vector<SampleFixed> c;
  ASSERT_TRUE(
      generation.Generate(MakeProject(Standard::kNtsc), &y, &c, &errors));

  const TimingConstants ntsc = GetTimingConstants(Standard::kNtsc);
  const int line_1based = 20;
  const int line_start = (line_1based - 1) * ntsc.samples_per_line_4fsc;
  const int burst_start =
      line_start + BurstStartSamples(Standard::kNtsc, ntsc.sample_rate_4fsc_hz);
  const int burst_end =
      line_start + BurstEndSamples(Standard::kNtsc, ntsc.sample_rate_4fsc_hz);
  const int burst_width = burst_end - burst_start;
  ASSERT_GT(burst_width, 8);

  const int center_start = burst_start + (burst_width / 3);
  const int center_end = burst_start + ((2 * burst_width) / 3);

  auto PeakPositive = [&](int start_sample, int end_sample) {
    double peak = 0.0;
    for (int i = start_sample; i < end_sample; ++i) {
      peak = std::max(peak, SampleFixedToMillivolts(c[i]));
    }
    return peak;
  };

  auto PeakNegativeMagnitude = [&](int start_sample, int end_sample) {
    double peak = 0.0;
    for (int i = start_sample; i < end_sample; ++i) {
      peak = std::max(peak, -SampleFixedToMillivolts(c[i]));
    }
    return peak;
  };

  const double edge_pos = PeakPositive(burst_start, burst_start + 4);
  const double edge_neg = PeakNegativeMagnitude(burst_start, burst_start + 4);
  const double center_pos = PeakPositive(center_start, center_end);
  const double center_neg = PeakNegativeMagnitude(center_start, center_end);

  EXPECT_LT(edge_pos, center_pos);
  EXPECT_LT(edge_neg, center_neg);
  EXPECT_GT(center_pos, 70.0);
  EXPECT_GT(center_neg, 70.0);
}

TEST(GenerationStageChromaTest,
     ActiveChromaUsesNtscBurstPlus180ReferenceModel) {
  GenerationStage generation;
  std::vector<std::string> errors;
  std::vector<SampleFixed> y;
  std::vector<SampleFixed> c;

  const Project project = MakeProject(Standard::kNtsc);
  ASSERT_TRUE(generation.Generate(project, &y, &c, &errors));

  const TimingConstants ntsc = GetTimingConstants(Standard::kNtsc);
  const int line_1based = 60;
  const int line_start = (line_1based - 1) * ntsc.samples_per_line_4fsc;
  const int burst_start =
      line_start + BurstStartSamples(Standard::kNtsc, ntsc.sample_rate_4fsc_hz);
  const int burst_mid = burst_start + 8;
  const double subcarrier_hz = ntsc.sample_rate_4fsc_hz / 4.0;
  const LineTimingPrimitive line =
      BuildLineTimingPrimitive(Standard::kNtsc, line_1based);
  const double burst_t =
      static_cast<double>(burst_mid) / ntsc.sample_rate_4fsc_hz;
  // SMPTE 170M-2004 Table 1: burst amplitude = 40 IRE p-p = 20 IRE peak.
  // 1 IRE = 1000/140 mV → peak burst = 20 × 1000/140 ≈ 142.857 mV.
  constexpr double kNtscBurstPeakMv = 20.0 * 1000.0 / 140.0;
  const double expected_burst =
      kNtscBurstPeakMv *
      std::sin((2.0 * M_PI * subcarrier_hz * burst_t) + line.burst_phase_rad);
  EXPECT_NEAR(SampleFixedToMillivolts(c[burst_mid]), expected_burst, 1e-6);

  const int active_start =
      ActiveWindowStartSamples(Standard::kNtsc, ntsc.sample_rate_4fsc_hz);
  const int active_end =
      ActiveWindowEndSamples(Standard::kNtsc, ntsc.sample_rate_4fsc_hz);
  const int active_window_samples = active_end - active_start;
  ProgressiveFrameSource frame_source;
  std::shared_ptr<const FrameSourceImage> source_frame;
  std::string frame_error;
  ASSERT_TRUE(frame_source.GenerateFrame(
      project.sections[0], 0, Standard::kNtsc, &source_frame, &frame_error));

  std::vector<YCbCr444Pixel> source_line(
      static_cast<std::size_t>(active_window_samples));
  const int active_line_index = line_1based - 22;
  const int source_y = source_frame->active_y + ((2 * active_line_index) + 1);
  for (int x_sample = 0; x_sample < active_window_samples; ++x_sample) {
    const int pixel_x = MapActiveSampleToSourcePixel(
        x_sample, active_window_samples, source_frame->active_width,
        source_frame->active_x);
    source_line[static_cast<std::size_t>(x_sample)] =
        source_frame->PixelAt(pixel_x, source_y);
  }

  const auto chroma_encoder =
      CreateChromaEncoder(Standard::kNtsc, ntsc.sample_rate_4fsc_hz);
  ASSERT_NE(chroma_encoder, nullptr);
  std::vector<SampleFixed> expected_active_line;
  const int line_start_absolute =
      (line_1based - 1) * ntsc.samples_per_line_4fsc;
  const double phase_start =
      (M_PI / 2.0) * static_cast<double>(line_start_absolute + active_start) +
      line.burst_phase_rad + M_PI;
  chroma_encoder->EncodeLineFromPhaseStart(source_line, phase_start,
                                           &expected_active_line);

  const int active_sample = active_window_samples / 3;
  const int generated_sample_index = line_start + active_start + active_sample;
  EXPECT_NEAR(
      SampleFixedToMillivolts(c[generated_sample_index]),
      SampleFixedToMillivolts(
          expected_active_line[static_cast<std::size_t>(active_sample)]),
      1e-3);
}

TEST(GenerationStageChromaTest,
     PalBurstLockedDecodeRecoversStableHueAcrossLines) {
  GenerationStage generation;
  std::vector<std::string> errors;
  std::vector<SampleFixed> y;
  std::vector<SampleFixed> c;

  const Project project = MakeProject(Standard::kPal);
  ASSERT_TRUE(generation.Generate(project, &y, &c, &errors));

  const TimingConstants pal = GetTimingConstants(Standard::kPal);
  ProgressiveFrameSource frame_source;
  std::shared_ptr<const FrameSourceImage> source_frame;
  std::string frame_error;
  ASSERT_TRUE(frame_source.GenerateFrame(project.sections[0], 0, Standard::kPal,
                                         &source_frame, &frame_error));

  // Use a window centered in the second EBU colour bar, away from transitions.
  const int active_start =
      ActiveWindowStartSamples(Standard::kPal, pal.sample_rate_4fsc_hz);
  const int active_window_samples =
      ActiveWindowEndSamples(Standard::kPal, pal.sample_rate_4fsc_hz) -
      active_start;
  const int x_sample = 180;
  const int sample_window = 64;
  const int sample_window_start = x_sample - (sample_window / 2);
  const int sample_window_end = sample_window_start + sample_window;

  const int pixel_x = MapActiveSampleToSourcePixel(
      x_sample, active_window_samples, source_frame->active_width,
      source_frame->active_x);
  const YCbCr444Pixel source_pixel =
      source_frame->PixelAt(pixel_x, source_frame->active_y + 1);
  const double expected_u = static_cast<double>(source_pixel.cb - 512) / 448.0;
  const double expected_v = static_cast<double>(source_pixel.cr - 512) / 448.0;
  const double expected_hue = std::atan2(expected_v, expected_u);

  std::vector<double> decoded_hues;
  decoded_hues.reserve(4);

  constexpr double kPi = 3.14159265358979323846;
  constexpr double kChromaScaleMv = 350.0;

  for (int line_1based = 23; line_1based <= 26; ++line_1based) {
    const int line_start = (line_1based - 1) * pal.samples_per_line_4fsc;
    const int sample_start = line_start + active_start + sample_window_start;
    const int sample_end = line_start + active_start + sample_window_end;

    double sum_u = 0.0;
    double sum_v = 0.0;
    int count = 0;
    for (int i = sample_start; i < sample_end; ++i) {
      const double wt =
          ((kPi / 2.0) * static_cast<double>(i)) + kPalSubcarrierAnchorRad;
      const double cn =
          SampleFixedToMillivolts(c[static_cast<std::size_t>(i)]) /
          kChromaScaleMv;
      sum_u += cn * std::sin(wt);
      sum_v += cn * std::cos(wt);
      ++count;
    }
    const double u =
        count > 0 ? (2.0 * sum_u / static_cast<double>(count)) : 0.0;
    const double v_switched =
        count > 0 ? (2.0 * sum_v / static_cast<double>(count)) : 0.0;

    // Frame 0, burst_seq=0: even lines have V-axis inversion in synthesis.
    const bool v_inverted = (line_1based % 2 == 0);
    const double v = v_inverted ? -v_switched : v_switched;

    const double decoded_hue = std::atan2(v, u);
    const double decoded_magnitude = std::sqrt(u * u + v * v);

    decoded_hues.push_back(decoded_hue);
    EXPECT_GT(decoded_magnitude, 0.08);
    EXPECT_NEAR(WrappedPhaseDeltaAbs(decoded_hue, expected_hue), 0.0, 0.25);
  }

  EXPECT_NEAR(WrappedPhaseDeltaAbs(decoded_hues[0], decoded_hues[1]), 0.0,
              0.15);
  EXPECT_NEAR(WrappedPhaseDeltaAbs(decoded_hues[1], decoded_hues[2]), 0.0,
              0.15);
  EXPECT_NEAR(WrappedPhaseDeltaAbs(decoded_hues[2], decoded_hues[3]), 0.0,
              0.15);
}

TEST(GenerationStageProgressiveTest, NtscMkvUsesField2DominantRowPairing) {
  const std::string source_path =
      (std::filesystem::path(VIDEOSYNTH_SOURCE_DIR) /
       "videosynth-assets/assets/mkv/720x486/MOVING_ZONE_2H.mkv")
          .string();

  ProgressiveFrameSource progressive_source;
  std::shared_ptr<const FrameSourceImage> source_frame;
  std::string source_error;
  Section section;
  section.type = "progressive";
  section.source = source_path;
  ASSERT_TRUE(progressive_source.GenerateFrame(section, 0, Standard::kNtsc,
                                               &source_frame, &source_error));

  const TimingConstants ntsc = GetTimingConstants(Standard::kNtsc);
  const int active_start =
      ActiveWindowStartSamples(Standard::kNtsc, ntsc.sample_rate_4fsc_hz);
  const int active_window_samples =
      ActiveWindowEndSamples(Standard::kNtsc, ntsc.sample_rate_4fsc_hz) -
      active_start;

  const int active_lines_per_field = 240;
  int selected_field_line = -1;
  int selected_x_sample = -1;
  for (int field_line = 0;
       field_line < active_lines_per_field && selected_x_sample < 0;
       ++field_line) {
    for (int x_sample = 0; x_sample < active_window_samples; ++x_sample) {
      const int pixel_x = MapActiveSampleToSourcePixel(
          x_sample, active_window_samples, source_frame->active_width,
          source_frame->active_x);
      if (source_frame
              ->PixelAt(pixel_x, source_frame->active_y + (2 * field_line))
              .y !=
          source_frame
              ->PixelAt(pixel_x, source_frame->active_y + (2 * field_line + 1))
              .y) {
        selected_field_line = field_line;
        selected_x_sample = x_sample;
        break;
      }
    }
  }
  ASSERT_NE(selected_field_line, -1);
  ASSERT_NE(selected_x_sample, -1);

  const int pixel_x = MapActiveSampleToSourcePixel(
      selected_x_sample, active_window_samples, source_frame->active_width,
      source_frame->active_x);

  GenerationStage generation;
  std::vector<std::string> errors;
  std::vector<SampleFixed> y;
  std::vector<SampleFixed> c;
  ASSERT_TRUE(generation.Generate(
      MakeProgressiveSourceProject(Standard::kNtsc, source_path), &y, &c,
      &errors));

  const SignalLevels levels = GetSignalLevels(Standard::kNtsc);
  const int field1_line_start =
      ((22 + selected_field_line) - 1) * ntsc.samples_per_line_4fsc;
  const int field2_line_start =
      ((284 + selected_field_line) - 1) * ntsc.samples_per_line_4fsc;
  const int sample_offset = active_start + selected_x_sample;

  const double expected_field1 = LumaMillivoltsFromCodeForTest(
      source_frame
          ->PixelAt(pixel_x,
                    source_frame->active_y + (2 * selected_field_line + 1))
          .y,
      levels);
  const double expected_field2 = LumaMillivoltsFromCodeForTest(
      source_frame
          ->PixelAt(pixel_x, source_frame->active_y + (2 * selected_field_line))
          .y,
      levels);

  EXPECT_NEAR(
      SampleFixedToMillivolts(
          y[static_cast<std::size_t>(field1_line_start + sample_offset)]),
      expected_field1, 1.0);
  EXPECT_NEAR(
      SampleFixedToMillivolts(
          y[static_cast<std::size_t>(field2_line_start + sample_offset)]),
      expected_field2, 1.0);
}

Project MakePalPilotBurstProject() {
  Project project;
  project.cvbs_presets.video_standard_preset = Standard::kPal;
  project.cvbs_presets.sample_encoding_preset = "CVBS_U10_4FSC";
  project.cvbs_presets.signal_state_preset = "STANDARD_STABLE_LOCKED";
  project.cvbs_presets.pal_laserdisc_pilot_burst = true;
  project.sections.push_back(
      Section{.name = "PilotBurst",
              .type = "progressive",
              .line_injections = {},
              .source = DefaultBarsExrPath(Standard::kPal),
              .duration_frames = 1});
  return project;
}

// Return absolute sample offset for the start of a PAL line (1-based).
// EBU Tech. 3280-E: lines 313 and 625 each carry 2 extra samples.
int PalLineSampleOffset(int line_1based) {
  // Lines 1–312: 1135 samples each.
  if (line_1based <= 312) {
    return (line_1based - 1) * 1135;
  }
  // Line 313: starts right after the 312 × 1135 block.
  if (line_1based == 313) {
    return 312 * 1135;
  }
  // Lines 314–624: after line 313 (1137 samples).
  if (line_1based <= 624) {
    return 312 * 1135 + 1137 + (line_1based - 314) * 1135;
  }
  // Line 625: after lines 1–624.
  return 312 * 1135 + 1137 + 311 * 1135;
}

// Samples within the flat (non-edge) middle region of the horizontal sync
// pulse on a PAL line should oscillate when the pilot burst is active.
TEST(GenerationStageTest, PalLaserdiscPilotBurstSyncSamplesOscillate) {
  const Project project = MakePalPilotBurstProject();
  const TimingConstants timing = GetTimingConstants(Standard::kPal);

  GenerationStage stage;
  std::vector<SampleFixed> y_mv, c_mv;
  std::vector<std::string> errors;
  ASSERT_TRUE(stage.Generate(project, &y_mv, &c_mv, &errors));
  ASSERT_TRUE(errors.empty());

  // Line 10: normal horizontal line well clear of vertical sync.
  const int line_offset = PalLineSampleOffset(10);
  const int pulse_samples =
      static_cast<int>(std::lround(timing.sample_rate_4fsc_hz * 4.7e-6));
  const int ramp_samples =
      RiseTimeToRampSamples(200.0e-9, timing.sample_rate_4fsc_hz);
  const int flat_start = line_offset + ramp_samples;
  const int flat_end = line_offset + pulse_samples - ramp_samples;
  ASSERT_LT(flat_start, flat_end);

  const SampleFixed first = y_mv[static_cast<std::size_t>(flat_start)];
  bool oscillates = false;
  for (int i = flat_start + 1; i < flat_end; ++i) {
    if (y_mv[static_cast<std::size_t>(i)] != first) {
      oscillates = true;
      break;
    }
  }
  EXPECT_TRUE(oscillates)
      << "Pilot burst sync samples must oscillate, not be constant";
}

// The mean value of the flat sync region should be near the PAL sync tip
// (−300 mV) because the pilot burst is centred there.
TEST(GenerationStageTest, PalLaserdiscPilotBurstMeanCentredOnSyncTip) {
  const Project project = MakePalPilotBurstProject();
  const TimingConstants timing = GetTimingConstants(Standard::kPal);
  const SignalLevels levels = GetSignalLevels(Standard::kPal);

  GenerationStage stage;
  std::vector<SampleFixed> y_mv, c_mv;
  std::vector<std::string> errors;
  ASSERT_TRUE(stage.Generate(project, &y_mv, &c_mv, &errors));
  ASSERT_TRUE(errors.empty());

  const int line_offset = PalLineSampleOffset(10);
  const int pulse_samples =
      static_cast<int>(std::lround(timing.sample_rate_4fsc_hz * 4.7e-6));
  const int ramp_samples =
      RiseTimeToRampSamples(200.0e-9, timing.sample_rate_4fsc_hz);
  const int flat_start = line_offset + ramp_samples;
  const int flat_end = line_offset + pulse_samples - ramp_samples;
  ASSERT_LT(flat_start, flat_end);

  double sum = 0.0;
  for (int i = flat_start; i < flat_end; ++i) {
    sum += SampleFixedToMillivolts(y_mv[static_cast<std::size_t>(i)]);
  }
  const double mean_mv = sum / static_cast<double>(flat_end - flat_start);

  // Mean should be within 10 mV of sync tip (−300 mV); a pure sine over
  // multiple complete cycles has zero mean, residual is from the partial
  // cycle at the window boundary (~5–6 mV worst case at 3.75 MHz/PAL 4fsc).
  EXPECT_NEAR(mean_mv, levels.sync_tip_mv, 10.0);
}

// Maximum Y value in the flat sync region should be near 0 mV (blanking),
// and minimum should be near −600 mV (300 mV below sync tip).
TEST(GenerationStageTest, PalLaserdiscPilotBurstAmplitudeMatchesSpec) {
  const Project project = MakePalPilotBurstProject();
  const TimingConstants timing = GetTimingConstants(Standard::kPal);

  GenerationStage stage;
  std::vector<SampleFixed> y_mv, c_mv;
  std::vector<std::string> errors;
  ASSERT_TRUE(stage.Generate(project, &y_mv, &c_mv, &errors));
  ASSERT_TRUE(errors.empty());

  const int line_offset = PalLineSampleOffset(10);
  const int pulse_samples =
      static_cast<int>(std::lround(timing.sample_rate_4fsc_hz * 4.7e-6));
  const int ramp_samples =
      RiseTimeToRampSamples(200.0e-9, timing.sample_rate_4fsc_hz);
  const int flat_start = line_offset + ramp_samples;
  const int flat_end = line_offset + pulse_samples - ramp_samples;
  ASSERT_LT(flat_start, flat_end);

  double max_mv = -1e9;
  double min_mv = 1e9;
  for (int i = flat_start; i < flat_end; ++i) {
    const double v = SampleFixedToMillivolts(y_mv[static_cast<std::size_t>(i)]);
    max_mv = std::max(max_mv, v);
    min_mv = std::min(min_mv, v);
  }

  // Peak ≈ 0 mV (blanking) ±20 mV tolerance for sampling discretisation.
  EXPECT_NEAR(max_mv, 0.0, 20.0);
  // Trough ≈ −600 mV ±20 mV.
  EXPECT_NEAR(min_mv, -600.0, 20.0);
}

// Active-picture samples (luma) must be unaffected by the pilot burst flag.
TEST(GenerationStageTest, PalLaserdiscPilotBurstDoesNotAffectActivePicture) {
  const Project project_burst = MakePalPilotBurstProject();
  const Project project_plain = MakeProject(Standard::kPal);
  const TimingConstants timing = GetTimingConstants(Standard::kPal);

  GenerationStage stage_burst;
  std::vector<SampleFixed> y_burst, c_burst;
  std::vector<std::string> errors_burst;
  ASSERT_TRUE(
      stage_burst.Generate(project_burst, &y_burst, &c_burst, &errors_burst));
  ASSERT_TRUE(errors_burst.empty());

  GenerationStage stage_plain;
  std::vector<SampleFixed> y_plain, c_plain;
  std::vector<std::string> errors_plain;
  ASSERT_TRUE(
      stage_plain.Generate(project_plain, &y_plain, &c_plain, &errors_plain));
  ASSERT_TRUE(errors_plain.empty());

  // Compare active window (177–1085 samples) on a mid-field active line.
  // Line 100 is well within the active picture region for PAL.
  const int line_offset = PalLineSampleOffset(100);
  const int active_start = line_offset + 177;
  const int active_end =
      line_offset + 177 +
      static_cast<int>(std::lround(timing.sample_rate_4fsc_hz * 52.0e-6));

  int differing = 0;
  for (int i = active_start; i < active_end; ++i) {
    if (y_burst[static_cast<std::size_t>(i)] !=
        y_plain[static_cast<std::size_t>(i)]) {
      ++differing;
    }
  }
  EXPECT_EQ(differing, 0)
      << "Pilot burst must not affect active-picture luma samples";
}

// Equalizing pulses (lines 4–5, 311–312, 316–318, 623–625) also receive the
// pilot burst — verify that their flat regions oscillate.
TEST(GenerationStageTest, PalLaserdiscPilotBurstAppliedToEqualizingPulses) {
  const Project project = MakePalPilotBurstProject();
  const TimingConstants timing = GetTimingConstants(Standard::kPal);

  GenerationStage stage;
  std::vector<SampleFixed> y_mv, c_mv;
  std::vector<std::string> errors;
  ASSERT_TRUE(stage.Generate(project, &y_mv, &c_mv, &errors));
  ASSERT_TRUE(errors.empty());

  // Line 4 carries two equalizing pulses. Check the first one (offset 0).
  const int line_offset = PalLineSampleOffset(4);
  const int eq_samples =
      static_cast<int>(std::lround(timing.sample_rate_4fsc_hz * 2.3e-6));
  const int ramp_samples =
      RiseTimeToRampSamples(200.0e-9, timing.sample_rate_4fsc_hz);
  const int flat_start = line_offset + ramp_samples;
  const int flat_end = line_offset + eq_samples - ramp_samples;
  ASSERT_LT(flat_start, flat_end);

  const SampleFixed first = y_mv[static_cast<std::size_t>(flat_start)];
  bool oscillates = false;
  for (int i = flat_start + 1; i < flat_end; ++i) {
    if (y_mv[static_cast<std::size_t>(i)] != first) {
      oscillates = true;
      break;
    }
  }
  EXPECT_TRUE(oscillates)
      << "Pilot burst must also be applied to equalizing pulses";
}

// Broad (vertical) sync pulses (lines 1–3, 314–315) must carry the burst for
// exactly q = 13.5 periods, then remain at constant sync tip (IEC 60856 fig
// 6iii).
TEST(GenerationStageTest, PalLaserdiscPilotBurstAppliedToBroadSyncPulses) {
  const Project project = MakePalPilotBurstProject();
  const TimingConstants timing = GetTimingConstants(Standard::kPal);
  const SignalLevels levels = GetSignalLevels(Standard::kPal);

  GenerationStage stage;
  std::vector<SampleFixed> y_mv, c_mv;
  std::vector<std::string> errors;
  ASSERT_TRUE(stage.Generate(project, &y_mv, &c_mv, &errors));
  ASSERT_TRUE(errors.empty());

  // Line 2 carries two broad sync pulses. Check the first one (offset 0).
  const int line_offset = PalLineSampleOffset(2);
  const int vsync_samples =
      static_cast<int>(std::lround(timing.sample_rate_4fsc_hz * 27.3e-6));
  const int ramp_samples =
      RiseTimeToRampSamples(200.0e-9, timing.sample_rate_4fsc_hz);
  const int flat_start = line_offset + ramp_samples;
  const int flat_end = line_offset + vsync_samples - ramp_samples;
  ASSERT_LT(flat_start, flat_end);

  // IEC 60856 §9.1.2 fig 6iii: q = 13.5 periods of 3.75 MHz.
  const int q_samples =
      static_cast<int>(std::lround(13.5 * timing.sample_rate_4fsc_hz / 3.75e6));
  const int burst_end = flat_start + q_samples;
  ASSERT_LT(burst_end, flat_end)
      << "Broad pulse must be far longer than 13.5 pilot-burst periods";

  // Burst region must oscillate.
  const SampleFixed first = y_mv[static_cast<std::size_t>(flat_start)];
  bool oscillates = false;
  for (int i = flat_start + 1; i < burst_end; ++i) {
    if (y_mv[static_cast<std::size_t>(i)] != first) {
      oscillates = true;
      break;
    }
  }
  EXPECT_TRUE(oscillates)
      << "First 13.5 periods of broad sync pulse must carry the pilot burst";

  // After the burst the pulse must stay at constant sync tip.
  const SampleFixed sync_tip_fixed =
      MillivoltsToSampleFixed(levels.sync_tip_mv);
  bool all_at_sync_tip = true;
  for (int i = burst_end; i < flat_end; ++i) {
    if (y_mv[static_cast<std::size_t>(i)] != sync_tip_fixed) {
      all_at_sync_tip = false;
      break;
    }
  }
  EXPECT_TRUE(all_at_sync_tip)
      << "Broad sync pulse after the 13.5-period burst must remain at sync tip";
}

// No sample in the entire sync pulse (including S-curve transitions) may exceed
// blanking. The positive peak of the burst at sync tip just reaches 0 mV, but
// the burst must be scaled to zero during rise/fall so it cannot overshoot.
TEST(GenerationStageTest, PalLaserdiscPilotBurstNeverExceedsBlanking) {
  const Project project = MakePalPilotBurstProject();
  const TimingConstants timing = GetTimingConstants(Standard::kPal);
  const SignalLevels levels = GetSignalLevels(Standard::kPal);

  GenerationStage stage;
  std::vector<SampleFixed> y_mv, c_mv;
  std::vector<std::string> errors;
  ASSERT_TRUE(stage.Generate(project, &y_mv, &c_mv, &errors));
  ASSERT_TRUE(errors.empty());

  const int line_offset = PalLineSampleOffset(10);
  const int pulse_samples =
      static_cast<int>(std::lround(timing.sample_rate_4fsc_hz * 4.7e-6));
  const SampleFixed blanking_fixed =
      MillivoltsToSampleFixed(levels.blanking_mv);

  double max_mv = -1e9;
  for (int i = line_offset; i < line_offset + pulse_samples; ++i) {
    max_mv = std::max(
        max_mv, SampleFixedToMillivolts(y_mv[static_cast<std::size_t>(i)]));
  }
  // Allow 5 mV margin for fixed-point rounding.
  EXPECT_LE(max_mv, levels.blanking_mv + 5.0)
      << "Pilot burst must not exceed blanking anywhere in the sync pulse";
}

// With the flag disabled (default), flat sync regions must remain constant
// at sync tip — confirming the existing behaviour is not disturbed.
TEST(GenerationStageTest, PalSyncSamplesAreConstantWithoutPilotBurst) {
  const Project project = MakeProject(Standard::kPal);
  const TimingConstants timing = GetTimingConstants(Standard::kPal);
  const SignalLevels levels = GetSignalLevels(Standard::kPal);

  GenerationStage stage;
  std::vector<SampleFixed> y_mv, c_mv;
  std::vector<std::string> errors;
  ASSERT_TRUE(stage.Generate(project, &y_mv, &c_mv, &errors));
  ASSERT_TRUE(errors.empty());

  const int line_offset = PalLineSampleOffset(10);
  const int pulse_samples =
      static_cast<int>(std::lround(timing.sample_rate_4fsc_hz * 4.7e-6));
  const int ramp_samples =
      RiseTimeToRampSamples(200.0e-9, timing.sample_rate_4fsc_hz);
  const int flat_start = line_offset + ramp_samples;
  const int flat_end = line_offset + pulse_samples - ramp_samples;
  ASSERT_LT(flat_start, flat_end);

  const SampleFixed sync_tip_fixed =
      MillivoltsToSampleFixed(levels.sync_tip_mv);
  bool all_at_sync_tip = true;
  for (int i = flat_start; i < flat_end; ++i) {
    if (y_mv[static_cast<std::size_t>(i)] != sync_tip_fixed) {
      all_at_sync_tip = false;
      break;
    }
  }
  EXPECT_TRUE(all_at_sync_tip)
      << "Without pilot burst, flat sync region must be constant at sync tip";
}

// The 625-line PAL colour sequence repeats exactly every four frames: the
// subcarrier lattice advances 709,379 ≡ 3 (mod 4) samples per frame (ITU-R
// BT.1700 Annex 1 Part B Table 1 item 10f gives the burst meander its period of
// two), and the LaserDisc pilot burst is period one because
// 17,734,475 = 25 × 709,379 (IEC 60856 §9.1.2).
//
// Reducing the subcarrier index onto the four-sample lattice, and deriving the
// pilot phase from the offset within the frame, makes that periodicity exact
// for any render length rather than something that drifts as the absolute
// sample index grows — and keeps it correct past the ~3,000-frame point where
// that index used to overflow the int it was held in.
TEST(GenerationStageTimingTest,
     PalColourSequenceRepeatsExactlyEveryFourFramesAtAnyDiscPosition) {
  Project project = MakeProject(Standard::kPal, 1);
  project.cvbs_presets.pal_laserdisc_pilot_burst = true;

  // Picture numbers whose (PN − 1) disc positions are 0, 4, 100,000 and
  // 100,004: two pairs one colour period apart, the second pair far beyond a
  // 32-bit absolute sample index.
  const std::vector<int> picture_numbers = {1, 5, 100001, 100005};
  std::vector<IGenerationStage::FrameScheduleItem> schedule;
  schedule.reserve(picture_numbers.size());
  for (int picture_number : picture_numbers) {
    schedule.push_back(IGenerationStage::FrameScheduleItem{
        .section = &project.sections.front(),
        .source_frame_index = 0,
        .disc_picture_number = picture_number,
    });
  }

  GenerationStage generation;
  std::vector<SampleFixed> y;
  std::vector<SampleFixed> c;
  std::vector<std::string> errors;
  ASSERT_TRUE(generation.GenerateFrameBatch(project, schedule, 0,
                                            schedule.size(), &y, &c, &errors))
      << (errors.empty() ? std::string() : errors.front());

  const auto frame_samples =
      static_cast<std::size_t>(SamplesPerFrame4fsc(Standard::kPal));
  ASSERT_EQ(y.size(), frame_samples * schedule.size());
  ASSERT_EQ(c.size(), frame_samples * schedule.size());

  auto ExpectFramesIdentical = [&](std::size_t first, std::size_t second) {
    std::size_t differing_samples = 0;
    for (std::size_t i = 0; i < frame_samples; ++i) {
      if (y[(first * frame_samples) + i] != y[(second * frame_samples) + i] ||
          c[(first * frame_samples) + i] != c[(second * frame_samples) + i]) {
        ++differing_samples;
      }
    }
    EXPECT_EQ(differing_samples, 0U)
        << "Disc frames " << picture_numbers[first] - 1 << " and "
        << picture_numbers[second] - 1
        << " are a whole colour period apart and must synthesise identically";
  };

  ExpectFramesIdentical(0, 1);
  ExpectFramesIdentical(2, 3);
  ExpectFramesIdentical(0, 2);
}

std::string MovingZoneMkvPath(Standard standard) {
  return (std::filesystem::path(VIDEOSYNTH_SOURCE_DIR) /
          (standard == Standard::kPal
               ? "videosynth-assets/assets/mkv/720x576/MOVING_ZONE_2H.mkv"
               : "videosynth-assets/assets/mkv/720x486/MOVING_ZONE_2H.mkv"))
      .string();
}

// Builds a CAV laserdisc project that exercises every per-frame patch applied
// over the cached clean template: biphase picture numbers, an OSD overlay
// with a per-frame token, a project VITS line, and (for PAL) the pilot burst.
Project MakeTemplatePatchProject(Standard standard, int duration_frames) {
  Project project = MakeProject(standard, duration_frames);
  project.line_injections.disc_type = "CAV";
  if (standard == Standard::kPal) {
    project.line_injections.vits.push_back(VitsInjection{"uk-national", {19}});
    project.cvbs_presets.pal_laserdisc_pilot_burst = true;
  } else {
    project.line_injections.vits.push_back(
        VitsInjection{"ntc7-composite", {17}});
  }

  Section& section = project.sections.front();
  section.section_type = SectionType::kProgrammeArea;

  Section::LineInjection laserdisc;
  laserdisc.type = "laserdisc";
  Section::LineInjectionCode picture_number;
  picture_number.code_type = "picture_number";
  picture_number.start_value = 1;
  picture_number.start_value_specified = true;
  laserdisc.codes.push_back(picture_number);
  section.line_injections.push_back(laserdisc);

  OsdOverlay overlay;
  overlay.text = "PN {picture_number}";
  overlay.x = 16;
  overlay.y = 16;
  overlay.scale = 2;
  section.osd.overlays.push_back(overlay);

  return project;
}

void ExpectCacheTogglesToIdenticalOutput(const Project& project) {
  GenerationStage cached;
  std::vector<SampleFixed> y_cached;
  std::vector<SampleFixed> c_cached;
  std::vector<std::string> errors;
  ASSERT_TRUE(cached.Generate(project, &y_cached, &c_cached, &errors))
      << (errors.empty() ? std::string() : errors.front());

  GenerationStage uncached;
  uncached.SetTemplateCacheCapacityBytes(0);
  std::vector<SampleFixed> y_direct;
  std::vector<SampleFixed> c_direct;
  errors.clear();
  ASSERT_TRUE(uncached.Generate(project, &y_direct, &c_direct, &errors))
      << (errors.empty() ? std::string() : errors.front());

  ASSERT_EQ(y_cached.size(), y_direct.size());
  ASSERT_EQ(c_cached.size(), c_direct.size());
  EXPECT_TRUE(y_cached == y_direct)
      << "Y samples differ between template cache enabled and disabled";
  EXPECT_TRUE(c_cached == c_direct)
      << "C samples differ between template cache enabled and disabled";
}

TEST(GenerationStageTemplateCacheTest,
     PalCavPilotBurstOutputIsByteIdenticalWithCacheDisabled) {
  // Six frames: every colour sequence phase is both built (miss) and reused
  // (hit) with per-frame biphase, OSD, and VITS patches applied on top.
  ExpectCacheTogglesToIdenticalOutput(
      MakeTemplatePatchProject(Standard::kPal, 6));
}

TEST(GenerationStageTemplateCacheTest,
     NtscCavOutputIsByteIdenticalWithCacheDisabled) {
  // Five frames of the two-frame NTSC colour sequence, with the NTSC-only FM
  // code and white flag lines among the per-frame patches.
  ExpectCacheTogglesToIdenticalOutput(
      MakeTemplatePatchProject(Standard::kNtsc, 5));
}

TEST(GenerationStageTemplateCacheTest,
     MkvDurationRepeatOutputIsByteIdenticalWithCacheDisabled) {
  Project project;
  project.cvbs_presets.video_standard_preset = Standard::kPal;
  project.cvbs_presets.sample_encoding_preset = "CVBS_U10_4FSC";
  project.cvbs_presets.signal_state_preset = "STANDARD_STABLE_LOCKED";
  Section section;
  section.name = "MovingRepeat";
  section.type = "progressive";
  section.source = MovingZoneMkvPath(Standard::kPal);
  section.duration_frames_all = true;
  section.duration_frames_repeat = 2;
  project.sections.push_back(section);

  GenerationStage cached;
  std::vector<IGenerationStage::FrameScheduleItem> cached_schedule;
  std::vector<std::string> errors;
  ASSERT_TRUE(cached.BuildFrameSchedule(project, &cached_schedule, &errors))
      << (errors.empty() ? std::string() : errors.front());

  GenerationStage uncached;
  uncached.SetTemplateCacheCapacityBytes(0);
  std::vector<IGenerationStage::FrameScheduleItem> direct_schedule;
  errors.clear();
  ASSERT_TRUE(uncached.BuildFrameSchedule(project, &direct_schedule, &errors))
      << (errors.empty() ? std::string() : errors.front());

  ASSERT_EQ(cached_schedule.size(), direct_schedule.size());
  ASSERT_EQ(cached_schedule.size() % 2U, 0U);
  const std::size_t pass_length = cached_schedule.size() / 2U;
  ASSERT_GE(pass_length, 4U);

  // The start of each duration_repeat pass, then the first range again: the
  // repeated visit is served from templates built on the first visit, and the
  // second pass replays the same source frames at different disc positions.
  const std::vector<std::size_t> batch_starts = {0U, pass_length, 0U};
  for (const std::size_t start : batch_starts) {
    std::vector<SampleFixed> y_cached;
    std::vector<SampleFixed> c_cached;
    errors.clear();
    ASSERT_TRUE(cached.GenerateFrameBatch(project, cached_schedule, start, 4U,
                                          &y_cached, &c_cached, &errors))
        << (errors.empty() ? std::string() : errors.front());

    std::vector<SampleFixed> y_direct;
    std::vector<SampleFixed> c_direct;
    errors.clear();
    ASSERT_TRUE(uncached.GenerateFrameBatch(project, direct_schedule, start, 4U,
                                            &y_direct, &c_direct, &errors))
        << (errors.empty() ? std::string() : errors.front());

    EXPECT_TRUE(y_cached == y_direct)
        << "Y samples differ at batch start " << start;
    EXPECT_TRUE(c_cached == c_direct)
        << "C samples differ at batch start " << start;
  }
}

}  // namespace
}  // namespace videosynth
