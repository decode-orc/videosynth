/*
 * File:        test_film_fixture_projects.cpp
 * Module:      fixture_project_tests
 * Purpose:     Checks the 2:3 pulldown fixture projects' per-field film
 *              frames, CAV picture numbers, white flags and OSD text against
 *              the cadence truth derived from FilmCadence.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "fixture_paths.h"
#include "videosynth/cav_code_generator.h"
#include "videosynth/film_cadence.h"
#include "videosynth/frame_enrichment.h"
#include "videosynth/generation_stage.h"
#include "videosynth/path_resolution.h"
#include "videosynth/progressive_frame_source_probe.h"
#include "videosynth/project_validator.h"
#include "videosynth/yaml_project_parser.h"

namespace videosynth {
namespace {

class NullLogger final : public ILogger {
 public:
  void Info(const std::string&) override {}
  void Warning(const std::string&) override {}
  void Error(const std::string&) override {}
  void Debug(const std::string&) override {}
  void Trace(const std::string&) override {}
};

// NTSC lines carrying the codes checked here, for field 1 and field 2: the
// white flag (IEC 60857 §10.2.4) and the second copy of the 24-bit picture
// number (§10.1.3).
constexpr int kWhiteFlagLine[2] = {11, 274};
constexpr int kPictureNumberLine[2] = {18, 281};

// What a fixture section plays, written out from the fixture's own header so
// the test does not reuse the code under test to size the sections.
struct ExpectedSection {
  std::string name;
  // nullopt for a video section, whose frames show `first_source_frame` +
  // frame index in both fields (always frame 0 for a still).
  std::optional<int> start_slot;
  bool still = false;
  int first_source_frame = 0;
  // Film frames in the selected range (2:3 sections).
  int range_frames = 0;
  // True for duration_frames: all, which shows every film frame of the range
  // in full and pads an odd field count with the last film frame.
  bool plays_all = false;
  int output_frames = 0;
};

// One field's expected film frame and cadence position.
struct ExpectedField {
  int source_frame = 0;
  bool starts_picture = true;
  // '\0' for a video section. The padding field of an odd count carries on
  // its film frame's letter (B4 after B3).
  char letter = '\0';
  int field_in_letter = 0;
};

std::vector<ExpectedField> ExpectedFields(const ExpectedSection& section) {
  std::vector<ExpectedField> fields;
  const std::int64_t field_count = 2LL * section.output_frames;
  if (!section.start_slot.has_value()) {
    for (std::int64_t field = 0; field < field_count; ++field) {
      fields.push_back({.source_frame = section.still
                                            ? 0
                                            : section.first_source_frame +
                                                  static_cast<int>(field / 2),
                        .starts_picture = field % 2 == 0});
    }
    return fields;
  }

  const int slot = *section.start_slot;
  const std::int64_t shown_fields =
      section.plays_all ? CadenceFieldsForFilmFrames(slot, section.range_frames)
                        : field_count;
  for (std::int64_t field = 0; field < field_count; ++field) {
    if (field >= shown_fields) {
      // The padding field repeats the last film frame without starting a
      // picture.
      ExpectedField padding = fields.back();
      padding.starts_picture = false;
      ++padding.field_in_letter;
      fields.push_back(padding);
      continue;
    }
    const CadenceField cadence = CadenceFieldAt(slot, field);
    fields.push_back(
        {.source_frame =
             section.first_source_frame +
             static_cast<int>(cadence.film_frame_offset % section.range_frames),
         .starts_picture = cadence.starts_picture,
         .letter = cadence.letter,
         .field_in_letter = cadence.field_in_slot});
  }
  return fields;
}

bool LoadFixture(const std::string& relative, Project* project) {
  const std::filesystem::path path =
      test_support::ProjectFixtureRoot() / relative;
  const ParseResult parsed = YamlProjectParser().ParseFile(path.string());
  if (!parsed.ok) {
    ADD_FAILURE() << relative << ": "
                  << (parsed.errors.empty() ? "parse failed"
                                            : parsed.errors[0]);
    return false;
  }
  AssetRootMap roots = DefaultAssetRoots();
  roots.roots["output"] =
      (std::filesystem::temp_directory_path() / "videosynth_film_fixtures")
          .string();
  *project =
      ResolveProjectPaths(parsed.project, roots, path.parent_path().string(),
                          /*anchor_unset=*/true);
  return true;
}

const VbiLineInjection* FindLine(const FrameEnrichment& enrichment, int line) {
  for (const VbiLineInjection& injection : enrichment.vbi_lines) {
    if (injection.line_number_1based == line) {
      return &injection;
    }
  }
  return nullptr;
}

std::string CadenceToken(const ExpectedField& field) {
  if (field.letter == '\0') {
    return "-";
  }
  return std::string(1, field.letter) + std::to_string(field.field_in_letter);
}

