/*
 * File:        channel_stage.cpp
 * Module:      channel
 * Purpose:     Applies the project-wide channel (pressing) impairments to
 *              fixed-point mV Y/C frame buffers between generation and
 *              noise injection.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "videosynth/channel_stage.h"

#include <algorithm>
#include <cmath>
#include <optional>

#include "videosynth/edge_smear_filter.h"
#include "videosynth/fm_beat_model.h"
#include "videosynth/picture_chroma.h"
#include "videosynth/sample_clamp.h"
#include "videosynth/timing_constants.h"
#include "videosynth/timing_offset_filter.h"

namespace videosynth {

bool ChannelStage::IsActive(const ChannelParameters& channel) {
  return channel.enabled &&
         (channel.timing_offset_samples != 0.0 || channel.fm_beat.enabled ||
          (channel.edge_smear.enabled && channel.edge_smear.fraction != 0.0));
}

void ChannelStage::ApplyChannel(
    const Project& project,
    const std::vector<IGenerationStage::FrameScheduleItem>& /*schedule*/,
    std::size_t /*frame_offset*/, std::size_t frame_count,
    std::vector<SampleFixed>* y_mv, std::vector<SampleFixed>* c_mv) const {
  const ChannelParameters& channel = project.channel;
  if (!IsActive(channel) || y_mv == nullptr || c_mv == nullptr) {
    return;
  }

  const Standard standard = project.cvbs_presets.video_standard_preset;
  // Generation always fills 4fsc frames; output resampling comes later.
  const auto samples_per_frame =
      static_cast<std::size_t>(SamplesPerFrame4fsc(standard));
  const TimingConstants timing = GetTimingConstants(standard);
  const SampleClampBounds bounds = SampleClampBoundsFor(standard);
  // The bounds are composite limits. Y carries sync and blanking, so each
  // step's excursions are held inside them; C is zero-centred, and saturated
  // chroma legitimately exceeds them, so it is left for the output stage to
  // quantise.
  const auto clamp_luma = [&bounds](SampleFixed* frame, std::size_t count) {
    for (std::size_t s = 0; s < count; ++s) {
      frame[s] = std::clamp(frame[s], bounds.min, bounds.max);
    }
  };

  // Validation restricts the beat to 625-line PAL.
  const bool beat = channel.fm_beat.enabled && standard == Standard::kPal;
  std::vector<PictureLineWindow> windows;
  std::optional<FmBeatModel> beat_model;
  double beat_scale = 0.0;
  if (beat) {
    windows = BuildPictureLineWindows(project, standard);
    const SignalLevels levels = GetSignalLevels(project.cvbs_presets);
    beat_model.emplace(timing.sample_rate_4fsc_hz,
                       channel.fm_beat.carrier_black_hz, levels.black_mv,
                       levels.white_mv);
    beat_scale = beat_model->LevelScale(channel.fm_beat.level_db);
  }
  const bool smear =
      channel.edge_smear.enabled && channel.edge_smear.fraction != 0.0;
  const bool offset = channel.timing_offset_samples != 0.0;
  const TimingOffsetFilter offset_filter(channel.timing_offset_samples);
  std::vector<double> basis;
  std::vector<SampleFixed> delayed(offset ? samples_per_frame : 0U);

  for (std::size_t i = 0; i < frame_count; ++i) {
    const std::size_t frame_start = i * samples_per_frame;
    if (frame_start + samples_per_frame > y_mv->size() ||
        frame_start + samples_per_frame > c_mv->size()) {
      break;
    }
    SampleFixed* y = y_mv->data() + frame_start;
    SampleFixed* c = c_mv->data() + frame_start;

    // The beat is on the disc, so the readout smear and the sampling offset
    // apply to it.
    if (beat) {
      beat_model->ComputeBasis(y, c, samples_per_frame, windows, &basis);
      for (const PictureLineWindow& window : windows) {
        for (std::size_t s = window.start; s < window.start + window.count;
             ++s) {
          y[s] = std::clamp(y[s] + static_cast<SampleFixed>(std::llround(
                                       beat_scale * basis[s] *
                                       static_cast<double>(kSampleScale))),
                            bounds.min, bounds.max);
        }
      }
    }

    if (smear) {
      ApplyEdgeSmear(channel.edge_smear.fraction,
                     channel.edge_smear.tau_samples, y, samples_per_frame);
      ApplyEdgeSmear(channel.edge_smear.fraction,
                     channel.edge_smear.tau_samples, c, samples_per_frame);
      clamp_luma(y, samples_per_frame);
    }

    if (offset) {
      offset_filter.Apply(y, samples_per_frame, delayed.data());
      std::copy(delayed.begin(), delayed.end(), y);
      clamp_luma(y, samples_per_frame);
      offset_filter.Apply(c, samples_per_frame, delayed.data());
      std::copy(delayed.begin(), delayed.end(), c);
    }
  }
}

}  // namespace videosynth
