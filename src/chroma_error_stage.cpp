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

  std::size_t max_window = 0;
  for (const PictureLineWindow& window : windows) {
    max_window = std::max(max_window, window.count);
  }
  std::vector<double> chroma(max_window);
  std::vector<double> quadrature(max_window);

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
    std::mt19937_64 rng(DeriveImpairmentSeed(
        jitter_seed, kChromaJitterSeedDomain, global_frame));
    std::normal_distribution<double> unit_normal(0.0, 1.0);

    for (const PictureLineWindow& window : windows) {
      double line_phase_rad = phase_rad;
      double line_gain = chroma_error.gain;
      if (jittered) {
        line_phase_rad += jitter_phase_rad * unit_normal(rng);
        line_gain += jitter.gain * unit_normal(rng);
      }

      SampleFixed* line = frame + window.start;
      const auto count = static_cast<std::ptrdiff_t>(window.count);
      for (std::ptrdiff_t n = 0; n < count; ++n) {
        chroma[static_cast<std::size_t>(n)] = static_cast<double>(line[n]);
      }

      HilbertTransformWindow(hilbert_kernel_, chroma.data(), window.count,
                             quadrature.data());

      const double in_phase_gain = line_gain * std::cos(line_phase_rad);
      const double quadrature_gain = line_gain * std::sin(line_phase_rad);
      for (std::ptrdiff_t n = 0; n < count; ++n) {
        const auto s = static_cast<std::size_t>(n);
        const double rotated =
            (in_phase_gain * chroma[s]) - (quadrature_gain * quadrature[s]);
        line[n] = static_cast<SampleFixed>(std::llround(rotated));
      }
    }
  }
}

}  // namespace videosynth
