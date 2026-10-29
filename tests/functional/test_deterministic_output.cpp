/*
 * File:        test_deterministic_output.cpp
 * Module:      deterministic_output_tests
 * Purpose:     Regression harness asserting byte-identical pipeline output
 *              across repeated runs of a fixed-seed multi-section project.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "videosynth/audio_track_generator.h"
#include "videosynth/audio_wav_writer.h"
#include "videosynth/dropout_injection_stage.h"
#include "videosynth/generation_stage.h"
#include "videosynth/noise_injection_stage.h"
#include "videosynth/output_stage.h"
#include "videosynth/pipeline.h"
#include "videosynth/progressive_frame_source_probe.h"
#include "videosynth/project_validator.h"
#include "videosynth/timing_constants.h"
#include "videosynth/yaml_project_parser.h"

namespace videosynth {
namespace {

// -----------------------------------------------------------------------
// Deterministic-output baseline (multi-threading regression guard).
//
// This harness runs the complete sequential pipeline twice over the same
// fixed-seed project and asserts every emitted artefact is byte-identical:
//   - <name>.cvbs           quantised CVBS sample stream
//   - <name>.meta           CVBS metadata SQLite database
//   - <name>_audio_00.wav   frame-locked stereo PCM track
//   - <name>.dropouts.meta  dropout sidecar SQLite database
//
// The project exercises every per-frame randomised or stateful feature in
// one run: laserdisc biphase VBI codes (sequential picture numbers), VITS,
// seeded Gaussian noise, seeded random and scratch dropouts, and synthetic
// audio. Any frame-generation parallelisation must keep this
// test passing unchanged: 1-thread and N-thread runs are required to
// produce output byte-identical to this sequential baseline.
// -----------------------------------------------------------------------

class NullLogger final : public ILogger {
 public:
  void Info(const std::string&) override {}
  void Warning(const std::string&) override {}
  void Error(const std::string&) override {}
  void Debug(const std::string&) override {}
  void Trace(const std::string&) override {}
};

std::string AssetPath(const std::string& relative) {
  return (std::filesystem::path(VIDEOSYNTH_SOURCE_DIR) / relative).string();
}

std::vector<char> ReadFileBytes(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  return std::vector<char>((std::istreambuf_iterator<char>(stream)),
                           std::istreambuf_iterator<char>());
}

// Builds a two-section PAL CAV project combining biphase codes, VITS,
// seeded noise, seeded dropouts, and audio. All random processes carry
// explicit seeds so repeated runs are bit-exact by contract.
Project MakeDeterministicProject(const std::filesystem::path& output_dir,
                                 const std::string& run_tag) {
  Project project;
  project.cvbs_presets.video_standard_preset = Standard::kPal;
  project.cvbs_presets.sample_encoding_preset = "CVBS_U10_4FSC";
  project.cvbs_presets.signal_state_preset = "STANDARD_STABLE_LOCKED";
  project.output.video_path =
      (output_dir / ("videosynth_determinism_" + run_tag + ".cvbs")).string();
  project.output.metadata_path =
      (output_dir / ("videosynth_determinism_" + run_tag + ".meta")).string();

  // Laserdisc format and VITS complement are project-wide decisions.
  project.line_injections.disc_type = "CAV";
  project.line_injections.vits.push_back(VitsInjection{"uk-national", {19}});

  Section lead_in;
  lead_in.name = "CavLeadIn";
  lead_in.type = "progressive";
  lead_in.section_type = SectionType::kLeadIn;
  lead_in.source =
      AssetPath("videosynth-assets/assets/exr/720x576/75_BARS.exr");
  lead_in.duration_frames = 8;
  {
    Section::LineInjection laserdisc;
    laserdisc.type = "laserdisc";
    Section::LineInjectionCode lead_in_code;
    lead_in_code.code_type = "lead_in";
    laserdisc.codes.push_back(lead_in_code);
    lead_in.line_injections.push_back(laserdisc);
  }
  lead_in.noise.enabled = true;
  lead_in.noise.noise_db = 40.0;
  lead_in.noise.noise_spread_db = 4.0;
  lead_in.noise.noise_seed = 1001;
  lead_in.noise.noise_seed_specified = true;
  {
    AudioChannelPair pair0;
    pair0.pair = 0;
    pair0.pair_specified = true;
    pair0.description = "Analogue stereo";
    pair0.left.enabled = true;
    pair0.left.waveform = AudioWaveform::kSine;
    pair0.left.frequency_hz = 1000.0;
    pair0.left.amplitude = 0.4;
    pair0.right.enabled = true;
    pair0.right.waveform = AudioWaveform::kSine;
    pair0.right.frequency_hz = 1000.0;
    pair0.right.amplitude = 0.4;
    lead_in.audio_channel_pairs.push_back(pair0);
  }
  project.sections.push_back(lead_in);

  Section programme;
  programme.name = "CavProgramme";
  programme.type = "progressive";
  programme.section_type = SectionType::kProgrammeArea;
  programme.source =
      AssetPath("videosynth-assets/assets/exr/720x576/LUMA_RAMP.exr");
  programme.duration_frames = 8;
  {
    Section::LineInjection laserdisc;
    laserdisc.type = "laserdisc";
    Section::LineInjectionCode picture_number;
    picture_number.code_type = "picture_number";
    picture_number.start_value = 1;
    picture_number.start_value_specified = true;
    laserdisc.codes.push_back(picture_number);
    programme.line_injections.push_back(laserdisc);
  }
  programme.noise.enabled = true;
  programme.noise.noise_db = 38.0;
  programme.noise.noise_spread_db = 4.0;
  programme.noise.noise_seed = 1002;
  programme.noise.noise_seed_specified = true;
  programme.dropouts.random.enabled = true;
  programme.dropouts.random.scale = 8;
  programme.dropouts.random.seed_specified = true;
  programme.dropouts.random.seed = 2002;
  programme.dropouts.scratch.enabled = true;
  programme.dropouts.scratch.scale = 8;
  programme.dropouts.scratch.seed_specified = true;
  programme.dropouts.scratch.seed = 2003;
  {
    // Pair 0: left is a ramped triangle, right is silent (all zeros).
    AudioChannelPair pair0;
    pair0.pair = 0;
    pair0.pair_specified = true;
    pair0.left.enabled = true;
    pair0.left.waveform = AudioWaveform::kTriangle;
    pair0.left.ramp_enabled = true;
    pair0.left.ramp_start_hz = 400.0;
    pair0.left.ramp_end_hz = 2000.0;
    pair0.left.ramp_start_specified = true;
    pair0.left.ramp_end_specified = true;
    pair0.left.ramp_mode = AudioRampMode::kUp;
    pair0.left.amplitude = 0.5;
    programme.audio_channel_pairs.push_back(pair0);

    // Pair 1: a distinct stereo commentary tone (exercises multi-track).
    AudioChannelPair pair1;
    pair1.pair = 1;
    pair1.pair_specified = true;
    pair1.description = "Commentary";
    pair1.left.enabled = true;
    pair1.left.waveform = AudioWaveform::kSine;
    pair1.left.frequency_hz = 1000.0;
    pair1.left.amplitude = 0.5;
    pair1.right.enabled = true;
    pair1.right.waveform = AudioWaveform::kSquare;
    pair1.right.frequency_hz = 500.0;
    pair1.right.amplitude = 0.5;
    programme.audio_channel_pairs.push_back(pair1);
  }
  project.sections.push_back(programme);

  return project;
}

// Runs the full pipeline once with a fresh set of concrete stages, as the
// CLI does, and returns the pipeline result. threads follows the RunOptions
// convention (1 = sequential path, N > 1 = worker-pool synthesis).
bool RunPipelineOnce(const Project& project, int threads = 1) {
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

struct RunArtefacts {
  std::filesystem::path composite;
  std::filesystem::path metadata;
  std::vector<std::filesystem::path> audio_pairs;
  std::filesystem::path dropout_sidecar;
};

RunArtefacts ArtefactPaths(const Project& project) {
  RunArtefacts artefacts;
  artefacts.composite = project.output.video_path;
  artefacts.metadata = project.output.metadata_path;
  for (const int pair : ProjectAudioChannelPairs(project)) {
    artefacts.audio_pairs.push_back(
        AudioWavWriter::DeriveAudioPath(project.output.video_path, pair));
  }
  const std::string metadata = project.output.metadata_path;
  artefacts.dropout_sidecar =
      metadata.substr(0, metadata.size() - std::string(".meta").size()) +
      ".dropouts.meta";
  return artefacts;
}

TEST(DeterministicOutputTest,
     RepeatedFixedSeedRunsProduceByteIdenticalArtefacts) {
  const std::filesystem::path output_dir =
      std::filesystem::temp_directory_path() / "videosynth_determinism";
  std::filesystem::create_directories(output_dir);

  const Project first_project = MakeDeterministicProject(output_dir, "run1");
  const Project second_project = MakeDeterministicProject(output_dir, "run2");

  // Validate once up front so failures surface as messages, not as a bare
  // pipeline failure.
  {
    NullLogger logger;
    ProgressiveFrameSourceProbe probe;
    ProjectValidator validator(&probe, &logger);
    const ValidationResult validation = validator.Validate(first_project);
    for (const std::string& error : validation.errors) {
      ADD_FAILURE() << "Validation error: " << error;
    }
    ASSERT_TRUE(validation.is_valid);
  }

  ASSERT_TRUE(RunPipelineOnce(first_project));
  ASSERT_TRUE(RunPipelineOnce(second_project));

  const RunArtefacts first = ArtefactPaths(first_project);
  const RunArtefacts second = ArtefactPaths(second_project);

  ASSERT_TRUE(std::filesystem::exists(first.composite));
  ASSERT_TRUE(std::filesystem::exists(first.metadata));
  ASSERT_TRUE(std::filesystem::exists(first.dropout_sidecar));

  const std::vector<char> first_composite = ReadFileBytes(first.composite);
  ASSERT_FALSE(first_composite.empty());
  EXPECT_EQ(first_composite, ReadFileBytes(second.composite))
      << "CVBS sample stream is not byte-identical across runs.";

  EXPECT_EQ(ReadFileBytes(first.metadata), ReadFileBytes(second.metadata))
      << "Metadata database is not byte-identical across runs.";

  ASSERT_EQ(first.audio_pairs.size(), second.audio_pairs.size());
  ASSERT_GE(first.audio_pairs.size(), 2U);  // Multi-track fixture.
  for (std::size_t i = 0; i < first.audio_pairs.size(); ++i) {
    ASSERT_TRUE(std::filesystem::exists(first.audio_pairs[i]));
    const std::vector<char> first_audio = ReadFileBytes(first.audio_pairs[i]);
    ASSERT_FALSE(first_audio.empty());
    EXPECT_EQ(first_audio, ReadFileBytes(second.audio_pairs[i]))
        << "Audio track " << i << " is not byte-identical across runs.";
  }

  EXPECT_EQ(ReadFileBytes(first.dropout_sidecar),
            ReadFileBytes(second.dropout_sidecar))
      << "Dropout sidecar is not byte-identical across runs.";

  std::error_code ec;
  std::filesystem::remove_all(output_dir, ec);
}

TEST(DeterministicOutputTest, SingleAndMultiThreadRunsAreByteIdentical) {
  const std::filesystem::path output_dir =
      std::filesystem::temp_directory_path() / "videosynth_thread_determinism";
  std::filesystem::create_directories(output_dir);

  const Project sequential_project =
      MakeDeterministicProject(output_dir, "threads1");
  const Project parallel_project =
      MakeDeterministicProject(output_dir, "threads4");

  ASSERT_TRUE(RunPipelineOnce(sequential_project, 1));
  ASSERT_TRUE(RunPipelineOnce(parallel_project, 4));

  const RunArtefacts sequential = ArtefactPaths(sequential_project);
  const RunArtefacts parallel = ArtefactPaths(parallel_project);

  const std::vector<char> sequential_composite =
      ReadFileBytes(sequential.composite);
  ASSERT_FALSE(sequential_composite.empty());
  EXPECT_EQ(sequential_composite, ReadFileBytes(parallel.composite))
      << "CVBS sample stream differs between 1-thread and 4-thread runs.";

  EXPECT_EQ(ReadFileBytes(sequential.metadata),
            ReadFileBytes(parallel.metadata))
      << "Metadata database differs between 1-thread and 4-thread runs.";

  ASSERT_EQ(sequential.audio_pairs.size(), parallel.audio_pairs.size());
  for (std::size_t i = 0; i < sequential.audio_pairs.size(); ++i) {
    const std::vector<char> sequential_audio =
        ReadFileBytes(sequential.audio_pairs[i]);
    ASSERT_FALSE(sequential_audio.empty());
    EXPECT_EQ(sequential_audio, ReadFileBytes(parallel.audio_pairs[i]))
        << "Audio track " << i << " differs between 1- and 4-thread runs.";
  }

  EXPECT_EQ(ReadFileBytes(sequential.dropout_sidecar),
            ReadFileBytes(parallel.dropout_sidecar))
      << "Dropout sidecar differs between 1-thread and 4-thread runs.";

  std::error_code ec;
  std::filesystem::remove_all(output_dir, ec);
}

// Adds shaped capture noise and a shaped shared (pressing) layer to every
// section of the deterministic project.
void AddLayeredNoise(Project* project) {
  for (Section& section : project->sections) {
    section.noise.spectrum = {{0.0, 0.0}, {3.5, 4.0}, {6.0, 10.0}};
    section.noise.shared.enabled = true;
    section.noise.shared.noise_db = 44.0;
    section.noise.shared.noise_seed = 77;
    section.noise.shared.noise_seed_specified = true;
    section.noise.shared.spectrum = {{0.0, 3.0}, {5.0, -3.0}};
  }
}

TEST(DeterministicOutputTest, LayeredNoiseIsByteIdenticalAcrossThreadCounts) {
  const std::filesystem::path output_dir =
      std::filesystem::temp_directory_path() / "videosynth_layered_noise";
  std::filesystem::create_directories(output_dir);

  Project sequential_project = MakeDeterministicProject(output_dir, "threads1");
  Project parallel_project = MakeDeterministicProject(output_dir, "threads8");
  AddLayeredNoise(&sequential_project);
  AddLayeredNoise(&parallel_project);

  {
    NullLogger logger;
    ProgressiveFrameSourceProbe probe;
    ProjectValidator validator(&probe, &logger);
    const ValidationResult validation = validator.Validate(sequential_project);
    for (const std::string& error : validation.errors) {
      ADD_FAILURE() << "Validation error: " << error;
    }
    ASSERT_TRUE(validation.is_valid);
  }

  ASSERT_TRUE(RunPipelineOnce(sequential_project, 1));
  ASSERT_TRUE(RunPipelineOnce(parallel_project, 8));

  const std::vector<char> sequential_composite =
      ReadFileBytes(sequential_project.output.video_path);
  ASSERT_FALSE(sequential_composite.empty());
  EXPECT_EQ(sequential_composite,
            ReadFileBytes(parallel_project.output.video_path))
      << "CVBS sample stream differs between 1-thread and 8-thread runs.";

  std::error_code ec;
  std::filesystem::remove_all(output_dir, ec);
}

}  // namespace
}  // namespace videosynth
