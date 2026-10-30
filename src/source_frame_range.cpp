/*
 * File:        source_frame_range.cpp
 * Module:      source_frame_range
 * Purpose:     Resolves a section's selected source frame range and maps
 *              section positions onto it, looping within the range.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "videosynth/source_frame_range.h"

#include <algorithm>

#include "videosynth/film_cadence.h"
#include "videosynth/progressive_frame_source.h"

namespace videosynth {

namespace {

void SetError(std::string* error, const std::string& message) {
  if (error != nullptr) {
    *error = message;
  }
}

}  // namespace

bool ResolveSourceFrameRange(const Section& section, int source_frame_count,
                             SourceFrameRange* out_range, std::string* error) {
  if (out_range == nullptr) {
    SetError(error, "Source frame range output pointer must not be null.");
    return false;
  }

  if (IsProgressiveStillSource(section.source)) {
    *out_range = SourceFrameRange{.first = 0, .count = 1};
    return true;
  }

  if (section.start_frame < 0) {
    SetError(error, "Section '" + section.name +
                        "': start_frame must be >= 0 (got " +
                        std::to_string(section.start_frame) + ").");
    return false;
  }
  if (section.end_frame_specified && section.end_frame < section.start_frame) {
    SetError(error, "Section '" + section.name + "': end_frame " +
                        std::to_string(section.end_frame) +
                        " is before start_frame " +
                        std::to_string(section.start_frame) + ".");
    return false;
  }

  if (source_frame_count == kUnknownSourceFrameCount) {
    if (!section.end_frame_specified) {
      SetError(error, "Section '" + section.name +
                          "': the source frame count is not known yet.");
      return false;
    }
  } else {
    if (section.start_frame >= source_frame_count) {
      SetError(error, "Section '" + section.name + "': start_frame " +
                          std::to_string(section.start_frame) +
                          " is beyond the end of the source (" +
                          std::to_string(source_frame_count) + " frames).");
      return false;
    }
    if (section.end_frame_specified &&
        section.end_frame >= source_frame_count) {
      SetError(error, "Section '" + section.name + "': end_frame " +
                          std::to_string(section.end_frame) +
                          " is beyond the end of the source (" +
                          std::to_string(source_frame_count) + " frames).");
      return false;
    }
  }

  const int last =
      section.end_frame_specified ? section.end_frame : source_frame_count - 1;
  *out_range = SourceFrameRange{.first = section.start_frame,
                                .count = last - section.start_frame + 1};
  return true;
}

std::int64_t SectionOutputFrameCount(const Section& section,
                                     const SourceFrameRange& range,
                                     int cadence_start_slot) {
  if (!section.duration_frames_all) {
    return section.duration_frames;
  }
  const std::int64_t source_frames =
      static_cast<std::int64_t>(range.count) *
      std::max(1, section.duration_frames_repeat);
  if (section.cadence.pattern == CadencePattern::kPulldown23) {
    return CadenceOutputFramesForFilmFrames(cadence_start_slot, source_frames);
  }
  return source_frames;
}

std::int64_t KnownSectionOutputFrameCount(const Section& section,
                                          int source_frame_count,
                                          int cadence_start_slot) {
  if (!section.duration_frames_all) {
    return section.duration_frames;
  }
  if (section.cadence.pattern == CadencePattern::kPulldown23 &&
      cadence_start_slot == kUnknownCadenceSlot) {
    return -1;
  }
  SourceFrameRange range;
  if (!ResolveSourceFrameRange(section, source_frame_count, &range, nullptr)) {
    return -1;
  }
  return SectionOutputFrameCount(section, range, cadence_start_slot);
}

std::int64_t KnownSectionPictureCount(const Section& section,
                                      int source_frame_count,
                                      int cadence_start_slot) {
  const std::int64_t output_frames = KnownSectionOutputFrameCount(
      section, source_frame_count, cadence_start_slot);
  if (output_frames < 0 ||
      section.cadence.pattern != CadencePattern::kPulldown23) {
    return output_frames;
  }
  if (cadence_start_slot == kUnknownCadenceSlot) {
    return -1;
  }
  if (section.duration_frames_all) {
    SourceFrameRange range;
    if (!ResolveSourceFrameRange(section, source_frame_count, &range,
                                 nullptr)) {
      return -1;
    }
    return static_cast<std::int64_t>(range.count) *
           std::max(1, section.duration_frames_repeat);
  }
  return CadenceFilmFramesForOutputFrames(cadence_start_slot, output_frames);
}

std::vector<int> ResolveCadenceStartSlots(
    const std::vector<Section>& sections,
    const std::vector<int>& source_frame_counts) {
  std::vector<int> slots(sections.size(), 0);
  // The slot after the previous section's last field, or kUnknownCadenceSlot
  // when unknown. Only meaningful after a 2:3 section.
  int slot_after_previous = kUnknownCadenceSlot;
  bool previous_is_pulldown = false;
  for (std::size_t index = 0; index < sections.size(); ++index) {
    const Section& section = sections[index];
    if (section.cadence.pattern != CadencePattern::kPulldown23) {
      previous_is_pulldown = false;
      continue;
    }
    int start_slot = CadencePhaseStartSlot(section.cadence.phase);
    if (section.cadence.continuity == CadenceContinuity::kContinue &&
        previous_is_pulldown) {
      start_slot = slot_after_previous;
    }
    slots[index] = start_slot;

    const int source_frame_count = index < source_frame_counts.size()
                                       ? source_frame_counts[index]
                                       : kUnknownSourceFrameCount;
    const std::int64_t output_frames =
        KnownSectionOutputFrameCount(section, source_frame_count, start_slot);
    slot_after_previous =
        (start_slot == kUnknownCadenceSlot || output_frames < 0)
            ? kUnknownCadenceSlot
            : CadenceSlotAfter(start_slot, output_frames);
    previous_is_pulldown = true;
  }
  return slots;
}

int SourceFrameAtSectionPosition(const SourceFrameRange& range,
                                 std::int64_t section_position) {
  return range.first + static_cast<int>(section_position % range.count);
}

}  // namespace videosynth
