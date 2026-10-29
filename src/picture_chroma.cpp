/*
 * File:        picture_chroma.cpp
 * Module:      picture_chroma
 * Purpose:     Picture-chroma helpers shared by the chroma error stage and
 *              the FM-beat model: the picture line windows of a 4fsc frame
 *              and the chroma Hilbert transformer.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "videosynth/picture_chroma.h"

#include <algorithm>
#include <set>

#include "videosynth/frame_line_layout.h"
#include "videosynth/kaiser_window.h"
#include "videosynth/signal_timing_model.h"
#include "videosynth/timing_constants.h"

namespace videosynth {

namespace {

constexpr double kPi = 3.14159265358979323846;

}  // namespace

std::vector<double> DesignChromaHilbertKernel() {
  constexpr int kHalf = kChromaHilbertHalfTaps;
  std::vector<double> kernel(2U * static_cast<std::size_t>(kHalf) + 1U, 0.0);
  for (int k = -kHalf; k <= kHalf; ++k) {
    if (k % 2 == 0) {
      continue;
    }
    const double ratio = static_cast<double>(k) / static_cast<double>(kHalf);
    const int tap_index = k + kHalf;
    kernel[static_cast<std::size_t>(tap_index)] =
        (2.0 / (kPi * static_cast<double>(k))) *
        KaiserWindow(ratio, kChromaHilbertKaiserBeta);
  }
  return kernel;
}

void HilbertTransformWindow(const std::vector<double>& kernel,
                            const double* input, std::size_t count,
                            double* output) {
  constexpr int kHalf = kChromaHilbertHalfTaps;
  static_assert(kHalf % 2 == 1, "stepping by two from -kHalf visits odd taps");
  std::fill_n(output, count, 0.0);
  const auto length = static_cast<std::ptrdiff_t>(count);
  // Only odd taps are non-zero.
  for (int k = -kHalf; k <= kHalf; k += 2) {
    const int tap_index = k + kHalf;
    const double tap = kernel[static_cast<std::size_t>(tap_index)];
    const std::ptrdiff_t first = std::max<std::ptrdiff_t>(0, k);
    const std::ptrdiff_t last = std::min<std::ptrdiff_t>(length, length + k);
    for (std::ptrdiff_t n = first; n < last; ++n) {
      output[n] += tap * input[n - k];
    }
  }
}

std::vector<PictureLineWindow> BuildPictureLineWindows(const Project& project,
                                                       Standard standard) {
  const TimingConstants timing = GetTimingConstants(standard);
  const std::vector<int> line_samples = BuildLineSampleCounts(
      standard, timing.lines_per_frame, timing.samples_per_line_4fsc);
  const std::vector<int> line_offsets = BuildLineSampleOffsets(line_samples);
  const ActiveRasterGeometry geometry =
      GetActiveRasterGeometry(standard, timing.sample_rate_4fsc_hz);

  std::set<int> vits_lines;
  for (const VitsInjection& vits : project.line_injections.vits) {
    vits_lines.insert(vits.target_lines.begin(), vits.target_lines.end());
  }

  std::vector<PictureLineWindow> windows;
  for (const LineTimingPrimitive& line : BuildFrameTimingPrimitives(standard)) {
    if (line.sync_pulse_kind != SyncPulseKind::kHorizontal ||
        line.content_kind != LineContentKind::kActivePicture ||
        ActiveFrameLineIndex(geometry, line.line_number_1based) < 0 ||
        vits_lines.count(line.line_number_1based) != 0U) {
      continue;
    }
    const auto index = static_cast<std::size_t>(line.line_number_1based - 1);
    const int samples = line_samples[index];
    const int start =
        std::clamp(geometry.active_window_start_samples, 0, samples);
    const int end =
        std::clamp(geometry.active_window_end_samples, start, samples);
    windows.push_back(PictureLineWindow{
        .start = static_cast<std::size_t>(line_offsets[index] + start),
        .count = static_cast<std::size_t>(end - start)});
  }
  return windows;
}

}  // namespace videosynth
