/*
 * File:        biphase_injection_manager.h
 * Module:      biphase_injection_manager
 * Purpose:     Orchestrates biphase and 40-bit FM code injection into VBI
 *              lines of the video signal for LaserDisc authoring per
 *              IEC 60856 (PAL) and IEC 60857 (NTSC).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "videosynth/biphase_encoder.h"
#include "videosynth/biphase_types.h"
#include "videosynth/code_generator.h"
#include "videosynth/fixed_point.h"
#include "videosynth/fm_code_generator.h"
#include "videosynth/fm_encoder.h"
#include "videosynth/frame_enrichment.h"
#include "videosynth/line_placement_engine.h"
#include "videosynth/model.h"
#include "videosynth/signal_timing_model.h"
#include "videosynth/timing_constants.h"

namespace videosynth {

// Writes the fully-resolved VBI waveforms for one frame into the luma buffer.
//
// Pure function of its arguments: it holds no state across calls and may be
// invoked concurrently from multiple threads on distinct buffers (or disjoint
// frame regions of a shared buffer). Encoders are constructed locally from
// sample_rate_hz, producing waveforms identical to the sequential
// BiphaseInjectionManager::ProcessFrame path.
//
// Args:
//   enrichment:                Resolved per-frame VBI payload (vbi_lines).
//   out_y_mv:                  Luma sample buffer for the current batch.
//   frame_sample_base:         Offset of this frame's first sample in
//                              out_y_mv.
//   line_sample_offsets:       Per-line sample offsets relative to the frame
//                              base (indexed by line_number - 1).
//   line_sample_counts:        Samples per line (indexed by line_number - 1).
//   standard:                  PAL or NTSC.
//   sample_rate_hz:            4fsc sample rate in Hz.
//   active_window_end_samples: Sample offset (exclusive) of the
//                              active-picture window end within a line;
//                              injection is clamped to this boundary.
void InjectResolvedVbiLines(const FrameEnrichment& enrichment,
                            std::vector<SampleFixed>* out_y_mv,
                            int frame_sample_base,
                            const std::vector<int>& line_sample_offsets,
                            const std::vector<int>& line_sample_counts,
                            Standard standard, double sample_rate_hz,
                            int active_window_end_samples);

// Writes resolved VBI waveforms into the luma buffer, reusing the
// frame-invariant encoder state across frames.
//
// The biphase and FM encoders and the white flag pulse shape are pure
// functions of (standard, sample rate): building them costs two S-curve
// inversion searches plus a shaped-pulse render, none of which varies per
// frame. A renderer therefore builds them once and reuses them for every frame
// it writes, producing waveforms identical to the free InjectResolvedVbiLines
// function above.
//
// Thread-safety: NOT thread-safe — the FM encoder and white flag waveform are
// filled in on first use. Construct one renderer per worker thread.
class VbiWaveformRenderer {
 public:
  // Args:
  //   standard:       PAL, NTSC or PAL-M (selects signal levels and timing).
  //   sample_rate_hz: 4fsc sample rate in Hz.
  VbiWaveformRenderer(Standard standard, double sample_rate_hz);

  // Renders every entry of enrichment.vbi_lines. Arguments carry the same
  // meaning as the free InjectResolvedVbiLines function above.
  void Render(const FrameEnrichment& enrichment,
              std::vector<SampleFixed>* out_y_mv, int frame_sample_base,
              const std::vector<int>& line_sample_offsets,
              const std::vector<int>& line_sample_counts,
              int active_window_end_samples) const;

 private:
  // Returns the FM encoder, constructing it on first use. The encoder is only
  // needed for NTSC FM codes and for the white flag ramp width.
  const FmEncoder& GetFmEncoder() const;

  // Returns the shaped white flag pulse for flag_length_samples, rendering it
  // on first use (and again only if a caller asks for a different length).
  const std::vector<SampleFixed>& GetWhiteFlagPulse(
      int flag_length_samples) const;

  Standard standard_;
  double sample_rate_hz_;
  SignalLevels levels_;
  BiphaseEncoder biphase_encoder_;
  mutable std::unique_ptr<FmEncoder> fm_encoder_;
  mutable std::vector<SampleFixed> white_flag_pulse_;
  mutable int white_flag_pulse_length_ = -1;
};

// Orchestrates biphase and 40-bit FM VBI injection for LaserDisc authoring.
//
// The manager maintains stateful code generators across frames and sections.
// Section transitions are detected automatically by comparing section pointers.
// Most generators are re-created on each section transition, but the
// disc-global timekeeping generators (programme_time_code, clv_picture_number,
// fm_programme_time, fm_picture_number) persist across sections so that the
// encoded time and picture values are continuous across chapter boundaries.
// Call Reset() to restart all generators from their initial values.
//
// Typical usage inside a frame generation loop:
//
//   BiphaseInjectionManager biphase_manager;
//   for (each frame) {
//     if (!biphase_manager.ProcessFrame(out_y_mv, local_frame_base,
//             line_offsets, line_counts, *section, standard, sample_rate,
//             frame_lines, active_window_start, errors)) {
//       return false;
//     }
//   }
//
// Call Reset() when starting a new generation pass (e.g. after
// BuildFrameSchedule) so that generators restart from their initial values.
//
// Signal level conventions:
//   PAL  24-bit biphase: baseline = 0 mV (blanking),    peak = 700 mV
//   NTSC 24-bit biphase: baseline =   0 mV (0 IRE),    peak = 714.3 mV
//   NTSC 40-bit FM:      baseline =   0 mV,             peak = 714.3 mV
//   White flag:          100 IRE pulse, 135 ns rise/fall (IEC 60857 Figure 12)
//
// Horizontal start timing per IEC 60856 Figure 14 / IEC 60857 Figures 11–13:
//   24-bit biphase codes: signal starts at active_window_start_samples.
//   0.172 H offset codes (programme_status, NTSC clv_code):
//                        signal starts at round(0.172 × samples_per_line).
//   NTSC white flag (IEC 60857 Figure 12):
//                        pulse starts at round(0.160 × samples_per_line),
//                        length = round(0.790 × samples_per_line).
//   NTSC 40-bit FM codes (IEC 60857 Figure 13):
//                        signal starts at round(0.215 × samples_per_line).
//   All codes end at active_window_end_samples (exclusive).
//
// Thread-safety: NOT thread-safe. Maintains mutable generator state.
class BiphaseInjectionManager {
 public:
  BiphaseInjectionManager() = default;

  // Resets all generator state. Must be called before starting a new
  // generation pass so that stateful generators (picture_number, etc.)
  // restart from their configured initial values.
  void Reset();

  // Sets the initial frame count used to derive colour_frame_index.
  // Call this after Reset() with (first_picture_number - 1) so that
  // colour_frame_index == (PN - 1) % colour_period for every video-cadence
  // frame, matching the disc-accurate colour phase for a CAV disc. 2:3
  // pulldown frames always use the frame count, because their picture
  // numbers count film pictures rather than disc frames.
  // Has no effect if called before Reset() (Reset() overwrites frame_count_).
  void SetInitialFrameCount(int initial_count);

  // Sets the project-wide laserdisc disc format (CAV/CLV). The disc_type is a
  // project-level decision shared by every section, so it is supplied once per
  // generation pass (after Reset()) rather than derived per section. A section
  // that carries laserdisc code injections only produces biphase output when a
  // known disc_type has been set here.
  void SetProjectDiscType(DiscType disc_type);

  // Resolves the VBI injection payload for one frame without writing samples.
  //
  // This is the sequential half of frame processing: it detects section
  // transitions, derives the colour-frame index, captures the current code
  // words (24-bit biphase values, 40-bit FM payloads, white flag placement)
  // as VbiLineInjection entries in out_enrichment->vbi_lines, updates the
  // GetLastFrameContext() snapshot (also copied to out_enrichment->context),
  // and advances all stateful generators. Must be called exactly once per
  // frame in schedule order.
  //
  // The resolved payload is later rendered by InjectResolvedVbiLines, which
  // is stateless and may run on any thread. out_enrichment->disc_frame_index
  // and field_osd_texts are NOT populated here; the caller owns those fields.
  //
  // The frame is one video-cadence picture: field 1 starts it.
  //
  // Args:
  //   section:                     The section rendered in this frame.
  //   standard:                    PAL or NTSC.
  //   sample_rate_hz:              4fsc sample rate in Hz.
  //   active_window_start_samples: Sample offset of the active-picture window
  //                                start within a line; used as the normal
  //                                biphase horizontal start position.
  //   out_enrichment:              Receives vbi_lines and context; non-null.
  //   errors:                      Output for error messages; non-null.
  //
  // Returns true on success. Returns false and appends a message to errors
  // on any error.
  bool ResolveFrame(const Section& section, Standard standard,
                    double sample_rate_hz, int active_window_start_samples,
                    FrameEnrichment* out_enrichment,
                    std::vector<std::string>* errors);

  // As above, with picture_starts[i] true when field i + 1 is the first field
  // of a picture. In a 2:3 pulldown section the CAV picture_number and
  // fm_picture_number advance once per picture start rather than once per
  // frame. The picture-field codes, picture stop and white flag follow the
  // fields that start pictures (see LinePlacementEngine). In every cadence
  // the FM picture number changes on each picture's second field (IEC 60857
  // §10.2.3).
  bool ResolveFrame(const Section& section, Standard standard,
                    double sample_rate_hz, int active_window_start_samples,
                    const std::array<bool, 2>& picture_starts,
                    FrameEnrichment* out_enrichment,
                    std::vector<std::string>* errors);

  // Processes biphase VBI injection for one frame: ResolveFrame followed by
  // InjectResolvedVbiLines on the same buffer.
  //
  // Must be called for every frame in the schedule in order; the manager
  // advances stateful code generators at the end of each call. If the
  // section differs from the previous call, generators are re-created.
  //
  // Args:
  //   out_y_mv:                  Luma sample buffer for the current batch.
  //   frame_sample_base:         Byte offset of this frame's first sample
  //                              within out_y_mv.
  //   line_sample_offsets:       Per-line byte offsets relative to the
  //                              frame base (indexed by line_number - 1).
  //   line_sample_counts:        Samples per line (indexed by line_number - 1).
  //   section:                   The section being rendered for this frame.
  //   standard:                  PAL or NTSC.
  //   sample_rate_hz:            4fsc sample rate in Hz.
  //   frame_lines:               Timing primitives for every line in the frame.
  //   active_window_start_samples: Sample offset of the active-picture window
  //                              start within a line; used as the normal
  //                              biphase horizontal start position.
  //   active_window_end_samples: Sample offset (exclusive) of the
  //   active-picture
  //                              window end within a line; injection is clamped
  //                              to this boundary so signals do not bleed into
  //                              the front porch.
  //   errors:                    Output for error messages; non-null required.
  //
  // Returns true on success. Returns false and appends a message to errors
  // on any error.
  bool ProcessFrame(std::vector<SampleFixed>* out_y_mv, int frame_sample_base,
                    const std::vector<int>& line_sample_offsets,
                    const std::vector<int>& line_sample_counts,
                    const Section& section, Standard standard,
                    double sample_rate_hz,
                    const std::vector<LineTimingPrimitive>& frame_lines,
                    int active_window_start_samples,
                    int active_window_end_samples,
                    std::vector<std::string>* errors);

  // Returns the context snapshot captured during the most recent ProcessFrame()
  // call.  The snapshot reflects the biphase codes and colour-frame index that
  // were active for that frame; it is reset to defaults by Reset().
  const PerFrameContext& GetLastFrameContext() const;

 private:
  // The section being processed; null = no section yet / reset.
  const Section* current_section_ = nullptr;

  // 24-bit biphase code generators, keyed by code_type string.
  std::unordered_map<std::string, std::unique_ptr<CodeGenerator>> generators_;

  // 40-bit FM generators (NTSC only).
  std::unique_ptr<FmPictureNumberGenerator> fm_picture_number_generator_;
  std::unique_ptr<FmProgrammeTimeGenerator> fm_programme_time_generator_;

  std::unique_ptr<LinePlacementEngine> placement_engine_;
  std::unique_ptr<BiphaseEncoder> biphase_encoder_;
  std::unique_ptr<FmEncoder> fm_encoder_;

  // Frame-invariant horizontal placement derived from the standard's line
  // length; resolved once per section in InitializeSection.
  struct LineOffsets {
    int lines_per_frame = 0;
    int white_flag_start_samples = 0;
    int white_flag_length_samples = 0;
    int offset_172h_samples = 0;
    int fm_start_samples = 0;
  };
  LineOffsets line_offsets_;

  // Project-wide disc format, set once per generation pass via
  // SetProjectDiscType. disc_type_ mirrors it for the active section.
  DiscType project_disc_type_ = DiscType::kUnknown;
  DiscType disc_type_ = DiscType::kUnknown;
  SectionType section_type_ = SectionType::kUnknown;
  bool has_laserdisc_ = false;

  // Snapshot populated at the end of each ProcessFrame() call.
  PerFrameContext last_context_ = {};
  // Monotone frame counter; resets to 0 on Reset(). Used to derive
  // colour_frame_index without needing access to the timing model here.
  int frame_count_ = 0;

  // True when the last field resolved started a picture, so the next field
  // carries its picture stop code (even across a frame or section boundary).
  bool previous_field_started_picture_ = false;
  // CAV picture number of the picture the last resolved field showed.
  int last_picture_number_ = 0;
  // FM picture number on the last resolved programme-area field, or -1
  // before the first one; a picture's first field repeats it.
  int fm_shown_value_ = -1;

  // (Re)initialises all generators for a new section.
  bool InitializeSection(const Section& section, Standard standard,
                         double sample_rate_hz,
                         std::vector<std::string>* errors);

  // Advances all stateful code generators by one frame, except
  // fm_picture_number, which ResolveFmPictureNumber advances per picture
  // start. When pictures_advanced is true the CAV picture_number is left
  // alone too: a 2:3 pulldown frame advances it once per picture start.
  void AdvanceGenerators(bool pictures_advanced);

  // Resolves one field's FM picture number, advancing the generator when the
  // field starts a picture. A picture's first field still carries the
  // previous picture's number; the new number appears from its second field
  // (IEC 60857 §10.2.3). The very first programme picture, with no previous
  // number, carries its own number in its first field.
  FmData ResolveFmPictureNumber(bool field_one, bool starts_picture);

  // Returns a pointer to the generator for code_type, or nullptr if absent.
  CodeGenerator* GetGenerator(const std::string& code_type) const;
};

}  // namespace videosynth
