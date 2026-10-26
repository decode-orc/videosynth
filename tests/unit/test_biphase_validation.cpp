/*
 * File:        test_biphase_validation.cpp
 * Module:      project_validator_tests
 * Purpose:     Validates biphase-specific section type, code parameter, and
 *              section duration constraints (IEC 60856/60857 §§ 5.4, 5.5,
 *              5.9, 5.10).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <string>

#include "videosynth/project_validator.h"

namespace videosynth {
namespace {

// ─── helpers
// ──────────────────────────────────────────────────────────────────

Project MakeBasePalProject() {
  Project p;
  p.cvbs_presets.video_standard_preset = Standard::kPal;
  p.cvbs_presets.sample_encoding_preset = "CVBS_U10_4FSC";
  p.cvbs_presets.signal_state_preset = "STANDARD_STABLE_LOCKED";
  p.output.video_path = "/tmp/bv_test.cvbs";
  p.output.metadata_path = "/tmp/bv_test.meta";
  return p;
}

Project MakeBaseNtscProject() {
  Project p = MakeBasePalProject();
  p.cvbs_presets.video_standard_preset = Standard::kNtsc;
  return p;
}

Section MakeSection(SectionType stype, int frames = 100) {
  Section s;
  s.name = "Section";
  s.type = "progressive";
  s.section_type = stype;
  s.source = "fixture.exr";
  s.duration_frames = frames;
  return s;
}

Section::LineInjection MakeLaserdiscInjection() {
  Section::LineInjection inj;
  inj.type = "laserdisc";
  return inj;
}

Section::LineInjectionCode MakeCode(const std::string& code_type) {
  Section::LineInjectionCode c;
  c.code_type = code_type;
  return c;
}

// ─── Task 5.4: section_type requirement
// ───────────────────────────────────────

TEST(BiphaseValidationTest, RequiresSectionTypeWhenDiscTypeIsPresent) {
  // section_type defaults to kUnknown; should be rejected when disc_type set.
  Project p = MakeBasePalProject();
  Section s = MakeSection(SectionType::kUnknown);
  p.line_injections.disc_type = "CAV";
  auto inj = MakeLaserdiscInjection();
  s.line_injections.push_back(inj);
  p.sections.push_back(s);

  ProjectValidator v;
  const auto r = v.Validate(p);

  EXPECT_FALSE(r.is_valid);
  ASSERT_FALSE(r.errors.empty());
  EXPECT_NE(r.errors[0].find("section_type"), std::string::npos);
}

TEST(BiphaseValidationTest, AcceptsSectionTypeLeadInWithDiscType) {
  Project p = MakeBasePalProject();
  Section s = MakeSection(SectionType::kLeadIn, 938);
  p.line_injections.disc_type = "CAV";
  auto inj = MakeLaserdiscInjection();
  inj.codes.push_back(MakeCode("lead_in"));
  s.line_injections.push_back(inj);
  p.sections.push_back(s);

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_TRUE(r.is_valid) << (!r.errors.empty() ? r.errors[0] : "");
}

TEST(BiphaseValidationTest, AcceptsSectionTypeProgrammeAreaWithDiscType) {
  Project p = MakeBasePalProject();
  Section s = MakeSection(SectionType::kProgrammeArea);
  p.line_injections.disc_type = "CAV";
  auto inj = MakeLaserdiscInjection();
  inj.codes.push_back(MakeCode("picture_number"));
  s.line_injections.push_back(inj);
  p.sections.push_back(s);

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_TRUE(r.is_valid) << (!r.errors.empty() ? r.errors[0] : "");
}

TEST(BiphaseValidationTest, AcceptsSectionTypeLeadOutWithDiscType) {
  Project p = MakeBasePalProject();
  Section s = MakeSection(SectionType::kLeadOut, 1250);
  p.line_injections.disc_type = "CAV";
  auto inj = MakeLaserdiscInjection();
  inj.codes.push_back(MakeCode("lead_out"));
  s.line_injections.push_back(inj);
  p.sections.push_back(s);

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_TRUE(r.is_valid) << (!r.errors.empty() ? r.errors[0] : "");
}

// ─── Minimum chapter length (IEC 60856/60857 §10.1.5) ───────────────────────

Section MakeChapterSection(int frames, bool chapter_specified, int chapter) {
  Section s = MakeSection(SectionType::kProgrammeArea, frames);
  auto inj = MakeLaserdiscInjection();
  inj.codes.push_back(MakeCode("picture_number"));
  Section::LineInjectionCode code = MakeCode("chapter_number");
  code.chapter_specified = chapter_specified;
  code.chapter = chapter;
  inj.codes.push_back(code);
  s.line_injections.push_back(inj);
  return s;
}

bool HasChapterWarning(const ValidationResult& r) {
  return std::any_of(r.warnings.begin(), r.warnings.end(),
                     [](const std::string& w) {
                       return w.find("CAV chapter") != std::string::npos;
                     });
}

TEST(BiphaseValidationTest, WarnsWhenCavChapterIsShorterThan30Frames) {
  Project p = MakeBasePalProject();
  p.line_injections.disc_type = "CAV";
  p.sections.push_back(MakeChapterSection(29, true, 1));
  p.sections.push_back(MakeChapterSection(100, true, 2));

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_TRUE(r.is_valid) << (!r.errors.empty() ? r.errors[0] : "");
  ASSERT_TRUE(HasChapterWarning(r));
}

TEST(BiphaseValidationTest, ContinuedChapterCountsEverySection) {
  // A section without `chapter` continues the previous chapter, so two
  // 20-frame sections form one 40-frame chapter.
  Project p = MakeBasePalProject();
  p.line_injections.disc_type = "CAV";
  p.sections.push_back(MakeChapterSection(20, true, 1));
  p.sections.push_back(MakeChapterSection(20, false, 0));

  ProjectValidator v;
  EXPECT_FALSE(HasChapterWarning(v.Validate(p)));
}

TEST(BiphaseValidationTest, ClvChapterLengthIsNotChecked) {
  Project p = MakeBasePalProject();
  p.line_injections.disc_type = "CLV";
  Section s = MakeSection(SectionType::kProgrammeArea, 10);
  auto inj = MakeLaserdiscInjection();
  inj.codes.push_back(MakeCode("chapter_number"));
  s.line_injections.push_back(inj);
  p.sections.push_back(s);

  ProjectValidator v;
  EXPECT_FALSE(HasChapterWarning(v.Validate(p)));
}

// ─── Task 5.4: code_type / section_type compatibility matrix ─────────────────

TEST(BiphaseValidationTest, RejectsLeadInCodeInProgrammeAreaSection) {
  Project p = MakeBasePalProject();
  Section s = MakeSection(SectionType::kProgrammeArea);
  p.line_injections.disc_type = "CAV";
  auto inj = MakeLaserdiscInjection();
  inj.codes.push_back(MakeCode("lead_in"));
  s.line_injections.push_back(inj);
  p.sections.push_back(s);

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_FALSE(r.is_valid);
  ASSERT_FALSE(r.errors.empty());
  EXPECT_NE(r.errors[0].find("lead_in"), std::string::npos);
}

TEST(BiphaseValidationTest, RejectsLeadOutCodeInProgrammeAreaSection) {
  Project p = MakeBasePalProject();
  Section s = MakeSection(SectionType::kProgrammeArea);
  p.line_injections.disc_type = "CAV";
  auto inj = MakeLaserdiscInjection();
  inj.codes.push_back(MakeCode("lead_out"));
  s.line_injections.push_back(inj);
  p.sections.push_back(s);

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_FALSE(r.is_valid);
}

TEST(BiphaseValidationTest, RejectsPictureNumberInLeadInSection) {
  Project p = MakeBasePalProject();
  Section s = MakeSection(SectionType::kLeadIn, 938);
  p.line_injections.disc_type = "CAV";
  auto inj = MakeLaserdiscInjection();
  inj.codes.push_back(MakeCode("picture_number"));
  s.line_injections.push_back(inj);
  p.sections.push_back(s);

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_FALSE(r.is_valid);
  ASSERT_FALSE(r.errors.empty());
  EXPECT_NE(r.errors[0].find("picture_number"), std::string::npos);
}

TEST(BiphaseValidationTest, RejectsPictureNumberInLeadOutSection) {
  Project p = MakeBasePalProject();
  Section s = MakeSection(SectionType::kLeadOut, 1250);
  p.line_injections.disc_type = "CAV";
  auto inj = MakeLaserdiscInjection();
  inj.codes.push_back(MakeCode("picture_number"));
  s.line_injections.push_back(inj);
  p.sections.push_back(s);

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_FALSE(r.is_valid);
}

TEST(BiphaseValidationTest, RejectsUsersCodeInProgrammeArea) {
  Project p = MakeBasePalProject();
  Section s = MakeSection(SectionType::kProgrammeArea);
  p.line_injections.disc_type = "CAV";
  auto inj = MakeLaserdiscInjection();
  inj.codes.push_back(MakeCode("users_code"));
  s.line_injections.push_back(inj);
  p.sections.push_back(s);

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_FALSE(r.is_valid);
  ASSERT_FALSE(r.errors.empty());
  EXPECT_NE(r.errors[0].find("users_code"), std::string::npos);
}

TEST(BiphaseValidationTest, AcceptsUsersCodeInLeadIn) {
  Project p = MakeBasePalProject();
  Section s = MakeSection(SectionType::kLeadIn, 938);
  p.line_injections.disc_type = "CAV";
  auto inj = MakeLaserdiscInjection();
  auto code = MakeCode("users_code");
  code.users_code =
      "0x80D234";  // X1=0, D=0xD (canonical format per IEC §10.1.9)
  code.users_code_specified = true;
  inj.codes.push_back(code);
  inj.codes.push_back(MakeCode("lead_in"));
  s.line_injections.push_back(inj);
  p.sections.push_back(s);

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_TRUE(r.is_valid) << (!r.errors.empty() ? r.errors[0] : "");
}

TEST(BiphaseValidationTest, AcceptsUsersCodeInLeadOut) {
  Project p = MakeBasePalProject();
  Section s = MakeSection(SectionType::kLeadOut, 1250);
  p.line_injections.disc_type = "CAV";
  auto inj = MakeLaserdiscInjection();
  auto code = MakeCode("users_code");
  code.users_code =
      "0x80D234";  // X1=0, D=0xD (canonical format per IEC §10.1.9)
  code.users_code_specified = true;
  inj.codes.push_back(code);
  inj.codes.push_back(MakeCode("lead_out"));
  s.line_injections.push_back(inj);
  p.sections.push_back(s);

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_TRUE(r.is_valid) << (!r.errors.empty() ? r.errors[0] : "");
}

TEST(BiphaseValidationTest, AcceptsFmWhiteFlagInAllSections) {
  // fm_white_flag is allowed in lead_in, programme_area, and lead_out on NTSC.
  for (SectionType st : {SectionType::kLeadIn, SectionType::kProgrammeArea,
                         SectionType::kLeadOut}) {
    Project p = MakeBaseNtscProject();
    int frames = (st == SectionType::kLeadIn)    ? 938
                 : (st == SectionType::kLeadOut) ? 1250
                                                 : 100;
    Section s = MakeSection(st, frames);
    p.line_injections.disc_type = "CAV";
    auto inj = MakeLaserdiscInjection();
    inj.codes.push_back(MakeCode("fm_white_flag"));
    if (st == SectionType::kLeadIn) inj.codes.push_back(MakeCode("lead_in"));
    if (st == SectionType::kLeadOut) inj.codes.push_back(MakeCode("lead_out"));
    if (st == SectionType::kProgrammeArea) {
      inj.codes.push_back(MakeCode("picture_number"));
    }
    s.line_injections.push_back(inj);

    // Add mandatory NTSC virs.
    p.line_injections.vits.push_back(VitsInjection{"virs", {19, 282}});

    p.sections.push_back(s);
    ProjectValidator v;
    const auto r = v.Validate(p);
    EXPECT_TRUE(r.is_valid) << "section_type=" << SectionTypeToString(st)
                            << (!r.errors.empty() ? ": " + r.errors[0] : "");
  }
}

TEST(BiphaseValidationTest, RejectsProgrammeTimeCodeInLeadIn) {
  Project p = MakeBasePalProject();
  Section s = MakeSection(SectionType::kLeadIn, 938);
  p.line_injections.disc_type = "CLV";
  auto inj = MakeLaserdiscInjection();
  inj.codes.push_back(MakeCode("programme_time_code"));
  s.line_injections.push_back(inj);
  p.sections.push_back(s);

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_FALSE(r.is_valid);
  ASSERT_FALSE(r.errors.empty());
  EXPECT_NE(r.errors[0].find("programme_time_code"), std::string::npos);
}

// ─── Task 5.5: NTSC-only FM code restrictions
// ─────────────────────────────────

TEST(BiphaseValidationTest, RejectsFmPictureNumberOnPalProject) {
  Project p = MakeBasePalProject();
  Section s = MakeSection(SectionType::kProgrammeArea);
  p.line_injections.disc_type = "CAV";
  auto inj = MakeLaserdiscInjection();
  inj.codes.push_back(MakeCode("fm_picture_number"));
  inj.codes.push_back(MakeCode("picture_number"));
  s.line_injections.push_back(inj);
  p.sections.push_back(s);

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_FALSE(r.is_valid);
  ASSERT_FALSE(r.errors.empty());
  EXPECT_NE(r.errors[0].find("fm_picture_number"), std::string::npos);
  EXPECT_NE(r.errors[0].find("NTSC"), std::string::npos);
}

TEST(BiphaseValidationTest, RejectsFmWhiteFlagOnPalProject) {
  Project p = MakeBasePalProject();
  Section s = MakeSection(SectionType::kProgrammeArea);
  p.line_injections.disc_type = "CAV";
  auto inj = MakeLaserdiscInjection();
  inj.codes.push_back(MakeCode("fm_white_flag"));
  inj.codes.push_back(MakeCode("picture_number"));
  s.line_injections.push_back(inj);
  p.sections.push_back(s);

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_FALSE(r.is_valid);
  ASSERT_FALSE(r.errors.empty());
  EXPECT_NE(r.errors[0].find("NTSC"), std::string::npos);
}

// ─── Task 5.5: picture_number value range
// ─────────────────────────────────────

TEST(BiphaseValidationTest, AcceptsPalPictureNumberAtMaximum) {
  Project p = MakeBasePalProject();
  Section s = MakeSection(SectionType::kProgrammeArea);
  p.line_injections.disc_type = "CAV";
  auto inj = MakeLaserdiscInjection();
  auto code = MakeCode("picture_number");
  code.start_value = 99999;
  code.start_value_specified = true;
  inj.codes.push_back(code);
  s.line_injections.push_back(inj);
  p.sections.push_back(s);

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_TRUE(r.is_valid) << (!r.errors.empty() ? r.errors[0] : "");
}

TEST(BiphaseValidationTest, RejectsPalPictureNumberAboveMaximum) {
  Project p = MakeBasePalProject();
  Section s = MakeSection(SectionType::kProgrammeArea);
  p.line_injections.disc_type = "CAV";
  auto inj = MakeLaserdiscInjection();
  auto code = MakeCode("picture_number");
  code.start_value = 100000;
  code.start_value_specified = true;
  inj.codes.push_back(code);
  s.line_injections.push_back(inj);
  p.sections.push_back(s);

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_FALSE(r.is_valid);
  ASSERT_FALSE(r.errors.empty());
  EXPECT_NE(r.errors[0].find("picture_number"), std::string::npos);
}

TEST(BiphaseValidationTest, AcceptsNtscPictureNumberAtMaximum) {
  Project p = MakeBaseNtscProject();
  Section s = MakeSection(SectionType::kProgrammeArea);
  p.line_injections.disc_type = "CAV";
  auto inj = MakeLaserdiscInjection();
  auto code = MakeCode("picture_number");
  code.start_value = 79999;
  code.start_value_specified = true;
  inj.codes.push_back(code);
  inj.codes.push_back(MakeCode("fm_picture_number"));
  s.line_injections.push_back(inj);

  p.line_injections.vits.push_back(VitsInjection{"virs", {19, 282}});

  p.sections.push_back(s);
  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_TRUE(r.is_valid) << (!r.errors.empty() ? r.errors[0] : "");
}

TEST(BiphaseValidationTest, RejectsNtscPictureNumberAboveNtscMaximum) {
  Project p = MakeBaseNtscProject();
  Section s = MakeSection(SectionType::kProgrammeArea);
  p.line_injections.disc_type = "CAV";
  p.line_injections.vits.push_back(VitsInjection{"virs", {19, 282}});
  auto inj = MakeLaserdiscInjection();
  auto code = MakeCode("picture_number");
  code.start_value = 80000;
  code.start_value_specified = true;
  inj.codes.push_back(code);
  s.line_injections.push_back(inj);
  p.sections.push_back(s);

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_FALSE(r.is_valid);
  ASSERT_FALSE(r.errors.empty());
  EXPECT_NE(r.errors[0].find("picture_number"), std::string::npos);
}

TEST(BiphaseValidationTest, RejectsPictureNumberBelowZero) {
  Project p = MakeBasePalProject();
  Section s = MakeSection(SectionType::kProgrammeArea);
  p.line_injections.disc_type = "CAV";
  auto inj = MakeLaserdiscInjection();
  auto code = MakeCode("picture_number");
  code.start_value = -1;
  code.start_value_specified = true;
  inj.codes.push_back(code);
  s.line_injections.push_back(inj);
  p.sections.push_back(s);

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_FALSE(r.is_valid);
}

// ─── Task 5.5: fm_picture_number value range
// ──────────────────────────────────

TEST(BiphaseValidationTest, AcceptsFmPictureNumberAtMaximum) {
  Project p = MakeBaseNtscProject();
  Section s = MakeSection(SectionType::kProgrammeArea);
  p.line_injections.disc_type = "CAV";
  auto inj = MakeLaserdiscInjection();
  auto code = MakeCode("fm_picture_number");
  code.start_value = 79999;
  code.start_value_specified = true;
  inj.codes.push_back(code);
  inj.codes.push_back(MakeCode("picture_number"));
  s.line_injections.push_back(inj);

  p.line_injections.vits.push_back(VitsInjection{"virs", {19, 282}});

  p.sections.push_back(s);
  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_TRUE(r.is_valid) << (!r.errors.empty() ? r.errors[0] : "");
}

TEST(BiphaseValidationTest, RejectsFmPictureNumberAboveMaximum) {
  Project p = MakeBaseNtscProject();
  Section s = MakeSection(SectionType::kProgrammeArea);
  p.line_injections.disc_type = "CAV";
  p.line_injections.vits.push_back(VitsInjection{"virs", {19, 282}});
  auto inj = MakeLaserdiscInjection();
  auto code = MakeCode("fm_picture_number");
  code.start_value = 80000;
  code.start_value_specified = true;
  inj.codes.push_back(code);
  s.line_injections.push_back(inj);
  p.sections.push_back(s);

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_FALSE(r.is_valid);
  ASSERT_FALSE(r.errors.empty());
  EXPECT_NE(r.errors[0].find("fm_picture_number"), std::string::npos);
}

// ─── Task 5.5: chapter_number range
// ───────────────────────────────────────────

TEST(BiphaseValidationTest, AcceptsChapterNumberAtMaximum) {
  Project p = MakeBasePalProject();
  Section s = MakeSection(SectionType::kProgrammeArea);
  p.line_injections.disc_type = "CAV";
  auto inj = MakeLaserdiscInjection();
  auto code = MakeCode("chapter_number");
  code.chapter = 79;
  code.chapter_specified = true;
  inj.codes.push_back(code);
  s.line_injections.push_back(inj);
  p.sections.push_back(s);

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_TRUE(r.is_valid) << (!r.errors.empty() ? r.errors[0] : "");
}

TEST(BiphaseValidationTest, AcceptsChapterNumberAtZero) {
  Project p = MakeBasePalProject();
  Section s = MakeSection(SectionType::kProgrammeArea);
  p.line_injections.disc_type = "CAV";
  auto inj = MakeLaserdiscInjection();
  auto code = MakeCode("chapter_number");
  code.chapter = 0;
  code.chapter_specified = true;
  inj.codes.push_back(code);
  s.line_injections.push_back(inj);
  p.sections.push_back(s);

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_TRUE(r.is_valid) << (!r.errors.empty() ? r.errors[0] : "");
}

TEST(BiphaseValidationTest, RejectsChapterNumberAboveMaximum) {
  Project p = MakeBasePalProject();
  Section s = MakeSection(SectionType::kProgrammeArea);
  p.line_injections.disc_type = "CAV";
  auto inj = MakeLaserdiscInjection();
  auto code = MakeCode("chapter_number");
  code.chapter = 80;
  code.chapter_specified = true;
  inj.codes.push_back(code);
  s.line_injections.push_back(inj);
  p.sections.push_back(s);

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_FALSE(r.is_valid);
  ASSERT_FALSE(r.errors.empty());
  EXPECT_NE(r.errors[0].find("chapter"), std::string::npos);
}

TEST(BiphaseValidationTest, RejectsChapterNumberBelowZero) {
  Project p = MakeBasePalProject();
  Section s = MakeSection(SectionType::kProgrammeArea);
  p.line_injections.disc_type = "CAV";
  auto inj = MakeLaserdiscInjection();
  auto code = MakeCode("chapter_number");
  code.chapter = -1;
  code.chapter_specified = true;
  inj.codes.push_back(code);
  s.line_injections.push_back(inj);
  p.sections.push_back(s);

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_FALSE(r.is_valid);
}

// ─── Task 5.5: users_code X1 nibble constraint
// ────────────────────────────────

TEST(BiphaseValidationTest, AcceptsUsersCodeWithX1EqualSeven) {
  Project p = MakeBasePalProject();
  Section s = MakeSection(SectionType::kLeadIn, 938);
  p.line_injections.disc_type = "CAV";
  auto inj = MakeLaserdiscInjection();
  inj.codes.push_back(MakeCode("lead_in"));
  auto code = MakeCode("users_code");
  code.users_code = "0x87D234";  // X1=7 (max valid), D=0xD (canonical)
  code.users_code_specified = true;
  inj.codes.push_back(code);
  s.line_injections.push_back(inj);
  p.sections.push_back(s);

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_TRUE(r.is_valid) << (!r.errors.empty() ? r.errors[0] : "");
}

TEST(BiphaseValidationTest, RejectsUsersCodeWithX1EqualEight) {
  Project p = MakeBasePalProject();
  Section s = MakeSection(SectionType::kLeadIn, 938);
  p.line_injections.disc_type = "CAV";
  auto inj = MakeLaserdiscInjection();
  inj.codes.push_back(MakeCode("lead_in"));
  auto code = MakeCode("users_code");
  code.users_code = "0x88D234";  // X1=8 (invalid), D=0xD (canonical)
  code.users_code_specified = true;
  inj.codes.push_back(code);
  s.line_injections.push_back(inj);
  p.sections.push_back(s);

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_FALSE(r.is_valid);
  ASSERT_FALSE(r.errors.empty());
  EXPECT_NE(r.errors[0].find("X1"), std::string::npos);
}

TEST(BiphaseValidationTest, RejectsUsersCodeWithX1EqualFifteen) {
  Project p = MakeBasePalProject();
  Section s = MakeSection(SectionType::kLeadIn, 938);
  p.line_injections.disc_type = "CAV";
  auto inj = MakeLaserdiscInjection();
  inj.codes.push_back(MakeCode("lead_in"));
  auto code = MakeCode("users_code");
  code.users_code = "0x8FD234";  // X1=15 (invalid), D=0xD (canonical)
  code.users_code_specified = true;
  inj.codes.push_back(code);
  s.line_injections.push_back(inj);
  p.sections.push_back(s);

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_FALSE(r.is_valid);
}

TEST(BiphaseValidationTest, RejectsUsersCodeWithInvalidDNibble) {
  // D nibble must be 0xD (IEC §10.1.9); 0x801234 has D=1, 0x870000 has D=0.
  for (const char* bad_code : {"0x801234", "0x870000", "0x80A000"}) {
    Project p = MakeBasePalProject();
    Section s = MakeSection(SectionType::kLeadIn, 938);
    p.line_injections.disc_type = "CAV";
    auto inj = MakeLaserdiscInjection();
    inj.codes.push_back(MakeCode("lead_in"));
    auto code = MakeCode("users_code");
    code.users_code = bad_code;
    code.users_code_specified = true;
    inj.codes.push_back(code);
    s.line_injections.push_back(inj);
    p.sections.push_back(s);

    ProjectValidator v;
    const auto r = v.Validate(p);
    EXPECT_FALSE(r.is_valid)
        << "Expected rejection for D-nibble violation: " << bad_code;
    ASSERT_FALSE(r.errors.empty()) << bad_code;
    EXPECT_NE(r.errors[0].find("D nibble"), std::string::npos) << bad_code;
  }
}

TEST(BiphaseValidationTest, RejectsUsersCodeWithNonHexString) {
  Project p = MakeBasePalProject();
  Section s = MakeSection(SectionType::kLeadIn, 938);
  p.line_injections.disc_type = "CAV";
  auto inj = MakeLaserdiscInjection();
  inj.codes.push_back(MakeCode("lead_in"));
  auto code = MakeCode("users_code");
  code.users_code = "not_a_hex";
  code.users_code_specified = true;
  inj.codes.push_back(code);
  s.line_injections.push_back(inj);
  p.sections.push_back(s);

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_FALSE(r.is_valid);
}

TEST(BiphaseValidationTest, RejectsUsersCodeExceeding24BitRange) {
  Project p = MakeBasePalProject();
  Section s = MakeSection(SectionType::kLeadIn, 938);
  p.line_injections.disc_type = "CAV";
  auto inj = MakeLaserdiscInjection();
  inj.codes.push_back(MakeCode("lead_in"));
  auto code = MakeCode("users_code");
  code.users_code = "0x1000000";  // > 0xFFFFFF
  code.users_code_specified = true;
  inj.codes.push_back(code);
  s.line_injections.push_back(inj);
  p.sections.push_back(s);

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_FALSE(r.is_valid);
}

// ─── Task 5.9/5.10: minimum section duration (CAV)
// ────────────────────────────

TEST(BiphaseValidationTest, AcceptsLeadInWithExactMinimumFrames) {
  Project p = MakeBasePalProject();
  Section s = MakeSection(SectionType::kLeadIn, 938);
  p.line_injections.disc_type = "CAV";
  auto inj = MakeLaserdiscInjection();
  inj.codes.push_back(MakeCode("lead_in"));
  s.line_injections.push_back(inj);
  p.sections.push_back(s);

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_TRUE(r.is_valid) << (!r.errors.empty() ? r.errors[0] : "");
  EXPECT_TRUE(r.warnings.empty());
}

TEST(BiphaseValidationTest, WarnsLeadInBelowMinimumFrames) {
  Project p = MakeBasePalProject();
  Section s = MakeSection(SectionType::kLeadIn, 937);  // one below IEC minimum
  p.line_injections.disc_type = "CAV";
  auto inj = MakeLaserdiscInjection();
  inj.codes.push_back(MakeCode("lead_in"));
  s.line_injections.push_back(inj);
  p.sections.push_back(s);

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_TRUE(r.is_valid);
  ASSERT_FALSE(r.warnings.empty());
  EXPECT_NE(r.warnings[0].find("lead_in"), std::string::npos);
}

TEST(BiphaseValidationTest, AcceptsLeadOutWithExactMinimumFrames) {
  Project p = MakeBasePalProject();
  Section s = MakeSection(SectionType::kLeadOut, 1250);
  p.line_injections.disc_type = "CAV";
  auto inj = MakeLaserdiscInjection();
  inj.codes.push_back(MakeCode("lead_out"));
  s.line_injections.push_back(inj);
  p.sections.push_back(s);

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_TRUE(r.is_valid) << (!r.errors.empty() ? r.errors[0] : "");
  EXPECT_TRUE(r.warnings.empty());
}

TEST(BiphaseValidationTest, WarnsLeadOutBelowMinimumFrames) {
  Project p = MakeBasePalProject();
  Section s =
      MakeSection(SectionType::kLeadOut, 1249);  // one below IEC minimum
  p.line_injections.disc_type = "CAV";
  auto inj = MakeLaserdiscInjection();
  inj.codes.push_back(MakeCode("lead_out"));
  s.line_injections.push_back(inj);
  p.sections.push_back(s);

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_TRUE(r.is_valid);
  ASSERT_FALSE(r.warnings.empty());
  EXPECT_NE(r.warnings[0].find("lead_out"), std::string::npos);
}

TEST(BiphaseValidationTest, AcceptsLeadInWithDurationAll) {
  // "all" semantics skip the frame-count check.
  Project p = MakeBasePalProject();
  Section s = MakeSection(SectionType::kLeadIn, 0);
  s.source = "fixture.mkv";
  s.duration_frames_all = true;
  p.line_injections.disc_type = "CAV";
  auto inj = MakeLaserdiscInjection();
  inj.codes.push_back(MakeCode("lead_in"));
  s.line_injections.push_back(inj);
  p.sections.push_back(s);

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_TRUE(r.is_valid) << (!r.errors.empty() ? r.errors[0] : "");
}

TEST(BiphaseValidationTest, SkipsMinDurationCheckForClvLeadIn) {
  // CLV track density varies; no minimum frame count is enforced for CLV.
  Project p = MakeBasePalProject();
  Section s = MakeSection(SectionType::kLeadIn, 10);  // tiny, accepted for CLV
  p.line_injections.disc_type = "CLV";
  auto inj = MakeLaserdiscInjection();
  inj.codes.push_back(MakeCode("lead_in"));
  s.line_injections.push_back(inj);
  p.sections.push_back(s);

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_TRUE(r.is_valid) << (!r.errors.empty() ? r.errors[0] : "");
}

TEST(BiphaseValidationTest, SkipsMinDurationCheckForClvLeadOut) {
  Project p = MakeBasePalProject();
  Section s = MakeSection(SectionType::kLeadOut, 10);
  p.line_injections.disc_type = "CLV";
  auto inj = MakeLaserdiscInjection();
  inj.codes.push_back(MakeCode("lead_out"));
  s.line_injections.push_back(inj);
  p.sections.push_back(s);

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_TRUE(r.is_valid) << (!r.errors.empty() ? r.errors[0] : "");
}

TEST(BiphaseValidationTest, ProgrammeAreaHasNoDurationConstraint) {
  // Programme area sections have no IEC minimum frame count.
  Project p = MakeBasePalProject();
  Section s = MakeSection(SectionType::kProgrammeArea, 1);
  p.line_injections.disc_type = "CAV";
  auto inj = MakeLaserdiscInjection();
  inj.codes.push_back(MakeCode("picture_number"));
  s.line_injections.push_back(inj);
  p.sections.push_back(s);

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_TRUE(r.is_valid) << (!r.errors.empty() ? r.errors[0] : "");
}

TEST(BiphaseValidationTest, RejectsLaserdiscSectionWhenDiscTypeIsAbsent) {
  // A section declaring laserdisc codes now requires a project-wide disc_type.
  Project p = MakeBasePalProject();
  Section s = MakeSection(SectionType::kProgrammeArea);
  Section::LineInjection inj;
  inj.type = "laserdisc";  // Project disc_type deliberately left unset.
  s.line_injections.push_back(inj);
  p.sections.push_back(s);

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_FALSE(r.is_valid);
  ASSERT_FALSE(r.errors.empty());
  EXPECT_NE(r.errors[0].find("disc_type"), std::string::npos);
}

}  // namespace
}  // namespace videosynth
