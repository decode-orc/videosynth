/*
 * File:        line_timing_stage.cpp
 * Module:      line_timing
 * Purpose:     Applies the per-line timing error left after time-base
 *              correction to the active picture of fixed-point mV Y/C frame
 *              buffers.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "videosynth/line_timing_stage.h"

#include <algorithm>
#include <cmath>
#include <random>

#include "videosynth/frame_enrichment.h"
#include "videosynth/frame_line_layout.h"
#include "videosynth/impairment_seed.h"
#include "videosynth/picture_chroma.h"
#include "videosynth/sample_clamp.h"
#include "videosynth/signal_timing_model.h"
#include "videosynth/timing_constants.h"

namespace videosynth {

namespace {

constexpr double kSecondsPerNs = 1.0e-9;

// Rewrites window [start, start + count) of frame from source (an unaltered
// copy of the frame), reading each sample at n - d(n).
void ShiftWindow(const VariableDelayInterpolator& interpolator,
                 const SampleFixed* source, std::size_t frame_samples,
                 const PictureLineWindow& window, double line_start,
                 double centre, double offset_samples, double slope_per_sample,
                 SampleFixed* frame) {
  for (std::size_t n = window.start; n < window.start + window.count; ++n) {
    const double position = static_cast<double>(n);
    const double delay =
        offset_samples + (slope_per_sample * (position - line_start - centre));
    frame[n] = static_cast<SampleFixed>(
        std::llround(interpolator.At(source, frame_samples, position - delay)));
  }
}

}  // namespace

std::vector<LineTimingError> DrawLineTimingErrors(const LineTimingLayer& layer,
                                                  Standard standard,
                                                  std::uint64_t seed) {
  const TimingConstants timing = GetTimingConstants(standard);
  const auto lines = static_cast<std::size_t>(timing.lines_per_frame);
  std::vector<LineTimingError> errors(lines);

  std::mt19937_64 rng(seed);
  std::normal_distribution<double> unit_normal(0.0, 1.0);
  const double correlation = layer.line_correlation;
  const double innovation =
      std::sqrt(std::max(0.0, 1.0 - (correlation * correlation)));
  for (int field = 1; field <= 2; ++field) {
    const double field_offset = layer.field_ns * unit_normal(rng);
    bool first = true;
    double shift = 0.0;
    double slope = 0.0;
    for (std::size_t index = 0; index < lines; ++index) {
      if (GetFieldIndex(standard, static_cast<int>(index) + 1) != field) {
        continue;
      }
      const double shift_draw = unit_normal(rng);
      const double slope_draw = unit_normal(rng);
      if (first) {
        shift = layer.line_ns * shift_draw;
        slope = layer.slope_ns * slope_draw;
        first = false;
      } else {
        shift =
            (correlation * shift) + (innovation * layer.line_ns * shift_draw);
        slope =
            (correlation * slope) + (innovation * layer.slope_ns * slope_draw);
      }
      errors[index] =
          LineTimingError{.offset_ns = field_offset + shift, .slope_ns = slope};
    }
  }
  return errors;
}

LineTimingStage::LineTimingStage() : LineTimingStage(std::random_device{}()) {}

LineTimingStage::LineTimingStage(std::uint64_t run_base_seed)
    : run_base_seed_(run_base_seed) {}

bool LineTimingStage::LayerIsActive(const LineTimingLayer& layer) {
  return layer.enabled && (layer.line_ns != 0.0 || layer.field_ns != 0.0 ||
                           layer.slope_ns != 0.0);
}

bool LineTimingStage::IsActive(const LineTimingParameters& line_timing) {
  return line_timing.enabled && (LayerIsActive(line_timing.capture) ||
                                 LayerIsActive(line_timing.shared));
}

std::vector<LineTimingError> LineTimingStage::FrameErrors(
    const Project& project,
    const std::vector<IGenerationStage::FrameScheduleItem>& schedule,
    std::size_t global_frame) const {
  const LineTimingParameters& line_timing = project.line_timing;
  const Standard standard = project.cvbs_presets.video_standard_preset;
  const auto lines =
      static_cast<std::size_t>(GetTimingConstants(standard).lines_per_frame);
  std::vector<LineTimingError> errors(lines);

  // The capture layer belongs to one capture: it follows the output frame.
  if (LayerIsActive(line_timing.capture)) {
    const std::uint64_t seed =
        line_timing.capture.seed_specified
            ? static_cast<std::uint64_t>(line_timing.capture.seed)
            : run_base_seed_;
    errors = DrawLineTimingErrors(
        line_timing.capture, standard,
        DeriveImpairmentSeed(seed, kLineTimingSeedDomain, global_frame));
  }

  // The shared layer is recorded on the disc: it follows the disc position,
  // as the shared noise layer does.
  if (LayerIsActive(line_timing.shared)) {
    std::size_t disc_frame = global_frame;
    if (global_frame < schedule.size() &&
        schedule[global_frame].enrichment != nullptr) {
      disc_frame = schedule[global_frame].enrichment->disc_frame_index;
    }
    const std::vector<LineTimingError> shared = DrawLineTimingErrors(
        line_timing.shared, standard,
        DeriveImpairmentSeed(
            static_cast<std::uint64_t>(line_timing.shared.seed),
            kSharedLineTimingSeedDomain, disc_frame));
    for (std::size_t i = 0; i < lines; ++i) {
      errors[i].offset_ns += shared[i].offset_ns;
      errors[i].slope_ns += shared[i].slope_ns;
    }
  }
  return errors;
}

void LineTimingStage::ApplyLineTiming(
    const Project& project,
    const std::vector<IGenerationStage::FrameScheduleItem>& schedule,
    std::size_t frame_offset, std::size_t frame_count,
    std::vector<SampleFixed>* y_mv, std::vector<SampleFixed>* c_mv) const {
  const LineTimingParameters& line_timing = project.line_timing;
  if (!IsActive(line_timing) || y_mv == nullptr || c_mv == nullptr) {
    return;
  }

  const Standard standard = project.cvbs_presets.video_standard_preset;
  // Generation always fills 4fsc frames; output resampling comes later.
  const auto samples_per_frame =
      static_cast<std::size_t>(SamplesPerFrame4fsc(standard));
  const TimingConstants timing = GetTimingConstants(standard);
  const SampleClampBounds bounds = SampleClampBoundsFor(standard);
  const std::vector<int> line_samples = BuildLineSampleCounts(
      standard, timing.lines_per_frame, timing.samples_per_line_4fsc);
  const std::vector<int> line_offsets = BuildLineSampleOffsets(line_samples);
  const ActiveRasterGeometry geometry =
      GetActiveRasterGeometry(standard, timing.sample_rate_4fsc_hz);
  const std::vector<PictureLineWindow> windows =
      BuildPictureLineWindows(project, standard);

  // The velocity term is referred to a full active line, so a half picture
  // line takes the same slope as its neighbours.
  const double active_width =
      static_cast<double>(geometry.active_window_end_samples -
                          geometry.active_window_start_samples);
  const double centre =
      static_cast<double>(geometry.active_window_start_samples) +
      (active_width / 2.0);
  const double samples_per_ns = timing.sample_rate_4fsc_hz * kSecondsPerNs;

  // With include_burst, the burst window of each picture line moves too.
  std::vector<PictureLineWindow> burst_windows;
  if (line_timing.include_burst) {
    const std::vector<PictureLineWindow> all_bursts =
        BuildBurstLineWindows(standard);
    for (const PictureLineWindow& window : windows) {
      burst_windows.push_back(
          all_bursts[static_cast<std::size_t>(window.line_1based - 1)]);
    }
  }

  std::vector<SampleFixed> source(samples_per_frame);
  for (std::size_t i = 0; i < frame_count; ++i) {
    const std::size_t global_frame = frame_offset + i;
    const std::size_t frame_start = i * samples_per_frame;
    if (frame_start + samples_per_frame > y_mv->size() ||
        frame_start + samples_per_frame > c_mv->size()) {
      break;
    }
    const std::vector<LineTimingError> errors =
        FrameErrors(project, schedule, global_frame);

    for (SampleFixed* frame :
         {y_mv->data() + frame_start, c_mv->data() + frame_start}) {
      const bool luma = frame == y_mv->data() + frame_start;
      std::copy(frame, frame + samples_per_frame, source.begin());
      for (std::size_t p = 0; p < windows.size(); ++p) {
        const PictureLineWindow& window = windows[p];
        const auto line_index =
            static_cast<std::size_t>(window.line_1based - 1);
        const LineTimingError& error = errors[line_index];
        const double offset = error.offset_ns * samples_per_ns;
        const double slope =
            error.slope_ns * samples_per_ns / (active_width / 2.0);
        const auto line_start = static_cast<double>(line_offsets[line_index]);
        ShiftWindow(interpolator_, source.data(), samples_per_frame, window,
                    line_start, centre, offset, slope, frame);
        if (luma) {
          for (std::size_t n = window.start; n < window.start + window.count;
               ++n) {
            frame[n] = std::clamp(frame[n], bounds.min, bounds.max);
          }
        } else if (!burst_windows.empty()) {
          ShiftWindow(interpolator_, source.data(), samples_per_frame,
                      burst_windows[p], line_start, centre, offset, slope,
                      frame);
        }
      }
    }
  }
}

}  // namespace videosynth
