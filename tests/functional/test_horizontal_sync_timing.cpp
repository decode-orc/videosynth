/*
 * File:        test_horizontal_sync_timing.cpp
 * Module:      generation_stage_tests
 * Purpose:     Measures where the half-amplitude point (0H) of every line's
 *              horizontal sync falls in synthesised frames of each standard,
 *              relative to the storage line and to the 4fsc lattice
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "sync_edge_measurement.h"
#include "videosynth/colour_sequence.h"
#include "videosynth/fixed_point.h"
#include "videosynth/frame_line_layout.h"
#include "videosynth/generation_stage.h"
#include "videosynth/model.h"
#include "videosynth/signal_shaping.h"
#include "videosynth/signal_timing_model.h"
#include "videosynth/timing_constants.h"

namespace videosynth {
namespace {

// CVBS File Format Specification, Stored Frame and Line Origin: 0H falls
// midway between two samples, half a sample before the frame's first sample.
constexpr double kFrameOriginSamples = -0.5;
constexpr double kOriginTolerance = 0.02;

// Linear interpolation across the three-sample 525-line sync edge misplaces
// a crossing that falls between samples by up to 0.03 sample.
constexpr double kPulseWidthTolerance = 0.05;

constexpr double kPi = 3.14159265358979323846;
constexpr double kDegree = kPi / 180.0;

// Phase Phi of the colour burst of the storage line starting at
// `line_offset`, fitted over its flat middle as c(n) = A sin((pi/2) n + Phi)
// on the absolute sample index n. Whole subcarrier cycles are fitted, so the
// result is exact for a constant-amplitude carrier.
double BurstLatticePhaseRad(const std::vector<SampleFixed>& c, int line_offset,
                            Standard standard, double sample_rate_hz) {
  constexpr int kEnvelopeGuardSamples = 16;
  const int start = line_offset + BurstStartSamples(standard, sample_rate_hz) +
                    kEnvelopeGuardSamples;
  const int cycles = (BurstEndSamples(standard, sample_rate_hz) -
                      BurstStartSamples(standard, sample_rate_hz) -
                      (2 * kEnvelopeGuardSamples)) /
                     4;
  double in_phase = 0.0;
  double quadrature = 0.0;
  for (int n = start; n < start + (4 * cycles); ++n) {
    const double carrier = (kPi / 2.0) * static_cast<double>(n);
    const double sample =
        SampleFixedToMillivolts(c[static_cast<std::size_t>(n)]);
    in_phase += sample * std::sin(carrier);
    quadrature += sample * std::cos(carrier);
  }
  return std::atan2(quadrature, in_phase);
}

// The difference a - b wrapped into [-pi, pi).
double WrapRad(double a, double b) {
  double delta = std::fmod(a - b + kPi, 2.0 * kPi);
  if (delta < 0.0) {
    delta += 2.0 * kPi;
  }
  return delta - kPi;
}

Project MakeBarsProject(Standard standard) {
  Project project;
  project.cvbs_presets.video_standard_preset = standard;
  project.cvbs_presets.sample_encoding_preset =
      standard == Standard::kSecam ? "CVBS_TPG21_4FSC" : "CVBS_U10_4FSC";
  project.cvbs_presets.signal_state_preset = standard == Standard::kSecam
                                                 ? "STANDARD_STABLE_UNLOCKED"
                                                 : "STANDARD_STABLE_LOCKED";
  const char* raster = Is625LineStandard(standard) ? "720x576" : "720x486";
  project.sections.push_back(Section{
      .name = "Bars",
      .type = "progressive",
      .line_injections = {},
      .source = (std::filesystem::path(VIDEOSYNTH_SOURCE_DIR) /
                 "videosynth-assets/assets/exr" / raster / "100_BARS.exr")
                    .string(),
      .duration_frames = 1});
  return project;
}

struct MeasuredFrame {
  // The frame's luma and chroma planes.
  std::vector<SampleFixed> y;
  std::vector<SampleFixed> c;
  std::vector<int> line_offsets;
  // 0H of each line, in samples after its storage line's first sample.
  std::vector<double> origin_after_line_start;
  // 0H of each line, in samples from the start of the frame.
  std::vector<double> absolute;
};

MeasuredFrame MeasureFrame(Standard standard) {
  std::vector<SampleFixed> y;
  std::vector<SampleFixed> c;
  std::vector<std::string> errors;
  MeasuredFrame frame;
  const bool generated =
      GenerationStage().Generate(MakeBarsProject(standard), &y, &c, &errors);
  EXPECT_TRUE(generated) << (errors.empty() ? "" : errors[0]);
  if (!generated) {
    return frame;
  }

  const TimingConstants timing = GetTimingConstants(standard);
  const SignalLevels levels = GetSignalLevels(standard);
  frame.line_offsets = BuildLineSampleOffsets(BuildLineSampleCounts(
      standard, timing.lines_per_frame, timing.samples_per_line_4fsc));
  // One frame only: the measurement treats the frame as periodic.
  const std::size_t frame_samples =
      static_cast<std::size_t>(frame.line_offsets.back()) +
      static_cast<std::size_t>(
          BuildLineSampleCounts(standard, timing.lines_per_frame,
                                timing.samples_per_line_4fsc)
              .back());
  EXPECT_EQ(y.size(), frame_samples);

  const std::vector<std::optional<double>> edges =
      test_support::MeasureLineSyncEdges(
          y, frame.line_offsets, levels.blanking_mv, levels.sync_tip_mv);
  for (std::size_t i = 0; i < edges.size(); ++i) {
    EXPECT_TRUE(edges[i].has_value()) << "line " << (i + 1);
    const double edge = edges[i].value_or(0.0);
    frame.absolute.push_back(edge);
    frame.origin_after_line_start.push_back(edge - frame.line_offsets[i]);
  }
  frame.y = std::move(y);
  frame.c = std::move(c);
  return frame;
}

// Every line's 0H sits half a sample before its storage line's first sample:
// the orthogonal 525-line standards have a whole number of samples per line.
TEST(HorizontalSyncTimingTest, OrthogonalStandardsPlace0HHalfASampleEarly) {
  for (Standard standard : {Standard::kNtsc, Standard::kPalM}) {
    const MeasuredFrame frame = MeasureFrame(standard);
    ASSERT_FALSE(frame.origin_after_line_start.empty());
    for (std::size_t k = 0; k < frame.origin_after_line_start.size(); ++k) {
      EXPECT_NEAR(frame.origin_after_line_start[k], kFrameOriginSamples,
                  kOriginTolerance)
          << "line " << (k + 1);
    }
  }
}

// EBU Tech. 3280-E Section 1.2: on the 625-line standards 0H of frame line k
// (0-based) falls k x 1135.0064 samples after 0H of line 0, so it lies
// -0.5 + d_k after its storage line's first sample: a sawtooth reaching
// 1.497 on line 313 and falling back on lines 314 and 1.
TEST(HorizontalSyncTimingTest, Pal625LineSyncFallsOnTheLattice) {
  for (Standard standard : {Standard::kPal, Standard::kSecam}) {
    const MeasuredFrame frame = MeasureFrame(standard);
    ASSERT_EQ(frame.absolute.size(), 625U);
    EXPECT_NEAR(frame.absolute[0], kFrameOriginSamples, kOriginTolerance);

    for (std::size_t k = 0; k < frame.absolute.size(); ++k) {
      const int line = static_cast<int>(k) + 1;
      EXPECT_NEAR(
          frame.origin_after_line_start[k],
          kFrameOriginSamples + LatticeLineDelay(standard, line).Samples(),
          kOriginTolerance)
          << "line " << line;
      const double lattice = kFrameOriginSamples +
                             (static_cast<double>(k) *
                              static_cast<double>(kPalLatticeSamplesPerFrame) /
                              kLatticeDelayDenominator);
      EXPECT_NEAR(frame.absolute[k], lattice, kOriginTolerance)
          << "line " << line;
    }
    EXPECT_NEAR(frame.origin_after_line_start[312], 1.497, kOriginTolerance);
  }
}

// ITU-R BT.1700 Part B Table 1 and SMPTE 170M-2004 Table 2 measure sync
// pulse widths between the half-amplitude points of their edges. Every pulse
// kind, at the line start and at the half-line point, falls half a sample
// before its nominal time (0H, or 0H + H/2) and is its nominal width long.
TEST(HorizontalSyncTimingTest, SyncPulsesFallOnTimeAndHaveNominalWidths) {
  struct PulseCase {
    Standard standard;
    int line;
    bool mid_line;
    double width_seconds;
  };
  const std::vector<PulseCase> cases = {
      // Horizontal, equalizing and broad pulses at the line start, and an
      // equalizing and broad pulse at the half-line point.
      {Standard::kPal, 23, false, 4.7e-6},
      {Standard::kPal, 4, false, 2.3e-6},
      {Standard::kPal, 4, true, 2.3e-6},
      {Standard::kPal, 1, false, 27.3e-6},
      {Standard::kPal, 313, true, 27.3e-6},
      {Standard::kNtsc, 20, false, 4.7e-6},
      {Standard::kNtsc, 2, false, 2.3e-6},
      {Standard::kNtsc, 2, true, 2.3e-6},
      {Standard::kNtsc, 5, false, 27.1e-6},
      {Standard::kNtsc, 5, true, 27.1e-6},
      {Standard::kPalM, 20, false, 4.7e-6},
      {Standard::kPalM, 5, true, 27.1e-6},
  };
  for (Standard standard : {Standard::kPal, Standard::kNtsc, Standard::kPalM}) {
    const MeasuredFrame frame = MeasureFrame(standard);
    ASSERT_FALSE(frame.y.empty());
    const TimingConstants timing = GetTimingConstants(standard);
    const SignalLevels levels = GetSignalLevels(standard);
    const double half_amplitude =
        0.5 * (levels.blanking_mv + levels.sync_tip_mv);
    const double line_period = static_cast<double>(frame.y.size()) /
                               static_cast<double>(timing.lines_per_frame);
    for (const PulseCase& pulse : cases) {
      if (pulse.standard != standard) {
        continue;
      }
      const auto index = static_cast<std::size_t>(pulse.line - 1);
      const double nominal_fall =
          frame.line_offsets[index] + kFrameOriginSamples +
          LatticeLineDelay(standard, pulse.line).Samples() +
          (pulse.mid_line ? 0.5 * line_period : 0.0);
      const auto search = static_cast<std::int64_t>(std::floor(nominal_fall));
      const std::optional<double> fall = test_support::FindLevelCrossing(
          frame.y, search - 4, search + 4, half_amplitude, /*falling=*/true);
      ASSERT_TRUE(fall.has_value()) << "line " << pulse.line;
      const double fall_at = fall.value_or(0.0);
      EXPECT_NEAR(fall_at, nominal_fall, kOriginTolerance)
          << StandardToString(standard) << " line " << pulse.line;

      const double width = pulse.width_seconds * timing.sample_rate_4fsc_hz;
      const auto rise_search =
          static_cast<std::int64_t>(std::floor(fall_at + width));
      const std::optional<double> rise = test_support::FindLevelCrossing(
          frame.y, rise_search - 4, rise_search + 4, half_amplitude,
          /*falling=*/false);
      ASSERT_TRUE(rise.has_value()) << "line " << pulse.line;
      const double rise_at = rise.value_or(0.0);
      EXPECT_NEAR(rise_at - fall_at, width, kPulseWidthTolerance)
          << StandardToString(standard) << " line " << pulse.line;
    }
  }
}

