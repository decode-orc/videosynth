/*
 * File:        dropout_injection_stage.h
 * Module:      dropout_injection
 * Purpose:     Applies per-section random and scratch dropout events to
 *              fixed-point mV Y/C buffers before output quantisation, and
 *              writes a conformant dropout sidecar SQLite file.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <sqlite3.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "videosynth/fixed_point.h"
#include "videosynth/interfaces.h"
#include "videosynth/model.h"
#include "videosynth/timing_constants.h"

namespace videosynth {

// One pre-computed scratch event bound to a section.
struct ScratchEvent {
  int anchor_line;
  int anchor_offset;
  int duration_frames;
  int peak_width;
  int direction;  // -1 (down) or +1 (up)
  double max_push_fraction;
};

// One dropout annotation destined for the sidecar database. Produced by
// ComputeFrameDropouts (any thread) and written by CommitSidecarRows on the
// pipeline thread so rows are inserted in frame order regardless of the
// frame synthesis order.
struct DropoutSidecarRow {
  std::int64_t frame_id = 0;
  std::int64_t sample_start = 0;
  std::int64_t sample_count = 0;
  int severity = 0;
};

// Applies random and scratch dropout events to the Y/C mV-domain sample
// buffers for sections that have dropout injection enabled, and writes dropout
// annotation rows to a conformant SQLite sidecar file
// (<basename>.dropouts.meta, schema version 5).
//
// Placement: called after NoiseInjectionStage and before IOutputStage::
// AppendSamples in VideoSynthPipeline::Run.
//
// Seeding: mirrors the scheme used by NoiseInjectionStage. When a user-
// supplied seed is present it is used; otherwise run_base_seed_ (captured
// from std::random_device at construction) provides per-run randomness that
// is fixed within a single pipeline invocation.
//
// Sidecar lifecycle: Begin() opens (or creates) the sidecar; InjectDropouts()
// appends rows inside a single open transaction; Finalize() commits and closes.
// If no section has dropout injection enabled, Begin() becomes a no-op and
// Finalize() does nothing.
//
// Thread-safety: ComputeFrameDropouts is const, reads only immutable state
// (run_base_seed_ is fixed at construction), and may be called concurrently
// from multiple threads on distinct buffers. All other methods (Begin,
// InjectDropouts, CommitSidecarRows, Finalize, Abort) own the SQLite handle
// and must be called from a single thread with no concurrent access.
class DropoutInjectionStage {
 public:
  explicit DropoutInjectionStage(ILogger* logger);
  ~DropoutInjectionStage();

  // Disallow copy and move — owns a raw SQLite handle.
  DropoutInjectionStage(const DropoutInjectionStage&) = delete;
  DropoutInjectionStage& operator=(const DropoutInjectionStage&) = delete;
  DropoutInjectionStage(DropoutInjectionStage&&) = delete;
  DropoutInjectionStage& operator=(DropoutInjectionStage&&) = delete;

  // Opens the sidecar SQLite file when at least one section has dropout
  // injection enabled. Must be called before the first InjectDropouts call.
  // Returns false and populates errors on failure.
  bool Begin(const Project& project, std::vector<std::string>* errors);

  // Applies random and scratch dropout events to y_mv/c_mv in-place for each
  // frame in the batch, and appends dropout_run rows to the sidecar.
  // Args match the contract used by NoiseInjectionStage::InjectNoise.
  // Equivalent to ComputeFrameDropouts + CommitSidecarRows per frame.
  void InjectDropouts(
      const Project& project,
      const std::vector<IGenerationStage::FrameScheduleItem>& schedule,
      std::size_t frame_offset, std::size_t frame_count,
      std::vector<SampleFixed>* y_mv, std::vector<SampleFixed>* c_mv);

  // Applies random and scratch dropout events for a single frame in-place and
  // appends the frame's sidecar rows to out_rows instead of writing them to
  // the database. Deterministic for a given (project, global_frame, seed)
  // regardless of processing order, and safe to call concurrently from
  // multiple threads on distinct buffers.
  //
  // Args:
  //   project:             Parsed project supplying signal levels and presets.
  //   schedule:            Frame schedule mapping frame indices to sections.
  //   global_frame:        Index of the frame within the schedule; also used
  //                        as the sidecar frame_id.
  //   y_mv, c_mv:          Sample buffers containing (at least) this frame.
  //   frame_buffer_offset: Offset of the frame's first sample within the
  //                        buffers.
  //   out_rows:            Receives the frame's sidecar rows (appended).
  void ComputeFrameDropouts(
      const Project& project,
      const std::vector<IGenerationStage::FrameScheduleItem>& schedule,
      std::size_t global_frame, std::vector<SampleFixed>* y_mv,
      std::vector<SampleFixed>* c_mv, std::size_t frame_buffer_offset,
      std::vector<DropoutSidecarRow>* out_rows) const;

  // Writes previously computed sidecar rows to the open sidecar session.
  // Rows must be committed in frame order to keep the sidecar byte-identical
  // to a sequential run. No-op when no sidecar session is open.
  void CommitSidecarRows(const std::vector<DropoutSidecarRow>& rows);

  // Commits the sidecar transaction and closes the database handle.
  // No-op if Begin was not called or no rows were written.
  // Returns false and populates errors on SQLite failure.
  bool Finalize(std::vector<std::string>* errors);

  // Abandons an in-progress sidecar session: discards the open transaction,
  // closes the database handle, and removes the partially-written sidecar
  // file. No-op if Begin was not called or the sidecar was not created.
  void Abort();

  // Derives the sidecar file path from the project metadata path. Public so
  // front-ends can report the artefact path without duplicating the rule.
  static std::string DeriveSidecarPath(const std::string& metadata_path);

 private:
  // Returns the index of section within project.sections, or 0.
  static std::size_t FindSectionIndex(const Project& project,
                                      const Section* section);

  // Returns true if any section in project has at least one dropout type
  // with scale > 0.
  static bool AnyDropoutEnabled(const Project& project);

  // Derives the deterministic scratch event set for a section. Pure function
  // of its arguments, so per-frame recomputation is order-independent.
  // section_disc_start is the disc frame index of the section's first frame;
  // it keys disc-anchored scratches in place of section_index.
  static std::vector<ScratchEvent> ComputeScratchEvents(
      const Section& section, std::size_t section_index,
      std::uint64_t section_disc_start, int lines_per_frame,
      int samples_per_line, uint64_t run_base_seed);

  // Writes a single dropout_run row to the sidecar.
  void WriteSidecarRow(int64_t frame_id, int64_t sample_start,
                       int64_t sample_count, int severity);

  // Applies random dropout events for one frame after overlap resolution,
  // appending sidecar rows to out_rows (null for unflagged dropouts).
  // disc_frame keys disc-anchored random dropouts in place of
  // (section_index, global_frame).
  void ProcessRandomDropouts(
      const Section& section, std::size_t section_index,
      std::size_t global_frame, std::uint64_t disc_frame, int samples_per_frame,
      int lines_per_frame, int samples_per_line, int64_t frame_id,
      const std::vector<std::pair<int, int>>& scratch_intervals,
      const SignalLevels& levels, SampleFixed clamp_min, SampleFixed clamp_max,
      std::vector<SampleFixed>* y_mv, std::vector<SampleFixed>* c_mv,
      std::size_t frame_buffer_offset,
      std::vector<DropoutSidecarRow>* out_rows) const;

  // Applies scratch dropout events for one frame, appending sidecar rows to
  // out_rows (null for unflagged scratches).
  static void ProcessScratchDropouts(
      const std::vector<ScratchEvent>& scratch_events, int samples_per_line,
      int64_t frame_id, int frame_index_in_section,
      std::vector<std::pair<int, int>>* out_scratch, const SignalLevels& levels,
      SampleFixed clamp_min, SampleFixed clamp_max,
      std::vector<SampleFixed>* y_mv, std::vector<SampleFixed>* c_mv,
      std::size_t frame_buffer_offset, int samples_per_frame,
      std::vector<DropoutSidecarRow>* out_rows);

  // Applies the signal push for a run and appends its sidecar rows, split at
  // any active-picture boundary. A null out_rows records nothing (unflagged).
  static void ApplyAndRecordRun(
      int sample_start, int sample_count, int direction, double push_fraction,
      int64_t frame_id, int active_picture_start, int active_picture_end,
      const SignalLevels& levels, SampleFixed clamp_min, SampleFixed clamp_max,
      std::vector<SampleFixed>* y_mv, std::vector<SampleFixed>* c_mv,
      std::size_t frame_buffer_offset,
      std::vector<DropoutSidecarRow>* out_rows);

  ILogger* logger_;
  uint64_t run_base_seed_;

  sqlite3* db_ = nullptr;
  sqlite3_stmt* insert_stmt_ = nullptr;
  // Sidecar file path for the currently open session; used by Abort to
  // remove the partially-written file.
  std::string sidecar_path_;
};

}  // namespace videosynth
