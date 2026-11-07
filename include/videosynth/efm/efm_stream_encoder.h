/*
 * File:        efm_stream_encoder.h
 * Module:      efm
 * Purpose:     Public interface of the EFM module: turns 16-bit stereo PCM and
 *              a track table into the T-value channel stream of a LaserDisc
 *              digital audio track.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "videosynth/efm/audio_frame_assembler.h"
#include "videosynth/efm/circ_encoder.h"
#include "videosynth/efm/efm_modulator.h"
#include "videosynth/efm/subcode_generator.h"

namespace videosynth::efm {

// Chains the stages of the module: samples are grouped into F1 frames, CIRC
// encoded into F2 frames, given the control byte of the subcode section they
// fall in, modulated into 588-bit channel frames and emitted as T values.
//
// Streaming contract: Begin(table) -> repeated PushSamples -> Flush. Each call
// appends the T values that the newly available channel bits complete;
// PushSamples emits nothing until a whole F1 frame is available. Reset restores
// the constructed state.
//
// Timing alignment: the CIRC delay registers start at digital silence, so
// source sample 0 shares its datum with subcode absolute time 00:00:00. A
// de-interleaving decoder therefore emits warm-up silence before source
// sample 0: kCircPipelineLatencyFrames frames for a backward-reading
// de-interleaver, kCircDrainFrames for a mirror-delay one (see the EFM
// implementation plan, Timing Alignment Contract).
//
// Error reporting: the module reports failure by return value and never throws
// across its public API.
//
// Thread-safety: EfmStreamEncoder is NOT thread-safe. It owns the state of
// every stage and must be driven from a single thread.
class EfmStreamEncoder {
 public:
  // Adopts `table` as the subcode layout of the stream and restores every stage
  // to its constructed state. Returns false, leaving the encoder unchanged,
  // when the track table is rejected by SubcodeGenerator::Begin.
  bool Begin(const TrackTable& table);

  // Appends interleaved stereo samples, appending the T values of every channel
  // frame the new samples complete to `t_values` (which is never cleared).
  // Returns false when Begin has not succeeded, the buffers differ in length or
  // `t_values` is null; in that case no samples are consumed.
  bool PushSamples(const std::vector<std::int16_t>& left,
                   const std::vector<std::int16_t>& right,
                   std::vector<std::uint8_t>* t_values);

  // Appends whole F1 frames (e.g. the frames of a data sector, see
  // data_sector.h), appending the T values of the channel frames they complete
  // to `t_values`. Returns false when Begin has not succeeded, `t_values` is
  // null, or stereo samples are pending in a partial frame.
  bool PushF1Frames(const F1Frame* frames, std::size_t count,
                    std::vector<std::uint8_t>* t_values);

  // Interrupts the channel stream for `count` channel frames, as where a disc
  // carries no EFM signal: emits the run still in progress, then advances the
  // subcode timeline by `count` frames without emitting anything, and restarts
  // the CIRC encoder, modulator and T-value encoder. A restarted CIRC encoder
  // is identical to one that has encoded kCircPipelineLatencyFrames frames of
  // silence, so the interleave across the gap is seamless; symbols of frames
  // pushed in the last kCircDrainFrames before the gap are lost with it.
  // Returns false when Begin has not succeeded, `t_values` is null, or stereo
  // samples are pending in a partial frame.
  bool SkipFrames(std::size_t count, std::vector<std::uint8_t>* t_values);

  // Pads the pending F1 frame with digital silence, flushes the CIRC pipeline
  // so every pushed sample is fully represented in the output, and emits the
  // final run length. Returns false when Begin has not succeeded or `t_values`
  // is null.
  bool Flush(std::vector<std::uint8_t>* t_values);

  // Number of channel frames emitted so far.
  std::size_t ChannelFrameCount() const { return frame_index_; }

  // Restores the encoder to its constructed state, discarding the track table.
  void Reset();

 private:
  // Modulates one F2 frame and appends the T values it completes to
  // `t_values`.
  bool EmitF2Frame(const F2Frame& f2_frame,
                   std::vector<std::uint8_t>* t_values);

  // CIRC encodes `frames` and emits the resulting F2 frames.
  bool EmitFrames(const std::vector<F1Frame>& frames,
                  std::vector<std::uint8_t>* t_values);

  // The 14 channel bits of the first byte of channel frame `frame_index`: the
  // sync patterns S0 and S1 for the first two frames of a subcode section and
  // the eight-to-fourteen translation of the control byte otherwise
  // (IEC 60908-1999, 17.3).
  std::uint16_t ControlSymbol(std::size_t frame_index);

  AudioFrameAssembler assembler_;
  CircEncoder circ_encoder_;
  SubcodeGenerator subcode_generator_;
  EfmModulator modulator_;
  TValueEncoder t_value_encoder_;

  // Subcode section cached across the 98 frames that share it. Sections beyond
  // the end of the track table carry no P or Q data.
  SubcodeSection section_{};
  std::size_t cached_section_index_ = 0;
  bool section_cached_ = false;

  std::size_t frame_index_ = 0;
  bool begun_ = false;
};

}  // namespace videosynth::efm