// Validates the fixture against its sources, builds its schedule and checks
// every field against `sections`, in order.
void CheckFixture(const std::string& relative,
                  const std::vector<ExpectedSection>& sections) {
  SCOPED_TRACE(relative);
  Project project;
  ASSERT_TRUE(LoadFixture(relative, &project));

  NullLogger logger;
  ProgressiveFrameSourceProbe probe;
  const ValidationResult validation =
      ProjectValidator(&probe, &logger).Validate(project);
  for (const std::string& error : validation.errors) {
    ADD_FAILURE() << "Validation error: " << error;
  }
  for (const std::string& warning : validation.warnings) {
    ADD_FAILURE() << "Validation warning: " << warning;
  }
  ASSERT_TRUE(validation.is_valid);

  GenerationStage generation(&logger);
  std::vector<IGenerationStage::FrameScheduleItem> schedule;
  std::vector<std::string> errors;
  ASSERT_TRUE(generation.BuildFrameSchedule(project, &schedule, &errors))
      << (errors.empty() ? "" : errors.front());

  ASSERT_EQ(project.sections.size(), sections.size());
  std::size_t frame = 0;
  int picture_number = 0;
  for (std::size_t s = 0; s < sections.size(); ++s) {
    const ExpectedSection& expected = sections[s];
    SCOPED_TRACE("section " + expected.name);
    ASSERT_EQ(project.sections[s].name, expected.name);
    const std::vector<ExpectedField> fields = ExpectedFields(expected);
    ASSERT_LE(frame + expected.output_frames, schedule.size());

    for (int f = 0; f < expected.output_frames; ++f, ++frame) {
      const IGenerationStage::FrameScheduleItem& item = schedule[frame];
      ASSERT_EQ(item.section, &project.sections[s]) << "frame " << frame;
      ASSERT_NE(item.enrichment, nullptr);
      for (int field = 0; field < 2; ++field) {
        SCOPED_TRACE("frame " + std::to_string(frame) + " field " +
                     std::to_string(field + 1));
        const ExpectedField& want = fields[(2 * f) + field];
        if (want.starts_picture) {
          ++picture_number;
        }

        if (expected.start_slot.has_value()) {
          const IGenerationStage::FieldPicture& got = item.fields[field];
          EXPECT_EQ(got.source_frame_index, want.source_frame);
          EXPECT_EQ(got.starts_picture, want.starts_picture);
          EXPECT_EQ(got.cadence_letter, want.letter);
          EXPECT_EQ(got.cadence_field_in_letter, want.field_in_letter);
        } else {
          EXPECT_EQ(item.source_frame_index, want.source_frame);
        }

        // Picture numbers count pictures across every section, film or
        // video; the white flag and the 24-bit number open each picture.
        const FrameEnrichment& enrichment = *item.enrichment;
        EXPECT_EQ(enrichment.field_picture_numbers[field], picture_number);
        const VbiLineInjection* flag =
            FindLine(enrichment, kWhiteFlagLine[field]);
        EXPECT_EQ(
            flag != nullptr && flag->kind == VbiLineInjection::Kind::kWhiteFlag,
            want.starts_picture);
        const VbiLineInjection* number =
            FindLine(enrichment, kPictureNumberLine[field]);
        ASSERT_NE(number, nullptr);
        EXPECT_EQ(
            number->biphase_code ==
                CavPictureNumberGenerator::EncodePictureNumber(picture_number),
            want.starts_picture);

        // The OSD names the field's film frame and cadence slot.
        const std::string prefix = "FILM:" + std::to_string(want.source_frame) +
                                   " " + CadenceToken(want) + " ";
        ASSERT_EQ(enrichment.field_osd_texts[field].size(), 1U);
        EXPECT_EQ(enrichment.field_osd_texts[field][0].rfind(prefix, 0), 0U)
            << enrichment.field_osd_texts[field][0];
      }
    }
  }
  EXPECT_EQ(frame, schedule.size());
}

TEST(FilmFixtureProjectsTest, PulldownFixturePlaysTheWholeClipFromPhaseA) {
  // 240 film frames from phase A: 600 fields, 300 frames, PNs 1-240.
  CheckFixture("general/ntsc_film_pulldown.yaml", {{.name = "Film",
                                                    .start_slot = 0,
                                                    .range_frames = 240,
                                                    .plays_all = true,
                                                    .output_frames = 300}});
}

TEST(FilmFixtureProjectsTest, MixedFixtureBreaksAndContinuesTheCadence) {
  CheckFixture("general/ntsc_film_mixed.yaml",
               {
                   {.name = "Bars", .still = true, .output_frames = 10},
                   // Phase A: 48 film frames fill 120 fields.
                   {.name = "FilmA",
                    .start_slot = 0,
                    .range_frames = 48,
                    .plays_all = true,
                    .output_frames = 60},
                   // Continues at slot 0; 50 film frames fill 125 fields,
                   // padded to 63 frames.
                   {.name = "FilmB",
                    .start_slot = 0,
                    .first_source_frame = 48,
                    .range_frames = 50,
                    .plays_all = true,
                    .output_frames = 63},
                   {.name = "MovingBars", .output_frames = 30},
                   // Phase C (slot 5) for a fixed 28 fields, ending on a single
                   // B field.
                   {.name = "FilmC",
                    .start_slot = 5,
                    .first_source_frame = 120,
                    .range_frames = 120,
                    .output_frames = 14},
                   // Continues at slot (5 + 28) % 10 = 3, B2: 2 + 117 fields,
                   // padded to 60 frames.
                   {.name = "FilmD",
                    .start_slot = 3,
                    .range_frames = 48,
                    .plays_all = true,
                    .output_frames = 60},
               });
}

}  // namespace
}  // namespace videosynth
