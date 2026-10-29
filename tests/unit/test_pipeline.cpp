/*
 * File:        test_pipeline.cpp
 * Module:      pipeline_tests
 * Purpose:     Validates pipeline control flow using deterministic mocks.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "videosynth/channel_stage.h"
#include "videosynth/chroma_error_stage.h"
#include "videosynth/noise_injection_stage.h"
#include "videosynth/pipeline.h"
#include "videosynth/timing_constants.h"

namespace videosynth {
namespace {

class MockParser final : public IProjectParser {
 public:
  ParseResult result;
  std::string last_path;
  std::vector<std::string>* call_log = nullptr;

  ParseResult ParseFile(const std::string& path) override {
    last_path = path;
    if (call_log != nullptr) {
      call_log->push_back("parse");
    }
    return result;
  }
};

class MockValidator final : public IProjectValidator {
 public:
  ValidationResult result;
  bool called = false;
  std::vector<std::string>* call_log = nullptr;

  ValidationResult Validate(const Project&) override {
    called = true;
    if (call_log != nullptr) {
      call_log->push_back("validate");
    }
    return result;
  }
};

class MockGeneration final : public IGenerationStage {
 public:
  // Atomic: GenerateFrameBatch may run concurrently on pool worker threads
  // when RunOptions::threads > 1.
  std::atomic<bool> called{false};
  // When true, embeds start_frame as a unique marker in every sample so
  // tests can verify which disc frame produced each output frame.
  bool embed_frame_id = false;
  // Frames at or beyond this index fail with an error message.
  std::size_t fail_at_frame = std::numeric_limits<std::size_t>::max();
  std::vector<std::string>* call_log = nullptr;

  bool BuildFrameSchedule(const Project& project,
                          std::vector<FrameScheduleItem>* out_schedule,
                          std::vector<std::string>* errors) override {
    if (call_log != nullptr) {
      call_log->push_back("build_schedule");
    }
    out_schedule->clear();
    for (const Section& s : project.sections) {
      for (int i = 0; i < s.duration_frames; ++i) {
        out_schedule->push_back(
            FrameScheduleItem{.section = &s, .source_frame_index = i});
      }
    }
    errors->clear();
    return true;
  }

  bool GenerateFrameBatch(const Project&, const std::vector<FrameScheduleItem>&,
                          std::size_t start_frame, std::size_t frame_count,
                          std::vector<SampleFixed>* out_y_mv,
                          std::vector<SampleFixed>* out_c_mv,
                          std::vector<std::string>* errors) override {
    called = true;
    if (call_log != nullptr) {
      call_log->push_back("generate_batch");
    }
    if (fail_at_frame < start_frame + frame_count &&
        fail_at_frame >= start_frame) {
      errors->push_back("mock generation failure");
      return false;
    }
    out_y_mv->clear();
    out_c_mv->clear();
    for (std::size_t i = 0; i < frame_count; ++i) {
      // 8 samples per frame; value = disc-frame index when embed_frame_id.
      const SampleFixed val =
          embed_frame_id ? static_cast<SampleFixed>(start_frame + i) : 0;
      for (int s = 0; s < 8; ++s) {
        out_y_mv->push_back(val);
        out_c_mv->push_back(0);
      }
    }
    errors->clear();
    return true;
  }

  bool Generate(const Project&, std::vector<SampleFixed>* out_y_mv,
                std::vector<SampleFixed>* out_c_mv,
                std::vector<std::string>* errors) override {
    called = true;
    out_y_mv->assign(8, 0);
    out_c_mv->assign(8, 0);
    errors->clear();
    return true;
  }
};

class MockOutput final : public IOutputStage {
 public:
  bool called = false;
  std::size_t begin_write_frame_count = 0;
  // Stores the first sample of each appended y_mv chunk.
  std::vector<SampleFixed> appended_first_samples;
  std::vector<std::string>* call_log = nullptr;
  // When non-null, requests cancellation on this token after the first
  // AppendSamples call, simulating a cancel arriving mid-run.
  CancellationToken* cancel_after_first_append = nullptr;

  bool BeginWrite(const Project&, std::size_t expected_frame_count,
                  std::vector<std::string>* errors) override {
    begin_write_frame_count = expected_frame_count;
    if (call_log != nullptr) {
      call_log->push_back("begin_write");
    }
    errors->clear();
    return true;
  }

  bool AppendSamples(const std::vector<SampleFixed>& y_mv,
                     const std::vector<SampleFixed>&,
                     std::vector<std::string>* errors) override {
    called = true;
    if (call_log != nullptr) {
      call_log->push_back("append_samples");
    }
    if (!y_mv.empty()) {
      appended_first_samples.push_back(y_mv[0]);
    }
    if (cancel_after_first_append != nullptr) {
      cancel_after_first_append->RequestCancellation();
    }
    errors->clear();
    return true;
  }

  // Encoding is the identity mapping here: the first code of each frame
  // carries the disc-frame marker MockGeneration embeds, so the recorded
  // output order is checked the same way on the encoded path.
  bool EncodeFrame(const std::vector<SampleFixed>& y_mv,
                   const std::vector<SampleFixed>&, EncodedFrame* out_frame,
                   std::vector<std::string>* errors) const override {
    out_frame->luma_codes.clear();
    for (const SampleFixed sample : y_mv) {
      out_frame->luma_codes.push_back(static_cast<std::int16_t>(sample));
    }
    out_frame->chroma_codes.clear();
    out_frame->has_nonstandard = false;
    errors->clear();
    return true;
  }

  bool AppendEncodedFrame(const EncodedFrame& frame,
                          std::vector<std::string>* errors) override {
    called = true;
    if (call_log != nullptr) {
      call_log->push_back("append_samples");
    }
    if (!frame.luma_codes.empty()) {
      appended_first_samples.push_back(
          static_cast<SampleFixed>(frame.luma_codes[0]));
    }
    if (cancel_after_first_append != nullptr) {
      cancel_after_first_append->RequestCancellation();
    }
    errors->clear();
    return true;
  }

  bool FinalizeWrite(std::vector<std::string>* errors) override {
    finalize_called = true;
    if (call_log != nullptr) {
      call_log->push_back("finalize_write");
    }
    errors->clear();
    return true;
  }

  void AbortWrite() override { abort_called = true; }

  bool Write(const Project&, const std::vector<SampleFixed>&,
             const std::vector<SampleFixed>&,
             std::vector<std::string>* errors) override {
    called = true;
    errors->clear();
    return true;
  }

  bool finalize_called = false;
  bool abort_called = false;
};

class MockLogger final : public ILogger {
 public:
  std::vector<std::string> infos;
  std::vector<std::string> warnings;
  std::vector<std::string> errors;
  std::vector<std::string> debugs;
  std::vector<std::string> traces;

  void Info(const std::string& message) override { infos.push_back(message); }
  void Warning(const std::string& message) override {
    warnings.push_back(message);
  }
  void Error(const std::string& message) override { errors.push_back(message); }
  void Debug(const std::string& message) override { debugs.push_back(message); }
  void Trace(const std::string& message) override { traces.push_back(message); }
};

Project MakeProject(int duration_frames = 1) {
  Project p;
  p.cvbs_presets.video_standard_preset = Standard::kPal;
  p.cvbs_presets.sample_encoding_preset = "CVBS_U10_4FSC";
  p.cvbs_presets.signal_state_preset = "STANDARD_STABLE_LOCKED";
  p.output.video_path = "/tmp/videosynth_pipeline_test.cvbs";
  p.output.metadata_path = "/tmp/videosynth_pipeline_test.meta";
  p.sections.push_back(Section{.name = "Valid",
                               .type = "progressive",
                               .line_injections = {},
                               .source = "/tmp/videosynth_pipeline_test.exr",
                               .duration_frames = duration_frames});
  return p;
}

TEST(PipelineTest, ValidateOnlyStopsBeforeGeneration) {
  MockParser parser;
  parser.result.ok = true;
  parser.result.project = MakeProject();

  MockValidator validator;
  validator.result.is_valid = true;

  MockGeneration generation;
  MockOutput output;
  MockLogger logger;

  VideoSynthPipeline pipeline(&parser, &validator, &generation, nullptr,
                              nullptr, &output, &logger);

  RunOptions options;
  options.project_path = "project.yaml";
  options.validate_only = true;

  EXPECT_TRUE(pipeline.Run(options));
  EXPECT_TRUE(validator.called);
  EXPECT_FALSE(generation.called);
  EXPECT_FALSE(output.called);
}

TEST(PipelineTest, FullRunCallsGenerationAndOutput) {
  MockParser parser;
  parser.result.ok = true;
  parser.result.project = MakeProject();

  MockValidator validator;
  validator.result.is_valid = true;

  MockGeneration generation;
  MockOutput output;
  MockLogger logger;

  VideoSynthPipeline pipeline(&parser, &validator, &generation, nullptr,
                              nullptr, &output, &logger);

  RunOptions options;
  options.project_path = "project.yaml";

  EXPECT_TRUE(pipeline.Run(options));
  EXPECT_TRUE(generation.called);
  EXPECT_TRUE(output.called);
}

TEST(PipelineTest, ValidationFailureStopsPipeline) {
  MockParser parser;
  parser.result.ok = true;
  parser.result.project = MakeProject();

  MockValidator validator;
  validator.result.is_valid = false;
  validator.result.errors = {"bad config"};

  MockGeneration generation;
  MockOutput output;
  MockLogger logger;

  VideoSynthPipeline pipeline(&parser, &validator, &generation, nullptr,
                              nullptr, &output, &logger);

  RunOptions options;
  options.project_path = "project.yaml";

  EXPECT_FALSE(pipeline.Run(options));
  EXPECT_FALSE(generation.called);
  EXPECT_FALSE(output.called);
  ASSERT_EQ(logger.errors.size(), 1U);
}

// ---------------------------------------------------------------------------
// In-memory execution path (RunProject), observer, and cancellation tests
// ---------------------------------------------------------------------------

class RecordingObserver final : public IPipelineObserver {
 public:
  std::vector<std::string> stages;
  std::vector<std::pair<std::size_t, std::size_t>> progress;
  std::vector<std::string> warnings;
  std::vector<PipelineRunStatus> statuses;

  void OnStageStarted(const std::string& stage_name) override {
    stages.push_back(stage_name);
  }
  void OnFrameProgress(std::size_t frames_completed,
                       std::size_t frames_total) override {
    progress.emplace_back(frames_completed, frames_total);
  }
  void OnWarning(const std::string& message) override {
    warnings.push_back(message);
  }
  void OnRunFinished(PipelineRunStatus status) override {
    statuses.push_back(status);
  }
};

TEST(PipelineTest, RunProjectMatchesFileBasedStageCallSequence) {
  const Project project = MakeProject(3);

  // File-based path.
  std::vector<std::string> file_calls;
  MockParser parser;
  parser.result.ok = true;
  parser.result.project = project;
  parser.call_log = &file_calls;
  MockValidator validator;
  validator.result.is_valid = true;
  validator.call_log = &file_calls;
  MockGeneration generation;
  generation.call_log = &file_calls;
  MockOutput output;
  output.call_log = &file_calls;
  MockLogger logger;

  VideoSynthPipeline file_pipeline(&parser, &validator, &generation, nullptr,
                                   nullptr, &output, &logger);
  RunOptions options;
  options.project_path = "project.yaml";
  EXPECT_TRUE(file_pipeline.Run(options));

  // In-memory path with an independent set of mocks.
  std::vector<std::string> memory_calls;
  MockParser unused_parser;
  MockValidator memory_validator;
  memory_validator.result.is_valid = true;
  memory_validator.call_log = &memory_calls;
  MockGeneration memory_generation;
  memory_generation.call_log = &memory_calls;
  MockOutput memory_output;
  memory_output.call_log = &memory_calls;

  VideoSynthPipeline memory_pipeline(&unused_parser, &memory_validator,
                                     &memory_generation, nullptr, nullptr,
                                     &memory_output, &logger);
  EXPECT_TRUE(memory_pipeline.RunProject(project, options));

  // The file-based path is exactly parse followed by the in-memory sequence.
  ASSERT_FALSE(file_calls.empty());
  EXPECT_EQ(file_calls.front(), "parse");
  EXPECT_EQ(std::vector<std::string>(file_calls.begin() + 1, file_calls.end()),
            memory_calls);
  EXPECT_TRUE(unused_parser.last_path.empty());
}

TEST(PipelineTest, ObserverReceivesMonotonicProgressAndSuccessStatus) {
  // 30 frames force multiple batches with the PAL batch size, so progress is
  // reported more than once.
  const Project project = MakeProject(30);

  MockParser parser;
  MockValidator validator;
  validator.result.is_valid = true;
  validator.result.warnings = {"minor issue"};
  MockGeneration generation;
  MockOutput output;
  MockLogger logger;
  RecordingObserver observer;

  VideoSynthPipeline pipeline(&parser, &validator, &generation, nullptr,
                              nullptr, &output, &logger);
  RunOptions options;
  EXPECT_TRUE(pipeline.RunProject(project, options, &observer));

  ASSERT_GE(observer.progress.size(), 2U);
  std::size_t previous_completed = 0U;
  for (const auto& [completed, total] : observer.progress) {
    EXPECT_EQ(total, 30U);
    EXPECT_GE(completed, previous_completed);
    previous_completed = completed;
  }
  EXPECT_EQ(observer.progress.back().first, 30U);

  EXPECT_EQ(observer.stages,
            (std::vector<std::string>{"validate", "generate", "finalize"}));
  EXPECT_EQ(observer.warnings, (std::vector<std::string>{"minor issue"}));
  ASSERT_EQ(observer.statuses.size(), 1U);
  EXPECT_EQ(observer.statuses[0], PipelineRunStatus::kSucceeded);
}

TEST(PipelineTest, CancellationBetweenBatchesReportsCancelledAndAborts) {
  const Project project = MakeProject(30);

  MockParser parser;
  MockValidator validator;
  validator.result.is_valid = true;
  MockGeneration generation;
  MockOutput output;
  MockLogger logger;
  RecordingObserver observer;
  CancellationToken cancellation;
  output.cancel_after_first_append = &cancellation;

  VideoSynthPipeline pipeline(&parser, &validator, &generation, nullptr,
                              nullptr, &output, &logger);
  RunOptions options;
  EXPECT_FALSE(pipeline.RunProject(project, options, &observer, &cancellation));

  // The run stopped after the first batch: the abort path was requested on
  // the output stage and the session was never finalized.
  EXPECT_EQ(output.appended_first_samples.size(), 1U);
  EXPECT_TRUE(output.abort_called);
  EXPECT_FALSE(output.finalize_called);
  ASSERT_EQ(observer.statuses.size(), 1U);
  EXPECT_EQ(observer.statuses[0], PipelineRunStatus::kCancelled);
}

TEST(PipelineTest, CancellationBeforeGenerationSkipsAllStages) {
  const Project project = MakeProject(4);

  MockParser parser;
  MockValidator validator;
  validator.result.is_valid = true;
  MockGeneration generation;
  MockOutput output;
  MockLogger logger;
  RecordingObserver observer;
  CancellationToken cancellation;
  cancellation.RequestCancellation();

  VideoSynthPipeline pipeline(&parser, &validator, &generation, nullptr,
                              nullptr, &output, &logger);
  RunOptions options;
  EXPECT_FALSE(pipeline.RunProject(project, options, &observer, &cancellation));

  EXPECT_TRUE(validator.called);
  EXPECT_FALSE(generation.called);
  EXPECT_FALSE(output.called);
  ASSERT_EQ(observer.statuses.size(), 1U);
  EXPECT_EQ(observer.statuses[0], PipelineRunStatus::kCancelled);
}

TEST(PipelineTest, ObserverReportsFailedStatusOnValidationError) {
  const Project project = MakeProject(2);

  MockParser parser;
  MockValidator validator;
  validator.result.is_valid = false;
  validator.result.errors = {"bad config"};
  MockGeneration generation;
  MockOutput output;
  MockLogger logger;
  RecordingObserver observer;

  VideoSynthPipeline pipeline(&parser, &validator, &generation, nullptr,
                              nullptr, &output, &logger);
  RunOptions options;
  EXPECT_FALSE(pipeline.RunProject(project, options, &observer));

  ASSERT_EQ(observer.statuses.size(), 1U);
  EXPECT_EQ(observer.statuses[0], PipelineRunStatus::kFailed);
}

// ---------------------------------------------------------------------------
// Worker-pool synthesis path (RunOptions::threads > 1)
// ---------------------------------------------------------------------------

TEST(PipelineTest,
     ParallelSynthesisProducesOrderedFramesIdenticalToSequential) {
  const Project project = MakeProject(30);

  auto RunWithThreads = [&project](int threads,
                                   std::vector<SampleFixed>* out_samples) {
    MockParser parser;
    MockValidator validator;
    validator.result.is_valid = true;
    MockGeneration generation;
    generation.embed_frame_id = true;
    MockOutput output;
    MockLogger logger;
    VideoSynthPipeline pipeline(&parser, &validator, &generation, nullptr,
                                nullptr, &output, &logger);
    RunOptions options;
    options.threads = threads;
    const bool ok = pipeline.RunProject(project, options);
    *out_samples = output.appended_first_samples;
    return ok;
  };

  std::vector<SampleFixed> sequential_samples;
  std::vector<SampleFixed> parallel_samples;
  ASSERT_TRUE(RunWithThreads(1, &sequential_samples));
  ASSERT_TRUE(RunWithThreads(4, &parallel_samples));

  // The parallel path appends one frame at a time, in frame order.
  ASSERT_EQ(parallel_samples.size(), 30U);
  for (std::size_t i = 0; i < parallel_samples.size(); ++i) {
    EXPECT_EQ(parallel_samples[i], static_cast<SampleFixed>(i));
  }
  // Sequential batches carry the same frames in the same order (the batch
  // path appends several frames per call; compare via its first markers).
  ASSERT_FALSE(sequential_samples.empty());
  EXPECT_EQ(sequential_samples.front(), parallel_samples.front());
}

TEST(PipelineTest, ParallelSynthesisReportsMonotonicPerFrameProgress) {
  const Project project = MakeProject(12);

  MockParser parser;
  MockValidator validator;
  validator.result.is_valid = true;
  MockGeneration generation;
  MockOutput output;
  MockLogger logger;
  RecordingObserver observer;

  VideoSynthPipeline pipeline(&parser, &validator, &generation, nullptr,
                              nullptr, &output, &logger);
  RunOptions options;
  options.threads = 4;
  EXPECT_TRUE(pipeline.RunProject(project, options, &observer));

  ASSERT_EQ(observer.progress.size(), 12U);
  for (std::size_t i = 0; i < observer.progress.size(); ++i) {
    EXPECT_EQ(observer.progress[i].first, i + 1U);
    EXPECT_EQ(observer.progress[i].second, 12U);
  }
  ASSERT_EQ(observer.statuses.size(), 1U);
  EXPECT_EQ(observer.statuses[0], PipelineRunStatus::kSucceeded);
}

TEST(PipelineTest, ParallelSynthesisCancellationAbortsCleanly) {
  const Project project = MakeProject(60);

  MockParser parser;
  MockValidator validator;
  validator.result.is_valid = true;
  MockGeneration generation;
  MockOutput output;
  MockLogger logger;
  RecordingObserver observer;
  CancellationToken cancellation;
  output.cancel_after_first_append = &cancellation;

  VideoSynthPipeline pipeline(&parser, &validator, &generation, nullptr,
                              nullptr, &output, &logger);
  RunOptions options;
  options.threads = 4;
  EXPECT_FALSE(pipeline.RunProject(project, options, &observer, &cancellation));

  EXPECT_TRUE(output.abort_called);
  EXPECT_FALSE(output.finalize_called);
  EXPECT_LT(output.appended_first_samples.size(), 60U);
  ASSERT_EQ(observer.statuses.size(), 1U);
  EXPECT_EQ(observer.statuses[0], PipelineRunStatus::kCancelled);
}

TEST(PipelineTest, ParallelSynthesisWorkerErrorReportsFailedStatus) {
  const Project project = MakeProject(20);

  MockParser parser;
  MockValidator validator;
  validator.result.is_valid = true;
  MockGeneration generation;
  generation.fail_at_frame = 7U;
  MockOutput output;
  MockLogger logger;
  RecordingObserver observer;

  VideoSynthPipeline pipeline(&parser, &validator, &generation, nullptr,
                              nullptr, &output, &logger);
  RunOptions options;
  options.threads = 4;
  EXPECT_FALSE(pipeline.RunProject(project, options, &observer));

  ASSERT_EQ(observer.statuses.size(), 1U);
  EXPECT_EQ(observer.statuses[0], PipelineRunStatus::kFailed);
  EXPECT_FALSE(logger.errors.empty());
  // Frames before the failing index were appended in order; none after it.
  EXPECT_LE(output.appended_first_samples.size(), 7U);
}

TEST(PipelineTest, SequentialRunsFromWorkerThreadProduceIdenticalResults) {
  // Two sequential runs of the same project on a non-main thread must
  // produce the same stage outputs as each other and as a main-thread run.
  const Project project = MakeProject(6);
  RunOptions options;

  auto RunOnce = [&project, &options](std::vector<SampleFixed>* out_samples) {
    MockParser parser;
    MockValidator validator;
    validator.result.is_valid = true;
    MockGeneration generation;
    generation.embed_frame_id = true;
    MockOutput output;
    MockLogger logger;
    VideoSynthPipeline pipeline(&parser, &validator, &generation, nullptr,
                                nullptr, &output, &logger);
    const bool ok = pipeline.RunProject(project, options);
    *out_samples = output.appended_first_samples;
    return ok;
  };

  std::vector<SampleFixed> worker_first;
  std::vector<SampleFixed> worker_second;
  bool worker_first_ok = false;
  bool worker_second_ok = false;
  std::thread worker([&]() {
    worker_first_ok = RunOnce(&worker_first);
    worker_second_ok = RunOnce(&worker_second);
  });
  worker.join();

  std::vector<SampleFixed> main_thread_samples;
  ASSERT_TRUE(RunOnce(&main_thread_samples));

  EXPECT_TRUE(worker_first_ok);
  EXPECT_TRUE(worker_second_ok);
  EXPECT_EQ(worker_first, worker_second);
  EXPECT_EQ(worker_first, main_thread_samples);
}

// Emits full-size PAL frames carrying a unique ramp in Y and a pattern in C,
// so a sample shift by the channel stage or a chroma rotation is visible and
// noise lands on a known signal.
class FullFrameGeneration final : public IGenerationStage {
 public:
  bool BuildFrameSchedule(const Project& project,
                          std::vector<FrameScheduleItem>* out_schedule,
                          std::vector<std::string>* errors) override {
    out_schedule->clear();
    for (const Section& s : project.sections) {
      for (int i = 0; i < s.duration_frames; ++i) {
        out_schedule->push_back(
            FrameScheduleItem{.section = &s, .source_frame_index = i});
      }
    }
    errors->clear();
    return true;
  }

  bool GenerateFrameBatch(const Project& project,
                          const std::vector<FrameScheduleItem>&,
                          std::size_t start_frame, std::size_t frame_count,
                          std::vector<SampleFixed>* out_y_mv,
                          std::vector<SampleFixed>* out_c_mv,
                          std::vector<std::string>* errors) override {
    const std::size_t spf = SamplesPerFrameForEncodingPreset(
        project.cvbs_presets.video_standard_preset,
        project.cvbs_presets.sample_encoding_preset);
    out_y_mv->assign(frame_count * spf, 0);
    out_c_mv->assign(frame_count * spf, 0);
    for (std::size_t n = 0; n < out_y_mv->size(); ++n) {
      (*out_y_mv)[n] = MillivoltsToSampleFixed(
          static_cast<double>(((start_frame * spf) + n) % 500U));
      (*out_c_mv)[n] = MillivoltsToSampleFixed(
          static_cast<double>(((start_frame * spf) + n) * 7U % 301U) - 150.0);
    }
    errors->clear();
    return true;
  }

  bool Generate(const Project&, std::vector<SampleFixed>*,
                std::vector<SampleFixed>*, std::vector<std::string>*) override {
    return false;
  }
};

// Records every Y and C sample the pipeline hands to output, on both the
// batched and the encoded (parallel) paths.
class RecordingOutput final : public IOutputStage {
 public:
  std::vector<SampleFixed> y;
  std::vector<SampleFixed> c;

  bool BeginWrite(const Project&, std::size_t,
                  std::vector<std::string>* errors) override {
    errors->clear();
    return true;
  }
  bool AppendSamples(const std::vector<SampleFixed>& y_mv,
                     const std::vector<SampleFixed>& c_mv,
                     std::vector<std::string>* errors) override {
    y.insert(y.end(), y_mv.begin(), y_mv.end());
    c.insert(c.end(), c_mv.begin(), c_mv.end());
    errors->clear();
    return true;
  }
  bool EncodeFrame(const std::vector<SampleFixed>& y_mv,
                   const std::vector<SampleFixed>& c_mv,
                   EncodedFrame* out_frame,
                   std::vector<std::string>* errors) const override {
    // The test project has one frame, so a single stash is unambiguous.
    const std::lock_guard<std::mutex> lock(mutex_);
    encoded_y_ = y_mv;
    encoded_c_ = c_mv;
    out_frame->luma_codes.clear();
    out_frame->chroma_codes.clear();
    out_frame->has_nonstandard = false;
    errors->clear();
    return true;
  }
  bool AppendEncodedFrame(const EncodedFrame&,
                          std::vector<std::string>* errors) override {
    const std::lock_guard<std::mutex> lock(mutex_);
    y.insert(y.end(), encoded_y_.begin(), encoded_y_.end());
    c.insert(c.end(), encoded_c_.begin(), encoded_c_.end());
    errors->clear();
    return true;
  }
  bool FinalizeWrite(std::vector<std::string>* errors) override {
    errors->clear();
    return true;
  }
  void AbortWrite() override {}
  bool Write(const Project&, const std::vector<SampleFixed>&,
             const std::vector<SampleFixed>&,
             std::vector<std::string>* errors) override {
    errors->clear();
    return true;
  }

 private:
  mutable std::mutex mutex_;
  mutable std::vector<SampleFixed> encoded_y_;
  mutable std::vector<SampleFixed> encoded_c_;
};

TEST(PipelineTest, ChannelStageRunsAfterGenerationAndBeforeNoise) {
  Project project = MakeProject(1);
  project.channel.enabled = true;
  project.channel.timing_offset_samples = 2.5;
  project.sections[0].noise.enabled = true;
  project.sections[0].noise.noise_db = 30.0;
  project.sections[0].noise.noise_seed = 4242;
  project.sections[0].noise.noise_seed_specified = true;

  // Expected: generate -> channel -> noise, composed by hand.
  FullFrameGeneration generation;
  std::vector<IGenerationStage::FrameScheduleItem> schedule;
  std::vector<std::string> errors;
  ASSERT_TRUE(generation.BuildFrameSchedule(project, &schedule, &errors));
  std::vector<SampleFixed> y;
  std::vector<SampleFixed> c;
  ASSERT_TRUE(generation.GenerateFrameBatch(project, schedule, 0U, 1U, &y, &c,
                                            &errors));
  const std::vector<SampleFixed> generated = y;
  MockLogger logger;
  NoiseInjectionStage noise(&logger);
  ChannelStage().ApplyChannel(project, schedule, 0U, 1U, &y, &c);
  ASSERT_NE(y, generated);
  noise.InjectNoise(project, schedule, 0U, 1U, &y, &c);

  for (int threads : {1, 2}) {
    MockParser parser;
    MockValidator validator;
    validator.result.is_valid = true;
    RecordingOutput output;
    VideoSynthPipeline pipeline(&parser, &validator, &generation, &noise,
                                nullptr, &output, &logger);
    RunOptions options;
    options.threads = threads;
    ASSERT_TRUE(pipeline.RunProject(project, options));
    EXPECT_EQ(output.y, y) << "threads " << threads;
  }
}

TEST(PipelineTest, ChromaErrorRunsAfterChannelAndBeforeNoise) {
  Project project = MakeProject(1);
  project.channel.enabled = true;
  project.channel.timing_offset_samples = 2.5;
  project.chroma_error.enabled = true;
  project.chroma_error.gain = 1.2;
  project.chroma_error.phase_deg = 30.0;
  project.chroma_error.line_jitter.enabled = true;
  project.chroma_error.line_jitter.phase_deg = 1.0;
  project.chroma_error.line_jitter.seed = 77;
  project.chroma_error.line_jitter.seed_specified = true;
  project.sections[0].noise.enabled = true;
  project.sections[0].noise.noise_db = 30.0;
  project.sections[0].noise.noise_seed = 4242;
  project.sections[0].noise.noise_seed_specified = true;

  // Expected: generate -> channel -> chroma error -> noise, composed by hand.
  FullFrameGeneration generation;
  std::vector<IGenerationStage::FrameScheduleItem> schedule;
  std::vector<std::string> errors;
  ASSERT_TRUE(generation.BuildFrameSchedule(project, &schedule, &errors));
  std::vector<SampleFixed> y;
  std::vector<SampleFixed> c;
  ASSERT_TRUE(generation.GenerateFrameBatch(project, schedule, 0U, 1U, &y, &c,
                                            &errors));
  MockLogger logger;
  NoiseInjectionStage noise(&logger);
  ChannelStage().ApplyChannel(project, schedule, 0U, 1U, &y, &c);
  const std::vector<SampleFixed> channelled_c = c;
  ChromaErrorStage(1U).ApplyChromaError(project, schedule, 0U, 1U, &c);
  ASSERT_NE(c, channelled_c);
  noise.InjectNoise(project, schedule, 0U, 1U, &y, &c);

  for (int threads : {1, 2}) {
    MockParser parser;
    MockValidator validator;
    validator.result.is_valid = true;
    RecordingOutput output;
    VideoSynthPipeline pipeline(&parser, &validator, &generation, &noise,
                                nullptr, &output, &logger);
    RunOptions options;
    options.threads = threads;
    ASSERT_TRUE(pipeline.RunProject(project, options));
    EXPECT_EQ(output.y, y) << "threads " << threads;
    EXPECT_EQ(output.c, c) << "threads " << threads;
  }
}

}  // namespace
}  // namespace videosynth
