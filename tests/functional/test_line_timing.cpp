/*
 * File:        test_line_timing.cpp
 * Module:      line_timing_tests
 * Purpose:     End-to-end check of the per-line timing error on a generated
 *              still: measured as on a real capture (each frame's luma
 *              against the mean of the other frames, by least-squares shift),
 *              the per-line, per-field, line-to-line and slope figures match
 *              the block, and the burst phase of every line is unchanged.
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
#include <string>
#include <vector>

#include "videosynth/fixed_point.h"
#include "videosynth/generation_stage.h"
#include "videosynth/line_timing_stage.h"
#include "videosynth/picture_chroma.h"
#include "videosynth/signal_timing_model.h"
#include "videosynth/timing_constants.h"

namespace videosynth {
namespace {

constexpr std::size_t kFrames = 20;

Project MakeStillProject() {
  Project project;
  project.cvbs_presets.video_standard_preset = Standard::kPal;
  project.cvbs_presets.sample_encoding_preset = "CVBS_U10_4FSC";
  project.cvbs_presets.signal_state_preset = "STANDARD_STABLE_LOCKED";
  project.sections.push_back(
      Section{.name = "Still",
              .type = "progressive",
              .line_injections = {},
              .source = (std::filesystem::path(VIDEOSYNTH_SOURCE_DIR) /
                         "videosynth-assets/assets/exr/720x576/TESTCARD-F.exr")
                            .string(),
              .duration_frames = 1});
  return project;
}

constexpr double kPi = 3.14159265358979323846;
// The measurement band: luma below 2.5 MHz, as the real-disc figures were
// measured.
constexpr double kMeasureCutoffHz = 2.5e6;
constexpr int kLowPassHalfTaps = 24;

// Blackman-windowed sinc low-pass of signal at cutoff_hz.
std::vector<double> LowPass(const std::vector<double>& signal, double rate_hz) {
  const double cutoff = kMeasureCutoffHz / rate_hz;
  std::vector<double> taps;
  double gain = 0.0;
  for (int k = -kLowPassHalfTaps; k <= kLowPassHalfTaps; ++k) {
    const double x = static_cast<double>(k);
    const double sinc =
        k == 0 ? 2.0 * cutoff : std::sin(2.0 * kPi * cutoff * x) / (kPi * x);
    const double phase = kPi * (x + kLowPassHalfTaps) / kLowPassHalfTaps;
    const double window =
        0.42 - (0.5 * std::cos(phase)) + (0.08 * std::cos(2.0 * phase));
    taps.push_back(sinc * window);
    gain += taps.back();
  }
  std::vector<double> filtered(signal.size(), 0.0);
  const auto half = static_cast<std::size_t>(kLowPassHalfTaps);
  for (std::size_t n = half; n + half < signal.size(); ++n) {
    double acc = 0.0;
    for (std::size_t k = 0; k < taps.size(); ++k) {
      acc += taps[k] * signal[n + k - half];
    }
    filtered[n] = acc / gain;
  }
  return filtered;
}

// Least-squares shift (ns) of frame against reference over [start, end):
// frame - reference ~ -shift * reference', with a five-point derivative.
// Also returns the slope energy, which sets how well the span determines
// its shift, and its centroid: the span's shift is that of the sample where
// its edge detail is centred.
double EstimateShiftNs(const std::vector<double>& frame,
                       const std::vector<double>& reference, std::size_t start,
                       std::size_t end, double samples_per_ns, double* energy,
                       double* centroid = nullptr) {
  double numerator = 0.0;
  double denominator = 0.0;
  double moment = 0.0;
  for (std::size_t n = start; n < end; ++n) {
    const double slope = (reference[n - 2] - (8.0 * reference[n - 1]) +
                          (8.0 * reference[n + 1]) - reference[n + 2]) /
                         12.0;
    numerator += (frame[n] - reference[n]) * slope;
    denominator += slope * slope;
    moment += static_cast<double>(n) * slope * slope;
  }
  if (energy != nullptr) {
    *energy = denominator;
  }
  if (centroid != nullptr) {
    *centroid = moment / denominator;
  }
  return -(numerator / denominator) / samples_per_ns;
}

TEST(LineTimingFunctionalTest, HeldStillMeasuresAsConfiguredAndBurstStays) {
  const Standard standard = Standard::kPal;
  Project project = MakeStillProject();
  std::vector<SampleFixed> still_y;
  std::vector<SampleFixed> still_c;
  std::vector<std::string> errors;
  ASSERT_TRUE(GenerationStage().Generate(project, &still_y, &still_c, &errors))
      << (errors.empty() ? "" : errors[0]);
  const std::size_t samples = still_y.size();

  project.line_timing.enabled = true;
  LineTimingLayer& capture = project.line_timing.capture;
  capture.enabled = true;
  capture.line_ns = 4.9;
  capture.line_correlation = 0.6;
  capture.field_ns = 3.1;
  capture.slope_ns = 3.4;
  capture.seed_specified = true;
  capture.seed = 7;
  std::vector<IGenerationStage::FrameScheduleItem> schedule(kFrames);
  std::vector<SampleFixed> y;
  std::vector<SampleFixed> c;
  for (std::size_t f = 0; f < kFrames; ++f) {
    schedule[f].section = &project.sections[0];
    y.insert(y.end(), still_y.begin(), still_y.end());
    c.insert(c.end(), still_c.begin(), still_c.end());
  }
  LineTimingStage(1U).ApplyLineTiming(project, schedule, 0U, kFrames, &y, &c);

  // The burst of every line, and so its phase, is untouched.
  for (const PictureLineWindow& burst : BuildBurstLineWindows(standard)) {
    for (std::size_t f = 0; f < kFrames; ++f) {
      for (std::size_t n = burst.start; n < burst.start + burst.count; ++n) {
        ASSERT_EQ(c[(f * samples) + n], still_c[n]) << burst.line_1based;
      }
    }
  }

  const TimingConstants timing = GetTimingConstants(standard);
  std::vector<std::vector<double>> luma(kFrames);
  std::vector<double> sum(samples, 0.0);
  for (std::size_t f = 0; f < kFrames; ++f) {
    const auto begin = y.begin() + static_cast<std::ptrdiff_t>(f * samples);
    luma[f] = LowPass(std::vector<double>(
                          begin, begin + static_cast<std::ptrdiff_t>(samples)),
                      timing.sample_rate_4fsc_hz);
    for (std::size_t n = 0; n < samples; ++n) {
      sum[n] += luma[f][n];
    }
  }
  const double samples_per_ns = timing.sample_rate_4fsc_hz * 1e-9;
  const std::vector<PictureLineWindow> windows =
      BuildPictureLineWindows(project, standard);

  // Only lines with enough edge detail in both halves determine their
  // shift: the slope energy must reach a fifth of the median line's.
  std::vector<double> frame(samples);
  std::vector<double> reference =
      LowPass(std::vector<double>(still_y.begin(), still_y.end()),
              timing.sample_rate_4fsc_hz);
  std::vector<double> energies;
  for (const PictureLineWindow& window : windows) {
    for (std::size_t half = 0; half < 2U; ++half) {
      const std::size_t start = window.start + 20U + (half * window.count / 2U);
      const std::size_t end = start + (window.count / 2U) - 20U;
      double energy = 0.0;
      EstimateShiftNs(reference, reference, start, end, samples_per_ns,
                      &energy);
      energies.push_back(energy);
    }
  }
  std::vector<double> sorted = energies;
  std::sort(sorted.begin(), sorted.end());
  const double threshold = 0.2 * sorted[sorted.size() / 2U];

  double field_sq = 0.0;
  double within_sq = 0.0;
  double lag1 = 0.0;
  double slope_sq = 0.0;
  std::size_t lines = 0;
  std::size_t lag_pairs = 0;
  std::size_t fields = 0;
  for (std::size_t f = 0; f < kFrames; ++f) {
    for (std::size_t n = 0; n < samples; ++n) {
      frame[n] = luma[f][n];
      reference[n] = (sum[n] - frame[n]) / static_cast<double>(kFrames - 1);
    }
    for (int field = 1; field <= 2; ++field) {
      // Shifts of the usable lines, with each one's field-line position so
      // that only true neighbours count towards the correlation.
      std::vector<double> shifts;
      std::vector<int> rows;
      for (std::size_t w = 0; w < windows.size(); ++w) {
        const PictureLineWindow& window = windows[w];
        if (GetFieldIndex(standard, window.line_1based) != field ||
            energies[2U * w] < threshold ||
            energies[(2U * w) + 1U] < threshold) {
          continue;
        }
        const std::size_t start = window.start + 20U;
        const std::size_t end = window.start + window.count - 20U;
        const std::size_t middle = window.start + (window.count / 2U);
        shifts.push_back(EstimateShiftNs(frame, reference, start, end,
                                         samples_per_ns, nullptr));
        double left_centre = 0.0;
        double right_centre = 0.0;
        const double left =
            EstimateShiftNs(frame, reference, start, middle, samples_per_ns,
                            nullptr, &left_centre);
        const double right =
            EstimateShiftNs(frame, reference, middle, end, samples_per_ns,
                            nullptr, &right_centre);
        // The slope is the shift difference between the centres of the two
        // halves, half a line apart; each half's detail sits where the
        // picture puts it, so the measured difference is scaled to that
        // spacing.
        const double spacing = static_cast<double>(window.count) / 2.0;
        const double slope =
            (right - left) * spacing / (right_centre - left_centre);
        slope_sq += slope * slope;
        rows.push_back(window.line_1based);
      }
      ASSERT_GT(shifts.size(), 100U);
      double mean = 0.0;
      for (double shift : shifts) {
        mean += shift;
      }
      mean /= static_cast<double>(shifts.size());
      field_sq += mean * mean;
      ++fields;
      for (std::size_t k = 0; k < shifts.size(); ++k) {
        within_sq += (shifts[k] - mean) * (shifts[k] - mean);
        if (k + 1 < shifts.size() && rows[k + 1] == rows[k] + 1) {
          lag1 += (shifts[k] - mean) * (shifts[k + 1] - mean);
          ++lag_pairs;
        }
      }
      lines += shifts.size();
    }
  }
  const double within_variance = within_sq / static_cast<double>(lines);
  // The leave-one-out reference carries 1/19 of the other frames' error.
  const double reference_gain =
      std::sqrt(1.0 + (1.0 / static_cast<double>(kFrames - 1)));
  const double within_rms = std::sqrt(within_variance) / reference_gain;
  const double field_rms =
      std::sqrt(field_sq / static_cast<double>(fields)) / reference_gain;
  // The halves' centres are a quarter-line either side of the line centre,
  // as the slope is defined; the trimmed edges move them slightly inwards.
  const double slope_rms =
      std::sqrt(slope_sq / static_cast<double>(lines)) / reference_gain;
  const double correlation =
      (lag1 / static_cast<double>(lag_pairs)) / within_variance;

  EXPECT_NEAR(within_rms, 4.9, 0.15 * 4.9);
  EXPECT_NEAR(field_rms, 3.1, 0.15 * 3.1);
  EXPECT_NEAR(slope_rms, 3.4, 0.15 * 3.4);
  EXPECT_NEAR(correlation, 0.6, 0.15 * 0.6);
}

}  // namespace
}  // namespace videosynth