// The PAL colour subcarrier is tied to absolute time, not to 0H: the burst
// of every line, whatever its lattice delay, is the same function of the
// absolute sample index, offset only by its +/-135 degree swing (ITU-R
// BT.1700 Part B Table 1 item 10f).
TEST(HorizontalSyncTimingTest, PalBurstIsLockedToTheLatticeOnEveryLine) {
  const MeasuredFrame frame = MeasureFrame(Standard::kPal);
  ASSERT_FALSE(frame.c.empty());
  const double fs = GetTimingConstants(Standard::kPal).sample_rate_4fsc_hz;
  // Lattice delays from 0.14 to 1.98 samples, in both fields of frame 0.
  for (int line : {23, 157, 309, 336, 500, 621}) {
    const double phase = BurstLatticePhaseRad(
        frame.c, frame.line_offsets[static_cast<std::size_t>(line - 1)],
        Standard::kPal, fs);
    const double swing = PalBurstPhaseRadForLine(
        0, BuildLineTimingPrimitive(Standard::kPal, line));
    EXPECT_NEAR(WrapRad(phase - swing, SubcarrierAnchorRad(Standard::kPal)),
                0.0, 0.1 * kDegree)
        << "line " << line
        << " (d = " << LatticeLineDelay(Standard::kPal, line).Samples() << ")";
  }
}

