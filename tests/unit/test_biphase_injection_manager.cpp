/*
 * File:        test_biphase_injection_manager.cpp
 * Module:      biphase_injection_manager_tests
 * Purpose:     Unit tests for BiphaseInjectionManager — biphase and 40-bit FM
 *              VBI injection orchestration per IEC 60856 (PAL) / IEC 60857
 *              (NTSC).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <cmath>
#include <string>
#include <vector>

#include "videosynth/biphase_injection_manager.h"
#include "videosynth/biphase_types.h"
#include "videosynth/cav_code_generator.h"
#include "videosynth/fixed_point.h"
#include "videosynth/line_placement_engine.h"
#include "videosynth/model.h"
#include "videosynth/signal_timing_model.h"
#include "videosynth/timing_constants.h"

namespace videosynth {
namespace {

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------

// Builds a Y buffer for one frame at blanking level (0 mV).
std::vector<SampleFixed> MakeBlankingBuffer(Standard standard) {
  const TimingConstants timing = GetTimingConstants(standard);
  const int lines = timing.lines_per_frame;
  const int spl = timing.samples_per_line_4fsc;
  // Approximate total: lines × spl (ignoring PAL long-line correction)
  return std::vector<SampleFixed>(static_cast<std::size_t>(lines * spl),
                                  MillivoltsToSampleFixed(0.0));
}

// Builds per-line sample offset/count vectors (uniform nominal line length).
void BuildLineLayout(Standard standard, std::vector<int>* offsets,
                     std::vector<int>* counts) {
  const TimingConstants timing = GetTimingConstants(standard);
  const int lines = timing.lines_per_frame;
  const int spl = timing.samples_per_line_4fsc;
  offsets->resize(static_cast<std::size_t>(lines));
  counts->resize(static_cast<std::size_t>(lines));
  for (int i = 0; i < lines; ++i) {
    (*offsets)[static_cast<std::size_t>(i)] = i * spl;
    (*counts)[static_cast<std::size_t>(i)] = spl;
  }
}

// Active-picture window start samples (mirrors generation_stage.cpp).
int ActiveWindowStart(Standard standard) {
  const TimingConstants timing = GetTimingConstants(standard);
  if (standard == Standard::kPal) {
    return 177;
  }
  return static_cast<int>(std::lround(timing.sample_rate_4fsc_hz * 10.5e-6));
}

// Active-picture window end samples, exclusive (mirrors generation_stage.cpp).
int ActiveWindowEnd(Standard standard) {
  const TimingConstants timing = GetTimingConstants(standard);
  if (standard == Standard::kPal) {
    return 177 +
           static_cast<int>(std::lround(timing.sample_rate_4fsc_hz * 52.0e-6));
  }
  return static_cast<int>(std::lround(timing.sample_rate_4fsc_hz * 62.5e-6));
}

// Builds a minimal Section with one laserdisc LineInjection.
Section MakeLaserdiscSection(
    SectionType section_type, DiscType disc_type,
    const std::vector<Section::LineInjectionCode>& codes) {
  // disc_type is now a project-level setting supplied to the manager via
  // SetProjectDiscType; the section carries only its laserdisc codes.
  (void)disc_type;
  Section section;
  section.name = "TestSection";
  section.type = "progressive";
  section.section_type = section_type;
  section.duration_frames = 1;

  Section::LineInjection injection;
  injection.type = "laserdisc";
  injection.codes = codes;
  section.line_injections.push_back(injection);

  return section;
}

// Returns the mean millivolt level on a given 1-based line number within
// the range [start_sample, end_sample) relative to the line start.
double MeanMvOnLine(const std::vector<SampleFixed>& y_mv, Standard standard,
                    int line_1based, int start_sample, int end_sample) {
  const TimingConstants timing = GetTimingConstants(standard);
  const int spl = timing.samples_per_line_4fsc;
  const int line_base = (line_1based - 1) * spl;
  double sum = 0.0;
  int count = 0;
  for (int i = start_sample;
       i < end_sample && (line_base + i) < static_cast<int>(y_mv.size()); ++i) {
    sum += SampleFixedToMillivolts(y_mv[static_cast<std::size_t>(line_base) +
                                        static_cast<std::size_t>(i)]);
    ++count;
  }
  return (count > 0) ? (sum / count) : 0.0;
}

// Returns the max millivolt level on a given line in [start_sample,
// end_sample).
double MaxMvOnLine(const std::vector<SampleFixed>& y_mv, Standard standard,
                   int line_1based, int start_sample, int end_sample) {
  const TimingConstants timing = GetTimingConstants(standard);
  const int spl = timing.samples_per_line_4fsc;
  const int line_base = (line_1based - 1) * spl;
  double max_mv = -1e9;
  for (int i = start_sample;
       i < end_sample && (line_base + i) < static_cast<int>(y_mv.size()); ++i) {
    const double mv =
        SampleFixedToMillivolts(y_mv[static_cast<std::size_t>(line_base) +
                                     static_cast<std::size_t>(i)]);
    if (mv > max_mv) {
      max_mv = mv;
    }
  }
  return max_mv;
}

// Returns the min millivolt level on a given line in [start_sample,
// end_sample).
double MinMvOnLine(const std::vector<SampleFixed>& y_mv, Standard standard,
                   int line_1based, int start_sample, int end_sample) {
  const TimingConstants timing = GetTimingConstants(standard);
  const int spl = timing.samples_per_line_4fsc;
  const int line_base = (line_1based - 1) * spl;
  double min_mv = 1e9;
  for (int i = start_sample;
       i < end_sample && (line_base + i) < static_cast<int>(y_mv.size()); ++i) {
    const double mv =
        SampleFixedToMillivolts(y_mv[static_cast<std::size_t>(line_base) +
                                     static_cast<std::size_t>(i)]);
    if (mv < min_mv) {
      min_mv = mv;
    }
  }
  return min_mv;
}

// Checks if any sample on a given line in [start_sample, end_sample) differs
// from blanking level (0 mV), indicating signal was injected.
bool LineHasSignal(const std::vector<SampleFixed>& y_mv, Standard standard,
                   int line_1based, int start_sample, int end_sample) {
  const TimingConstants timing = GetTimingConstants(standard);
  const int spl = timing.samples_per_line_4fsc;
  const int line_base = (line_1based - 1) * spl;
  const SampleFixed blanking = MillivoltsToSampleFixed(0.0);
  for (int i = start_sample;
       i < end_sample && (line_base + i) < static_cast<int>(y_mv.size()); ++i) {
    if (y_mv[static_cast<std::size_t>(line_base) +
             static_cast<std::size_t>(i)] != blanking) {
      return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// BiphaseInjectionManagerTest — basic structure
// ---------------------------------------------------------------------------

class BiphaseInjectionManagerTest : public ::testing::Test {
 protected:
  BiphaseInjectionManager manager_;
  std::vector<std::string> errors_;

  bool RunProcessFrame(Standard standard, const Section& section) {
    auto y_mv = MakeBlankingBuffer(standard);
    std::vector<int> offsets, counts;
    BuildLineLayout(standard, &offsets, &counts);
    const auto frame_lines = BuildFrameTimingPrimitives(standard);
    const int aws = ActiveWindowStart(standard);
    const int awe = ActiveWindowEnd(standard);
    const TimingConstants timing = GetTimingConstants(standard);

    // Member-manager tests exercise CAV sections; the disc format is a
    // project-level setting supplied once per pass.
    manager_.SetProjectDiscType(DiscType::kCAV);
    return manager_.ProcessFrame(&y_mv, 0, offsets, counts, section, standard,
                                 timing.sample_rate_4fsc_hz, frame_lines, aws,
                                 awe, &errors_);
  }
};

// ---------------------------------------------------------------------------
// Section with no laserdisc injection should process without error.
// ---------------------------------------------------------------------------

TEST_F(BiphaseInjectionManagerTest, NoLaserdiscInjectionSucceeds) {
  Section section;
  section.name = "Plain";
  section.type = "progressive";
  section.section_type = SectionType::kProgrammeArea;
  section.duration_frames = 1;

  EXPECT_TRUE(RunProcessFrame(Standard::kPal, section));
  EXPECT_TRUE(errors_.empty());
}

// ---------------------------------------------------------------------------
// Reset clears section state so the next ProcessFrame reinitialises.
// ---------------------------------------------------------------------------

TEST_F(BiphaseInjectionManagerTest, ResetAllowsReinit) {
  Section::LineInjectionCode code;
  code.code_type = "lead_in";
  auto section =
      MakeLaserdiscSection(SectionType::kLeadIn, DiscType::kCAV, {code});

  EXPECT_TRUE(RunProcessFrame(Standard::kPal, section));
  manager_.Reset();
  // After reset, same section pointer re-initialises cleanly.
  EXPECT_TRUE(RunProcessFrame(Standard::kPal, section));
  EXPECT_TRUE(errors_.empty());
}

// ---------------------------------------------------------------------------
// A laserdisc section with no project disc_type set returns false with an
// error (the disc format is required project-wide).
// ---------------------------------------------------------------------------

TEST_F(BiphaseInjectionManagerTest, MissingProjectDiscTypeReturnsError) {
  Section section;
  section.name = "Bad";
  section.type = "progressive";
  section.section_type = SectionType::kLeadIn;
  Section::LineInjection inj;
  inj.type = "laserdisc";
  section.line_injections.push_back(inj);
  // Deliberately do not call SetProjectDiscType: project_disc_type_ is unknown.

  auto y_mv = MakeBlankingBuffer(Standard::kPal);
  std::vector<int> offsets, counts;
  BuildLineLayout(Standard::kPal, &offsets, &counts);
  const auto frame_lines = BuildFrameTimingPrimitives(Standard::kPal);
  const TimingConstants timing = GetTimingConstants(Standard::kPal);
  const bool ok =
      manager_.ProcessFrame(&y_mv, 0, offsets, counts, section, Standard::kPal,
                            timing.sample_rate_4fsc_hz, frame_lines, 177,
                            ActiveWindowEnd(Standard::kPal), &errors_);
  EXPECT_FALSE(ok);
  EXPECT_FALSE(errors_.empty());
}

// ---------------------------------------------------------------------------
// PAL CAV lead-in: lead_in code injected on lines 17 and 18.
// ---------------------------------------------------------------------------

TEST(BiphaseInjectionManagerPalCavTest, LeadInCodeAppearsOnCorrectLines) {
  Section::LineInjectionCode code;
  code.code_type = "lead_in";
  const auto section =
      MakeLaserdiscSection(SectionType::kLeadIn, DiscType::kCAV, {code});

  BiphaseInjectionManager manager;
  auto y_mv = MakeBlankingBuffer(Standard::kPal);
  std::vector<int> offsets, counts;
  BuildLineLayout(Standard::kPal, &offsets, &counts);
  const auto frame_lines = BuildFrameTimingPrimitives(Standard::kPal);
  const TimingConstants timing = GetTimingConstants(Standard::kPal);
  std::vector<std::string> errors;

  manager.SetProjectDiscType(DiscType::kCAV);
  ASSERT_TRUE(manager.ProcessFrame(&y_mv, 0, offsets, counts, section,
                                   Standard::kPal, timing.sample_rate_4fsc_hz,
                                   frame_lines, 177,
                                   ActiveWindowEnd(Standard::kPal), &errors));
  EXPECT_TRUE(errors.empty());

  const int aws = 177;
  const int end = timing.samples_per_line_4fsc;

  // Lines 17 and 18 (field 1) carry lead_in for PAL CAV.
  EXPECT_TRUE(LineHasSignal(y_mv, Standard::kPal, 17, aws, end));
  EXPECT_TRUE(LineHasSignal(y_mv, Standard::kPal, 18, aws, end));

  // Lines outside the biphase range should remain at blanking.
  EXPECT_FALSE(LineHasSignal(y_mv, Standard::kPal, 19, aws, end));
  EXPECT_FALSE(LineHasSignal(y_mv, Standard::kPal, 15, aws, end));
}

// ---------------------------------------------------------------------------
// PAL CAV lead-in: signal levels swing blanking (0 mV) to 100% white (700 mV).
// IEC 60856 Figure 14: zero level is at blanking; §10.1 "30%–100%" is the
// allowed range for the high level only.
// ---------------------------------------------------------------------------

TEST(BiphaseInjectionManagerPalCavTest, LeadInSignalLevelsWithinSpec) {
  Section::LineInjectionCode code;
  code.code_type = "lead_in";
  const auto section =
      MakeLaserdiscSection(SectionType::kLeadIn, DiscType::kCAV, {code});

  BiphaseInjectionManager manager;
  auto y_mv = MakeBlankingBuffer(Standard::kPal);
  std::vector<int> offsets, counts;
  BuildLineLayout(Standard::kPal, &offsets, &counts);
  const auto frame_lines = BuildFrameTimingPrimitives(Standard::kPal);
  const TimingConstants timing = GetTimingConstants(Standard::kPal);
  std::vector<std::string> errors;

  manager.SetProjectDiscType(DiscType::kCAV);
  ASSERT_TRUE(manager.ProcessFrame(&y_mv, 0, offsets, counts, section,
                                   Standard::kPal, timing.sample_rate_4fsc_hz,
                                   frame_lines, 177,
                                   ActiveWindowEnd(Standard::kPal), &errors));

  const int aws = 177;
  const int awe = ActiveWindowEnd(Standard::kPal);

  // Baseline (digital low) should be ~0 mV (blanking); peak should be ~700 mV.
  const double min_mv = MinMvOnLine(y_mv, Standard::kPal, 17, aws, awe);
  const double max_mv = MaxMvOnLine(y_mv, Standard::kPal, 17, aws, awe);

  EXPECT_NEAR(min_mv, 0.0, 5.0);
  EXPECT_NEAR(max_mv, 700.0, 5.0);
}

// ---------------------------------------------------------------------------
// PAL CAV lead-out: lead_out code injected on field-2 lines 330 and 331.
// ---------------------------------------------------------------------------

TEST(BiphaseInjectionManagerPalCavTest, LeadOutCodeAppearsOnField2Lines) {
  Section::LineInjectionCode code;
  code.code_type = "lead_out";
  const auto section =
      MakeLaserdiscSection(SectionType::kLeadOut, DiscType::kCAV, {code});

  BiphaseInjectionManager manager;
  auto y_mv = MakeBlankingBuffer(Standard::kPal);
  std::vector<int> offsets, counts;
  BuildLineLayout(Standard::kPal, &offsets, &counts);
  const auto frame_lines = BuildFrameTimingPrimitives(Standard::kPal);
  const TimingConstants timing = GetTimingConstants(Standard::kPal);
  std::vector<std::string> errors;

  manager.SetProjectDiscType(DiscType::kCAV);
  ASSERT_TRUE(manager.ProcessFrame(&y_mv, 0, offsets, counts, section,
                                   Standard::kPal, timing.sample_rate_4fsc_hz,
                                   frame_lines, 177,
                                   ActiveWindowEnd(Standard::kPal), &errors));

  const int aws = 177;
  const int end = timing.samples_per_line_4fsc;

  // Lines 330 and 331 are field-2 biphase lines for PAL CAV.
  EXPECT_TRUE(LineHasSignal(y_mv, Standard::kPal, 330, aws, end));
  EXPECT_TRUE(LineHasSignal(y_mv, Standard::kPal, 331, aws, end));
}

// ---------------------------------------------------------------------------
// PAL CAV programme area: picture_number auto-increments across frames.
// ---------------------------------------------------------------------------

TEST(BiphaseInjectionManagerPalCavTest, PictureNumberIncrementsAcrossFrames) {
  Section::LineInjectionCode code;
  code.code_type = "picture_number";
  code.start_value = 1;
  code.start_value_specified = true;
  const auto section =
      MakeLaserdiscSection(SectionType::kProgrammeArea, DiscType::kCAV, {code});

  BiphaseInjectionManager manager;
  std::vector<int> offsets, counts;
  BuildLineLayout(Standard::kPal, &offsets, &counts);
  const auto frame_lines = BuildFrameTimingPrimitives(Standard::kPal);
  const TimingConstants timing = GetTimingConstants(Standard::kPal);
  const int aws = 177;
  const int awe = ActiveWindowEnd(Standard::kPal);

  // Generate two frames and capture the Y buffer state for line 17 each time.
  auto y1 = MakeBlankingBuffer(Standard::kPal);
  std::vector<std::string> errors1;
  manager.SetProjectDiscType(DiscType::kCAV);
  ASSERT_TRUE(manager.ProcessFrame(&y1, 0, offsets, counts, section,
                                   Standard::kPal, timing.sample_rate_4fsc_hz,
                                   frame_lines, aws, awe, &errors1));

  auto y2 = MakeBlankingBuffer(Standard::kPal);
  std::vector<std::string> errors2;
  manager.SetProjectDiscType(DiscType::kCAV);
  ASSERT_TRUE(manager.ProcessFrame(&y2, 0, offsets, counts, section,
                                   Standard::kPal, timing.sample_rate_4fsc_hz,
                                   frame_lines, aws, awe, &errors2));

  // Both frames must have signal on the picture_number lines.
  const int end = timing.samples_per_line_4fsc;
  EXPECT_TRUE(LineHasSignal(y1, Standard::kPal, 17, aws, end));
  EXPECT_TRUE(LineHasSignal(y2, Standard::kPal, 17, aws, end));

  // The encoded codes differ because the picture number advanced by one.
  // Compare raw sample values on line 17 between the two frames.
  const int spl = timing.samples_per_line_4fsc;
  const int line17_base = (17 - 1) * spl;
  bool any_sample_differs = false;
  for (int i = aws; i < spl && !any_sample_differs; ++i) {
    if (y1[static_cast<std::size_t>(line17_base) +
           static_cast<std::size_t>(i)] !=
        y2[static_cast<std::size_t>(line17_base) +
           static_cast<std::size_t>(i)]) {
      any_sample_differs = true;
    }
  }
  EXPECT_TRUE(any_sample_differs);
}

// ---------------------------------------------------------------------------
// PAL CAV: picture_number continues across section boundaries unless a
// section re-anchors it with an explicit start_value.
// ---------------------------------------------------------------------------

namespace {

// Processes one PAL CAV frame of `section` and returns the CAV picture number
// stamped on it (0 when no picture_number generator is active).
int ProcessCavFrameGetPictureNumber(BiphaseInjectionManager* manager,
                                    const Section& section) {
  std::vector<int> offsets, counts;
  BuildLineLayout(Standard::kPal, &offsets, &counts);
  const auto frame_lines = BuildFrameTimingPrimitives(Standard::kPal);
  const TimingConstants timing = GetTimingConstants(Standard::kPal);
  auto y = MakeBlankingBuffer(Standard::kPal);
  std::vector<std::string> errors;
  EXPECT_TRUE(manager->ProcessFrame(&y, 0, offsets, counts, section,
                                    Standard::kPal, timing.sample_rate_4fsc_hz,
                                    frame_lines, 177,
                                    ActiveWindowEnd(Standard::kPal), &errors));
  return manager->GetLastFrameContext().picture_number;
}

}  // namespace

TEST(BiphaseInjectionManagerPalCavTest, PictureNumberContinuesAcrossSections) {
  // Section A anchors numbering at 1; section B (a distinct Section, so the
  // manager re-initialises) omits start_value and must continue from where A
  // left off rather than restart at 1.
  Section::LineInjectionCode anchored;
  anchored.code_type = "picture_number";
  anchored.start_value = 1;
  anchored.start_value_specified = true;
  const Section section_a = MakeLaserdiscSection(SectionType::kProgrammeArea,
                                                 DiscType::kCAV, {anchored});

  Section::LineInjectionCode continued;
  continued.code_type = "picture_number";  // start_value left unspecified
  const Section section_b = MakeLaserdiscSection(SectionType::kProgrammeArea,
                                                 DiscType::kCAV, {continued});

  BiphaseInjectionManager manager;
  manager.SetProjectDiscType(DiscType::kCAV);

  EXPECT_EQ(ProcessCavFrameGetPictureNumber(&manager, section_a), 1);
  EXPECT_EQ(ProcessCavFrameGetPictureNumber(&manager, section_a), 2);
  EXPECT_EQ(ProcessCavFrameGetPictureNumber(&manager, section_a), 3);
  // Crossing into section B without a start_value continues the count.
  EXPECT_EQ(ProcessCavFrameGetPictureNumber(&manager, section_b), 4);
  EXPECT_EQ(ProcessCavFrameGetPictureNumber(&manager, section_b), 5);
}

TEST(BiphaseInjectionManagerPalCavTest,
     PictureNumberReanchorsOnExplicitStartValue) {
  Section::LineInjectionCode from_one;
  from_one.code_type = "picture_number";
  from_one.start_value = 1;
  from_one.start_value_specified = true;
  const Section section_a = MakeLaserdiscSection(SectionType::kProgrammeArea,
                                                 DiscType::kCAV, {from_one});

  Section::LineInjectionCode from_hundred;
  from_hundred.code_type = "picture_number";
  from_hundred.start_value = 100;
  from_hundred.start_value_specified = true;
  const Section section_b = MakeLaserdiscSection(
      SectionType::kProgrammeArea, DiscType::kCAV, {from_hundred});

  BiphaseInjectionManager manager;
  manager.SetProjectDiscType(DiscType::kCAV);

  EXPECT_EQ(ProcessCavFrameGetPictureNumber(&manager, section_a), 1);
  EXPECT_EQ(ProcessCavFrameGetPictureNumber(&manager, section_a), 2);
  // An explicit start_value re-anchors the count for the new section.
  EXPECT_EQ(ProcessCavFrameGetPictureNumber(&manager, section_b), 100);
  EXPECT_EQ(ProcessCavFrameGetPictureNumber(&manager, section_b), 101);
}

// ---------------------------------------------------------------------------
// PAL CAV: chapter_number continues across section boundaries unless a
// section re-anchors it with an explicit chapter.
// ---------------------------------------------------------------------------

namespace {

// Processes one PAL CAV frame of `section` and returns the chapter number
// stamped on it, decoded from the chapter biphase word 8X₁X₂DDD (-1 when no
// chapter_number generator is active).
int ProcessCavFrameGetChapterNumber(BiphaseInjectionManager* manager,
                                    const Section& section) {
  std::vector<int> offsets, counts;
  BuildLineLayout(Standard::kPal, &offsets, &counts);
  const auto frame_lines = BuildFrameTimingPrimitives(Standard::kPal);
  const TimingConstants timing = GetTimingConstants(Standard::kPal);
  auto y = MakeBlankingBuffer(Standard::kPal);
  std::vector<std::string> errors;
  EXPECT_TRUE(manager->ProcessFrame(&y, 0, offsets, counts, section,
                                    Standard::kPal, timing.sample_rate_4fsc_hz,
                                    frame_lines, 177,
                                    ActiveWindowEnd(Standard::kPal), &errors));
  for (const uint32_t word : manager->GetLastFrameContext().biphase_words) {
    // IEC 60856 §10.1.5: chapter codes end in the constant DDD pattern.
    if ((word & 0x000FFFu) == ChapterNumberGenerator::kDddPattern) {
      return ChapterNumberGenerator::DecodeChapterNumber(word);
    }
  }
  return -1;
}

}  // namespace

TEST(BiphaseInjectionManagerPalCavTest, ChapterNumberContinuesAcrossSections) {
  // Section A starts chapter 5; section B (a distinct Section, so the manager
  // re-initialises) omits the chapter and must continue chapter 5 rather than
  // restart at 0.
  Section::LineInjectionCode anchored;
  anchored.code_type = "chapter_number";
  anchored.chapter = 5;
  anchored.chapter_specified = true;
  const Section section_a = MakeLaserdiscSection(SectionType::kProgrammeArea,
                                                 DiscType::kCAV, {anchored});

  Section::LineInjectionCode continued;
  continued.code_type = "chapter_number";  // chapter left unspecified
  const Section section_b = MakeLaserdiscSection(SectionType::kProgrammeArea,
                                                 DiscType::kCAV, {continued});

  BiphaseInjectionManager manager;
  manager.SetProjectDiscType(DiscType::kCAV);

  EXPECT_EQ(ProcessCavFrameGetChapterNumber(&manager, section_a), 5);
  // Crossing into section B without a chapter continues the chapter.
  EXPECT_EQ(ProcessCavFrameGetChapterNumber(&manager, section_b), 5);
  EXPECT_EQ(ProcessCavFrameGetChapterNumber(&manager, section_b), 5);
}

TEST(BiphaseInjectionManagerPalCavTest,
     ChapterNumberReanchorsOnExplicitChapter) {
  Section::LineInjectionCode chapter_five;
  chapter_five.code_type = "chapter_number";
  chapter_five.chapter = 5;
  chapter_five.chapter_specified = true;
  const Section section_a = MakeLaserdiscSection(
      SectionType::kProgrammeArea, DiscType::kCAV, {chapter_five});

  Section::LineInjectionCode chapter_six;
  chapter_six.code_type = "chapter_number";
  chapter_six.chapter = 6;
  chapter_six.chapter_specified = true;
  const Section section_b = MakeLaserdiscSection(SectionType::kProgrammeArea,
                                                 DiscType::kCAV, {chapter_six});

  BiphaseInjectionManager manager;
  manager.SetProjectDiscType(DiscType::kCAV);

  EXPECT_EQ(ProcessCavFrameGetChapterNumber(&manager, section_a), 5);
  // An explicit chapter starts the new chapter at the section boundary.
  EXPECT_EQ(ProcessCavFrameGetChapterNumber(&manager, section_b), 6);
}

TEST(BiphaseInjectionManagerPalCavTest,
     ChapterNumberDefaultsToZeroWhenNeverSpecified) {
  Section::LineInjectionCode unspecified;
  unspecified.code_type = "chapter_number";  // chapter left unspecified
  const Section section = MakeLaserdiscSection(SectionType::kProgrammeArea,
                                               DiscType::kCAV, {unspecified});

  BiphaseInjectionManager manager;
  manager.SetProjectDiscType(DiscType::kCAV);

  // No earlier section carried a chapter: numbering begins at 0.
  EXPECT_EQ(ProcessCavFrameGetChapterNumber(&manager, section), 0);
}

// ---------------------------------------------------------------------------
// Chapter stop-bit exceptions (IEC 60856/60857 §10.1.5).
// ---------------------------------------------------------------------------

namespace {

Section MakeChapterSection(SectionType section_type, int chapter,
                           bool chapter_specified) {
  Section::LineInjectionCode code;
  code.code_type = "chapter_number";
  code.chapter = chapter;
  code.chapter_specified = chapter_specified;
  return MakeLaserdiscSection(section_type, DiscType::kCAV, {code});
}

Section MakeLeadInSection() {
  Section::LineInjectionCode code;
  code.code_type = "lead_in";
  return MakeLaserdiscSection(SectionType::kLeadIn, DiscType::kCAV, {code});
}

// Processes one PAL CAV frame of `section` and returns the stop-bit of the
// chapter code stamped on it.
bool ProcessCavFrameGetStopBit(BiphaseInjectionManager* manager,
                               const Section& section) {
  std::vector<int> offsets, counts;
  BuildLineLayout(Standard::kPal, &offsets, &counts);
  const auto frame_lines = BuildFrameTimingPrimitives(Standard::kPal);
  const TimingConstants timing = GetTimingConstants(Standard::kPal);
  auto y = MakeBlankingBuffer(Standard::kPal);
  std::vector<std::string> errors;
  EXPECT_TRUE(manager->ProcessFrame(&y, 0, offsets, counts, section,
                                    Standard::kPal, timing.sample_rate_4fsc_hz,
                                    frame_lines, 177,
                                    ActiveWindowEnd(Standard::kPal), &errors));
  for (const uint32_t word : manager->GetLastFrameContext().biphase_words) {
    if ((word & 0x000FFFu) == ChapterNumberGenerator::kDddPattern) {
      return ChapterNumberGenerator::DecodeStopBit(word);
    }
  }
  ADD_FAILURE() << "No chapter code on the frame";
  return false;
}

}  // namespace

TEST(ChapterStopBitResolutionTest, FirstChapterAfterLeadInIsAlwaysOne) {
  const Section lead_in = MakeLeadInSection();
  const Section first =
      MakeChapterSection(SectionType::kProgrammeArea, 1, true);
  const Section second =
      MakeChapterSection(SectionType::kProgrammeArea, 2, true);
  const std::vector<bool> result = ResolveChapterStopBitOne(
      {{&lead_in, 100}, {&first, 1000}, {&second, 1000}}, DiscType::kCAV);
  EXPECT_EQ(result, (std::vector<bool>{true, false}));
}

TEST(ChapterStopBitResolutionTest, FirstChapterWithoutLeadInFollowsNormalRule) {
  const Section first =
      MakeChapterSection(SectionType::kProgrammeArea, 1, true);
  const Section second =
      MakeChapterSection(SectionType::kProgrammeArea, 2, true);
  const std::vector<bool> result = ResolveChapterStopBitOne(
      {{&first, 1000}, {&second, 1000}}, DiscType::kCAV);
  EXPECT_EQ(result, (std::vector<bool>{false, false}));
}

TEST(ChapterStopBitResolutionTest, ShortCavChapterMakesEveryChapterOne) {
  const Section first =
      MakeChapterSection(SectionType::kProgrammeArea, 1, true);
  const Section second =
      MakeChapterSection(SectionType::kProgrammeArea, 2, true);
  const Section third =
      MakeChapterSection(SectionType::kProgrammeArea, 3, true);
  const std::vector<bool> result = ResolveChapterStopBitOne(
      {{&first, 5000}, {&second, 799}, {&third, 5000}}, DiscType::kCAV);
  EXPECT_EQ(result, (std::vector<bool>{true, true, true}));
}

TEST(ChapterStopBitResolutionTest, ContinuedChapterCountsAllItsSections) {
  // 500 + 300 frames of one chapter is 800 tracks: not short.
  const Section start =
      MakeChapterSection(SectionType::kProgrammeArea, 1, true);
  const Section continued =
      MakeChapterSection(SectionType::kProgrammeArea, 0, false);
  const Section next = MakeChapterSection(SectionType::kProgrammeArea, 2, true);
  const std::vector<bool> result = ResolveChapterStopBitOne(
      {{&start, 500}, {&continued, 300}, {&next, 800}}, DiscType::kCAV);
  EXPECT_EQ(result, (std::vector<bool>{false, false}));
}

TEST(ChapterStopBitResolutionTest, ShortChapterRuleIsNotAppliedToClv) {
  const Section first =
      MakeChapterSection(SectionType::kProgrammeArea, 1, true);
  const Section second =
      MakeChapterSection(SectionType::kProgrammeArea, 2, true);
  const std::vector<bool> result =
      ResolveChapterStopBitOne({{&first, 100}, {&second, 100}}, DiscType::kCLV);
  EXPECT_EQ(result, (std::vector<bool>{false, false}));
}

TEST(BiphaseInjectionManagerPalCavTest, ChapterStopBitFollowsResolvedList) {
  const Section first =
      MakeChapterSection(SectionType::kProgrammeArea, 1, true);
  const Section second =
      MakeChapterSection(SectionType::kProgrammeArea, 2, true);

  BiphaseInjectionManager manager;
  manager.SetProjectDiscType(DiscType::kCAV);
  manager.SetChapterStopBitOne({true, false});

  EXPECT_TRUE(ProcessCavFrameGetStopBit(&manager, first));
  EXPECT_FALSE(ProcessCavFrameGetStopBit(&manager, second));
}

// ---------------------------------------------------------------------------
// PAL CLV programme area: programme_time_code appears on correct lines.
// ---------------------------------------------------------------------------

TEST(BiphaseInjectionManagerPalClvTest,
     ProgrammeTimeCodeAppearsOnCorrectLines) {
  Section::LineInjectionCode code;
  code.code_type = "programme_time_code";
  const auto section =
      MakeLaserdiscSection(SectionType::kProgrammeArea, DiscType::kCLV, {code});

  BiphaseInjectionManager manager;
  auto y_mv = MakeBlankingBuffer(Standard::kPal);
  std::vector<int> offsets, counts;
  BuildLineLayout(Standard::kPal, &offsets, &counts);
  const auto frame_lines = BuildFrameTimingPrimitives(Standard::kPal);
  const TimingConstants timing = GetTimingConstants(Standard::kPal);
  std::vector<std::string> errors;

  manager.SetProjectDiscType(DiscType::kCLV);
  ASSERT_TRUE(manager.ProcessFrame(&y_mv, 0, offsets, counts, section,
                                   Standard::kPal, timing.sample_rate_4fsc_hz,
                                   frame_lines, 177,
                                   ActiveWindowEnd(Standard::kPal), &errors));
  EXPECT_TRUE(errors.empty());

  const int aws = 177;
  const int end = timing.samples_per_line_4fsc;

  // programme_time_code appears on lines 17 and 18 (field-1 priority).
  EXPECT_TRUE(LineHasSignal(y_mv, Standard::kPal, 17, aws, end));
  EXPECT_TRUE(LineHasSignal(y_mv, Standard::kPal, 18, aws, end));
}

// ---------------------------------------------------------------------------
// PAL CLV programme area: clv_picture_number appears on line 16.
// ---------------------------------------------------------------------------

TEST(BiphaseInjectionManagerPalClvTest, ClvPictureNumberAppearsOnLine16) {
  std::vector<Section::LineInjectionCode> codes;
  Section::LineInjectionCode ptc;
  ptc.code_type = "programme_time_code";
  codes.push_back(ptc);
  Section::LineInjectionCode cpn;
  cpn.code_type = "clv_picture_number";
  codes.push_back(cpn);

  const auto section =
      MakeLaserdiscSection(SectionType::kProgrammeArea, DiscType::kCLV, codes);

  BiphaseInjectionManager manager;
  auto y_mv = MakeBlankingBuffer(Standard::kPal);
  std::vector<int> offsets, counts;
  BuildLineLayout(Standard::kPal, &offsets, &counts);
  const auto frame_lines = BuildFrameTimingPrimitives(Standard::kPal);
  const TimingConstants timing = GetTimingConstants(Standard::kPal);
  std::vector<std::string> errors;

  manager.SetProjectDiscType(DiscType::kCLV);
  ASSERT_TRUE(manager.ProcessFrame(&y_mv, 0, offsets, counts, section,
                                   Standard::kPal, timing.sample_rate_4fsc_hz,
                                   frame_lines, 177,
                                   ActiveWindowEnd(Standard::kPal), &errors));
  EXPECT_TRUE(errors.empty());

  const int aws = 177;
  const int end = timing.samples_per_line_4fsc;
  EXPECT_TRUE(LineHasSignal(y_mv, Standard::kPal, 16, aws, end));
}

// ---------------------------------------------------------------------------
// PAL CAV: programme_status uses 0.172 H horizontal offset on line 16.
// ---------------------------------------------------------------------------

TEST(BiphaseInjectionManagerPalCavTest, ProgrammeStatusUses172HOffset) {
  std::vector<Section::LineInjectionCode> codes;
  Section::LineInjectionCode pn;
  pn.code_type = "picture_number";
  pn.start_value = 1;
  pn.start_value_specified = true;
  codes.push_back(pn);
  Section::LineInjectionCode ps;
  ps.code_type = "programme_status";
  ps.programme_status = "8DC000";
  ps.programme_status_specified = true;
  codes.push_back(ps);

  const auto section =
      MakeLaserdiscSection(SectionType::kProgrammeArea, DiscType::kCAV, codes);

  BiphaseInjectionManager manager;
  const TimingConstants timing = GetTimingConstants(Standard::kPal);
  const int spl = timing.samples_per_line_4fsc;
  auto y_mv = MakeBlankingBuffer(Standard::kPal);
  std::vector<int> offsets, counts;
  BuildLineLayout(Standard::kPal, &offsets, &counts);
  const auto frame_lines = BuildFrameTimingPrimitives(Standard::kPal);
  const int aws = 177;
  const int awe = ActiveWindowEnd(Standard::kPal);
  std::vector<std::string> errors;

  manager.SetProjectDiscType(DiscType::kCAV);
  ASSERT_TRUE(manager.ProcessFrame(&y_mv, 0, offsets, counts, section,
                                   Standard::kPal, timing.sample_rate_4fsc_hz,
                                   frame_lines, aws, awe, &errors));
  EXPECT_TRUE(errors.empty());

  // 0.172 H offset for PAL: round(0.172 × 1135) = 195
  const int offset_172h = static_cast<int>(std::round(0.172 * spl));

  // Line 16 carries programme_status. The signal must not start at sample
  // aws (177) but at offset_172h (195). Samples in [aws, offset_172h)
  // are at blanking level (0 mV) since no biphase baseline is written before
  // the 0.172 H horizontal start position.
  const double mean_before =
      MeanMvOnLine(y_mv, Standard::kPal, 16, aws, offset_172h);
  EXPECT_NEAR(mean_before, 0.0, 1.0);

  // Signal region (first bit cell starts at offset_172h) must have content.
  EXPECT_TRUE(LineHasSignal(y_mv, Standard::kPal, 16, offset_172h, spl));
}

// ---------------------------------------------------------------------------
// PAL CLV: clv_code uses 0.172 H offset (NTSC only; PAL has no offset).
// PAL programme_status does use 0.172 H offset.
// ---------------------------------------------------------------------------

TEST(BiphaseInjectionManagerPalClvTest, ClvCodeAppearsWithNoOffset) {
  std::vector<Section::LineInjectionCode> codes;
  Section::LineInjectionCode clvc;
  clvc.code_type = "clv_code";
  codes.push_back(clvc);

  const auto section =
      MakeLaserdiscSection(SectionType::kProgrammeArea, DiscType::kCLV, codes);

  BiphaseInjectionManager manager;
  const TimingConstants timing = GetTimingConstants(Standard::kPal);
  auto y_mv = MakeBlankingBuffer(Standard::kPal);
  std::vector<int> offsets, counts;
  BuildLineLayout(Standard::kPal, &offsets, &counts);
  const auto frame_lines = BuildFrameTimingPrimitives(Standard::kPal);
  std::vector<std::string> errors;

  manager.SetProjectDiscType(DiscType::kCLV);
  ASSERT_TRUE(manager.ProcessFrame(&y_mv, 0, offsets, counts, section,
                                   Standard::kPal, timing.sample_rate_4fsc_hz,
                                   frame_lines, 177,
                                   ActiveWindowEnd(Standard::kPal), &errors));
  EXPECT_TRUE(errors.empty());

  const int aws = 177;
  const int end = timing.samples_per_line_4fsc;
  EXPECT_TRUE(LineHasSignal(y_mv, Standard::kPal, 17, aws, end));
}

// ---------------------------------------------------------------------------
// NTSC CAV lead-in: lead_in code injected on lines 17 and 18.
// ---------------------------------------------------------------------------

TEST(BiphaseInjectionManagerNtscCavTest, LeadInCodeAppearsOnField1Lines) {
  Section::LineInjectionCode code;
  code.code_type = "lead_in";
  const auto section =
      MakeLaserdiscSection(SectionType::kLeadIn, DiscType::kCAV, {code});

  BiphaseInjectionManager manager;
  auto y_mv = MakeBlankingBuffer(Standard::kNtsc);
  std::vector<int> offsets, counts;
  BuildLineLayout(Standard::kNtsc, &offsets, &counts);
  const auto frame_lines = BuildFrameTimingPrimitives(Standard::kNtsc);
  const TimingConstants timing = GetTimingConstants(Standard::kNtsc);
  const int aws = ActiveWindowStart(Standard::kNtsc);
  const int awe = ActiveWindowEnd(Standard::kNtsc);
  std::vector<std::string> errors;

  manager.SetProjectDiscType(DiscType::kCAV);
  ASSERT_TRUE(manager.ProcessFrame(&y_mv, 0, offsets, counts, section,
                                   Standard::kNtsc, timing.sample_rate_4fsc_hz,
                                   frame_lines, aws, awe, &errors));
  EXPECT_TRUE(errors.empty());

  const int end = timing.samples_per_line_4fsc;
  EXPECT_TRUE(LineHasSignal(y_mv, Standard::kNtsc, 17, aws, end));
  EXPECT_TRUE(LineHasSignal(y_mv, Standard::kNtsc, 18, aws, end));
  EXPECT_FALSE(LineHasSignal(y_mv, Standard::kNtsc, 19, aws, end));
}

// ---------------------------------------------------------------------------
// NTSC CAV: signal levels are 0–714 mV (0–100 IRE).
// ---------------------------------------------------------------------------

TEST(BiphaseInjectionManagerNtscCavTest, SignalLevelsWithinSpec) {
  Section::LineInjectionCode code;
  code.code_type = "lead_in";
  const auto section =
      MakeLaserdiscSection(SectionType::kLeadIn, DiscType::kCAV, {code});

  BiphaseInjectionManager manager;
  auto y_mv = MakeBlankingBuffer(Standard::kNtsc);
  std::vector<int> offsets, counts;
  BuildLineLayout(Standard::kNtsc, &offsets, &counts);
  const auto frame_lines = BuildFrameTimingPrimitives(Standard::kNtsc);
  const TimingConstants timing = GetTimingConstants(Standard::kNtsc);
  const int aws = ActiveWindowStart(Standard::kNtsc);
  const int awe = ActiveWindowEnd(Standard::kNtsc);
  std::vector<std::string> errors;

  manager.SetProjectDiscType(DiscType::kCAV);
  ASSERT_TRUE(manager.ProcessFrame(&y_mv, 0, offsets, counts, section,
                                   Standard::kNtsc, timing.sample_rate_4fsc_hz,
                                   frame_lines, aws, awe, &errors));

  const int end = timing.samples_per_line_4fsc;
  const double min_mv = MinMvOnLine(y_mv, Standard::kNtsc, 17, aws, end);
  const double max_mv = MaxMvOnLine(y_mv, Standard::kNtsc, 17, aws, end);

  // NTSC biphase: baseline = 0 mV, peak = 714.3 mV.
  EXPECT_NEAR(min_mv, 0.0, 5.0);
  EXPECT_NEAR(max_mv, 714.3, 5.0);
}

// ---------------------------------------------------------------------------
// NTSC CAV: FM picture number appears on FM lines (10 and 273).
// ---------------------------------------------------------------------------

TEST(BiphaseInjectionManagerNtscCavTest, FmPictureNumberAppearsOnFmLines) {
  std::vector<Section::LineInjectionCode> codes;
  Section::LineInjectionCode pn;
  pn.code_type = "picture_number";
  pn.start_value = 1;
  pn.start_value_specified = true;
  codes.push_back(pn);
  Section::LineInjectionCode fm;
  fm.code_type = "fm_picture_number";
  fm.start_value = 1;
  fm.start_value_specified = true;
  codes.push_back(fm);
  Section::LineInjectionCode wf;
  wf.code_type = "fm_white_flag";
  codes.push_back(wf);

  const auto section =
      MakeLaserdiscSection(SectionType::kProgrammeArea, DiscType::kCAV, codes);

  BiphaseInjectionManager manager;
  auto y_mv = MakeBlankingBuffer(Standard::kNtsc);
  std::vector<int> offsets, counts;
  BuildLineLayout(Standard::kNtsc, &offsets, &counts);
  const auto frame_lines = BuildFrameTimingPrimitives(Standard::kNtsc);
  const TimingConstants timing = GetTimingConstants(Standard::kNtsc);
  const int aws = ActiveWindowStart(Standard::kNtsc);
  const int awe = ActiveWindowEnd(Standard::kNtsc);
  std::vector<std::string> errors;

  manager.SetProjectDiscType(DiscType::kCAV);
  ASSERT_TRUE(manager.ProcessFrame(&y_mv, 0, offsets, counts, section,
                                   Standard::kNtsc, timing.sample_rate_4fsc_hz,
                                   frame_lines, aws, awe, &errors));
  EXPECT_TRUE(errors.empty());

  const int end = timing.samples_per_line_4fsc;
  // FM picture number on lines 10 (field 1) and 273 (field 2).
  EXPECT_TRUE(LineHasSignal(y_mv, Standard::kNtsc, 10, aws, end));
  EXPECT_TRUE(LineHasSignal(y_mv, Standard::kNtsc, 273, aws, end));
}

// ---------------------------------------------------------------------------
// NTSC CAV: white flag on line 11 (field 1) in programme area.
// ---------------------------------------------------------------------------

TEST(BiphaseInjectionManagerNtscCavTest, WhiteFlagAppearsOnLine11) {
  std::vector<Section::LineInjectionCode> codes;
  Section::LineInjectionCode pn;
  pn.code_type = "picture_number";
  pn.start_value = 1;
  pn.start_value_specified = true;
  codes.push_back(pn);
  Section::LineInjectionCode wf;
  wf.code_type = "fm_white_flag";
  codes.push_back(wf);

  const auto section =
      MakeLaserdiscSection(SectionType::kProgrammeArea, DiscType::kCAV, codes);

  BiphaseInjectionManager manager;
  auto y_mv = MakeBlankingBuffer(Standard::kNtsc);
  std::vector<int> offsets, counts;
  BuildLineLayout(Standard::kNtsc, &offsets, &counts);
  const auto frame_lines = BuildFrameTimingPrimitives(Standard::kNtsc);
  const TimingConstants timing = GetTimingConstants(Standard::kNtsc);
  const int aws = ActiveWindowStart(Standard::kNtsc);
  const int awe = ActiveWindowEnd(Standard::kNtsc);
  std::vector<std::string> errors;

  manager.SetProjectDiscType(DiscType::kCAV);
  ASSERT_TRUE(manager.ProcessFrame(&y_mv, 0, offsets, counts, section,
                                   Standard::kNtsc, timing.sample_rate_4fsc_hz,
                                   frame_lines, aws, awe, &errors));
  EXPECT_TRUE(errors.empty());

  const int end = timing.samples_per_line_4fsc;
  // White flag on line 11 (field 1) in programme area.
  EXPECT_TRUE(LineHasSignal(y_mv, Standard::kNtsc, 11, aws, end));
  // White flag should be at 100 IRE ≈ 714.3 mV.
  const double max_mv = MaxMvOnLine(y_mv, Standard::kNtsc, 11, aws, end);
  EXPECT_NEAR(max_mv, 714.3, 2.0);
}

// ---------------------------------------------------------------------------
// NTSC CLV: FM programme time code appears on FM lines (10 and 273).
// ---------------------------------------------------------------------------

TEST(BiphaseInjectionManagerNtscClvTest, FmProgrammeTimeAppearsOnFmLines) {
  std::vector<Section::LineInjectionCode> codes;
  Section::LineInjectionCode ptc;
  ptc.code_type = "programme_time_code";
  codes.push_back(ptc);
  Section::LineInjectionCode fmt;
  fmt.code_type = "fm_programme_time";
  codes.push_back(fmt);
  Section::LineInjectionCode wf;
  wf.code_type = "fm_white_flag";
  codes.push_back(wf);

  const auto section =
      MakeLaserdiscSection(SectionType::kProgrammeArea, DiscType::kCLV, codes);

  BiphaseInjectionManager manager;
  auto y_mv = MakeBlankingBuffer(Standard::kNtsc);
  std::vector<int> offsets, counts;
  BuildLineLayout(Standard::kNtsc, &offsets, &counts);
  const auto frame_lines = BuildFrameTimingPrimitives(Standard::kNtsc);
  const TimingConstants timing = GetTimingConstants(Standard::kNtsc);
  const int aws = ActiveWindowStart(Standard::kNtsc);
  const int awe = ActiveWindowEnd(Standard::kNtsc);
  std::vector<std::string> errors;

  manager.SetProjectDiscType(DiscType::kCLV);
  ASSERT_TRUE(manager.ProcessFrame(&y_mv, 0, offsets, counts, section,
                                   Standard::kNtsc, timing.sample_rate_4fsc_hz,
                                   frame_lines, aws, awe, &errors));
  EXPECT_TRUE(errors.empty());

  const int end = timing.samples_per_line_4fsc;
  EXPECT_TRUE(LineHasSignal(y_mv, Standard::kNtsc, 10, aws, end));
  EXPECT_TRUE(LineHasSignal(y_mv, Standard::kNtsc, 273, aws, end));
}

// ---------------------------------------------------------------------------
// NTSC lead-in: white flag only on line 11 (not line 274).
// ---------------------------------------------------------------------------

TEST(BiphaseInjectionManagerNtscCavTest, WhiteFlagLeadInOnlyLine11) {
  std::vector<Section::LineInjectionCode> codes;
  Section::LineInjectionCode li;
  li.code_type = "lead_in";
  codes.push_back(li);
  Section::LineInjectionCode wf;
  wf.code_type = "fm_white_flag";
  codes.push_back(wf);

  const auto section =
      MakeLaserdiscSection(SectionType::kLeadIn, DiscType::kCAV, codes);

  BiphaseInjectionManager manager;
  auto y_mv = MakeBlankingBuffer(Standard::kNtsc);
  std::vector<int> offsets, counts;
  BuildLineLayout(Standard::kNtsc, &offsets, &counts);
  const auto frame_lines = BuildFrameTimingPrimitives(Standard::kNtsc);
  const TimingConstants timing = GetTimingConstants(Standard::kNtsc);
  const int aws = ActiveWindowStart(Standard::kNtsc);
  const int awe = ActiveWindowEnd(Standard::kNtsc);
  std::vector<std::string> errors;

  manager.SetProjectDiscType(DiscType::kCAV);
  ASSERT_TRUE(manager.ProcessFrame(&y_mv, 0, offsets, counts, section,
                                   Standard::kNtsc, timing.sample_rate_4fsc_hz,
                                   frame_lines, aws, awe, &errors));
  EXPECT_TRUE(errors.empty());

  const int end = timing.samples_per_line_4fsc;
  // Per IEC 60857 §10.2.1: white flag in lead-in on line 11 only.
  EXPECT_TRUE(LineHasSignal(y_mv, Standard::kNtsc, 11, aws, end));
  EXPECT_FALSE(LineHasSignal(y_mv, Standard::kNtsc, 274, aws, end));
}

// ---------------------------------------------------------------------------
// NTSC lead-out: white flag on both lines 11 and 274.
// ---------------------------------------------------------------------------

TEST(BiphaseInjectionManagerNtscCavTest, WhiteFlagLeadOutBothLines) {
  std::vector<Section::LineInjectionCode> codes;
  Section::LineInjectionCode lo;
  lo.code_type = "lead_out";
  codes.push_back(lo);
  Section::LineInjectionCode wf;
  wf.code_type = "fm_white_flag";
  codes.push_back(wf);

  const auto section =
      MakeLaserdiscSection(SectionType::kLeadOut, DiscType::kCAV, codes);

  BiphaseInjectionManager manager;
  auto y_mv = MakeBlankingBuffer(Standard::kNtsc);
  std::vector<int> offsets, counts;
  BuildLineLayout(Standard::kNtsc, &offsets, &counts);
  const auto frame_lines = BuildFrameTimingPrimitives(Standard::kNtsc);
  const TimingConstants timing = GetTimingConstants(Standard::kNtsc);
  const int aws = ActiveWindowStart(Standard::kNtsc);
  const int awe = ActiveWindowEnd(Standard::kNtsc);
  std::vector<std::string> errors;

  manager.SetProjectDiscType(DiscType::kCAV);
  ASSERT_TRUE(manager.ProcessFrame(&y_mv, 0, offsets, counts, section,
                                   Standard::kNtsc, timing.sample_rate_4fsc_hz,
                                   frame_lines, aws, awe, &errors));
  EXPECT_TRUE(errors.empty());

  const int end = timing.samples_per_line_4fsc;
  // Per IEC 60857 §10.2.2: white flag in lead-out on both lines 11 and 274.
  EXPECT_TRUE(LineHasSignal(y_mv, Standard::kNtsc, 11, aws, end));
  EXPECT_TRUE(LineHasSignal(y_mv, Standard::kNtsc, 274, aws, end));
}

// ---------------------------------------------------------------------------
// Section transition: switching from lead_in to programme_area reinitialises.
// ---------------------------------------------------------------------------

TEST(BiphaseInjectionManagerPalCavTest, SectionTransitionReinitialises) {
  BiphaseInjectionManager manager;
  const TimingConstants timing = GetTimingConstants(Standard::kPal);
  const int aws = 177;
  const int awe = ActiveWindowEnd(Standard::kPal);
  std::vector<int> offsets, counts;
  BuildLineLayout(Standard::kPal, &offsets, &counts);
  const auto frame_lines = BuildFrameTimingPrimitives(Standard::kPal);

  // Process one lead_in frame.
  Section::LineInjectionCode li_code;
  li_code.code_type = "lead_in";
  const auto lead_in_section =
      MakeLaserdiscSection(SectionType::kLeadIn, DiscType::kCAV, {li_code});

  auto y1 = MakeBlankingBuffer(Standard::kPal);
  std::vector<std::string> errors1;
  manager.SetProjectDiscType(DiscType::kCAV);
  ASSERT_TRUE(manager.ProcessFrame(&y1, 0, offsets, counts, lead_in_section,
                                   Standard::kPal, timing.sample_rate_4fsc_hz,
                                   frame_lines, aws, awe, &errors1));

  // Process one programme_area frame (picture_number).
  Section::LineInjectionCode pn_code;
  pn_code.code_type = "picture_number";
  pn_code.start_value = 1;
  pn_code.start_value_specified = true;
  const auto prog_section = MakeLaserdiscSection(SectionType::kProgrammeArea,
                                                 DiscType::kCAV, {pn_code});

  auto y2 = MakeBlankingBuffer(Standard::kPal);
  std::vector<std::string> errors2;
  manager.SetProjectDiscType(DiscType::kCAV);
  ASSERT_TRUE(manager.ProcessFrame(&y2, 0, offsets, counts, prog_section,
                                   Standard::kPal, timing.sample_rate_4fsc_hz,
                                   frame_lines, aws, awe, &errors2));

  EXPECT_TRUE(errors1.empty());
  EXPECT_TRUE(errors2.empty());

  const int end = timing.samples_per_line_4fsc;
  // Programme frame must carry picture_number signal.
  EXPECT_TRUE(LineHasSignal(y2, Standard::kPal, 17, aws, end));
}

// ---------------------------------------------------------------------------
// Users code appears on correct lines in lead_in (PAL CAV).
// ---------------------------------------------------------------------------

TEST(BiphaseInjectionManagerPalCavTest, UsersCodeInLeadIn) {
  Section::LineInjectionCode li;
  li.code_type = "lead_in";
  Section::LineInjectionCode uc;
  uc.code_type = "users_code";
  uc.users_code = "0x80D234";  // X1=0, D=0xD (canonical per IEC §10.1.9)
  uc.users_code_specified = true;

  const auto section =
      MakeLaserdiscSection(SectionType::kLeadIn, DiscType::kCAV, {li, uc});

  BiphaseInjectionManager manager;
  auto y_mv = MakeBlankingBuffer(Standard::kPal);
  std::vector<int> offsets, counts;
  BuildLineLayout(Standard::kPal, &offsets, &counts);
  const auto frame_lines = BuildFrameTimingPrimitives(Standard::kPal);
  const TimingConstants timing = GetTimingConstants(Standard::kPal);
  std::vector<std::string> errors;

  manager.SetProjectDiscType(DiscType::kCAV);
  ASSERT_TRUE(manager.ProcessFrame(&y_mv, 0, offsets, counts, section,
                                   Standard::kPal, timing.sample_rate_4fsc_hz,
                                   frame_lines, 177,
                                   ActiveWindowEnd(Standard::kPal), &errors));
  EXPECT_TRUE(errors.empty());

  // users_code goes on line 16 (or 329) in lead-in for PAL CAV.
  const int end = timing.samples_per_line_4fsc;
  EXPECT_TRUE(LineHasSignal(y_mv, Standard::kPal, 16, 177, end));
}

// ---------------------------------------------------------------------------
// Chapter number code in programme area.
// ---------------------------------------------------------------------------

TEST(BiphaseInjectionManagerPalCavTest, ChapterNumberInProgrammeArea) {
  std::vector<Section::LineInjectionCode> codes;
  Section::LineInjectionCode pn;
  pn.code_type = "picture_number";
  pn.start_value = 1;
  pn.start_value_specified = true;
  codes.push_back(pn);
  Section::LineInjectionCode cn;
  cn.code_type = "chapter_number";
  cn.chapter = 1;
  cn.chapter_specified = true;
  codes.push_back(cn);

  const auto section =
      MakeLaserdiscSection(SectionType::kProgrammeArea, DiscType::kCAV, codes);

  BiphaseInjectionManager manager;
  auto y_mv = MakeBlankingBuffer(Standard::kPal);
  std::vector<int> offsets, counts;
  BuildLineLayout(Standard::kPal, &offsets, &counts);
  const auto frame_lines = BuildFrameTimingPrimitives(Standard::kPal);
  const TimingConstants timing = GetTimingConstants(Standard::kPal);
  std::vector<std::string> errors;

  manager.SetProjectDiscType(DiscType::kCAV);
  ASSERT_TRUE(manager.ProcessFrame(&y_mv, 0, offsets, counts, section,
                                   Standard::kPal, timing.sample_rate_4fsc_hz,
                                   frame_lines, 177,
                                   ActiveWindowEnd(Standard::kPal), &errors));
  EXPECT_TRUE(errors.empty());

  // chapter_number on lines where picture_number is NOT present (the engine
  // may skip if all lines are consumed by picture_number). With both codes the
  // engine places chapter on lines that picture_number doesn't occupy.
  // We just confirm no errors occurred and the function returned success.
}

// ---------------------------------------------------------------------------
// PAL non-VBI lines remain at blanking after biphase injection.
// ---------------------------------------------------------------------------

TEST(BiphaseInjectionManagerPalCavTest, ActivePictureLinesUnaffected) {
  Section::LineInjectionCode code;
  code.code_type = "lead_in";
  const auto section =
      MakeLaserdiscSection(SectionType::kLeadIn, DiscType::kCAV, {code});

  BiphaseInjectionManager manager;
  auto y_mv = MakeBlankingBuffer(Standard::kPal);
  std::vector<int> offsets, counts;
  BuildLineLayout(Standard::kPal, &offsets, &counts);
  const auto frame_lines = BuildFrameTimingPrimitives(Standard::kPal);
  const TimingConstants timing = GetTimingConstants(Standard::kPal);
  std::vector<std::string> errors;

  manager.SetProjectDiscType(DiscType::kCAV);
  ASSERT_TRUE(manager.ProcessFrame(&y_mv, 0, offsets, counts, section,
                                   Standard::kPal, timing.sample_rate_4fsc_hz,
                                   frame_lines, 177,
                                   ActiveWindowEnd(Standard::kPal), &errors));

  const int aws = 177;
  const int end = timing.samples_per_line_4fsc;
  // Active picture line 100 is far outside VBI range; must remain at blanking.
  EXPECT_FALSE(LineHasSignal(y_mv, Standard::kPal, 100, aws, end));
}

// ---------------------------------------------------------------------------
// PAL CLV: programme_time_code and clv_picture_number are continuous across
// section (chapter) boundaries — they must NOT reset when a new section starts.
// ---------------------------------------------------------------------------

TEST(BiphaseInjectionManagerPalClvTest, TimeCodeIsContinuousAcrossSections) {
  // Section A: 25 PAL frames (exactly 1 second of programme_time_code).
  std::vector<Section::LineInjectionCode> codes;
  Section::LineInjectionCode ptc;
  ptc.code_type = "programme_time_code";
  codes.push_back(ptc);
  Section::LineInjectionCode cpn;
  cpn.code_type = "clv_picture_number";
  codes.push_back(cpn);

  Section section_a =
      MakeLaserdiscSection(SectionType::kProgrammeArea, DiscType::kCLV, codes);
  section_a.duration_frames = 25;

  Section section_b =
      MakeLaserdiscSection(SectionType::kProgrammeArea, DiscType::kCLV, codes);
  section_b.name = "SectionB";
  section_b.duration_frames = 1;

  BiphaseInjectionManager manager;
  std::vector<int> offsets, counts;
  BuildLineLayout(Standard::kPal, &offsets, &counts);
  const auto frame_lines = BuildFrameTimingPrimitives(Standard::kPal);
  const TimingConstants timing = GetTimingConstants(Standard::kPal);
  const int aws = 177;
  const int awe = ActiveWindowEnd(Standard::kPal);
  std::vector<std::string> errors;

  // Process 25 frames of section_a (1 second).
  for (int f = 0; f < 25; ++f) {
    auto y = MakeBlankingBuffer(Standard::kPal);
    manager.SetProjectDiscType(DiscType::kCLV);
    ASSERT_TRUE(manager.ProcessFrame(&y, 0, offsets, counts, section_a,
                                     Standard::kPal, timing.sample_rate_4fsc_hz,
                                     frame_lines, aws, awe, &errors))
        << "section_a frame " << f;
  }

  // Capture the first frame of section_b.
  auto y_b = MakeBlankingBuffer(Standard::kPal);
  manager.SetProjectDiscType(DiscType::kCLV);
  ASSERT_TRUE(manager.ProcessFrame(&y_b, 0, offsets, counts, section_b,
                                   Standard::kPal, timing.sample_rate_4fsc_hz,
                                   frame_lines, aws, awe, &errors));
  EXPECT_TRUE(errors.empty());

  // Also capture a fresh single-frame session starting from zero for
  // comparison.
  BiphaseInjectionManager fresh_manager;
  fresh_manager.SetProjectDiscType(DiscType::kCLV);
  auto y_fresh = MakeBlankingBuffer(Standard::kPal);
  ASSERT_TRUE(fresh_manager.ProcessFrame(
      &y_fresh, 0, offsets, counts, section_b, Standard::kPal,
      timing.sample_rate_4fsc_hz, frame_lines, aws, awe, &errors));

  // The clv_picture_number on line 16 must differ: the continuous manager has
  // advanced 25 frames (1 PAL second) so the encoded second has changed from
  // 0 to 1, while the fresh manager is still at second 0, frame 0.
  // (programme_time_code on line 17 would also differ but only after 1500
  // frames — one full minute — which is too slow for a unit test.)
  const int spl = timing.samples_per_line_4fsc;
  const int line16_base = (16 - 1) * spl;
  bool differs = false;
  for (int i = aws; i < spl && !differs; ++i) {
    if (y_b[static_cast<std::size_t>(line16_base) +
            static_cast<std::size_t>(i)] !=
        y_fresh[static_cast<std::size_t>(line16_base) +
                static_cast<std::size_t>(i)]) {
      differs = true;
    }
  }
  EXPECT_TRUE(differs) << "clv_picture_number must differ after section "
                          "transition: picture number should be continuous";
}

// ---------------------------------------------------------------------------
// Multiple frames with same section: generators advance correctly.
// ---------------------------------------------------------------------------

TEST(BiphaseInjectionManagerPalCavTest, MultipleFramesSameSection) {
  Section::LineInjectionCode code;
  code.code_type = "picture_number";
  code.start_value = 1;
  code.start_value_specified = true;
  const auto section =
      MakeLaserdiscSection(SectionType::kProgrammeArea, DiscType::kCAV, {code});

  BiphaseInjectionManager manager;
  std::vector<int> offsets, counts;
  BuildLineLayout(Standard::kPal, &offsets, &counts);
  const auto frame_lines = BuildFrameTimingPrimitives(Standard::kPal);
  const TimingConstants timing = GetTimingConstants(Standard::kPal);
  const int aws = 177;
  const int awe = ActiveWindowEnd(Standard::kPal);
  std::vector<std::string> errors;

  // Process 5 frames; all must succeed.
  for (int f = 0; f < 5; ++f) {
    auto y_mv = MakeBlankingBuffer(Standard::kPal);
    manager.SetProjectDiscType(DiscType::kCAV);
    EXPECT_TRUE(manager.ProcessFrame(&y_mv, 0, offsets, counts, section,
                                     Standard::kPal, timing.sample_rate_4fsc_hz,
                                     frame_lines, aws, awe, &errors))
        << "Frame " << f << " failed";
  }
  EXPECT_TRUE(errors.empty());
}

// ---------------------------------------------------------------------------
// PerFrameContext — GetLastFrameContext() captures VBI state per frame.
// ---------------------------------------------------------------------------

TEST_F(BiphaseInjectionManagerTest, DefaultContextBeforeFirstFrame) {
  const PerFrameContext& ctx = manager_.GetLastFrameContext();
  EXPECT_EQ(ctx.picture_number, 0);
  EXPECT_TRUE(ctx.biphase_words.empty());
  EXPECT_EQ(ctx.colour_frame_index, 0);
}

TEST_F(BiphaseInjectionManagerTest, PictureNumberCapturedAfterProcessFrame) {
  Section::LineInjectionCode pn_code;
  pn_code.code_type = "picture_number";
  pn_code.start_value = 17;
  pn_code.start_value_specified = true;

  Section::LineInjectionCode ps_code;
  ps_code.code_type = "programme_status";
  ps_code.programme_status = "0x8DC000";
  ps_code.programme_status_specified = true;

  auto section = MakeLaserdiscSection(SectionType::kProgrammeArea,
                                      DiscType::kCAV, {pn_code, ps_code});

  EXPECT_TRUE(RunProcessFrame(Standard::kPal, section));
  EXPECT_EQ(manager_.GetLastFrameContext().picture_number, 17);
}

TEST_F(BiphaseInjectionManagerTest, ColourFrameIndexAdvancesEachFrame) {
  Section section;
  section.name = "Plain";
  section.type = "progressive";
  section.section_type = SectionType::kProgrammeArea;
  section.duration_frames = 1;

  // PAL: colour period is 4.
  for (int i = 0; i < 4; ++i) {
    EXPECT_TRUE(RunProcessFrame(Standard::kPal, section));
    EXPECT_EQ(manager_.GetLastFrameContext().colour_frame_index, i);
  }
  // Wraps back to 0 on the fifth frame.
  EXPECT_TRUE(RunProcessFrame(Standard::kPal, section));
  EXPECT_EQ(manager_.GetLastFrameContext().colour_frame_index, 0);
}

TEST_F(BiphaseInjectionManagerTest, ResetClearsLastFrameContext) {
  Section::LineInjectionCode pn_code;
  pn_code.code_type = "picture_number";
  pn_code.start_value = 5;
  pn_code.start_value_specified = true;
  Section::LineInjectionCode ps_code;
  ps_code.code_type = "programme_status";
  ps_code.programme_status = "0x8DC000";
  ps_code.programme_status_specified = true;
  auto section = MakeLaserdiscSection(SectionType::kProgrammeArea,
                                      DiscType::kCAV, {pn_code, ps_code});

  EXPECT_TRUE(RunProcessFrame(Standard::kPal, section));
  EXPECT_GT(manager_.GetLastFrameContext().picture_number, 0);

  manager_.Reset();
  EXPECT_EQ(manager_.GetLastFrameContext().picture_number, 0);
  EXPECT_TRUE(manager_.GetLastFrameContext().biphase_words.empty());
  EXPECT_EQ(manager_.GetLastFrameContext().colour_frame_index, 0);
}

TEST_F(BiphaseInjectionManagerTest, BiphaseWordsPopulatedForLaserdiscSection) {
  Section::LineInjectionCode pn_code;
  pn_code.code_type = "picture_number";
  pn_code.start_value = 1;
  pn_code.start_value_specified = true;
  Section::LineInjectionCode ps_code;
  ps_code.code_type = "programme_status";
  ps_code.programme_status = "0x8DC000";
  ps_code.programme_status_specified = true;
  auto section = MakeLaserdiscSection(SectionType::kProgrammeArea,
                                      DiscType::kCAV, {pn_code, ps_code});

  EXPECT_TRUE(RunProcessFrame(Standard::kPal, section));
  // Two generators active (picture_number + programme_status), so two words.
  EXPECT_EQ(manager_.GetLastFrameContext().biphase_words.size(), 2U);
}

// ---------------------------------------------------------------------------
// CLV sections: colour_frame_index is driven by the monotonic frame_count_
// counter (absolute disc position from Reset()), NOT by the CLV timecode
// generators (which count from programme start and do not include lead-in).
//
// This is verified by interleaving a non-multiple-of-period lead-in with a
// CLV programme section: if colour_frame_index were derived from
// clv_picture_number.total_frames_ it would snap to 0 at programme start;
// the correct behaviour is to continue the monotonic sequence.
// ---------------------------------------------------------------------------

TEST_F(BiphaseInjectionManagerTest,
       ClvColourFrameIndexContinuesFromFrameCountAcrossSections) {
  // Lead-in section: 3 frames with no laserdisc injection (plain section).
  // PAL colour period = 4; 3 is not a multiple of 4.
  Section lead_in;
  lead_in.name = "LeadIn";
  lead_in.type = "progressive";
  lead_in.section_type = SectionType::kLeadIn;
  lead_in.duration_frames = 3;

  // CLV programme section: has clv_picture_number whose internal total_frames_
  // counter resets to 0 at creation. If colour phase mistakenly used that
  // counter, it would give 0 at the start of this section instead of 3.
  Section::LineInjectionCode ptc;
  ptc.code_type = "programme_time_code";
  Section::LineInjectionCode cpn;
  cpn.code_type = "clv_picture_number";
  auto clv_section = MakeLaserdiscSection(SectionType::kProgrammeArea,
                                          DiscType::kCLV, {ptc, cpn});

  // Three lead-in frames: colour phase advances 0, 1, 2 via frame_count_.
  EXPECT_TRUE(RunProcessFrame(Standard::kPal, lead_in));
  EXPECT_EQ(manager_.GetLastFrameContext().colour_frame_index, 0);
  EXPECT_TRUE(RunProcessFrame(Standard::kPal, lead_in));
  EXPECT_EQ(manager_.GetLastFrameContext().colour_frame_index, 1);
  EXPECT_TRUE(RunProcessFrame(Standard::kPal, lead_in));
  EXPECT_EQ(manager_.GetLastFrameContext().colour_frame_index, 2);

  // First CLV programme frame: frame_count_ = 3 → correct index is 3.
  // If clv_picture_number.total_frames_ (= 0 at creation) were used instead,
  // the index would incorrectly snap back to 0.
  EXPECT_TRUE(RunProcessFrame(Standard::kPal, clv_section));
  EXPECT_EQ(manager_.GetLastFrameContext().colour_frame_index, 3);

  // Subsequent CLV frames continue wrapping normally.
  EXPECT_TRUE(RunProcessFrame(Standard::kPal, clv_section));
  EXPECT_EQ(manager_.GetLastFrameContext().colour_frame_index, 0);  // 4%4
  EXPECT_TRUE(RunProcessFrame(Standard::kPal, clv_section));
  EXPECT_EQ(manager_.GetLastFrameContext().colour_frame_index, 1);
  EXPECT_TRUE(RunProcessFrame(Standard::kPal, clv_section));
  EXPECT_EQ(manager_.GetLastFrameContext().colour_frame_index, 2);
}

// CLV colour phase is continuous across CLV section (chapter) boundaries.
// The clv_picture_number generator persists across sections; frame_count_
// also persists, so the phase never resets on a chapter transition.
TEST_F(BiphaseInjectionManagerTest,
       ClvColourFrameIndexContinuesAcrossChapterBoundary) {
  Section::LineInjectionCode ptc;
  ptc.code_type = "programme_time_code";
  Section::LineInjectionCode cpn;
  cpn.code_type = "clv_picture_number";

  auto chapter_a = MakeLaserdiscSection(SectionType::kProgrammeArea,
                                        DiscType::kCLV, {ptc, cpn});
  auto chapter_b = MakeLaserdiscSection(SectionType::kProgrammeArea,
                                        DiscType::kCLV, {ptc, cpn});
  chapter_b.name = "ChapterB";

  // Five frames of chapter_a: colour indices 0, 1, 2, 3, 0.
  for (int i = 0; i < 4; ++i) {
    EXPECT_TRUE(RunProcessFrame(Standard::kPal, chapter_a));
    EXPECT_EQ(manager_.GetLastFrameContext().colour_frame_index, i);
  }
  EXPECT_TRUE(RunProcessFrame(Standard::kPal, chapter_a));
  EXPECT_EQ(manager_.GetLastFrameContext().colour_frame_index, 0);

  // Chapter boundary: phase must continue from 1, not reset.
  EXPECT_TRUE(RunProcessFrame(Standard::kPal, chapter_b));
  EXPECT_EQ(manager_.GetLastFrameContext().colour_frame_index, 1);
  EXPECT_TRUE(RunProcessFrame(Standard::kPal, chapter_b));
  EXPECT_EQ(manager_.GetLastFrameContext().colour_frame_index, 2);
}

// ---------------------------------------------------------------------------
// colour_frame_index derived from disc picture number on section transitions.
// Verifies that backward-skip and forward-skip sections produce PN-accurate
// phase rather than the file-position-based monotonic counter.
// ---------------------------------------------------------------------------

// Simulates a backward-skip: section A encodes PN 1–3, then section B
// restarts at PN 25 (replay). PAL colour period = 4.
// Without the fix, frame_count_ after 3 frames of A would be 3 → cfi = 3;
// with the fix, PN 25 → (25-1)%4 = 0.
TEST_F(BiphaseInjectionManagerTest, ColourFrameIndexCorrectedOnBackwardSkip) {
  Section::LineInjectionCode pn_a;
  pn_a.code_type = "picture_number";
  pn_a.start_value = 1;
  pn_a.start_value_specified = true;
  auto section_a =
      MakeLaserdiscSection(SectionType::kProgrammeArea, DiscType::kCAV, {pn_a});

  // Three frames of section A: PN 1 → cfi 0, PN 2 → cfi 1, PN 3 → cfi 2.
  EXPECT_TRUE(RunProcessFrame(Standard::kPal, section_a));
  EXPECT_EQ(manager_.GetLastFrameContext().colour_frame_index, 0);
  EXPECT_TRUE(RunProcessFrame(Standard::kPal, section_a));
  EXPECT_EQ(manager_.GetLastFrameContext().colour_frame_index, 1);
  EXPECT_TRUE(RunProcessFrame(Standard::kPal, section_a));
  EXPECT_EQ(manager_.GetLastFrameContext().colour_frame_index, 2);

  // Section B: backward skip, replay starts at PN 25.
  // (25-1) % 4 = 0; without the fix the monotonic counter gives 3.
  Section::LineInjectionCode pn_b;
  pn_b.code_type = "picture_number";
  pn_b.start_value = 25;
  pn_b.start_value_specified = true;
  auto section_b =
      MakeLaserdiscSection(SectionType::kProgrammeArea, DiscType::kCAV, {pn_b});

  EXPECT_TRUE(RunProcessFrame(Standard::kPal, section_b));
  EXPECT_EQ(manager_.GetLastFrameContext().colour_frame_index, 0);  // (25-1)%4
  EXPECT_EQ(manager_.GetLastFrameContext().picture_number, 25);

  // PN advances normally within section B.
  EXPECT_TRUE(RunProcessFrame(Standard::kPal, section_b));
  EXPECT_EQ(manager_.GetLastFrameContext().colour_frame_index, 1);  // (26-1)%4
  EXPECT_TRUE(RunProcessFrame(Standard::kPal, section_b));
  EXPECT_EQ(manager_.GetLastFrameContext().colour_frame_index, 2);  // (27-1)%4
  EXPECT_TRUE(RunProcessFrame(Standard::kPal, section_b));
  EXPECT_EQ(manager_.GetLastFrameContext().colour_frame_index, 3);  // (28-1)%4
}

// Simulates a forward-skip: section A encodes PN 17–20, then section B
// resumes at PN 23 (gap of 2). PAL colour period = 4.
// Without the fix, frame_count_ after 4 frames of A = 4 → cfi = 0;
// with the fix, PN 23 → (23-1)%4 = 2.
TEST_F(BiphaseInjectionManagerTest, ColourFrameIndexCorrectedOnForwardSkip) {
  Section::LineInjectionCode pn_a;
  pn_a.code_type = "picture_number";
  pn_a.start_value = 17;
  pn_a.start_value_specified = true;
  auto section_a =
      MakeLaserdiscSection(SectionType::kProgrammeArea, DiscType::kCAV, {pn_a});

  // Four frames of section A: PN 17→cfi 0, 18→1, 19→2, 20→3.
  EXPECT_TRUE(RunProcessFrame(Standard::kPal, section_a));
  EXPECT_EQ(manager_.GetLastFrameContext().colour_frame_index, 0);  // (17-1)%4
  EXPECT_TRUE(RunProcessFrame(Standard::kPal, section_a));
  EXPECT_EQ(manager_.GetLastFrameContext().colour_frame_index, 1);  // (18-1)%4
  EXPECT_TRUE(RunProcessFrame(Standard::kPal, section_a));
  EXPECT_EQ(manager_.GetLastFrameContext().colour_frame_index, 2);  // (19-1)%4
  EXPECT_TRUE(RunProcessFrame(Standard::kPal, section_a));
  EXPECT_EQ(manager_.GetLastFrameContext().colour_frame_index, 3);  // (20-1)%4

  // Section B: forward skip, resumes at PN 23 (PNs 21–22 absent).
  // (23-1) % 4 = 2; without the fix the monotonic counter gives 0.
  Section::LineInjectionCode pn_b;
  pn_b.code_type = "picture_number";
  pn_b.start_value = 23;
  pn_b.start_value_specified = true;
  auto section_b =
      MakeLaserdiscSection(SectionType::kProgrammeArea, DiscType::kCAV, {pn_b});

  EXPECT_TRUE(RunProcessFrame(Standard::kPal, section_b));
  EXPECT_EQ(manager_.GetLastFrameContext().colour_frame_index, 2);  // (23-1)%4
  EXPECT_EQ(manager_.GetLastFrameContext().picture_number, 23);

  EXPECT_TRUE(RunProcessFrame(Standard::kPal, section_b));
  EXPECT_EQ(manager_.GetLastFrameContext().colour_frame_index, 3);  // (24-1)%4
}

}  // namespace
}  // namespace videosynth
