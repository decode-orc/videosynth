/*
 * File:        test_biphase_integration.cpp
 * Module:      project_validator_tests
 * Purpose:     Integration-level validation tests for complete multi-section
 *              laserdisc projects covering lead-in, programme area, and
 *              lead-out for PAL/NTSC CAV/CLV configurations (IEC 60856/60857,
 *              task 5.8).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "videosynth/project_validator.h"

namespace videosynth {
namespace {

// ─── helpers
// ──────────────────────────────────────────────────────────────────

Project MakeBaseProject(Standard standard) {
  Project p;
  p.cvbs_presets.video_standard_preset = standard;
  p.cvbs_presets.sample_encoding_preset = "CVBS_U10_4FSC";
  p.cvbs_presets.signal_state_preset = "STANDARD_STABLE_LOCKED";
  p.output.video_path = "/tmp/bi_test.cvbs";
  p.output.metadata_path = "/tmp/bi_test.meta";
  return p;
}

Section MakeSection(const std::string& name, SectionType stype, int frames) {
  Section s;
  s.name = name;
  s.type = "progressive";
  s.section_type = stype;
  s.source = "fixture.exr";
  s.duration_frames = frames;
  return s;
}

Section::LineInjection MakeLaserdiscInjection(const std::string& disc_type) {
  // disc_type is now a project-level decision (Project::line_injections.
  // disc_type); the section injection only carries codes. The argument is
  // retained so call sites still document the intended disc format.
  (void)disc_type;
  Section::LineInjection inj;
  inj.type = "laserdisc";
  return inj;
}

// VITS injections are now project-wide. This returns a VitsInjection to be
// pushed into Project::line_injections.vits (not a section injection).
VitsInjection MakeVirsInjection() {
  VitsInjection inj;
  inj.vits_type = "virs";
  inj.target_lines = {19, 282};
  return inj;
}

Section::LineInjectionCode MakeCode(const std::string& code_type) {
  Section::LineInjectionCode c;
  c.code_type = code_type;
  return c;
}

Section::LineInjectionCode MakePictureNumberCode(int start_value) {
  Section::LineInjectionCode c;
  c.code_type = "picture_number";
  c.start_value = start_value;
  c.start_value_specified = true;
  return c;
}

Section::LineInjectionCode MakeChapterCode(int chapter) {
  Section::LineInjectionCode c;
  c.code_type = "chapter_number";
  c.chapter = chapter;
  c.chapter_specified = true;
  return c;
}

// ─── Complete PAL CAV project
// ─────────────────────────────────────────────────

TEST(BiphaseIntegrationTest, AcceptsCompletePalCavProject) {
  Project p = MakeBaseProject(Standard::kPal);
  p.laserdisc.disc_type = "CAV";

  // Lead-in (minimum 938 frames for CAV).
  Section lead_in = MakeSection("Lead-in", SectionType::kLeadIn, 938);
  auto li_inj = MakeLaserdiscInjection("CAV");
  li_inj.codes.push_back(MakeCode("lead_in"));
  lead_in.line_injections.push_back(li_inj);
  p.sections.push_back(lead_in);

  // Programme area.
  Section prog = MakeSection("Programme", SectionType::kProgrammeArea, 500);
  auto prog_inj = MakeLaserdiscInjection("CAV");
  prog_inj.codes.push_back(MakePictureNumberCode(1));
  prog.line_injections.push_back(prog_inj);
  p.sections.push_back(prog);

  // Lead-out (minimum 1250 frames for CAV).
  Section lead_out = MakeSection("Lead-out", SectionType::kLeadOut, 1250);
  auto lo_inj = MakeLaserdiscInjection("CAV");
  lo_inj.codes.push_back(MakeCode("lead_out"));
  lead_out.line_injections.push_back(lo_inj);
  p.sections.push_back(lead_out);

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_TRUE(r.is_valid) << (!r.errors.empty() ? r.errors[0] : "");
}

// ─── Complete PAL CLV project
// ─────────────────────────────────────────────────

TEST(BiphaseIntegrationTest, AcceptsCompletePalClvProject) {
  Project p = MakeBaseProject(Standard::kPal);
  p.laserdisc.disc_type = "CLV";

  // Lead-in (CLV: no minimum frame count enforced).
  Section lead_in = MakeSection("Lead-in", SectionType::kLeadIn, 10);
  auto li_inj = MakeLaserdiscInjection("CLV");
  li_inj.codes.push_back(MakeCode("lead_in"));
  lead_in.line_injections.push_back(li_inj);
  p.sections.push_back(lead_in);

  // Programme area.
  Section prog = MakeSection("Programme", SectionType::kProgrammeArea, 1000);
  auto prog_inj = MakeLaserdiscInjection("CLV");
  prog_inj.codes.push_back(MakeCode("programme_time_code"));
  prog_inj.codes.push_back(MakeCode("clv_picture_number"));
  prog.line_injections.push_back(prog_inj);
  p.sections.push_back(prog);

  // Lead-out (CLV: no minimum frame count enforced).
  Section lead_out = MakeSection("Lead-out", SectionType::kLeadOut, 10);
  auto lo_inj = MakeLaserdiscInjection("CLV");
  lo_inj.codes.push_back(MakeCode("lead_out"));
  lead_out.line_injections.push_back(lo_inj);
  p.sections.push_back(lead_out);

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_TRUE(r.is_valid) << (!r.errors.empty() ? r.errors[0] : "");
}

// ─── Complete NTSC CAV project
// ────────────────────────────────────────────────

TEST(BiphaseIntegrationTest, AcceptsCompleteNtscCavProject) {
  Project p = MakeBaseProject(Standard::kNtsc);
  p.laserdisc.disc_type = "CAV";
  p.line_injections.vits.push_back(MakeVirsInjection());

  // Lead-in.
  Section lead_in = MakeSection("Lead-in", SectionType::kLeadIn, 938);
  auto li_inj = MakeLaserdiscInjection("CAV");
  li_inj.codes.push_back(MakeCode("lead_in"));
  lead_in.line_injections.push_back(li_inj);
  p.sections.push_back(lead_in);

  // Programme area with picture_number and fm_picture_number.
  Section prog = MakeSection("Programme", SectionType::kProgrammeArea, 300);
  auto prog_inj = MakeLaserdiscInjection("CAV");
  prog_inj.codes.push_back(MakePictureNumberCode(1));
  prog_inj.codes.push_back(MakeCode("fm_picture_number"));
  prog_inj.codes.push_back(MakeCode("fm_white_flag"));
  prog.line_injections.push_back(prog_inj);
  p.sections.push_back(prog);

  // Lead-out.
  Section lead_out = MakeSection("Lead-out", SectionType::kLeadOut, 1250);
  auto lo_inj = MakeLaserdiscInjection("CAV");
  lo_inj.codes.push_back(MakeCode("lead_out"));
  lead_out.line_injections.push_back(lo_inj);
  p.sections.push_back(lead_out);

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_TRUE(r.is_valid) << (!r.errors.empty() ? r.errors[0] : "");
}

// ─── Complete NTSC CLV project
// ────────────────────────────────────────────────

TEST(BiphaseIntegrationTest, AcceptsCompleteNtscClvProject) {
  Project p = MakeBaseProject(Standard::kNtsc);
  p.laserdisc.disc_type = "CLV";
  p.line_injections.vits.push_back(MakeVirsInjection());

  // Lead-in.
  Section lead_in = MakeSection("Lead-in", SectionType::kLeadIn, 10);
  auto li_inj = MakeLaserdiscInjection("CLV");
  li_inj.codes.push_back(MakeCode("lead_in"));
  lead_in.line_injections.push_back(li_inj);
  p.sections.push_back(lead_in);

  // Programme area.
  Section prog = MakeSection("Programme", SectionType::kProgrammeArea, 500);
  auto prog_inj = MakeLaserdiscInjection("CLV");
  prog_inj.codes.push_back(MakeCode("programme_time_code"));
  prog_inj.codes.push_back(MakeCode("clv_picture_number"));
  prog_inj.codes.push_back(MakeCode("fm_programme_time"));
  prog_inj.codes.push_back(MakeCode("fm_white_flag"));
  prog.line_injections.push_back(prog_inj);
  p.sections.push_back(prog);

  // Lead-out.
  Section lead_out = MakeSection("Lead-out", SectionType::kLeadOut, 10);
  auto lo_inj = MakeLaserdiscInjection("CLV");
  lo_inj.codes.push_back(MakeCode("lead_out"));
  lead_out.line_injections.push_back(lo_inj);
  p.sections.push_back(lead_out);

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_TRUE(r.is_valid) << (!r.errors.empty() ? r.errors[0] : "");
}

// ─── Multi-chapter PAL CAV project ───────────────────────────────────────────

TEST(BiphaseIntegrationTest, AcceptsMultiChapterPalCavProject) {
  Project p = MakeBaseProject(Standard::kPal);
  p.laserdisc.disc_type = "CAV";

  // Lead-in.
  Section lead_in = MakeSection("Lead-in", SectionType::kLeadIn, 938);
  auto li_inj = MakeLaserdiscInjection("CAV");
  li_inj.codes.push_back(MakeCode("lead_in"));
  lead_in.line_injections.push_back(li_inj);
  p.sections.push_back(lead_in);

  // Programme area with chapters and picture numbers.
  Section prog = MakeSection("Programme", SectionType::kProgrammeArea, 2000);
  auto prog_inj = MakeLaserdiscInjection("CAV");
  prog_inj.codes.push_back(MakePictureNumberCode(1));
  prog_inj.codes.push_back(MakeChapterCode(0));
  prog_inj.codes.push_back(MakeChapterCode(1));
  prog_inj.codes.push_back(MakeCode("picture_stop"));
  prog_inj.codes.push_back(MakeCode("programme_status"));
  prog.line_injections.push_back(prog_inj);
  p.sections.push_back(prog);

  // Lead-out.
  Section lead_out = MakeSection("Lead-out", SectionType::kLeadOut, 1250);
  auto lo_inj = MakeLaserdiscInjection("CAV");
  lo_inj.codes.push_back(MakeCode("lead_out"));
  lead_out.line_injections.push_back(lo_inj);
  p.sections.push_back(lead_out);

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_TRUE(r.is_valid) << (!r.errors.empty() ? r.errors[0] : "");
}

// ─── Mixed-section project: one section without laserdisc ────────────────────

TEST(BiphaseIntegrationTest,
     AcceptsProjectWithMixedLaserdiscAndNonLaserdiscSections) {
  // Non-laserdisc sections do not require any laserdisc-specific validation.
  Project p = MakeBaseProject(Standard::kPal);
  p.laserdisc.disc_type = "CAV";

  // Section without laserdisc.
  Section plain = MakeSection("Plain", SectionType::kUnknown, 25);
  p.sections.push_back(plain);

  // Section with laserdisc programme area.
  Section prog = MakeSection("Programme", SectionType::kProgrammeArea, 500);
  auto prog_inj = MakeLaserdiscInjection("CAV");
  prog_inj.codes.push_back(MakePictureNumberCode(1));
  prog.line_injections.push_back(prog_inj);
  p.sections.push_back(prog);

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_TRUE(r.is_valid) << (!r.errors.empty() ? r.errors[0] : "");
}

// ─── Invalid complete project: wrong code type in wrong section
// ───────────────

TEST(BiphaseIntegrationTest, RejectsProjectWithLeadInCodeInProgrammeSection) {
  Project p = MakeBaseProject(Standard::kPal);
  p.laserdisc.disc_type = "CAV";

  Section prog = MakeSection("Programme", SectionType::kProgrammeArea, 200);
  auto prog_inj = MakeLaserdiscInjection("CAV");
  prog_inj.codes.push_back(MakeCode("lead_in"));  // wrong section
  prog.line_injections.push_back(prog_inj);
  p.sections.push_back(prog);

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_FALSE(r.is_valid);
  ASSERT_FALSE(r.errors.empty());
  EXPECT_NE(r.errors[0].find("lead_in"), std::string::npos);
}

// ─── Invalid project: insufficient lead-in duration ──────────────────────────

TEST(BiphaseIntegrationTest, WarnsCompleteProjectWithInsufficientLeadIn) {
  Project p = MakeBaseProject(Standard::kPal);
  p.laserdisc.disc_type = "CAV";

  Section lead_in =
      MakeSection("Lead-in", SectionType::kLeadIn, 100);  // below IEC minimum
  auto li_inj = MakeLaserdiscInjection("CAV");
  li_inj.codes.push_back(MakeCode("lead_in"));
  lead_in.line_injections.push_back(li_inj);
  p.sections.push_back(lead_in);

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_TRUE(r.is_valid);
  ASSERT_FALSE(r.warnings.empty());
  EXPECT_NE(r.warnings[0].find("lead_in"), std::string::npos);
}

// ─── Invalid project: NTSC missing VIRS in one section
// ────────────────────────

TEST(BiphaseIntegrationTest, RejectsNtscProjectWhenOneSectionMissingVirs) {
  Project p = MakeBaseProject(Standard::kNtsc);
  p.laserdisc.disc_type = "CAV";
  // VITS are now project-wide; omitting the virs injection at the project
  // level must reject an NTSC laserdisc project (IEC 60857 §9.1.3).

  Section s1 = MakeSection("Section1", SectionType::kProgrammeArea, 200);
  auto inj1 = MakeLaserdiscInjection("CAV");
  inj1.codes.push_back(MakePictureNumberCode(1));
  s1.line_injections.push_back(inj1);
  p.sections.push_back(s1);

  Section s2 = MakeSection("Section2", SectionType::kProgrammeArea, 200);
  auto inj2 = MakeLaserdiscInjection("CAV");
  inj2.codes.push_back(MakePictureNumberCode(200));
  s2.line_injections.push_back(inj2);
  p.sections.push_back(s2);

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_FALSE(r.is_valid);
  ASSERT_FALSE(r.errors.empty());
  EXPECT_NE(r.errors[0].find("virs"), std::string::npos);
}

// ─── Users code in lead-in and lead-out ──────────────────────────────────────

TEST(BiphaseIntegrationTest, AcceptsUsersCodeInBothLeadInAndLeadOut) {
  Project p = MakeBaseProject(Standard::kPal);
  p.laserdisc.disc_type = "CAV";

  Section lead_in = MakeSection("Lead-in", SectionType::kLeadIn, 938);
  auto li_inj = MakeLaserdiscInjection("CAV");
  li_inj.codes.push_back(MakeCode("lead_in"));
  Section::LineInjectionCode uc1;
  uc1.code_type = "users_code";
  uc1.users_code = "0x80D234";  // X1=0, D=0xD (canonical per IEC §10.1.9)
  uc1.users_code_specified = true;
  li_inj.codes.push_back(uc1);
  lead_in.line_injections.push_back(li_inj);
  p.sections.push_back(lead_in);

  Section lead_out = MakeSection("Lead-out", SectionType::kLeadOut, 1250);
  auto lo_inj = MakeLaserdiscInjection("CAV");
  lo_inj.codes.push_back(MakeCode("lead_out"));
  Section::LineInjectionCode uc2;
  uc2.code_type = "users_code";
  uc2.users_code = "0x87D000";  // X1=7, D=0xD (canonical per IEC §10.1.9)
  uc2.users_code_specified = true;
  lo_inj.codes.push_back(uc2);
  lead_out.line_injections.push_back(lo_inj);
  p.sections.push_back(lead_out);

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_TRUE(r.is_valid) << (!r.errors.empty() ? r.errors[0] : "");
}

}  // namespace
}  // namespace videosynth
