/*
 * File:        test_cadence_validation.cpp
 * Module:      cadence_validation_tests
 * Purpose:     Validates section cadence rules: 2:3 pulldown standard,
 *              source and frame-rate requirements and continuity.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

#include "videosynth/project_validator.h"

namespace videosynth {
namespace {

constexpr double kFilmRateHz = 24000.0 / 1001.0;
constexpr double kNtscRateHz = 30000.0 / 1001.0;

// A conformant 720x486 FFV1 clip profile at `frame_rate_hz`.
ProgressiveFrameSourceProfile MakeClipProfile(double frame_rate_hz,
                                              int height = 486) {
  ProgressiveFrameSourceProfile profile;
  profile.container = "matroska,webm";
  profile.codec = "ffv1";
  profile.pixel_format = "yuv422p10le";
  profile.field_order = "progressive";
  profile.color_space = "smpte170m";
  profile.color_primaries = "smpte170m";
  profile.color_transfer = "bt709";
  profile.color_range = "tv";
  profile.bit_depth = 10;
  profile.width = 720;
  profile.height = height;
  profile.sample_aspect_ratio = height == 486 ? (108.0 / 119.0) : 128.0 / 117.0;
  profile.frame_rate_hz = frame_rate_hz;
  profile.average_frame_rate_hz = frame_rate_hz;
  profile.frame_count = 96;
  return profile;
}

// Probes by source path, so one project can mix film and video clips.
class SourceMapProbe final : public IProgressiveFrameSourceProbe {
 public:
  std::map<std::string, ProgressiveFrameSourceProfile> profiles;

  bool Probe(const Section& section, ProgressiveFrameSourceProfile* out_profile,
             std::string* error) override {
    const auto found = profiles.find(section.source);
    if (found == profiles.end()) {
      *error = "no profile for " + section.source;
      return false;
    }
    *out_profile = found->second;
    return true;
  }
};

Section MakeClipSection(const std::string& name, const std::string& source,
                        CadencePattern pattern) {
  Section section;
  section.name = name;
  section.type = "progressive";
  section.source = source;
  section.duration_frames_all = true;
  section.cadence.pattern = pattern;
  return section;
}

Project MakeProject(Standard standard) {
  Project project;
  project.cvbs_presets.video_standard_preset = standard;
  project.cvbs_presets.sample_encoding_preset = "CVBS_U10_4FSC";
  project.cvbs_presets.signal_state_preset = "STANDARD_STABLE_LOCKED";
  project.output.video_path = "/tmp/videosynth_cadence_test.cvbs";
  project.output.metadata_path = "/tmp/videosynth_cadence_test.meta";
  return project;
}

SourceMapProbe MakeNtscProbe() {
  SourceMapProbe probe;
  probe.profiles["film.mkv"] = MakeClipProfile(kFilmRateHz);
  probe.profiles["video.mkv"] = MakeClipProfile(kNtscRateHz);
  return probe;
}

bool AnyContains(const std::vector<std::string>& messages,
                 const std::string& text) {
  for (const std::string& message : messages) {
    if (message.find(text) != std::string::npos) {
      return true;
    }
  }
  return false;
}

TEST(CadenceValidationTest, AcceptsFilmSourceInPulldownSectionOnNtscAndPalM) {
  for (const Standard standard : {Standard::kNtsc, Standard::kPalM}) {
    Project project = MakeProject(standard);
    project.sections.push_back(
        MakeClipSection("Film", "film.mkv", CadencePattern::kPulldown23));
    SourceMapProbe probe = MakeNtscProbe();

    const ValidationResult result = ProjectValidator(&probe).Validate(project);
    EXPECT_TRUE(result.is_valid)
        << (result.errors.empty() ? "" : result.errors.front());
    EXPECT_TRUE(result.warnings.empty());
  }
}

TEST(CadenceValidationTest, AcceptsMixedVideoAndPulldownSections) {
  Project project = MakeProject(Standard::kNtsc);
  project.sections.push_back(
      MakeClipSection("Video", "video.mkv", CadencePattern::kVideo));
  project.sections.push_back(
      MakeClipSection("FilmA", "film.mkv", CadencePattern::kPulldown23));
  Section continued =
      MakeClipSection("FilmB", "film.mkv", CadencePattern::kPulldown23);
  continued.cadence.continuity = CadenceContinuity::kContinue;
  project.sections.push_back(continued);
  project.sections.push_back(
      MakeClipSection("Video2", "video.mkv", CadencePattern::kVideo));
  SourceMapProbe probe = MakeNtscProbe();

  const ValidationResult result = ProjectValidator(&probe).Validate(project);
  EXPECT_TRUE(result.is_valid)
      << (result.errors.empty() ? "" : result.errors.front());
}

TEST(CadenceValidationTest, RejectsFilmSourceInVideoSection) {
  Project project = MakeProject(Standard::kNtsc);
  project.sections.push_back(
      MakeClipSection("Film", "film.mkv", CadencePattern::kVideo));
  SourceMapProbe probe = MakeNtscProbe();

  const ValidationResult result = ProjectValidator(&probe).Validate(project);
  EXPECT_FALSE(result.is_valid);
  EXPECT_TRUE(AnyContains(result.errors, "30000/1001"));
}

TEST(CadenceValidationTest, RejectsVideoSourceInPulldownSection) {
  Project project = MakeProject(Standard::kNtsc);
  project.sections.push_back(
      MakeClipSection("Video", "video.mkv", CadencePattern::kPulldown23));
  SourceMapProbe probe = MakeNtscProbe();

  const ValidationResult result = ProjectValidator(&probe).Validate(project);
  EXPECT_FALSE(result.is_valid);
  EXPECT_TRUE(AnyContains(result.errors, "24000/1001"));
}

TEST(CadenceValidationTest, RejectsPulldownOn625LineStandards) {
  for (const Standard standard : {Standard::kPal, Standard::kSecam}) {
    Project project = MakeProject(standard);
    project.cvbs_presets.signal_state_preset = standard == Standard::kSecam
                                                   ? "STANDARD_STABLE_UNLOCKED"
                                                   : "STANDARD_STABLE_LOCKED";
    project.sections.push_back(
        MakeClipSection("Film", "film.mkv", CadencePattern::kPulldown23));
    SourceMapProbe probe;
    probe.profiles["film.mkv"] = MakeClipProfile(25.0, 576);

    const ValidationResult result = ProjectValidator(&probe).Validate(project);
    EXPECT_FALSE(result.is_valid);
    EXPECT_TRUE(AnyContains(result.errors, "525-line"));
  }
}

TEST(CadenceValidationTest, RejectsPulldownOnExrStill) {
  Project project = MakeProject(Standard::kNtsc);
  Section still =
      MakeClipSection("Still", "bars.exr", CadencePattern::kPulldown23);
  still.duration_frames_all = false;
  still.duration_frames = 10;
  project.sections.push_back(still);

  const ValidationResult result = ProjectValidator().Validate(project);
  EXPECT_FALSE(result.is_valid);
  EXPECT_TRUE(AnyContains(result.errors, "EXR still"));
}

TEST(CadenceValidationTest, RejectsContinueOnFirstSection) {
  Project project = MakeProject(Standard::kNtsc);
  Section film =
      MakeClipSection("Film", "film.mkv", CadencePattern::kPulldown23);
  film.cadence.continuity = CadenceContinuity::kContinue;
  project.sections.push_back(film);
  SourceMapProbe probe = MakeNtscProbe();

  const ValidationResult result = ProjectValidator(&probe).Validate(project);
  EXPECT_FALSE(result.is_valid);
  EXPECT_TRUE(AnyContains(result.errors, "continue"));
}

TEST(CadenceValidationTest, RejectsContinueAfterVideoSection) {
  Project project = MakeProject(Standard::kNtsc);
  project.sections.push_back(
      MakeClipSection("Video", "video.mkv", CadencePattern::kVideo));
  Section film =
      MakeClipSection("Film", "film.mkv", CadencePattern::kPulldown23);
  film.cadence.continuity = CadenceContinuity::kContinue;
  project.sections.push_back(film);
  SourceMapProbe probe = MakeNtscProbe();

  const ValidationResult result = ProjectValidator(&probe).Validate(project);
  EXPECT_FALSE(result.is_valid);
  EXPECT_TRUE(AnyContains(result.errors, "previous section"));
}

TEST(CadenceValidationTest, WarnsThatPhaseIsIgnoredWhenContinuing) {
  Project project = MakeProject(Standard::kNtsc);
  project.sections.push_back(
      MakeClipSection("FilmA", "film.mkv", CadencePattern::kPulldown23));
  Section film =
      MakeClipSection("FilmB", "film.mkv", CadencePattern::kPulldown23);
  film.cadence.continuity = CadenceContinuity::kContinue;
  film.cadence.phase = CadencePhase::kC;
  film.cadence.phase_specified = true;
  project.sections.push_back(film);
  SourceMapProbe probe = MakeNtscProbe();

  const ValidationResult result = ProjectValidator(&probe).Validate(project);
  EXPECT_TRUE(result.is_valid)
      << (result.errors.empty() ? "" : result.errors.front());
  EXPECT_TRUE(AnyContains(result.warnings, "phase is ignored"));
}

TEST(CadenceValidationTest, WarnsThatPhaseIsIgnoredForVideoCadence) {
  Project project = MakeProject(Standard::kNtsc);
  Section video = MakeClipSection("Video", "video.mkv", CadencePattern::kVideo);
  video.cadence.phase = CadencePhase::kB;
  video.cadence.phase_specified = true;
  project.sections.push_back(video);
  SourceMapProbe probe = MakeNtscProbe();

  const ValidationResult result = ProjectValidator(&probe).Validate(project);
  EXPECT_TRUE(result.is_valid)
      << (result.errors.empty() ? "" : result.errors.front());
  EXPECT_TRUE(AnyContains(result.warnings, "ignored for a video cadence"));
}

// A CAV programme section numbering its pictures from `start_value`.
Section WithPictureNumbers(Section section, int start_value) {
  section.section_type = SectionType::kProgrammeArea;
  Section::LineInjection injection;
  injection.type = "laserdisc";
  Section::LineInjectionCode picture_number;
  picture_number.code_type = "picture_number";
  picture_number.start_value = start_value;
  picture_number.start_value_specified = true;
  Section::LineInjectionCode fm_picture_number;
  fm_picture_number.code_type = "fm_picture_number";
  injection.codes = {picture_number, fm_picture_number};
  section.line_injections.push_back(injection);
  return section;
}

Project MakeCavProject() {
  Project project = MakeProject(Standard::kNtsc);
  project.line_injections.disc_type = "CAV";
  project.line_injections.vits.push_back(VitsInjection{"virs", {19, 282}});
  return project;
}

// IEC 60857 Amendment 2 §10.2.3: the 96 film frames of a 2:3 section are 96
// pictures (in 120 output frames), so numbering from 79904 ends exactly on
// the 79 999 maximum.
TEST(CadenceValidationTest, PictureNumberMaximumCountsFilmPictures) {
  Project project = MakeCavProject();
  project.sections.push_back(WithPictureNumbers(
      MakeClipSection("Film", "film.mkv", CadencePattern::kPulldown23), 79904));
  SourceMapProbe probe = MakeNtscProbe();

  const ValidationResult result = ProjectValidator(&probe).Validate(project);
  EXPECT_TRUE(result.is_valid)
      << (result.errors.empty() ? "" : result.errors.front());
  EXPECT_FALSE(AnyContains(result.warnings, "picture number"));
}

TEST(CadenceValidationTest, WarnsWhenFilmPicturesPassTheMaximum) {
  Project project = MakeCavProject();
  project.sections.push_back(WithPictureNumbers(
      MakeClipSection("Film", "film.mkv", CadencePattern::kPulldown23), 79905));
  SourceMapProbe probe = MakeNtscProbe();

  const ValidationResult result = ProjectValidator(&probe).Validate(project);
  EXPECT_TRUE(result.is_valid)
      << (result.errors.empty() ? "" : result.errors.front());
  EXPECT_TRUE(
      AnyContains(result.warnings, "would reach CAV picture number 80000"));
}

// The count runs on across sections: 96 video frames after the film use up
// 96 more picture numbers.
TEST(CadenceValidationTest, PictureNumberCountContinuesIntoLaterSections) {
  Project project = MakeCavProject();
  project.sections.push_back(WithPictureNumbers(
      MakeClipSection("Film", "film.mkv", CadencePattern::kPulldown23), 79808));
  Section video = MakeClipSection("Video", "video.mkv", CadencePattern::kVideo);
  video.section_type = SectionType::kProgrammeArea;
  Section::LineInjection injection;
  injection.type = "laserdisc";
  Section::LineInjectionCode picture_number;
  picture_number.code_type = "picture_number";
  Section::LineInjectionCode fm_picture_number;
  fm_picture_number.code_type = "fm_picture_number";
  injection.codes = {picture_number, fm_picture_number};
  video.line_injections.push_back(injection);
  project.sections.push_back(video);
  SourceMapProbe probe = MakeNtscProbe();

  ValidationResult result = ProjectValidator(&probe).Validate(project);
  EXPECT_FALSE(AnyContains(result.warnings, "picture number"));

  project.sections[0].line_injections[0].codes[0].start_value = 79809;
  result = ProjectValidator(&probe).Validate(project);
  EXPECT_TRUE(AnyContains(result.warnings,
                          "section 'Video' would reach CAV picture number "
                          "80000"));
}

}  // namespace
}  // namespace videosynth
