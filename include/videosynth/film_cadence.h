/*
 * File:        film_cadence.h
 * Module:      film_cadence
 * Purpose:     Maps output fields onto film frames for the 2:3 pulldown
 *              cadence and sizes pulldown sections in output frames.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <cstdint>

#include "videosynth/model.h"

namespace videosynth {

// SMPTE 2:3 pulldown: four 24000/1001 film frames span ten 59.94 Hz fields,
// in the cycle A A B B B C C D D D (slots 0-9).
inline constexpr int kPulldownCycleFields = 10;
inline constexpr int kPulldownCycleFilmFrames = 4;

// Picture rate of film sources shown with 2:3 pulldown (24000/1001 Hz).
inline constexpr double kFilmFrameRateHz = 24000.0 / 1001.0;

// One output field's place in the cadence.
struct CadenceField {
  // 0-based film frame within the section's film sequence (before any
  // range looping or end padding is applied).
  std::int64_t film_frame_offset = 0;
  // Film-frame letter of the field's slot: 'A', 'B', 'C' or 'D'.
  char letter = 'A';
  // 1-based position of the field within its film frame's slot run, as the
  // cycle defines it (B3 is the third field of B). A section that starts
  // mid-slot begins at that slot's position, not at 1.
  int field_in_slot = 1;
  // True for the first field the section shows of this film frame.
  bool starts_picture = true;
};

// The cycle slot of the first field of `phase` (A 0, B 2, C 5, D 7).
//
// Thread-safety: thread-safe (pure function).
int CadencePhaseStartSlot(CadencePhase phase);

// The cadence position of 0-based `section_field` in a section whose first
// field occupies `start_slot` (0-9). O(1).
//
// Thread-safety: thread-safe (pure function).
CadenceField CadenceFieldAt(int start_slot, std::int64_t section_field);

// Fields needed to show `film_frames` film frames in full from `start_slot`;
// the first film frame gets only the rest of its slot when `start_slot` is
// mid-slot. Zero for zero film frames. O(1).
//
// Thread-safety: thread-safe (pure function).
std::int64_t CadenceFieldsForFilmFrames(int start_slot,
                                        std::int64_t film_frames);

// Output frames needed to show `film_frames` film frames: ceil(fields / 2).
// An odd final field repeats the last film frame. O(1).
//
// Thread-safety: thread-safe (pure function).
std::int64_t CadenceOutputFramesForFilmFrames(int start_slot,
                                              std::int64_t film_frames);

// Distinct film frames (counted with repeats of a looped range) that
// `output_frames` output frames begin, from `start_slot`. O(1).
//
// Thread-safety: thread-safe (pure function).
std::int64_t CadenceFilmFramesForOutputFrames(int start_slot,
                                              std::int64_t output_frames);

// The cycle slot of the field after `output_frames` output frames that start
// at `start_slot`: where a following `continuity: continue` section begins.
//
// Thread-safety: thread-safe (pure function).
int CadenceSlotAfter(int start_slot, std::int64_t output_frames);

}  // namespace videosynth
