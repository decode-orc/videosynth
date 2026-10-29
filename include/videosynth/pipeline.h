/*
 * File:        pipeline.h
 * Module:      pipeline
 * Purpose:     Orchestrates parsing, validation, generation, and output stages.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <cstddef>
#include <vector>

#include "videosynth/audio_track_generator.h"
#include "videosynth/channel_stage.h"
#include "videosynth/chroma_error_stage.h"
#include "videosynth/dropout_injection_stage.h"
#include "videosynth/interfaces.h"
#include "videosynth/noise_injection_stage.h"

namespace videosynth {

// Thread-safety: VideoSynthPipeline is NOT thread-safe. A whole pipeline run
// (Run or RunProject) executes on a single thread, which may be any thread —
// a worker thread is fine; there is no main-thread affinity. Run/RunProject
// must not be called concurrently from multiple threads on the same instance
// because the injected single-owner stage collaborators (IOutputStage,
// DropoutInjectionStage's sidecar session, AudioWavWriter) are accessed
// without synchronization. ILogger, IProjectParser, and IProjectValidator
// (and AudioTrackGenerator)
// implementations must remain thread-safe per their interface contracts.
//
// When RunOptions::threads resolves to more than one worker, the pipeline
// internally spawns a FrameSynthesisPool for the run: frame synthesis
// (IGenerationStage::GenerateFrameBatch, ChannelStage::ApplyChannel,
// ChromaErrorStage::ApplyChromaError, NoiseInjectionStage::InjectNoise,
// DropoutInjectionStage::ComputeFrameDropouts) executes concurrently on pool
// workers, while output, sidecar-row commits, audio, and observer callbacks
// stay on the run thread in frame order. Output is byte-identical to the
// sequential path.
//
// Relative paths in the project (section sources, output paths) are resolved
// against the process working directory by the stages; callers running on
// worker threads must pass already-resolved paths.
class VideoSynthPipeline {
 public:
  // audio_generator is an optional collaborator (nullable, like noise_injection
  // and dropout_injection). When non-null and the project declares audio
  // channel pairs, one frame-locked stereo WAV track per channel pair is
  // emitted alongside the CVBS output.
  VideoSynthPipeline(IProjectParser* parser, IProjectValidator* validator,
                     IGenerationStage* generation,
                     NoiseInjectionStage* noise_injection,
                     DropoutInjectionStage* dropout_injection,
                     IOutputStage* output, ILogger* logger,
                     AudioTrackGenerator* audio_generator = nullptr);

  // Orchestrates the full pipeline from a project file:
  //   parse -> RunProject (validate -> generate -> channel -> chroma error ->
  //   noise -> dropout -> output).
  //
  // Args:
  //   options: Contains project path and runtime configuration.
  //   observer: Optional (nullable) progress observer; see RunProject.
  //   cancellation: Optional (nullable) cancellation token; see RunProject.
  //
  // Returns:
  //   true if the entire pipeline completed successfully, false on any error
  //   or cancellation. Errors are logged via the logger_ and also returned
  //   through the IProjectParser, IProjectValidator, IGenerationStage, and
  //   IOutputStage interfaces' error output parameters.
  bool Run(const RunOptions& options, IPipelineObserver* observer = nullptr,
           CancellationToken* cancellation = nullptr);

  // Orchestrates the pipeline for an already-parsed project:
  //   validate -> generate -> channel -> chroma error -> noise -> dropout ->
  //   output.
  //
  // The channel and chroma error stages are owned by the pipeline rather than
  // injected: each is immutable during a run and a no-op unless the project
  // declares its block (`channel:`, `chroma_error:`). The chroma error stage
  // draws its unseeded-jitter base seed when the pipeline is constructed.
  //
  // Relative paths inside the project (section sources, output targets) are
  // used as-is; callers must resolve them before invoking this method.
  //
  // Args:
  //   project: Fully-populated project model to run.
  //   options: Runtime configuration (validate_only etc.); project_path is
  //     ignored on this entry point.
  //   observer: Optional (nullable) progress observer. Callbacks are invoked
  //     synchronously on the calling thread; OnRunFinished fires exactly once.
  //   cancellation: Optional (nullable) cooperative cancellation token,
  //     polled between frame batches. On cancellation all in-progress output
  //     artefacts (video/chroma/metadata/WAV/dropout-sidecar files) are
  //     removed, OnRunFinished(kCancelled) is reported, and false is
  //     returned.
  //
  // Returns:
  //   true if the entire pipeline completed successfully, false on any error
  //   or cancellation.
  bool RunProject(const Project& project, const RunOptions& options,
                  IPipelineObserver* observer = nullptr,
                  CancellationToken* cancellation = nullptr);

 private:
  IProjectParser* parser_;
  IProjectValidator* validator_;
  IGenerationStage* generation_;
  NoiseInjectionStage* noise_injection_;
  DropoutInjectionStage* dropout_injection_;
  IOutputStage* output_;
  ILogger* logger_;
  AudioTrackGenerator* audio_generator_;
  ChannelStage channel_;
  ChromaErrorStage chroma_error_;
};

}  // namespace videosynth
