/*
 * File:        source_frame_range.h
 * Module:      source_frame_range
 * Purpose:     Resolves a section's selected source frame range and maps
 *              section positions onto it, looping within the range.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <cstdint>
#include <string>

#include "videosynth/model.h"

namespace videosynth {

// Source frame count to pass when the source has not been probed (yet).
inline constexpr int kUnknownSourceFrameCount = -1;

// A contiguous run of source frames: [first, first + count).
struct SourceFrameRange {
  int first = 0;
  int count = 0;
};

// Resolves the source frames a section plays: [start_frame, end_frame], or
// [start_frame, last frame] when end_frame is absent. An EXR still has one
// frame and ignores start_frame/end_frame (the validator rejects them).
//
// `source_frame_count` is the probed length of a video source, or
// kUnknownSourceFrameCount; with an unknown count only an explicit end_frame
// resolves. Returns false with `error` set when the range is empty, inverted,
// out of bounds, or depends on the unknown count.
//
// Thread-safety: thread-safe (pure function).
bool ResolveSourceFrameRange(const Section& section, int source_frame_count,
                             SourceFrameRange* out_range, std::string* error);

// Output frames the section produces over `range`: range length x
// duration_frames_repeat for an "all" duration, duration_frames otherwise.
//
// Thread-safety: thread-safe (pure function).
std::int64_t SectionOutputFrameCount(const Section& section,
                                     const SourceFrameRange& range);

// Output frames the section produces, or -1 while that depends on a source
// frame count that is unknown (an "all" duration over an open-ended range).
// A fixed duration is always known, even when its range is invalid.
//
// Thread-safety: thread-safe (pure function).
std::int64_t KnownSectionOutputFrameCount(const Section& section,
                                          int source_frame_count);

// The source frame at 0-based `section_position`: positions past the end of
// the range wrap back to its first frame, so a duration longer than the range
// loops it. `range.count` must be > 0.
//
// Thread-safety: thread-safe (pure function).
int SourceFrameAtSectionPosition(const SourceFrameRange& range,
                                 std::int64_t section_position);

}  // namespace videosynth
