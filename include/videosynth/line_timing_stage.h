/*
 * File:        line_timing_stage.h
 * Module:      line_timing
 * Purpose:     Applies the per-line timing error left after time-base
 *              correction to the active picture of fixed-point mV Y/C frame
 *              buffers.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "videosynth/fixed_point.h"
#include "videosynth/interfaces.h"
#include "videosynth/model.h"
#include "videosynth/variable_delay_interpolator.h"

namespace videosynth {

// The timing error of one frame line: its shift (positive arrives later)
// and the difference between the shifts of its two halves, both in ns.
struct LineTimingError {
  double offset_ns = 0.0;
  double slope_ns = 0.0;
};

// Draws one layer's errors for every line of one frame (index line - 1).
// Each field is drawn independently, field 1 first: one unit normal for the
// field offset, then two per frame line of the field in line order (shift,
// then slope), drawn whether or not each figure is set, so changing one
// never moves another. Both per-line processes are stationary AR(1) along
// the field's lines with correlation line_correlation:
//   x[0] = s e[0],  x[k] = r x[k-1] + sqrt(1 - r^2) s e[k].
// The processes advance over every line of the field, picture or not, so the
// correlation is a function of the line spacing alone.
//
// Thread-safety: pure function; safe to call concurrently.
std::vector<LineTimingError> DrawLineTimingErrors(const LineTimingLayer& layer,
                                                  Standard standard,
                                                  std::uint64_t seed);

// Applies Project::line_timing to the active picture. Inside the active
// window of each picture line (BuildPictureLineWindows: no VBI, no VITS
// target lines), Y and C are replaced by the line read at
//   n - d(n),  d(n) = offset + slope (n - c) / (W / 2),
// with c the centre and W the width of a full active line, so the shift
// grows linearly along the line and its two halves' centres differ by slope.
// The read is band-limited (VariableDelayInterpolator) and samples just
// outside the window are read as they are, so the picture's edges move into
// blanking. Sync, burst, VBI and VITS stay where they are; with include_burst
// the burst window of each picture line (C only: Y is flat blanking there)
// takes the same d(n).
//
// The two layers add: the capture layer is keyed by (seed, output frame
// index), the shared (pressing) layer by (seed, disc frame index), each with
// its own seed domain. The stage runs after the channel and chroma error
// stages and before noise, so noise is not shifted; the picture truth, drawn
// from generation, stays the unjittered picture. Y is clamped to the
// composite bounds and C is not clamped.
//
// The stage is a no-op when the project has no `line_timing:` block or every
// figure is zero, leaving the buffers bit-identical.
//
// Thread-safety: immutable after construction; ApplyLineTiming is const and
// may be called concurrently on distinct buffers.
class LineTimingStage {
 public:
  // Draws the per-run base seed used when the capture layer has no seed.
  LineTimingStage();
  explicit LineTimingStage(std::uint64_t run_base_seed);

  // True when the parameters change the signal.
  static bool IsActive(const LineTimingParameters& line_timing);
  // True when the layer is present with a non-zero figure.
  static bool LayerIsActive(const LineTimingLayer& layer);

  // The summed errors of both layers for every line of output frame
  // global_frame (index line - 1).
  std::vector<LineTimingError> FrameErrors(
      const Project& project,
      const std::vector<IGenerationStage::FrameScheduleItem>& schedule,
      std::size_t global_frame) const;

  // Applies the line timing error to a batch of frames in place.
  //
  // Args:
  //   project:      Parsed project supplying the line_timing block, standard,
  //                 encoding preset and project VITS lines.
  //   schedule:     Frame schedule; its indices are output frame indices and
  //                 its enrichment supplies the disc frame index.
  //   frame_offset: Index into schedule of the first frame in this batch.
  //   frame_count:  Number of frames in this batch.
  //   y_mv:         Y-channel mV buffer (frame_count x samples_per_frame).
  //   c_mv:         C-channel mV buffer (frame_count x samples_per_frame).
  void ApplyLineTiming(
      const Project& project,
      const std::vector<IGenerationStage::FrameScheduleItem>& schedule,
      std::size_t frame_offset, std::size_t frame_count,
      std::vector<SampleFixed>* y_mv, std::vector<SampleFixed>* c_mv) const;

 private:
  std::uint64_t run_base_seed_;
  VariableDelayInterpolator interpolator_;
};

}  // namespace videosynth
