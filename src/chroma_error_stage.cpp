/*
 * File:        chroma_error_stage.cpp
 * Module:      chroma_error
 * Purpose:     Applies one capture's chroma gain and phase error to the
 *              picture chroma of fixed-point mV C frame buffers.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "videosynth/chroma_error_stage.h"

#include <algorithm>
#include <cmath>
#include <random>

#include "videosynth/impairment_seed.h"
#include "videosynth/timing_constants.h"

namespace videosynth {

namespace {

constexpr double kPi = 3.14159265358979323846;

}  // namespace

ChromaErrorStage::ChromaErrorStage()
    : ChromaErrorStage(std::random_device{}()) {}

ChromaErrorStage::ChromaErrorStage(std::uint64_t run_base_seed)
    : run_base_seed_(run_base_seed),
      hilbert_kernel_(DesignChromaHilbertKernel()) {}

bool ChromaErrorStage::IsActive(const ChromaErrorParameters& chroma_error) {
  if (!chroma_error.enabled) {
    return false;
  }
  const ChromaJitterParameters& jitter = chroma_error.line_jitter;
  const bool jittered =
      jitter.enabled && (jitter.phase_deg != 0.0 || jitter.gain != 0.0);
  return chroma_error.gain != 1.0 || chroma_error.phase_deg != 0.0 || jittered;
}

void ChromaErrorStage::ApplyChromaError(
    const Project& project,
    const std::vector<IGenerationStage::FrameScheduleItem>& schedule,
    std::size_t frame_offset, std::size_t frame_count,
    std::vector<SampleFixed>* c_mv) const {
  const ChromaErrorParameters& chroma_error = project.chroma_error;
  const Standard standard = project.cvbs_presets.video_standard_preset;
  if (!IsActive(chroma_error) || c_mv == nullptr ||
      (standard != Standard::kPal && standard != Standard::kNtsc &&
       standard != Standard::kPalM)) {
    return;
  }

  // Generation always fills 4fsc frames; output resampling comes later.
  const auto samples_per_frame =
      static_cast<std::size_t>(SamplesPerFrame4fsc(standard));
  const std::vector<PictureLineWindow> windows =
      BuildPictureLineWindows(project, standard);

  const ChromaJitterParameters& jitter = chroma_error.line_jitter;
  const bool jittered =
      jitter.enabled && (jitter.phase_deg != 0.0 || jitter.gain != 0.0);
  const std::uint64_t jitter_seed =
      jitter.seed_specified ? static_cast<std::uint64_t>(jitter.seed)
                            : run_base_seed_;
  const double phase_rad = chroma_error.phase_deg * kPi / 180.0;
  const double jitter_phase_rad = jitter.phase_deg * kPi / 180.0;

  // With include_burst the burst of every line takes the error too: a
  // picture line's burst its own (jittered) error, any other line's burst the
  // block's gain and phase.
  std::vector<PictureLineWindow> burst_windows;
  std::vector<int> picture_index_by_line;
  if (chroma_error.include_burst) {
    burst_windows = BuildBurstLineWindows(standard);
    picture_index_by_line.assign(burst_windows.size(), -1);
    for (std::size_t p = 0; p < windows.size(); ++p) {
      picture_index_by_line[static_cast<std::size_t>(windows[p].line_1based -
                                                     1)] = static_cast<int>(p);
    }
  }

  std::size_t max_window = 0;
  for (const PictureLineWindow& window : windows) {
    max_window = std::max(max_window, window.count);
  }
  for (const PictureLineWindow& window : burst_windows) {
    max_window = std::max(max_window, window.count);
  }
  std::vector<double> chroma(max_window);
  std::vector<double> quadrature(max_window);
  std::vector<double> line_phase_rad(windows.size(), phase_rad);
  std::vector<double> line_gain(windows.size(), chroma_error.gain);

  // Scales window's chroma by gain and advances its phase by phase.
  auto rotate = [&](SampleFixed* frame, const PictureLineWindow& window,
                    double gain, double phase) {
    SampleFixed* line = frame + window.start;
    const auto count = static_cast<std::ptrdiff_t>(window.count);
    for (std::ptrdiff_t n = 0; n < count; ++n) {
      chroma[static_cast<std::size_t>(n)] = static_cast<double>(line[n]);
    }

    HilbertTransformWindow(hilbert_kernel_, chroma.data(), window.count,
                           quadrature.data());

    const double in_phase_gain = gain * std::cos(phase);
    const double quadrature_gain = gain * std::sin(phase);
    for (std::ptrdiff_t n = 0; n < count; ++n) {
      const auto s = static_cast<std::size_t>(n);
      const double rotated =
          (in_phase_gain * chroma[s]) - (quadrature_gain * quadrature[s]);
      line[n] = static_cast<SampleFixed>(std::llround(rotated));
    }
  };

  for (std::size_t i = 0; i < frame_count; ++i) {
    const std::size_t global_frame = frame_offset + i;
    const std::size_t frame_start = i * samples_per_frame;
    if (global_frame >= schedule.size() ||
        frame_start + samples_per_frame > c_mv->size()) {
      break;
    }
    SampleFixed* frame = c_mv->data() + frame_start;

    // Two unit normals per picture line, phase then gain, drawn whether or
    // not each deviation is set, so changing one never moves the other.
    if (jittered) {
      std::mt19937_64 rng(DeriveImpairmentSeed(
          jitter_seed, kChromaJitterSeedDomain, global_frame));
      std::normal_distribution<double> unit_normal(0.0, 1.0);
      for (std::size_t p = 0; p < windows.size(); ++p) {
        line_phase_rad[p] = phase_rad + (jitter_phase_rad * unit_normal(rng));
        line_gain[p] = chroma_error.gain + (jitter.gain * unit_normal(rng));
      }
    }

    for (std::size_t p = 0; p < windows.size(); ++p) {
      rotate(frame, windows[p], line_gain[p], line_phase_rad[p]);
    }
    for (std::size_t b = 0; b < burst_windows.size(); ++b) {
      const int p = picture_index_by_line[b];
      if (p < 0) {
        rotate(frame, burst_windows[b], chroma_error.gain, phase_rad);
      } else {
        const auto picture = static_cast<std::size_t>(p);
        rotate(frame, burst_windows[b], line_gain[picture],
               line_phase_rad[picture]);
      }
    }
  }
}

}  // namespace videosynth
