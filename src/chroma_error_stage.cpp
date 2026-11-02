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

#include "videosynth/colour_sequence.h"
#include "videosynth/frame_enrichment.h"
#include "videosynth/impairment_seed.h"
#include "videosynth/signal_timing_model.h"
#include "videosynth/timing_constants.h"

namespace videosynth {

namespace {

constexpr double kPi = 3.14159265358979323846;

// Interpolates a differential curve at level_ire: breakpoints joined by
// straight lines, the end values held beyond them, `identity` when empty.
double InterpolateLevelCurve(const std::vector<LevelCurvePoint>& curve,
                             double level_ire, double identity) {
  if (curve.empty()) {
    return identity;
  }
  if (level_ire <= curve.front().level_ire) {
    return curve.front().value;
  }
  for (std::size_t i = 1; i < curve.size(); ++i) {
    const LevelCurvePoint& lower = curve[i - 1];
    const LevelCurvePoint& upper = curve[i];
    if (level_ire <= upper.level_ire) {
      const double fraction =
          (level_ire - lower.level_ire) / (upper.level_ire - lower.level_ire);
      return lower.value + (fraction * (upper.value - lower.value));
    }
  }
  return curve.back().value;
}

bool CurveIsIdentity(const std::vector<LevelCurvePoint>& curve,
                     double identity) {
  return std::all_of(curve.begin(), curve.end(),
                     [identity](const LevelCurvePoint& point) {
                       return point.value == identity;
                     });
}

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
  return chroma_error.gain != 1.0 || chroma_error.phase_deg != 0.0 ||
         jittered || DifferentialIsActive(chroma_error) ||
         CrosstalkIsActive(chroma_error);
}

bool ChromaErrorStage::DifferentialIsActive(
    const ChromaErrorParameters& chroma_error) {
  const DifferentialParameters& differential = chroma_error.differential;
  return chroma_error.enabled && differential.enabled &&
         (!CurveIsIdentity(differential.gain, 1.0) ||
          !CurveIsIdentity(differential.phase_deg, 0.0));
}

bool ChromaErrorStage::CrosstalkIsActive(
    const ChromaErrorParameters& chroma_error) {
  const CrosstalkParameters& crosstalk = chroma_error.crosstalk;
  return chroma_error.enabled && crosstalk.enabled &&
         (crosstalk.v_to_u != 0.0 || crosstalk.u_to_v != 0.0);
}

void ChromaErrorStage::ApplyChromaError(
    const Project& project,
    const std::vector<IGenerationStage::FrameScheduleItem>& schedule,
    std::size_t frame_offset, std::size_t frame_count,
    std::vector<SampleFixed>* c_mv) const {
  ApplyChromaError(project, schedule, frame_offset, frame_count, nullptr, c_mv);
}

void ChromaErrorStage::ApplyChromaError(
    const Project& project,
    const std::vector<IGenerationStage::FrameScheduleItem>& schedule,
    std::size_t frame_offset, std::size_t frame_count,
    const std::vector<SampleFixed>* y_mv,
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

  // Differential gain and phase read the luma under each sample, in IRE.
  const bool differential =
      DifferentialIsActive(chroma_error) && y_mv != nullptr;
  const SignalLevels levels = GetSignalLevels(project.cvbs_presets);
  const double ire_per_fixed = 100.0 / ((levels.white_mv - levels.blanking_mv) *
                                        static_cast<double>(kSampleScale));
  const double blanking_fixed =
      levels.blanking_mv * static_cast<double>(kSampleScale);

  // Crosstalk mixes the decoded U and V, so each line is demodulated against
  // the subcarrier reference generation modulated it with.
  const CrosstalkParameters& crosstalk = chroma_error.crosstalk;
  const bool crosstalk_active = CrosstalkIsActive(chroma_error);
  const std::vector<LineTimingPrimitive> frame_lines =
      crosstalk_active ? BuildFrameTimingPrimitives(standard)
                       : std::vector<LineTimingPrimitive>{};
  const std::size_t sequence_period = ColourSequencePeriodFrames(standard);
  const bool pal_switch =
      standard == Standard::kPal || standard == Standard::kPalM;

  // Applies the error to one window of one frame. frame_base is the frame's
  // absolute sample position on the subcarrier lattice, y the frame's luma
  // (null without differential).
  auto apply = [&](SampleFixed* frame, const SampleFixed* y,
                   std::size_t sequence_phase, std::size_t frame_base,
                   const PictureLineWindow& window, double gain, double phase) {
    SampleFixed* line = frame + window.start;
    const std::size_t count = window.count;
    for (std::size_t n = 0; n < count; ++n) {
      chroma[n] = static_cast<double>(line[n]);
    }

    HilbertTransformWindow(hilbert_kernel_, chroma.data(), count,
                           quadrature.data());

    if (crosstalk_active) {
      // C = U sin(p) + W cos(p) with W = s V, s the line's V-switch sign, so
      // U = C sin(p) - H{C} cos(p) and W = C cos(p) + H{C} sin(p). Mixing
      // U' = U + a k_vu V and V' = V + a k_uv U in W terms gives the factor
      // a s: 1 when a follows the switch, s otherwise.
      const LineTimingPrimitive& timing =
          frame_lines[static_cast<std::size_t>(window.line_1based - 1)];
      const double v_switch =
          (pal_switch && PalInvertVAxisForLine(sequence_phase, timing)) ? -1.0
                                                                        : 1.0;
      const double factor =
          (crosstalk.alternate_with_switch && pal_switch) ? 1.0 : v_switch;
      const double v_to_u = factor * crosstalk.v_to_u;
      const double u_to_v = factor * crosstalk.u_to_v;
      const double offset = PictureChromaPhaseOffsetRad(standard, timing);
      for (std::size_t n = 0; n < count; ++n) {
        const double carrier =
            SubcarrierPhaseRad(frame_base + window.start + n) + offset;
        const double sin_p = std::sin(carrier);
        const double cos_p = std::cos(carrier);
        const double u = (chroma[n] * sin_p) - (quadrature[n] * cos_p);
        const double w = (chroma[n] * cos_p) + (quadrature[n] * sin_p);
        const double mixed_u = u + (v_to_u * w);
        const double mixed_w = w + (u_to_v * u);
        chroma[n] = (mixed_u * sin_p) + (mixed_w * cos_p);
        quadrature[n] = (mixed_w * sin_p) - (mixed_u * cos_p);
      }
    }

    double in_phase_gain = gain * std::cos(phase);
    double quadrature_gain = gain * std::sin(phase);
    for (std::size_t n = 0; n < count; ++n) {
      if (differential) {
        const double level_ire =
            (static_cast<double>(y[window.start + n]) - blanking_fixed) *
            ire_per_fixed;
        const double sample_gain =
            gain * InterpolateLevelCurve(chroma_error.differential.gain,
                                         level_ire, 1.0);
        const double sample_phase =
            phase + (InterpolateLevelCurve(chroma_error.differential.phase_deg,
                                           level_ire, 0.0) *
                     kPi / 180.0);
        in_phase_gain = sample_gain * std::cos(sample_phase);
        quadrature_gain = sample_gain * std::sin(sample_phase);
      }
      const double rotated =
          (in_phase_gain * chroma[n]) - (quadrature_gain * quadrature[n]);
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
    const SampleFixed* y =
        (differential && frame_start + samples_per_frame <= y_mv->size())
            ? y_mv->data() + frame_start
            : nullptr;
    if (differential && y == nullptr) {
      break;
    }
    // The frame's place in the colour sequence, as generation keys it.
    std::size_t disc_frame = global_frame;
    if (schedule[global_frame].enrichment != nullptr) {
      disc_frame = schedule[global_frame].enrichment->disc_frame_index;
    } else if (schedule[global_frame].disc_picture_number > 0) {
      disc_frame = static_cast<std::size_t>(
          schedule[global_frame].disc_picture_number - 1);
    }
    const std::size_t sequence_phase = disc_frame % sequence_period;
    const std::size_t frame_base =
        sequence_phase * static_cast<std::size_t>(samples_per_frame);

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
      apply(frame, y, sequence_phase, frame_base, windows[p], line_gain[p],
            line_phase_rad[p]);
    }
    for (std::size_t b = 0; b < burst_windows.size(); ++b) {
      const int p = picture_index_by_line[b];
      if (p < 0) {
        apply(frame, y, sequence_phase, frame_base, burst_windows[b],
              chroma_error.gain, phase_rad);
      } else {
        const auto picture = static_cast<std::size_t>(p);
        apply(frame, y, sequence_phase, frame_base, burst_windows[b],
              line_gain[picture], line_phase_rad[picture]);
      }
    }
  }
}

}  // namespace videosynth
