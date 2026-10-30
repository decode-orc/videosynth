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
#include <vector>

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

// Cadence start slot to pass while it depends on a preceding section whose
// length is not known (yet).
inline constexpr int kUnknownCadenceSlot = -1;

// Output frames the section produces over `range`: duration_frames for a
// fixed duration. An "all" duration plays range length x
// duration_frames_repeat source frames: one output frame each for a video
// cadence, or, for 2:3 pulldown, the frames those film frames fill from
// `cadence_start_slot` (see CadenceOutputFramesForFilmFrames).
//
// Thread-safety: thread-safe (pure function).
std::int64_t SectionOutputFrameCount(const Section& section,
                                     const SourceFrameRange& range,
                                     int cadence_start_slot = 0);

// Output frames the section produces, or -1 while that depends on a source
// frame count that is unknown (an "all" duration over an open-ended range)
// or on a cadence start slot that is kUnknownCadenceSlot. A fixed duration is
// always known, even when its range is invalid.
//
// Thread-safety: thread-safe (pure function).
std::int64_t KnownSectionOutputFrameCount(const Section& section,
                                          int source_frame_count,
                                          int cadence_start_slot = 0);

// Pictures the section shows, or -1 when its output frame count is not known
// (see KnownSectionOutputFrameCount). A video-cadence frame is one picture;
// under 2:3 pulldown each film frame shown is one, counting the repeats of a
// looped range, and the padding field of an odd "all" field count starts
// none.
//
// Thread-safety: thread-safe (pure function).
std::int64_t KnownSectionPictureCount(const Section& section,
                                      int source_frame_count,
                                      int cadence_start_slot = 0);

// The 2:3 cycle slot each section's first field occupies, index for index
// with `sections`. `source_frame_counts` holds each section's probed source
// frame count (or kUnknownSourceFrameCount), index for index.
//
// A restarting 2:3 section starts at its phase. A continuing one starts at
// the slot after the previous section's last field, or kUnknownCadenceSlot
// when the previous section's length or start is unknown; with no 2:3
// section before it (a validation error) it falls back to its phase. Video
// sections ignore the slot and get 0.
//
// Thread-safety: thread-safe (pure function).
std::vector<int> ResolveCadenceStartSlots(
    const std::vector<Section>& sections,
    const std::vector<int>& source_frame_counts);

// The source frame at 0-based `section_position`: positions past the end of
// the range wrap back to its first frame, so a duration longer than the range
// loops it. `range.count` must be > 0.
//
// Thread-safety: thread-safe (pure function).
int SourceFrameAtSectionPosition(const SourceFrameRange& range,
                                 std::int64_t section_position);

}  // namespace videosynth
