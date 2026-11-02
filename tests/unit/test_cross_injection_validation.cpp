/*
 * File:        test_cross_injection_validation.cpp
 * Module:      project_validator_tests
 * Purpose:     Validates cross-injection line conflict rules — project VITS on
 *              laserdisc reserved ranges, and rejection of unsupported
 *              section-level injection types (IEC 60856/60857 §§ 5.11, 5.12).
 *              VITS and disc_type are now project-wide settings.
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

Project MakeBasePalProject() {
  Project p;
  p.cvbs_presets.video_standard_preset = Standard::kPal;
  p.cvbs_presets.sample_encoding_preset = "CVBS_U10_4FSC";
  p.cvbs_presets.signal_state_preset = "STANDARD_STABLE_LOCKED";
  p.output.video_path = "/tmp/ci_test.cvbs";
  p.output.metadata_path = "/tmp/ci_test.meta";
  return p;
}

Project MakeBaseNtscProject() {
  Project p = MakeBasePalProject();
  p.cvbs_presets.video_standard_preset = Standard::kNtsc;
  return p;
}

Section MakePalSection() {
  Section s;
  s.name = "Section";
  s.type = "progressive";
  s.section_type = SectionType::kProgrammeArea;
  s.source = "fixture.exr";
  s.duration_frames = 100;
  return s;
}

Section::LineInjection MakeLaserdiscInjection() {
  Section::LineInjection inj;
  inj.type = "laserdisc";
  return inj;
}

// ─── Task 5.11: PAL incompatible project VITS types on reserved lines ────────

TEST(CrossInjectionValidationTest, RejectsPalVits17WhenLaserdiscIsActive) {
  // vits17 targets line 17 which is in the PAL reserved range 6–18.
  Project p = MakeBasePalProject();
  p.laserdisc.disc_type = "CAV";
  p.line_injections.vits.push_back(VitsInjection{"vits17", {17}});
  Section s = MakePalSection();
  s.line_injections.push_back(MakeLaserdiscInjection());
  p.sections.push_back(s);

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_FALSE(r.is_valid);
  ASSERT_FALSE(r.errors.empty());
  EXPECT_NE(r.errors[0].find("17"), std::string::npos);
}

TEST(CrossInjectionValidationTest,
     RejectsPalItuMultiburstWhenLaserdiscIsActive) {
  // itu-multiburst targets line 18, inside PAL reserved range 6–18.
  Project p = MakeBasePalProject();
  p.laserdisc.disc_type = "CAV";
  p.line_injections.vits.push_back(VitsInjection{"itu-multiburst", {18}});
  Section s = MakePalSection();
  s.line_injections.push_back(MakeLaserdiscInjection());
  p.sections.push_back(s);

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_FALSE(r.is_valid);
  ASSERT_FALSE(r.errors.empty());
  EXPECT_NE(r.errors[0].find("18"), std::string::npos);
}

TEST(CrossInjectionValidationTest,
     RejectsPalItuCompositeWhenLaserdiscIsActive) {
  // itu-composite targets line 330, inside PAL reserved range 319–331.
  Project p = MakeBasePalProject();
  p.laserdisc.disc_type = "CAV";
  p.line_injections.vits.push_back(VitsInjection{"itu-composite", {330}});
  Section s = MakePalSection();
  s.line_injections.push_back(MakeLaserdiscInjection());
  p.sections.push_back(s);

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_FALSE(r.is_valid);
  ASSERT_FALSE(r.errors.empty());
  EXPECT_NE(r.errors[0].find("330"), std::string::npos);
}

TEST(CrossInjectionValidationTest,
     RejectsPalItuCombinationWhenLaserdiscIsActive) {
  // itu-combination targets line 331, inside PAL reserved range 319–331.
  Project p = MakeBasePalProject();
  p.laserdisc.disc_type = "CAV";
  p.line_injections.vits.push_back(VitsInjection{"itu-combination", {331}});
  Section s = MakePalSection();
  s.line_injections.push_back(MakeLaserdiscInjection());
  p.sections.push_back(s);

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_FALSE(r.is_valid);
  ASSERT_FALSE(r.errors.empty());
  EXPECT_NE(r.errors[0].find("331"), std::string::npos);
}

// ─── Task 5.11: NTSC incompatible project VITS types on reserved lines ───────

TEST(CrossInjectionValidationTest,
     RejectsNtscNtc7CompositeWhenLaserdiscIsActive) {
  // ntc7-composite targets line 17, inside NTSC reserved range 10–18.
  Project p = MakeBaseNtscProject();
  p.laserdisc.disc_type = "CAV";
  p.line_injections.vits.push_back(VitsInjection{"ntc7-composite", {17}});
  Section s = MakePalSection();
  s.line_injections.push_back(MakeLaserdiscInjection());
  p.sections.push_back(s);

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_FALSE(r.is_valid);
  ASSERT_FALSE(r.errors.empty());
  EXPECT_NE(r.errors[0].find("17"), std::string::npos);
}

TEST(CrossInjectionValidationTest,
     RejectsNtscFccMultiburstWhenLaserdiscIsActive) {
  // fcc-multiburst targets line 18, inside NTSC reserved range 10–18.
  Project p = MakeBaseNtscProject();
  p.laserdisc.disc_type = "CAV";
  p.line_injections.vits.push_back(VitsInjection{"fcc-multiburst", {18}});
  Section s = MakePalSection();
  s.line_injections.push_back(MakeLaserdiscInjection());
  p.sections.push_back(s);

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_FALSE(r.is_valid);
  ASSERT_FALSE(r.errors.empty());
  EXPECT_NE(r.errors[0].find("18"), std::string::npos);
}

TEST(CrossInjectionValidationTest,
     RejectsNtscNtc7CombinationWhenLaserdiscIsActive) {
  // ntc7-combination targets line 280, inside NTSC reserved range 273–281.
  Project p = MakeBaseNtscProject();
  p.laserdisc.disc_type = "CAV";
  p.line_injections.vits.push_back(VitsInjection{"ntc7-combination", {280}});
  Section s = MakePalSection();
  s.line_injections.push_back(MakeLaserdiscInjection());
  p.sections.push_back(s);

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_FALSE(r.is_valid);
  ASSERT_FALSE(r.errors.empty());
  EXPECT_NE(r.errors[0].find("280"), std::string::npos);
}

TEST(CrossInjectionValidationTest,
     RejectsNtscFccCompositeWhenLaserdiscIsActive) {
  // fcc-composite targets line 281, inside NTSC reserved range 273–281.
  Project p = MakeBaseNtscProject();
  p.laserdisc.disc_type = "CAV";
  p.line_injections.vits.push_back(VitsInjection{"fcc-composite", {281}});
  Section s = MakePalSection();
  s.line_injections.push_back(MakeLaserdiscInjection());
  p.sections.push_back(s);

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_FALSE(r.is_valid);
  ASSERT_FALSE(r.errors.empty());
  EXPECT_NE(r.errors[0].find("281"), std::string::npos);
}

// ─── Compatible project VITS types (outside reserved ranges) ─────────────────

TEST(CrossInjectionValidationTest,
     AcceptsPalUkNationalOnLine19WhenLaserdiscIsActive) {
  // uk-national targets line 19, outside the PAL reserved range 6–18 / 319–331.
  Project p = MakeBasePalProject();
  p.laserdisc.disc_type = "CAV";
  p.line_injections.vits.push_back(VitsInjection{"uk-national", {19}});
  Section s = MakePalSection();
  s.line_injections.push_back(MakeLaserdiscInjection());
  p.sections.push_back(s);

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_TRUE(r.is_valid) << (!r.errors.empty() ? r.errors[0] : "");
}

TEST(CrossInjectionValidationTest,
     AcceptsPalVits20OnLine20WhenLaserdiscIsActive) {
  // vits20 targets line 20, outside the PAL reserved range.
  Project p = MakeBasePalProject();
  p.laserdisc.disc_type = "CAV";
  p.line_injections.vits.push_back(VitsInjection{"vits20", {20}});
  Section s = MakePalSection();
  s.line_injections.push_back(MakeLaserdiscInjection());
  p.sections.push_back(s);

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_TRUE(r.is_valid) << (!r.errors.empty() ? r.errors[0] : "");
}

TEST(CrossInjectionValidationTest,
     AcceptsNtscVirsOnLines19And282WhenLaserdiscIsActive) {
  // virs targets lines 19 and 282 — both outside NTSC reserved ranges.
  Project p = MakeBaseNtscProject();
  p.laserdisc.disc_type = "CAV";
  p.line_injections.vits.push_back(VitsInjection{"virs", {19, 282}});
  Section s = MakePalSection();
  s.line_injections.push_back(MakeLaserdiscInjection());
  p.sections.push_back(s);

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_TRUE(r.is_valid) << (!r.errors.empty() ? r.errors[0] : "");
}

// ─── Task 5.12: unsupported section-level injection types are rejected ───────
// line_content is not an implemented section injection type; the section
// validator rejects it regardless of the line it targets.

TEST(CrossInjectionValidationTest, RejectsPalLineContentSectionInjection) {
  Project p = MakeBasePalProject();
  p.laserdisc.disc_type = "CAV";
  Section s = MakePalSection();
  s.line_injections.push_back(MakeLaserdiscInjection());

  Section::LineInjection lc;
  lc.type = "line_content";
  lc.target_lines = {6};
  s.line_injections.push_back(lc);
  p.sections.push_back(s);

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_FALSE(r.is_valid);
  ASSERT_FALSE(r.errors.empty());
  EXPECT_NE(r.errors[0].find("line_content"), std::string::npos);
}

TEST(CrossInjectionValidationTest, RejectsNtscLineContentSectionInjection) {
  Project p = MakeBaseNtscProject();
  p.laserdisc.disc_type = "CAV";
  p.line_injections.vits.push_back(VitsInjection{"virs", {19, 282}});
  Section s = MakePalSection();
  s.line_injections.push_back(MakeLaserdiscInjection());

  Section::LineInjection lc;
  lc.type = "line_content";
  lc.target_lines = {10};
  s.line_injections.push_back(lc);
  p.sections.push_back(s);

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_FALSE(r.is_valid);
  ASSERT_FALSE(r.errors.empty());
  EXPECT_NE(r.errors[0].find("line_content"), std::string::npos);
}

TEST(CrossInjectionValidationTest, RejectsSectionVitsInjection) {
  // A section-level VITS injection is rejected: VITS are configured
  // project-wide now.
  Project p = MakeBasePalProject();
  Section s = MakePalSection();
  s.section_type = SectionType::kUnknown;
  Section::LineInjection vits;
  vits.type = "vits";
  vits.target_lines = {17};
  s.line_injections.push_back(vits);
  p.sections.push_back(s);

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_FALSE(r.is_valid);
  ASSERT_FALSE(r.errors.empty());
  EXPECT_NE(r.errors[0].find("VITS"), std::string::npos);
}

// ─── No-laserdisc baseline
// ────────────────────────────────────────────────────

TEST(CrossInjectionValidationTest,
     AllowsProjectVitsOutsideReservedRangesWithoutLaserdisc) {
  // Without a laserdisc disc_type, reserved-range checks don't apply.
  Project p = MakeBasePalProject();
  p.line_injections.vits.push_back(VitsInjection{"vits17", {17}});
  p.line_injections.vits.push_back(VitsInjection{"itu-multiburst", {18}});
  Section s = MakePalSection();
  s.section_type = SectionType::kUnknown;
  p.sections.push_back(s);

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_TRUE(r.is_valid) << (!r.errors.empty() ? r.errors[0] : "");
}

}  // namespace
}  // namespace videosynth
