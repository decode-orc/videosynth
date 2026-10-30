/*
 * File:        film_cadence.cpp
 * Module:      film_cadence
 * Purpose:     Maps output fields onto film frames for the 2:3 pulldown
 *              cadence and sizes pulldown sections in output frames.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "videosynth/film_cadence.h"

#include <array>

namespace videosynth {

namespace {

// Film-frame letter of each cycle slot.
constexpr std::array<char, kPulldownCycleFields> kSlotLetters = {
    'A', 'A', 'B', 'B', 'B', 'C', 'C', 'D', 'D', 'D'};

// The slots that open a film frame (the first A, B, C and D fields).
constexpr std::array<int, kPulldownCycleFilmFrames> kPictureStartSlots = {0, 2,
                                                                          5, 7};

int NormaliseSlot(int slot) {
  return ((slot % kPulldownCycleFields) + kPulldownCycleFields) %
         kPulldownCycleFields;
}

// The first slot of the film frame that owns `slot`.
int PictureStartSlotOf(int slot) {
  int start = kPictureStartSlots[0];
  for (const int candidate : kPictureStartSlots) {
    if (candidate <= slot) {
      start = candidate;
    }
  }
  return start;
}

// Film-frame openings at absolute cycle positions [0, position).
std::int64_t PictureStartsBefore(std::int64_t position) {
  const std::int64_t cycles = position / kPulldownCycleFields;
  const int remainder = static_cast<int>(position % kPulldownCycleFields);
  std::int64_t starts = cycles * kPulldownCycleFilmFrames;
  for (const int slot : kPictureStartSlots) {
    if (slot < remainder) {
      ++starts;
    }
  }
  return starts;
}

}  // namespace

int CadencePhaseStartSlot(CadencePhase phase) {
  switch (phase) {
    case CadencePhase::kA:
      return kPictureStartSlots[0];
    case CadencePhase::kB:
      return kPictureStartSlots[1];
    case CadencePhase::kC:
      return kPictureStartSlots[2];
    case CadencePhase::kD:
      return kPictureStartSlots[3];
  }
  return kPictureStartSlots[0];
}

// A field's film frame is the count of film-frame openings between the
// section's first field (exclusive) and the field itself (inclusive): the
// first field always opens film frame 0, even mid-slot.
CadenceField CadenceFieldAt(int start_slot, std::int64_t section_field) {
  const int start = NormaliseSlot(start_slot);
  const std::int64_t position = start + section_field;
  const int slot = static_cast<int>(position % kPulldownCycleFields);

  CadenceField field;
  field.film_frame_offset =
      PictureStartsBefore(position + 1) - PictureStartsBefore(start + 1);
  field.letter = kSlotLetters[static_cast<std::size_t>(slot)];
  field.field_in_slot = slot - PictureStartSlotOf(slot) + 1;
  field.starts_picture = section_field == 0 || PictureStartSlotOf(slot) == slot;
  return field;
}

// The fields end where film frame `film_frames` would open: the
// (openings before the start + film_frames)-th opening after position 0.
std::int64_t CadenceFieldsForFilmFrames(int start_slot,
                                        std::int64_t film_frames) {
  if (film_frames <= 0) {
    return 0;
  }
  const int start = NormaliseSlot(start_slot);
  const std::int64_t opening = PictureStartsBefore(start + 1) + film_frames;
  const std::int64_t cycle = (opening - 1) / kPulldownCycleFilmFrames;
  const int index = static_cast<int>((opening - 1) % kPulldownCycleFilmFrames);
  const std::int64_t end_position =
      cycle * kPulldownCycleFields +
      kPictureStartSlots[static_cast<std::size_t>(index)];
  return end_position - start;
}

std::int64_t CadenceOutputFramesForFilmFrames(int start_slot,
                                              std::int64_t film_frames) {
  return (CadenceFieldsForFilmFrames(start_slot, film_frames) + 1) / 2;
}

std::int64_t CadenceFilmFramesForOutputFrames(int start_slot,
                                              std::int64_t output_frames) {
  if (output_frames <= 0) {
    return 0;
  }
  return CadenceFieldAt(start_slot, output_frames * 2 - 1).film_frame_offset +
         1;
}

int CadenceSlotAfter(int start_slot, std::int64_t output_frames) {
  const std::int64_t position = NormaliseSlot(start_slot) + output_frames * 2;
  return static_cast<int>(position % kPulldownCycleFields);
}

}  // namespace videosynth
