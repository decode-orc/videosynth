/*
 * File:        test_pulldown_determinism.cpp
 * Module:      deterministic_output_tests
 * Purpose:     Asserts byte-identical output across thread counts for an NTSC
 *              project mixing video and 2:3 pulldown sections, over clips
 *              generated with ffmpeg at test time.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "videosynth/audio_track_generator.h"
#include "videosynth/dropout_injection_stage.h"
#include "videosynth/generation_stage.h"
#include "videosynth/noise_injection_stage.h"
#include "videosynth/output_stage.h"
#include "videosynth/pipeline.h"
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

// Generated media and output go to a scratch directory, never into the source
// tree.
std::filesystem::path ScratchDirectory() {
  return std::filesystem::temp_directory_path() /
         "videosynth_pulldown_determinism";
}

// Generates `name` once per test process with `ffmpeg -v error -y <args>`,
// staged under a process-unique name and renamed into place so concurrent
// test processes never read a partial file.
std::string Media(const std::string& name, const std::string& ffmpeg_args) {
  static std::mutex mutex;
  static std::set<std::string> generated;
  const std::lock_guard<std::mutex> lock(mutex);
  const std::filesystem::path path = ScratchDirectory() / name;
  if (generated.count(name) == 0U) {
    std::filesystem::create_directories(ScratchDirectory());
    const std::filesystem::path staging =
        ScratchDirectory() / (std::to_string(getpid()) + "_" + name);
    const std::string command = "ffmpeg -v error -y -nostdin " + ffmpeg_args +
                                " '" + staging.string() + "'";
    EXPECT_EQ(std::system(command.c_str()), 0) << command;
    std::filesystem::rename(staging, path);
    generated.insert(name);
  }
  return path.string();
}

// A conformant 525-line FFV1 clip at `rate` frames/s lasting `seconds`.
std::string NtscClip(const std::string& name, const std::string& rate,
                     int seconds) {
  return Media(name, "-f lavfi -i testsrc2=s=720x486:r=" + rate +
                         ":d=" + std::to_string(seconds) +
                         " -c:v ffv1 -pix_fmt yuv422p10le -color_range tv"
                         " -colorspace smpte170m -color_primaries smpte170m"
                         " -color_trc bt709");
}

std::vector<char> ReadFileBytes(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  return std::vector<char>((std::istreambuf_iterator<char>(stream)),
                           std::istreambuf_iterator<char>());
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
  OsdOverlay overlay;
  overlay.text = "{film_frame} {cadence}";
  section.osd.overlays.push_back(overlay);
  section.noise.enabled = true;
  section.noise.noise_db = 40.0;
  section.noise.noise_seed = 2302;
  section.noise.noise_seed_specified = true;
  return section;
}

Section MakeFilm(const std::string& name, const std::string& source,
                 int start_frame, int end_frame, CadencePhase phase,
                 CadenceContinuity continuity) {
  Section section = MakeClip(name, source, start_frame, end_frame);
  section.cadence.pattern = CadencePattern::kPulldown23;
  section.cadence.phase = phase;
  section.cadence.continuity = continuity;
  return section;
}

// video → 2:3 (phase A) → 2:3 (continue, odd length) → video → 2:3 (restart,
// phase C).
Project MakeMixedProject(const std::string& run_tag) {
  const std::string film = NtscClip("film.mkv", "24000/1001", 2);
  const std::string video = NtscClip("video.mkv", "30000/1001", 1);

  Project project;
  project.cvbs_presets.video_standard_preset = Standard::kNtsc;
  project.cvbs_presets.sample_encoding_preset = "CVBS_U10_4FSC";
  project.cvbs_presets.signal_state_preset = "STANDARD_STABLE_LOCKED";
  project.output.video_path =
      (ScratchDirectory() / ("mixed_" + run_tag + ".cvbs")).string();
  project.output.metadata_path =
      (ScratchDirectory() / ("mixed_" + run_tag + ".meta")).string();

  project.sections.push_back(MakeClip("Video1", video, 0, 9));
  project.sections.push_back(MakeFilm("FilmA", film, 0, 12, CadencePhase::kA,
                                      CadenceContinuity::kRestart));
  project.sections.push_back(MakeFilm("FilmB", film, 13, 23, CadencePhase::kA,
                                      CadenceContinuity::kContinue));
  project.sections.push_back(MakeClip("Video2", video, 10, 19));
  project.sections.push_back(MakeFilm("FilmC", film, 30, 41, CadencePhase::kC,
                                      CadenceContinuity::kRestart));
  return project;
}

bool RunPipelineOnce(const Project& project, int threads) {
  NullLogger logger;
  YamlProjectParser parser(&logger);
  ProgressiveFrameSourceProbe probe;
  ProjectValidator validator(&probe, &logger);
  GenerationStage generation(&logger);
  NoiseInjectionStage noise_injection(&logger);
  DropoutInjectionStage dropout_injection(&logger);
  OutputStage output(&logger);
  AudioTrackGenerator audio_generator(&logger);

  VideoSynthPipeline pipeline(&parser, &validator, &generation,
                              &noise_injection, &dropout_injection, &output,
                              &logger, &audio_generator);
  RunOptions options;
  options.threads = threads;
  return pipeline.RunProject(project, options);
}

TEST(PulldownDeterminismTest, MixedProjectIsValid) {
  const Project project = MakeMixedProject("validate");
  ProgressiveFrameSourceProbe probe;
  const ValidationResult result = ProjectValidator(&probe).Validate(project);
  for (const std::string& error : result.errors) {
    ADD_FAILURE() << "Validation error: " << error;
  }
  EXPECT_TRUE(result.is_valid);
}

TEST(PulldownDeterminismTest, SingleAndMultiThreadRunsAreByteIdentical) {
  const Project sequential_project = MakeMixedProject("threads1");
  const Project parallel_project = MakeMixedProject("threads4");

  ASSERT_TRUE(RunPipelineOnce(sequential_project, 1));
  ASSERT_TRUE(RunPipelineOnce(parallel_project, 4));

  const std::vector<char> sequential_composite =
      ReadFileBytes(sequential_project.output.video_path);
  ASSERT_FALSE(sequential_composite.empty());
  EXPECT_EQ(sequential_composite,
            ReadFileBytes(parallel_project.output.video_path))
      << "CVBS sample stream differs between 1-thread and 4-thread runs.";
  EXPECT_EQ(ReadFileBytes(sequential_project.output.metadata_path),
            ReadFileBytes(parallel_project.output.metadata_path))
      << "Metadata database differs between 1-thread and 4-thread runs.";

  for (const Project* project : {&sequential_project, &parallel_project}) {
    std::error_code ec;
    std::filesystem::remove(project->output.video_path, ec);
    std::filesystem::remove(project->output.metadata_path, ec);
  }
}

}  // namespace
}  // namespace videosynth
