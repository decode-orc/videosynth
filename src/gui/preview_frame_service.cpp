/*
 * File:        preview_frame_service.cpp
 * Module:      gui
 * Purpose:     On-demand single-frame preview synthesis on a worker thread
 *              with latest-wins coalescing and an LRU frame cache
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "preview_frame_service.h"

#include <QStringList>
#include <algorithm>
#include <utility>

#include "videosynth/project_validator.h"
#include "videosynth/results.h"

namespace videosynth::gui {

namespace {

// Index of `section` within project.sections, or -1 for hand-built items.
int SectionIndexOf(const Project& project, const Section* section) {
  for (std::size_t i = 0; i < project.sections.size(); ++i) {
    if (&project.sections[i] == section) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

// Default synthesizer: the exact per-frame recipe of the pipeline's frame
// job (GenerateFrameBatch, the channel and chroma error stages, then optional
// noise and dropouts), plus the decoded source image for progressive sections.
// Runs on the worker thread.
bool SynthesizeWithPipelineStages(
    const PreviewFrameService::SynthesisContext& context,
    std::size_t disc_frame, const PreviewOptions& options,
    PreviewFrameData* out_frame, std::string* error) {
  std::vector<std::string> errors;
  if (!context.generation->GenerateFrameBatch(
          *context.project, *context.schedule, disc_frame, 1U, &out_frame->y_mv,
          &out_frame->c_mv, &errors)) {
    *error = errors.empty() ? "Preview frame synthesis failed." : errors[0];
    return false;
  }

  // The shared noise level term reads the pressing's picture, before the
  // channel stage alters it.
  std::vector<SampleFixed> pressing_y_mv;
  const bool needs_pressing_luma =
      options.apply_noise &&
      NoiseInjectionStage::NeedsPressingLuma(*context.project);
  if (needs_pressing_luma) {
    pressing_y_mv = out_frame->y_mv;
  }

  // The channel describes the pressing, so it is part of the previewed
  // signal itself rather than an optional impairment.
  context.channel->ApplyChannel(*context.project, *context.schedule, disc_frame,
                                1U, &out_frame->y_mv, &out_frame->c_mv);
  // The chroma error is a fixed property of the capture's player, not a
  // random impairment, so it is previewed with the channel.
  context.chroma_error->ApplyChromaError(*context.project, *context.schedule,
                                         disc_frame, 1U, &out_frame->c_mv);

  if (options.apply_noise) {
    context.noise->InjectNoise(*context.project, *context.schedule, disc_frame,
                               1U,
                               needs_pressing_luma ? &pressing_y_mv : nullptr,
                               &out_frame->y_mv, &out_frame->c_mv);
  }
  if (options.apply_dropouts) {
    std::vector<DropoutSidecarRow> sidecar_rows;
    context.dropouts->ComputeFrameDropouts(*context.project, *context.schedule,
                                           disc_frame, &out_frame->y_mv,
                                           &out_frame->c_mv, 0U, &sidecar_rows);
  }

  const IGenerationStage::FrameScheduleItem& item =
      (*context.schedule)[disc_frame];
  if (item.section != nullptr && item.section->type == "progressive") {
    std::string source_error;
    std::shared_ptr<const FrameSourceImage> image;
    if (context.source_provider->GenerateFrame(
            *item.section, item.source_frame_index,
            context.project->cvbs_presets.video_standard_preset, &image,
            &source_error) &&
        image != nullptr) {
      // The provider's copy stays cached and immutable; the preview keeps its
      // own so it can outlive a cache eviction or a project reload.
      out_frame->source_image = *image;
      out_frame->has_source_image = true;
    }
    // A source decode failure is not fatal for the encoded preview; the
    // encoded buffers above already synthesised successfully.
  }

  return true;
}

}  // namespace

QString FormatFieldCadence(
    const std::array<IGenerationStage::FieldPicture, 2>& fields) {
  if (fields[0].cadence_letter == '\0') {
    return QString();
  }
  QStringList parts;
  for (std::size_t field = 0; field < fields.size(); ++field) {
    const IGenerationStage::FieldPicture& picture = fields[field];
    parts.append(QStringLiteral("F%1: film %2 (%3%4)")
                     .arg(field + 1)
                     .arg(picture.source_frame_index)
                     .arg(QChar::fromLatin1(picture.cadence_letter))
                     .arg(picture.cadence_field_in_letter));
  }
  return parts.join(QStringLiteral(" · "));
}

PreviewFrameService::PreviewFrameService(SynthesizeFrameFn synthesizer,
                                         int cache_capacity, QObject* parent)
    : QObject(parent),
      synthesizer_(std::move(synthesizer)),
      cache_capacity_(static_cast<std::size_t>(std::max(1, cache_capacity))) {
  if (!synthesizer_) {
    synthesizer_ = SynthesizeWithPipelineStages;
  }
}

PreviewFrameService::~PreviewFrameService() { JoinWorker(); }

void PreviewFrameService::SetProject(const Project& project) {
  project_ = project;
  has_project_ = true;
  ++revision_;
  schedule_context_.reset();
  schedule_info_.reset();
  cache_.clear();
  pending_request_.reset();
}

void PreviewFrameService::RequestFrame(std::size_t output_frame_index,
                                       const PreviewOptions& options) {
  if (!has_project_) {
    return;
  }

  const CacheKey key{revision_, output_frame_index, options};
  if (std::shared_ptr<const PreviewFrameData> cached = CacheLookup(key)) {
    emit FrameReady(std::move(cached));
    return;
  }

  pending_request_ = Request{output_frame_index, options};
  if (!worker_active_) {
    StartWorker();
  }
}

void PreviewFrameService::StartWorker() {
  if (worker_active_ || !pending_request_.has_value()) {
    return;
  }

  const Request request = *pending_request_;
  pending_request_.reset();
  JoinWorker();
  worker_active_ = true;

  const quint64 revision = revision_;
  std::shared_ptr<ScheduleContext> context = schedule_context_;
  // The worker may not touch project_ (SetProject can replace it while the
  // worker runs), so a fresh context gets its own snapshot up front.
  Project project_snapshot;
  if (context == nullptr) {
    project_snapshot = project_;
  }

  worker_ = std::thread([this, revision, request, context,
                         project_snapshot = std::move(project_snapshot),
                         synthesizer = synthesizer_]() mutable {
    // Publishes a failure together with the schedule context when it was
    // fully built, so navigator ranges stay fresh even when the requested
    // frame itself failed (for example an out-of-range index after edits).
    auto fail = [this, revision, &context](QString message) {
      QMetaObject::invokeMethod(
          this,
          [this, revision, context, message = std::move(message)] {
            PublishFailure(revision, context, message);
          },
          Qt::QueuedConnection);
    };

    if (context == nullptr) {
      auto fresh = std::make_shared<ScheduleContext>();
      fresh->project = std::move(project_snapshot);

      // Structural validation guards synthesis: the generation stage assumes
      // a validated project (an unknown standard would throw, for example).
      ProjectValidator validator;
      const ValidationResult validation = validator.Validate(fresh->project);
      if (!validation.is_valid) {
        fail(validation.errors.empty()
                 ? tr("Project is invalid.")
                 : tr("Project is invalid: %1")
                       .arg(QString::fromStdString(validation.errors[0])));
        return;
      }

      std::vector<std::string> errors;
      if (!fresh->generation.BuildFrameSchedule(fresh->project,
                                                &fresh->schedule, &errors)) {
        fail(errors.empty() ? tr("Failed to build the preview frame schedule.")
                            : QString::fromStdString(errors[0]));
        return;
      }

      PreviewScheduleInfo info;
      info.output_frame_count = fresh->schedule.size();
      info.section_first_output_frame.assign(fresh->project.sections.size(),
                                             -1);
      for (std::size_t output_frame = 0; output_frame < fresh->schedule.size();
           ++output_frame) {
        const int section_index = SectionIndexOf(
            fresh->project, fresh->schedule[output_frame].section);
        if (section_index >= 0 &&
            info.section_first_output_frame[static_cast<std::size_t>(
                section_index)] < 0) {
          info.section_first_output_frame[static_cast<std::size_t>(
              section_index)] = static_cast<qint64>(output_frame);
        }
      }
      info.standard = fresh->project.cvbs_presets.video_standard_preset;
      const TimingConstants timing = GetTimingConstants(info.standard);
      info.samples_per_line = timing.samples_per_line_4fsc;
      info.lines_per_frame = timing.lines_per_frame;
      info.sample_rate_hz = timing.sample_rate_4fsc_hz;
      info.levels = GetSignalLevels(fresh->project.cvbs_presets);
      info.signal_type = fresh->project.output.signal_type;
      fresh->info = std::move(info);

      context = std::move(fresh);
    }

    if (request.output_frame_index >= context->info.output_frame_count) {
      fail(tr("Preview frame %1 is out of range (%2 output frames).")
               .arg(request.output_frame_index)
               .arg(context->info.output_frame_count));
      return;
    }

    const std::size_t disc_frame = request.output_frame_index;
    auto frame = std::make_shared<PreviewFrameData>();
    frame->revision = revision;
    frame->output_frame_index = request.output_frame_index;
    frame->options = request.options;
    const Section* section = context->schedule[disc_frame].section;
    frame->section_index = SectionIndexOf(context->project, section);
    if (section != nullptr) {
      frame->section_name = QString::fromStdString(section->name);
    }
    frame->fields = context->schedule[disc_frame].fields;

    SynthesisContext synthesis_context;
    synthesis_context.project = &context->project;
    synthesis_context.schedule = &context->schedule;
    synthesis_context.generation = &context->generation;
    synthesis_context.channel = &context->channel;
    synthesis_context.chroma_error = &context->chroma_error;
    synthesis_context.noise = &context->noise;
    synthesis_context.dropouts = &context->dropouts;
    synthesis_context.source_provider = &context->source_provider;

    std::string error;
    if (!synthesizer(synthesis_context, disc_frame, request.options,
                     frame.get(), &error)) {
      fail(error.empty() ? tr("Preview frame synthesis failed.")
                         : QString::fromStdString(error));
      return;
    }

    // Marshal back to the owning thread; if the service is destroyed before
    // delivery, Qt drops the queued call with the context object.
    QMetaObject::invokeMethod(
        this,
        [this, revision, context = std::move(context),
         frame = std::shared_ptr<const PreviewFrameData>(std::move(frame))]() {
          PublishSuccess(revision, context, frame);
        },
        Qt::QueuedConnection);
  });
}

void PreviewFrameService::AdoptScheduleContext(
    quint64 revision, const std::shared_ptr<ScheduleContext>& context) {
  if (revision != revision_ || context == nullptr ||
      schedule_context_ != nullptr) {
    return;
  }
  schedule_context_ = context;
  schedule_info_ = schedule_context_->info;
  emit ScheduleInfoChanged();
}

void PreviewFrameService::PublishSuccess(
    quint64 revision, std::shared_ptr<ScheduleContext> context,
    std::shared_ptr<const PreviewFrameData> frame) {
  worker_active_ = false;

  // Results for a superseded snapshot are dropped; a pending request for the
  // new revision (if any) restarts below.
  if (revision == revision_) {
    AdoptScheduleContext(revision, context);
    StoreInCache(frame);
    emit FrameReady(std::move(frame));
  }

  if (pending_request_.has_value()) {
    StartWorker();
  }
}

void PreviewFrameService::PublishFailure(
    quint64 revision, std::shared_ptr<ScheduleContext> context,
    QString message) {
  worker_active_ = false;

  if (revision == revision_) {
    AdoptScheduleContext(revision, context);
    emit PreviewFailed(revision, message);
  }

  if (pending_request_.has_value()) {
    StartWorker();
  }
}

void PreviewFrameService::StoreInCache(
    std::shared_ptr<const PreviewFrameData> frame) {
  const CacheKey key{frame->revision, frame->output_frame_index,
                     frame->options};
  cache_.remove_if(
      [&key](const CacheEntry& entry) { return entry.key == key; });
  cache_.push_front(CacheEntry{key, std::move(frame)});
  while (cache_.size() > cache_capacity_) {
    cache_.pop_back();
  }
}

std::shared_ptr<const PreviewFrameData> PreviewFrameService::CacheLookup(
    const CacheKey& key) {
  for (auto it = cache_.begin(); it != cache_.end(); ++it) {
    if (it->key == key) {
      // Move the hit to the front so eviction stays least-recently-used.
      cache_.splice(cache_.begin(), cache_, it);
      return cache_.front().frame;
    }
  }
  return nullptr;
}

void PreviewFrameService::JoinWorker() {
  if (worker_.joinable()) {
    worker_.join();
  }
}

}  // namespace videosynth::gui
