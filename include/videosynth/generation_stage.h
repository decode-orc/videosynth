/*
 * File:        generation_stage.h
 * Module:      generation_stage
 * Purpose:     Generates CVBS-domain Y and C sample buffers from frame-based
 * source data.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <unordered_map>

#include "videosynth/biphase_injection_manager.h"
#include "videosynth/interfaces.h"
#include "videosynth/osd_renderer.h"
#include "videosynth/osd_token_resolver.h"
#include "videosynth/progressive_frame_source.h"
#include "videosynth/vits_definition_provider.h"
#include "videosynth/vits_generator.h"

namespace videosynth {

// Thread-safety: BuildFrameSchedule and Generate are NOT thread-safe and must
// not run concurrently with any other method (they mutate the biphase
// enrichment state). After BuildFrameSchedule has returned successfully,
// GenerateFrameBatch is safe to call concurrently from multiple threads for
// disjoint frame ranges with distinct output buffers: it reads only the
// immutable per-item FrameEnrichment payload, the internally-synchronised
// progressive frame source cache, synthesis resource cache and frame template
// cache, and const collaborators. The resource cache hands each worker its own
// set of frame-invariant resources, so no synthesis state is shared between
// threads; template cache entries are immutable once built and populated under
// per-key single-flight locking.
class GenerationStage final : public IGenerationStage {
 public:
  // A null progressive_source uses a default ProgressiveFrameSource (ffmpeg
  // decode and probe); tests inject one built on fake decoders.
  explicit GenerationStage(
      ILogger* logger = nullptr,
      const IVitsDefinitionProvider* vits_definition_provider = nullptr,
      const IVitsGenerator* vits_generator = nullptr,
      std::unique_ptr<ProgressiveFrameSource> progressive_source = nullptr);

  ~GenerationStage() override;

  // Builds a schedule mapping project sections to output frames, then runs
  // the sequential enrichment pass: every FrameScheduleItem receives a
  // FrameEnrichment payload (resolved VBI code words, colour context, OSD
  // token strings) so GenerateFrameBatch is a pure function of
  // (project, enriched schedule item) and frames can be synthesised in any
  // order.
  //
  // Args:
  //   project: The validated project configuration.
  //   out_schedule: Output vector populated with FrameScheduleItem entries.
  //   errors: Output vector for any error messages.
  //
  // Returns:
  //   true on success, false on any error.
  bool BuildFrameSchedule(const Project& project,
                          std::vector<FrameScheduleItem>* out_schedule,
                          std::vector<std::string>* errors) override;

  // Generates a batch of frames from the project's progressive frame sources.
  //
  // Schedule items must carry their enrichment payload when the section uses
  // laserdisc line injections or OSD overlays; hand-built schedules without
  // enrichment are only accepted for sections that use neither.
  //
  // Args:
  //   project: The validated project configuration.
  //   schedule: The frame schedule built by BuildFrameSchedule.
  //   start_frame: Index of the first frame to generate in this batch.
  //   frame_count: Number of frames to generate.
  //   out_y_mv: Output vector for luma samples (Y channel).
  //   out_c_mv: Output vector for chroma samples (C channel).
  //   errors: Output vector for any error messages.
  //
  // Returns:
  //   true on success, false on any error.
  bool GenerateFrameBatch(const Project& project,
                          const std::vector<FrameScheduleItem>& schedule,
                          std::size_t start_frame, std::size_t frame_count,
                          std::vector<SampleFixed>* out_y_mv,
                          std::vector<SampleFixed>* out_c_mv,
                          std::vector<std::string>* errors) override;

  // Convenience wrapper that builds the schedule and generates all frames.
  //
  // Args:
  //   project: The validated project configuration.
  //   out_y_mv: Output vector for luma samples (Y channel).
  //   out_c_mv: Output vector for chroma samples (C channel).
  //   errors: Output vector for any error messages.
  //
  // Returns:
  //   true on success, false on any error.
  bool Generate(const Project& project, std::vector<SampleFixed>* out_y_mv,
                std::vector<SampleFixed>* out_c_mv,
                std::vector<std::string>* errors) override;

  // Sets the frame template cache capacity in bytes; 0 disables the cache so
  // every frame is synthesised directly. Changing the capacity clears the
  // cache. Not thread-safe: call before generation starts, not concurrently
  // with GenerateFrameBatch. Output is byte-identical for every capacity —
  // the cache only removes repeated synthesis of identical clean frames.
  void SetTemplateCacheCapacityBytes(std::size_t capacity_bytes);

  // Default template cache capacity. A PAL clean frame is ~11.35 MB (two
  // 709,379-sample fixed-point buffers), so this holds ~45 PAL templates:
  // ample for still sections (colour-period templates per distinct source) and
  // short repeated clips, while bounding memory on clip sources with more
  // distinct frames than the cache can hold (those fall back to direct
  // synthesis once the cache is full).
  static constexpr std::size_t kDefaultTemplateCacheCapacityBytes =
      512ULL * 1024ULL * 1024ULL;

 private:
  // Frame-invariant synthesis resources (sampled timing context, chroma
  // encoder, rendered VITS lines, VBI waveform renderer) and the per-worker
  // cache that owns them. Both are defined in generation_stage.cpp; a worker
  // builds its set once and reuses it for every frame it synthesises.
  struct SynthesisResources;
  class SynthesisResourceCache;

  // A clean synthesised frame (sync, burst, pilot, active picture, VITS —
  // everything except the per-frame VBI and OSD patches) and the bounded
  // cache that shares such frames between workers. For constant source
  // content the clean frame is an exact function of (source, each field's
  // source frame, disc_frame_index mod P) with P the colour sequence period (4
  // frames for PAL/PAL-M, 2 for NTSC), so a still section contains only P
  // distinct clean frames. Defined in generation_stage.cpp.
  struct FrameTemplate;
  class TemplateCache;

  // Synthesises one clean frame into y_out/c_out (each frame_samples long),
  // drawing field 1's active lines from field_sources[0] and field 2's from
  // field_sources[1] (the same image except across a 2:3 pulldown picture).
  // sequence_phase is disc_frame_index reduced modulo the colour sequence
  // period; the reduction is exact because every use of the disc frame index
  // in clean synthesis depends only on that residue. Fills the buffers
  // completely (blanking / zero) before writing, so callers need not clear
  // them. Uses (and mutates) the worker-private workspaces in resources.
  static void SynthesiseTemplate(
      SynthesisResources& resources, const Section& section,
      const std::array<const FrameSourceImage*, 2>& field_sources,
      std::size_t sequence_phase, SampleFixed* y_out, SampleFixed* c_out);

  ILogger* logger_;
  // Keyed by section: the schedule index where the next section's run starts.
  // Populated by BuildFrameSchedule (single-threaded) and only read during
  // generation, so workers may consult it without synchronisation. Drives the
  // background source prefetch that overlaps the next section's decode with
  // the current section's synthesis; empty for hand-built schedules, which
  // simply do not prefetch.
  //
  // Storing an index rather than a section pointer keeps a stale map harmless:
  // the section that gets prefetched is always read out of the schedule the
  // caller passed in, never out of the map.
  std::unordered_map<const Section*, std::size_t> next_section_start_index_;
  std::unique_ptr<SynthesisResourceCache> resource_cache_;
  std::unique_ptr<TemplateCache> template_cache_;
  std::unique_ptr<ProgressiveFrameSource> progressive_source_;
  VitsDefinitionProvider default_vits_definition_provider_;
  VitsGenerator default_vits_generator_;
  const IVitsDefinitionProvider* vits_definition_provider_;
  const IVitsGenerator* vits_generator_;
  // Advanced only during the BuildFrameSchedule enrichment pass; not touched
  // by GenerateFrameBatch.
  BiphaseInjectionManager biphase_manager_;
  OsdRenderer osd_renderer_;
  OsdTokenResolver osd_token_resolver_;
};

}  // namespace videosynth
