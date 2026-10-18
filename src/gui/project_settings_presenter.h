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

// View state for the project settings form: the option catalogues plus the
// standard-dependent enablement flags. Enablement mirrors the rules
// ProjectValidator applies (pilot burst PAL-only, VBI burst NTSC-only, black
// setup IRE System-M-only, SECAM chroma PAL-only and exclusive with the pilot
// burst) so invalid combinations cannot be entered.
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
  // SECAM chroma rides on the PAL preset and excludes the pilot burst; the
  // field identification option only applies once SECAM is selected.
  bool secam_editable = false;
  bool secam_field_identification_editable = false;

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
};

// Builds the form state (catalogues + enablement) for the given project.
ProjectSettingsFormState BuildProjectSettingsFormState(const Project& project);

// Clears preset flags the selected standard cannot carry, mirroring
// ProjectValidator: pilot burst outside PAL, VBI burst outside NTSC, the
// black setup IRE override outside NTSC/PAL-M, and SECAM chroma outside PAL.
// SECAM takes precedence over the pilot burst, and the SECAM field
// identification option returns to its default when SECAM is off. Returns the
// adjusted presets.
CvbsPresets NormalizeCvbsPresetsForStandard(CvbsPresets presets);

// Clears output selections the given standard cannot carry, mirroring
// ProjectValidator: LaserDisc digital audio (EFM) exists only for PAL and
// NTSC, so the selection is disabled for any other standard. Returns the
// adjusted targets.
OutputTargets NormalizeOutputTargetsForStandard(OutputTargets output,
                                                Standard standard);

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
