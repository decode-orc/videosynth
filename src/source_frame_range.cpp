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
                                     const SourceFrameRange& range) {
  if (!section.duration_frames_all) {
    return section.duration_frames;
  }
  return static_cast<std::int64_t>(range.count) *
         std::max(1, section.duration_frames_repeat);
}

std::int64_t KnownSectionOutputFrameCount(const Section& section,
                                          int source_frame_count) {
  if (!section.duration_frames_all) {
    return section.duration_frames;
  }
  SourceFrameRange range;
  if (!ResolveSourceFrameRange(section, source_frame_count, &range, nullptr)) {
    return -1;
  }
  return SectionOutputFrameCount(section, range);
}

int SourceFrameAtSectionPosition(const SourceFrameRange& range,
                                 std::int64_t section_position) {
  return range.first + static_cast<int>(section_position % range.count);
}

}  // namespace videosynth