// Sc/H of the first frame, measured from the generated signal: the burst
// phase extrapolated along the lattice to the measured 0H.
// - PAL and PAL-M (EBU Tech. 3280-E Section 1.1.1): the +U axis, the burst
//   less its +/-135 degree swing, is at zero phase at 0H of line 1.
// - NTSC (SMPTE 170M-2004 Section 13.2): 0H of line 10 in colour frame A is a
//   positive-going zero crossing of the burst-locked sine wave.
TEST(HorizontalSyncTimingTest, FirstFrameIsAtZeroScH) {
  for (Standard standard : {Standard::kPal, Standard::kPalM, Standard::kNtsc}) {
    const MeasuredFrame frame = MeasureFrame(standard);
    ASSERT_FALSE(frame.c.empty());
    const double fs = GetTimingConstants(standard).sample_rate_4fsc_hz;
    constexpr int kBurstLine = 30;
    const double burst_phase = BurstLatticePhaseRad(
        frame.c, frame.line_offsets[kBurstLine - 1], standard, fs);
    double sch = 0.0;
    if (standard == Standard::kNtsc) {
      constexpr int kReferenceLine = 10;
      sch = burst_phase + ((kPi / 2.0) * frame.absolute[kReferenceLine - 1]);
    } else {
      const double swing = PalBurstPhaseRadForLine(
          0, BuildLineTimingPrimitive(standard, kBurstLine));
      sch = burst_phase - swing + ((kPi / 2.0) * frame.absolute[0]);
    }
    EXPECT_NEAR(WrapRad(sch, 0.0), 0.0, 1.0 * kDegree)
        << StandardToString(standard);
  }
}

