/*
 * File:        test_chroma_error.cpp
 * Module:      chroma_error_tests
 * Purpose:     End-to-end check of the chroma error on generated colour bars:
 *              demodulated against the generated burst, every bar's hue
 *              shifts by phase_deg and its saturation scales by gain on PAL,
 *              NTSC and PAL-M.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <cmath>
#include <complex>
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

#include "videosynth/chroma_error_stage.h"
#include "videosynth/fixed_point.h"
#include "videosynth/frame_line_layout.h"
#include "videosynth/generation_stage.h"
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

}  // namespace
}  // namespace videosynth
