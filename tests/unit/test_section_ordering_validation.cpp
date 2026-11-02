/*
 * File:        test_section_ordering_validation.cpp
 * Module:      project_validator_tests
 * Purpose:     Validates disc-structure section ordering: lead_in first,
 *              lead_out last, and only programme_area sections between them.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <string>

#include "videosynth/project_validator.h"

namespace videosynth {
namespace {

// ─── helpers ─────────────────────────────────────────────────────────────────

Project MakeBasePalProject() {
  Project p;
  p.cvbs_presets.video_standard_preset = Standard::kPal;
  p.cvbs_presets.sample_encoding_preset = "CVBS_U10_4FSC";
  p.cvbs_presets.signal_state_preset = "STANDARD_STABLE_LOCKED";
  p.output.video_path = "/tmp/so_test.cvbs";
  p.output.metadata_path = "/tmp/so_test.meta";
  p.laserdisc.enabled = true;
  return p;
}

Section MakeSection(const std::string& name, SectionType stype,
                    int frames = 100) {
  Section s;
  s.name = name;
  s.type = "progressive";
  s.section_type = stype;
  s.source = "fixture.exr";
  s.duration_frames = frames;
  return s;
}

bool HasErrorContaining(const ValidationResult& result,
                        const std::string& fragment) {
  for (const std::string& error : result.errors) {
    if (error.find(fragment) != std::string::npos) {
      return true;
    }
  }
  return false;
}

// ─── accepted orderings ──────────────────────────────────────────────────────

TEST(SectionOrderingValidationTest, AcceptsLeadInProgrammeAreaLeadOut) {
  Project p = MakeBasePalProject();
  p.sections.push_back(MakeSection("intro", SectionType::kLeadIn, 938));
  p.sections.push_back(MakeSection("main", SectionType::kProgrammeArea));
  p.sections.push_back(MakeSection("outro", SectionType::kLeadOut, 1250));

  ProjectValidator v;
  const ValidationResult r = v.Validate(p);
  EXPECT_TRUE(r.is_valid);
}

TEST(SectionOrderingValidationTest, AcceptsMultipleProgrammeAreaSections) {
  Project p = MakeBasePalProject();
  p.sections.push_back(MakeSection("intro", SectionType::kLeadIn, 938));
  p.sections.push_back(MakeSection("part1", SectionType::kProgrammeArea));
  p.sections.push_back(MakeSection("part2", SectionType::kProgrammeArea));
  p.sections.push_back(MakeSection("outro", SectionType::kLeadOut, 1250));

  ProjectValidator v;
  const ValidationResult r = v.Validate(p);
  EXPECT_TRUE(r.is_valid);
}

TEST(SectionOrderingValidationTest, AcceptsUntypedSectionsWithoutDiscBounds) {
  // No lead_in/lead_out declared: untyped and programme_area sections may mix.
  Project p = MakeBasePalProject();
  p.sections.push_back(MakeSection("plain", SectionType::kUnknown));
  p.sections.push_back(MakeSection("main", SectionType::kProgrammeArea));
  p.sections.push_back(MakeSection("tail", SectionType::kUnknown));

  ProjectValidator v;
  const ValidationResult r = v.Validate(p);
  EXPECT_TRUE(r.is_valid);
}

TEST(SectionOrderingValidationTest, AcceptsLeadInWithoutLeadOut) {
  Project p = MakeBasePalProject();
  p.sections.push_back(MakeSection("intro", SectionType::kLeadIn, 938));
  p.sections.push_back(MakeSection("main", SectionType::kProgrammeArea));

  ProjectValidator v;
  const ValidationResult r = v.Validate(p);
  EXPECT_TRUE(r.is_valid);
}

// ─── rejected orderings ──────────────────────────────────────────────────────

TEST(SectionOrderingValidationTest, RejectsSectionBeforeLeadIn) {
  Project p = MakeBasePalProject();
  p.sections.push_back(MakeSection("early", SectionType::kProgrammeArea));
  p.sections.push_back(MakeSection("intro", SectionType::kLeadIn, 938));
  p.sections.push_back(MakeSection("outro", SectionType::kLeadOut, 1250));

  ProjectValidator v;
  const ValidationResult r = v.Validate(p);
  EXPECT_FALSE(r.is_valid);
  EXPECT_TRUE(HasErrorContaining(
      r, "'early' must not appear before the lead_in section 'intro'"));
}

TEST(SectionOrderingValidationTest, RejectsSectionAfterLeadOut) {
  Project p = MakeBasePalProject();
  p.sections.push_back(MakeSection("intro", SectionType::kLeadIn, 938));
  p.sections.push_back(MakeSection("main", SectionType::kProgrammeArea));
  p.sections.push_back(MakeSection("outro", SectionType::kLeadOut, 1250));
  p.sections.push_back(MakeSection("late", SectionType::kProgrammeArea));

  ProjectValidator v;
  const ValidationResult r = v.Validate(p);
  EXPECT_FALSE(r.is_valid);
  EXPECT_TRUE(HasErrorContaining(
      r, "'late' must not appear after the lead_out section 'outro'"));
}

TEST(SectionOrderingValidationTest,
     RejectsUntypedSectionBetweenLeadInAndLeadOut) {
  Project p = MakeBasePalProject();
  p.sections.push_back(MakeSection("intro", SectionType::kLeadIn, 938));
  p.sections.push_back(MakeSection("plain", SectionType::kUnknown));
  p.sections.push_back(MakeSection("outro", SectionType::kLeadOut, 1250));

  ProjectValidator v;
  const ValidationResult r = v.Validate(p);
  EXPECT_FALSE(r.is_valid);
  EXPECT_TRUE(HasErrorContaining(
      r,
      "'plain' must have laserdisc.area 'programme_area' because it is between "
      "the lead_in and lead_out sections"));
}

TEST(SectionOrderingValidationTest, RejectsUntypedSectionAfterLoneLeadIn) {
  Project p = MakeBasePalProject();
  p.sections.push_back(MakeSection("intro", SectionType::kLeadIn, 938));
  p.sections.push_back(MakeSection("plain", SectionType::kUnknown));

  ProjectValidator v;
  const ValidationResult r = v.Validate(p);
  EXPECT_FALSE(r.is_valid);
  EXPECT_TRUE(HasErrorContaining(
      r,
      "'plain' must have laserdisc.area 'programme_area' because it is after "
      "the lead_in section"));
}

TEST(SectionOrderingValidationTest, RejectsUntypedSectionBeforeLoneLeadOut) {
  Project p = MakeBasePalProject();
  p.sections.push_back(MakeSection("plain", SectionType::kUnknown));
  p.sections.push_back(MakeSection("outro", SectionType::kLeadOut, 1250));

  ProjectValidator v;
  const ValidationResult r = v.Validate(p);
  EXPECT_FALSE(r.is_valid);
  EXPECT_TRUE(HasErrorContaining(
      r,
      "'plain' must have laserdisc.area 'programme_area' because it is before "
      "the lead_out section"));
}

TEST(SectionOrderingValidationTest, RejectsMultipleLeadInSections) {
  Project p = MakeBasePalProject();
  p.sections.push_back(MakeSection("intro1", SectionType::kLeadIn, 938));
  p.sections.push_back(MakeSection("intro2", SectionType::kLeadIn, 938));
  p.sections.push_back(MakeSection("main", SectionType::kProgrammeArea));

  ProjectValidator v;
  const ValidationResult r = v.Validate(p);
  EXPECT_FALSE(r.is_valid);
  EXPECT_TRUE(
      HasErrorContaining(r, "only one lead_in section is allowed; found 2"));
}

TEST(SectionOrderingValidationTest, RejectsMultipleLeadOutSections) {
  Project p = MakeBasePalProject();
  p.sections.push_back(MakeSection("main", SectionType::kProgrammeArea));
  p.sections.push_back(MakeSection("outro1", SectionType::kLeadOut, 1250));
  p.sections.push_back(MakeSection("outro2", SectionType::kLeadOut, 1250));

  ProjectValidator v;
  const ValidationResult r = v.Validate(p);
  EXPECT_FALSE(r.is_valid);
  EXPECT_TRUE(
      HasErrorContaining(r, "only one lead_out section is allowed; found 2"));
}

TEST(SectionOrderingValidationTest, RejectsLeadOutBeforeLeadIn) {
  Project p = MakeBasePalProject();
  p.sections.push_back(MakeSection("outro", SectionType::kLeadOut, 1250));
  p.sections.push_back(MakeSection("intro", SectionType::kLeadIn, 938));

  ProjectValidator v;
  const ValidationResult r = v.Validate(p);
  EXPECT_FALSE(r.is_valid);
  EXPECT_TRUE(HasErrorContaining(
      r, "'outro' must not appear before the lead_in section 'intro'"));
  EXPECT_TRUE(HasErrorContaining(
      r, "'intro' must not appear after the lead_out section 'outro'"));
}

}  // namespace
}  // namespace videosynth
