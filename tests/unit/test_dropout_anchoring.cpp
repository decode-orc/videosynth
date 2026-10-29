/*
 * File:        test_dropout_anchoring.cpp
 * Module:      dropout_injection_tests
 * Purpose:     Validates unflagged dropouts (applied but absent from the
 *              sidecar) and disc-anchored dropouts (keyed by disc position
 *              rather than by section and output position).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "videosynth/dropout_injection_stage.h"
#include "videosynth/fixed_point.h"
#include "videosynth/frame_enrichment.h"
#include "videosynth/interfaces.h"
#include "videosynth/model.h"
#include "videosynth/project_validator.h"
#include "videosynth/timing_constants.h"
#include "videosynth/yaml_project_emitter.h"
#include "videosynth/yaml_project_parser.h"

namespace videosynth {
namespace {

using Schedule = std::vector<IGenerationStage::FrameScheduleItem>;

const std::string kPalYamlPrefix =
    "project:\n"
    "  name: DropoutAnchorTest\n"
    "  version: \"1.0\"\n"
    "cvbs_presets:\n"
    "  video_standard_preset: PAL\n"
    "  sample_encoding_preset: CVBS_U10_4FSC\n"
    "  signal_state_preset: STANDARD_STABLE_LOCKED\n"
    "output:\n"
    "  video_path: out.cvbs\n";

Section MakeSection(const std::string& name, int random_scale,
                    int scratch_scale, int duration_frames) {
  Section section;
  section.name = name;
  section.type = "progressive";
  section.source = "f.exr";
  section.duration_frames = duration_frames;
  if (random_scale > 0) {
    section.dropouts.random.enabled = true;
    section.dropouts.random.scale = random_scale;
    section.dropouts.random.seed_specified = true;
    section.dropouts.random.seed = 42;
  }
  if (scratch_scale > 0) {
    section.dropouts.scratch.enabled = true;
    section.dropouts.scratch.scale = scratch_scale;
    section.dropouts.scratch.seed_specified = true;
    section.dropouts.scratch.seed = 7;
  }
  return section;
}

Project MakeProject(std::vector<Section> sections) {
  Project project;
  project.cvbs_presets.video_standard_preset = Standard::kPal;
  project.cvbs_presets.sample_encoding_preset = "CVBS_U10_4FSC";
  project.cvbs_presets.signal_state_preset = "STANDARD_STABLE_LOCKED";
  project.sections = std::move(sections);
  return project;
}

// One schedule item per frame of each section, in order; disc frames run on
// from first_disc_frames[section] within each section.
Schedule MakeSchedule(const Project& project,
                      const std::vector<std::size_t>& first_disc_frames) {
  Schedule schedule;
  for (std::size_t s = 0; s < project.sections.size(); ++s) {
    for (int i = 0; i < project.sections[s].duration_frames; ++i) {
      IGenerationStage::FrameScheduleItem item;
      item.section = &project.sections[s];
      item.source_frame_index = i;
      item.section_frame_index = i;
      auto enrichment = std::make_shared<FrameEnrichment>();
      enrichment->disc_frame_index =
          first_disc_frames[s] + static_cast<std::size_t>(i);
      item.enrichment = std::move(enrichment);
      schedule.push_back(item);
    }
  }
  return schedule;
}

struct FrameResult {
  std::vector<SampleFixed> y;
  std::vector<SampleFixed> c;
  std::vector<DropoutSidecarRow> rows;
};

FrameResult RunFrame(const Project& project, const Schedule& schedule,
                     std::size_t global_frame) {
  const auto samples = static_cast<std::size_t>(
      SamplesPerFrame4fsc(project.cvbs_presets.video_standard_preset));
  FrameResult result{
      .y = std::vector<SampleFixed>(samples, MillivoltsToSampleFixed(350.0)),
      .c = std::vector<SampleFixed>(samples, 0),
      .rows = {}};
  const DropoutInjectionStage stage(nullptr);
  stage.ComputeFrameDropouts(project, schedule, global_frame, &result.y,
                             &result.c, 0U, &result.rows);
  return result;
}

bool SameRows(const std::vector<DropoutSidecarRow>& a,
              const std::vector<DropoutSidecarRow>& b, bool compare_frame_id) {
  if (a.size() != b.size()) {
    return false;
  }
  for (std::size_t i = 0; i < a.size(); ++i) {
    if ((compare_frame_id && a[i].frame_id != b[i].frame_id) ||
        a[i].sample_start != b[i].sample_start ||
        a[i].sample_count != b[i].sample_count ||
        a[i].severity != b[i].severity) {
      return false;
    }
  }
  return true;
}

bool ContainsRow(const std::vector<DropoutSidecarRow>& rows,
                 const DropoutSidecarRow& row) {
  for (const DropoutSidecarRow& candidate : rows) {
    if (candidate.frame_id == row.frame_id &&
        candidate.sample_start == row.sample_start &&
        candidate.sample_count == row.sample_count &&
        candidate.severity == row.severity) {
      return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// flagged
// ---------------------------------------------------------------------------

TEST(DropoutAnchoringTest, UnflaggedDropoutsChangeTheSignalButRecordNoRows) {
  const Project flagged = MakeProject({MakeSection("S", 12, 12, 8)});
  Project unflagged = flagged;
  unflagged.sections[0].dropouts.random.flagged = false;
  unflagged.sections[0].dropouts.scratch.flagged = false;
  const Schedule flagged_schedule = MakeSchedule(flagged, {0});
  const Schedule unflagged_schedule = MakeSchedule(unflagged, {0});

  std::size_t flagged_rows = 0;
  for (std::size_t frame = 0; frame < flagged_schedule.size(); ++frame) {
    const FrameResult a = RunFrame(flagged, flagged_schedule, frame);
    const FrameResult b = RunFrame(unflagged, unflagged_schedule, frame);
    ASSERT_EQ(a.y, b.y) << frame;
    ASSERT_EQ(a.c, b.c) << frame;
    EXPECT_TRUE(b.rows.empty()) << frame;
    flagged_rows += a.rows.size();
  }
  EXPECT_GT(flagged_rows, 0U);
}

TEST(DropoutAnchoringTest, MixedProjectRecordsOnlyTheFlaggedTypesRows) {
  const Project both = MakeProject({MakeSection("S", 12, 12, 8)});
  Project random_only = both;
  random_only.sections[0].dropouts.scratch.flagged = false;
  Project scratch_only = both;
  scratch_only.sections[0].dropouts.random.flagged = false;

  std::size_t random_rows = 0;
  std::size_t scratch_rows = 0;
  for (std::size_t frame = 0; frame < 8U; ++frame) {
    const FrameResult all = RunFrame(both, MakeSchedule(both, {0}), frame);
    const FrameResult random =
        RunFrame(random_only, MakeSchedule(random_only, {0}), frame);
    const FrameResult scratch =
        RunFrame(scratch_only, MakeSchedule(scratch_only, {0}), frame);
    ASSERT_EQ(random.y, all.y);
    ASSERT_EQ(scratch.y, all.y);
    // The two types' rows partition the fully flagged set.
    ASSERT_EQ(random.rows.size() + scratch.rows.size(), all.rows.size());
    for (const DropoutSidecarRow& row : random.rows) {
      EXPECT_TRUE(ContainsRow(all.rows, row));
      EXPECT_FALSE(ContainsRow(scratch.rows, row));
    }
    for (const DropoutSidecarRow& row : scratch.rows) {
      EXPECT_TRUE(ContainsRow(all.rows, row));
    }
    random_rows += random.rows.size();
    scratch_rows += scratch.rows.size();
  }
  EXPECT_GT(random_rows, 0U);
  EXPECT_GT(scratch_rows, 0U);
}

// ---------------------------------------------------------------------------
// anchor
// ---------------------------------------------------------------------------

TEST(DropoutAnchoringTest, DiscAnchoredRandomDropoutsFollowTheDiscFrame) {
  // Layout A: one section on disc frames 0-19. Layout B: a lead-in section,
  // then a section on disc frames 5-14, so the same disc frame sits at a
  // different section index and output position.
  Section a_section = MakeSection("A", 12, 0, 20);
  a_section.dropouts.random.anchor = DropoutAnchor::kDisc;
  const Project layout_a = MakeProject({a_section});
  Section lead_in = MakeSection("LeadIn", 0, 0, 3);
  Section b_section = a_section;
  b_section.name = "B";
  b_section.duration_frames = 10;
  const Project layout_b = MakeProject({lead_in, b_section});
  const Schedule schedule_a = MakeSchedule(layout_a, {0});
  const Schedule schedule_b = MakeSchedule(layout_b, {100, 5});

  std::size_t rows = 0;
  for (std::size_t disc_frame = 5; disc_frame < 15U; ++disc_frame) {
    const FrameResult a = RunFrame(layout_a, schedule_a, disc_frame);
    const FrameResult b = RunFrame(layout_b, schedule_b, disc_frame - 2U);
    ASSERT_EQ(a.y, b.y) << disc_frame;
    ASSERT_EQ(a.c, b.c) << disc_frame;
    EXPECT_TRUE(SameRows(a.rows, b.rows, false)) << disc_frame;
    rows += a.rows.size();
  }
  EXPECT_GT(rows, 0U);

  // Section anchoring keys the same frames by output position instead.
  Project section_a = layout_a;
  section_a.sections[0].dropouts.random.anchor = DropoutAnchor::kSection;
  Project section_b = layout_b;
  section_b.sections[1].dropouts.random.anchor = DropoutAnchor::kSection;
  EXPECT_NE(RunFrame(section_a, MakeSchedule(section_a, {0}), 7U).y,
            RunFrame(section_b, MakeSchedule(section_b, {100, 5}), 5U).y);
}

TEST(DropoutAnchoringTest, DiscAnchoredScratchesRepeatOnAReplay) {
  // A first play and a replay that start on the same disc frame, at
  // different section indices and output positions, carry the same
  // scratches; a section starting elsewhere on the disc does not.
  Section first = MakeSection("First", 0, 12, 12);
  first.dropouts.scratch.anchor = DropoutAnchor::kDisc;
  Section replay = first;
  replay.name = "Replay";
  Section elsewhere = first;
  elsewhere.name = "Elsewhere";
  const Project project = MakeProject({first, elsewhere, replay});
  const Schedule schedule = MakeSchedule(project, {16, 40, 16});

  std::size_t rows = 0;
  bool elsewhere_differs = false;
  for (std::size_t i = 0; i < 12U; ++i) {
    const FrameResult a = RunFrame(project, schedule, i);
    const FrameResult b = RunFrame(project, schedule, 12U + i);
    const FrameResult c = RunFrame(project, schedule, 24U + i);
    ASSERT_EQ(a.y, c.y) << i;
    EXPECT_TRUE(SameRows(a.rows, c.rows, false)) << i;
    elsewhere_differs = elsewhere_differs || (a.y != b.y);
    rows += a.rows.size();
  }
  EXPECT_GT(rows, 0U);
  EXPECT_TRUE(elsewhere_differs);
}

TEST(DropoutAnchoringTest, ExplicitSectionAnchorMatchesTheDefault) {
  const Project by_default = MakeProject({MakeSection("S", 12, 12, 6)});
  Project explicit_section = by_default;
  explicit_section.sections[0].dropouts.random.anchor = DropoutAnchor::kSection;
  explicit_section.sections[0].dropouts.scratch.anchor =
      DropoutAnchor::kSection;
  // Disc frames are deliberately offset: section anchoring ignores them.
  const Schedule schedule_a = MakeSchedule(by_default, {0});
  const Schedule schedule_b = MakeSchedule(explicit_section, {300});
  for (std::size_t frame = 0; frame < 6U; ++frame) {
    const FrameResult a = RunFrame(by_default, schedule_a, frame);
    const FrameResult b = RunFrame(explicit_section, schedule_b, frame);
    ASSERT_EQ(a.y, b.y);
    EXPECT_TRUE(SameRows(a.rows, b.rows, true));
  }
}

// ---------------------------------------------------------------------------
// Parser, emitter and validator
// ---------------------------------------------------------------------------

std::string SectionYaml(const std::string& dropouts) {
  return kPalYamlPrefix +
         "sections:\n"
         "  - name: S\n"
         "    type: progressive\n"
         "    source: f.exr\n"
         "    duration_frames: 40\n"
         "    dropouts:\n" +
         dropouts;
}

TEST(DropoutAnchoringTest, ParserReadsFlaggedAndAnchor) {
  YamlProjectParser parser;
  const ParseResult result =
      parser.ParseString(SectionYaml("      random:\n"
                                     "        scale: 5\n"
                                     "        seed: 3\n"
                                     "        flagged: false\n"
                                     "        anchor: disc\n"
                                     "      scratch:\n"
                                     "        scale: 4\n"
                                     "        anchor: section\n"));
  ASSERT_TRUE(result.errors.empty()) << result.errors[0];
  const DropoutParameters& dropouts = result.project.sections[0].dropouts;
  EXPECT_FALSE(dropouts.random.flagged);
  EXPECT_EQ(dropouts.random.anchor, DropoutAnchor::kDisc);
  EXPECT_TRUE(dropouts.scratch.flagged);
  EXPECT_EQ(dropouts.scratch.anchor, DropoutAnchor::kSection);
}

TEST(DropoutAnchoringTest, ParserRejectsBadAnchorAndFlagged) {
  YamlProjectParser parser;
  const ParseResult bad_anchor =
      parser.ParseString(SectionYaml("      random:\n"
                                     "        scale: 5\n"
                                     "        anchor: pressing\n"));
  ASSERT_FALSE(bad_anchor.errors.empty());
  EXPECT_NE(bad_anchor.errors[0].find("anchor must be 'section' or 'disc'"),
            std::string::npos);
  const ParseResult bad_flag =
      parser.ParseString(SectionYaml("      scratch:\n"
                                     "        scale: 5\n"
                                     "        flagged: maybe\n"));
  ASSERT_FALSE(bad_flag.errors.empty());
  EXPECT_NE(bad_flag.errors[0].find("flagged must be true or false"),
            std::string::npos);
}

TEST(DropoutAnchoringTest, EmitterRoundTripsFlaggedAndAnchor) {
  YamlProjectParser parser;
  const ParseResult parsed =
      parser.ParseString(SectionYaml("      random:\n"
                                     "        scale: 5\n"
                                     "        seed: 3\n"
                                     "        flagged: false\n"
                                     "        anchor: disc\n"
                                     "      scratch:\n"
                                     "        scale: 4\n"
                                     "        seed: 9\n"
                                     "        anchor: disc\n"));
  ASSERT_TRUE(parsed.errors.empty()) << parsed.errors[0];
  const std::string emitted = YamlProjectEmitter().EmitString(parsed.project);
  const ParseResult round_trip = parser.ParseString(emitted);
  ASSERT_TRUE(round_trip.errors.empty()) << round_trip.errors[0];
  EXPECT_EQ(round_trip.project.sections[0].dropouts,
            parsed.project.sections[0].dropouts);
  // Defaults are not written out.
  EXPECT_EQ(emitted.find("anchor: section"), std::string::npos);
  EXPECT_EQ(emitted.find("flagged: true"), std::string::npos);
}

TEST(DropoutAnchoringTest, ValidatorRequiresASeedForDiscAnchoring) {
  YamlProjectParser parser;
  ProjectValidator validator;
  for (const std::string& type : {"random", "scratch"}) {
    const ParseResult unseeded =
        parser.ParseString(SectionYaml("      " + type +
                                       ":\n"
                                       "        scale: 5\n"
                                       "        anchor: disc\n"));
    ASSERT_TRUE(unseeded.errors.empty()) << unseeded.errors[0];
    const ValidationResult rejected = validator.Validate(unseeded.project);
    ASSERT_FALSE(rejected.is_valid) << type;
    EXPECT_NE(rejected.errors[0].find(type + ".anchor 'disc' requires " + type +
                                      ".seed"),
              std::string::npos)
        << rejected.errors[0];

    const ParseResult seeded =
        parser.ParseString(SectionYaml("      " + type +
                                       ":\n"
                                       "        scale: 5\n"
                                       "        seed: 11\n"
                                       "        anchor: disc\n"));
    ASSERT_TRUE(seeded.errors.empty()) << seeded.errors[0];
    const ValidationResult accepted = validator.Validate(seeded.project);
    EXPECT_TRUE(accepted.is_valid)
        << type << ": " << (accepted.errors.empty() ? "" : accepted.errors[0]);
  }
}

}  // namespace
}  // namespace videosynth
