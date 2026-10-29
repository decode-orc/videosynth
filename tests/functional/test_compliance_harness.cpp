/*
 * File:        test_compliance_harness.cpp
 * Module:      compliance_harness_tests
 * Purpose:     Provides tolerance-driven compliance checks for waveform and
 * code-space behavior.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <vector>

#include "videosynth/chroma_encoder.h"
#include "videosynth/fixed_point.h"
#include "videosynth/generation_stage.h"
#include "videosynth/signal_shaping.h"
#include "videosynth/signal_timing_model.h"
#include "videosynth/timing_constants.h"

namespace videosynth {
namespace {

constexpr double kPi = 3.14159265358979323846;

// Matches kPalSubcarrierAnchorRad in generation_stage.cpp: the 625-line PAL
// subcarrier lattice is rotated 270° so the burst-blanking meander pairs with
// the subcarrier phase of colour fields 1/2 (ITU-R BT.1700 Annex 1 Part B
// Figure 8 with Table 1 item 10f).
constexpr double kPalSubcarrierAnchorRad = 3.0 * kPi / 2.0;

Project MakeProject(Standard standard, const std::string& source = "") {
  const std::string selected_source =
      source.empty()
          ? ((std::filesystem::path(VIDEOSYNTH_SOURCE_DIR) /
              (standard == Standard::kPal
                   ? "videosynth-assets/assets/exr/720x576/100_BARS.exr"
                   : "videosynth-assets/assets/exr/720x486/100_BARS.exr"))
                 .string())
          : source;

  Project project;
  project.cvbs_presets.video_standard_preset = standard;
  project.cvbs_presets.sample_encoding_preset = "CVBS_U10_4FSC";
  project.cvbs_presets.signal_state_preset = "STANDARD_STABLE_LOCKED";
  project.sections.push_back(Section{.name = "Compliance",
                                     .type = "progressive",
                                     .line_injections = {},
                                     .source = selected_source,
                                     .duration_frames = 1});
  return project;
}

int BurstStartSamples(double sample_rate_hz) {
  return static_cast<int>(std::lround(sample_rate_hz * 5.6e-6));
}

int BurstEndSamples(double sample_rate_hz) {
  return static_cast<int>(std::lround(sample_rate_hz * 8.0e-6));
}

int PulseWidthSamples(SyncPulseKind kind, Standard standard,
                      double sample_rate_hz) {
  double pulse_width_seconds = 4.7e-6;
  if (kind == SyncPulseKind::kEqualizing) {
    pulse_width_seconds = 2.3e-6;
  } else if (kind == SyncPulseKind::kVerticalSync) {
    pulse_width_seconds = (standard == Standard::kNtsc) ? 27.1e-6 : 27.3e-6;
  }
  return std::max(
      1, static_cast<int>(std::lround(sample_rate_hz * pulse_width_seconds)));
}

int MeasureLeadingPulseWidth(const std::vector<SampleFixed>& y_mv,
                             int line_1based, const TimingConstants& timing,
                             double threshold_mv) {
  const int line_start = (line_1based - 1) * timing.samples_per_line_4fsc;
  const int line_end = line_start + timing.samples_per_line_4fsc;

  int first_at_or_below = -1;
  for (int i = line_start; i < line_end; ++i) {
    if (SampleFixedToMillivolts(y_mv[static_cast<std::size_t>(i)]) <=
        threshold_mv) {
      first_at_or_below = i;
      break;
    }
  }
  if (first_at_or_below < 0) {
    return 0;
  }

  int width = 0;
  for (int i = first_at_or_below; i < line_end; ++i) {
    if (SampleFixedToMillivolts(y_mv[static_cast<std::size_t>(i)]) <=
        threshold_mv) {
      ++width;
      continue;
    }
    break;
  }
  return width;
}

int ExpectedThresholdPulseWidth(SyncPulseKind kind, Standard standard,
                                const TimingConstants& timing,
                                double threshold_mv,
                                const SignalLevels& levels) {
  const int pulse_width =
      PulseWidthSamples(kind, standard, timing.sample_rate_4fsc_hz);
  const double rise_time_seconds =
      (standard == Standard::kNtsc) ? 140.0e-9 : 200.0e-9;
  const int ramp_samples =
      RiseTimeToRampSamples(rise_time_seconds, timing.sample_rate_4fsc_hz);

  int count = 0;
  for (int i = 0; i < pulse_width; ++i) {
    const double shaped_level = ShapedPulseLevel(
        i, pulse_width, ramp_samples, levels.blanking_mv, levels.sync_tip_mv);
    if (shaped_level <= threshold_mv) {
      ++count;
    }
  }
  return count;
}

int FindFirstCrossingIndex(const std::vector<SampleFixed>& y_mv, int start,
                           int end, double threshold_mv) {
  for (int i = start; i < end; ++i) {
    if (SampleFixedToMillivolts(y_mv[static_cast<std::size_t>(i)]) <=
        threshold_mv) {
      return i;
    }
  }
  return -1;
}

// Estimates burst phase relative to the subcarrier lattice rotated by
// |reference_anchor_rad| (kPalSubcarrierAnchorRad for 625-line PAL, 0 for
// NTSC).
double EstimateBurstPhaseRad(const std::vector<SampleFixed>& c_mv,
                             int line_1based, const TimingConstants& timing,
                             double reference_anchor_rad = 0.0) {
  const int line_start = (line_1based - 1) * timing.samples_per_line_4fsc;
  const int start = line_start + BurstStartSamples(timing.sample_rate_4fsc_hz);
  const int end = line_start + BurstEndSamples(timing.sample_rate_4fsc_hz);
  const double subcarrier_hz = timing.sample_rate_4fsc_hz / 4.0;

  double sum_sin = 0.0;
  double sum_cos = 0.0;
  for (int i = start; i < end; ++i) {
    const double wt = ((2.0 * kPi * subcarrier_hz * static_cast<double>(i)) /
                       timing.sample_rate_4fsc_hz) +
                      reference_anchor_rad;
    const double c_mv_double =
        SampleFixedToMillivolts(c_mv[static_cast<std::size_t>(i)]);
    sum_sin += c_mv_double * std::sin(wt);
    sum_cos += c_mv_double * std::cos(wt);
  }

  return std::atan2(sum_cos, sum_sin);
}

double WrappedPhaseDelta(double a_rad, double b_rad) {
  const double two_pi = 2.0 * kPi;
  double delta = std::fmod((b_rad - a_rad) + kPi, two_pi);
  if (delta < 0.0) {
    delta += two_pi;
  }
  return delta - kPi;
}

double WrappedPhaseDeltaAbs(double a_rad, double b_rad) {
  return std::abs(WrappedPhaseDelta(a_rad, b_rad));
}

int MapCompositeToCode(Standard standard, double composite_mv) {
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

double RootMeanSquare(const std::vector<SampleFixed>& values) {
  double square_sum = 0.0;
  for (SampleFixed value : values) {
    const double value_mv = SampleFixedToMillivolts(value);
    square_sum += value_mv * value_mv;
  }
  return values.empty()
             ? 0.0
             : std::sqrt(square_sum / static_cast<double>(values.size()));
}

std::vector<YCbCr444Pixel> MakeCbSinusoidLine(std::size_t sample_count,
                                              double amplitude_norm,
                                              double cycles_per_sample) {
  std::vector<YCbCr444Pixel> line(
      sample_count, YCbCr444Pixel{.y = 512, .cb = 512, .cr = 512});
  for (std::size_t index = 0; index < sample_count; ++index) {
    const double angle =
        2.0 * kPi * cycles_per_sample * static_cast<double>(index);
    const double cb_norm = amplitude_norm * std::sin(angle);
    line[index].cb =
        static_cast<std::int16_t>(std::lround(512.0 + (448.0 * cb_norm)));
  }
  return line;
}

TEST(ComplianceHarnessTest, CodeSpaceAnchorsMatchPalAndNtscProfiles) {
  const SignalLevels pal = GetSignalLevels(Standard::kPal);
  EXPECT_EQ(MapCompositeToCode(Standard::kPal, pal.sync_tip_mv), 4);
  EXPECT_EQ(MapCompositeToCode(Standard::kPal, pal.blanking_mv), 256);
  EXPECT_EQ(MapCompositeToCode(Standard::kPal, pal.black_mv), 256);
  EXPECT_EQ(MapCompositeToCode(Standard::kPal, pal.white_mv), 844);
  EXPECT_GE(MapCompositeToCode(Standard::kPal, -5000.0), 4);
  EXPECT_LE(MapCompositeToCode(Standard::kPal, 5000.0), 1019);

  const SignalLevels ntsc = GetSignalLevels(Standard::kNtsc);
  EXPECT_EQ(MapCompositeToCode(Standard::kNtsc, ntsc.sync_tip_mv), 16);
  EXPECT_EQ(MapCompositeToCode(Standard::kNtsc, ntsc.blanking_mv), 240);
  EXPECT_EQ(MapCompositeToCode(Standard::kNtsc, ntsc.black_mv), 282);
  EXPECT_EQ(MapCompositeToCode(Standard::kNtsc, ntsc.white_mv), 800);
  EXPECT_GE(MapCompositeToCode(Standard::kNtsc, -5000.0), 16);
  EXPECT_LE(MapCompositeToCode(Standard::kNtsc, 5000.0), 1019);
}

TEST(ComplianceHarnessTest, NtscPulseAndEdgeTimingRemainWithinTolerance) {
  GenerationStage generation;
  std::vector<std::string> errors;
  std::vector<SampleFixed> y_mv;
  std::vector<SampleFixed> c_mv;
  ASSERT_TRUE(
      generation.Generate(MakeProject(Standard::kNtsc), &y_mv, &c_mv, &errors));

  const TimingConstants ntsc = GetTimingConstants(Standard::kNtsc);
  const SignalLevels levels = GetSignalLevels(Standard::kNtsc);
  const double pulse_threshold =
      levels.blanking_mv + 0.5 * (levels.sync_tip_mv - levels.blanking_mv);

  const int hsync_width =
      MeasureLeadingPulseWidth(y_mv, 20, ntsc, pulse_threshold);
  const int eq_width = MeasureLeadingPulseWidth(y_mv, 2, ntsc, pulse_threshold);
  const int vsync_width =
      MeasureLeadingPulseWidth(y_mv, 5, ntsc, pulse_threshold);

  EXPECT_NEAR(
      hsync_width,
      ExpectedThresholdPulseWidth(SyncPulseKind::kHorizontal, Standard::kNtsc,
                                  ntsc, pulse_threshold, levels),
      1);
  EXPECT_NEAR(
      eq_width,
      ExpectedThresholdPulseWidth(SyncPulseKind::kEqualizing, Standard::kNtsc,
                                  ntsc, pulse_threshold, levels),
      1);
  EXPECT_NEAR(
      vsync_width,
      ExpectedThresholdPulseWidth(SyncPulseKind::kVerticalSync, Standard::kNtsc,
                                  ntsc, pulse_threshold, levels),
      1);

  const int line_start = (20 - 1) * ntsc.samples_per_line_4fsc;
  const int burst_start =
      line_start + BurstStartSamples(Standard::kNtsc, ntsc.sample_rate_4fsc_hz);
  const int burst_end =
      line_start + BurstEndSamples(Standard::kNtsc, ntsc.sample_rate_4fsc_hz);
  EXPECT_EQ(c_mv[static_cast<std::size_t>(burst_start)], 0);
  EXPECT_EQ(c_mv[static_cast<std::size_t>(burst_end - 1)], 0);
  EXPECT_GT(std::abs(SampleFixedToMillivolts(
                c_mv[static_cast<std::size_t>((burst_start + burst_end) / 2)])),
            100.0);

  const double level_10 =
      levels.blanking_mv + (levels.sync_tip_mv - levels.blanking_mv) * 0.1;
  const double level_90 =
      levels.blanking_mv + (levels.sync_tip_mv - levels.blanking_mv) * 0.9;
  const int cross_10 =
      FindFirstCrossingIndex(y_mv, line_start, line_start + 20, level_10);
  const int cross_90 =
      FindFirstCrossingIndex(y_mv, line_start, line_start + 20, level_90);
  ASSERT_GE(cross_10, 0);
  ASSERT_GE(cross_90, 0);

  const int measured_10_90_samples = cross_90 - cross_10;
  const int expected_10_90_samples = std::max(
      1, static_cast<int>(std::lround(140.0e-9 * ntsc.sample_rate_4fsc_hz)));
  EXPECT_NEAR(measured_10_90_samples, expected_10_90_samples, 1);
}

TEST(ComplianceHarnessTest, PalPulseAndEdgeTimingRemainWithinTolerance) {
  GenerationStage generation;
  std::vector<std::string> errors;
  std::vector<SampleFixed> y_mv;
  std::vector<SampleFixed> c_mv;
  ASSERT_TRUE(
      generation.Generate(MakeProject(Standard::kPal), &y_mv, &c_mv, &errors));

  const TimingConstants pal = GetTimingConstants(Standard::kPal);
  const SignalLevels levels = GetSignalLevels(Standard::kPal);
  const double pulse_threshold =
      levels.blanking_mv + 0.5 * (levels.sync_tip_mv - levels.blanking_mv);

  const int hsync_width =
      MeasureLeadingPulseWidth(y_mv, 23, pal, pulse_threshold);
  const int eq_width = MeasureLeadingPulseWidth(y_mv, 4, pal, pulse_threshold);
  const int vsync_width =
      MeasureLeadingPulseWidth(y_mv, 1, pal, pulse_threshold);

  EXPECT_NEAR(
      hsync_width,
      ExpectedThresholdPulseWidth(SyncPulseKind::kHorizontal, Standard::kPal,
                                  pal, pulse_threshold, levels),
      1);
  EXPECT_NEAR(
      eq_width,
      ExpectedThresholdPulseWidth(SyncPulseKind::kEqualizing, Standard::kPal,
                                  pal, pulse_threshold, levels),
      1);
  EXPECT_NEAR(
      vsync_width,
      ExpectedThresholdPulseWidth(SyncPulseKind::kVerticalSync, Standard::kPal,
                                  pal, pulse_threshold, levels),
      1);

  const int line_start = (23 - 1) * pal.samples_per_line_4fsc;
  const int burst_start =
      line_start + BurstStartSamples(pal.sample_rate_4fsc_hz);
  const int burst_end = line_start + BurstEndSamples(pal.sample_rate_4fsc_hz);
  EXPECT_EQ(c_mv[static_cast<std::size_t>(burst_start)], 0);
  EXPECT_EQ(c_mv[static_cast<std::size_t>(burst_end - 1)], 0);
  double burst_peak_mv = 0.0;
  for (int bi = burst_start + 1; bi < burst_end - 1; ++bi) {
    burst_peak_mv = std::max(
        burst_peak_mv,
        std::abs(SampleFixedToMillivolts(c_mv[static_cast<std::size_t>(bi)])));
  }
  EXPECT_GT(burst_peak_mv, 100.0);

  const double level_10 =
      levels.blanking_mv + (levels.sync_tip_mv - levels.blanking_mv) * 0.1;
  const double level_90 =
      levels.blanking_mv + (levels.sync_tip_mv - levels.blanking_mv) * 0.9;
  const int cross_10 =
      FindFirstCrossingIndex(y_mv, line_start, line_start + 20, level_10);
  const int cross_90 =
      FindFirstCrossingIndex(y_mv, line_start, line_start + 20, level_90);
  ASSERT_GE(cross_10, 0);
  ASSERT_GE(cross_90, 0);

  const int measured_10_90_samples = cross_90 - cross_10;
  const int expected_10_90_samples = std::max(
      1, static_cast<int>(std::lround(200.0e-9 * pal.sample_rate_4fsc_hz)));
  EXPECT_NEAR(measured_10_90_samples, expected_10_90_samples, 1);
}

TEST(ComplianceHarnessTest,
     NtscScHProgressionAssumptionKeepsStableBurstReference) {
  GenerationStage generation;
  std::vector<std::string> errors;
  std::vector<SampleFixed> y_mv;
  std::vector<SampleFixed> c_mv;
  ASSERT_TRUE(
      generation.Generate(MakeProject(Standard::kNtsc), &y_mv, &c_mv, &errors));

  const TimingConstants ntsc = GetTimingConstants(Standard::kNtsc);
  const double phase_20 = EstimateBurstPhaseRad(c_mv, 20, ntsc);
  const double phase_21 = EstimateBurstPhaseRad(c_mv, 21, ntsc);
  const double phase_22 = EstimateBurstPhaseRad(c_mv, 22, ntsc);

  EXPECT_NEAR(std::abs(WrappedPhaseDelta(phase_20, phase_21)), 0.0, 0.05);
  EXPECT_NEAR(std::abs(WrappedPhaseDelta(phase_21, phase_22)), 0.0, 0.05);
}

TEST(ComplianceHarnessTest, PalBurstPolarityAlternatesPerLineInIecSequence) {
  // ITU-R BT.1700 Annex 1 Part B Table 1 item 10f: PAL burst at ±135° from
  // EU axis, alternating per line. Seq I (frame 0, field 1): odd lines carry
  // +135°, even lines carry -135°. Item 10h: EV' component of the burst signals
  // V-switching direction to the decoder; this requires the ±135° encoding.
  GenerationStage generation;
  std::vector<std::string> errors;
  std::vector<SampleFixed> y_mv;
  std::vector<SampleFixed> c_mv;
  ASSERT_TRUE(
      generation.Generate(MakeProject(Standard::kPal), &y_mv, &c_mv, &errors));

  const TimingConstants pal = GetTimingConstants(Standard::kPal);
  const double phase_23 =
      EstimateBurstPhaseRad(c_mv, 23, pal, kPalSubcarrierAnchorRad);
  const double phase_24 =
      EstimateBurstPhaseRad(c_mv, 24, pal, kPalSubcarrierAnchorRad);
  const double phase_25 =
      EstimateBurstPhaseRad(c_mv, 25, pal, kPalSubcarrierAnchorRad);

  constexpr double k135Deg = 3.0 * kPi / 4.0;
  EXPECT_NEAR(phase_23, k135Deg, 0.1);
  EXPECT_NEAR(phase_24, -k135Deg, 0.1);
  EXPECT_NEAR(phase_25, k135Deg, 0.1);
}

TEST(ComplianceHarnessTest,
     ChromaFilterAttenuatesHighFrequencyRelativeToLowFrequency) {
  const TimingConstants pal_timing = GetTimingConstants(Standard::kPal);
  const auto pal_encoder =
      CreateChromaEncoder(Standard::kPal, pal_timing.sample_rate_4fsc_hz);
  ASSERT_NE(pal_encoder, nullptr);

  std::vector<SampleFixed> low_output;
  std::vector<SampleFixed> high_output;

  const std::vector<YCbCr444Pixel> low_line =
      MakeCbSinusoidLine(512, 0.35, 0.02);
  const std::vector<YCbCr444Pixel> high_line =
      MakeCbSinusoidLine(512, 0.35, 0.20);
  pal_encoder->EncodeLineFromPhaseStart(low_line, kPi / 2.0, &low_output);
  pal_encoder->EncodeLineFromPhaseStart(high_line, kPi / 2.0, &high_output);

  const double low_rms = RootMeanSquare(low_output);
  const double high_rms = RootMeanSquare(high_output);
  ASSERT_GT(high_rms, 0.0);

  const double attenuation_db = 20.0 * std::log10(low_rms / high_rms);
  EXPECT_GT(attenuation_db, 8.0);
}

}  // namespace
}  // namespace videosynth