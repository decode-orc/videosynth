/*
 * File:        pipeline.cpp
 * Module:      pipeline
 * Purpose:     Orchestrates parsing, validation, generation, and output stages.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "videosynth/pipeline.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <utility>
#include <vector>

#include "videosynth/audio_track_generator.h"
#include "videosynth/channel_stage.h"
#include "videosynth/chroma_error_stage.h"
#include "videosynth/dropout_injection_stage.h"
#include "videosynth/fixed_point.h"
#include "videosynth/frame_synthesis_pool.h"
#include "videosynth/noise_injection_stage.h"
#include "videosynth/path_resolution.h"
#include "videosynth/timing_constants.h"

namespace videosynth {

namespace {

std::size_t ComputeBatchFrameCount(const Project& project) {
  constexpr std::size_t kTargetBatchBytes = 64ULL * 1024ULL * 1024ULL;
  const std::size_t frame_span = static_cast<std::size_t>(
      SamplesPerFrame4fsc(project.cvbs_presets.video_standard_preset));
  if (frame_span == 0U) {
    return 1U;
  }

  const std::size_t bytes_per_frame = frame_span * sizeof(SampleFixed) * 2U;
  if (bytes_per_frame == 0U) {
    return 1U;
  }

  const std::size_t computed =
      std::max<std::size_t>(1U, kTargetBatchBytes / bytes_per_frame);
  return std::min<std::size_t>(computed, 64U);
}

std::size_t ComputeProgressInterval(std::size_t total_frames) {
  if (total_frames <= 120U) {
    return 1U;
  }
  constexpr std::size_t kTargetProgressMessages = 40U;
  return std::max<std::size_t>(
      1U,
      (total_frames + kTargetProgressMessages - 1U) / kTargetProgressMessages);
}

}  // namespace

VideoSynthPipeline::VideoSynthPipeline(IProjectParser* parser,
                                       IProjectValidator* validator,
                                       IGenerationStage* generation,
                                       NoiseInjectionStage* noise_injection,
                                       DropoutInjectionStage* dropout_injection,
                                       IOutputStage* output, ILogger* logger,
                                       AudioTrackGenerator* audio_generator)
    : parser_(parser),
      validator_(validator),
      generation_(generation),
      noise_injection_(noise_injection),
      dropout_injection_(dropout_injection),
      output_(output),
      logger_(logger),
      audio_generator_(audio_generator) {}

bool VideoSynthPipeline::Run(const RunOptions& options,
                             IPipelineObserver* observer,
                             CancellationToken* cancellation) {
  logger_->Info("Starting pipeline: parse -> validate -> generate -> output");

  const ParseResult parse_result = parser_->ParseFile(options.project_path);
  if (!parse_result.ok) {
    for (const std::string& error : parse_result.errors) {
      logger_->Error(error);
    }
    if (observer != nullptr) {
      observer->OnRunFinished(PipelineRunStatus::kFailed);
    }
    return false;
  }

  // Resolve {name}/path logical-root tokens against the run's asset roots.
  // Plain relative paths keep their historical working-directory-relative
  // behaviour (anchor_unset = false), so existing projects are unaffected.
  const std::string project_dir =
      std::filesystem::path(options.project_path).parent_path().string();
  const Project resolved =
      ResolveProjectPaths(parse_result.project, options.asset_roots,
                          project_dir, /*anchor_unset=*/false);
  return RunProject(resolved, options, observer, cancellation);
}

