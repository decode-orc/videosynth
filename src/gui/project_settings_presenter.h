/*
 * File:        project_settings_presenter.h
 * Module:      gui
 * Purpose:     Widget-free mapping layer for the project settings editor
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <string>
#include <vector>

#include "videosynth/model.h"

namespace videosynth::gui {

// Thread-safety: all functions in this module are thread-safe pure functions
// over their inputs; ProjectSettingsFormState is a plain data container.

// Channel and chroma error input limits mirrored from
// ValidateChannelParameters and ValidateChromaErrorParameters
// (project_validator.cpp) so widgets accept exactly what validation accepts.
namespace project_editor_limits {
inline constexpr double kTimingOffsetMaxSamples = 8.0;
inline constexpr double kEdgeSmearFractionMax = 0.5;
// tau_samples must be > 0; the editor's smallest step stands in for the open
// lower bound.
inline constexpr double kEdgeSmearTauMinSamples = 0.1;
inline constexpr double kEdgeSmearTauMaxSamples = 100.0;
inline constexpr double kFmBeatCarrierMinHz = 6.9e6;
inline constexpr double kFmBeatCarrierMaxHz = 7.3e6;
inline constexpr double kFmBeatLevelMinDb = -60.0;
inline constexpr double kFmBeatLevelMaxDb = 0.0;
inline constexpr double kChromaGainMin = 0.5;
inline constexpr double kChromaGainMax = 1.5;
inline constexpr double kChromaPhaseMaxDeg = 180.0;
inline constexpr double kChromaJitterPhaseMaxDeg = 10.0;
inline constexpr double kChromaJitterGainMax = 0.1;
// Differential gain and phase curves (ValidateChromaErrorParameters): luma
// levels in IRE, gains and phases at each level.
inline constexpr double kDifferentialLevelMinIre = -50.0;
inline constexpr double kDifferentialLevelMaxIre = 150.0;
inline constexpr double kDifferentialGainMin = 0.5;
inline constexpr double kDifferentialGainMax = 1.5;
inline constexpr double kDifferentialPhaseMaxDeg = 30.0;
// Chroma crosstalk coefficients (v_to_u and u_to_v).
inline constexpr double kCrosstalkMax = 0.5;
// LaserVision channel (ValidateLaserdiscChannel). Curve frequencies run from
// 0 MHz to the 4fsc Nyquist frequency of the standard
// (NoiseSpectrumMaxFrequencyMhz).
inline constexpr double kGroupDelayMaxNs = 1000.0;
inline constexpr double kGroupDelayScaleMax = 2.0;
inline constexpr double kReplayResponseMinDb = -60.0;
inline constexpr double kReplayResponseMaxDb = 20.0;
// response_by_picture picture numbers start at 1; the upper bound is the
// editor's, comfortably above any disc's picture count.
inline constexpr int kResponsePictureMin = 1;
inline constexpr int kResponsePictureMax = 999999;
inline constexpr double kField2OffsetMaxSamples = 8.0;
inline constexpr double kField2OffsetMaxLines = 4.0;
// Per-line timing error (ValidateLineTimingParameters).
inline constexpr double kLineTimingMaxNs = 50.0;
inline constexpr double kLineTimingCorrelationMax = 0.99;
}  // namespace project_editor_limits

// Outcome of the project-level LaserDisc digital audio (EFM) selection.
enum class EfmOutputStatus {
  // The project does not request EFM output.
  kDisabled,
  // Requested, but the video standard has no LaserDisc digital audio
  // specification (only PAL and NTSC do).
  kUnsupportedStandard,
  // Requested, but the selected channel pair is out of the 0–7 range.
  kPairOutOfRange,
  // Requested, but no section declares the selected channel pair.
  kPairNotDeclared,
  // Requested and emitted: an `.efm` stream is written for the selected pair.
  kActive,
};

// One breakpoint of a curve as the generic curve editor shows it: `x` is the
// abscissa (MHz or IRE) and `y` the value at it.
struct CurvePoint {
  double x = 0.0;
  double y = 0.0;
};

inline bool operator==(const CurvePoint& a, const CurvePoint& b) {
  return a.x == b.x && a.y == b.y;
}

// How the LaserVision replay amplitude response is given: not at all, one
// fixed response (response_db), or responses at several disc positions
// (response_by_picture). The two curve forms are mutually exclusive.
enum class ReplayResponseMode {
  kNone,
  kFixed,
  kByPicture,
};

// View state for the project settings form: the option catalogues plus the
// standard-dependent enablement flags. Enablement mirrors the rules
// ProjectValidator applies (pilot burst PAL-only, VBI burst NTSC-only, black
// setup IRE System-M-only, SECAM field identification and the unlocked signal
// state SECAM-only, no laserdisc format for SECAM) so invalid combinations
// cannot be entered.
struct ProjectSettingsFormState {
  std::vector<std::string> standard_options;
  std::vector<std::string> sample_encoding_options;
  std::vector<std::string> signal_state_options;
  std::vector<std::string> signal_type_options;
  std::vector<double> ntsc_black_setup_ire_options;

  // Standard-dependent enablement for the current project.
  bool pilot_burst_editable = false;
  bool vbi_burst_editable = false;
  bool setup_ire_editable = false;
  bool secam_field_identification_editable = false;
  // False for SECAM: there is no SECAM LaserVision format, so the laserdisc
  // block and laserdisc VITS placement are unavailable.
  bool laserdisc_editable = true;

  // The FM-beat model is fitted to 625-line PAL LaserDisc pressings, so
  // laserdisc.channel.fm_beat is offered for PAL only.
  bool fm_beat_editable = false;
  // The IEC 60856 nominal group-delay curve is the 625-line PAL
  // pre-distortion; other standards give a custom curve.
  bool group_delay_nominal_editable = false;
  // chroma_error.crosstalk.alternate_with_switch follows the PAL V-switch, so
  // it is offered for PAL and PAL-M only.
  bool crosstalk_alternate_editable = false;
  // chroma_error models a quadrature (PAL/NTSC) chroma path, so it is not
  // offered for SECAM.
  bool chroma_error_editable = true;
  // line_timing.include_burst moves the colour burst, which SECAM lacks.
  bool line_timing_burst_editable = true;

  // True when signal_type is "yc" and video_path must carry a ".cvbsy" suffix.
  bool video_path_requires_luma_suffix = false;

  // Channel-pair numbers offered by the EFM pair selector (0 .. 7).
  std::vector<int> efm_pair_options;
  // LaserDisc digital audio is specified for PAL (IEC 60856:1986 Amd 2,
  // clause 13) and NTSC (IEC 60857:1986 Amd 2, clause 13) only, so the enable
  // control is offered for those standards alone.
  bool efm_output_editable = false;
  // The pair selector additionally requires the output to be enabled.
  bool efm_pair_editable = false;
  // Why the EFM selection is (or is not) producing a stream. The view renders
  // the matching translated sentence; the reasons mirror ProjectValidator's
  // diagnostics and ProjectEfmAudioPair's predicate for the same conditions.
  EfmOutputStatus efm_status = EfmOutputStatus::kDisabled;

  // The picture truth (output.truth) carries the quadrature U/V components,
  // so it is offered for PAL, NTSC and PAL-M but not SECAM.
  bool truth_editable = true;
};

// Builds the form state (catalogues + enablement) for the given project.
ProjectSettingsFormState BuildProjectSettingsFormState(const Project& project);

// Clears preset flags the selected standard cannot carry, mirroring
// ProjectValidator: the black setup IRE override outside NTSC/PAL-M. The SECAM
// field identification option returns to its default outside SECAM, and the
// signal state is set to the one the standard requires
// (GeneratedSignalStatePreset). Returns the adjusted presets.
CvbsPresets NormalizeCvbsPresetsForStandard(CvbsPresets presets);

// Clears output selections the given standard cannot carry, mirroring
// ProjectValidator: the picture truth carries quadrature U/V components, so
// it is cleared for SECAM. Returns the adjusted targets.
OutputTargets NormalizeOutputTargetsForStandard(OutputTargets output,
                                                Standard standard);

// Clears laserdisc settings the given standard cannot carry, mirroring
// ProjectValidator: there is no SECAM LaserVision format (the whole block is
// cleared), the pilot burst, FM beat and IEC 60856 nominal group delay are
// PAL-only, the VBI burst is NTSC-only, and LaserDisc digital audio (EFM)
// exists only for the standards StandardHasLaserdiscDigitalAudio accepts.
// Returns the adjusted block.
LaserdiscParameters NormalizeLaserdiscForStandard(LaserdiscParameters laserdisc,
                                                  Standard standard);

// Clears the chroma error for SECAM, which has no quadrature chroma path, and
// the PAL V-switch crosstalk alternation outside PAL and PAL-M. Returns the
// adjusted block.
ChromaErrorParameters NormalizeChromaErrorForStandard(
    ChromaErrorParameters chroma_error, Standard standard);

// Clears line_timing.include_burst for SECAM, which has no colour burst.
// Returns the adjusted block.
LineTimingParameters NormalizeLineTimingForStandard(
    LineTimingParameters line_timing, Standard standard);

// Enable/disable helpers for the project-level channel, laserdisc and chroma
// error blocks. Disabling resets the block (or sub-block) to its default so
// the emitter drops it; enabling seeds validator-clean defaults and keeps any
// values already present. Enabling a sub-block also enables every block that
// holds it.
void SetChannelEnabled(ChannelParameters* channel, bool enabled);
void SetEdgeSmearEnabled(ChannelParameters* channel, bool enabled);
void SetChromaErrorEnabled(ChromaErrorParameters* chroma_error, bool enabled);
void SetChromaJitterEnabled(ChromaErrorParameters* chroma_error, bool enabled);
// Differential gain and phase: enabling seeds a mild gain droop and phase
// shift towards white.
void SetDifferentialEnabled(ChromaErrorParameters* chroma_error, bool enabled);
// Crosstalk: enabling seeds a small symmetric U/V leak.
void SetCrosstalkEnabled(ChromaErrorParameters* chroma_error, bool enabled);
// Line timing: enabling the block with neither layer present seeds the
// capture layer with figures measured on a real capture (4.9 ns per line,
// correlation 0.6, 3.1 ns per field, 3.4 ns slope).
void SetLineTimingEnabled(LineTimingParameters* line_timing, bool enabled);
// The pressing layer: enabling seeds a smaller recorded part with a fixed
// seed (validation requires one) and enables the block.
void SetSharedLineTimingEnabled(LineTimingParameters* line_timing,
                                bool enabled);

void SetLaserdiscEnabled(LaserdiscParameters* laserdisc, bool enabled);
void SetLaserdiscChannelEnabled(LaserdiscParameters* laserdisc, bool enabled);
void SetFmBeatEnabled(LaserdiscParameters* laserdisc, bool enabled);
// Enabling selects the IEC 60856 nominal curve for PAL and a custom curve
// seeded with the nominal breakpoints for the other standards (the preset is
// PAL-only).
void SetGroupDelayEnabled(LaserdiscParameters* laserdisc, bool enabled,
                          Standard standard);
// Switches the group-delay curve source. Choosing the custom curve seeds it
// with the nominal breakpoints when it is empty, so the switch starts from
// the curve that was in force.
void SetGroupDelayPreset(LaserdiscParameters* laserdisc,
                         GroupDelayPreset preset);
void SetField2OffsetEnabled(LaserdiscParameters* laserdisc, bool enabled);

// The replay response form the channel currently uses.
ReplayResponseMode ReplayResponseModeOf(
    const LaserdiscChannelParameters& channel);
// Switches the replay response form. kNone clears both curves; kFixed keeps
// response_db (seeded from the first by-picture entry, else a gentle default
// roll-off); kByPicture keeps response_by_picture (seeded with two entries
// carrying the fixed response, else the default). The form not chosen is
// cleared, so the two never coexist.
void SetReplayResponseMode(LaserdiscParameters* laserdisc,
                           ReplayResponseMode mode);

// Appends a response_by_picture entry after the last one, a fixed step of
// pictures later and carrying a copy of its curve; the first entry is
// picture 1 with the default roll-off.
void AddResponseByPictureEntry(std::vector<ResponseByPictureEntry>* entries);
// Removes entry `row`; out-of-range rows change nothing.
void RemoveResponseByPictureEntry(std::vector<ResponseByPictureEntry>* entries,
                                  int row);

// Appends a curve breakpoint `x_step` above the last one at the same value,
// or halfway between the last breakpoint and `x_max` when that is closer.
// The first breakpoint is `first`. Nothing is added when the last breakpoint
// already sits at `x_max`.
void AddCurvePoint(std::vector<CurvePoint>* points, double x_step, double x_max,
                   CurvePoint first);
// Removes breakpoint `row`; out-of-range rows change nothing.
void RemoveCurvePoint(std::vector<CurvePoint>* points, int row);

// Conversions between the model's breakpoint types and the generic curve
// editor's points.
std::vector<CurvePoint> ToCurvePoints(
    const std::vector<NoiseSpectrumPoint>& points);
std::vector<CurvePoint> ToCurvePoints(
    const std::vector<GroupDelayPoint>& points);
std::vector<CurvePoint> ToCurvePoints(
    const std::vector<LevelCurvePoint>& points);
std::vector<NoiseSpectrumPoint> ToResponsePoints(
    const std::vector<CurvePoint>& points);
std::vector<GroupDelayPoint> ToGroupDelayPoints(
    const std::vector<CurvePoint>& points);
std::vector<LevelCurvePoint> ToLevelCurvePoints(
    const std::vector<CurvePoint>& points);

// Derives the metadata path from a video path: strips a trailing
// ".cvbs" or ".cvbsy" (else any final extension) and appends ".meta".
// Returns an empty string for an empty video path.
std::string DeriveMetadataPath(const std::string& video_path);

// Adjusts a video path to the selected signal type: for "yc" the path must
// end in ".cvbsy" (a trailing ".cvbs" is rewritten, otherwise ".cvbsy" is
// appended); for "composite" a trailing ".cvbsy" is rewritten to ".cvbs".
std::string EnforceSignalTypeVideoPath(std::string video_path,
                                       const std::string& signal_type);

}  // namespace videosynth::gui
