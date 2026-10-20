/*
 * File:        test_line_injection_presenter.cpp
 * Module:      gui_tests
 * Purpose:     Unit tests for the line-injection catalogues against the
 *              validator's compatibility matrix
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "line_injection_presenter.h"
#include "videosynth/biphase_types.h"
#include "videosynth/model.h"
#include "videosynth/project_validator.h"

namespace videosynth::gui {
namespace {

bool Contains(const std::vector<std::string>& values,
              const std::string& value) {
  return std::find(values.begin(), values.end(), value) != values.end();
}

// Builds a structurally valid laserdisc project carrying exactly one code of
// `code_type` so the validator's verdict isolates the compatibility matrix.
Project MakeLaserdiscProject(Standard standard, DiscType disc_type,
                             SectionType section_type,
                             const std::string& code_type) {
  Project project;
  project.cvbs_presets.video_standard_preset = standard;
  project.output.video_path = "out/video.cvbs";
  project.output.metadata_path = "out/video.meta";
  // disc_type and the VITS set are project-wide.
  project.line_injections.disc_type = DiscTypeToString(disc_type);
  if (standard == Standard::kNtsc || standard == Standard::kPalM) {
    // IEC 60857 §9.1.3: System M laserdisc discs need a virs reference.
    project.line_injections.vits.push_back(VitsInjection{"virs", {19, 282}});
  }

  Section section;
  section.name = "Disc";
  section.type = "progressive";
  section.source = "assets/disc.exr";
  section.duration_frames = 2000;
  section.section_type = section_type;

  Section::LineInjection laserdisc;
  laserdisc.type = "laserdisc";
  Section::LineInjectionCode code;
  code.code_type = code_type;
  laserdisc.codes.push_back(code);
  section.line_injections.push_back(laserdisc);

  project.sections.push_back(section);
  return project;
}

TEST(LineInjectionPresenterTest, InjectionTypesAreGeneratable) {
  const std::vector<std::string> types = AvailableInjectionTypes();
  // VITS moved to the project-wide block; section-level injections carry only
  // laserdisc biphase codes.
  EXPECT_TRUE(Contains(types, "laserdisc"));
  EXPECT_FALSE(Contains(types, "vits"));
  EXPECT_FALSE(Contains(types, "vitc"));
  EXPECT_FALSE(Contains(types, "line_content"));
}

TEST(LineInjectionPresenterTest, VitsCatalogueIsStandardFiltered) {
  const std::vector<std::string> pal = AvailableVitsTypes(Standard::kPal);
  EXPECT_TRUE(Contains(pal, "vits17"));
  EXPECT_TRUE(Contains(pal, "itu-multiburst"));
  EXPECT_FALSE(Contains(pal, "virs"));
  EXPECT_FALSE(Contains(pal, "ntc7-composite"));

  const std::vector<std::string> ntsc = AvailableVitsTypes(Standard::kNtsc);
  EXPECT_TRUE(Contains(ntsc, "virs"));
  EXPECT_TRUE(Contains(ntsc, "ntc7-composite"));
  EXPECT_FALSE(Contains(ntsc, "vits17"));

  // PAL-M uses the System M catalogue (mirrors TryGetDefinition fallback).
  EXPECT_EQ(AvailableVitsTypes(Standard::kPalM), ntsc);
}

TEST(LineInjectionPresenterTest, LaserdiscVitsSetPlacesPalSignals) {
  const std::vector<VitsInjection> set = LaserdiscVitsSet(Standard::kPal);
  ASSERT_EQ(set.size(), 2U);
  EXPECT_EQ(set[0].vits_type, "uk-national");
  EXPECT_EQ(set[0].target_lines, (std::vector<int>{19, 332}));
  EXPECT_EQ(set[1].vits_type, "vits20");
  EXPECT_EQ(set[1].target_lines, (std::vector<int>{20, 333}));
}

TEST(LineInjectionPresenterTest, LaserdiscVitsSetPlacesNtscSignals) {
  const std::vector<VitsInjection> set = LaserdiscVitsSet(Standard::kNtsc);
  ASSERT_EQ(set.size(), 3U);
  EXPECT_EQ(set[0].vits_type, "virs");
  EXPECT_EQ(set[0].target_lines, (std::vector<int>{19, 282}));
  EXPECT_EQ(set[1].vits_type, "ntc7-composite");
  EXPECT_EQ(set[1].target_lines, (std::vector<int>{20}));
  EXPECT_EQ(set[2].vits_type, "ntc7-combination");
  EXPECT_EQ(set[2].target_lines, (std::vector<int>{283}));
  // PAL-M shares the System M set.
  EXPECT_EQ(LaserdiscVitsSet(Standard::kPalM), set);
}

TEST(LineInjectionPresenterTest,
     LaserdiscVitsSetValidatesUnderLaserdiscPlacement) {
  for (const Standard standard : {Standard::kPal, Standard::kNtsc}) {
    Project project =
        MakeLaserdiscProject(standard, DiscType::kCAV,
                             SectionType::kProgrammeArea, "picture_number");
    project.line_injections.placement = VitsPlacement::kLaserdisc;
    project.line_injections.vits = LaserdiscVitsSet(standard);

    ProjectValidator validator;
    const ValidationResult result = validator.Validate(project);

    EXPECT_TRUE(result.is_valid)
        << "standard=" << StandardToString(standard) << ": "
        << (result.errors.empty() ? "" : result.errors.front());
  }
}

TEST(LineInjectionPresenterTest, ReconcileReSeedsLaserdiscSetOnStandardChange) {
  // PAL laserdisc set → switch to NTSC must re-seed the NTSC laserdisc set.
  ProjectLineInjections pal;
  pal.disc_type = "CAV";
  pal.placement = VitsPlacement::kLaserdisc;
  pal.vits = LaserdiscVitsSet(Standard::kPal);

  const ProjectLineInjections ntsc =
      ReconcileVitsForStandard(pal, Standard::kNtsc);

  EXPECT_EQ(ntsc.disc_type, "CAV");
  EXPECT_EQ(ntsc.placement, VitsPlacement::kLaserdisc);
  EXPECT_EQ(ntsc.vits, LaserdiscVitsSet(Standard::kNtsc));
}

TEST(LineInjectionPresenterTest, ReconcileDropsCrossStandardVitsTypes) {
  // A standard/custom-placement PAL type has no NTSC counterpart and is
  // dropped.
  ProjectLineInjections pal;
  pal.placement = VitsPlacement::kCustom;
  pal.vits.push_back(VitsInjection{"vits17", {21}});

  const ProjectLineInjections ntsc =
      ReconcileVitsForStandard(pal, Standard::kNtsc);

  EXPECT_EQ(ntsc.placement, VitsPlacement::kCustom);
  EXPECT_TRUE(ntsc.vits.empty());
}

TEST(LineInjectionPresenterTest, ReconcileKeepsStillValidVitsTypes) {
  // NTSC virs remains valid on PAL-M (shared System M catalogue).
  ProjectLineInjections ntsc;
  ntsc.placement = VitsPlacement::kCustom;
  ntsc.vits.push_back(VitsInjection{"virs", {19, 282}});

  const ProjectLineInjections palm =
      ReconcileVitsForStandard(ntsc, Standard::kPalM);

  ASSERT_EQ(palm.vits.size(), 1U);
  EXPECT_EQ(palm.vits[0].vits_type, "virs");
}

TEST(LineInjectionPresenterTest, ReconcileClearsLaserdiscFormatForSecam) {
  // There is no SECAM LaserVision format: the disc format is dropped and
  // laserdisc placement falls back to standard placement.
  ProjectLineInjections pal;
  pal.disc_type = "CAV";
  pal.placement = VitsPlacement::kLaserdisc;
  pal.vits = LaserdiscVitsSet(Standard::kPal);

  const ProjectLineInjections secam =
      ReconcileVitsForStandard(pal, Standard::kSecam);

  EXPECT_TRUE(secam.disc_type.empty());
  EXPECT_EQ(secam.placement, VitsPlacement::kStandard);
  EXPECT_TRUE(secam.vits.empty());
}

TEST(LineInjectionPresenterTest, ReconcileKeepsPalVitsTypesForSecam) {
  // SECAM uses the 625-line PAL VITS catalogue.
  ProjectLineInjections pal;
  pal.vits.push_back(VitsInjection{"vits17", {17}});

  const ProjectLineInjections secam =
      ReconcileVitsForStandard(pal, Standard::kSecam);

  ASSERT_EQ(secam.vits.size(), 1U);
  EXPECT_EQ(secam.vits[0].vits_type, "vits17");
  EXPECT_EQ(RecommendedVitsLine(Standard::kSecam, "vits17"), 17);
}

TEST(LineInjectionPresenterTest, RecommendedLinesComeFromCatalogue) {
  EXPECT_EQ(RecommendedVitsLine(Standard::kPal, "vits17"), 17);
  EXPECT_EQ(RecommendedVitsLine(Standard::kPal, "itu-multiburst"), 18);
  // virs has no fixed placement line.
  EXPECT_EQ(RecommendedVitsLine(Standard::kNtsc, "virs"), 0);
  EXPECT_EQ(RecommendedVitsLine(Standard::kPal, "no-such-type"), 0);
}

// The offered code types must be exactly the master-list entries the
// validator accepts for that disc type, section type, and standard.
TEST(LineInjectionPresenterTest, CodeTypeCatalogueMatchesValidatorMatrix) {
  ProjectValidator validator;

  for (const Standard standard : {Standard::kPal, Standard::kNtsc}) {
    for (const DiscType disc_type : {DiscType::kCAV, DiscType::kCLV}) {
      for (const SectionType section_type :
           {SectionType::kLeadIn, SectionType::kProgrammeArea,
            SectionType::kLeadOut}) {
        const std::vector<std::string> offered =
            AvailableLaserdiscCodeTypes(disc_type, section_type, standard);

        for (const std::string& code_type : AllLaserdiscCodeTypes()) {
          const Project project = MakeLaserdiscProject(standard, disc_type,
                                                       section_type, code_type);
          const bool accepted = validator.Validate(project).is_valid;
          EXPECT_EQ(accepted, Contains(offered, code_type))
              << StandardToString(standard) << "/"
              << DiscTypeToString(disc_type) << "/"
              << SectionTypeToString(section_type) << ": " << code_type;
        }
      }
    }
  }
}

TEST(LineInjectionPresenterTest, CommonCodeTypesIntersectSelectedSectionTypes) {
  // A single type behaves like the plain catalogue.
  EXPECT_EQ(CommonLaserdiscCodeTypes(
                DiscType::kCAV, {SectionType::kProgrammeArea}, Standard::kPal),
            AvailableLaserdiscCodeTypes(
                DiscType::kCAV, SectionType::kProgrammeArea, Standard::kPal));

  // Every offered code must be valid for every listed section type — nothing
  // in a batch catalogue may fail the validator on any selected section.
  const std::vector<SectionType> mixed = {
      SectionType::kLeadIn, SectionType::kProgrammeArea, SectionType::kLeadOut};
  for (const Standard standard : {Standard::kPal, Standard::kNtsc}) {
    for (const DiscType disc_type : {DiscType::kCAV, DiscType::kCLV}) {
      for (const std::string& code_type :
           CommonLaserdiscCodeTypes(disc_type, mixed, standard)) {
        for (const SectionType section_type : mixed) {
          EXPECT_TRUE(IsCodeTypeValidForSectionType(code_type, section_type))
              << code_type << " in " << SectionTypeToString(section_type);
        }
      }
    }
  }

  // The user's failure case: users_code is valid for lead-in and lead-out but
  // not programme_area, so a mixed selection must not offer it.
  EXPECT_FALSE(
      Contains(CommonLaserdiscCodeTypes(DiscType::kCAV, mixed, Standard::kPal),
               "users_code"));
  // Lead-in + lead-out alone do share users_code.
  EXPECT_TRUE(Contains(
      CommonLaserdiscCodeTypes(DiscType::kCAV,
                               {SectionType::kLeadIn, SectionType::kLeadOut},
                               Standard::kPal),
      "users_code"));
  // No selection, no catalogue.
  EXPECT_TRUE(
      CommonLaserdiscCodeTypes(DiscType::kCAV, {}, Standard::kPal).empty());
}

TEST(LineInjectionPresenterTest, CodeParameterMappingFollowsSchema) {
  EXPECT_TRUE(CodeTypeUsesStartValue("picture_number"));
  EXPECT_TRUE(CodeTypeUsesStartValue("fm_picture_number"));
  EXPECT_FALSE(CodeTypeUsesStartValue("chapter_number"));
  EXPECT_TRUE(CodeTypeUsesChapter("chapter_number"));
  EXPECT_TRUE(CodeTypeUsesProgrammeStatus("programme_status"));
  EXPECT_TRUE(CodeTypeUsesUsersCode("users_code"));
  EXPECT_FALSE(CodeTypeUsesUsersCode("lead_in"));
}

TEST(LineInjectionPresenterTest, CodeTypeHelpDescribesEveryKnownCodeType) {
  // Every code type the editor can offer must have help text, so the effect of
  // adding a code is never blank in the GUI.
  for (const std::string& code_type : AllLaserdiscCodeTypes()) {
    EXPECT_FALSE(CodeTypeHelp(code_type).empty()) << code_type;
  }
  EXPECT_TRUE(CodeTypeHelp("not_a_code_type").empty());
}

TEST(LineInjectionPresenterTest, CodeTypeHelpFlagsAutoProgressingClocks) {
  // The disc-global CLV clocks progress on their own; their help must say so
  // (this is the behaviour a user cannot otherwise discover from the GUI).
  EXPECT_NE(CodeTypeHelp("clv_picture_number").find("automatically"),
            std::string::npos);
  EXPECT_NE(CodeTypeHelp("programme_time_code").find("automatically"),
            std::string::npos);
  // CAV picture number continues across sections; help must mention it.
  EXPECT_NE(CodeTypeHelp("picture_number").find("continue"), std::string::npos);
}

TEST(LineInjectionPresenterTest, RecommendedCodesAreExpectedPerSectionType) {
  using std::vector;
  // Lead-in / lead-out: just the marker code (PAL, no System-M FM codes).
  EXPECT_EQ(RecommendedLaserdiscCodeTypes(DiscType::kCAV, SectionType::kLeadIn,
                                          Standard::kPal),
            (vector<std::string>{"lead_in"}));
  EXPECT_EQ(RecommendedLaserdiscCodeTypes(DiscType::kCLV, SectionType::kLeadOut,
                                          Standard::kPal),
            (vector<std::string>{"lead_out"}));

  // CAV programme area → picture_number + chapter_number.
  const auto cav = RecommendedLaserdiscCodeTypes(
      DiscType::kCAV, SectionType::kProgrammeArea, Standard::kPal);
  EXPECT_TRUE(Contains(cav, "picture_number"));
  EXPECT_TRUE(Contains(cav, "chapter_number"));
  EXPECT_FALSE(Contains(cav, "programme_time_code"));

  // CLV programme area → programme_time_code + clv_code + chapter_number.
  const auto clv = RecommendedLaserdiscCodeTypes(
      DiscType::kCLV, SectionType::kProgrammeArea, Standard::kPal);
  EXPECT_TRUE(Contains(clv, "programme_time_code"));
  EXPECT_TRUE(Contains(clv, "clv_code"));
  EXPECT_TRUE(Contains(clv, "chapter_number"));
  EXPECT_FALSE(Contains(clv, "picture_number"));

  // Unknown section type → nothing recommended.
  EXPECT_TRUE(RecommendedLaserdiscCodeTypes(
                  DiscType::kCAV, SectionType::kUnknown, Standard::kPal)
                  .empty());
}

TEST(LineInjectionPresenterTest, RecommendedCodesAddSystemMFmCodes) {
  const auto ntsc = RecommendedLaserdiscCodeTypes(
      DiscType::kCAV, SectionType::kProgrammeArea, Standard::kNtsc);
  EXPECT_TRUE(Contains(ntsc, "fm_picture_number"));
  EXPECT_TRUE(Contains(ntsc, "fm_white_flag"));
  // PAL (not System-M) gets no FM codes.
  const auto pal = RecommendedLaserdiscCodeTypes(
      DiscType::kCAV, SectionType::kProgrammeArea, Standard::kPal);
  EXPECT_FALSE(Contains(pal, "fm_picture_number"));
  EXPECT_FALSE(Contains(pal, "fm_white_flag"));
}

TEST(LineInjectionPresenterTest, RecommendedCodesAreAlwaysAValidSubset) {
  // Every recommended code must be offered by the availability catalogue for
  // the same context, so an editor can always render the pre-ticked set.
  const DiscType discs[] = {DiscType::kCAV, DiscType::kCLV};
  const SectionType sections[] = {
      SectionType::kLeadIn, SectionType::kProgrammeArea, SectionType::kLeadOut};
  const Standard standards[] = {Standard::kPal, Standard::kNtsc};
  for (DiscType disc : discs) {
    for (SectionType section : sections) {
      for (Standard standard : standards) {
        const auto available =
            AvailableLaserdiscCodeTypes(disc, section, standard);
        for (const std::string& code :
             RecommendedLaserdiscCodeTypes(disc, section, standard)) {
          EXPECT_TRUE(Contains(available, code))
              << code << " not offered for the context";
        }
      }
    }
  }
}

TEST(LineInjectionPresenterTest, VitsFixedLineAndDefaultLines) {
  // Fixed-placement PAL type: has a fixed line, defaults to exactly that line.
  EXPECT_TRUE(VitsHasFixedLine(Standard::kPal, "uk-national"));
  EXPECT_EQ(DefaultVitsLines(Standard::kPal, "uk-national"),
            (std::vector<int>{19}));

  // virs (System-M colour reference) is free-placement; defaults to both
  // fields, matching the built-in laserdisc template.
  EXPECT_FALSE(VitsHasFixedLine(Standard::kNtsc, "virs"));
  EXPECT_EQ(DefaultVitsLines(Standard::kNtsc, "virs"),
            (std::vector<int>{19, 282}));
}

TEST(LineInjectionPresenterTest, ParseTargetLinesAcceptsListsRejectsGarbage) {
  std::vector<int> lines;
  EXPECT_TRUE(ParseTargetLines("19, 282", &lines));
  EXPECT_EQ(lines, (std::vector<int>{19, 282}));

  EXPECT_TRUE(ParseTargetLines("17", &lines));
  EXPECT_EQ(lines, (std::vector<int>{17}));

  EXPECT_TRUE(ParseTargetLines("10 11 12", &lines));
  EXPECT_EQ(lines, (std::vector<int>{10, 11, 12}));

  EXPECT_TRUE(ParseTargetLines("", &lines));
  EXPECT_TRUE(lines.empty());

  EXPECT_FALSE(ParseTargetLines("abc", &lines));
  EXPECT_FALSE(ParseTargetLines("19,x", &lines));
  EXPECT_FALSE(ParseTargetLines("19a", &lines));
}

TEST(LineInjectionPresenterTest, FormatTargetLinesRoundTrips) {
  const std::vector<int> lines = {19, 282};
  std::vector<int> parsed;
  EXPECT_TRUE(ParseTargetLines(FormatTargetLines(lines), &parsed));
  EXPECT_EQ(parsed, lines);
  EXPECT_EQ(FormatTargetLines({}), "");
}

TEST(LineInjectionPresenterTest, TotalDiscFramesSumsSections) {
  Project project;
  Section a;
  a.duration_frames = 100;
  Section b;
  b.duration_frames = 50;
  project.sections = {a, b};
  EXPECT_EQ(TotalDiscFrames(project), 150);
}

}  // namespace
}  // namespace videosynth::gui
