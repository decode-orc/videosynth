/*
 * File:        test_cadence_presenters.cpp
 * Module:      gui_tests
 * Purpose:     Unit tests for the section editor's cadence presenters: field
 *              enablement, cadence edits, YAML round trip, undo, and the
 *              duration hint under 2:3 pulldown
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "project_document.h"
#include "section_block_presenters.h"
#include "videosynth/film_cadence.h"
#include "videosynth/model.h"
#include "videosynth/results.h"
#include "videosynth/source_frame_range.h"
#include "videosynth/yaml_project_emitter.h"
#include "videosynth/yaml_project_parser.h"

namespace videosynth::gui {
namespace {

// A film section playing source frames 0..`last_frame` once ("all").
Section MakeFilmSection(int last_frame) {
  Section section;
  section.name = "Film";
  section.type = "progressive";
  section.source = "film.mkv";
  section.end_frame = last_frame;
  section.end_frame_specified = true;
  section.duration_frames_all = true;
  section.duration_frames_repeat = 1;
  section.cadence.pattern = CadencePattern::kPulldown23;
  return section;
}

Project MakeNtscProject(const std::vector<Section>& sections) {
  Project project;
  project.cvbs_presets.video_standard_preset = Standard::kNtsc;
  project.output.video_path = "out/video.cvbs";
  project.output.metadata_path = "out/video.meta";
  project.sections = sections;
  return project;
}

// Emits `project` and parses it back.
Project RoundTrip(const Project& project) {
  YamlProjectParser parser;
  const ParseResult parsed =
      parser.ParseString(YamlProjectEmitter().EmitString(project));
  EXPECT_TRUE(parsed.ok) << (parsed.errors.empty() ? "" : parsed.errors[0]);
  return parsed.project;
}

TEST(CadencePresenterTest, VideoSectionOnNtscOffersOnlyThePattern) {
  Section section = MakeFilmSection(9);
  section.cadence = CadenceParameters{};

  const CadenceEditorState state =
      CadenceEditorStateFor(section, Standard::kNtsc);
  EXPECT_TRUE(state.pattern_enabled);
  EXPECT_FALSE(state.continue_enabled);
  EXPECT_FALSE(state.phase_enabled);
}

TEST(CadencePresenterTest, RestartingFilmSectionOffersPhaseAndContinue) {
  for (const Standard standard : {Standard::kNtsc, Standard::kPalM}) {
    const CadenceEditorState state =
        CadenceEditorStateFor(MakeFilmSection(9), standard);
    EXPECT_TRUE(state.pattern_enabled);
    EXPECT_TRUE(state.continue_enabled);
    EXPECT_TRUE(state.phase_enabled);
  }
}

TEST(CadencePresenterTest, ContinuingFilmSectionDisablesPhase) {
  Section section = MakeFilmSection(9);
  section.cadence.continuity = CadenceContinuity::kContinue;

  const CadenceEditorState state =
      CadenceEditorStateFor(section, Standard::kNtsc);
  EXPECT_TRUE(state.continue_enabled);
  EXPECT_FALSE(state.phase_enabled);
}

TEST(CadencePresenterTest, GroupIsDisabledFor625LineVideoSections) {
  Section section = MakeFilmSection(9);
  section.cadence = CadenceParameters{};

  for (const Standard standard : {Standard::kPal, Standard::kSecam}) {
    const CadenceEditorState state = CadenceEditorStateFor(section, standard);
    EXPECT_FALSE(state.pattern_enabled);
    EXPECT_FALSE(state.continue_enabled);
    EXPECT_FALSE(state.phase_enabled);
  }
}

TEST(CadencePresenterTest, StoredFilmCadenceOnPalStaysEditable) {
  // A 2:3 section in a PAL project is a validation error; the pattern stays
  // enabled so the user can switch it back to video.
  const CadenceEditorState state =
      CadenceEditorStateFor(MakeFilmSection(9), Standard::kPal);
  EXPECT_TRUE(state.pattern_enabled);
}

TEST(CadencePresenterTest, SupportsFilmPulldownOn525LineStandardsOnly) {
  EXPECT_TRUE(SupportsFilmPulldown(Standard::kNtsc));
  EXPECT_TRUE(SupportsFilmPulldown(Standard::kPalM));
  EXPECT_FALSE(SupportsFilmPulldown(Standard::kPal));
  EXPECT_FALSE(SupportsFilmPulldown(Standard::kSecam));
}

TEST(CadencePresenterTest, ChoosingVideoDropsTheCadenceBlock) {
  Section section = MakeFilmSection(9);
  SetCadencePhase(&section, CadencePhase::kC);

  SetCadencePattern(&section, CadencePattern::kVideo);
  EXPECT_EQ(section.cadence, CadenceParameters{});
  EXPECT_FALSE(section.cadence.phase_specified);
  EXPECT_EQ(YamlProjectEmitter()
                .EmitString(MakeNtscProject({section}))
                .find("cadence"),
            std::string::npos);
}

TEST(CadencePresenterTest, ChoosingFilmKeepsPhaseAndContinuity) {
  Section section = MakeFilmSection(9);
  section.cadence.phase = CadencePhase::kD;
  section.cadence.continuity = CadenceContinuity::kContinue;

  SetCadencePattern(&section, CadencePattern::kPulldown23);
  EXPECT_EQ(section.cadence.pattern, CadencePattern::kPulldown23);
  EXPECT_EQ(section.cadence.phase, CadencePhase::kD);
  EXPECT_EQ(section.cadence.continuity, CadenceContinuity::kContinue);
}

TEST(CadencePresenterTest, ContinuingClearsThePhase) {
  Section section = MakeFilmSection(9);
  SetCadencePhase(&section, CadencePhase::kB);
  ASSERT_TRUE(section.cadence.phase_specified);

  SetCadenceContinuity(&section, CadenceContinuity::kContinue);
  EXPECT_EQ(section.cadence.continuity, CadenceContinuity::kContinue);
  EXPECT_EQ(section.cadence.phase, CadencePhase::kA);
  EXPECT_FALSE(section.cadence.phase_specified);

  SetCadenceContinuity(&section, CadenceContinuity::kRestart);
  EXPECT_EQ(section.cadence.continuity, CadenceContinuity::kRestart);
}

TEST(CadencePresenterTest, CadenceEditsRoundTripThroughYaml) {
  Section restart = MakeFilmSection(47);
  SetCadencePhase(&restart, CadencePhase::kC);
  Section continued = MakeFilmSection(47);
  continued.name = "Film 2";
  SetCadenceContinuity(&continued, CadenceContinuity::kContinue);
  Section video = MakeFilmSection(47);
  video.name = "Video";
  video.source = "video.mkv";
  SetCadencePattern(&video, CadencePattern::kVideo);

  const Project project = MakeNtscProject({restart, continued, video});
  const Project parsed = RoundTrip(project);
  ASSERT_EQ(parsed.sections.size(), 3U);
  EXPECT_EQ(parsed.sections[0].cadence, restart.cadence);
  EXPECT_TRUE(parsed.sections[0].cadence.phase_specified);
  EXPECT_EQ(parsed.sections[1].cadence, continued.cadence);
  EXPECT_FALSE(parsed.sections[1].cadence.phase_specified);
  EXPECT_EQ(parsed.sections[2].cadence, CadenceParameters{});
}

TEST(CadencePresenterTest, CadenceEditUndoesThroughTheDocument) {
  ProjectDocument document;
  Section section = MakeFilmSection(47);
  section.cadence = CadenceParameters{};
  document.ResetProject(MakeNtscProject({section}), QString());

  Section edited = document.project().sections[0];
  SetCadencePattern(&edited, CadencePattern::kPulldown23);
  SetCadencePhase(&edited, CadencePhase::kB);
  ASSERT_TRUE(document.SetSection(0, edited));
  EXPECT_EQ(document.project().sections[0].cadence.phase, CadencePhase::kB);

  ASSERT_TRUE(document.Undo());
  EXPECT_EQ(document.project().sections[0].cadence, CadenceParameters{});

  ASSERT_TRUE(document.Redo());
  EXPECT_EQ(document.project().sections[0].cadence.pattern,
            CadencePattern::kPulldown23);
  EXPECT_EQ(document.project().sections[0].cadence.phase, CadencePhase::kB);
}

TEST(CadencePresenterTest, BatchEditMirrorsTheCadence) {
  const Section before = MakeFilmSection(9);
  Section after = before;
  SetCadencePhase(&after, CadencePhase::kD);
  Section target = MakeFilmSection(9);
  target.name = "Other";
  target.cadence = CadenceParameters{};

  const Section mirrored = ApplySectionEditDelta(before, after, target);
  EXPECT_EQ(mirrored.cadence, after.cadence);
  EXPECT_TRUE(mirrored.cadence.phase_specified);
  EXPECT_EQ(mirrored.name, "Other");
}

TEST(CadencePresenterTest, PickingAStillReturnsTheCadenceToVideo) {
  Section section = MakeFilmSection(9);
  section.source = "bars.exr";

  ApplySourceSelectionDefaults("film.mkv", 25, &section);
  EXPECT_EQ(section.cadence, CadenceParameters{});
}

TEST(CadencePresenterTest, AllFramesHintCountsOutputFramesForEachPhase) {
  // Five film frames: A A B B B C C D D D A A (12 fields) from A, and
  // B B B C C D D D A A B B B (13 fields) from B.
  struct Case {
    CadencePhase phase;
    const char* expected;
  };
  const Case cases[] = {
      {CadencePhase::kA, "5 film frames x 1 = 5 → 6 frames (2:3 from A1)"},
      {CadencePhase::kB, "5 film frames x 1 = 5 → 7 frames (2:3 from B1)"},
      {CadencePhase::kC, "5 film frames x 1 = 5 → 6 frames (2:3 from C1)"},
      {CadencePhase::kD, "5 film frames x 1 = 5 → 7 frames (2:3 from D1)"},
  };
  for (const Case& test_case : cases) {
    Section section = MakeFilmSection(4);
    SetCadencePhase(&section, test_case.phase);
    EXPECT_EQ(DurationSummaryFor(section, kUnknownSourceFrameCount,
                                 CadencePhaseStartSlot(test_case.phase)),
              test_case.expected);
  }
}

TEST(CadencePresenterTest, AllFramesHintCountsRepeatsAndMidSlotStarts) {
  Section section = MakeFilmSection(3);
  section.duration_frames_repeat = 2;
  // Eight film frames from A: two full cycles, 20 fields.
  EXPECT_EQ(DurationSummaryFor(section, kUnknownSourceFrameCount, 0),
            "4 film frames x 2 = 8 → 10 frames (2:3 from A1)");

  // A continuing section starting on the second field of B: the first film
  // frame gets the last two B fields, then C C D D D A A (9 fields): 5 frames.
  section.duration_frames_repeat = 1;
  EXPECT_EQ(DurationSummaryFor(section, kUnknownSourceFrameCount, 3),
            "4 film frames x 1 = 4 → 5 frames (2:3 from B2)");
}

TEST(CadencePresenterTest, AllFramesHintWaitsForAnUnknownStartSlot) {
  EXPECT_EQ(DurationSummaryFor(MakeFilmSection(4), kUnknownSourceFrameCount,
                               kUnknownCadenceSlot),
            "5 film frames x 1 = 5 film frames; output frames depend on "
            "where the previous section's cadence ends");
}

TEST(CadencePresenterTest, FixedDurationHintShowsFilmFramesPlayed) {
  Section section = MakeFilmSection(9);
  section.duration_frames_all = false;
  section.duration_frames = 5;
  // Five frames from A are one full cycle: four film frames.
  EXPECT_EQ(DurationSummaryFor(section, kUnknownSourceFrameCount, 0),
            "Plays 4 film frames (2:3 from A1)");

  section.end_frame = 2;
  EXPECT_EQ(DurationSummaryFor(section, kUnknownSourceFrameCount, 0),
            "Plays 4 film frames (2:3 from A1); loops the 3-frame source "
            "range");
}

TEST(CadencePresenterTest, VideoHintsAreUnchanged) {
  Section section = MakeFilmSection(4);
  section.cadence = CadenceParameters{};
  section.duration_frames_repeat = 2;
  EXPECT_EQ(DurationSummaryFor(section, kUnknownSourceFrameCount, 0),
            "5 frames x 2 = 10 total");

  section.duration_frames_all = false;
  section.duration_frames = 8;
  EXPECT_EQ(DurationSummaryFor(section, kUnknownSourceFrameCount, 0),
            "Loops the 5-frame source range");
  section.duration_frames = 5;
  EXPECT_EQ(DurationSummaryFor(section, kUnknownSourceFrameCount, 0), "");
}

}  // namespace
}  // namespace videosynth::gui
