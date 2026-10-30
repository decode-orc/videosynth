/*
 * File:        preview_frame_service.h
 * Module:      gui
 * Purpose:     On-demand single-frame preview synthesis on a worker thread
 *              with latest-wins coalescing and an LRU frame cache
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <QObject>
#include <QString>
#include <array>
#include <cstddef>
#include <functional>
#include <list>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "videosynth/channel_stage.h"
#include "videosynth/chroma_error_stage.h"
#include "videosynth/dropout_injection_stage.h"
#include "videosynth/fixed_point.h"
#include "videosynth/generation_stage.h"
#include "videosynth/interfaces.h"
#include "videosynth/model.h"
#include "videosynth/noise_injection_stage.h"
#include "videosynth/progressive_frame_source.h"
#include "videosynth/timing_constants.h"

namespace videosynth::gui {

// Preview degradation toggles. Noise and dropouts are opt-in so the default
// preview shows the clean encoded signal.
struct PreviewOptions {
  bool apply_noise = false;
  bool apply_dropouts = false;
};

inline bool operator==(const PreviewOptions& a, const PreviewOptions& b) {
  return a.apply_noise == b.apply_noise && a.apply_dropouts == b.apply_dropouts;
}

// Immutable description of the current project's output frame space,
// published after each successful schedule build. Output frame indices
// address the frames the pipeline would write, which is the project's frame
// schedule in order.
struct PreviewScheduleInfo {
  std::size_t output_frame_count = 0;
  // First output frame showing each project section; -1 when the section
  // contributes no output frames.
  std::vector<qint64> section_first_output_frame;
  Standard standard = Standard::kUnknown;
  int samples_per_line = 0;
  int lines_per_frame = 0;
  double sample_rate_hz = 0.0;
  SignalLevels levels = {};
  // Project output signal_type ("composite" or "yc"), for the default
  // encoded-view mode.
  std::string signal_type;
};

// One synthesised preview frame: the exact Y/C mV buffers the pipeline would
// hand to the output stage for this frame, plus the decoded source image for
// progressive sections. Immutable after publication; shared between the
// cache and views.
struct PreviewFrameData {
  quint64 revision = 0;
  std::size_t output_frame_index = 0;
  int section_index = -1;
  QString section_name;
  // What each field shows (index 0 = field 1), from the frame schedule. In a
  // 2:3 pulldown section the two fields may show different film frames.
  std::array<IGenerationStage::FieldPicture, 2> fields{};
  PreviewOptions options;
  std::vector<SampleFixed> y_mv;
  std::vector<SampleFixed> c_mv;
  bool has_source_image = false;
  FrameSourceImage source_image;
};

// The cadence of a previewed frame for the frame info line, e.g.
// "F1: film 37 (B3) · F2: film 38 (C1)": each field's 0-based source film
// frame and its place in the 2:3 cycle (as the {cadence} OSD token shows it).
// Empty for a video-cadence frame, whose fields show the same picture.
//
// Thread-safety: thread-safe (pure function).
QString FormatFieldCadence(
    const std::array<IGenerationStage::FieldPicture, 2>& fields);

// Synthesises single preview frames on demand through the same deterministic
// backend the pipeline uses: BuildFrameSchedule's enriched schedule plus the
// pure per-frame GenerateFrameBatch, so what is previewed is exactly what
// will be written. Synthesis runs on a worker thread with latest-wins
// coalescing (rapid scrubbing collapses to the newest request); results are
// kept in a small LRU cache keyed by (project revision, output frame,
// options) that SetProject invalidates.
//
// Relative paths in the project must be resolved before SetProject (see
// videosynth::ResolveProjectPaths in path_resolution.h) because synthesis
// executes on a worker thread.
//
// Thread-safety: all public members must be called from the owning (GUI)
// thread, which must run an event loop for results to be delivered. The
// injected synthesizer executes on the worker thread and must be safe to run
// off the GUI thread (the default uses the schedule-owned stages, which
// satisfies this).
class PreviewFrameService : public QObject {
  Q_OBJECT

 public:
  // Collaborators available to a synthesizer call. All pointers refer to
  // state owned by the service's per-revision schedule context and stay
  // valid for the duration of the call.
  struct SynthesisContext {
    const Project* project = nullptr;
    const std::vector<IGenerationStage::FrameScheduleItem>* schedule = nullptr;
    IGenerationStage* generation = nullptr;
    const ChannelStage* channel = nullptr;
    const ChromaErrorStage* chroma_error = nullptr;
    const NoiseInjectionStage* noise = nullptr;
    const DropoutInjectionStage* dropouts = nullptr;
    const IProgressiveFrameProvider* source_provider = nullptr;
  };

  // Synthesises the disc frame at schedule index `disc_frame` into
  // out_frame's y_mv/c_mv (and optionally source_image). Runs on the worker
  // thread. Returns false and sets error on failure.
  using SynthesizeFrameFn =
      std::function<bool(const SynthesisContext& context,
                         std::size_t disc_frame, const PreviewOptions& options,
                         PreviewFrameData* out_frame, std::string* error)>;

  static constexpr int kDefaultCacheCapacity = 4;

  // synthesizer: callable executed on the worker thread for each frame; when
  // empty, the default (GenerateFrameBatch + channel and chroma error
  // stages + optional noise/dropout stages + progressive source decode) is
  // used.
  explicit PreviewFrameService(SynthesizeFrameFn synthesizer = {},
                               int cache_capacity = kDefaultCacheCapacity,
                               QObject* parent = nullptr);
  ~PreviewFrameService() override;

  // Replaces the previewed project snapshot: bumps the revision, drops the
  // cached frames and schedule, and cancels the pending (unstarted) request.
  // The next RequestFrame rebuilds the schedule on the worker.
  void SetProject(const Project& project);

  // Monotonic project snapshot counter; results and failures carry the
  // revision they were computed for so stale deliveries can be recognised.
  quint64 revision() const { return revision_; }

  // Requests the frame at `output_frame_index`. Cache hits emit FrameReady
  // synchronously; misses are queued latest-wins and synthesised on the
  // worker thread. No-op before the first SetProject.
  void RequestFrame(std::size_t output_frame_index,
                    const PreviewOptions& options);

  // True from worker start until its result (or failure) is published.
  bool is_working() const { return worker_active_; }

  // Schedule info for the current revision; nullopt until the first
  // successful build after SetProject.
  const std::optional<PreviewScheduleInfo>& schedule_info() const {
    return schedule_info_;
  }

  // Number of frames currently held by the LRU cache (test observability).
  int cached_frame_count() const { return static_cast<int>(cache_.size()); }

 signals:
  // A schedule build for the current revision completed; schedule_info() is
  // populated.
  void ScheduleInfoChanged();

  // A requested frame is available (fresh synthesis or cache hit).
  void FrameReady(std::shared_ptr<const PreviewFrameData> frame);

  // Schedule build or synthesis failed for `revision` (for example the
  // project is invalid). The previously published frames stay valid as
  // last-good fallbacks.
  void PreviewFailed(quint64 revision, const QString& message);

 private:
  struct Request {
    std::size_t output_frame_index = 0;
    PreviewOptions options;
  };

  // Per-revision synthesis context: the stable project snapshot the schedule
  // items point into, the enriched schedule, and the single-owner stages.
  // Built once per revision on the worker thread, then reused by subsequent
  // requests. Only the active worker touches a ScheduleContext while it is
  // being built; afterwards it is immutable apart from the internally
  // synchronised source cache.
  struct ScheduleContext {
    Project project;
    GenerationStage generation;
    ChannelStage channel;
    ChromaErrorStage chroma_error;
    NoiseInjectionStage noise{nullptr};
    DropoutInjectionStage dropouts{nullptr};
    ProgressiveFrameSource source_provider;
    std::vector<IGenerationStage::FrameScheduleItem> schedule;
    PreviewScheduleInfo info;
  };

  struct CacheKey {
    quint64 revision = 0;
    std::size_t output_frame_index = 0;
    PreviewOptions options;

    bool operator==(const CacheKey& other) const {
      return revision == other.revision &&
             output_frame_index == other.output_frame_index &&
             options == other.options;
    }
  };

  struct CacheEntry {
    CacheKey key;
    std::shared_ptr<const PreviewFrameData> frame;
  };

  void StartWorker();
  void AdoptScheduleContext(quint64 revision,
                            const std::shared_ptr<ScheduleContext>& context);
  void PublishSuccess(quint64 revision,
                      std::shared_ptr<ScheduleContext> context,
                      std::shared_ptr<const PreviewFrameData> frame);
  // context is null when the failure occurred before the schedule was fully
  // built (for example a validation error).
  void PublishFailure(quint64 revision,
                      std::shared_ptr<ScheduleContext> context,
                      QString message);
  void StoreInCache(std::shared_ptr<const PreviewFrameData> frame);
  std::shared_ptr<const PreviewFrameData> CacheLookup(const CacheKey& key);
  void JoinWorker();

  SynthesizeFrameFn synthesizer_;
  std::size_t cache_capacity_;

  quint64 revision_ = 0;
  bool has_project_ = false;
  Project project_;

  // Schedule context for revision_; null until the worker has built it.
  std::shared_ptr<ScheduleContext> schedule_context_;
  std::optional<PreviewScheduleInfo> schedule_info_;

  std::optional<Request> pending_request_;
  std::thread worker_;
  bool worker_active_ = false;

  // Most-recently-used first.
  std::list<CacheEntry> cache_;
};

}  // namespace videosynth::gui
