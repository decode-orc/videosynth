/*
 * File:        test_validation_issues_model.cpp
 * Module:      gui_tests
 * Purpose:     Unit tests for issue building and the issues list model
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "validation_issues_model.h"
#include "videosynth/model.h"
#include "videosynth/project_validator.h"
#include "videosynth/results.h"

namespace videosynth::gui {
namespace {

Project MakeTwoSectionProject() {
  Project project;
  Section first;
  first.name = "Bars";
  project.sections.push_back(first);
  Section second;
  second.name = "Ramp";
  project.sections.push_back(second);
  return project;
}

TEST(ValidationIssuesModelTest, BuildMapsErrorsAndWarningsWithSeverity) {
  ValidationResult result;
  result.errors.push_back("Project configuration error: broken.");
  result.warnings.push_back("Something looks odd.");

  const std::vector<ValidationIssue> issues =
      BuildValidationIssues(MakeTwoSectionProject(), result);

  ASSERT_EQ(issues.size(), 2U);
  EXPECT_EQ(issues[0].severity, ValidationIssue::Severity::kError);
  EXPECT_EQ(issues[0].message,
            QStringLiteral("Project configuration error: broken."));
  EXPECT_EQ(issues[1].severity, ValidationIssue::Severity::kWarning);
}

TEST(ValidationIssuesModelTest, BuildAttributesSectionByQuotedName) {
  ValidationResult result;
  result.errors.push_back("Noise validation error in section 'Ramp': bad.");
  result.errors.push_back("Project configuration error: no section named.");

  const std::vector<ValidationIssue> issues =
      BuildValidationIssues(MakeTwoSectionProject(), result);

  ASSERT_EQ(issues.size(), 2U);
  EXPECT_EQ(issues[0].section_index, 1);
  EXPECT_EQ(issues[1].section_index, -1);
}

TEST(ValidationIssuesModelTest, ModelExposesRolesAndCounts) {
  ValidationResult result;
  result.errors.push_back("Error in section 'Bars': broken.");
  result.warnings.push_back("Warning one.");
  result.warnings.push_back("Warning two.");

  ValidationIssuesModel model;
  model.SetIssues(BuildValidationIssues(MakeTwoSectionProject(), result));

  ASSERT_EQ(model.rowCount(), 3);
  EXPECT_EQ(model.error_count(), 1);
  EXPECT_EQ(model.warning_count(), 2);

  const QModelIndex first = model.index(0);
  EXPECT_EQ(first.data(Qt::DisplayRole).toString(),
            QStringLiteral("Error in section 'Bars': broken."));
  EXPECT_EQ(first.data(ValidationIssuesModel::kSeverityRole).toInt(),
            static_cast<int>(ValidationIssue::Severity::kError));
  EXPECT_EQ(first.data(ValidationIssuesModel::kSectionIndexRole).toInt(), 0);
}

TEST(ValidationIssuesModelTest, SetIssuesReplacesRows) {
  ValidationIssuesModel model;

  ValidationResult result;
  result.errors.push_back("Broken.");
  model.SetIssues(BuildValidationIssues(Project{}, result));
  ASSERT_EQ(model.rowCount(), 1);

  model.SetIssues({});
  EXPECT_EQ(model.rowCount(), 0);
  EXPECT_EQ(model.error_count(), 0);
  EXPECT_EQ(model.warning_count(), 0);
}

// A structurally valid PAL project whose second section, 'Ramp', is the one
// the tests break.
Project MakeValidProject() {
  Project project;
  project.cvbs_presets.video_standard_preset = Standard::kPal;
  project.output.video_path = "out/video.cvbs";
  project.output.metadata_path = "out/video.meta";
  for (const char* name : {"Bars", "Ramp"}) {
    Section section;
    section.name = name;
    section.type = "progressive";
    section.source = std::string("assets/") + name + ".exr";
    section.duration_frames = 10;
    project.sections.push_back(section);
  }
  return project;
}

// Validates `project` and returns the issue built from its first error.
ValidationIssue FirstIssue(const Project& project) {
  const ValidationResult result = ProjectValidator().Validate(project);
  EXPECT_FALSE(result.is_valid);
  const std::vector<ValidationIssue> issues =
      BuildValidationIssues(project, result);
  EXPECT_FALSE(issues.empty());
  return issues.empty() ? ValidationIssue{} : issues.front();
}

TEST(ValidationIssuesModelTest, ImpairmentIssuesNavigateToTheirField) {
  struct Case {
    const char* label;
    void (*breakage)(Project*);
    int section_index;
    IssueField field;
  };
  const Case cases[] = {
      {"capture noise",
       [](Project* p) {
         p->sections[1].noise.enabled = true;
         p->sections[1].noise.noise_db = 48.0;
         p->sections[1].noise.spectrum = {{2.0, 0.0}, {1.0, 0.0}};
       },
       1, IssueField::kCaptureNoise},
      {"capture noise spread spectrum",
       [](Project* p) {
         p->sections[1].noise.enabled = true;
         p->sections[1].noise.noise_db = 48.0;
         p->sections[1].noise.spread_spectrum = {{0.0, 80.0}};
       },
       1, IssueField::kCaptureNoise},
      {"shared noise seed",
       [](Project* p) {
         p->sections[1].noise.shared.enabled = true;
         p->sections[1].noise.shared.noise_db = 48.0;
       },
       1, IssueField::kSharedNoise},
      {"shared noise spectrum",
       [](Project* p) {
         SharedNoiseParameters& shared = p->sections[1].noise.shared;
         shared.enabled = true;
         shared.noise_db = 48.0;
         shared.noise_seed_specified = true;
         shared.spectrum = {{0.0, 80.0}};
       },
       1, IssueField::kSharedNoise},
      {"random disc anchor",
       [](Project* p) {
         p->sections[1].dropouts.random.enabled = true;
         p->sections[1].dropouts.random.scale = 3;
         p->sections[1].dropouts.random.anchor = DropoutAnchor::kDisc;
       },
       1, IssueField::kRandomDropouts},
      {"scratch disc anchor",
       [](Project* p) {
         p->sections[1].dropouts.scratch.enabled = true;
         p->sections[1].dropouts.scratch.scale = 3;
         p->sections[1].dropouts.scratch.anchor = DropoutAnchor::kDisc;
       },
       1, IssueField::kScratchDropouts},
      {"luma bandwidth",
       [](Project* p) {
         p->sections[1].luma_bandwidth.enabled = true;
         p->sections[1].luma_bandwidth.cutoff_mhz = 0.2;
       },
       1, IssueField::kLumaBandwidth},
      {"channel",
       [](Project* p) {
         p->channel.enabled = true;
         p->channel.timing_offset_samples = 9.0;
       },
       -1, IssueField::kChannel},
      {"fm beat on NTSC",
       [](Project* p) {
         p->cvbs_presets.video_standard_preset = Standard::kNtsc;
         for (Section& section : p->sections) {
           section.source = "assets/ntsc.exr";
         }
         p->channel.enabled = true;
         p->channel.fm_beat.enabled = true;
       },
       -1, IssueField::kChannel},
      {"chroma error",
       [](Project* p) {
         p->chroma_error.enabled = true;
         p->chroma_error.gain = 2.0;
       },
       -1, IssueField::kChromaError},
  };
  for (const Case& test_case : cases) {
    SCOPED_TRACE(test_case.label);
    Project project = MakeValidProject();
    ASSERT_TRUE(ProjectValidator().Validate(project).is_valid);
    test_case.breakage(&project);
    const ValidationIssue issue = FirstIssue(project);
    EXPECT_EQ(issue.section_index, test_case.section_index)
        << issue.message.toStdString();
    EXPECT_EQ(issue.field, test_case.field) << issue.message.toStdString();
  }
}

TEST(ValidationIssuesModelTest, FieldClassificationIgnoresTheSectionName) {
  EXPECT_EQ(ClassifyIssueField("Noise validation error in section "
                               "'shared.intro': noise_db must be in range."),
            IssueField::kCaptureNoise);
  EXPECT_EQ(ClassifyIssueField("Dropout validation error in section "
                               "'scratch.x': random.scale must be in [0, 20]."),
            IssueField::kRandomDropouts);
  EXPECT_EQ(ClassifyIssueField("Progressive section validation error: source "
                               "must be set."),
            IssueField::kNone);
}

TEST(ValidationIssuesModelTest, ModelExposesTheFieldRole) {
  ValidationIssuesModel model;
  ValidationIssue issue;
  issue.field = IssueField::kLumaBandwidth;
  model.SetIssues({issue});
  EXPECT_EQ(
      model.data(model.index(0), ValidationIssuesModel::kFieldRole).toInt(),
      static_cast<int>(IssueField::kLumaBandwidth));
}

}  // namespace
}  // namespace videosynth::gui
