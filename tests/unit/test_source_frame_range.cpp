/*
 * File:        test_source_frame_range.cpp
 * Module:      source_frame_range_tests
 * Purpose:     Unit tests for section source-range resolution, output frame
 *              counts and looping within the selected range.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "videosynth/source_frame_range.h"

namespace videosynth {
namespace {

Section MakeVideoSection() {
  Section section;
  section.name = "Clip";
  section.type = "progressive";
  section.source = "clip.mkv";
  section.duration_frames_all = true;
  return section;
}

Section MakeStillSection() {
  Section section;
  section.name = "Still";
  section.type = "progressive";
  section.source = "bars.exr";
  section.duration_frames = 50;
  return section;
}

// The source frames a section plays, position by position.
std::vector<int> PlayedFrames(const Section& section, int source_frame_count) {
  SourceFrameRange range;
  std::string error;
  EXPECT_TRUE(
      ResolveSourceFrameRange(section, source_frame_count, &range, &error))
      << error;
  std::vector<int> frames;
  for (std::int64_t i = 0; i < SectionOutputFrameCount(section, range); ++i) {
    frames.push_back(SourceFrameAtSectionPosition(range, i));
  }
  return frames;
}

TEST(SourceFrameRangeTest, WithoutEndFrameRunsToTheLastSourceFrame) {
  Section section = MakeVideoSection();
  section.start_frame = 30;
  SourceFrameRange range;
  ASSERT_TRUE(ResolveSourceFrameRange(section, 100, &range, nullptr));
  EXPECT_EQ(range.first, 30);
  EXPECT_EQ(range.count, 70);
}

TEST(SourceFrameRangeTest, EndFrameIsInclusive) {
  Section section = MakeVideoSection();
  section.start_frame = 1500;
  section.end_frame = 2999;
  section.end_frame_specified = true;
  SourceFrameRange range;
  ASSERT_TRUE(ResolveSourceFrameRange(section, 4000, &range, nullptr));
  EXPECT_EQ(range.first, 1500);
  EXPECT_EQ(range.count, 1500);
}

TEST(SourceFrameRangeTest, SingleFrameRangeWhenEndEqualsStart) {
  Section section = MakeVideoSection();
  section.start_frame = 7;
  section.end_frame = 7;
  section.end_frame_specified = true;
  SourceFrameRange range;
  ASSERT_TRUE(ResolveSourceFrameRange(section, 10, &range, nullptr));
  EXPECT_EQ(range.count, 1);
}

TEST(SourceFrameRangeTest, ExplicitEndResolvesWithoutAProbe) {
  Section section = MakeVideoSection();
  section.start_frame = 10;
  section.end_frame = 19;
  section.end_frame_specified = true;
  SourceFrameRange range;
  ASSERT_TRUE(ResolveSourceFrameRange(section, kUnknownSourceFrameCount, &range,
                                      nullptr));
  EXPECT_EQ(range.first, 10);
  EXPECT_EQ(range.count, 10);
}

TEST(SourceFrameRangeTest, OpenEndedRangeNeedsTheSourceFrameCount) {
  Section section = MakeVideoSection();
  SourceFrameRange range;
  std::string error;
  EXPECT_FALSE(ResolveSourceFrameRange(section, kUnknownSourceFrameCount,
                                       &range, &error));
  EXPECT_NE(error.find("not known"), std::string::npos) << error;
}

TEST(SourceFrameRangeTest, RejectsEndBeforeStart) {
  Section section = MakeVideoSection();
  section.start_frame = 20;
  section.end_frame = 19;
  section.end_frame_specified = true;
  SourceFrameRange range;
  std::string error;
  EXPECT_FALSE(ResolveSourceFrameRange(section, 100, &range, &error));
  EXPECT_NE(error.find("before start_frame"), std::string::npos) << error;
}

TEST(SourceFrameRangeTest, RejectsNegativeStart) {
  Section section = MakeVideoSection();
  section.start_frame = -1;
  SourceFrameRange range;
  EXPECT_FALSE(ResolveSourceFrameRange(section, 100, &range, nullptr));
}

TEST(SourceFrameRangeTest, RejectsStartOrEndBeyondTheSource) {
  Section section = MakeVideoSection();
  section.start_frame = 100;
  SourceFrameRange range;
  std::string error;
  EXPECT_FALSE(ResolveSourceFrameRange(section, 100, &range, &error));
  EXPECT_NE(error.find("start_frame 100"), std::string::npos) << error;

  section.start_frame = 0;
  section.end_frame = 100;
  section.end_frame_specified = true;
  EXPECT_FALSE(ResolveSourceFrameRange(section, 100, &range, &error));
  EXPECT_NE(error.find("end_frame 100"), std::string::npos) << error;
}

TEST(SourceFrameRangeTest, StillsIgnoreTheRange) {
  Section section = MakeStillSection();
  section.start_frame = 12;
  section.end_frame = 40;
  section.end_frame_specified = true;
  SourceFrameRange range;
  ASSERT_TRUE(ResolveSourceFrameRange(section, kUnknownSourceFrameCount, &range,
                                      nullptr));
  EXPECT_EQ(range.first, 0);
  EXPECT_EQ(range.count, 1);
  EXPECT_EQ(PlayedFrames(section, kUnknownSourceFrameCount),
            std::vector<int>(50, 0));
}

TEST(SourceFrameRangeTest, AllDurationPlaysTheRangeOnce) {
  Section section = MakeVideoSection();
  section.start_frame = 3;
  section.end_frame = 6;
  section.end_frame_specified = true;
  EXPECT_EQ(PlayedFrames(section, 10), (std::vector<int>{3, 4, 5, 6}));
}

TEST(SourceFrameRangeTest, AllDurationWithStartFrameStopsAtTheSourceEnd) {
  Section section = MakeVideoSection();
  section.start_frame = 7;
  EXPECT_EQ(PlayedFrames(section, 10), (std::vector<int>{7, 8, 9}));
}

TEST(SourceFrameRangeTest, AllDurationRepeatReplaysTheRange) {
  Section section = MakeVideoSection();
  section.start_frame = 2;
  section.end_frame = 4;
  section.end_frame_specified = true;
  section.duration_frames_repeat = 3;
  EXPECT_EQ(PlayedFrames(section, 10),
            (std::vector<int>{2, 3, 4, 2, 3, 4, 2, 3, 4}));
}

TEST(SourceFrameRangeTest, FixedDurationShorterThanRangePlaysItsStart) {
  Section section = MakeVideoSection();
  section.duration_frames_all = false;
  section.duration_frames = 3;
  section.start_frame = 5;
  EXPECT_EQ(PlayedFrames(section, 100), (std::vector<int>{5, 6, 7}));
}

TEST(SourceFrameRangeTest, FixedDurationLongerThanRangeLoopsIt) {
  Section section = MakeVideoSection();
  section.duration_frames_all = false;
  section.duration_frames = 8;
  section.start_frame = 7;
  EXPECT_EQ(PlayedFrames(section, 10),
            (std::vector<int>{7, 8, 9, 7, 8, 9, 7, 8}));
}

TEST(SourceFrameRangeTest, KnownOutputCountForFixedAndClosedRanges) {
  Section fixed = MakeVideoSection();
  fixed.duration_frames_all = false;
  fixed.duration_frames = 250;
  EXPECT_EQ(KnownSectionOutputFrameCount(fixed, kUnknownSourceFrameCount), 250);

  Section closed = MakeVideoSection();
  closed.start_frame = 10;
  closed.end_frame = 19;
  closed.end_frame_specified = true;
  closed.duration_frames_repeat = 2;
  EXPECT_EQ(KnownSectionOutputFrameCount(closed, kUnknownSourceFrameCount), 20);
}

TEST(SourceFrameRangeTest, KnownOutputCountIsUnknownForOpenEndedAll) {
  Section section = MakeVideoSection();
  section.duration_frames_repeat = 2;
  EXPECT_EQ(KnownSectionOutputFrameCount(section, kUnknownSourceFrameCount),
            -1);
  EXPECT_EQ(KnownSectionOutputFrameCount(section, 40), 80);
}

Section MakeFilmSection(CadencePhase phase, CadenceContinuity continuity) {
  Section section = MakeVideoSection();
  section.cadence.pattern = CadencePattern::kPulldown23;
  section.cadence.phase = phase;
  section.cadence.continuity = continuity;
  return section;
}

// A closed range of `count` frames from frame 0.
void SetClosedRange(Section* section, int count) {
  section->end_frame = count - 1;
  section->end_frame_specified = true;
}

TEST(SourceFrameRangeTest, PulldownAllDurationCountsFieldsOfFilmFrames) {
  Section section =
      MakeFilmSection(CadencePhase::kA, CadenceContinuity::kRestart);
  SetClosedRange(&section, 8);
  SourceFrameRange range;
  ASSERT_TRUE(ResolveSourceFrameRange(section, 100, &range, nullptr));
  EXPECT_EQ(SectionOutputFrameCount(section, range, 0), 10);
  EXPECT_EQ(SectionOutputFrameCount(section, range, 2), 10);

  SetClosedRange(&section, 2);
  ASSERT_TRUE(ResolveSourceFrameRange(section, 100, &range, nullptr));
  // A A B B B: five fields, the sixth repeats B.
  EXPECT_EQ(SectionOutputFrameCount(section, range, 0), 3);
  // D D D A A
  EXPECT_EQ(SectionOutputFrameCount(section, range, 7), 3);
}

TEST(SourceFrameRangeTest, PulldownRepeatMultipliesFilmFramesNotFrames) {
  Section section =
      MakeFilmSection(CadencePhase::kA, CadenceContinuity::kRestart);
  SetClosedRange(&section, 2);
  section.duration_frames_repeat = 2;
  EXPECT_EQ(KnownSectionOutputFrameCount(section, kUnknownSourceFrameCount, 0),
            5);
}

TEST(SourceFrameRangeTest, PulldownFixedDurationIsUnchanged) {
  Section section =
      MakeFilmSection(CadencePhase::kB, CadenceContinuity::kRestart);
  section.duration_frames_all = false;
  section.duration_frames = 7;
  EXPECT_EQ(KnownSectionOutputFrameCount(section, kUnknownSourceFrameCount,
                                         kUnknownCadenceSlot),
            7);
}

TEST(SourceFrameRangeTest, PulldownAllIsUnknownWithoutAStartSlot) {
  Section section =
      MakeFilmSection(CadencePhase::kA, CadenceContinuity::kContinue);
  SetClosedRange(&section, 8);
  EXPECT_EQ(KnownSectionOutputFrameCount(section, 100, kUnknownCadenceSlot),
            -1);
}

TEST(SourceFrameRangeTest, ResolvesCadenceStartSlotsAcrossAMixedChain) {
  std::vector<Section> sections;
  sections.push_back(MakeVideoSection());  // 0: video, ignored
  Section film = MakeFilmSection(CadencePhase::kA, CadenceContinuity::kRestart);
  SetClosedRange(&film, 4);  // 1: A A B B B C C D D D -> 5 frames, ends at 0
  sections.push_back(film);
  Section fixed =
      MakeFilmSection(CadencePhase::kD, CadenceContinuity::kContinue);
  fixed.duration_frames_all = false;
  fixed.duration_frames = 3;  // 2: starts at 0, 6 fields, ends at 6
  sections.push_back(fixed);
  Section orphan =
      MakeFilmSection(CadencePhase::kA, CadenceContinuity::kContinue);
  SetClosedRange(&orphan, 1);  // 3: starts at C2, one field padded to 2
  sections.push_back(orphan);
  sections.push_back(MakeVideoSection());  // 4: video breaks the chain
  sections.push_back(
      MakeFilmSection(CadencePhase::kC, CadenceContinuity::kContinue));  // 5

  const std::vector<int> counts(sections.size(), 100);
  const std::vector<int> slots = ResolveCadenceStartSlots(sections, counts);
  EXPECT_EQ(slots, (std::vector<int>{0, 0, 0, 6, 0, 5}));
}

TEST(SourceFrameRangeTest, CadenceStartSlotIsUnknownAfterAnUnknownLength) {
  std::vector<Section> sections;
  sections.push_back(
      MakeFilmSection(CadencePhase::kB, CadenceContinuity::kRestart));
  sections.push_back(
      MakeFilmSection(CadencePhase::kA, CadenceContinuity::kContinue));
  Section fixed =
      MakeFilmSection(CadencePhase::kA, CadenceContinuity::kContinue);
  fixed.duration_frames_all = false;
  fixed.duration_frames = 4;
  sections.push_back(fixed);
  sections.push_back(
      MakeFilmSection(CadencePhase::kA, CadenceContinuity::kContinue));

  const std::vector<int> counts(sections.size(), kUnknownSourceFrameCount);
  const std::vector<int> slots = ResolveCadenceStartSlots(sections, counts);
  EXPECT_EQ(slots,
            (std::vector<int>{2, kUnknownCadenceSlot, kUnknownCadenceSlot,
                              kUnknownCadenceSlot}));
}

}  // namespace
}  // namespace videosynth
