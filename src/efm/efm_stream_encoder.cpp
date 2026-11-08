/*
 * File:        efm_stream_encoder.cpp
 * Module:      efm
 * Purpose:     Public interface of the EFM module: turns 16-bit stereo PCM and
 *              a track table into the T-value channel stream of a LaserDisc
 *              digital audio track.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "videosynth/efm/efm_stream_encoder.h"

#include <utility>

namespace videosynth::efm {

bool EfmStreamEncoder::Begin(const TrackTable& table) {
  SubcodeGenerator generator;
  if (!generator.Begin(table)) {
    return false;
  }

  Reset();
  subcode_generator_ = std::move(generator);
  begun_ = true;
  return true;
}

void EfmStreamEncoder::Reset() {
  assembler_.Reset();
  circ_encoder_.Reset();
  subcode_generator_.Reset();
  modulator_.Reset();
  t_value_encoder_.Reset();
  section_ = SubcodeSection{};
  cached_section_index_ = 0;
  section_cached_ = false;
  frame_index_ = 0;
  begun_ = false;
}

std::uint16_t EfmStreamEncoder::ControlSymbol(std::size_t frame_index) {
  const std::size_t section_index = frame_index / kFramesPerSubcodeSection;
  const std::size_t frame_in_section = frame_index % kFramesPerSubcodeSection;

  // IEC 60908-1999, 17.3: the control bytes of the first two frames of a
  // section are replaced by the sync patterns S0 and S1, which lie outside the
  // eight-to-fourteen table.
  if (frame_in_section == 0) {
    return kSubcodeSyncS0;
  }
  if (frame_in_section == 1) {
    return kSubcodeSyncS1;
  }

  if (!section_cached_ || cached_section_index_ != section_index) {
    // Sections past the end of the track table carry no P or Q data: the
    // flushed CIRC pipeline extends the stream beyond the described timeline.
    if (!subcode_generator_.GenerateSection(section_index, &section_)) {
      section_ = SubcodeSection{};
    }
    cached_section_index_ = section_index;
    section_cached_ = true;
  }
  return EfmSymbol(section_.ControlByte(frame_in_section));
}

bool EfmStreamEncoder::EmitF2Frame(const F2Frame& f2_frame,
                                   std::vector<std::uint8_t>* t_values) {
  std::vector<bool> channel_bits;
  channel_bits.reserve(kChannelBitsPerFrame);
  if (!modulator_.ModulateFrame(ControlSymbol(frame_index_), f2_frame,
                                &channel_bits)) {
    return false;
  }
  if (!t_value_encoder_.PushBits(channel_bits, t_values)) {
    return false;
  }
  ++frame_index_;
  return true;
}

bool EfmStreamEncoder::EmitFrames(const std::vector<F1Frame>& frames,
                                  std::vector<std::uint8_t>* t_values) {
  for (const F1Frame& frame : frames) {
    if (!EmitF2Frame(circ_encoder_.EncodeFrame(frame), t_values)) {
      return false;
    }
  }
  return true;
}

bool EfmStreamEncoder::PushSamples(const std::vector<std::int16_t>& left,
                                   const std::vector<std::int16_t>& right,
                                   std::vector<std::uint8_t>* t_values) {
  if (!begun_ || t_values == nullptr) {
    return false;
  }

  std::vector<F1Frame> frames;
  if (!assembler_.PushSamples(left, right, &frames)) {
    return false;
  }
  return EmitFrames(frames, t_values);
}

bool EfmStreamEncoder::PushF1Frames(const F1Frame* frames, std::size_t count,
                                    std::vector<std::uint8_t>* t_values) {
  if (!begun_ || t_values == nullptr || (frames == nullptr && count > 0) ||
      assembler_.PendingSampleCount() != 0) {
    return false;
  }
  for (std::size_t index = 0; index < count; ++index) {
    if (!EmitF2Frame(circ_encoder_.EncodeFrame(frames[index]), t_values)) {
      return false;
    }
  }
  return true;
}

bool EfmStreamEncoder::SkipFrames(std::size_t count,
                                  std::vector<std::uint8_t>* t_values) {
  if (!begun_ || t_values == nullptr || assembler_.PendingSampleCount() != 0) {
    return false;
  }
  if (count == 0) {
    return true;
  }
  // The last run has no closing transition; emit it before the signal stops.
  if (!t_value_encoder_.Flush(t_values)) {
    return false;
  }
  circ_encoder_.Reset();
  modulator_.Reset();
  t_value_encoder_.Reset();
  frame_index_ += count;
  return true;
}

bool EfmStreamEncoder::Flush(std::vector<std::uint8_t>* t_values) {
  if (!begun_ || t_values == nullptr) {
    return false;
  }

  std::vector<F1Frame> frames;
  if (!assembler_.Flush(&frames)) {
    return false;
  }
  if (!EmitFrames(frames, t_values)) {
    return false;
  }

  // ECMA-130, C.9: the CIRC interleave spreads a frame over the following 108
  // output frames, and a mirror-delay decoder needs kCircDrainFrames of stream
  // beyond it, so the pipeline is flushed with digital silence before the
  // stream ends.
  std::vector<F2Frame> flushed;
  if (!circ_encoder_.Flush(&flushed)) {
    return false;
  }
  for (const F2Frame& f2_frame : flushed) {
    if (!EmitF2Frame(f2_frame, t_values)) {
      return false;
    }
  }

  return t_value_encoder_.Flush(t_values);
}

}  // namespace videosynth::efm
