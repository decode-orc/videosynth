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

// Applies Project::laserdisc.channel and Project::channel to whole frames, in
// the order the signal meets them on the disc and in the player:
//   1. laserdisc.channel.fm_beat: the PAL FM-beat herringbone (FmBeatModel),
//      added to Y inside the active window of picture lines only. It is
//      computed from the frame's error-free Y and C, before any capture
//      impairment, so every capture of one pressing carries the same beat.
//   2. laserdisc.channel group_delay and response_db / response_by_picture:
//      one ReplayChannelFilter over Y and C of the whole frame, VITS
//      included. With response_by_picture it is designed per frame from the
//      frame's disc picture number.
//   3. channel.edge_smear: the readout smear (ApplyEdgeSmear) of Y and C over
//      the whole frame, VITS included.
//   4. channel.timing_offset_samples: a band-limited delay of Y and C over
//      the whole frame (sync, VBI and VITS included).
// Steps 2-4 wrap circularly within the frame. The frame boundary lies in
// vertical blanking, so the wrapped samples are sync and equalising pulses
// only. Each step clamps Y to the composite bounds and never clamps C.
// laserdisc.channel.field2_offset is a picture-domain impairment applied by
// generation, not here.
//
// The stage is a no-op when neither block configures anything but the
// identity, leaving the buffers bit-identical.
//
// Thread-safety: stateless; ApplyChannel is const and may be called
// concurrently on distinct buffers.
class ChannelStage {
 public:
  // True when the project's channel or laserdisc channel changes the signal.
  static bool IsActive(const Project& project);

  // True when the laserdisc channel's group delay or replay response is not
  // the identity.
  static bool ReplayFilterIsActive(const LaserdiscChannelParameters& channel);

  // Applies the channel impairments to a batch of frames in place.
  //
  // Args:
  //   project:      Parsed project supplying the channel block, standard and
  //                 encoding preset.
  //   schedule:     Frame schedule; supplies each frame's disc picture
  //                 number to response_by_picture.
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
