/*
 * File:        interfaces.h
 * Module:      interfaces
 * Purpose:     Defines core pipeline interfaces and runtime options.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "videosynth/fixed_point.h"
#include "videosynth/model.h"
#include "videosynth/path_resolution.h"
#include "videosynth/results.h"

namespace videosynth {

struct FrameEnrichment;

struct RunOptions {
  std::string project_path;
  bool validate_only = false;
  std::string log_level = "info";
  std::string log_file;
  // Frame synthesis worker thread count:
  //   0  = auto (std::thread::hardware_concurrency; the CLI default),
  //   1  = pure sequential path (the library default),
  //   N  = N worker threads.
  // Output is byte-identical regardless of this value.
  int threads = 1;
  // Logical asset roots for resolving {name}/path source and output tokens.
  // The CLI populates this from DefaultAssetRoots() plus --asset-root
  // overrides; empty leaves only the built-in "project" root available.
  AssetRootMap asset_roots;
};

// Terminal status of a pipeline run, reported through
// IPipelineObserver::OnRunFinished.
enum class PipelineRunStatus {
  kSucceeded,
  kCancelled,
  kFailed,
};

// Cooperative cancellation flag for a pipeline run. The producer (for example
// a GUI cancel button) calls RequestCancellation; the pipeline polls
// IsCancellationRequested between frame batches and stops at the next check.
//
// Thread-safety: CancellationToken IS thread-safe. RequestCancellation and
// IsCancellationRequested may be called concurrently from any thread; the
// flag is a single std::atomic<bool>. The token is one-shot per run: it
// cannot be reset, so use a fresh token for each pipeline run.
class CancellationToken {
 public:
  void RequestCancellation() {
    cancellation_requested_.store(true, std::memory_order_relaxed);
  }

  bool IsCancellationRequested() const {
    return cancellation_requested_.load(std::memory_order_relaxed);
  }

 private:
  std::atomic<bool> cancellation_requested_{false};
};

// Receives progress callbacks during a pipeline run.
//
// Thread-safety: all callbacks are invoked synchronously on the thread that
// executes the pipeline run (which may be a worker thread). Implementations
// that forward to another thread (for example a GUI event loop) must perform
// their own cross-thread marshalling. Callbacks must not re-enter the
// pipeline.
class IPipelineObserver {
 public:
  virtual ~IPipelineObserver() = default;

  // Called when a pipeline stage begins ("validate", "generate", "finalize").
  virtual void OnStageStarted(const std::string& stage_name) = 0;

  // Called after each processed frame batch. frames_completed is monotonic
  // non-decreasing and reaches frames_total on an uncancelled run.
  virtual void OnFrameProgress(std::size_t frames_completed,
                               std::size_t frames_total) = 0;

  // Called for each non-fatal warning (for example validation warnings).
  virtual void OnWarning(const std::string& message) = 0;

  // Called exactly once at the end of the run with the terminal status.
  virtual void OnRunFinished(PipelineRunStatus status) = 0;
};

// Thread-safety: Implementations of ILogger must be thread-safe.
// Multiple threads may call logging methods concurrently.
class ILogger {
 public:
  virtual ~ILogger() = default;
  virtual void Info(const std::string& message) = 0;
  virtual void Warning(const std::string& message) = 0;
  virtual void Error(const std::string& message) = 0;
  virtual void Debug(const std::string& message) = 0;
  virtual void Trace(const std::string& message) = 0;
};

// Thread-safety: Implementations of IProjectParser must be thread-safe.
// ParseFile may be called concurrently from multiple threads.
class IProjectParser {
 public:
  virtual ~IProjectParser() = default;
  virtual ParseResult ParseFile(const std::string& path) = 0;
};

// Thread-safety: Implementations of IProjectValidator must be thread-safe.
// Validate may be called concurrently from multiple threads.
class IProjectValidator {
 public:
  virtual ~IProjectValidator() = default;
  virtual ValidationResult Validate(const Project& project) = 0;
};

// One audio stream of a video source, in the order the container lists its
// audio streams (index 0 is the first audio stream, whatever its absolute
// stream number).
struct ProgressiveAudioStreamProfile {
  // Absolute stream index within the container.
  int stream_index = 0;
  std::string codec;
  int channels = 0;
  int sample_rate_hz = 0;
  // 0 when the container declares no stream duration.
  double duration_seconds = 0.0;
  double start_time_seconds = 0.0;
};

struct ProgressiveFrameSourceProfile {
  std::string container;
  std::string codec;
  std::string pixel_format;
  std::string field_order;
  std::string color_space;
  std::string color_primaries;
  std::string color_transfer;
  std::string color_range;
  int bit_depth = 0;
  int width = 0;
  int height = 0;
  double sample_aspect_ratio = 0.0;
  int crop_left = 0;
  int crop_right = 0;
  int crop_top = 0;
  int crop_bottom = 0;
  // Base (r_frame_rate) rate of the first video stream.
  double frame_rate_hz = 0.0;
  // Average rate over the stream (avg_frame_rate); 0 when undeclared. A
  // constant-rate source has avg == base rate.
  double average_frame_rate_hz = 0.0;
  int frame_count = 0;
  // Earliest presentation time over all streams (the container start), and
  // that of the first video frame, in seconds. ffmpeg's input seek is
  // measured from the container start, so a decoder seeking to a video frame
  // must add the difference.
  double container_start_time_seconds = 0.0;
  double video_start_time_seconds = 0.0;
  std::vector<ProgressiveAudioStreamProfile> audio_streams;
};

// Thread-safety: Implementations of IProgressiveFrameSourceProbe must be
// thread-safe. Probe may be called concurrently from multiple threads.
class IProgressiveFrameSourceProbe {
 public:
  virtual ~IProgressiveFrameSourceProbe() = default;
  // Ownership: out_profile and error are output parameters. The caller owns
  // the pointed-to memory and must ensure the pointers are valid (non-null).
  // The implementation writes to these locations but does not take ownership.
  virtual bool Probe(const Section& section,
                     ProgressiveFrameSourceProfile* out_profile,
                     std::string* error) = 0;
};

struct FrameSourceImage;

// Thread-safety: Implementations of IProgressiveFrameProvider must be
// thread-safe. GenerateFrame may be called concurrently from multiple threads.
class IProgressiveFrameProvider {
 public:
  virtual ~IProgressiveFrameProvider() = default;
  // Ownership: out_image and error are output parameters. The caller owns the
  // pointed-to memory and must ensure the pointers are valid (non-null). The
  // decoded image itself is shared and immutable: the provider retains its own
  // reference, and callers hold a const view for as long as they need it, so
  // delivery costs a reference count rather than an image copy.
  virtual bool GenerateFrame(const Section& section, int frame_index,
                             Standard standard,
                             std::shared_ptr<const FrameSourceImage>* out_image,
                             std::string* error) const = 0;
};

// Thread-safety: BuildFrameSchedule and Generate are NOT thread-safe and must
// not run concurrently with any other method. After BuildFrameSchedule has
// returned successfully, GenerateFrameBatch may be called concurrently from
// multiple threads for disjoint frame ranges with distinct output buffers;
// this relies on the schedule items carrying their FrameEnrichment payload
// (see frame_enrichment.h).
class IGenerationStage {
 public:
  virtual ~IGenerationStage() = default;
  struct FrameScheduleItem {
    const Section* section = nullptr;
    int source_frame_index = 0;
    // CAV picture number for this frame (> 0 when a picture_number injection
    // code is present); 0 for sections without a picture_number code.
    // Used to derive disc-accurate colour-subcarrier phase independent of file
    // position.
    int disc_picture_number = 0;
    // Fully-resolved per-frame VBI payload, colour context, and OSD token
    // strings, populated by the sequential enrichment pass inside
    // BuildFrameSchedule. Shared (immutable) so schedule copies stay cheap.
    // Null for hand-built schedules; such schedules can only be generated for
    // sections without laserdisc injections or OSD overlays.
    std::shared_ptr<const FrameEnrichment> enrichment;
  };

  // Ownership: out_schedule and errors are output parameters. The caller owns
  // the pointed-to vectors and must ensure the pointers are valid (non-null).
  // The implementation clears and populates these vectors but does not take
  // ownership.
  virtual bool BuildFrameSchedule(const Project& project,
                                  std::vector<FrameScheduleItem>* out_schedule,
                                  std::vector<std::string>* errors) = 0;

  // Ownership: out_y_mv, out_c_mv, and errors are output parameters. The caller
  // owns the pointed-to vectors and must ensure the pointers are valid
  // (non-null). The implementation clears and populates these vectors but does
  // not take ownership.
  virtual bool GenerateFrameBatch(
      const Project& project, const std::vector<FrameScheduleItem>& schedule,
      std::size_t start_frame, std::size_t frame_count,
      std::vector<SampleFixed>* out_y_mv, std::vector<SampleFixed>* out_c_mv,
      std::vector<std::string>* errors) = 0;

  // Ownership: out_y_mv, out_c_mv, and errors are output parameters. The caller
  // owns the pointed-to vectors and must ensure the pointers are valid
  // (non-null). The implementation clears and populates these vectors but does
  // not take ownership.
  virtual bool Generate(const Project& project,
                        std::vector<SampleFixed>* out_y_mv,
                        std::vector<SampleFixed>* out_c_mv,
                        std::vector<std::string>* errors) = 0;
};

// One frame converted to output sample codes, ready to be written. Produced by
// IOutputStage::EncodeFrame (any thread) and written by AppendEncodedFrame on
// the thread that owns the write session.
struct EncodedFrame {
  // Composite samples, or luma samples when the output signal type is Y/C.
  std::vector<std::int16_t> luma_codes;
  // Chroma samples; empty unless the output signal type is Y/C.
  std::vector<std::int16_t> chroma_codes;
  // True when any sample of this frame mapped outside the legal code range.
  // Accumulated into the session flag by AppendEncodedFrame.
  bool has_nonstandard = false;
};

// Thread-safety: Implementations of IOutputStage are NOT thread-safe, with the
// single exception of EncodeFrame: it is const, reads only the session state
// resolved by BeginWrite, and may be called concurrently from any thread while
// the session is open. Every other method (BeginWrite, AppendSamples,
// AppendEncodedFrame, FinalizeWrite, AbortWrite, Write) must be called
// sequentially from one thread.
class IOutputStage {
 public:
  virtual ~IOutputStage() = default;

  // Ownership: errors is an output parameter. The caller owns the pointed-to
  // vector and must ensure the pointer is valid (non-null). The implementation
  // clears and populates this vector but does not take ownership.
  virtual bool BeginWrite(const Project& project,
                          std::size_t expected_frame_count,
                          std::vector<std::string>* errors) = 0;

  // Ownership: errors is an output parameter. The caller owns the pointed-to
  // vector and must ensure the pointer is valid (non-null). The implementation
  // clears and populates this vector but does not take ownership.
  virtual bool AppendSamples(const std::vector<SampleFixed>& y_mv,
                             const std::vector<SampleFixed>& c_mv,
                             std::vector<std::string>* errors) = 0;

  // Converts exactly one frame of samples to output codes without touching the
  // write session. Const and thread-safe, so frame synthesis workers can encode
  // concurrently while the session owner writes an earlier frame; the result is
  // independent of the order frames are encoded in.
  //
  // Ownership: out_frame and errors are output parameters owned by the caller.
  // out_frame's buffers are reused when they are already the right length, so
  // callers may recycle EncodedFrame objects to avoid per-frame allocation.
  virtual bool EncodeFrame(const std::vector<SampleFixed>& y_mv,
                           const std::vector<SampleFixed>& c_mv,
                           EncodedFrame* out_frame,
                           std::vector<std::string>* errors) const = 0;

  // Writes one previously encoded frame to the open session. Frames must be
  // appended in output order.
  //
  // Ownership: errors is an output parameter. The caller owns the pointed-to
  // vector and must ensure the pointer is valid (non-null).
  virtual bool AppendEncodedFrame(const EncodedFrame& frame,
                                  std::vector<std::string>* errors) = 0;

  // Ownership: errors is an output parameter. The caller owns the pointed-to
  // vector and must ensure the pointer is valid (non-null). The implementation
  // clears and populates this vector but does not take ownership.
  virtual bool FinalizeWrite(std::vector<std::string>* errors) = 0;

  // Abandons an in-progress write session: closes any open streams and
  // removes the partially-written output artefacts (video/chroma/metadata
  // files) so a cancelled run leaves nothing behind. No-op when no session is
  // open. Never fails; removal errors are ignored because the session state
  // is discarded regardless.
  virtual void AbortWrite() = 0;

  // Ownership: errors is an output parameter. The caller owns the pointed-to
  // vector and must ensure the pointer is valid (non-null). The implementation
  // clears and populates this vector but does not take ownership.
  virtual bool Write(const Project& project,
                     const std::vector<SampleFixed>& y_mv,
                     const std::vector<SampleFixed>& c_mv,
                     std::vector<std::string>* errors) = 0;
};

}  // namespace videosynth
