/*
 * File:        test_film_cadence.cpp
 * Module:      film_cadence_tests
 * Purpose:     Unit tests for the 2:3 pulldown field-to-film-frame mapping
 *              and pulldown section sizing.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include "videosynth/film_cadence.h"

namespace videosynth {
namespace {

// The cycle as letters, field by field, from `start_slot`.
std::string LettersFrom(int start_slot, int field_count) {
  std::string letters;
  for (int field = 0; field < field_count; ++field) {
    letters += CadenceFieldAt(start_slot, field).letter;
  }
  return letters;
}

TEST(FilmCadenceTest, PhaseStartSlotsAreTheFirstFieldOfEachLetter) {
  EXPECT_EQ(CadencePhaseStartSlot(CadencePhase::kA), 0);
  EXPECT_EQ(CadencePhaseStartSlot(CadencePhase::kB), 2);
  EXPECT_EQ(CadencePhaseStartSlot(CadencePhase::kC), 5);
  EXPECT_EQ(CadencePhaseStartSlot(CadencePhase::kD), 7);
}

TEST(FilmCadenceTest, CycleFromAFollowsTwoThreeTwoThree) {
  EXPECT_EQ(LettersFrom(0, 20), "AABBBCCDDDAABBBCCDDD");
  const std::int64_t expected_offsets[] = {0, 0, 1, 1, 1, 2, 2, 3, 3, 3, 4};
  for (int field = 0; field <= 10; ++field) {
    EXPECT_EQ(CadenceFieldAt(0, field).film_frame_offset,
              expected_offsets[field])
        << "field " << field;
  }
}

TEST(FilmCadenceTest, EveryPhaseOpensFilmFrameZeroOnItsFirstField) {
  for (const CadencePhase phase : {CadencePhase::kA, CadencePhase::kB,
                                   CadencePhase::kC, CadencePhase::kD}) {
    const int start = CadencePhaseStartSlot(phase);
    const CadenceField first = CadenceFieldAt(start, 0);
    EXPECT_EQ(first.film_frame_offset, 0);
    EXPECT_EQ(first.field_in_slot, 1);
    EXPECT_TRUE(first.starts_picture);
  }
  EXPECT_EQ(LettersFrom(CadencePhaseStartSlot(CadencePhase::kB), 10),
            "BBBCCDDDAA");
  EXPECT_EQ(LettersFrom(CadencePhaseStartSlot(CadencePhase::kD), 10),
            "DDDAABBBCC");
}

TEST(FilmCadenceTest, FieldInSlotCountsWithinTheLetter) {
  const int expected[] = {1, 2, 1, 2, 3, 1, 2, 1, 2, 3};
  for (int field = 0; field < kPulldownCycleFields; ++field) {
    EXPECT_EQ(CadenceFieldAt(0, field).field_in_slot, expected[field])
        << "field " << field;
  }
}

TEST(FilmCadenceTest, PictureStartsAtEachLetterBoundary) {
  const bool expected[] = {true, false, true, false, false,
                           true, false, true, false, false};
  for (int field = 0; field < kPulldownCycleFields; ++field) {
    EXPECT_EQ(CadenceFieldAt(0, field).starts_picture, expected[field])
        << "field " << field;
  }
}

TEST(FilmCadenceTest, MidSlotStartGivesAnOrphanFirstFilmFrame) {
  // Slot 4 is B3: the section's first film frame shows one field only.
  const CadenceField orphan = CadenceFieldAt(4, 0);
  EXPECT_EQ(orphan.letter, 'B');
  EXPECT_EQ(orphan.field_in_slot, 3);
  EXPECT_EQ(orphan.film_frame_offset, 0);
  EXPECT_TRUE(orphan.starts_picture);

  const CadenceField next = CadenceFieldAt(4, 1);
  EXPECT_EQ(next.letter, 'C');
  EXPECT_EQ(next.film_frame_offset, 1);
  EXPECT_TRUE(next.starts_picture);

  EXPECT_EQ(CadenceFieldsForFilmFrames(4, 1), 1);
  EXPECT_EQ(CadenceFieldsForFilmFrames(1, 1), 1);
  EXPECT_EQ(CadenceFieldsForFilmFrames(3, 2), 4);
}

TEST(FilmCadenceTest, FieldsForFilmFramesMatchTheCycle) {
  EXPECT_EQ(CadenceFieldsForFilmFrames(0, 0), 0);
  EXPECT_EQ(CadenceFieldsForFilmFrames(0, 1), 2);
  EXPECT_EQ(CadenceFieldsForFilmFrames(0, 2), 5);
  EXPECT_EQ(CadenceFieldsForFilmFrames(0, 4), 10);
  EXPECT_EQ(CadenceFieldsForFilmFrames(2, 1), 3);
  EXPECT_EQ(CadenceFieldsForFilmFrames(2, 4), 10);
  EXPECT_EQ(CadenceFieldsForFilmFrames(7, 1), 3);
}

// Every start slot: the fields counted for F film frames end exactly where
// film frame F would open.
TEST(FilmCadenceTest, FieldsForFilmFramesAgreeWithFieldMapping) {
  for (int start = 0; start < kPulldownCycleFields; ++start) {
    for (std::int64_t film_frames = 1; film_frames <= 40; ++film_frames) {
      const std::int64_t fields =
          CadenceFieldsForFilmFrames(start, film_frames);
      EXPECT_EQ(CadenceFieldAt(start, fields - 1).film_frame_offset,
                film_frames - 1)
          << "start " << start << " film frames " << film_frames;
      EXPECT_EQ(CadenceFieldAt(start, fields).film_frame_offset, film_frames)
          << "start " << start << " film frames " << film_frames;
    }
  }
}

TEST(FilmCadenceTest, OutputFramesRoundAnOddFieldCountUp) {
  EXPECT_EQ(CadenceOutputFramesForFilmFrames(0, 4), 5);
  EXPECT_EQ(CadenceOutputFramesForFilmFrames(0, 2), 3);  // 5 fields, padded
  EXPECT_EQ(CadenceOutputFramesForFilmFrames(2, 1), 2);  // 3 fields, padded
  EXPECT_EQ(CadenceOutputFramesForFilmFrames(0, 1), 1);
  EXPECT_EQ(CadenceOutputFramesForFilmFrames(0, 0), 0);
  // 24 film frames last exactly as long as 30 frames, from any phase.
  for (const CadencePhase phase : {CadencePhase::kA, CadencePhase::kB,
                                   CadencePhase::kC, CadencePhase::kD}) {
    EXPECT_EQ(
        CadenceOutputFramesForFilmFrames(CadencePhaseStartSlot(phase), 24), 30);
  }
}

TEST(FilmCadenceTest, FilmFramesForOutputFramesCountsOpenedFilmFrames) {
  EXPECT_EQ(CadenceFilmFramesForOutputFrames(0, 0), 0);
  EXPECT_EQ(CadenceFilmFramesForOutputFrames(0, 1), 1);
  EXPECT_EQ(CadenceFilmFramesForOutputFrames(0, 2), 2);
  EXPECT_EQ(CadenceFilmFramesForOutputFrames(0, 5), 4);
  EXPECT_EQ(CadenceFilmFramesForOutputFrames(0, 30), 24);
  EXPECT_EQ(CadenceFilmFramesForOutputFrames(4, 1), 2);  // B3 + C1
}

TEST(FilmCadenceTest, SlotAfterAdvancesTwoFieldsPerFrame) {
  EXPECT_EQ(CadenceSlotAfter(0, 5), 0);
  EXPECT_EQ(CadenceSlotAfter(0, 1), 2);
  EXPECT_EQ(CadenceSlotAfter(2, 1), 4);
  EXPECT_EQ(CadenceSlotAfter(7, 2), 1);
  EXPECT_EQ(CadenceSlotAfter(5, 0), 5);
}

TEST(FilmCadenceTest, LargeFieldIndicesStayExact) {
  const std::int64_t cycles = 1000000000000LL;
  const CadenceField field =
      CadenceFieldAt(0, cycles * kPulldownCycleFields + 2);
  EXPECT_EQ(field.film_frame_offset, cycles * kPulldownCycleFilmFrames + 1);
  EXPECT_EQ(field.letter, 'B');
  EXPECT_EQ(CadenceFieldsForFilmFrames(0, cycles * kPulldownCycleFilmFrames),
            cycles * kPulldownCycleFields);
}

}  // namespace
}  // namespace videosynth
