/*
 * File:        efm_track_layout.h
 * Module:      efm_track_layout
 * Purpose:     Maps a project's output section layout onto the subcode track
 *              table of the LaserDisc digital audio (EFM) stream.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "videosynth/efm/audio_frame_assembler.h"
#include "videosynth/efm/subcode_generator.h"
#include "videosynth/model.h"

namespace videosynth {

// Stereo sampling periods spanned by one subcode section: six sampling periods
// per frame (IEC 60908-1999, clause 14) and 98 frames per subcode section
// (IEC 60908-1999, 17.3), i.e. 1/75 s at 44 100 Hz.
inline constexpr std::size_t kEfmSamplesPerSubcodeSection =
    efm::kStereoSamplesPerF1Frame * efm::kFramesPerSubcodeSection;

// The subcode layout of one generated stream together with the sample window
// the encoder must mute.
struct EfmTrackLayout {
  // Track table tiling the stream, in output order.
  efm::TrackTable table;

  // Half-open range of 44.1 kHz stereo sampling periods, counted from the first
  // stored output frame, covering the mandatory pause that precedes track 1
  // (IEC 60908-1999, 17.5.1). The EFM audio of that window is digital silence
  // while the 48 kHz WAV path carries the section's tone. The range is empty
  // when the project has no programme area.
  std::size_t pause_start_sample = 0;
  std::size_t pause_end_sample = 0;
};

// How the project's programme-area sections map onto subcode tracks.
struct EfmTrackLayoutOptions {
  // When true, every programme-area run belongs to track 01: the layout of a
  // single digital data track (ECMA-130 clause 20). When false, each run is
  // its own track, as for digital audio.
  bool single_programme_track = false;

  // When set and the stream opens with a lead-in followed by the programme
  // area, the subcode section at which the programme area (and the pause
  // before track 1) starts, in place of the first programme-area frame: the
  // programme area takes over the lead-in's sections from there on. A start
  // after the first programme-area frame is moved back to it. Unset, the
  // areas follow the sections.
  std::optional<std::int64_t> programme_start_section;
};

// Builds the track table for `output_frame_sections` — the section shown by
// each stored output frame, in output order, as passed to
// AudioTrackGenerator::Begin.
//
// Each contiguous run of output frames sharing one programme-area section
// becomes one track, numbered sequentially from 01 in output order
// (independently of any chapter_number biphase codes); adjacent lead-in and
// lead-out runs are merged into a single area entry. Sections with no declared
// type are treated as programme area. Boundaries are placed on the nearest
// subcode section, so a track boundary sits within 1/150 s of the video frame
// boundary that carries it.
//
// Returns false and appends a message to errors when the standard has no
// LaserDisc digital audio specification, the areas are not in lead-in,
// programme, lead-out order, a run is too short to occupy a whole subcode
// section, or more than efm::kMaxTrackNumber tracks would be emitted
// (IEC 60856:1986 Amd 2, 13.5.3.3 / IEC 60857:1986 Amd 2, 13.6.3.3).
//
// With options.single_programme_track the programme-area runs are merged into
// one track 01 instead, so the track limit does not apply. With
// options.programme_start_section the programme area may start inside the
// lead-in; it is an error for that start to leave the lead-in no subcode
// section.
//
// Thread-safety: a pure function of its arguments.
bool BuildEfmTrackLayout(
    Standard standard, const std::vector<const Section*>& output_frame_sections,
    EfmTrackLayout* layout, std::vector<std::string>* errors,
    const EfmTrackLayoutOptions& options = {});

}  // namespace videosynth
