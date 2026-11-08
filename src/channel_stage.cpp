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
#include "videosynth/frame_enrichment.h"
#include "videosynth/picture_chroma.h"
#include "videosynth/replay_channel_filter.h"
#include "videosynth/sample_clamp.h"
#include "videosynth/timing_constants.h"
#include "videosynth/timing_offset_filter.h"

namespace videosynth {

bool ChannelStage::IsActive(const Project& project) {
  const ChannelParameters& channel = project.channel;
  const bool generic =
      channel.enabled &&
      (channel.timing_offset_samples != 0.0 ||
       (channel.edge_smear.enabled && channel.edge_smear.fraction != 0.0));
  const LaserdiscChannelParameters& replay = project.laserdisc.channel;
  return generic || (replay.enabled && replay.fm_beat.enabled) ||
         ReplayFilterIsActive(replay);
}

bool ChannelStage::ReplayFilterIsActive(
    const LaserdiscChannelParameters& channel) {
  if (!channel.enabled) {
    return false;
  }
  const GroupDelayParameters& group_delay = channel.group_delay;
  bool delayed = false;
  if (group_delay.enabled && group_delay.scale != 0.0) {
    for (const GroupDelayPoint& point : GroupDelayCurve(group_delay)) {
      delayed = delayed || point.delay_ns != 0.0;
    }
  }
  bool shaped = false;
  for (const NoiseSpectrumPoint& point : channel.response_db) {
    shaped = shaped || point.relative_db != 0.0;
  }
  for (const ResponseByPictureEntry& entry : channel.response_by_picture) {
    for (const NoiseSpectrumPoint& point : entry.response_db) {
      shaped = shaped || point.relative_db != 0.0;
    }
  }
  return delayed || shaped;
}

void ChannelStage::ApplyChannel(
    const Project& project,
    const std::vector<IGenerationStage::FrameScheduleItem>& schedule,
    std::size_t frame_offset, std::size_t frame_count,
    std::vector<SampleFixed>* y_mv, std::vector<SampleFixed>* c_mv) const {
  const ChannelParameters& channel = project.channel;
  const LaserdiscChannelParameters& replay = project.laserdisc.channel;
  if (!IsActive(project) || y_mv == nullptr || c_mv == nullptr) {
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
  const bool beat =
      replay.enabled && replay.fm_beat.enabled && standard == Standard::kPal;
  std::vector<PictureLineWindow> windows;
  std::optional<FmBeatModel> beat_model;
  double beat_scale = 0.0;
  if (beat) {
    windows = BuildPictureLineWindows(project, standard);
    const SignalLevels levels = GetSignalLevels(project.cvbs_presets);
    beat_model.emplace(timing.sample_rate_4fsc_hz,
                       replay.fm_beat.carrier_black_hz, levels.black_mv,
                       levels.white_mv);
    beat_scale = beat_model->LevelScale(replay.fm_beat.level_db);
  }

  // The replay filter is fixed unless the response varies with disc
  // position, in which case it is designed per frame from the picture number.
  const bool replay_filter = ReplayFilterIsActive(replay);
  const bool per_picture = replay_filter && !replay.response_by_picture.empty();
  std::vector<GroupDelayPoint> delay_curve;
  if (replay.group_delay.enabled) {
    delay_curve = GroupDelayCurve(replay.group_delay);
  }
  const double delay_scale =
      replay.group_delay.enabled ? replay.group_delay.scale : 0.0;
  const auto delay_ns = [&delay_curve, delay_scale](double mhz) {
    return delay_scale * InterpolateGroupDelayNs(delay_curve, mhz);
  };
  const auto design_filter = [&](double picture) {
    return ReplayChannelFilter(
        [&replay, picture](double mhz) {
          return ReplayResponseDb(replay, picture, mhz);
        },
        delay_ns, timing.sample_rate_4fsc_hz);
  };
  std::optional<ReplayChannelFilter> fixed_filter;
  if (replay_filter && !per_picture) {
    fixed_filter.emplace(design_filter(0.0));
  }

  const bool smear = channel.enabled && channel.edge_smear.enabled &&
                     channel.edge_smear.fraction != 0.0;
  const bool offset = channel.enabled && channel.timing_offset_samples != 0.0;
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

    // The replay channel reads the disc, so it sees the beat; the player's
    // readout smear and the capture's sampling offset follow it.
    if (replay_filter) {
      std::optional<ReplayChannelFilter> frame_filter;
      if (per_picture) {
        // The picture number is the disc position: the CAV picture number
        // when the frame's section anchors one, otherwise the output
        // position, as the shared noise layer keys it.
        const std::size_t global_frame = frame_offset + i;
        std::size_t disc_frame = global_frame;
        if (global_frame < schedule.size() &&
            schedule[global_frame].enrichment != nullptr) {
          disc_frame = schedule[global_frame].enrichment->DiscPosition();
        }
        frame_filter.emplace(
            design_filter(static_cast<double>(disc_frame) + 1.0));
      }
      const ReplayChannelFilter& filter =
          per_picture ? *frame_filter : *fixed_filter;
      filter.Apply(y, samples_per_frame);
      clamp_luma(y, samples_per_frame);
      filter.Apply(c, samples_per_frame);
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
