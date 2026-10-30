/*
 * File:        frame_enrichment.h
 * Module:      frame_enrichment
 * Purpose:     Fully-resolved per-frame VBI payload and context produced by
 *              the sequential schedule-enrichment pass, enabling stateless
 *              (order-independent) frame sample synthesis.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "videosynth/fm_encoder.h"

namespace videosynth {

// Per-frame snapshot captured after code resolution and before generator
// advance. Used for token-based OSD rendering.
struct PerFrameContext {
  // Decoded CAV picture number of the picture shown (0 if no picture_number
  // generator is active). Per field: the fields of a 2:3 pulldown frame can
  // show different pictures.
  int picture_number = 0;
  // Raw 24-bit biphase code values produced by all active generators.
  std::vector<uint32_t> biphase_words;
  // Colour-frame sequence index: 0–3 for PAL (V-axis cycle), 0–1 for NTSC.
  int colour_frame_index = 0;
  // Sequential position of this frame within the whole generated output,
  // 1-based. Independent of any biphase code; it cannot be re-anchored by a
  // section and simply runs from 1 to the total output frame count.
  int frame_number = 0;
  // True when a CLV programme timecode is available for this frame, i.e. the
  // project is a CLV disc. When false the {timecode} token renders a
  // placeholder. Set by GenerationStage, not the biphase manager.
  bool has_clv_timecode = false;
  // CLV programme timecode components (HH:MM:SS:FF). Computed from the 0-based
  // sequential output frame position (see ClvTimecodeForFrame), so the timecode
  // runs continuously from the start of the output independent of which VBI
  // codes a section injects. Valid only when has_clv_timecode is true.
  int clv_hours = 0;
  int clv_minutes = 0;
  int clv_seconds = 0;
  int clv_frames = 0;
  // Source frame of the field being drawn (0-based). Set per field by
  // GenerationStage; across a 2:3 pulldown picture the two fields differ.
  int film_frame = 0;
  // 2:3 pulldown cycle letter ('A'-'D') and 1-based field within it for the
  // field being drawn; '\0' and 0 for a video cadence.
  char cadence_letter = '\0';
  int cadence_field_in_letter = 0;
};

// One fully-resolved VBI code injection for a single line of one frame.
// Produced by BiphaseInjectionManager::ResolveFrame and consumed by
// InjectResolvedVbiLines; carries everything the waveform writer needs so
// injection requires no generator state.
struct VbiLineInjection {
  enum class Kind {
    kBiphase24,  // IEC 60856/60857 24-bit Manchester biphase code
    kFm40,       // IEC 60857 Figure 13 40-bit FM code (NTSC only)
    kWhiteFlag,  // IEC 60857 Figure 12 100 IRE white flag (NTSC only)
  };

  int line_number_1based = 0;
  Kind kind = Kind::kBiphase24;
  // 24-bit code word for this frame (Kind::kBiphase24 only).
  uint32_t biphase_code = 0;
  // Resolved 40-bit FM payload for this frame (Kind::kFm40 only).
  FmData fm_data = {};
  // Horizontal start offset of the signal within the line, in samples.
  int start_sample = 0;
  // White flag pulse length in samples (Kind::kWhiteFlag only).
  int flag_length_samples = 0;
};

// Fully-resolved per-frame payload attached to a FrameScheduleItem by the
// sequential enrichment pass in BuildFrameSchedule. With this payload, sample
// synthesis is a pure function of (Project, enriched FrameScheduleItem):
// frames may be synthesised out of order and on concurrent worker threads.
struct FrameEnrichment {
  // Disc frame index, i.e. (picture number - 1) when a video-cadence frame is
  // anchored to a CAV picture_number start_value, otherwise the schedule
  // position plus the project's first-picture-number offset. Drives
  // colour-subcarrier phase, PAL burst blanking parity, and pilot-burst phase.
  std::size_t disc_frame_index = 0;
  // Context snapshot for OSD token resolution.
  PerFrameContext context;
  // CAV picture number of the picture each field shows (index 0 = field 1),
  // 0 without a picture_number generator. Both fields of a video-cadence
  // frame show one picture; a 2:3 pulldown frame's fields can differ.
  std::array<int, 2> field_picture_numbers{};
  // Resolved VBI code injections for this frame, in ascending line order.
  std::vector<VbiLineInjection> vbi_lines;
  // Token-resolved OSD overlay strings, one per section OSD overlay, for
  // field 1 (index 0) and field 2 (index 1): per-field tokens such as
  // {film_frame} can differ between the fields of one frame.
  std::array<std::vector<std::string>, 2> field_osd_texts;
};

}  // namespace videosynth