bool VideoSynthPipeline::RunProject(const Project& project,
                                    const RunOptions& options,
                                    IPipelineObserver* observer,
                                    CancellationToken* cancellation) {
  // Reports the terminal status exactly once and converts it to the boolean
  // pipeline result.
  auto FinishRun = [&](PipelineRunStatus status) {
    if (observer != nullptr) {
      observer->OnRunFinished(status);
    }
    return status == PipelineRunStatus::kSucceeded;
  };

  auto NotifyStage = [&](const char* stage_name) {
    if (observer != nullptr) {
      observer->OnStageStarted(stage_name);
    }
  };

  auto IsCancelled = [&]() {
    return cancellation != nullptr && cancellation->IsCancellationRequested();
  };

  NotifyStage("validate");
  const ValidationResult validation_result = validator_->Validate(project);
  for (const std::string& warning : validation_result.warnings) {
    logger_->Warning(warning);
    if (observer != nullptr) {
      observer->OnWarning(warning);
    }
  }
  if (!validation_result.is_valid) {
    for (const std::string& error : validation_result.errors) {
      logger_->Error(error);
    }
    return FinishRun(PipelineRunStatus::kFailed);
  }

  if (options.validate_only) {
    logger_->Info("Validation successful.");
    return FinishRun(PipelineRunStatus::kSucceeded);
  }

  if (IsCancelled()) {
    logger_->Info("Pipeline run cancelled before generation started.");
    return FinishRun(PipelineRunStatus::kCancelled);
  }

  NotifyStage("generate");
  std::vector<IGenerationStage::FrameScheduleItem> schedule;
  std::vector<std::string> generation_errors;
  if (!generation_->BuildFrameSchedule(project, &schedule,
                                       &generation_errors)) {
    for (const std::string& error : generation_errors) {
      logger_->Error(error);
    }
    return FinishRun(PipelineRunStatus::kFailed);
  }

  const std::size_t total_disc_frames = schedule.size();

  std::vector<std::string> output_errors;
  if (!output_->BeginWrite(project, total_disc_frames, &output_errors)) {
    for (const std::string& error : output_errors) {
      logger_->Error(error);
    }
    return FinishRun(PipelineRunStatus::kFailed);
  }

  if (dropout_injection_ != nullptr) {
    std::vector<std::string> dropout_errors;
    if (!dropout_injection_->Begin(project, &dropout_errors)) {
      for (const std::string& error : dropout_errors) {
        logger_->Error(error);
      }
      std::vector<std::string> cleanup_errors;
      output_->FinalizeWrite(&cleanup_errors);
      return FinishRun(PipelineRunStatus::kFailed);
    }
  }

  auto CloseOutputSessionOnFailure = [&]() {
    std::vector<std::string> cleanup_errors;
    output_->FinalizeWrite(&cleanup_errors);
  };

  // -------------------------------------------------------------------
  // Optional frame-locked audio tracks. Active when an audio generator is
  // supplied and at least one section declares an audio channel pair. The
  // generator is driven with the output-order frame sequence so every stored
  // frame carries the correct per-frame sample count and each channel pair
  // stays sample-accurately frame-locked to the output stream.
  // -------------------------------------------------------------------
  const bool audio_enabled =
      audio_generator_ != nullptr && ProjectEnablesAudio(project);
  if (audio_enabled) {
    // The section and source frame of each stored output frame, in output
    // order; video-soundtrack pairs follow the source frames.
    std::vector<AudioOutputFrame> output_frames;
    output_frames.reserve(total_disc_frames);
    for (const IGenerationStage::FrameScheduleItem& item : schedule) {
      output_frames.push_back({item.section, item.source_frame_index});
    }

    std::vector<std::string> audio_errors;
    if (!audio_generator_->Begin(project, output_frames, &audio_errors)) {
      for (const std::string& error : audio_errors) {
        logger_->Error(error);
      }
      CloseOutputSessionOnFailure();
      return FinishRun(PipelineRunStatus::kFailed);
    }
  }

  // Monotonic output-frame counter driving audio emission. Incremented for
  // every stored output frame in output order.
  std::size_t audio_output_index = 0U;

  // Aborts all in-progress output artefacts after a cancellation request:
  // video/chroma/metadata via the output stage, the WAV tracks, and the
  // dropout sidecar. Each abort is a no-op when its session is not open.
  auto CancelRun = [&]() {
    output_->AbortWrite();
    if (audio_enabled) {
      audio_generator_->Abort();
    }
    if (dropout_injection_ != nullptr) {
      dropout_injection_->Abort();
    }
    logger_->Info(
        "Pipeline run cancelled; in-progress output files were removed.");
    return FinishRun(PipelineRunStatus::kCancelled);
  };

  auto NotifyProgress = [&](std::size_t frames_completed) {
    if (observer != nullptr) {
      observer->OnFrameProgress(frames_completed, total_disc_frames);
    }
  };

  // Emits the next output frame's audio across all channel pairs, in output
  // order. Returns false and logs on failure.
  auto EmitAudioFrame = [&]() -> bool {
    std::vector<std::string> audio_errors;
    if (!audio_generator_->EmitFrame(audio_output_index, &audio_errors)) {
      for (const std::string& error : audio_errors) {
        logger_->Error(error);
      }
      return false;
    }
    ++audio_output_index;
    return true;
  };

  // Reports a pool run that stopped before consuming every item: cancellation
  // takes precedence, otherwise the job or consumer errors are logged and the
  // output session is closed.
  auto FinishFailedPoolRun = [&](const std::vector<std::string>& pool_errors,
                                 bool consumer_failed) {
    if (IsCancelled()) {
      return CancelRun();
    }
    for (const std::string& error : pool_errors) {
      logger_->Error(error);
    }
    if (!consumer_failed && pool_errors.empty()) {
      logger_->Error("Frame synthesis pool stopped without completing.");
    }
    CloseOutputSessionOnFailure();
    return FinishRun(PipelineRunStatus::kFailed);
  };

  // Resolve the frame synthesis worker count. The pool is used when more than
  // one thread is requested: every frame is a deterministic function of
  // (project, schedule, frame index), so synthesis order never affects the
  // emitted bytes.
  const unsigned synthesis_threads =
      FrameSynthesisPool::ResolveThreadCount(options.threads);
  const bool parallel_synthesis = synthesis_threads > 1U;

  if (parallel_synthesis) {
    // -------------------------------------------------------------------
    // Worker-pool frame synthesis (threads > 1).
    //
    // Frames are synthesised out of order on the pool using the enriched
    // schedule and per-frame noise/dropout seeds, then reassembled in frame
    // order on this thread for output, sidecar rows, and audio — so the
    // emitted bytes are identical to the sequential path.
    // -------------------------------------------------------------------
    const std::size_t progress_interval =
        ComputeProgressInterval(total_disc_frames);

    logger_->Info("Generating and writing " +
                  std::to_string(total_disc_frames) + " frame(s) using " +
                  std::to_string(synthesis_threads) + " synthesis threads.");

    auto SynthesizeFrameJob = [&](std::size_t disc_frame,
                                  SynthesizedFrame* out_frame) {
      std::vector<std::string> job_errors;
      if (!generation_->GenerateFrameBatch(project, schedule, disc_frame, 1U,
                                           &out_frame->y_mv, &out_frame->c_mv,
                                           &job_errors)) {
        out_frame->ok = false;
        out_frame->errors = job_errors;
        return;
      }
      channel_.ApplyChannel(project, schedule, disc_frame, 1U, &out_frame->y_mv,
                            &out_frame->c_mv);
      chroma_error_.ApplyChromaError(project, schedule, disc_frame, 1U,
                                     &out_frame->c_mv);
      if (noise_injection_ != nullptr) {
        noise_injection_->InjectNoise(project, schedule, disc_frame, 1U,
                                      &out_frame->y_mv, &out_frame->c_mv);
      }
      if (dropout_injection_ != nullptr) {
        dropout_injection_->ComputeFrameDropouts(
            project, schedule, disc_frame, &out_frame->y_mv, &out_frame->c_mv,
            0U, &out_frame->dropout_rows);
      }
      // Output encoding is a pure function of the frame's samples and the
      // session state resolved in BeginWrite, so it runs here rather than on
      // the single consumer thread.
      if (!output_->EncodeFrame(out_frame->y_mv, out_frame->c_mv,
                                &out_frame->encoded, &job_errors)) {
        out_frame->ok = false;
        out_frame->errors = job_errors;
      }
    };

    std::size_t next_progress_mark = progress_interval;
    bool consumer_failed = false;
    auto ConsumeFrame = [&](std::size_t disc_frame,
                            SynthesizedFrame& frame) -> bool {
      output_errors.clear();
      if (!output_->AppendEncodedFrame(frame.encoded, &output_errors)) {
        for (const std::string& error : output_errors) {
          logger_->Error(error);
        }
        consumer_failed = true;
        return false;
      }

      if (dropout_injection_ != nullptr) {
        dropout_injection_->CommitSidecarRows(frame.dropout_rows);
      }

      if (audio_enabled && !EmitAudioFrame()) {
        consumer_failed = true;
        return false;
      }

      NotifyProgress(disc_frame + 1U);
      if (disc_frame + 1U >= next_progress_mark ||
          disc_frame + 1U == total_disc_frames) {
        logger_->Info("Pipeline progress: " + std::to_string(disc_frame + 1U) +
                      "/" + std::to_string(total_disc_frames) +
                      " frame(s) written.");
        next_progress_mark += progress_interval;
      }
      return true;
    };

    FrameSynthesisPool pool(synthesis_threads);
    std::vector<std::string> pool_errors;
    if (!pool.RunOrdered(total_disc_frames, SynthesizeFrameJob, ConsumeFrame,
                         cancellation, &pool_errors)) {
      return FinishFailedPoolRun(pool_errors, consumer_failed);
    }
  } else {
    // -------------------------------------------------------------------
    // Standard batched loop (single-threaded synthesis).
    // -------------------------------------------------------------------
    const std::size_t batch_frame_count = ComputeBatchFrameCount(project);
    const std::size_t progress_interval =
        ComputeProgressInterval(total_disc_frames);

    logger_->Info(
        "Generating and writing " + std::to_string(total_disc_frames) +
        " frame(s) in batches of " + std::to_string(batch_frame_count) + ".");

    std::size_t processed_frames = 0U;
    std::size_t next_progress_mark = progress_interval;

    // Reused across batches so steady-state batches resize to an unchanged
    // length instead of zero-filling fresh allocations.
    std::vector<SampleFixed> y_mv;
    std::vector<SampleFixed> c_mv;

    while (processed_frames < total_disc_frames) {
      if (IsCancelled()) {
        return CancelRun();
      }

      const std::size_t frames_this_batch =
          std::min(batch_frame_count, total_disc_frames - processed_frames);

      generation_errors.clear();
      if (!generation_->GenerateFrameBatch(project, schedule, processed_frames,
                                           frames_this_batch, &y_mv, &c_mv,
                                           &generation_errors)) {
        CloseOutputSessionOnFailure();
        for (const std::string& error : generation_errors) {
          logger_->Error(error);
        }
        return FinishRun(PipelineRunStatus::kFailed);
      }

      channel_.ApplyChannel(project, schedule, processed_frames,
                            frames_this_batch, &y_mv, &c_mv);
      chroma_error_.ApplyChromaError(project, schedule, processed_frames,
                                     frames_this_batch, &c_mv);

      if (noise_injection_ != nullptr) {
        noise_injection_->InjectNoise(project, schedule, processed_frames,
                                      frames_this_batch, &y_mv, &c_mv);
      }

      if (dropout_injection_ != nullptr) {
        dropout_injection_->InjectDropouts(project, schedule, processed_frames,
                                           frames_this_batch, &y_mv, &c_mv);
      }

      output_errors.clear();
      if (!output_->AppendSamples(y_mv, c_mv, &output_errors)) {
        CloseOutputSessionOnFailure();
        for (const std::string& error : output_errors) {
          logger_->Error(error);
        }
        return FinishRun(PipelineRunStatus::kFailed);
      }

      if (audio_enabled) {
        for (std::size_t i = 0U; i < frames_this_batch; ++i) {
          if (!EmitAudioFrame()) {
            CloseOutputSessionOnFailure();
            return FinishRun(PipelineRunStatus::kFailed);
          }
        }
      }

      processed_frames += frames_this_batch;
      NotifyProgress(processed_frames);
      if (processed_frames >= next_progress_mark ||
          processed_frames == total_disc_frames) {
        logger_->Info("Pipeline progress: " + std::to_string(processed_frames) +
                      "/" + std::to_string(total_disc_frames) +
                      " frame(s) written.");
        next_progress_mark += progress_interval;
      }
    }
  }

  NotifyStage("finalize");
  output_errors.clear();
  if (!output_->FinalizeWrite(&output_errors)) {
    for (const std::string& error : output_errors) {
      logger_->Error(error);
    }
    return FinishRun(PipelineRunStatus::kFailed);
  }

  if (audio_enabled) {
    std::vector<std::string> audio_errors;
    if (!audio_generator_->Finalize(&audio_errors)) {
      for (const std::string& error : audio_errors) {
        logger_->Error(error);
      }
      return FinishRun(PipelineRunStatus::kFailed);
    }
  }

  if (dropout_injection_ != nullptr) {
    std::vector<std::string> dropout_errors;
    if (!dropout_injection_->Finalize(&dropout_errors)) {
      for (const std::string& error : dropout_errors) {
        logger_->Error(error);
      }
      return FinishRun(PipelineRunStatus::kFailed);
    }
  }

  logger_->Info("Generation completed successfully.");
  return FinishRun(PipelineRunStatus::kSucceeded);
}

}  // namespace videosynth
