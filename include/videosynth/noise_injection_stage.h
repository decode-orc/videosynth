/*
 * File:        noise_injection_stage.h
 * Module:      noise_injection
 * Purpose:     Applies per-section capture and shared (pressing) Gaussian
 *              noise to fixed-point mV Y/C frame buffers before output
 *              quantisation.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "videosynth/fixed_point.h"
#include "videosynth/interfaces.h"
#include "videosynth/model.h"
#include "videosynth/noise_spectrum_filter.h"

namespace videosynth {

// Injects additive Gaussian noise into the Y and C mV-domain sample buffers.
// A section may carry two independent layers.
//
// Capture layer (NoiseParameters::enabled), describing one capture:
//   - Floor noise (sigma_f): zero-mean Gaussian, signal-independent; controls
//     Black PSNR as measured by orc-gui.
//   - Proportional noise (k): standard deviation scales with the instantaneous
//     Y-channel amplitude; makes white noisier than black, controlled by
//     noise_spread_db.
//   - Optional spectrum: unit normals are shaped by a NoiseSpectrumFilter
//     before the per-sample sigma is applied, so the level-dependent term
//     modulates the shaped noise.
//   - Optional spread_spectrum: the level-dependent term is drawn
//     independently, shaped by its own filter, and added to the floor
//     (shaped by spectrum alone); the total variance is unchanged.
//
// Shared (pressing) layer (NoiseParameters::shared), describing the disc:
//   constant sigma from shared.noise_db, optional spectrum, seeded from
//   (shared.noise_seed, kSharedNoiseSeedDomain, disc frame index). It never
//   depends on section index or output position, so every capture of one
//   pressing carries identical pressing noise on the same picture. The
//   project's channel timing offset delays it as it delays the picture, so
//   it stays aligned with the picture on every capture.
//   - Optional level_term: a second, independent pressing component seeded
//     from (shared.noise_seed, kSharedLevelNoiseSeedDomain, disc frame
//     index), with its own spectrum, whose sigma is level_term.noise_db
//     scaled by a piecewise-linear dB curve against the pressing picture's
//     luma in mV above blanking. The level is read from the picture before the
//     channel stage (pressing_y_mv), so captures with different channel
//     settings still share the layer; it is delayed with the constant
//     component.
//
// Both layers are summed into a single noise value per sample position, so
// noise_db is the level of each output signal. On Y/C output
// (output.signal_type "yc") the value is applied to both Y and C, producing
// correlated Y/C noise that matches real analogue sources. On composite
// output it is applied to Y only: the output is Y + C, and adding it to both
// would double the composite noise. Y is then clamped to the legal composite
// range; C is zero-centred and is left for the output stage, which clamps
// Y + C.
//
// Capture seeding: by default a random base seed is captured from
// std::random_device at construction time, so each run of the same project
// produces different noise. Setting NoiseParameters::noise_seed_specified =
// true in a section causes that section to mix the user-supplied noise_seed
// into the per-frame seed formula instead, making the output fully
// deterministic and reproducible across runs. Within a single run, the same
// (frame, section) pair always produces identical output regardless of batch
// size.
//
// Thread-safety: NoiseInjectionStage IS thread-safe for concurrent InjectNoise
// calls on distinct buffers: the method is const, per-frame RNGs are seeded
// deterministically from immutable state (run_base_seed_ is fixed at
// construction), the spectrum filter cache is mutex-guarded and holds
// immutable filters, and the logger contract requires thread-safety. Callers
// must not pass the same buffer to concurrent calls.
class NoiseInjectionStage {
 public:
  explicit NoiseInjectionStage(ILogger* logger);

  // Adds noise to frame buffers in-place.
  //
  // Args:
  //   project:      Parsed project supplying signal levels and encoding
  //                 preset.
  //   schedule:     Frame schedule mapping global frame indices to sections.
  //                 The shared layer reads each item's enrichment for the
  //                 disc frame index and falls back to the schedule position
  //                 for hand-built schedules without enrichment.
  //   frame_offset: Index into schedule of the first frame in this batch.
  //   frame_count:  Number of frames in this batch.
  //   y_mv:         Y-channel mV buffer (frame_count x samples_per_frame).
  //   c_mv:         C-channel mV buffer (frame_count x samples_per_frame).
  void InjectNoise(
      const Project& project,
      const std::vector<IGenerationStage::FrameScheduleItem>& schedule,
      std::size_t frame_offset, std::size_t frame_count,
      std::vector<SampleFixed>* y_mv, std::vector<SampleFixed>* c_mv) const;

  // As above, with the pressing's picture for the shared level term.
  //
  // Args:
  //   pressing_y_mv: Y as generated, before the channel stage, in the same
  //                  layout as y_mv. nullptr (or a size mismatch) reads the
  //                  level from y_mv as it arrives, which is exact when no
  //                  channel stage ran. Not modified.
  void InjectNoise(
      const Project& project,
      const std::vector<IGenerationStage::FrameScheduleItem>& schedule,
      std::size_t frame_offset, std::size_t frame_count,
      const std::vector<SampleFixed>* pressing_y_mv,
      std::vector<SampleFixed>* y_mv, std::vector<SampleFixed>* c_mv) const;

  // True when the channel stage is active and some section's shared layer
  // carries a level term, so the caller must keep a copy of Y before the
  // channel stage and pass it as pressing_y_mv.
  static bool NeedsPressingLuma(const Project& project);

 private:
  // Returns the shaping filter for a spectrum table, designing it on first
  // use. Filters are cached per distinct (sample rate, table), so a section's
  // kernel is designed once per run rather than once per frame.
  std::shared_ptr<const NoiseSpectrumFilter> FilterFor(
      const std::vector<NoiseSpectrumPoint>& spectrum,
      double sample_rate_hz) const;

  ILogger* logger_;
  uint64_t run_base_seed_;

  mutable std::mutex filter_cache_mutex_;
  // Key: {sample_rate_hz, f0, dB0, f1, dB1, ...}.
  mutable std::map<std::vector<double>,
                   std::shared_ptr<const NoiseSpectrumFilter>>
      filter_cache_;
};

}  // namespace videosynth
