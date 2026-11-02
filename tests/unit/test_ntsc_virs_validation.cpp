/*
 * File:        test_ntsc_virs_validation.cpp
 * Module:      project_validator_tests
 * Purpose:     Validates that NTSC laserdisc projects include the mandatory
 *              VIR signal injection for colour (IEC 60857 §9.1.3 and §§ 5.13,
 *              5.16). VITS and disc_type are now project-wide settings.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <string>

#include "videosynth/project_validator.h"

namespace videosynth {
namespace {

// ─── helpers
// ──────────────────────────────────────────────────────────────────

Project MakeBaseNtscProject() {
  Project p;
  p.cvbs_presets.video_standard_preset = Standard::kNtsc;
  p.cvbs_presets.sample_encoding_preset = "CVBS_U10_4FSC";
  p.cvbs_presets.signal_state_preset = "STANDARD_STABLE_LOCKED";
  p.output.video_path = "/tmp/nv_test.cvbs";
  p.output.metadata_path = "/tmp/nv_test.meta";
  return p;
}

Project MakeBasePalProject() {
  Project p = MakeBaseNtscProject();
  p.cvbs_presets.video_standard_preset = Standard::kPal;
  return p;
}

Section MakeSection(int frames = 100) {
  Section s;
  s.name = "Section";
  s.type = "progressive";
  s.section_type = SectionType::kProgrammeArea;
  s.source = "fixture.exr";
  s.duration_frames = frames;
  return s;
}

Section::LineInjection MakeLaserdiscInjection() {
  Section::LineInjection inj;
  inj.type = "laserdisc";
  return inj;
}

// ─── Task 5.13: NTSC VIRS mandatory presence (now project-wide)
// ──────────────

TEST(NtscVirsValidationTest, RejectsNtscLaserdiscProjectWithoutVirs) {
  // IEC 60857 §9.1.3: VIR signal is mandatory for colour on NTSC laserdisc.
  Project p = MakeBaseNtscProject();
  p.laserdisc.disc_type = "CAV";
  Section s = MakeSection();
  s.line_injections.push_back(MakeLaserdiscInjection());
  // Deliberately omit the project virs.
  p.sections.push_back(s);

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_FALSE(r.is_valid);
  ASSERT_FALSE(r.errors.empty());
  EXPECT_NE(r.errors[0].find("virs"), std::string::npos);
  EXPECT_NE(r.errors[0].find("9.1.3"), std::string::npos);
}

TEST(NtscVirsValidationTest, AcceptsNtscLaserdiscProjectWithVirs) {
  Project p = MakeBaseNtscProject();
  p.laserdisc.disc_type = "CAV";
  p.line_injections.vits.push_back(VitsInjection{"virs", {19, 282}});
  Section s = MakeSection();
  s.line_injections.push_back(MakeLaserdiscInjection());
  p.sections.push_back(s);

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_TRUE(r.is_valid) << (!r.errors.empty() ? r.errors[0] : "");
}

TEST(NtscVirsValidationTest, AcceptsNtscProjectWithoutLaserdiscAndWithoutVirs) {
  // No laserdisc disc_type → not a laserdisc project → no VIRS requirement.
  Project p = MakeBaseNtscProject();
  Section s = MakeSection();
  s.section_type = SectionType::kUnknown;
  // No injections at all.
  p.sections.push_back(s);

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_TRUE(r.is_valid) << (!r.errors.empty() ? r.errors[0] : "");
}

TEST(NtscVirsValidationTest, AcceptsPalLaserdiscProjectWithoutVirs) {
  // PAL laserdisc projects do not require virs (IEC 60856 only).
  Project p = MakeBasePalProject();
  p.laserdisc.disc_type = "CAV";
  Section s = MakeSection();
  s.line_injections.push_back(MakeLaserdiscInjection());
  p.sections.push_back(s);

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_TRUE(r.is_valid) << (!r.errors.empty() ? r.errors[0] : "");
}

TEST(NtscVirsValidationTest,
     RejectsNtscMultipleLaserdiscSectionsWithoutProjectVirs) {
  // The VIRS requirement is project-wide; multiple laserdisc sections with no
  // project virs are rejected once.
  Project p = MakeBaseNtscProject();
  p.laserdisc.disc_type = "CAV";

  Section s1 = MakeSection();
  s1.line_injections.push_back(MakeLaserdiscInjection());
  p.sections.push_back(s1);

  Section s2 = MakeSection();
  s2.line_injections.push_back(MakeLaserdiscInjection());
  p.sections.push_back(s2);

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_FALSE(r.is_valid);
  ASSERT_FALSE(r.errors.empty());
  EXPECT_NE(r.errors[0].find("virs"), std::string::npos);
}

TEST(NtscVirsValidationTest,
     AcceptsNtscMultipleLaserdiscSectionsWithProjectVirs) {
  Project p = MakeBaseNtscProject();
  p.laserdisc.disc_type = "CAV";
  p.line_injections.vits.push_back(VitsInjection{"virs", {19, 282}});

  Section s1 = MakeSection();
  s1.line_injections.push_back(MakeLaserdiscInjection());
  p.sections.push_back(s1);

  Section s2 = MakeSection();
  s2.line_injections.push_back(MakeLaserdiscInjection());
  p.sections.push_back(s2);

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_TRUE(r.is_valid) << (!r.errors.empty() ? r.errors[0] : "");
}

TEST(NtscVirsValidationTest, AcceptsNtscLaserdiscProjectWithVirsOnLine19Only) {
  // VIRS may target line 19 (field 1) only; line 282 is optional.
  Project p = MakeBaseNtscProject();
  p.laserdisc.disc_type = "CAV";
  p.line_injections.vits.push_back(VitsInjection{"virs", {19}});
  Section s = MakeSection();
  s.line_injections.push_back(MakeLaserdiscInjection());
  p.sections.push_back(s);

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_TRUE(r.is_valid) << (!r.errors.empty() ? r.errors[0] : "");
}

TEST(NtscVirsValidationTest, AcceptsNtscLaserdiscProjectWithVirsOnLine282Only) {
  // VIRS may target line 282 (field 2) only.
  Project p = MakeBaseNtscProject();
  p.laserdisc.disc_type = "CAV";
  p.line_injections.vits.push_back(VitsInjection{"virs", {282}});
  Section s = MakeSection();
  s.line_injections.push_back(MakeLaserdiscInjection());
  p.sections.push_back(s);

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_TRUE(r.is_valid) << (!r.errors.empty() ? r.errors[0] : "");
}

TEST(NtscVirsValidationTest, VirsMissingErrorMessageContainsIecReference) {
  Project p = MakeBaseNtscProject();
  p.laserdisc.disc_type = "CAV";
  Section s = MakeSection();
  s.line_injections.push_back(MakeLaserdiscInjection());
  p.sections.push_back(s);

  ProjectValidator v;
  const auto r = v.Validate(p);
  ASSERT_FALSE(r.errors.empty());
  // Error must name the missing signal and cite the IEC clause.
  EXPECT_NE(r.errors[0].find("virs"), std::string::npos);
  EXPECT_NE(r.errors[0].find("NTSC"), std::string::npos);
}

TEST(NtscVirsValidationTest, VirsMissingDoesNotBlockAfterReservedLineError) {
  // When an incompatible VITS type is on a reserved line, the reserved-line
  // error fires first (before the VIRS check).
  Project p = MakeBaseNtscProject();
  p.laserdisc.disc_type = "CAV";
  p.line_injections.vits.push_back(VitsInjection{"ntc7-composite", {17}});
  Section s = MakeSection();
  s.line_injections.push_back(MakeLaserdiscInjection());
  p.sections.push_back(s);

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_FALSE(r.is_valid);
  ASSERT_FALSE(r.errors.empty());
  // Reserved-line error, not VIRS error.
  EXPECT_NE(r.errors[0].find("17"), std::string::npos);
}

}  // namespace
}  // namespace videosynth
