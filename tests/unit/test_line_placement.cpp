/*
 * File:        test_line_placement.cpp
 * Module:      line_placement_tests
 * Purpose:     Unit tests for LinePlacementEngine — field-aware VBI line
 *              placement for LaserDisc biphase and 40-bit FM injection.
 *
 *              Verifies:
 *              - PAL/NTSC reserved-range detection
 *              - Field-1 / field-2 detection
 *              - Lead-in / lead-out line assignments for PAL and NTSC
 *              - PAL CAV programme_area priority rules
 *              - PAL CLV programme_area priority rules
 *              - NTSC CAV programme_area (biphase + FM) assignments
 *              - NTSC CLV programme_area (biphase + FM) assignments
 *              - 0.172H offset flags for programme_status and NTSC clv_code
 *              - White flag section-aware placement (NTSC)
 *              - Lines outside reserved range return unassigned
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "videosynth/biphase_types.h"
#include "videosynth/line_placement_engine.h"
#include "videosynth/model.h"

namespace videosynth {
namespace {

// ---------------------------------------------------------------------------
// Static helper tests: IsInBiphaseReservedRange
// ---------------------------------------------------------------------------

TEST(LinePlacementTest, PalReservedRangeField1) {
  // Lines 6–18: reserved (IEC 60856).
  for (int line = 6; line <= 18; ++line) {
    EXPECT_TRUE(
        LinePlacementEngine::IsInBiphaseReservedRange(Standard::kPal, line))
        << "line=" << line;
  }
}

TEST(LinePlacementTest, PalReservedRangeField2) {
  // Lines 319–331: reserved (IEC 60856).
  for (int line = 319; line <= 331; ++line) {
    EXPECT_TRUE(
        LinePlacementEngine::IsInBiphaseReservedRange(Standard::kPal, line))
        << "line=" << line;
  }
}

TEST(LinePlacementTest, PalOutsideReservedRange) {
  EXPECT_FALSE(
      LinePlacementEngine::IsInBiphaseReservedRange(Standard::kPal, 1));
  EXPECT_FALSE(
      LinePlacementEngine::IsInBiphaseReservedRange(Standard::kPal, 5));
  EXPECT_FALSE(
      LinePlacementEngine::IsInBiphaseReservedRange(Standard::kPal, 19));
  EXPECT_FALSE(
      LinePlacementEngine::IsInBiphaseReservedRange(Standard::kPal, 318));
  EXPECT_FALSE(
      LinePlacementEngine::IsInBiphaseReservedRange(Standard::kPal, 332));
  EXPECT_FALSE(
      LinePlacementEngine::IsInBiphaseReservedRange(Standard::kPal, 625));
}

TEST(LinePlacementTest, NtscReservedRangeField1) {
  // Lines 10–18: reserved (IEC 60857).
  for (int line = 10; line <= 18; ++line) {
    EXPECT_TRUE(
        LinePlacementEngine::IsInBiphaseReservedRange(Standard::kNtsc, line))
        << "line=" << line;
  }
}

TEST(LinePlacementTest, NtscReservedRangeField2) {
  // Lines 273–281: reserved (IEC 60857).
  for (int line = 273; line <= 281; ++line) {
    EXPECT_TRUE(
        LinePlacementEngine::IsInBiphaseReservedRange(Standard::kNtsc, line))
        << "line=" << line;
  }
}

TEST(LinePlacementTest, NtscOutsideReservedRange) {
  EXPECT_FALSE(
      LinePlacementEngine::IsInBiphaseReservedRange(Standard::kNtsc, 1));
  EXPECT_FALSE(
      LinePlacementEngine::IsInBiphaseReservedRange(Standard::kNtsc, 9));
  EXPECT_FALSE(
      LinePlacementEngine::IsInBiphaseReservedRange(Standard::kNtsc, 19));
  EXPECT_FALSE(
      LinePlacementEngine::IsInBiphaseReservedRange(Standard::kNtsc, 272));
  EXPECT_FALSE(
      LinePlacementEngine::IsInBiphaseReservedRange(Standard::kNtsc, 282));
  EXPECT_FALSE(
      LinePlacementEngine::IsInBiphaseReservedRange(Standard::kNtsc, 525));
}

// ---------------------------------------------------------------------------
// Static helper tests: IsFieldOne
// ---------------------------------------------------------------------------

TEST(LinePlacementTest, PalField1DetectionBoundary) {
  EXPECT_TRUE(LinePlacementEngine::IsFieldOne(Standard::kPal, 1));
  EXPECT_TRUE(LinePlacementEngine::IsFieldOne(Standard::kPal, 17));
  EXPECT_TRUE(LinePlacementEngine::IsFieldOne(Standard::kPal, 312));
  EXPECT_FALSE(LinePlacementEngine::IsFieldOne(Standard::kPal, 313));
  EXPECT_FALSE(LinePlacementEngine::IsFieldOne(Standard::kPal, 330));
  EXPECT_FALSE(LinePlacementEngine::IsFieldOne(Standard::kPal, 625));
}

TEST(LinePlacementTest, NtscField1DetectionBoundary) {
  EXPECT_TRUE(LinePlacementEngine::IsFieldOne(Standard::kNtsc, 1));
  EXPECT_TRUE(LinePlacementEngine::IsFieldOne(Standard::kNtsc, 17));
  EXPECT_TRUE(LinePlacementEngine::IsFieldOne(Standard::kNtsc, 262));
  EXPECT_FALSE(LinePlacementEngine::IsFieldOne(Standard::kNtsc, 263));
  EXPECT_FALSE(LinePlacementEngine::IsFieldOne(Standard::kNtsc, 280));
  EXPECT_FALSE(LinePlacementEngine::IsFieldOne(Standard::kNtsc, 525));
}

// ---------------------------------------------------------------------------
// Lines outside reserved range always return unassigned
// ---------------------------------------------------------------------------

TEST(LinePlacementTest, LinesOutsideReservedRangeReturnNone) {
  LinePlacementEngine engine(Standard::kPal, DiscType::kCAV,
                             SectionType::kLeadIn, {"lead_in"});
  EXPECT_FALSE(engine.GetAssignment(1, true).assigned);
  EXPECT_FALSE(engine.GetAssignment(5, true).assigned);
  EXPECT_FALSE(engine.GetAssignment(19, true).assigned);
  EXPECT_FALSE(engine.GetAssignment(318, false).assigned);
  EXPECT_FALSE(engine.GetAssignment(332, false).assigned);
  EXPECT_FALSE(engine.GetAssignment(625, false).assigned);
}

// Lines 6–15 and 319–328 are reserved but carry no active biphase code.
TEST(LinePlacementTest, PalReservedNonActiveLines) {
  LinePlacementEngine engine(Standard::kPal, DiscType::kCAV,
                             SectionType::kLeadIn, {"lead_in", "users_code"});
  for (int line : {6, 7, 8, 9, 10, 11, 12, 13, 14, 15}) {
    EXPECT_FALSE(engine.GetAssignment(line, true).assigned)
        << "Expected no code on reserved PAL line " << line;
  }
  for (int line : {319, 320, 321, 322, 323, 324, 325, 326, 327, 328}) {
    EXPECT_FALSE(engine.GetAssignment(line, false).assigned)
        << "Expected no code on reserved PAL line " << line;
  }
}

// ---------------------------------------------------------------------------
// PAL lead_in — CAV
// ---------------------------------------------------------------------------

TEST(LinePlacementTest, PalCavLeadInLine17And18GetLeadInCode) {
  LinePlacementEngine engine(Standard::kPal, DiscType::kCAV,
                             SectionType::kLeadIn, {"lead_in"});

  auto a17 = engine.GetAssignment(17, true);
  EXPECT_TRUE(a17.assigned);
  EXPECT_EQ(a17.code_type, "lead_in");
  EXPECT_FALSE(a17.is_fm);
  EXPECT_FALSE(a17.is_white_flag);

  auto a18 = engine.GetAssignment(18, true);
  EXPECT_TRUE(a18.assigned);
  EXPECT_EQ(a18.code_type, "lead_in");
}

TEST(LinePlacementTest, PalCavLeadInField2Lines330And331GetLeadInCode) {
  LinePlacementEngine engine(Standard::kPal, DiscType::kCAV,
                             SectionType::kLeadIn, {"lead_in"});
  for (int line : {330, 331}) {
    auto a = engine.GetAssignment(line, false);
    EXPECT_TRUE(a.assigned) << "line=" << line;
    EXPECT_EQ(a.code_type, "lead_in") << "line=" << line;
  }
}

TEST(LinePlacementTest, PalCavLeadInLine16GetsUsersCodeWhenPresent) {
  LinePlacementEngine engine(Standard::kPal, DiscType::kCAV,
                             SectionType::kLeadIn, {"lead_in", "users_code"});
  auto a = engine.GetAssignment(16, true);
  EXPECT_TRUE(a.assigned);
  EXPECT_EQ(a.code_type, "users_code");
  EXPECT_FALSE(a.uses_172h_offset);
}

TEST(LinePlacementTest, PalCavLeadInLine16EmptyWhenNoUsersCode) {
  LinePlacementEngine engine(Standard::kPal, DiscType::kCAV,
                             SectionType::kLeadIn, {"lead_in"});
  EXPECT_FALSE(engine.GetAssignment(16, true).assigned);
}

TEST(LinePlacementTest, PalCavLeadInField2Line329GetsUsersCode) {
  LinePlacementEngine engine(Standard::kPal, DiscType::kCAV,
                             SectionType::kLeadIn, {"lead_in", "users_code"});
  auto a = engine.GetAssignment(329, false);
  EXPECT_TRUE(a.assigned);
  EXPECT_EQ(a.code_type, "users_code");
}

// ---------------------------------------------------------------------------
// PAL lead_out — CLV
// ---------------------------------------------------------------------------

TEST(LinePlacementTest, PalClvLeadOutLine17And18GetLeadOutCode) {
  LinePlacementEngine engine(Standard::kPal, DiscType::kCLV,
                             SectionType::kLeadOut, {"lead_out"});
  for (int line : {17, 18}) {
    auto a = engine.GetAssignment(line, true);
    EXPECT_TRUE(a.assigned) << "line=" << line;
    EXPECT_EQ(a.code_type, "lead_out") << "line=" << line;
  }
}

TEST(LinePlacementTest, PalClvLeadOutField2Lines330And331GetLeadOutCode) {
  LinePlacementEngine engine(Standard::kPal, DiscType::kCLV,
                             SectionType::kLeadOut, {"lead_out"});
  for (int line : {330, 331}) {
    auto a = engine.GetAssignment(line, false);
    EXPECT_TRUE(a.assigned) << "line=" << line;
    EXPECT_EQ(a.code_type, "lead_out") << "line=" << line;
  }
}

// ---------------------------------------------------------------------------
// PAL CAV programme_area — picture_number placement
// ---------------------------------------------------------------------------

TEST(LinePlacementTest, PalCavProgrammePictureNumberOnLines17And18) {
  LinePlacementEngine engine(Standard::kPal, DiscType::kCAV,
                             SectionType::kProgrammeArea, {"picture_number"});
  for (int line : {17, 18}) {
    auto a = engine.GetAssignment(line, true);
    EXPECT_TRUE(a.assigned) << "line=" << line;
    EXPECT_EQ(a.code_type, "picture_number") << "line=" << line;
  }
}

TEST(LinePlacementTest, PalCavProgrammePictureNumberFirstFieldOnly) {
  // IEC 60856 §10.1.4: picture numbers go in the first field of the picture.
  LinePlacementEngine engine(Standard::kPal, DiscType::kCAV,
                             SectionType::kProgrammeArea, {"picture_number"});
  for (int line : {330, 331}) {
    EXPECT_FALSE(engine.GetAssignment(line, false).assigned) << "line=" << line;
  }
}

TEST(LinePlacementTest, PalCavProgrammePictureStopInSecondField) {
  // IEC 60856 §10.1.5: picture stop goes in lines 329/330 of the field after
  // the picture number; programme status stays on line 16.
  LinePlacementEngine engine(
      Standard::kPal, DiscType::kCAV, SectionType::kProgrammeArea,
      {"picture_number", "picture_stop", "programme_status"});
  EXPECT_EQ(engine.GetAssignment(16, true).code_type, "programme_status");
  EXPECT_EQ(engine.GetAssignment(17, true).code_type, "picture_number");
  EXPECT_EQ(engine.GetAssignment(18, true).code_type, "picture_number");
  EXPECT_EQ(engine.GetAssignment(329, false).code_type, "picture_stop");
  EXPECT_EQ(engine.GetAssignment(330, false).code_type, "picture_stop");
  EXPECT_FALSE(engine.GetAssignment(331, false).assigned);
}

// ---------------------------------------------------------------------------
// PAL CAV programme_area — programme_status placement
// ---------------------------------------------------------------------------

TEST(LinePlacementTest, PalCavProgrammeProgrammeStatusOnLine16With172hOffset) {
  LinePlacementEngine engine(Standard::kPal, DiscType::kCAV,
                             SectionType::kProgrammeArea, {"programme_status"});
  auto a = engine.GetAssignment(16, true);
  EXPECT_TRUE(a.assigned);
  EXPECT_EQ(a.code_type, "programme_status");
  EXPECT_TRUE(a.uses_172h_offset);
}

TEST(LinePlacementTest, PalCavProgrammeProgrammeStatusOnField2Line329With172h) {
  LinePlacementEngine engine(Standard::kPal, DiscType::kCAV,
                             SectionType::kProgrammeArea, {"programme_status"});
  auto a = engine.GetAssignment(329, false);
  EXPECT_TRUE(a.assigned);
  EXPECT_EQ(a.code_type, "programme_status");
  EXPECT_TRUE(a.uses_172h_offset);
}

// ---------------------------------------------------------------------------
// PAL CAV programme_area — picture_stop priority
// ---------------------------------------------------------------------------

TEST(LinePlacementTest,
     PalCavProgrammePictureStopOnLine16BeforeProgrammeStatus) {
  LinePlacementEngine engine(Standard::kPal, DiscType::kCAV,
                             SectionType::kProgrammeArea,
                             {"picture_stop", "programme_status"});
  auto a = engine.GetAssignment(16, true);
  EXPECT_TRUE(a.assigned);
  EXPECT_EQ(a.code_type, "picture_stop");
}

TEST(LinePlacementTest, PalCavProgrammePictureStopOnLine17WhenNoPictureNumber) {
  LinePlacementEngine engine(Standard::kPal, DiscType::kCAV,
                             SectionType::kProgrammeArea, {"picture_stop"});
  auto a = engine.GetAssignment(17, true);
  EXPECT_TRUE(a.assigned);
  EXPECT_EQ(a.code_type, "picture_stop");
}

TEST(LinePlacementTest, PalCavProgrammePictureNumberBeforePictureStopOnLine17) {
  LinePlacementEngine engine(Standard::kPal, DiscType::kCAV,
                             SectionType::kProgrammeArea,
                             {"picture_number", "picture_stop"});
  auto a = engine.GetAssignment(17, true);
  EXPECT_TRUE(a.assigned);
  EXPECT_EQ(a.code_type, "picture_number");
}

// ---------------------------------------------------------------------------
// PAL CAV programme_area — chapter_number priority
// ---------------------------------------------------------------------------

TEST(LinePlacementTest, PalCavProgrammeChapterOnLine17WhenNoPicOrStop) {
  LinePlacementEngine engine(Standard::kPal, DiscType::kCAV,
                             SectionType::kProgrammeArea, {"chapter_number"});
  auto a = engine.GetAssignment(17, true);
  EXPECT_TRUE(a.assigned);
  EXPECT_EQ(a.code_type, "chapter_number");
}

TEST(LinePlacementTest, PalCavProgrammeChapterOnLine18WhenNoPictureNumber) {
  LinePlacementEngine engine(Standard::kPal, DiscType::kCAV,
                             SectionType::kProgrammeArea, {"chapter_number"});
  auto a = engine.GetAssignment(18, true);
  EXPECT_TRUE(a.assigned);
  EXPECT_EQ(a.code_type, "chapter_number");
}

TEST(LinePlacementTest, PalCavProgrammeChapterInFieldWithoutPictureNumber) {
  // IEC 60856 §10.1.6: chapter numbers go in the fields without picture
  // numbers.
  LinePlacementEngine engine(Standard::kPal, DiscType::kCAV,
                             SectionType::kProgrammeArea,
                             {"picture_number", "chapter_number"});
  EXPECT_EQ(engine.GetAssignment(17, true).code_type, "picture_number");
  EXPECT_EQ(engine.GetAssignment(18, true).code_type, "picture_number");
  EXPECT_EQ(engine.GetAssignment(330, false).code_type, "chapter_number");
  EXPECT_EQ(engine.GetAssignment(331, false).code_type, "chapter_number");
}

TEST(LinePlacementTest,
     PalCavProgrammePictureStopBeforeChapterOnConflictLine17) {
  // IEC 60856: picture_stop has priority over chapter_number on line 17/330.
  LinePlacementEngine engine(Standard::kPal, DiscType::kCAV,
                             SectionType::kProgrammeArea,
                             {"picture_stop", "chapter_number"});
  auto a = engine.GetAssignment(17, true);
  EXPECT_EQ(a.code_type, "picture_stop");
}

TEST(LinePlacementTest,
     PalCavProgrammeChapterOnLine18WhenPictureStopPresentButNoPicNum) {
  // picture_stop occupies line 16 and 17, NOT line 18.
  LinePlacementEngine engine(Standard::kPal, DiscType::kCAV,
                             SectionType::kProgrammeArea,
                             {"picture_stop", "chapter_number"});
  auto a = engine.GetAssignment(18, true);
  EXPECT_TRUE(a.assigned);
  EXPECT_EQ(a.code_type, "chapter_number");
}

// ---------------------------------------------------------------------------
// PAL CLV programme_area
// ---------------------------------------------------------------------------

TEST(LinePlacementTest, PalClvProgrammeClvPictureNumberOnLine16) {
  LinePlacementEngine engine(Standard::kPal, DiscType::kCLV,
                             SectionType::kProgrammeArea,
                             {"clv_picture_number"});
  auto a = engine.GetAssignment(16, true);
  EXPECT_TRUE(a.assigned);
  EXPECT_EQ(a.code_type, "clv_picture_number");
  EXPECT_FALSE(a.uses_172h_offset);
}

TEST(LinePlacementTest, PalClvProgrammeClvPictureNumberFirstFieldOnly) {
  // IEC 60856 Amd 2: the CLV picture number goes in the first field.
  LinePlacementEngine engine(Standard::kPal, DiscType::kCLV,
                             SectionType::kProgrammeArea,
                             {"clv_picture_number"});
  EXPECT_FALSE(engine.GetAssignment(329, false).assigned);
}

TEST(LinePlacementTest, PalClvProgrammeClvPictureNumberBeforeProgrammeStatus) {
  LinePlacementEngine engine(Standard::kPal, DiscType::kCLV,
                             SectionType::kProgrammeArea,
                             {"clv_picture_number", "programme_status"});
  auto a = engine.GetAssignment(16, true);
  EXPECT_EQ(a.code_type, "clv_picture_number");
}

TEST(LinePlacementTest,
     PalClvProgrammeProgrammeStatusOnLine16With172hOffsetWhenNoPicNum) {
  LinePlacementEngine engine(Standard::kPal, DiscType::kCLV,
                             SectionType::kProgrammeArea, {"programme_status"});
  auto a = engine.GetAssignment(16, true);
  EXPECT_TRUE(a.assigned);
  EXPECT_EQ(a.code_type, "programme_status");
  EXPECT_TRUE(a.uses_172h_offset);
}

TEST(LinePlacementTest, PalClvProgrammeProgrammeTimeCodeOnLine17) {
  LinePlacementEngine engine(Standard::kPal, DiscType::kCLV,
                             SectionType::kProgrammeArea,
                             {"programme_time_code"});
  for (int line : {17, 18}) {
    auto a = engine.GetAssignment(line, true);
    EXPECT_TRUE(a.assigned) << "line=" << line;
    EXPECT_EQ(a.code_type, "programme_time_code") << "line=" << line;
  }
}

TEST(LinePlacementTest, PalClvProgrammeClvCodeOnLine17WhenNoProgrammeTimeCode) {
  LinePlacementEngine engine(Standard::kPal, DiscType::kCLV,
                             SectionType::kProgrammeArea, {"clv_code"});
  auto a = engine.GetAssignment(17, true);
  EXPECT_TRUE(a.assigned);
  EXPECT_EQ(a.code_type, "clv_code");
  // PAL clv_code does NOT have the 0.172H offset (NTSC only).
  EXPECT_FALSE(a.uses_172h_offset);
}

TEST(LinePlacementTest, PalClvProgrammeProgrammeTimeCodeBeforeClvCodeOnLine17) {
  LinePlacementEngine engine(Standard::kPal, DiscType::kCLV,
                             SectionType::kProgrammeArea,
                             {"programme_time_code", "clv_code"});
  auto a = engine.GetAssignment(17, true);
  EXPECT_EQ(a.code_type, "programme_time_code");
}

TEST(LinePlacementTest, PalClvProgrammeChapterOnLine18WhenNoPtc) {
  LinePlacementEngine engine(Standard::kPal, DiscType::kCLV,
                             SectionType::kProgrammeArea, {"chapter_number"});
  auto a = engine.GetAssignment(18, true);
  EXPECT_TRUE(a.assigned);
  EXPECT_EQ(a.code_type, "chapter_number");
}

TEST(LinePlacementTest, PalClvProgrammeChapterInFieldWithoutTimeCode) {
  // IEC 60856 §10.1.6: CLV chapter numbers go on line 18/331 of the fields
  // without programme time code.
  LinePlacementEngine engine(Standard::kPal, DiscType::kCLV,
                             SectionType::kProgrammeArea,
                             {"programme_time_code", "chapter_number"});
  EXPECT_EQ(engine.GetAssignment(18, true).code_type, "programme_time_code");
  EXPECT_EQ(engine.GetAssignment(331, false).code_type, "chapter_number");
}

TEST(LinePlacementTest, PalClvProgrammeSecondFieldCarriesNonPictureCodes) {
  LinePlacementEngine engine(
      Standard::kPal, DiscType::kCLV, SectionType::kProgrammeArea,
      {"programme_time_code", "clv_picture_number", "clv_code",
       "programme_status", "chapter_number"});
  EXPECT_EQ(engine.GetAssignment(16, true).code_type, "clv_picture_number");
  EXPECT_EQ(engine.GetAssignment(17, true).code_type, "programme_time_code");
  EXPECT_EQ(engine.GetAssignment(18, true).code_type, "programme_time_code");

  auto a329 = engine.GetAssignment(329, false);
  EXPECT_EQ(a329.code_type, "programme_status");
  EXPECT_TRUE(a329.uses_172h_offset);
  auto a330 = engine.GetAssignment(330, false);
  EXPECT_EQ(a330.code_type, "clv_code");
  EXPECT_FALSE(a330.uses_172h_offset);  // the 0.172H offset is NTSC only
  EXPECT_EQ(engine.GetAssignment(331, false).code_type, "chapter_number");
}

// ---------------------------------------------------------------------------
// NTSC lead_in / lead_out
// ---------------------------------------------------------------------------

TEST(LinePlacementTest, NtscCavLeadInLines17And18GetLeadInCode) {
  LinePlacementEngine engine(Standard::kNtsc, DiscType::kCAV,
                             SectionType::kLeadIn, {"lead_in"});
  for (int line : {17, 18}) {
    auto a = engine.GetAssignment(line, true);
    EXPECT_TRUE(a.assigned) << "line=" << line;
    EXPECT_EQ(a.code_type, "lead_in") << "line=" << line;
  }
}

TEST(LinePlacementTest, NtscCavLeadInField2Lines280And281GetLeadInCode) {
  LinePlacementEngine engine(Standard::kNtsc, DiscType::kCAV,
                             SectionType::kLeadIn, {"lead_in"});
  for (int line : {280, 281}) {
    auto a = engine.GetAssignment(line, false);
    EXPECT_TRUE(a.assigned) << "line=" << line;
    EXPECT_EQ(a.code_type, "lead_in") << "line=" << line;
  }
}

TEST(LinePlacementTest, NtscLeadInLine16GetsUsersCode) {
  LinePlacementEngine engine(Standard::kNtsc, DiscType::kCAV,
                             SectionType::kLeadIn, {"lead_in", "users_code"});
  auto a = engine.GetAssignment(16, true);
  EXPECT_TRUE(a.assigned);
  EXPECT_EQ(a.code_type, "users_code");
}

TEST(LinePlacementTest, NtscLeadInField2Line279GetsUsersCode) {
  LinePlacementEngine engine(Standard::kNtsc, DiscType::kCAV,
                             SectionType::kLeadIn, {"lead_in", "users_code"});
  auto a = engine.GetAssignment(279, false);
  EXPECT_TRUE(a.assigned);
  EXPECT_EQ(a.code_type, "users_code");
}

// White flag in lead_in: line 11 (field 1) only, line 274 NOT used.
TEST(LinePlacementTest, NtscLeadInWhiteFlagOnLine11Only) {
  LinePlacementEngine engine(Standard::kNtsc, DiscType::kCAV,
                             SectionType::kLeadIn,
                             {"lead_in", "fm_white_flag"});

  auto a11 = engine.GetAssignment(11, true);
  EXPECT_TRUE(a11.assigned);
  EXPECT_TRUE(a11.is_white_flag);
  EXPECT_EQ(a11.code_type, "fm_white_flag");

  // Line 274 (field 2) must NOT emit white flag during lead-in.
  auto a274 = engine.GetAssignment(274, false);
  EXPECT_FALSE(a274.assigned)
      << "White flag must not appear on line 274 during lead-in";
}

// White flag in lead_out: both lines 11 and 274.
TEST(LinePlacementTest, NtscLeadOutWhiteFlagOnBothLines11And274) {
  LinePlacementEngine engine(Standard::kNtsc, DiscType::kCAV,
                             SectionType::kLeadOut,
                             {"lead_out", "fm_white_flag"});

  auto a11 = engine.GetAssignment(11, true);
  EXPECT_TRUE(a11.assigned);
  EXPECT_TRUE(a11.is_white_flag);

  auto a274 = engine.GetAssignment(274, false);
  EXPECT_TRUE(a274.assigned);
  EXPECT_TRUE(a274.is_white_flag);
}

// Line 10 and 273 carry no data in lead-in/lead-out.
TEST(LinePlacementTest, NtscLeadInLine10And273ReturnNone) {
  LinePlacementEngine engine(Standard::kNtsc, DiscType::kCAV,
                             SectionType::kLeadIn,
                             {"lead_in", "fm_white_flag"});
  EXPECT_FALSE(engine.GetAssignment(10, true).assigned);
  EXPECT_FALSE(engine.GetAssignment(273, false).assigned);
}

// ---------------------------------------------------------------------------
// NTSC CAV programme_area — FM lines
// ---------------------------------------------------------------------------

TEST(LinePlacementTest, NtscCavProgrammeFmPictureNumberOnLines10And273) {
  LinePlacementEngine engine(
      Standard::kNtsc, DiscType::kCAV, SectionType::kProgrammeArea,
      {"picture_number", "fm_picture_number", "fm_white_flag"});

  auto a10 = engine.GetAssignment(10, true);
  EXPECT_TRUE(a10.assigned);
  EXPECT_TRUE(a10.is_fm);
  EXPECT_EQ(a10.code_type, "fm_picture_number");
  EXPECT_FALSE(a10.is_white_flag);

  auto a273 = engine.GetAssignment(273, false);
  EXPECT_TRUE(a273.assigned);
  EXPECT_TRUE(a273.is_fm);
  EXPECT_EQ(a273.code_type, "fm_picture_number");
}

TEST(LinePlacementTest, NtscCavProgrammeWhiteFlagOnLine11OnlyFirstField) {
  LinePlacementEngine engine(
      Standard::kNtsc, DiscType::kCAV, SectionType::kProgrammeArea,
      {"picture_number", "fm_picture_number", "fm_white_flag"});

  // White flag in programme area is first-field only (IEC 60857 §10.2.4).
  auto a11 = engine.GetAssignment(11, true);
  EXPECT_TRUE(a11.assigned);
  EXPECT_TRUE(a11.is_white_flag);

  // Line 274 (field 2) must NOT emit white flag in programme area.
  auto a274 = engine.GetAssignment(274, false);
  EXPECT_FALSE(a274.assigned)
      << "White flag must not appear on line 274 in programme area";
}

TEST(LinePlacementTest, NtscCavProgrammeNoFmWhenNotPresent) {
  LinePlacementEngine engine(Standard::kNtsc, DiscType::kCAV,
                             SectionType::kProgrammeArea, {"picture_number"});
  EXPECT_FALSE(engine.GetAssignment(10, true).assigned);
  EXPECT_FALSE(engine.GetAssignment(11, true).assigned);
  EXPECT_FALSE(engine.GetAssignment(273, false).assigned);
  EXPECT_FALSE(engine.GetAssignment(274, false).assigned);
}

// ---------------------------------------------------------------------------
// NTSC CAV programme_area — 24-bit biphase lines
// ---------------------------------------------------------------------------

TEST(LinePlacementTest, NtscCavProgrammePictureNumberOnLines17And18) {
  LinePlacementEngine engine(Standard::kNtsc, DiscType::kCAV,
                             SectionType::kProgrammeArea, {"picture_number"});
  for (int line : {17, 18}) {
    auto a = engine.GetAssignment(line, true);
    EXPECT_TRUE(a.assigned) << "line=" << line;
    EXPECT_EQ(a.code_type, "picture_number") << "line=" << line;
    EXPECT_FALSE(a.is_fm) << "line=" << line;
  }
}

TEST(LinePlacementTest, NtscCavProgrammePictureNumberFirstFieldOnly) {
  // IEC 60857 §10.1.4: picture numbers go in the first field of the picture.
  LinePlacementEngine engine(Standard::kNtsc, DiscType::kCAV,
                             SectionType::kProgrammeArea, {"picture_number"});
  for (int line : {280, 281}) {
    EXPECT_FALSE(engine.GetAssignment(line, false).assigned) << "line=" << line;
  }
}

TEST(LinePlacementTest, NtscCavProgrammeProgrammeStatusOnLine16With172h) {
  LinePlacementEngine engine(Standard::kNtsc, DiscType::kCAV,
                             SectionType::kProgrammeArea, {"programme_status"});
  auto a = engine.GetAssignment(16, true);
  EXPECT_TRUE(a.assigned);
  EXPECT_EQ(a.code_type, "programme_status");
  EXPECT_TRUE(a.uses_172h_offset);

  auto a279 = engine.GetAssignment(279, false);
  EXPECT_TRUE(a279.assigned);
  EXPECT_EQ(a279.code_type, "programme_status");
  EXPECT_TRUE(a279.uses_172h_offset);
}

TEST(LinePlacementTest,
     NtscCavProgrammePictureStopBeforeProgrammeStatusOnLine16) {
  LinePlacementEngine engine(Standard::kNtsc, DiscType::kCAV,
                             SectionType::kProgrammeArea,
                             {"picture_stop", "programme_status"});
  auto a = engine.GetAssignment(16, true);
  EXPECT_EQ(a.code_type, "picture_stop");
  EXPECT_FALSE(a.uses_172h_offset);
}

TEST(LinePlacementTest, NtscCavProgrammePriorityOnLine17) {
  // picture_number > picture_stop > chapter_number
  {
    LinePlacementEngine e(Standard::kNtsc, DiscType::kCAV,
                          SectionType::kProgrammeArea,
                          {"picture_number", "picture_stop", "chapter_number"});
    EXPECT_EQ(e.GetAssignment(17, true).code_type, "picture_number");
  }
  {
    LinePlacementEngine e(Standard::kNtsc, DiscType::kCAV,
                          SectionType::kProgrammeArea,
                          {"picture_stop", "chapter_number"});
    EXPECT_EQ(e.GetAssignment(17, true).code_type, "picture_stop");
  }
  {
    LinePlacementEngine e(Standard::kNtsc, DiscType::kCAV,
                          SectionType::kProgrammeArea, {"chapter_number"});
    EXPECT_EQ(e.GetAssignment(17, true).code_type, "chapter_number");
  }
}

TEST(LinePlacementTest, NtscCavProgrammeChapterInFieldWithoutPictureNumber) {
  {
    LinePlacementEngine e(Standard::kNtsc, DiscType::kCAV,
                          SectionType::kProgrammeArea,
                          {"picture_number", "chapter_number"});
    EXPECT_EQ(e.GetAssignment(18, true).code_type, "picture_number");
    EXPECT_EQ(e.GetAssignment(281, false).code_type, "chapter_number");
  }
  {
    // Without picture numbers every field is a chapter field.
    LinePlacementEngine e(Standard::kNtsc, DiscType::kCAV,
                          SectionType::kProgrammeArea, {"chapter_number"});
    EXPECT_EQ(e.GetAssignment(18, true).code_type, "chapter_number");
    EXPECT_EQ(e.GetAssignment(281, false).code_type, "chapter_number");
  }
}

// ---------------------------------------------------------------------------
// NTSC CLV programme_area — FM lines
// ---------------------------------------------------------------------------

TEST(LinePlacementTest, NtscClvProgrammeFmProgrammeTimeOnLines10And273) {
  LinePlacementEngine engine(
      Standard::kNtsc, DiscType::kCLV, SectionType::kProgrammeArea,
      {"programme_time_code", "fm_programme_time", "fm_white_flag"});

  auto a10 = engine.GetAssignment(10, true);
  EXPECT_TRUE(a10.assigned);
  EXPECT_TRUE(a10.is_fm);
  EXPECT_EQ(a10.code_type, "fm_programme_time");

  auto a273 = engine.GetAssignment(273, false);
  EXPECT_TRUE(a273.assigned);
  EXPECT_TRUE(a273.is_fm);
  EXPECT_EQ(a273.code_type, "fm_programme_time");
}

TEST(LinePlacementTest, NtscClvProgrammeWhiteFlagOnLine11OnlyFirstField) {
  LinePlacementEngine engine(Standard::kNtsc, DiscType::kCLV,
                             SectionType::kProgrammeArea,
                             {"fm_programme_time", "fm_white_flag"});

  // White flag in programme area is first-field only (IEC 60857 §10.2.4).
  EXPECT_TRUE(engine.GetAssignment(11, true).is_white_flag);
  // Line 274 (field 2) must NOT emit white flag in programme area.
  EXPECT_FALSE(engine.GetAssignment(274, false).assigned)
      << "White flag must not appear on line 274 in programme area";
}

// ---------------------------------------------------------------------------
// NTSC CLV programme_area — 24-bit biphase lines
// ---------------------------------------------------------------------------

TEST(LinePlacementTest, NtscClvProgrammeClvPictureNumberOnLine16) {
  LinePlacementEngine engine(Standard::kNtsc, DiscType::kCLV,
                             SectionType::kProgrammeArea,
                             {"clv_picture_number"});
  auto a16 = engine.GetAssignment(16, true);
  EXPECT_TRUE(a16.assigned);
  EXPECT_EQ(a16.code_type, "clv_picture_number");

  // IEC 60857 Amd 2: first field of the picture only.
  EXPECT_FALSE(engine.GetAssignment(279, false).assigned);
}

TEST(LinePlacementTest, NtscClvProgrammeProgrammeTimeCodeOnLines17And18) {
  LinePlacementEngine engine(Standard::kNtsc, DiscType::kCLV,
                             SectionType::kProgrammeArea,
                             {"programme_time_code"});
  for (int line : {17, 18}) {
    auto a = engine.GetAssignment(line, true);
    EXPECT_TRUE(a.assigned) << "line=" << line;
    EXPECT_EQ(a.code_type, "programme_time_code") << "line=" << line;
    EXPECT_FALSE(a.is_fm) << "line=" << line;
  }
}

TEST(LinePlacementTest, NtscClvProgrammeProgrammeTimeCodeFirstFieldOnly) {
  LinePlacementEngine engine(Standard::kNtsc, DiscType::kCLV,
                             SectionType::kProgrammeArea,
                             {"programme_time_code"});
  for (int line : {280, 281}) {
    EXPECT_FALSE(engine.GetAssignment(line, false).assigned) << "line=" << line;
  }
}

// NTSC clv_code: uses 0.172H offset (IEC 60857 Figure 11).
TEST(LinePlacementTest, NtscClvProgrammeClvCodeOnLine17With172hOffset) {
  LinePlacementEngine engine(Standard::kNtsc, DiscType::kCLV,
                             SectionType::kProgrammeArea, {"clv_code"});
  auto a = engine.GetAssignment(17, true);
  EXPECT_TRUE(a.assigned);
  EXPECT_EQ(a.code_type, "clv_code");
  EXPECT_TRUE(a.uses_172h_offset);  // NTSC only — IEC 60857 Figure 11
}

TEST(LinePlacementTest, NtscClvProgrammeClvCodeOnField2Line280With172h) {
  LinePlacementEngine engine(Standard::kNtsc, DiscType::kCLV,
                             SectionType::kProgrammeArea, {"clv_code"});
  auto a = engine.GetAssignment(280, false);
  EXPECT_TRUE(a.assigned);
  EXPECT_EQ(a.code_type, "clv_code");
  EXPECT_TRUE(a.uses_172h_offset);
}

TEST(LinePlacementTest,
     NtscClvProgrammeProgrammeTimeCodeBeforeClvCodeOnLine17) {
  LinePlacementEngine engine(Standard::kNtsc, DiscType::kCLV,
                             SectionType::kProgrammeArea,
                             {"programme_time_code", "clv_code"});
  auto a = engine.GetAssignment(17, true);
  EXPECT_EQ(a.code_type, "programme_time_code");
  EXPECT_FALSE(a.uses_172h_offset);
}

TEST(LinePlacementTest, NtscClvProgrammeChapterOnLine18WhenNoPtc) {
  LinePlacementEngine engine(Standard::kNtsc, DiscType::kCLV,
                             SectionType::kProgrammeArea, {"chapter_number"});
  auto a = engine.GetAssignment(18, true);
  EXPECT_TRUE(a.assigned);
  EXPECT_EQ(a.code_type, "chapter_number");
}

TEST(LinePlacementTest, NtscClvProgrammeChapterInFieldWithoutTimeCode) {
  LinePlacementEngine engine(Standard::kNtsc, DiscType::kCLV,
                             SectionType::kProgrammeArea,
                             {"programme_time_code", "chapter_number"});
  EXPECT_EQ(engine.GetAssignment(18, true).code_type, "programme_time_code");
  EXPECT_EQ(engine.GetAssignment(281, false).code_type, "chapter_number");
}

// ---------------------------------------------------------------------------
// NTSC reserved lines 12–15 and 275–278 return unassigned
// ---------------------------------------------------------------------------

TEST(LinePlacementTest, NtscReservedNonActiveLines) {
  LinePlacementEngine engine(
      Standard::kNtsc, DiscType::kCAV, SectionType::kProgrammeArea,
      {"picture_number", "fm_picture_number", "fm_white_flag"});
  for (int line : {12, 13, 14, 15}) {
    EXPECT_FALSE(engine.GetAssignment(line, true).assigned)
        << "Expected no code on reserved NTSC line " << line;
  }
  for (int line : {275, 276, 277, 278}) {
    EXPECT_FALSE(engine.GetAssignment(line, false).assigned)
        << "Expected no code on reserved NTSC line " << line;
  }
}

// ---------------------------------------------------------------------------
// Cross-check: is_fm is false for 24-bit biphase codes
// ---------------------------------------------------------------------------

TEST(LinePlacementTest, BiPhaseAssignmentHasIsFmFalse) {
  LinePlacementEngine engine(Standard::kPal, DiscType::kCAV,
                             SectionType::kProgrammeArea,
                             {"picture_number", "programme_status"});
  auto a17 = engine.GetAssignment(17, true);
  EXPECT_FALSE(a17.is_fm);
  EXPECT_FALSE(a17.is_white_flag);

  auto a16 = engine.GetAssignment(16, true);
  EXPECT_FALSE(a16.is_fm);
  EXPECT_FALSE(a16.is_white_flag);
}

// ---------------------------------------------------------------------------
// Ensure programme_status does NOT get 0.172H offset when picture_stop wins
// ---------------------------------------------------------------------------

TEST(LinePlacementTest, PictureStopHasNo172hOffset) {
  LinePlacementEngine engine(Standard::kPal, DiscType::kCAV,
                             SectionType::kProgrammeArea,
                             {"picture_stop", "programme_status"});
  auto a = engine.GetAssignment(16, true);
  EXPECT_EQ(a.code_type, "picture_stop");
  EXPECT_FALSE(a.uses_172h_offset);
}

}  // namespace
}  // namespace videosynth
