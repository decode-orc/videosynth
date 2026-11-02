/*
 * File:        test_generation_parity.cpp
 * Module:      gui_tests
 * Purpose:     Functional tests: GUI generation controller output matches
 *              the CLI pipeline byte-for-byte; cancellation leaves no
 *              partial output files
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "generation_controller.h"
#include "videosynth/audio_track_generator.h"
#include "videosynth/audio_wav_writer.h"
#include "videosynth/dropout_injection_stage.h"
#include "videosynth/generation_stage.h"
#include "videosynth/model.h"
#include "videosynth/noise_injection_stage.h"
#include "videosynth/output_stage.h"
#include "videosynth/pipeline.h"
#include "videosynth/progressive_frame_source_probe.h"
#include "videosynth/project_validator.h"
#include "videosynth/yaml_project_emitter.h"
#include "videosynth/yaml_project_parser.h"

namespace videosynth::gui {
namespace {

using RunStatus = GenerationController::RunStatus;

class NullLogger final : public ILogger {
 public:
  void Info(const std::string&) override {}
  void Warning(const std::string&) override {}
  void Error(const std::string&) override {}
  void Debug(const std::string&) override {}
  void Trace(const std::string&) override {}
};

bool PumpUntil(const std::function<bool()>& predicate, int timeout_msec) {
  QElapsedTimer timer;
  timer.start();
  while (!predicate() && timer.elapsed() < timeout_msec) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return predicate();
}

std::string AssetPath(const std::string& relative) {
  return (std::filesystem::path(VIDEOSYNTH_SOURCE_DIR) / relative).string();
}

std::vector<char> ReadFileBytes(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  return std::vector<char>((std::istreambuf_iterator<char>(stream)),
                           std::istreambuf_iterator<char>());
}

// Builds a fixed-seed PAL CAV project exercising biphase codes, seeded
// noise, seeded dropouts, and audio, so every emitted artefact
// (composite/metadata/WAV/dropout sidecar) is deterministic by contract.
Project MakeFixedSeedProject(const std::filesystem::path& output_dir,
                             const std::string& run_tag, int frames) {
  Project project;
  project.name = "gui-parity-" + run_tag;
  project.cvbs_presets.video_standard_preset = Standard::kPal;
  project.cvbs_presets.sample_encoding_preset = "CVBS_U10_4FSC";
  project.cvbs_presets.signal_state_preset = "STANDARD_STABLE_LOCKED";
  project.output.video_path =
      (output_dir / ("gui_parity_" + run_tag + ".cvbs")).string();
  project.output.metadata_path =
      (output_dir / ("gui_parity_" + run_tag + ".meta")).string();
  project.laserdisc.enabled = true;
  project.laserdisc.disc_type = "CAV";

  Section programme;
  programme.name = "Programme";
  programme.type = "progressive";
  programme.section_type = SectionType::kProgrammeArea;
  programme.source =
      AssetPath("videosynth-assets/assets/exr/720x576/75_BARS.exr");
  programme.duration_frames = frames;
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
  programme.noise.noise_db = 40.0;
  programme.noise.noise_spread_db = 4.0;
  programme.noise.noise_seed = 5001;
  programme.noise.noise_seed_specified = true;
  programme.dropouts.random.enabled = true;
  programme.dropouts.random.scale = 8;
  programme.dropouts.random.seed_specified = true;
  programme.dropouts.random.seed = 5002;
  {
    AudioChannelPair pair0;
    pair0.pair = 0;
    pair0.pair_specified = true;
    pair0.left.enabled = true;
    pair0.left.waveform = AudioWaveform::kSine;
    pair0.left.frequency_hz = 1000.0;
    pair0.left.amplitude = 0.4;
    pair0.right.enabled = true;
    pair0.right.waveform = AudioWaveform::kSine;
    pair0.right.frequency_hz = 1000.0;
    pair0.right.amplitude = 0.4;
    // Second pair exercises multi-track output.
    AudioChannelPair pair1;
    pair1.pair = 1;
    pair1.pair_specified = true;
    pair1.left.enabled = true;
    pair1.left.waveform = AudioWaveform::kSquare;
    pair1.left.frequency_hz = 500.0;
    pair1.left.amplitude = 0.3;
    programme.audio_channel_pairs = {pair0, pair1};
  }
  project.sections.push_back(programme);

  return project;
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
  artefacts.dropout_sidecar =
      DropoutInjectionStage::DeriveSidecarPath(project.output.metadata_path);
  return artefacts;
}

// Runs the pipeline from a YAML file exactly as the CLI does (fresh concrete
// stages, file-based Run entry point).
bool RunCliPipeline(const std::string& yaml_path) {
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
  options.project_path = yaml_path;
  options.threads = 1;
  return pipeline.Run(options);
}

TEST(GenerationParityTest, GuiControllerOutputMatchesCliRunOfSameYaml) {
  const std::filesystem::path output_dir =
      std::filesystem::temp_directory_path() / "videosynth_gui_parity";
  std::filesystem::create_directories(output_dir);

  // Two projects identical except for output paths, saved to YAML so both
  // runs start from the on-disk project format.
  const Project cli_project = MakeFixedSeedProject(output_dir, "cli", 6);
  const Project gui_project = MakeFixedSeedProject(output_dir, "gui", 6);

  YamlProjectEmitter emitter;
  const std::string cli_yaml = (output_dir / "cli_project.yaml").string();
  const std::string gui_yaml = (output_dir / "gui_project.yaml").string();
  std::string emit_error;
  ASSERT_TRUE(emitter.EmitFile(cli_project, cli_yaml, &emit_error))
      << emit_error;
  ASSERT_TRUE(emitter.EmitFile(gui_project, gui_yaml, &emit_error))
      << emit_error;

  // CLI-style run: file-based pipeline entry point.
  ASSERT_TRUE(RunCliPipeline(cli_yaml));

  // GUI-style run: parse the YAML as File > Open does, then generate the
  // in-memory project through the controller's default (real) pipeline.
  NullLogger logger;
  YamlProjectParser parser(&logger);
  const ParseResult parse_result = parser.ParseFile(gui_yaml);
  ASSERT_TRUE(parse_result.ok);

  GenerationController controller;
  std::optional<RunStatus> finish_status;
  QObject::connect(
      &controller, &GenerationController::RunFinished,
      [&finish_status](RunStatus status) { finish_status = status; });

  RunOptions gui_options;
  gui_options.threads = 1;
  ASSERT_TRUE(controller.StartGeneration(parse_result.project, gui_options));
  ASSERT_TRUE(PumpUntil([&] { return finish_status.has_value(); }, 120000));
  ASSERT_EQ(finish_status.value_or(RunStatus::kFailed), RunStatus::kSucceeded);

  const RunArtefacts cli = ArtefactPaths(cli_project);
  const RunArtefacts gui = ArtefactPaths(gui_project);

  ASSERT_TRUE(std::filesystem::exists(cli.composite));
  ASSERT_TRUE(std::filesystem::exists(gui.composite));

  const std::vector<char> cli_composite = ReadFileBytes(cli.composite);
  ASSERT_FALSE(cli_composite.empty());
  EXPECT_EQ(cli_composite, ReadFileBytes(gui.composite))
      << "GUI CVBS sample stream differs from the CLI run.";
  EXPECT_EQ(ReadFileBytes(cli.metadata), ReadFileBytes(gui.metadata))
      << "GUI metadata database differs from the CLI run.";
  ASSERT_EQ(cli.audio_pairs.size(), gui.audio_pairs.size());
  ASSERT_GE(cli.audio_pairs.size(), 2U);  // Multi-track fixture.
  for (std::size_t i = 0; i < cli.audio_pairs.size(); ++i) {
    EXPECT_EQ(ReadFileBytes(cli.audio_pairs[i]),
              ReadFileBytes(gui.audio_pairs[i]))
        << "GUI audio track " << i << " differs from the CLI run.";
  }
  EXPECT_EQ(ReadFileBytes(cli.dropout_sidecar),
            ReadFileBytes(gui.dropout_sidecar))
      << "GUI dropout sidecar differs from the CLI run.";

  std::error_code ec;
  std::filesystem::remove_all(output_dir, ec);
}

TEST(GenerationParityTest, CancelledRunLeavesNoPartialOutputFiles) {
  const std::filesystem::path output_dir =
      std::filesystem::temp_directory_path() / "videosynth_gui_cancel";
  std::filesystem::create_directories(output_dir);

  // Long enough that cancellation lands mid-generation.
  const Project project = MakeFixedSeedProject(output_dir, "cancel", 200);

  GenerationController controller;
  std::optional<RunStatus> finish_status;
  bool cancel_requested = false;
  QObject::connect(&controller, &GenerationController::FrameProgress,
                   [&](qulonglong, qulonglong) {
                     if (!cancel_requested) {
                       cancel_requested = true;
                       controller.RequestCancellation();
                     }
                   });
  QObject::connect(
      &controller, &GenerationController::RunFinished,
      [&finish_status](RunStatus status) { finish_status = status; });

  RunOptions options;
  options.threads = 1;
  ASSERT_TRUE(controller.StartGeneration(project, options));
  ASSERT_TRUE(PumpUntil([&] { return finish_status.has_value(); }, 120000));
  ASSERT_TRUE(cancel_requested);
  ASSERT_EQ(finish_status.value_or(RunStatus::kFailed), RunStatus::kCancelled);

  const RunArtefacts artefacts = ArtefactPaths(project);
  EXPECT_FALSE(std::filesystem::exists(artefacts.composite));
  EXPECT_FALSE(std::filesystem::exists(artefacts.metadata));
  for (const std::filesystem::path& audio_path : artefacts.audio_pairs) {
    EXPECT_FALSE(std::filesystem::exists(audio_path));
  }
  EXPECT_FALSE(std::filesystem::exists(artefacts.dropout_sidecar));

  std::error_code ec;
  std::filesystem::remove_all(output_dir, ec);
}

}  // namespace
}  // namespace videosynth::gui
