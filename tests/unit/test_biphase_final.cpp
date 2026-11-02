/*
 * File:        test_biphase_final.cpp
 * Module:      biphase_final_tests
 * Purpose:     Final integration test suite for the biphase injection system.
 *              Covers complete PAL/NTSC CAV/CLV multi-section projects,
 *              multi-chapter configurations, edge cases, and cross-type code
 *              exclusivity (task 6.9).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "videosynth/model.h"
#include "videosynth/project_validator.h"

namespace videosynth {
namespace {

// ---------------------------------------------------------------------------
// Helper builders
// ---------------------------------------------------------------------------

Project MakeBaseProject(Standard standard) {
  Project p;
  p.cvbs_presets.video_standard_preset = standard;
  p.cvbs_presets.sample_encoding_preset = "CVBS_U10_4FSC";
  p.cvbs_presets.signal_state_preset = "STANDARD_STABLE_LOCKED";
  p.output.video_path = "/tmp/final_test.cvbs";
  p.output.metadata_path = "/tmp/final_test.meta";
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

Section::LineInjectionCode MakeChapterCode(int chapter_num) {
  Section::LineInjectionCode c;
  c.code_type = "chapter_number";
  c.chapter = chapter_num;
  c.chapter_specified = true;
  return c;
}

Section::LineInjectionCode MakeUsersCode(const std::string& hex) {
  Section::LineInjectionCode c;
  c.code_type = "users_code";
  c.users_code = hex;
  c.users_code_specified = true;
  return c;
}

// Build a minimal valid PAL CAV project with lead-in, programme, lead-out.
Project MakeMinimalPalCavProject() {
  Project p = MakeBaseProject(Standard::kPal);
  p.laserdisc.disc_type = "CAV";

  Section lead_in = MakeSection("Lead-in", SectionType::kLeadIn, 938);
  auto li_inj = MakeLaserdiscInjection("CAV");
  li_inj.codes.push_back(MakeCode("lead_in"));
  lead_in.line_injections.push_back(li_inj);
  p.sections.push_back(lead_in);

  Section prog = MakeSection("Programme", SectionType::kProgrammeArea, 500);
  auto prog_inj = MakeLaserdiscInjection("CAV");
  prog_inj.codes.push_back(MakePictureNumberCode(1));
  prog.line_injections.push_back(prog_inj);
  p.sections.push_back(prog);

  Section lead_out = MakeSection("Lead-out", SectionType::kLeadOut, 1250);
  auto lo_inj = MakeLaserdiscInjection("CAV");
  lo_inj.codes.push_back(MakeCode("lead_out"));
  lead_out.line_injections.push_back(lo_inj);
  p.sections.push_back(lead_out);

  return p;
}

// Build a minimal valid PAL CLV project.
Project MakeMinimalPalClvProject() {
  Project p = MakeBaseProject(Standard::kPal);
  p.laserdisc.disc_type = "CLV";

  Section lead_in = MakeSection("Lead-in", SectionType::kLeadIn, 938);
  auto li_inj = MakeLaserdiscInjection("CLV");
  li_inj.codes.push_back(MakeCode("lead_in"));
  lead_in.line_injections.push_back(li_inj);
  p.sections.push_back(lead_in);

  Section prog = MakeSection("Programme", SectionType::kProgrammeArea, 1500);
  auto prog_inj = MakeLaserdiscInjection("CLV");
  prog_inj.codes.push_back(MakeCode("programme_time_code"));
  prog_inj.codes.push_back(MakeCode("clv_code"));
  prog_inj.codes.push_back(MakeCode("clv_picture_number"));
  prog.line_injections.push_back(prog_inj);
  p.sections.push_back(prog);

  Section lead_out = MakeSection("Lead-out", SectionType::kLeadOut, 1250);
  auto lo_inj = MakeLaserdiscInjection("CLV");
  lo_inj.codes.push_back(MakeCode("lead_out"));
  lead_out.line_injections.push_back(lo_inj);
  p.sections.push_back(lead_out);

  return p;
}

// Build a minimal valid NTSC CAV project with VIRS.
Project MakeMinimalNtscCavProject() {
  Project p = MakeBaseProject(Standard::kNtsc);
  p.laserdisc.disc_type = "CAV";
  p.line_injections.vits.push_back(MakeVirsInjection());

  Section lead_in = MakeSection("Lead-in", SectionType::kLeadIn, 938);
  auto li_inj = MakeLaserdiscInjection("CAV");
  li_inj.codes.push_back(MakeCode("lead_in"));
  li_inj.codes.push_back(MakeCode("fm_white_flag"));
  lead_in.line_injections.push_back(li_inj);
  p.sections.push_back(lead_in);

  Section prog = MakeSection("Programme", SectionType::kProgrammeArea, 500);
  auto prog_inj = MakeLaserdiscInjection("CAV");
  prog_inj.codes.push_back(MakePictureNumberCode(1));
  prog_inj.codes.push_back(MakeCode("fm_picture_number"));
  prog_inj.codes.push_back(MakeCode("fm_white_flag"));
  prog.line_injections.push_back(prog_inj);
  p.sections.push_back(prog);

  Section lead_out = MakeSection("Lead-out", SectionType::kLeadOut, 1250);
  auto lo_inj = MakeLaserdiscInjection("CAV");
  lo_inj.codes.push_back(MakeCode("lead_out"));
  lo_inj.codes.push_back(MakeCode("fm_white_flag"));
  lead_out.line_injections.push_back(lo_inj);
  p.sections.push_back(lead_out);

  return p;
}

// ---------------------------------------------------------------------------
// BiphaseFinalTest — complete PAL CAV project
// ---------------------------------------------------------------------------

TEST(BiphaseFinalTest, AcceptsMinimalPalCavProject) {
  const Project p = MakeMinimalPalCavProject();
  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_TRUE(r.is_valid) << (!r.errors.empty() ? r.errors[0] : "");
}

TEST(BiphaseFinalTest, AcceptsPalCavProjectWithPictureStop) {
  Project p = MakeMinimalPalCavProject();
  auto& prog_inj = p.sections[1].line_injections[0];
  prog_inj.codes.push_back(MakeCode("picture_stop"));

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_TRUE(r.is_valid) << (!r.errors.empty() ? r.errors[0] : "");
}

TEST(BiphaseFinalTest, AcceptsPalCavProjectWithUsersCodeInLeadIn) {
  Project p = MakeMinimalPalCavProject();
  p.sections[0].line_injections[0].codes.push_back(MakeUsersCode("0x80D234"));

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_TRUE(r.is_valid) << (!r.errors.empty() ? r.errors[0] : "");
}

TEST(BiphaseFinalTest, AcceptsPalCavProjectWithUsersCodeInLeadOut) {
  Project p = MakeMinimalPalCavProject();
  p.sections[2].line_injections[0].codes.push_back(MakeUsersCode("0x87D000"));

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_TRUE(r.is_valid) << (!r.errors.empty() ? r.errors[0] : "");
}

TEST(BiphaseFinalTest, AcceptsPalCavProjectWithMaxPictureNumber) {
  Project p = MakeMinimalPalCavProject();
  // Replace picture_number with max PAL value 99999.
  p.sections[1].line_injections[0].codes.clear();
  p.sections[1].line_injections[0].codes.push_back(
      MakePictureNumberCode(99999));

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_TRUE(r.is_valid) << (!r.errors.empty() ? r.errors[0] : "");
}

// ---------------------------------------------------------------------------
// BiphaseFinalTest — complete PAL CLV project
// ---------------------------------------------------------------------------

TEST(BiphaseFinalTest, AcceptsMinimalPalClvProject) {
  const Project p = MakeMinimalPalClvProject();
  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_TRUE(r.is_valid) << (!r.errors.empty() ? r.errors[0] : "");
}

TEST(BiphaseFinalTest, AcceptsPalClvProjectWithChapterCode) {
  Project p = MakeMinimalPalClvProject();
  p.sections[1].line_injections[0].codes.push_back(MakeChapterCode(0));

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_TRUE(r.is_valid) << (!r.errors.empty() ? r.errors[0] : "");
}

// ---------------------------------------------------------------------------
// BiphaseFinalTest — complete NTSC CAV project
// ---------------------------------------------------------------------------

TEST(BiphaseFinalTest, AcceptsMinimalNtscCavProject) {
  const Project p = MakeMinimalNtscCavProject();
  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_TRUE(r.is_valid) << (!r.errors.empty() ? r.errors[0] : "");
}

TEST(BiphaseFinalTest, AcceptsNtscCavProjectWithMaxPictureNumber) {
  Project p = MakeMinimalNtscCavProject();
  // Replace picture_number in programme area with max NTSC value 79999.
  p.sections[1].line_injections[0].codes.clear();
  p.sections[1].line_injections[0].codes.push_back(
      MakePictureNumberCode(79999));
  p.sections[1].line_injections[0].codes.push_back(
      MakeCode("fm_picture_number"));
  p.sections[1].line_injections[0].codes.push_back(MakeCode("fm_white_flag"));

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_TRUE(r.is_valid) << (!r.errors.empty() ? r.errors[0] : "");
}

// ---------------------------------------------------------------------------
// BiphaseFinalTest — complete NTSC CLV project
// ---------------------------------------------------------------------------

TEST(BiphaseFinalTest, AcceptsMinimalNtscClvProject) {
  Project p = MakeBaseProject(Standard::kNtsc);
  p.laserdisc.disc_type = "CLV";
  p.line_injections.vits.push_back(MakeVirsInjection());

  Section lead_in = MakeSection("Lead-in", SectionType::kLeadIn, 938);
  auto li_inj = MakeLaserdiscInjection("CLV");
  li_inj.codes.push_back(MakeCode("lead_in"));
  li_inj.codes.push_back(MakeCode("fm_white_flag"));
  lead_in.line_injections.push_back(li_inj);
  p.sections.push_back(lead_in);

  Section prog = MakeSection("Programme", SectionType::kProgrammeArea, 1800);
  auto prog_inj = MakeLaserdiscInjection("CLV");
  prog_inj.codes.push_back(MakeCode("programme_time_code"));
  prog_inj.codes.push_back(MakeCode("clv_picture_number"));
  prog_inj.codes.push_back(MakeCode("clv_code"));
  prog_inj.codes.push_back(MakeCode("fm_programme_time"));
  prog_inj.codes.push_back(MakeCode("fm_white_flag"));
  prog.line_injections.push_back(prog_inj);
  p.sections.push_back(prog);

  Section lead_out = MakeSection("Lead-out", SectionType::kLeadOut, 1250);
  auto lo_inj = MakeLaserdiscInjection("CLV");
  lo_inj.codes.push_back(MakeCode("lead_out"));
  lo_inj.codes.push_back(MakeCode("fm_white_flag"));
  lead_out.line_injections.push_back(lo_inj);
  p.sections.push_back(lead_out);

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_TRUE(r.is_valid) << (!r.errors.empty() ? r.errors[0] : "");
}

// ---------------------------------------------------------------------------
// BiphaseFinalTest — multi-chapter PAL CAV project
// ---------------------------------------------------------------------------

TEST(BiphaseFinalTest, AcceptsPalCavProjectWithSingleChapter) {
  Project p = MakeMinimalPalCavProject();
  p.sections[1].line_injections[0].codes.push_back(MakeChapterCode(0));

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_TRUE(r.is_valid) << (!r.errors.empty() ? r.errors[0] : "");
}

TEST(BiphaseFinalTest, AcceptsPalCavProjectWithThreeChapters) {
  Project p = MakeBaseProject(Standard::kPal);
  p.laserdisc.disc_type = "CAV";

  Section lead_in = MakeSection("Lead-in", SectionType::kLeadIn, 938);
  auto li_inj = MakeLaserdiscInjection("CAV");
  li_inj.codes.push_back(MakeCode("lead_in"));
  lead_in.line_injections.push_back(li_inj);
  p.sections.push_back(lead_in);

  // Chapter 0 (first after lead-in).
  Section ch0 = MakeSection("Chapter0", SectionType::kProgrammeArea, 500);
  auto ch0_inj = MakeLaserdiscInjection("CAV");
  ch0_inj.codes.push_back(MakePictureNumberCode(1));
  ch0_inj.codes.push_back(MakeChapterCode(0));
  ch0.line_injections.push_back(ch0_inj);
  p.sections.push_back(ch0);

  // Chapter 1.
  Section ch1 = MakeSection("Chapter1", SectionType::kProgrammeArea, 500);
  auto ch1_inj = MakeLaserdiscInjection("CAV");
  ch1_inj.codes.push_back(MakePictureNumberCode(501));
  ch1_inj.codes.push_back(MakeChapterCode(1));
  ch1.line_injections.push_back(ch1_inj);
  p.sections.push_back(ch1);

  // Chapter 2.
  Section ch2 = MakeSection("Chapter2", SectionType::kProgrammeArea, 500);
  auto ch2_inj = MakeLaserdiscInjection("CAV");
  ch2_inj.codes.push_back(MakePictureNumberCode(1001));
  ch2_inj.codes.push_back(MakeChapterCode(2));
  ch2.line_injections.push_back(ch2_inj);
  p.sections.push_back(ch2);

  Section lead_out = MakeSection("Lead-out", SectionType::kLeadOut, 1250);
  auto lo_inj = MakeLaserdiscInjection("CAV");
  lo_inj.codes.push_back(MakeCode("lead_out"));
  lead_out.line_injections.push_back(lo_inj);
  p.sections.push_back(lead_out);

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_TRUE(r.is_valid) << (!r.errors.empty() ? r.errors[0] : "");
}

TEST(BiphaseFinalTest, AcceptsPalCavProjectWithMaxChapterNumber79) {
  Project p = MakeMinimalPalCavProject();
  p.sections[1].line_injections[0].codes.push_back(MakeChapterCode(79));

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_TRUE(r.is_valid) << (!r.errors.empty() ? r.errors[0] : "");
}

// ---------------------------------------------------------------------------
// BiphaseFinalTest — minimum section durations
// ---------------------------------------------------------------------------

TEST(BiphaseFinalTest, AcceptsPalCavWithExactMinimumLeadIn938Frames) {
  Project p = MakeMinimalPalCavProject();
  // sections[0] already has 938 frames — verify acceptance.
  EXPECT_EQ(p.sections[0].duration_frames, 938);

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_TRUE(r.is_valid) << (!r.errors.empty() ? r.errors[0] : "");
}

TEST(BiphaseFinalTest, WarnsPalCavWithLeadInBelow938Frames) {
  Project p = MakeMinimalPalCavProject();
  p.sections[0].duration_frames = 937;  // one below IEC minimum

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_TRUE(r.is_valid);
  EXPECT_FALSE(r.warnings.empty());
}

TEST(BiphaseFinalTest, AcceptsPalCavWithExactMinimumLeadOut1250Frames) {
  Project p = MakeMinimalPalCavProject();
  EXPECT_EQ(p.sections[2].duration_frames, 1250);

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_TRUE(r.is_valid) << (!r.errors.empty() ? r.errors[0] : "");
}

TEST(BiphaseFinalTest, WarnsPalCavWithLeadOutBelow1250Frames) {
  Project p = MakeMinimalPalCavProject();
  p.sections[2].duration_frames = 1249;  // one below IEC minimum

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_TRUE(r.is_valid);
  EXPECT_FALSE(r.warnings.empty());
}

// ---------------------------------------------------------------------------
// BiphaseFinalTest — cross-type code exclusivity
// ---------------------------------------------------------------------------

TEST(BiphaseFinalTest, RejectsCavProjectWithClvOnlyCode) {
  // CAV disc must not accept CLV-only codes like programme_time_code.
  Project p = MakeMinimalPalCavProject();
  p.sections[1].line_injections[0].codes.push_back(
      MakeCode("programme_time_code"));

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_FALSE(r.is_valid);
}

TEST(BiphaseFinalTest, RejectsClvProjectWithCavOnlyCode) {
  // CLV disc must not accept CAV-only codes like picture_number.
  Project p = MakeMinimalPalClvProject();
  p.sections[1].line_injections[0].codes.push_back(MakePictureNumberCode(1));

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_FALSE(r.is_valid);
}

TEST(BiphaseFinalTest, RejectsPalProjectWithNtscOnlyFmCodes) {
  // PAL projects must not accept NTSC-only FM codes.
  Project p = MakeMinimalPalCavProject();
  p.sections[1].line_injections[0].codes.push_back(
      MakeCode("fm_picture_number"));

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_FALSE(r.is_valid);
}

TEST(BiphaseFinalTest, RejectsPalProjectWithFmWhiteFlag) {
  // fm_white_flag is NTSC-only.
  Project p = MakeMinimalPalCavProject();
  p.sections[1].line_injections[0].codes.push_back(MakeCode("fm_white_flag"));

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_FALSE(r.is_valid);
}

TEST(BiphaseFinalTest, RejectsUsersCodeInProgrammeArea) {
  // Users code is only permitted in lead-in and lead-out.
  Project p = MakeMinimalPalCavProject();
  p.sections[1].line_injections[0].codes.push_back(MakeUsersCode("0x80D234"));

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_FALSE(r.is_valid);
}

TEST(BiphaseFinalTest, RejectsPictureNumberInLeadInSection) {
  Project p = MakeMinimalPalCavProject();
  p.sections[0].line_injections[0].codes.push_back(MakePictureNumberCode(1));

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_FALSE(r.is_valid);
}

TEST(BiphaseFinalTest, RejectsLeadInCodeInProgrammeSection) {
  Project p = MakeMinimalPalCavProject();
  p.sections[1].line_injections[0].codes.push_back(MakeCode("lead_in"));

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_FALSE(r.is_valid);
}

TEST(BiphaseFinalTest, RejectsLeadOutCodeInProgrammeSection) {
  Project p = MakeMinimalPalCavProject();
  p.sections[1].line_injections[0].codes.push_back(MakeCode("lead_out"));

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_FALSE(r.is_valid);
}

// ---------------------------------------------------------------------------
// BiphaseFinalTest — value range edge cases
// ---------------------------------------------------------------------------

TEST(BiphaseFinalTest, RejectsPalPictureNumberAbove99999) {
  Project p = MakeMinimalPalCavProject();
  p.sections[1].line_injections[0].codes.clear();
  p.sections[1].line_injections[0].codes.push_back(
      MakePictureNumberCode(100000));

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_FALSE(r.is_valid);
}

TEST(BiphaseFinalTest, RejectsNtscPictureNumberAbove79999) {
  Project p = MakeMinimalNtscCavProject();
  p.sections[1].line_injections[0].codes.clear();
  p.sections[1].line_injections[0].codes.push_back(
      MakePictureNumberCode(80000));
  p.sections[1].line_injections[0].codes.push_back(
      MakeCode("fm_picture_number"));
  p.sections[1].line_injections[0].codes.push_back(MakeCode("fm_white_flag"));

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_FALSE(r.is_valid);
}

TEST(BiphaseFinalTest, RejectsChapterNumberAbove79) {
  Project p = MakeMinimalPalCavProject();
  Section::LineInjectionCode bad_chapter;
  bad_chapter.code_type = "chapter_number";
  bad_chapter.chapter = 80;
  bad_chapter.chapter_specified = true;
  p.sections[1].line_injections[0].codes.push_back(bad_chapter);

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_FALSE(r.is_valid);
}

TEST(BiphaseFinalTest, RejectsUsersCodeWithX1Above7) {
  Project p = MakeMinimalPalCavProject();
  // X1 = 8 → invalid (must be 0-7).
  p.sections[0].line_injections[0].codes.push_back(MakeUsersCode("0x881234"));

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_FALSE(r.is_valid);
}

// ---------------------------------------------------------------------------
// BiphaseFinalTest — NTSC VIRS mandatory presence
// ---------------------------------------------------------------------------

TEST(BiphaseFinalTest, RejectsNtscProjectMissingVirs) {
  // NTSC projects with laserdisc injection require virs on 19/282.
  Project p = MakeBaseProject(Standard::kNtsc);
  p.laserdisc.disc_type = "CAV";
  // Intentionally no virs injection at the project level.

  Section prog = MakeSection("Programme", SectionType::kProgrammeArea, 500);
  auto prog_inj = MakeLaserdiscInjection("CAV");
  prog_inj.codes.push_back(MakePictureNumberCode(1));
  prog_inj.codes.push_back(MakeCode("fm_picture_number"));
  prog_inj.codes.push_back(MakeCode("fm_white_flag"));
  prog.line_injections.push_back(prog_inj);
  p.sections.push_back(prog);

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_FALSE(r.is_valid);
  ASSERT_FALSE(r.errors.empty());
  EXPECT_NE(r.errors[0].find("virs"), std::string::npos);
}

// ---------------------------------------------------------------------------
// BiphaseFinalTest — PAL-only project rejects CAV duration constraint
// violation even with valid CLV marker
// ---------------------------------------------------------------------------

TEST(BiphaseFinalTest, ClvLeadInHasNoDurationConstraint) {
  // CLV discs have variable track density; the validator must not enforce a
  // frame-count minimum on CLV lead-in sections.
  Project p = MakeBaseProject(Standard::kPal);
  p.laserdisc.disc_type = "CLV";

  Section lead_in = MakeSection("Lead-in", SectionType::kLeadIn, 100);
  auto li_inj = MakeLaserdiscInjection("CLV");
  li_inj.codes.push_back(MakeCode("lead_in"));
  lead_in.line_injections.push_back(li_inj);
  p.sections.push_back(lead_in);

  Section prog = MakeSection("Programme", SectionType::kProgrammeArea, 500);
  auto prog_inj = MakeLaserdiscInjection("CLV");
  prog_inj.codes.push_back(MakeCode("programme_time_code"));
  prog_inj.codes.push_back(MakeCode("clv_code"));
  prog_inj.codes.push_back(MakeCode("clv_picture_number"));
  prog.line_injections.push_back(prog_inj);
  p.sections.push_back(prog);

  Section lead_out = MakeSection("Lead-out", SectionType::kLeadOut, 50);
  auto lo_inj = MakeLaserdiscInjection("CLV");
  lo_inj.codes.push_back(MakeCode("lead_out"));
  lead_out.line_injections.push_back(lo_inj);
  p.sections.push_back(lead_out);

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_TRUE(r.is_valid) << (!r.errors.empty() ? r.errors[0] : "");
}

// ---------------------------------------------------------------------------
// BiphaseFinalTest — programme_area has no duration constraint
// ---------------------------------------------------------------------------

TEST(BiphaseFinalTest, ProgrammeAreaAcceptsAnyDuration) {
  // The IEC standards do not impose a minimum frame count on the programme
  // area.
  Project p = MakeMinimalPalCavProject();
  p.sections[1].duration_frames = 1;  // single frame programme area

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_TRUE(r.is_valid) << (!r.errors.empty() ? r.errors[0] : "");
}

// ---------------------------------------------------------------------------
// BiphaseFinalTest — section-type-only validation (no injection) passes
// ---------------------------------------------------------------------------

TEST(BiphaseFinalTest, SectionWithNoLaserdiscInjectionIsNotValidated) {
  // A section without a laserdisc injection must pass without biphase checks.
  Project p = MakeBaseProject(Standard::kPal);

  p.laserdisc.enabled = true;
  Section s = MakeSection("Plain", SectionType::kProgrammeArea, 100);
  // No line_injections — plain video section.
  p.sections.push_back(s);

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_TRUE(r.is_valid) << (!r.errors.empty() ? r.errors[0] : "");
}

// ---------------------------------------------------------------------------
// BiphaseFinalTest — complete project with programme_status code
// ---------------------------------------------------------------------------

TEST(BiphaseFinalTest, AcceptsPalCavProjectWithProgrammeStatusCode) {
  Project p = MakeMinimalPalCavProject();

  Section::LineInjectionCode ps;
  ps.code_type = "programme_status";
  ps.programme_status = "0x8DC000";
  ps.programme_status_specified = true;
  p.sections[1].line_injections[0].codes.push_back(ps);

  ProjectValidator v;
  const auto r = v.Validate(p);
  EXPECT_TRUE(r.is_valid) << (!r.errors.empty() ? r.errors[0] : "");
}

}  // namespace
}  // namespace videosynth
