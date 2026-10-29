/*
 * File:        test_channel_impairments.cpp
 * Module:      channel_tests
 * Purpose:     End-to-end checks of the channel edge smear and FM beat on
 *              generated PAL frames: the smear is recovered from a VITS bar
 *              edge, the beat is the same on captures with different chroma
 *              errors, and a greyscale picture carries no beat.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "videosynth/channel_stage.h"
#include "videosynth/chroma_error_stage.h"
#include "videosynth/fixed_point.h"
#include "videosynth/generation_stage.h"
#include "videosynth/picture_chroma.h"
#include "videosynth/timing_constants.h"

namespace videosynth {
namespace {

Project MakePalProject(const std::string& source) {
  Project project;
  project.cvbs_presets.video_standard_preset = Standard::kPal;
  project.cvbs_presets.sample_encoding_preset = "CVBS_U10_4FSC";
  project.cvbs_presets.signal_state_preset = "STANDARD_STABLE_LOCKED";
  project.line_injections.vits.push_back(
      VitsInjection{.vits_type = "uk-national", .target_lines = {19}});
  project.sections.push_back(
      Section{.name = "S",
              .type = "progressive",
              .line_injections = {},
              .source = (std::filesystem::path(VIDEOSYNTH_SOURCE_DIR) /
                         "videosynth-assets/assets/exr/720x576" / source)
                            .string(),
              .duration_frames = 1});
  return project;
}

void Generate(const Project& project, std::vector<SampleFixed>* y,
              std::vector<SampleFixed>* c) {
  std::vector<std::string> errors;
  ASSERT_TRUE(GenerationStage().Generate(project, y, c, &errors))
      << (errors.empty() ? "" : errors[0]);
  ASSERT_EQ(y->size(),
            static_cast<std::size_t>(SamplesPerFrame4fsc(Standard::kPal)));
}

// Least-squares fit of y = x + c (LP_tau(x) - x) over [start, end): c is
// solved in closed form for each tau, and tau is found by golden-section
// search. LP_tau is primed over the 400 samples before start.
void FitEdgeSmear(const std::vector<double>& x, const std::vector<double>& y,
                  std::size_t start, std::size_t end, double* fraction,
                  double* tau) {
  const auto residual = [&](double candidate_tau, double* best_c) {
    const double pole = std::exp(-1.0 / candidate_tau);
    double state = x[start - 400U];
    double cross = 0.0;
    double energy = 0.0;
    std::vector<double> basis;
    for (std::size_t n = start - 400U; n < end; ++n) {
      state = (pole * state) + ((1.0 - pole) * x[n]);
      if (n >= start) {
        const double b = state - x[n];
        cross += b * (y[n] - x[n]);
        energy += b * b;
        basis.push_back(b);
      }
    }
    *best_c = cross / energy;
    double error = 0.0;
    for (std::size_t n = start; n < end; ++n) {
      const double e = y[n] - x[n] - (*best_c * basis[n - start]);
      error += e * e;
    }
    return error;
  };

  constexpr double kGolden = 0.6180339887498949;
  double low = 0.2;
  double high = 50.0;
  double c_unused = 0.0;
  for (int i = 0; i < 100; ++i) {
    const double a = high - (kGolden * (high - low));
    const double b = low + (kGolden * (high - low));
    if (residual(a, &c_unused) < residual(b, &c_unused)) {
      high = b;
    } else {
      low = a;
    }
  }
  *tau = 0.5 * (low + high);
  residual(*tau, fraction);
}

TEST(ChannelImpairmentsFunctionalTest, EdgeSmearFitsBackFromVitsBarEdge) {
  constexpr double kFraction = 0.18;
  constexpr double kTau = 6.5;
  Project project = MakePalProject("BLACK.exr");
  std::vector<SampleFixed> y;
  std::vector<SampleFixed> c;
  Generate(project, &y, &c);
  const std::vector<SampleFixed> clean = y;

  project.channel.enabled = true;
  project.channel.edge_smear.enabled = true;
  project.channel.edge_smear.fraction = kFraction;
  project.channel.edge_smear.tau_samples = kTau;
  ChannelStage().ApplyChannel(project, {}, 0U, 1U, &y, &c);

  // The steepest fall on VITS line 19 (the trailing edge of its bar).
  const TimingConstants timing = GetTimingConstants(Standard::kPal);
  const std::size_t line_start =
      18U * static_cast<std::size_t>(timing.samples_per_line_4fsc);
  std::vector<double> x(clean.size());
  std::vector<double> smeared(y.size());
  for (std::size_t n = 0; n < clean.size(); ++n) {
    x[n] = SampleFixedToMillivolts(clean[n]);
    smeared[n] = SampleFixedToMillivolts(y[n]);
  }
  std::size_t edge = 0;
  double steepest = 0.0;
  // Skip sync and burst: the bar sits in the active part of the line.
  for (std::size_t s = line_start + 250U; s < line_start + 1100U; ++s) {
    const double fall = x[s] - x[s + 1U];
    if (fall > steepest) {
      steepest = fall;
      edge = s;
    }
  }
  ASSERT_GT(steepest, 50.0) << "no bar edge found on line 19";

  double fraction = 0.0;
  double tau = 0.0;
  FitEdgeSmear(x, smeared, edge - 20U, edge + 120U, &fraction, &tau);
  EXPECT_NEAR(fraction, kFraction, 0.02 * kFraction);
  EXPECT_NEAR(tau, kTau, 0.02 * kTau);
}

TEST(ChannelImpairmentsFunctionalTest,
     CapturesWithDifferentChromaErrorShareTheBeat) {
  Project project = MakePalProject("75_BARS.exr");
  std::vector<SampleFixed> y;
  std::vector<SampleFixed> c;
  Generate(project, &y, &c);
  const std::vector<SampleFixed> clean_y = y;

  project.channel.enabled = true;
  project.channel.fm_beat.enabled = true;
  project.channel.fm_beat.level_db = -20.0;
  std::vector<IGenerationStage::FrameScheduleItem> schedule(1U);
  schedule[0].section = &project.sections[0];

  const auto capture = [&](double gain, double phase_deg,
                           std::vector<SampleFixed>* capture_y,
                           std::vector<SampleFixed>* capture_c) {
    *capture_y = y;
    *capture_c = c;
    Project capture_project = project;
    capture_project.chroma_error.enabled = true;
    capture_project.chroma_error.gain = gain;
    capture_project.chroma_error.phase_deg = phase_deg;
    ChannelStage().ApplyChannel(capture_project, schedule, 0U, 1U, capture_y,
                                capture_c);
    ChromaErrorStage(1U).ApplyChromaError(capture_project, schedule, 0U, 1U,
                                          capture_c);
  };
  std::vector<SampleFixed> y_a;
  std::vector<SampleFixed> c_a;
  std::vector<SampleFixed> y_b;
  std::vector<SampleFixed> c_b;
  capture(1.05, 2.0, &y_a, &c_a);
  capture(0.92, -4.0, &y_b, &c_b);

  EXPECT_NE(c_a, c_b);
  ASSERT_EQ(y_a, y_b);
  // The beat is present, and only in the picture windows.
  std::vector<bool> in_window(y.size(), false);
  for (const PictureLineWindow& window :
       BuildPictureLineWindows(project, Standard::kPal)) {
    for (std::size_t s = window.start; s < window.start + window.count; ++s) {
      in_window[s] = true;
    }
  }
  std::size_t changed = 0;
  for (std::size_t s = 0; s < y.size(); ++s) {
    if (y_a[s] != clean_y[s]) {
      ASSERT_TRUE(in_window[s]) << s;
      ++changed;
    }
  }
  EXPECT_GT(changed, 100000U);
}

TEST(ChannelImpairmentsFunctionalTest, GreyscalePictureIsBitIdenticalWithBeat) {
  // A rendered black frame with luma steps and ramps painted into every
  // picture line. The bundled greyscale EXRs are not used: their black
  // border columns carry Cb = Cr = 514, a faint tint that encodes as ~2 mV
  // of picture chroma at the line ends.
  Project project = MakePalProject("BLACK.exr");
  std::vector<SampleFixed> y;
  std::vector<SampleFixed> c;
  Generate(project, &y, &c);
  for (const PictureLineWindow& window :
       BuildPictureLineWindows(project, Standard::kPal)) {
    for (std::size_t i = 0; i < window.count; ++i) {
      const std::size_t s = window.start + i;
      ASSERT_EQ(c[s], 0) << "black picture carries chroma at " << s;
      // Five 70 mV steps across the first half, then a ramp to white.
      const double position =
          static_cast<double>(i) / static_cast<double>(window.count);
      const double level_mv = (position < 0.5)
                                  ? 70.0 * std::floor(10.0 * position)
                                  : 700.0 * position;
      y[s] = MillivoltsToSampleFixed(level_mv);
    }
  }
  const std::vector<SampleFixed> clean_y = y;
  const std::vector<SampleFixed> clean_c = c;

  project.channel.enabled = true;
  project.channel.fm_beat.enabled = true;
  project.channel.fm_beat.level_db = 0.0;
  ChannelStage().ApplyChannel(project, {}, 0U, 1U, &y, &c);
  EXPECT_EQ(y, clean_y);
  EXPECT_EQ(c, clean_c);
}

}  // namespace
}  // namespace videosynth
