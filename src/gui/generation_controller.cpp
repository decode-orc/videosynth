/*
 * File:        generation_controller.cpp
 * Module:      gui
 * Purpose:     Runs the generation pipeline on a worker thread for the GUI
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "generation_controller.h"

#include <utility>

#include "videosynth/audio_track_generator.h"
#include "videosynth/dropout_injection_stage.h"
#include "videosynth/generation_stage.h"
#include "videosynth/logger.h"
#include "videosynth/noise_injection_stage.h"
#include "videosynth/output_stage.h"
#include "videosynth/pipeline.h"
#include "videosynth/progressive_frame_source_probe.h"
#include "videosynth/project_validator.h"
#include "videosynth/yaml_project_parser.h"

namespace videosynth::gui {

namespace {

LogLevel ParseLogLevel(const std::string& log_level) {
  if (log_level == "debug") {
    return LogLevel::kDebug;
  }
  if (log_level == "trace") {
    return LogLevel::kTrace;
  }
  return LogLevel::kInfo;
}

}  // namespace

// Invoked synchronously on the worker thread by the pipeline; every callback
// is marshalled to the controller's thread as a queued invocation. If the
// controller is destroyed before delivery, Qt drops the queued call with the
// context object (the destructor joins the worker first, so `controller`
// itself remains valid while the run executes).
class GenerationController::ObserverBridge final : public IPipelineObserver {
 public:
  explicit ObserverBridge(GenerationController* controller)
      : controller_(controller) {}

  void OnStageStarted(const std::string& stage_name) override {
    QMetaObject::invokeMethod(
        controller_,
        [controller = controller_, name = QString::fromStdString(stage_name)] {
          emit controller->StageStarted(name);
        },
        Qt::QueuedConnection);
  }

  void OnFrameProgress(std::size_t frames_completed,
                       std::size_t frames_total) override {
    QMetaObject::invokeMethod(
        controller_,
        [controller = controller_, frames_completed, frames_total] {
          emit controller->FrameProgress(
              static_cast<qulonglong>(frames_completed),
              static_cast<qulonglong>(frames_total));
        },
        Qt::QueuedConnection);
  }

  void OnWarning(const std::string& message) override {
    QMetaObject::invokeMethod(
        controller_,
        [controller = controller_, text = QString::fromStdString(message)] {
          emit controller->WarningReported(text);
        },
        Qt::QueuedConnection);
  }

  void OnRunFinished(PipelineRunStatus status) override {
    RunStatus run_status = RunStatus::kFailed;
    switch (status) {
      case PipelineRunStatus::kSucceeded:
        run_status = RunStatus::kSucceeded;
        break;
      case PipelineRunStatus::kCancelled:
        run_status = RunStatus::kCancelled;
        break;
      case PipelineRunStatus::kFailed:
        run_status = RunStatus::kFailed;
        break;
    }
    QMetaObject::invokeMethod(
        controller_,
        [controller = controller_, run_status] {
          controller->HandleRunFinished(run_status);
        },
        Qt::QueuedConnection);
  }

 private:
  GenerationController* controller_;
};

GenerationController::GenerationController(RunnerFn runner, QObject* parent)
    : QObject(parent), runner_(std::move(runner)) {}

GenerationController::~GenerationController() { JoinWorker(); }

bool GenerationController::StartGeneration(const Project& project,
                                           const RunOptions& options) {
  if (running_) {
    return false;
  }

  JoinWorker();
  cancellation_ = std::make_unique<CancellationToken>();
  running_ = true;
  emit RunStarted();

  worker_ =
      std::thread([this, project, options, cancellation = cancellation_.get()] {
        ObserverBridge bridge(this);
        if (runner_) {
          runner_(project, options, &bridge, cancellation);
        } else {
          RunDefaultPipeline(project, options, &bridge, cancellation);
        }
      });
  return true;
}

void GenerationController::RequestCancellation() {
  if (running_ && cancellation_ != nullptr) {
    cancellation_->RequestCancellation();
  }
}

void GenerationController::RunDefaultPipeline(const Project& project,
                                              const RunOptions& options,
                                              IPipelineObserver* observer,
                                              CancellationToken* cancellation) {
  SpdlogLogger base_logger(ParseLogLevel(options.log_level), options.log_file);
  // Forward every log line to the controller's thread; the queued
  // invocation is thread-safe and ordered per sender thread.
  ForwardingLogger logger(
      &base_logger, ParseLogLevel(options.log_level),
      [this](LogSeverity severity, const std::string& message) {
        QMetaObject::invokeMethod(
            this,
            [controller = this, severity,
             text = QString::fromStdString(message)] {
              emit controller->LogMessageReceived(static_cast<int>(severity),
                                                  text);
            },
            Qt::QueuedConnection);
      });

  YamlProjectParser parser(&logger);
  ProgressiveFrameSourceProbe probe;
  FfprobeAudioFileProbe audio_file_probe;
  ProjectValidator validator(&probe, &logger, &audio_file_probe);
  GenerationStage generation(&logger);
  NoiseInjectionStage noise_injection(&logger);
  DropoutInjectionStage dropout_injection(&logger);
  OutputStage output(&logger);
  AudioTrackGenerator audio_generator(&logger);

  VideoSynthPipeline pipeline(&parser, &validator, &generation,
                              &noise_injection, &dropout_injection, &output,
                              &logger, &audio_generator);
  pipeline.RunProject(project, options, observer, cancellation);
}

void GenerationController::HandleRunFinished(RunStatus status) {
  running_ = false;
  emit RunFinished(status);
}

void GenerationController::JoinWorker() {
  if (worker_.joinable()) {
    worker_.join();
  }
}

}  // namespace videosynth::gui
