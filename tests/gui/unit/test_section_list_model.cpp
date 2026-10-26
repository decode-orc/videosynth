/*
 * File:        test_section_list_model.cpp
 * Module:      gui_tests
 * Purpose:     Unit tests for the section list model, templates, and the
 *              reorder/duplicate document operations
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <QItemSelectionModel>
#include <string>

#include "project_document.h"
#include "project_templates.h"
#include "section_list_model.h"
#include "videosynth/model.h"
#include "videosynth/project_validator.h"
#include "videosynth/yaml_project_emitter.h"

namespace videosynth::gui {
namespace {

Section MakeSection(const std::string& name, int duration_frames) {
  Section section;
  section.name = name;
  section.type = "progressive";
  section.source = "assets/" + name + ".exr";
  section.duration_frames = duration_frames;
  return section;
}

Project MakeProject() {
  Project project;
  project.name = "Test";
  project.cvbs_presets.video_standard_preset = Standard::kPal;
  project.output.video_path = "out/video.cvbs";
  project.output.metadata_path = "out/video.meta";
  project.sections.push_back(MakeSection("First", 100));
  project.sections.push_back(MakeSection("Second", 50));
  project.sections.push_back(MakeSection("Third", 25));
  return project;
}

TEST(SectionListModelTest, RowsRecalculateStartFrames) {
  const std::vector<SectionListRow> rows = BuildSectionListRows(MakeProject());
  ASSERT_EQ(rows.size(), 3U);
  EXPECT_EQ(rows[0].start_frame, 0);
  EXPECT_EQ(rows[1].start_frame, 100);
  EXPECT_EQ(rows[2].start_frame, 150);
  EXPECT_EQ(rows[1].DurationText(), QStringLiteral("50 frames"));
}

TEST(SectionListModelTest, AllDurationRowShowsAllFrames) {
  Project project = MakeProject();
  project.sections[1].duration_frames_all = true;
  project.sections[1].duration_frames = 0;
  const std::vector<SectionListRow> rows = BuildSectionListRows(project);
  EXPECT_EQ(rows[1].DurationText(), QStringLiteral("all frames"));
}

TEST(SectionListModelTest, AllDurationRowWithRepeatShowsMultiplier) {
  Project project = MakeProject();
  project.sections[1].duration_frames_all = true;
  project.sections[1].duration_frames = 0;
  project.sections[1].duration_frames_repeat = 4;
  const std::vector<SectionListRow> rows = BuildSectionListRows(project);
  EXPECT_EQ(rows[1].DurationText(), QStringLiteral("all frames x4"));
}

TEST(SectionListModelTest, ClosedRangeRowCountsRangeTimesRepeat) {
  Project project = MakeProject();
  Section& section = project.sections[1];
  section.source = "assets/capture.mp4";
  section.duration_frames_all = true;
  section.duration_frames = 0;
  section.start_frame = 10;
  section.end_frame = 19;
  section.end_frame_specified = true;
  section.duration_frames_repeat = 3;
  const std::vector<SectionListRow> rows = BuildSectionListRows(project);
  EXPECT_EQ(rows[1].output_frames, 30);
  EXPECT_EQ(rows[1].DurationText(), QStringLiteral("all 10 frames x3"));
  // The following section starts after the 30 resolved frames.
  EXPECT_EQ(rows[2].start_frame, 130);
}

TEST(SectionListModelTest, OpenEndedAllRowStaysUnknown) {
  Project project = MakeProject();
  Section& section = project.sections[1];
  section.source = "assets/capture.mp4";
  section.duration_frames_all = true;
  section.duration_frames = 0;
  section.start_frame = 10;
  const std::vector<SectionListRow> rows = BuildSectionListRows(project);
  EXPECT_EQ(rows[1].output_frames, -1);
  EXPECT_EQ(rows[1].DurationText(), QStringLiteral("all frames"));
  EXPECT_EQ(rows[2].start_frame, 100);
}

TEST(SectionListModelTest, FrameRangeTextFormatsKnownAndUnknownEnd) {
  EXPECT_EQ(FrameRangeText(0, 249), QStringLiteral("0 – 249"));
  EXPECT_EQ(FrameRangeText(100, 100), QStringLiteral("100 – 100"));
  // End below start = unresolved "all source frames" duration.
  EXPECT_EQ(FrameRangeText(150, 149), QStringLiteral("150 – ?"));
}

TEST(SectionListModelTest, DiscRangeTitleNamesTheDiscFormat) {
  EXPECT_EQ(DiscRangeTitle(DiscType::kCAV),
            QStringLiteral("CAV picture numbers:"));
  EXPECT_EQ(DiscRangeTitle(DiscType::kCLV), QStringLiteral("CLV timecode:"));
  EXPECT_TRUE(DiscRangeTitle(DiscType::kUnknown).isEmpty());
}

TEST(SectionListModelTest, CavDiscRangeShowsOneBasedPictureNumbers) {
  // Disc frame offset 0 = picture number 00001, five digits.
  EXPECT_EQ(DiscRangeText(DiscType::kCAV, Standard::kPal, 0, 249),
            QStringLiteral("00001 – 00250"));
  EXPECT_EQ(DiscRangeText(DiscType::kCAV, Standard::kPal, 150, 149),
            QStringLiteral("00151 – ?"));
}

TEST(SectionListModelTest, ClvDiscRangeShowsProgrammeTimecodes) {
  // PAL: 25 fps — offset 250 is 00:00:10:00; NTSC: 30 fps.
  EXPECT_EQ(DiscRangeText(DiscType::kCLV, Standard::kPal, 0, 250),
            QStringLiteral("00:00:00:00 – 00:00:10:00"));
  EXPECT_EQ(DiscRangeText(DiscType::kCLV, Standard::kNtsc, 30, 3629),
            QStringLiteral("00:00:01:00 – 00:02:00:29"));
  EXPECT_EQ(DiscRangeText(DiscType::kCLV, Standard::kPal, 150, 149),
            QStringLiteral("00:00:06:00 – ?"));
}

TEST(SectionListModelTest, DiscRangeIsEmptyForNonLaserdiscProjects) {
  EXPECT_TRUE(
      DiscRangeText(DiscType::kUnknown, Standard::kPal, 0, 100).isEmpty());
}

TEST(SectionListModelTest, DiscFrameOffsetsAnchorAtProgrammeAreaStart) {
  // lead_in (100) → programme (50) → programme (25) → lead_out.
  Project project = MakeProject();
  project.sections[0].section_type = SectionType::kLeadIn;
  project.sections[1].section_type = SectionType::kProgrammeArea;
  project.sections[2].section_type = SectionType::kProgrammeArea;
  project.sections.push_back(MakeSection("Out", 10));
  project.sections[3].section_type = SectionType::kLeadOut;

  const std::vector<int> offsets =
      BuildDiscFrameOffsets(project, DiscType::kCAV);
  ASSERT_EQ(offsets.size(), 4U);
  // IEC 60856/60857: lead-in and lead-out carry no picture numbers, and the
  // lead-in's 100 frames are not counted — numbering starts at the first
  // programme_area section.
  EXPECT_EQ(offsets[0], -1);
  EXPECT_EQ(offsets[1], 0);
  EXPECT_EQ(offsets[2], 50);
  EXPECT_EQ(offsets[3], -1);
}

TEST(SectionListModelTest, DiscFrameOffsetsSkipUntypedSectionsBeforeAnchor) {
  // Untyped sections before the programme area are not counted; an untyped
  // section between programme sections still occupies disc frames (the
  // engine's timekeeping generators persist across section boundaries).
  Project project = MakeProject();
  project.sections[1].section_type = SectionType::kProgrammeArea;
  project.sections.push_back(MakeSection("Fourth", 10));
  project.sections[3].section_type = SectionType::kProgrammeArea;

  const std::vector<int> offsets =
      BuildDiscFrameOffsets(project, DiscType::kCLV);
  ASSERT_EQ(offsets.size(), 4U);
  EXPECT_EQ(offsets[0], -1);
  EXPECT_EQ(offsets[1], 0);
  EXPECT_EQ(offsets[2], -1);  // Untyped: no disc position of its own...
  EXPECT_EQ(offsets[3], 75);  // ...but its 25 frames stay in the count.
}

TEST(SectionListModelTest, DiscFrameOffsetsHonourCavStartValueAnchor) {
  Project project = MakeProject();
  project.sections[0].section_type = SectionType::kProgrammeArea;
  project.sections[1].section_type = SectionType::kProgrammeArea;
  Section::LineInjectionCode code;
  code.code_type = "picture_number";
  code.start_value = 1000;
  code.start_value_specified = true;
  Section::LineInjection injection;
  injection.type = "laserdisc";
  injection.codes.push_back(code);
  project.sections[1].line_injections.push_back(injection);

  const std::vector<int> offsets =
      BuildDiscFrameOffsets(project, DiscType::kCAV);
  EXPECT_EQ(offsets[0], 0);
  // Explicit start_value 1000 re-anchors: offset 999 = picture 01000.
  EXPECT_EQ(offsets[1], 999);
  EXPECT_EQ(offsets[2], -1);
}

TEST(SectionListModelTest, DiscFrameOffsetsAllUnknownForNonLaserdisc) {
  Project project = MakeProject();
  project.sections[0].section_type = SectionType::kProgrammeArea;
  const std::vector<int> offsets =
      BuildDiscFrameOffsets(project, DiscType::kUnknown);
  EXPECT_EQ(offsets, std::vector<int>({-1, -1, -1}));
}

TEST(SectionListModelTest, ModelTracksDocumentMutations) {
  ProjectDocument document;
  document.ResetProject(MakeProject(), QString());
  SectionListModel model(&document);

  ASSERT_EQ(model.rowCount(), 3);
  EXPECT_EQ(
      model.data(model.index(0, SectionListModel::kNameColumn), Qt::DisplayRole)
          .toString(),
      QStringLiteral("First"));

  document.InsertSection(-1, MakeSection("Fourth", 10));
  ASSERT_EQ(model.rowCount(), 4);

  document.RemoveSection(0);
  ASSERT_EQ(model.rowCount(), 3);
  EXPECT_EQ(
      model.data(model.index(0, SectionListModel::kNameColumn), Qt::DisplayRole)
          .toString(),
      QStringLiteral("Second"));

  document.MoveSection(0, 2);
  EXPECT_EQ(
      model.data(model.index(2, SectionListModel::kNameColumn), Qt::DisplayRole)
          .toString(),
      QStringLiteral("Second"));
  // Start frames recalculate after the move.
  EXPECT_EQ(model
                .data(model.index(0, SectionListModel::kStartFrameColumn),
                      Qt::DisplayRole)
                .toInt(),
            0);

  Section renamed = document.project().sections[0];
  renamed.name = "Renamed";
  document.SetSection(0, renamed);
  EXPECT_EQ(
      model.data(model.index(0, SectionListModel::kNameColumn), Qt::DisplayRole)
          .toString(),
      QStringLiteral("Renamed"));

  document.ResetProject(MakeProject(), QString());
  EXPECT_EQ(model.rowCount(), 3);
}

// Editing a section must not reset the model: a reset clears the dock's
// selection, which in turn closes the section editor (the newly-ticked block
// disappears until the section is re-selected).
TEST(SectionListModelTest, EditKeepsSelectionAndDoesNotReset) {
  ProjectDocument document;
  document.ResetProject(MakeProject(), QString());
  SectionListModel model(&document);

  QItemSelectionModel selection(&model);
  selection.setCurrentIndex(
      model.index(1, SectionListModel::kNameColumn),
      QItemSelectionModel::SelectCurrent | QItemSelectionModel::Rows);
  ASSERT_EQ(selection.currentIndex().row(), 1);

  Section edited = document.project().sections[1];
  edited.noise.enabled = true;
  edited.noise.noise_db = 48.0;
  document.SetSection(1, edited);

  // A model reset would invalidate the current index; an in-place refresh keeps
  // it, so the section editor stays open on the row just edited.
  EXPECT_TRUE(selection.currentIndex().isValid());
  EXPECT_EQ(selection.currentIndex().row(), 1);
}

// Reordering through the document must emit YAML identical to a hand-written
// project with the sections in the new order.
TEST(SectionListModelTest, ReorderEmitsYamlIdenticalToHandWritten) {
  ProjectDocument document;
  document.ResetProject(MakeProject(), QString());
  document.MoveSection(0, 2);

  Project expected = MakeProject();
  Section first = expected.sections[0];
  expected.sections.erase(expected.sections.begin());
  expected.sections.push_back(first);

  const YamlProjectEmitter emitter;
  EXPECT_EQ(emitter.EmitString(document.project()),
            emitter.EmitString(expected));
}

TEST(SectionListModelTest, MoveUpPlanShiftsEveryUnpinnedSelectedRow) {
  // A contiguous block away from the top shifts as one; application order is
  // ascending so each step's indices stay valid.
  EXPECT_EQ(PlanMoveSectionsUp({2, 3}),
            (std::vector<SectionMoveStep>{{2, 1}, {3, 2}}));
  // Rows packed against the top stay put; the rest still move.
  EXPECT_EQ(PlanMoveSectionsUp({0, 2}), (std::vector<SectionMoveStep>{{2, 1}}));
  EXPECT_EQ(PlanMoveSectionsUp({0, 1, 3}),
            (std::vector<SectionMoveStep>{{3, 2}}));
  // A block already at the top cannot move at all (Up button disables).
  EXPECT_TRUE(PlanMoveSectionsUp({0, 1}).empty());
  EXPECT_TRUE(PlanMoveSectionsUp({}).empty());
}

TEST(SectionListModelTest, MoveDownPlanShiftsEveryUnpinnedSelectedRow) {
  // Descending application order keeps each step's indices valid.
  EXPECT_EQ(PlanMoveSectionsDown({1, 2}, 4),
            (std::vector<SectionMoveStep>{{2, 3}, {1, 2}}));
  // Rows packed against the bottom stay put; the rest still move.
  EXPECT_EQ(PlanMoveSectionsDown({1, 3}, 4),
            (std::vector<SectionMoveStep>{{1, 2}}));
  // A block already at the bottom cannot move at all (Down button disables).
  EXPECT_TRUE(PlanMoveSectionsDown({2, 3}, 4).empty());
  EXPECT_TRUE(PlanMoveSectionsDown({}, 4).empty());
}

TEST(SectionListModelTest, MultiRowMovePlanReordersDocumentAsABlock) {
  // Sections {Second, Third} move down one place: the block hops over the
  // section below it and keeps its internal order.
  ProjectDocument document;
  Project project = MakeProject();
  project.sections.push_back(MakeSection("Fourth", 10));
  document.ResetProject(project, QString());

  for (const SectionMoveStep& step : PlanMoveSectionsDown({1, 2}, 4)) {
    document.MoveSection(step.from, step.to);
  }
  EXPECT_EQ(document.project().sections[0].name, "First");
  EXPECT_EQ(document.project().sections[1].name, "Fourth");
  EXPECT_EQ(document.project().sections[2].name, "Second");
  EXPECT_EQ(document.project().sections[3].name, "Third");

  // And back up again restores the original order.
  for (const SectionMoveStep& step : PlanMoveSectionsUp({2, 3})) {
    document.MoveSection(step.from, step.to);
  }
  EXPECT_EQ(document.project().sections[1].name, "Second");
  EXPECT_EQ(document.project().sections[2].name, "Third");
  EXPECT_EQ(document.project().sections[3].name, "Fourth");
}

TEST(SectionListModelTest, DuplicateEmitsYamlIdenticalToHandWritten) {
  ProjectDocument document;
  document.ResetProject(MakeProject(), QString());
  const Project& project = document.project();
  document.InsertSection(
      1, MakeDuplicateSection(project.sections[0], project.sections));

  Project expected = MakeProject();
  Section copy = expected.sections[0];
  copy.name = "First (copy)";
  expected.sections.insert(expected.sections.begin() + 1, copy);

  const YamlProjectEmitter emitter;
  EXPECT_EQ(emitter.EmitString(document.project()),
            emitter.EmitString(expected));
}

TEST(SectionListModelTest, DuplicateNamesStayUnique) {
  Project project = MakeProject();
  const Section copy1 =
      MakeDuplicateSection(project.sections[0], project.sections);
  EXPECT_EQ(copy1.name, "First (copy)");
  project.sections.push_back(copy1);
  const Section copy2 =
      MakeDuplicateSection(project.sections[0], project.sections);
  EXPECT_EQ(copy2.name, "First (copy 2)");
}

// Every typed section template must produce a structurally valid project
// for its standard (warnings allowed; errors not). The untyped progressive
// template is validated in its own project: section ordering forbids
// untyped sections inside a lead_in/lead_out disc structure.
TEST(SectionListModelTest, SectionTemplatesValidateStructurally) {
  for (const Standard standard : {Standard::kPal, Standard::kNtsc}) {
    Project project;
    project.cvbs_presets.video_standard_preset = standard;
    project.output.video_path = "out/video.cvbs";
    project.output.metadata_path = "out/video.meta";
    project.line_injections = MakeLaserdiscLineInjections(standard);
    project.sections.push_back(MakeLaserdiscLeadInSectionTemplate(standard));
    project.sections.push_back(MakeLaserdiscProgrammeSectionTemplate(standard));
    project.sections.push_back(MakeLaserdiscLeadOutSectionTemplate(standard));

    ProjectValidator validator;
    const ValidationResult result = validator.Validate(project);
    EXPECT_TRUE(result.is_valid)
        << "standard " << StandardToString(standard) << ": "
        << (result.errors.empty() ? "" : result.errors.front());

    Project plain_project;
    plain_project.cvbs_presets.video_standard_preset = standard;
    plain_project.output.video_path = "out/video.cvbs";
    plain_project.output.metadata_path = "out/video.meta";
    plain_project.sections.push_back(
        MakeProgressiveSectionTemplate(1, standard));

    const ValidationResult plain_result = validator.Validate(plain_project);
    EXPECT_TRUE(plain_result.is_valid)
        << "standard " << StandardToString(standard) << ": "
        << (plain_result.errors.empty() ? "" : plain_result.errors.front());
  }
}

// New sections must default to the bundled colour-bar source for the
// standard's active raster, not a non-existent local file path.
TEST(SectionListModelTest, SectionTemplatesDefaultToBundledColourBar) {
  for (const Standard standard : {Standard::kPal, Standard::kNtsc}) {
    const std::string expected = DefaultBundledSource(standard);
    EXPECT_EQ(MakeProgressiveSectionTemplate(1, standard).source, expected);
    EXPECT_EQ(MakeLaserdiscLeadInSectionTemplate(standard).source, expected);
    EXPECT_EQ(MakeLaserdiscProgrammeSectionTemplate(standard).source, expected);
    EXPECT_EQ(MakeLaserdiscLeadOutSectionTemplate(standard).source, expected);
  }
}

}  // namespace
}  // namespace videosynth::gui