// Centre of a rising step, as the first moment of its first differences.
// values[i] lies at `first_position` + i samples. A band-limited delay moves
// it by exactly the delay, and linear interpolation does not bias it.
double RisingStepCentre(const std::vector<double>& values,
                        double first_position) {
  double moment = 0.0;
  double rise = 0.0;
  for (std::size_t i = 0; i + 1 < values.size(); ++i) {
    const double step = values[i + 1] - values[i];
    moment += (first_position + static_cast<double>(i) + 0.5) * step;
    rise += step;
  }
  return moment / rise;
}

// Samples either side of a step that its centre is measured over.
constexpr int kStepGuard = 12;

// Samples searched past the active window's start for the first bar edge
// (PAL black is at blanking, so the bars' black border is not an edge), and
// how far a feature's measured distance after 0H may vary from line to line.
// The burst envelope estimate ripples slightly with the carrier's phase
// against the line's lattice delay.
constexpr int kPictureEdgeSearch = 40;
constexpr double kFeatureSpreadTolerance = 0.05;

// Features placed after 0H keep their place once the line is delayed onto
// the lattice: the burst envelope and the picture lie the same distance
// after the measured 0H on every line. The generator places them at whole
// samples after the storage line's first sample, which is half a sample after
// 0H (the midway convention): the burst gate opens at BurstStartSamples and
// reaches half amplitude half an envelope ramp later.
TEST(HorizontalSyncTimingTest, BurstAndPictureFollow0H) {
  for (Standard standard : {Standard::kPal, Standard::kNtsc, Standard::kPalM}) {
    const MeasuredFrame frame = MeasureFrame(standard);
    ASSERT_FALSE(frame.y.empty());
    const TimingConstants timing = GetTimingConstants(standard);
    const double fs = timing.sample_rate_4fsc_hz;
    const SignalLevels levels = GetSignalLevels(standard);
    const ActiveRasterGeometry geometry = GetActiveRasterGeometry(standard, fs);
    const double burst_ramp = RiseTimeToRampSamples(300.0e-9, fs);
    const double nominal_burst_half =
        static_cast<double>(BurstStartSamples(standard, fs)) +
        (0.5 * (burst_ramp - 1.0)) - kFrameOriginSamples;
    std::vector<double> burst_after_0h;
    std::vector<double> picture_after_0h;

    const int first_line = geometry.first_active_line_field1;
    for (int line : {first_line, first_line + 100, first_line + 200}) {
      const auto index = static_cast<std::size_t>(line - 1);
      const double origin = frame.absolute[index];
      const int offset = frame.line_offsets[index];

      // Burst envelope, from successive sample pairs (exact for a constant
      // carrier at fs / 4), over its rise.
      const int gate = offset + BurstStartSamples(standard, fs);
      const int ramp = static_cast<int>(burst_ramp);
      std::vector<double> envelope;
      for (int n = gate - kStepGuard; n < gate + ramp + kStepGuard; ++n) {
        envelope.push_back(std::hypot(
            SampleFixedToMillivolts(frame.c[static_cast<std::size_t>(n)]),
            SampleFixedToMillivolts(
                frame.c[static_cast<std::size_t>(n) + 1U])));
      }
      burst_after_0h.push_back(
          RisingStepCentre(envelope, gate - kStepGuard + 0.5) - origin);

      // The picture's first rising edge, from blanking to the first bar.
      const int window = offset + geometry.active_window_start_samples;
      double bar_mv = levels.blanking_mv;
      for (int n = window; n < window + kPictureEdgeSearch; ++n) {
        bar_mv = std::max(bar_mv, test_support::PeriodicSampleMv(frame.y, n));
      }
      const std::optional<double> picture_half =
          test_support::FindLevelCrossing(
              frame.y, window - 1, window + kPictureEdgeSearch,
              0.5 * (levels.blanking_mv + bar_mv), /*falling=*/false);
      ASSERT_TRUE(picture_half.has_value()) << "line " << line;
      const auto edge =
          static_cast<int>(std::floor(picture_half.value_or(0.0)));
      std::vector<double> luma;
      for (int n = edge - kStepGuard; n <= edge + kStepGuard; ++n) {
        luma.push_back(test_support::PeriodicSampleMv(frame.y, n));
      }
      picture_after_0h.push_back(RisingStepCentre(luma, edge - kStepGuard) -
                                 origin);
    }

    for (const std::vector<double>* after_0h :
         {&burst_after_0h, &picture_after_0h}) {
      const auto [lowest, highest] =
          std::minmax_element(after_0h->begin(), after_0h->end());
      EXPECT_LT(*highest - *lowest, kFeatureSpreadTolerance)
          << StandardToString(standard);
    }
    EXPECT_NEAR(burst_after_0h[0], nominal_burst_half, 0.5)
        << StandardToString(standard);
    EXPECT_GE(picture_after_0h[0], geometry.active_window_start_samples)
        << StandardToString(standard);
  }
}

}  // namespace
}  // namespace videosynth
