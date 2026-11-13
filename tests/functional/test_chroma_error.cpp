/*
 * File:        test_chroma_error.cpp
 * Module:      chroma_error_tests
 * Purpose:     End-to-end check of the chroma error on generated colour bars:
 *              demodulated against the generated burst, every bar's hue
 *              shifts by phase_deg and its saturation scales by gain on PAL,
 *              NTSC and PAL-M; the subcarrier reference agrees with the
 *              generated burst; crosstalk mixes the decoded U and V; and
 *              differential gain and phase follow each bar's luma; and the
 *              picture and burst windows follow the PAL 4fsc lattice.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "sync_edge_measurement.h"
#include "videosynth/chroma_error_stage.h"
#include "videosynth/colour_sequence.h"
#include "videosynth/fixed_point.h"
#include "videosynth/frame_line_layout.h"
#include "videosynth/generation_stage.h"
#include "videosynth/picture_chroma.h"
#include "videosynth/signal_timing_model.h"
#include "videosynth/timing_constants.h"

namespace videosynth {
namespace {

constexpr double kPi = 3.14159265358979323846;

Project MakeBarsProject(Standard standard) {
  const std::string raster =
      Is625LineStandard(standard) ? "720x576" : "720x486";
  Project project;
  project.cvbs_presets.video_standard_preset = standard;
  project.cvbs_presets.sample_encoding_preset = "CVBS_U10_4FSC";
  project.cvbs_presets.signal_state_preset = "STANDARD_STABLE_LOCKED";
  project.sections.push_back(Section{
      .name = "Bars",
      .type = "progressive",
      .line_injections = {},
      .source = (std::filesystem::path(VIDEOSYNTH_SOURCE_DIR) /
                 "videosynth-assets/assets/exr" / raster / "75_BARS.exr")
                    .string(),
      .duration_frames = 1});
  return project;
}

// Burst gate as the generation stage places it (ITU-R BT.470-6 Table 2 item
// 2.14g/h), trimmed clear of the envelope edges.
void BurstRange(Standard standard, double fs, int* start, int* end) {
  double start_s = 5.6e-6;
  double end_s = 8.0e-6;
  if (standard == Standard::kNtsc) {
    start_s = 5.3e-6;
    end_s = 7.97e-6;
  } else if (standard == Standard::kPalM) {
    start_s = 5.8e-6;
    end_s = 8.32e-6;
  }
  *start = static_cast<int>(std::lround(fs * start_s)) + 6;
  *end = static_cast<int>(std::lround(fs * end_s)) - 6;
  *end -= (*end - *start) % 4;
}

// Subcarrier phasor of samples [start, end) of the frame, against the
// frame-wide fs/4 carrier.
std::complex<double> Phasor(const std::vector<SampleFixed>& c, int start,
                            int end) {
  std::complex<double> sum = 0.0;
  for (int s = start; s < end; ++s) {
    sum += SampleFixedToMillivolts(c[static_cast<std::size_t>(s)]) *
           std::polar(1.0, -kPi / 2.0 * s);
  }
  return sum * (2.0 / static_cast<double>(end - start));
}

TEST(ChromaErrorFunctionalTest, PalWindowsFollowTheLatticeZeroH) {
  // The generator draws each line 0H-relative, with 0H half a sample before
  // the line's first sample, and then delays it onto the 4fsc lattice. The
  // first picture sample is active_window_start after the drawn line start,
  // so it sits active_window_start + 0.5 after the line's measured 0H; the
  // burst gate likewise.
  const Project project = MakeBarsProject(Standard::kPal);
  std::vector<SampleFixed> y;
  std::vector<SampleFixed> c;
  std::vector<std::string> errors;
  ASSERT_TRUE(GenerationStage().Generate(project, &y, &c, &errors))
      << (errors.empty() ? "" : errors[0]);
  const TimingConstants timing = GetTimingConstants(Standard::kPal);
  y.resize(static_cast<std::size_t>(SamplesPerFrame4fsc(Standard::kPal)));
  const std::vector<int> offsets = BuildLineSampleOffsets(BuildLineSampleCounts(
      Standard::kPal, timing.lines_per_frame, timing.samples_per_line_4fsc));
  const SignalLevels levels = GetSignalLevels(project.cvbs_presets);
  const std::vector<std::optional<double>> edges =
      test_support::MeasureLineSyncEdges(y, offsets, levels.blanking_mv,
                                         levels.sync_tip_mv);
  const ActiveRasterGeometry geometry =
      GetActiveRasterGeometry(Standard::kPal, timing.sample_rate_4fsc_hz);
  const std::vector<PictureLineWindow> pictures =
      BuildPictureLineWindows(project, Standard::kPal);
  const std::vector<PictureLineWindow> bursts =
      BuildBurstLineWindows(Standard::kPal);
  const int burst_gate_start =
      BurstStartSamples(Standard::kPal, timing.sample_rate_4fsc_hz) -
      kBurstWindowGuardSamples;

  for (const int line : {23, 157, 310}) {
    const auto index = static_cast<std::size_t>(line - 1);
    ASSERT_TRUE(edges[index].has_value()) << "line " << line;
    const double zero_h = edges[index].value_or(0.0);
    const auto picture = std::find_if(
        pictures.begin(), pictures.end(),
        [line](const PictureLineWindow& w) { return w.line_1based == line; });
    ASSERT_NE(picture, pictures.end()) << "line " << line;
    EXPECT_NEAR(static_cast<double>(picture->start),
                zero_h + 0.5 + geometry.active_window_start_samples, 0.5)
        << "line " << line;
    EXPECT_NEAR(static_cast<double>(bursts[index].start),
                zero_h + 0.5 + burst_gate_start, 0.5)
        << "line " << line;
  }
  // Line 310's lattice delay is nearly two samples: the windows moved.
  const auto line310 = std::find_if(
      pictures.begin(), pictures.end(),
      [](const PictureLineWindow& w) { return w.line_1based == 310; });
  EXPECT_EQ(line310->start,
            static_cast<std::size_t>(offsets[309] +
                                     geometry.active_window_start_samples + 2));
}

TEST(ChromaErrorFunctionalTest, BarsHueAndSaturationShiftAgainstBurst) {
  constexpr double kGain = 1.05;
  constexpr double kPhaseDeg = 2.0;
  // 75% bars: white, yellow, cyan, green, magenta, red, blue, black. The six
  // coloured bars carry chroma.
  constexpr int kBars = 8;

  for (Standard standard : {Standard::kPal, Standard::kNtsc, Standard::kPalM}) {
    Project project = MakeBarsProject(standard);
    std::vector<SampleFixed> y;
    std::vector<SampleFixed> c;
    std::vector<std::string> errors;
    ASSERT_TRUE(GenerationStage().Generate(project, &y, &c, &errors))
        << (errors.empty() ? "" : errors[0]);

    project.chroma_error.enabled = true;
    project.chroma_error.gain = kGain;
    project.chroma_error.phase_deg = kPhaseDeg;
    std::vector<IGenerationStage::FrameScheduleItem> schedule(1U);
    schedule[0].section = &project.sections[0];
    std::vector<SampleFixed> shifted = c;
    ChromaErrorStage(1U).ApplyChromaError(project, schedule, 0U, 1U, &shifted);

    const TimingConstants timing = GetTimingConstants(standard);
    const std::vector<int> offsets =
        BuildLineSampleOffsets(BuildLineSampleCounts(
            standard, timing.lines_per_frame, timing.samples_per_line_4fsc));
    const ActiveRasterGeometry geometry =
        GetActiveRasterGeometry(standard, timing.sample_rate_4fsc_hz);
    int burst_start = 0;
    int burst_end = 0;
    BurstRange(standard, timing.sample_rate_4fsc_hz, &burst_start, &burst_end);
    const int window = geometry.active_window_end_samples -
                       geometry.active_window_start_samples;

    int measured = 0;
    for (const LineTimingPrimitive& line :
         BuildFrameTimingPrimitives(standard)) {
      // Skip the half lines at the field edges, which carry half a picture.
      const int row = ActiveFrameLineIndex(geometry, line.line_number_1based);
      const int field_row = row % geometry.active_lines_per_field;
      if (row < 0 || field_row == 0 ||
          field_row == geometry.active_lines_per_field - 1) {
        continue;
      }
      const int base =
          offsets[static_cast<std::size_t>(line.line_number_1based - 1)];
      const std::complex<double> burst_before =
          Phasor(c, base + burst_start, base + burst_end);
      const std::complex<double> burst_after =
          Phasor(shifted, base + burst_start, base + burst_end);
      ASSERT_EQ(burst_before, burst_after);
      // PAL-M burst blanking (BT.1700 Figure 9) reaches into the picture on
      // lines 260-261 and 522-525; there is no burst to demodulate against.
      if (std::abs(burst_before) < 1.0) {
        continue;
      }

      for (int bar = 1; bar < kBars - 1; ++bar) {
        // The middle half of the bar, clear of the chroma transitions.
        const int start = base + geometry.active_window_start_samples +
                          ((4 * bar + 1) * window) / (4 * kBars);
        int end = base + geometry.active_window_start_samples +
                  ((4 * bar + 3) * window) / (4 * kBars);
        end -= (end - start) % 4;
        const std::complex<double> before =
            Phasor(c, start, end) / burst_before;
        const std::complex<double> after =
            Phasor(shifted, start, end) / burst_after;
        const double hue_shift_deg =
            std::remainder(std::arg(after) - std::arg(before), 2.0 * kPi) *
            180.0 / kPi;
        ASSERT_NEAR(hue_shift_deg, kPhaseDeg, 0.2)
            << StandardToString(standard) << " line " << line.line_number_1based
            << " bar " << bar;
        ASSERT_NEAR(std::abs(after) / std::abs(before), kGain, 0.005 * kGain)
            << StandardToString(standard) << " line " << line.line_number_1based
            << " bar " << bar;
        ++measured;
      }
    }
    EXPECT_GT(measured, 6 * 400) << StandardToString(standard);
  }
}

// The generated frames of these tests are disc frame 0, colour sequence
// phase 0, so the frame starts the subcarrier lattice.
constexpr std::size_t kSequencePhase = 0U;

// U and the transmitted V (W = s V) of a subcarrier phasor measured by
// Phasor() on `line`: with C = U sin(p) + W cos(p) and p = pi/2 s + offset,
// the phasor is (W - jU) e^{j offset}.
void DecodeAxes(std::complex<double> phasor, Standard standard,
                const LineTimingPrimitive& line, double* u, double* w) {
  const std::complex<double> axes =
      phasor * std::polar(1.0, -PictureChromaPhaseOffsetRad(standard, line));
  *u = -axes.imag();
  *w = axes.real();
}

double VSwitch(Standard standard, const LineTimingPrimitive& line) {
  const bool pal = standard == Standard::kPal || standard == Standard::kPalM;
  return (pal && PalInvertVAxisForLine(kSequencePhase, line)) ? -1.0 : 1.0;
}

// The crosstalk demodulates against PictureChromaPhaseOffsetRad and the
// V-switch, so both must agree with the burst generation emits: PAL and PAL-M
// burst at +-135 degrees from +U with the decoded V positive (BT.1700 Part B
// Table 1 item 10f), NTSC burst at 180 degrees (SMPTE 170M Section 10).
TEST(ChromaErrorFunctionalTest, SubcarrierReferenceMatchesTheGeneratedBurst) {
  for (Standard standard : {Standard::kPal, Standard::kNtsc, Standard::kPalM}) {
    Project project = MakeBarsProject(standard);
    std::vector<SampleFixed> y;
    std::vector<SampleFixed> c;
    std::vector<std::string> errors;
    ASSERT_TRUE(GenerationStage().Generate(project, &y, &c, &errors))
        << (errors.empty() ? "" : errors[0]);
    const TimingConstants timing = GetTimingConstants(standard);
    const std::vector<int> offsets =
        BuildLineSampleOffsets(BuildLineSampleCounts(
            standard, timing.lines_per_frame, timing.samples_per_line_4fsc));
    const ActiveRasterGeometry geometry =
        GetActiveRasterGeometry(standard, timing.sample_rate_4fsc_hz);
    int burst_start = 0;
    int burst_end = 0;
    BurstRange(standard, timing.sample_rate_4fsc_hz, &burst_start, &burst_end);

    int measured = 0;
    for (const LineTimingPrimitive& line :
         BuildFrameTimingPrimitives(standard)) {
      const int base =
          offsets[static_cast<std::size_t>(line.line_number_1based - 1)];
      const std::complex<double> burst =
          Phasor(c, base + burst_start, base + burst_end);
      // Picture lines only: the reference is defined for picture chroma.
      if (std::abs(burst) < 1.0 ||
          ActiveFrameLineIndex(geometry, line.line_number_1based) < 0) {
        continue;
      }
      double u = 0.0;
      double w = 0.0;
      DecodeAxes(burst, standard, line, &u, &w);
      const double v = VSwitch(standard, line) * w;
      // The shaped burst envelope biases the four-sample phasor by about
      // 0.4 degrees; a wrong reference is off by a multiple of 90 degrees or
      // flips the sign of V, far outside this tolerance.
      constexpr double kTolerance = 0.01;
      const double amplitude = std::abs(burst);
      if (standard == Standard::kNtsc) {
        ASSERT_NEAR(u / amplitude, -1.0, kTolerance) << line.line_number_1based;
        ASSERT_NEAR(v / amplitude, 0.0, kTolerance) << line.line_number_1based;
      } else {
        ASSERT_NEAR(u / amplitude, -std::sqrt(0.5), kTolerance)
            << StandardToString(standard) << " line "
            << line.line_number_1based;
        ASSERT_NEAR(v / amplitude, std::sqrt(0.5), kTolerance)
            << StandardToString(standard) << " line "
            << line.line_number_1based;
      }
      ++measured;
    }
    EXPECT_GT(measured, 400) << StandardToString(standard);
  }
}

// Middle half of bar `bar` of 8 on the line starting at `base`, a whole
// number of subcarrier cycles long.
void BarRange(const ActiveRasterGeometry& geometry, int base, int bar,
              int* start, int* end) {
  constexpr int kBars = 8;
  const int window =
      geometry.active_window_end_samples - geometry.active_window_start_samples;
  *start = base + geometry.active_window_start_samples +
           ((4 * bar + 1) * window) / (4 * kBars);
  *end = base + geometry.active_window_start_samples +
         ((4 * bar + 3) * window) / (4 * kBars);
  *end -= (*end - *start) % 4;
}

TEST(ChromaErrorFunctionalTest, CrosstalkMixesTheDecodedComponents) {
  constexpr double kVToU = 0.2;
  constexpr double kUToV = -0.1;
  for (Standard standard : {Standard::kPal, Standard::kNtsc, Standard::kPalM}) {
    for (bool alternate : {false, true}) {
      if (alternate && standard == Standard::kNtsc) {
        continue;
      }
      Project project = MakeBarsProject(standard);
      std::vector<SampleFixed> y;
      std::vector<SampleFixed> c;
      std::vector<std::string> errors;
      ASSERT_TRUE(GenerationStage().Generate(project, &y, &c, &errors))
          << (errors.empty() ? "" : errors[0]);
      project.chroma_error.enabled = true;
      project.chroma_error.crosstalk = {.enabled = true,
                                        .v_to_u = kVToU,
                                        .u_to_v = kUToV,
                                        .alternate_with_switch = alternate};
      std::vector<IGenerationStage::FrameScheduleItem> schedule(1U);
      schedule[0].section = &project.sections[0];
      std::vector<SampleFixed> mixed = c;
      ChromaErrorStage(1U).ApplyChromaError(project, schedule, 0U, 1U, &y,
                                            &mixed);

      const TimingConstants timing = GetTimingConstants(standard);
      const std::vector<int> offsets =
          BuildLineSampleOffsets(BuildLineSampleCounts(
              standard, timing.lines_per_frame, timing.samples_per_line_4fsc));
      const ActiveRasterGeometry geometry =
          GetActiveRasterGeometry(standard, timing.sample_rate_4fsc_hz);
      int measured = 0;
      for (const LineTimingPrimitive& line :
           BuildFrameTimingPrimitives(standard)) {
        const int row = ActiveFrameLineIndex(geometry, line.line_number_1based);
        const int field_row = row % geometry.active_lines_per_field;
        if (row < 0 || field_row == 0 ||
            field_row == geometry.active_lines_per_field - 1) {
          continue;
        }
        const int base =
            offsets[static_cast<std::size_t>(line.line_number_1based - 1)];
        const double s = VSwitch(standard, line);
        const double a = alternate ? s : 1.0;
        for (int bar = 1; bar < 7; ++bar) {
          int start = 0;
          int end = 0;
          BarRange(geometry, base, bar, &start, &end);
          double u0 = 0.0;
          double w0 = 0.0;
          double u1 = 0.0;
          double w1 = 0.0;
          DecodeAxes(Phasor(c, start, end), standard, line, &u0, &w0);
          DecodeAxes(Phasor(mixed, start, end), standard, line, &u1, &w1);
          const double v0 = s * w0;
          const double v1 = s * w1;
          const double scale = std::hypot(u0, v0);
          ASSERT_NEAR(u1, u0 + (a * kVToU * v0), 0.002 * scale)
              << StandardToString(standard) << " line "
              << line.line_number_1based << " bar " << bar;
          ASSERT_NEAR(v1, v0 + (a * kUToV * u0), 0.002 * scale)
              << StandardToString(standard) << " line "
              << line.line_number_1based << " bar " << bar;
          ++measured;
        }
      }
      EXPECT_GT(measured, 6 * 400) << StandardToString(standard);
    }
  }
}

TEST(ChromaErrorFunctionalTest, DifferentialGainAndPhaseFollowTheLuma) {
  // Gain falls by half from blanking to 100 IRE; phase rises 10 degrees.
  const std::vector<LevelCurvePoint> gain = {{0.0, 1.0}, {100.0, 0.5}};
  const std::vector<LevelCurvePoint> phase = {{0.0, 0.0}, {100.0, 10.0}};
  for (Standard standard : {Standard::kPal, Standard::kNtsc}) {
    Project project = MakeBarsProject(standard);
    std::vector<SampleFixed> y;
    std::vector<SampleFixed> c;
    std::vector<std::string> errors;
    ASSERT_TRUE(GenerationStage().Generate(project, &y, &c, &errors))
        << (errors.empty() ? "" : errors[0]);
    project.chroma_error.enabled = true;
    project.chroma_error.differential = {
        .enabled = true, .gain = gain, .phase_deg = phase};
    std::vector<IGenerationStage::FrameScheduleItem> schedule(1U);
    schedule[0].section = &project.sections[0];
    std::vector<SampleFixed> shifted = c;
    ChromaErrorStage(1U).ApplyChromaError(project, schedule, 0U, 1U, &y,
                                          &shifted);

    const TimingConstants timing = GetTimingConstants(standard);
    const SignalLevels levels = GetSignalLevels(project.cvbs_presets);
    const std::vector<int> offsets =
        BuildLineSampleOffsets(BuildLineSampleCounts(
            standard, timing.lines_per_frame, timing.samples_per_line_4fsc));
    const ActiveRasterGeometry geometry =
        GetActiveRasterGeometry(standard, timing.sample_rate_4fsc_hz);
    // One mid-picture line of field 1.
    const int line_1based = Is625LineStandard(standard) ? 160 : 140;
    const int base = offsets[static_cast<std::size_t>(line_1based - 1)];
    for (int bar = 1; bar < 7; ++bar) {
      int start = 0;
      int end = 0;
      BarRange(geometry, base, bar, &start, &end);
      double luma_mv = 0.0;
      for (int n = start; n < end; ++n) {
        luma_mv += SampleFixedToMillivolts(y[static_cast<std::size_t>(n)]);
      }
      luma_mv /= static_cast<double>(end - start);
      const double ire = 100.0 * (luma_mv - levels.blanking_mv) /
                         (levels.white_mv - levels.blanking_mv);
      const std::complex<double> before = Phasor(c, start, end);
      const std::complex<double> after = Phasor(shifted, start, end);
      EXPECT_NEAR(std::abs(after) / std::abs(before), 1.0 - (0.005 * ire),
                  0.003)
          << StandardToString(standard) << " bar " << bar;
      EXPECT_NEAR(std::remainder(std::arg(after) - std::arg(before), 2 * kPi) *
                      180.0 / kPi,
                  0.1 * ire, 0.2)
          << StandardToString(standard) << " bar " << bar;
    }
  }
}

}  // namespace
}  // namespace videosynth
