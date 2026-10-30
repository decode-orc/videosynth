/*
 * File:        test_pulldown_generation.cpp
 * Module:      generation_stage_tests
 * Purpose:     Validates the field-level frame schedule, source time and
 *              per-field synthesis of mixed video and 2:3 pulldown sections,
 *              using a fake probe and a fake flat-frame decoder.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "videosynth/audio_track_generator.h"
#include "videosynth/fixed_point.h"
#include "videosynth/frame_enrichment.h"
#include "videosynth/frame_line_layout.h"
#include "videosynth/generation_stage.h"
#include "videosynth/progressive_frame_source.h"
#include "videosynth/timing_constants.h"

namespace videosynth {
namespace {

constexpr int kNtscWidth = 720;
constexpr int kNtscHeight = 486;

// Every frame is one flat luma code, unique per (source, frame), so a field's
// active lines show which frame it was drawn from.
int LumaCodeFor(const std::string& source, int frame_index) {
  return source == "film.mkv" ? 200 + (40 * frame_index)
                              : 700 + (20 * frame_index);
}

class FlatFrameDecoder final : public IVideoFrameDecoder {
 public:
  bool Open(const VideoDecodeRequest& request, std::string*) override {
    source_ = request.source;
    next_ = request.first_frame;
    return true;
  }

  VideoFrameReadStatus ReadFrame(FrameSourceImage* out_image,
                                 std::string*) override {
    out_image->width = kNtscWidth;
    out_image->height = kNtscHeight;
    out_image->active_width = kNtscWidth;
    out_image->active_height = kNtscHeight;
    out_image->pixels.assign(static_cast<std::size_t>(kNtscWidth) * kNtscHeight,
                             YCbCr444Pixel{.y = static_cast<std::int16_t>(
                                               LumaCodeFor(source_, next_)),
                                           .cb = 512,
                                           .cr = 512});
    ++next_;
    return VideoFrameReadStatus::kFrame;
  }

 private:
  std::string source_;
  int next_ = 0;
};

class FrameCountProbe final : public IProgressiveFrameSourceProbe {
 public:
  bool Probe(const Section& section, ProgressiveFrameSourceProfile* out_profile,
             std::string*) override {
    ProgressiveFrameSourceProfile profile;
    profile.container = "matroska,webm";
    profile.codec = "ffv1";
    profile.pixel_format = "yuv422p10le";
    profile.color_space = "smpte170m";
    profile.color_range = "tv";
    profile.width = kNtscWidth;
    profile.height = kNtscHeight;
    const bool film = section.source == "film.mkv";
    profile.frame_rate_hz = film ? 24000.0 / 1001.0 : 30000.0 / 1001.0;
    profile.frame_count = film ? 10 : 3;
    *out_profile = profile;
    return true;
  }
};

std::unique_ptr<GenerationStage> MakeStage() {
  return std::make_unique<GenerationStage>(
      nullptr, nullptr, nullptr,
      std::make_unique<ProgressiveFrameSource>(
          std::make_shared<FrameCountProbe>(),
          []() { return std::make_unique<FlatFrameDecoder>(); }, 32U));
}

Section MakeClip(const std::string& name, const std::string& source,
                 int start_frame, int end_frame) {
  Section section;
  section.name = name;
  section.type = "progressive";
  section.source = source;
  section.duration_frames_all = true;
  section.start_frame = start_frame;
  section.end_frame = end_frame;
  section.end_frame_specified = true;
  return section;
}

Section MakeFilm(const std::string& name, int start_frame, int end_frame,
                 CadencePhase phase, CadenceContinuity continuity) {
  Section section = MakeClip(name, "film.mkv", start_frame, end_frame);
  section.cadence.pattern = CadencePattern::kPulldown23;
  section.cadence.phase = phase;
  section.cadence.continuity = continuity;
  return section;
}

// video → 2:3 (phase A) → 2:3 (continue) → video → 2:3 (restart, phase C).
Project MakeMixedProject() {
  Project project;
  project.cvbs_presets.video_standard_preset = Standard::kNtsc;
  project.cvbs_presets.sample_encoding_preset = "CVBS_U10_4FSC";
  project.cvbs_presets.signal_state_preset = "STANDARD_STABLE_LOCKED";
  project.sections.push_back(MakeClip("Video1", "video.mkv", 0, 2));
  project.sections.push_back(
      MakeFilm("FilmA", 0, 4, CadencePhase::kA, CadenceContinuity::kRestart));
  project.sections.push_back(
      MakeFilm("FilmB", 5, 6, CadencePhase::kA, CadenceContinuity::kContinue));
  project.sections.push_back(MakeClip("Video2", "video.mkv", 0, 1));
  project.sections.push_back(
      MakeFilm("FilmC", 0, 1, CadencePhase::kC, CadenceContinuity::kRestart));
  return project;
}

struct ExpectedField {
  int source_frame;
  // "-" for video cadence, otherwise letter + field within it.
  std::string cadence;
  bool starts_picture;
};

struct ExpectedFrame {
  std::string section;
  ExpectedField field1;
  ExpectedField field2;
};

std::vector<ExpectedFrame> ExpectedMixedSchedule() {
  return {
      {"Video1", {0, "-", true}, {0, "-", false}},
      {"Video1", {1, "-", true}, {1, "-", false}},
      {"Video1", {2, "-", true}, {2, "-", false}},
      // Slot 0, five film frames: AABBBCCDDDAA, twelve fields.
      {"FilmA", {0, "A1", true}, {0, "A2", false}},
      {"FilmA", {1, "B1", true}, {1, "B2", false}},
      {"FilmA", {1, "B3", false}, {2, "C1", true}},
      {"FilmA", {2, "C2", false}, {3, "D1", true}},
      {"FilmA", {3, "D2", false}, {3, "D3", false}},
      {"FilmA", {4, "A1", true}, {4, "A2", false}},
      // Continues at slot 2 with film frames 5-6: BBBCC, padded by a sixth
      // field repeating C.
      {"FilmB", {5, "B1", true}, {5, "B2", false}},
      {"FilmB", {5, "B3", false}, {6, "C1", true}},
      {"FilmB", {6, "C2", false}, {6, "C3", false}},
      {"Video2", {0, "-", true}, {0, "-", false}},
      {"Video2", {1, "-", true}, {1, "-", false}},
      // Restarts at phase C: CCDDD, padded with a fourth D.
      {"FilmC", {0, "C1", true}, {0, "C2", false}},
      {"FilmC", {1, "D1", true}, {1, "D2", false}},
      {"FilmC", {1, "D3", false}, {1, "D4", false}},
  };
}

std::string CadenceLabel(const IGenerationStage::FieldPicture& field) {
  if (field.cadence_letter == '\0') {
    return "-";
  }
  return std::string(1, field.cadence_letter) +
         std::to_string(field.cadence_field_in_letter);
}

void ExpectField(const IGenerationStage::FieldPicture& actual,
                 const ExpectedField& expected, std::size_t frame, int field) {
  EXPECT_EQ(actual.source_frame_index, expected.source_frame)
      << "frame " << frame << " field " << field;
  EXPECT_EQ(CadenceLabel(actual), expected.cadence)
      << "frame " << frame << " field " << field;
  EXPECT_EQ(actual.starts_picture, expected.starts_picture)
      << "frame " << frame << " field " << field;
}

TEST(PulldownGenerationTest, MixedChainProducesExpectedFieldTable) {
  const Project project = MakeMixedProject();
  std::unique_ptr<GenerationStage> stage = MakeStage();
  std::vector<IGenerationStage::FrameScheduleItem> schedule;
  std::vector<std::string> errors;
  ASSERT_TRUE(stage->BuildFrameSchedule(project, &schedule, &errors))
      << (errors.empty() ? "" : errors.front());

  const std::vector<ExpectedFrame> expected = ExpectedMixedSchedule();
  ASSERT_EQ(schedule.size(), expected.size());
  for (std::size_t frame = 0; frame < expected.size(); ++frame) {
    const IGenerationStage::FrameScheduleItem& item = schedule[frame];
    EXPECT_EQ(item.section->name, expected[frame].section) << "frame " << frame;
    EXPECT_EQ(item.source_frame_index, expected[frame].field1.source_frame)
        << "frame " << frame;
    ExpectField(item.fields[0], expected[frame].field1, frame, 1);
    ExpectField(item.fields[1], expected[frame].field2, frame, 2);
  }
}

TEST(PulldownGenerationTest, SectionFrameIndexCountsOutputFrames) {
  const Project project = MakeMixedProject();
  std::unique_ptr<GenerationStage> stage = MakeStage();
  std::vector<IGenerationStage::FrameScheduleItem> schedule;
  std::vector<std::string> errors;
  ASSERT_TRUE(stage->BuildFrameSchedule(project, &schedule, &errors));

  // FilmA occupies schedule positions 3-8.
  for (int position = 0; position < 6; ++position) {
    EXPECT_EQ(
        schedule[static_cast<std::size_t>(3 + position)].section_frame_index,
        position);
  }
}

TEST(PulldownGenerationTest, FixedDurationLoopsTheFilmRange) {
  Project project;
  project.cvbs_presets.video_standard_preset = Standard::kNtsc;
  project.cvbs_presets.sample_encoding_preset = "CVBS_U10_4FSC";
  project.cvbs_presets.signal_state_preset = "STANDARD_STABLE_LOCKED";
  Section film =
      MakeFilm("Film", 0, 1, CadencePhase::kA, CadenceContinuity::kRestart);
  film.duration_frames_all = false;
  film.duration_frames = 3;
  project.sections.push_back(film);

  std::unique_ptr<GenerationStage> stage = MakeStage();
  std::vector<IGenerationStage::FrameScheduleItem> schedule;
  std::vector<std::string> errors;
  ASSERT_TRUE(stage->BuildFrameSchedule(project, &schedule, &errors));
  ASSERT_EQ(schedule.size(), 3U);
  // AABBB C: the third film frame wraps back to the range start.
  EXPECT_EQ(schedule[2].fields[0].source_frame_index, 1);
  EXPECT_EQ(schedule[2].fields[1].source_frame_index, 0);
  EXPECT_EQ(CadenceLabel(schedule[2].fields[1]), "C1");
}

// The disc frame counter, not the picture number, anchors the colour
// sequence: a 2:3 CAV section numbers four pictures in five frames while its
// disc frames, seeded from the first picture number, run on one per frame.
TEST(PulldownGenerationTest, CavFilmKeepsTheColourSequenceOnDiscFrames) {
  Project project;
  project.cvbs_presets.video_standard_preset = Standard::kNtsc;
  project.cvbs_presets.sample_encoding_preset = "CVBS_U10_4FSC";
  project.cvbs_presets.signal_state_preset = "STANDARD_STABLE_LOCKED";
  project.line_injections.disc_type = "CAV";
  Section film =
      MakeFilm("Film", 0, 9, CadencePhase::kA, CadenceContinuity::kRestart);
  film.section_type = SectionType::kProgrammeArea;
  Section::LineInjection injection;
  injection.type = "laserdisc";
  Section::LineInjectionCode picture_number;
  picture_number.code_type = "picture_number";
  picture_number.start_value = 101;
  picture_number.start_value_specified = true;
  injection.codes = {picture_number};
  film.line_injections.push_back(injection);
  OsdOverlay overlay;
  overlay.text = "{picture_number}";
  film.osd.overlays.push_back(overlay);
  project.sections.push_back(film);

  std::unique_ptr<GenerationStage> stage = MakeStage();
  std::vector<IGenerationStage::FrameScheduleItem> schedule;
  std::vector<std::string> errors;
  ASSERT_TRUE(stage->BuildFrameSchedule(project, &schedule, &errors))
      << (errors.empty() ? "" : errors.front());
  // Ten film frames fill 25 fields: 13 frames.
  ASSERT_EQ(schedule.size(), 13U);

  for (std::size_t frame = 0; frame < schedule.size(); ++frame) {
    EXPECT_EQ(schedule[frame].enrichment->disc_frame_index, 100 + frame)
        << "frame " << frame;
  }
  // Pictures 101-104 start in frames 0-3; frame 4 (D2 D3) starts none, so
  // frame 5 opens picture 105.
  const std::vector<std::array<int, 2>> expected = {
      {101, 101}, {102, 102}, {102, 103}, {103, 104}, {104, 104}, {105, 105}};
  for (std::size_t frame = 0; frame < expected.size(); ++frame) {
    const FrameEnrichment& enrichment = *schedule[frame].enrichment;
    EXPECT_EQ(enrichment.field_picture_numbers[0], expected[frame][0])
        << "frame " << frame;
    EXPECT_EQ(enrichment.field_picture_numbers[1], expected[frame][1])
        << "frame " << frame;
  }
  EXPECT_EQ(schedule[2].enrichment->field_osd_texts[0].at(0), "00102");
  EXPECT_EQ(schedule[2].enrichment->field_osd_texts[1].at(0), "00103");
}

TEST(PulldownGenerationTest, AudioFollowsFilmTimeAndVideoFrames) {
  const Project project = MakeMixedProject();
  std::unique_ptr<GenerationStage> stage = MakeStage();
  std::vector<IGenerationStage::FrameScheduleItem> schedule;
  std::vector<std::string> errors;
  ASSERT_TRUE(stage->BuildFrameSchedule(project, &schedule, &errors));
  const std::vector<AudioOutputFrame> audio =
      AudioOutputFramesForSchedule(schedule);
  ASSERT_EQ(audio.size(), schedule.size());

  // Video frames start at the frame they show; film frames run on in output
  // fields from their range's first film frame, whatever the cadence shows.
  struct Expected {
    int source_frame_index;
    std::int64_t fields_after;
  };
  const std::vector<Expected> expected = {
      {0, 0}, {1, 0}, {2, 0},                           // Video1
      {0, 0}, {0, 2}, {0, 4}, {0, 6}, {0, 8}, {0, 10},  // FilmA
      {5, 0}, {5, 2}, {5, 4},                           // FilmB
      {0, 0}, {1, 0},                                   // Video2
      {0, 0}, {0, 2}, {0, 4},                           // FilmC
  };
  ASSERT_EQ(audio.size(), expected.size());
  for (std::size_t frame = 0; frame < expected.size(); ++frame) {
    EXPECT_EQ(audio[frame].section, schedule[frame].section);
    EXPECT_EQ(audio[frame].source_frame_index,
              expected[frame].source_frame_index)
        << "frame " << frame;
    EXPECT_EQ(audio[frame].fields_after, expected[frame].fields_after)
        << "frame " << frame;
  }
}

TEST(PulldownGenerationTest, AudioRestartsWithEachPassOfALoopedFilmRange) {
  Project project;
  project.cvbs_presets.video_standard_preset = Standard::kNtsc;
  project.cvbs_presets.sample_encoding_preset = "CVBS_U10_4FSC";
  project.cvbs_presets.signal_state_preset = "STANDARD_STABLE_LOCKED";
  Section film =
      MakeFilm("Film", 3, 4, CadencePhase::kA, CadenceContinuity::kRestart);
  film.duration_frames_all = false;
  film.duration_frames = 5;
  project.sections.push_back(film);

  std::unique_ptr<GenerationStage> stage = MakeStage();
  std::vector<IGenerationStage::FrameScheduleItem> schedule;
  std::vector<std::string> errors;
  ASSERT_TRUE(stage->BuildFrameSchedule(project, &schedule, &errors));
  ASSERT_EQ(schedule.size(), 5U);

  // AABBB|CCDDD: the second pass starts on field 5, so frames 3 and 4 sit one
  // and three fields into it.
  const std::int64_t expected_fields[] = {0, 2, 4, 1, 3};
  for (std::size_t frame = 0; frame < schedule.size(); ++frame) {
    EXPECT_EQ(schedule[frame].source_time.frame_index, 3) << "frame " << frame;
    EXPECT_EQ(schedule[frame].source_time.fields_after, expected_fields[frame])
        << "frame " << frame;
  }
}

TEST(PulldownGenerationTest, OsdFieldTokensResolvePerField) {
  Project project = MakeMixedProject();
  OsdOverlay overlay;
  overlay.text = "{film_frame} {cadence}";
  for (Section& section : project.sections) {
    section.osd.overlays.push_back(overlay);
  }

  std::unique_ptr<GenerationStage> stage = MakeStage();
  std::vector<IGenerationStage::FrameScheduleItem> schedule;
  std::vector<std::string> errors;
  ASSERT_TRUE(stage->BuildFrameSchedule(project, &schedule, &errors));

  const auto texts = [&](std::size_t frame, std::size_t field) {
    return schedule[frame].enrichment->field_osd_texts[field].at(0);
  };
  EXPECT_EQ(texts(1, 0), "1 -");
  EXPECT_EQ(texts(1, 1), "1 -");
  // FilmA's third frame: B3 then C1.
  EXPECT_EQ(texts(5, 0), "1 B3");
  EXPECT_EQ(texts(5, 1), "2 C1");
}

// The level of the active luma at the centre of a field's line, as a 10-bit
// code.
int FieldLumaCode(const std::vector<SampleFixed>& y_mv, int field) {
  const Standard standard = Standard::kNtsc;
  const TimingConstants timing = GetTimingConstants(standard);
  const ActiveRasterGeometry geometry =
      GetActiveRasterGeometry(standard, timing.sample_rate_4fsc_hz);
  const std::vector<int> offsets = BuildLineSampleOffsets(BuildLineSampleCounts(
      standard, timing.lines_per_frame, timing.samples_per_line_4fsc));
  const int first_line = field == 1 ? geometry.first_active_line_field1
                                    : geometry.first_active_line_field2;
  const int line_1based = first_line + (geometry.active_lines_per_field / 2);
  const int sample_in_frame =
      offsets[static_cast<std::size_t>(line_1based - 1)] +
      geometry.active_window_start_samples + 300;
  const auto sample = static_cast<std::size_t>(sample_in_frame);
  const SignalLevels levels = GetSignalLevels(standard);
  const double millivolts = SampleFixedToMillivolts(y_mv[sample]);
  return static_cast<int>(
      std::lround(64.0 + ((millivolts - levels.black_mv) /
                          (levels.white_mv - levels.black_mv) * 876.0)));
}

TEST(PulldownGenerationTest, EachFieldShowsItsOwnSourceFrame) {
  const Project project = MakeMixedProject();
  std::unique_ptr<GenerationStage> stage = MakeStage();
  std::vector<IGenerationStage::FrameScheduleItem> schedule;
  std::vector<std::string> errors;
  ASSERT_TRUE(stage->BuildFrameSchedule(project, &schedule, &errors));

  for (std::size_t frame = 0; frame < schedule.size(); ++frame) {
    std::vector<SampleFixed> y_mv;
    std::vector<SampleFixed> c_mv;
    ASSERT_TRUE(stage->GenerateFrameBatch(project, schedule, frame, 1U, &y_mv,
                                          &c_mv, &errors))
        << (errors.empty() ? "" : errors.front());
    const IGenerationStage::FrameScheduleItem& item = schedule[frame];
    for (int field = 1; field <= 2; ++field) {
      EXPECT_EQ(FieldLumaCode(y_mv, field),
                LumaCodeFor(item.section->source,
                            item.fields[static_cast<std::size_t>(field - 1)]
                                .source_frame_index))
          << "frame " << frame << " field " << field;
    }
  }
}

// Frames with the same field-1 film frame but a different field-2 one must
// not share a cached template.
TEST(PulldownGenerationTest, TemplateCacheSeparatesFieldPairs) {
  const Project project = MakeMixedProject();
  std::unique_ptr<GenerationStage> stage = MakeStage();
  std::vector<IGenerationStage::FrameScheduleItem> schedule;
  std::vector<std::string> errors;
  ASSERT_TRUE(stage->BuildFrameSchedule(project, &schedule, &errors));

  // Request every frame twice so the cache admits and then serves templates.
  for (int pass = 0; pass < 2; ++pass) {
    for (std::size_t frame = 0; frame < schedule.size(); ++frame) {
      std::vector<SampleFixed> y_mv;
      std::vector<SampleFixed> c_mv;
      ASSERT_TRUE(stage->GenerateFrameBatch(project, schedule, frame, 1U, &y_mv,
                                            &c_mv, &errors));
      EXPECT_EQ(FieldLumaCode(y_mv, 2),
                LumaCodeFor(schedule[frame].section->source,
                            schedule[frame].fields[1].source_frame_index))
          << "pass " << pass << " frame " << frame;
    }
  }
}

}  // namespace
}  // namespace videosynth
