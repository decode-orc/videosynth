/*
 * File:        chroma_error_stage.h
 * Module:      chroma_error
 * Purpose:     Applies one capture's chroma gain and phase error to the
 *              picture chroma of fixed-point mV C frame buffers.
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
#include "videosynth/picture_chroma.h"

namespace videosynth {

// Applies Project::chroma_error to picture chroma. Inside the active window
// of each active picture line, C is replaced by g * (C cos t - H{C} sin t),
// which scales the subcarrier amplitude by g and advances its phase by t.
// H{C} is formed from the window's own samples only, so burst and blanking
// never leak into it. Burst, VITS target lines and VBI lines are untouched,
// so a burst-locked decoder sees a real hue and saturation error. With
// include_burst the burst window of every line is rotated as well (a picture
// line's burst by that line's own error), modelling a whole chroma path that
// a burst-referenced decoder can correct. C is
// zero-centred and is not clamped: saturated chroma may legitimately exceed
// the composite limits, and the output stage quantises it.
//
// t and g are the configured constants plus optional per-line Gaussian
// jitter. Chroma error is a capture property, so the jitter RNG is keyed by
// (seed, output frame index) and drawn line by line in frame order. The stage
// runs after the channel stage, so pressing impairments are computed from
// error-free chroma.
//
// The stage is a no-op when the project has no `chroma_error:` block or when
// it is the identity (gain 1, phase 0, no jitter), leaving the buffer
// bit-identical. SECAM is rejected by validation and skipped here.
//
// Thread-safety: immutable after construction; ApplyChromaError is const and
// may be called concurrently on distinct buffers.
class ChromaErrorStage {
 public:
  // Draws the per-run base seed used when line_jitter has no seed.
  ChromaErrorStage();
  explicit ChromaErrorStage(std::uint64_t run_base_seed);

  // True when the parameters change the signal.
  static bool IsActive(const ChromaErrorParameters& chroma_error);

  // Applies the chroma error to a batch of frames in place.
  //
  // Args:
  //   project:      Parsed project supplying the chroma_error block, standard
  //                 and project VITS lines.
  //   schedule:     Frame schedule; its indices are output frame indices.
  //   frame_offset: Index into schedule of the first frame in this batch.
  //   frame_count:  Number of frames in this batch.
  //   c_mv:         C-channel mV buffer (frame_count x samples_per_frame).
  void ApplyChromaError(
      const Project& project,
      const std::vector<IGenerationStage::FrameScheduleItem>& schedule,
      std::size_t frame_offset, std::size_t frame_count,
      std::vector<SampleFixed>* c_mv) const;

 private:
  std::uint64_t run_base_seed_;
  std::vector<double> hilbert_kernel_;
};

}  // namespace videosynth
