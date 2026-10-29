/*
 * File:        channel_stage.h
 * Module:      channel
 * Purpose:     Applies the project-wide channel (pressing) impairments to
 *              fixed-point mV Y/C frame buffers between generation and
 *              noise injection.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <cstddef>
#include <vector>

#include "videosynth/fixed_point.h"
#include "videosynth/interfaces.h"
#include "videosynth/model.h"

namespace videosynth {

// Applies Project::channel to whole frames, in the order the signal meets
// them on the disc and in the player:
//   1. fm_beat: the PAL FM-beat herringbone (FmBeatModel), added to Y inside
//      the active window of picture lines only. It is computed from the
//      frame's error-free Y and C, before any capture impairment, so every
//      capture of one pressing carries the same beat.
//   2. edge_smear: the readout smear (ApplyEdgeSmear) of Y and C over the
//      whole frame, VITS included.
//   3. timing_offset_samples: a band-limited delay of Y and C over the whole
//      frame (sync, VBI and VITS included).
// The smear and the delay wrap circularly within the frame. The frame
// boundary lies in vertical blanking, so the wrapped samples are sync and
// equalising pulses only. Each step clamps Y to the composite bounds and
// never clamps C.
//
// The stage is a no-op when the project has no `channel:` block or when
// every configured impairment is the identity, leaving the buffers
// bit-identical.
//
// Thread-safety: stateless; ApplyChannel is const and may be called
// concurrently on distinct buffers.
class ChannelStage {
 public:
  // True when the parameters change the signal.
  static bool IsActive(const ChannelParameters& channel);

  // Applies the channel impairments to a batch of frames in place.
  //
  // Args:
  //   project:      Parsed project supplying the channel block, standard and
  //                 encoding preset.
  //   schedule:     Frame schedule; unused, as every channel impairment is
  //                 a function of the frame's own samples.
  //   frame_offset: Index into schedule of the first frame in this batch.
  //   frame_count:  Number of frames in this batch.
  //   y_mv:         Y-channel mV buffer (frame_count x samples_per_frame).
  //   c_mv:         C-channel mV buffer (frame_count x samples_per_frame).
  void ApplyChannel(
      const Project& project,
      const std::vector<IGenerationStage::FrameScheduleItem>& schedule,
      std::size_t frame_offset, std::size_t frame_count,
      std::vector<SampleFixed>* y_mv, std::vector<SampleFixed>* c_mv) const;
};

}  // namespace videosynth
