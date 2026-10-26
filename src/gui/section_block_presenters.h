/*
 * File:        section_block_presenters.h
 * Module:      gui
 * Purpose:     Widget-free mapping layer for the optional per-section YAML
 *              blocks (audio, noise, dropouts, OSD)
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
// over their arguments.

// Editor input limits mirrored from the rules in project_validator.cpp (and
// the design documents it cites) so widgets constrain values to exactly what
// validation accepts.
namespace editor_limits {
// Noise: noise-injection-design.md bounds enforced by ValidateNoiseParameters.
inline constexpr double kNoiseDbMin = 20.0;
inline constexpr double kNoiseDbMax = 61.0;
// Dropouts: active scale range enforced by ValidateDropoutParameters (0 only
// as "block disabled", so editors offer 1–20).
inline constexpr int kDropoutScaleMin = 1;
inline constexpr int kDropoutScaleMax = 20;
// OSD: glyph scale range enforced by ValidateOsdConfig (fg/bg levels are
// discrete enums chosen from dropdowns, so no numeric bounds).
inline constexpr int kOsdScaleMin = 1;
inline constexpr int kOsdScaleMax = 4;
// Audio: audio-generation-design.md bounds enforced by
// ValidateAudioParameters.
inline constexpr double kAudioFrequencyMinHz = 0.0;
inline constexpr double kAudioFrequencyMaxHz = 22000.0;
inline constexpr double kAudioAmplitudeMin = 0.0;
inline constexpr double kAudioAmplitudeMax = 1.0;
}  // namespace editor_limits

// Enable/disable helpers for the optional blocks. Disabling always resets
// the block to its default-constructed state so YamlProjectEmitter drops it
// from the emitted file (a disabled block must never emit defaults);
// enabling seeds validator-clean defaults.

void SetNoiseBlockEnabled(Section* section, bool enabled);
void SetNoiseSeedSpecified(Section* section, bool specified);

void SetRandomDropoutsEnabled(Section* section, bool enabled);
void SetScratchDropoutsEnabled(Section* section, bool enabled);

// A freshly-enabled audio channel (validator-clean fixed-frequency sine).
AudioParameters MakeDefaultAudioChannel();

// A freshly-added channel pair carrying `pair` with the same default tone on
// both channels (the editor's linked "same tone on both channels" mode).
AudioChannelPair MakeDefaultAudioChannelPair(int pair);

// Switching a channel to ramp mode seeds a validator-clean up-ramp; switching
// back clears every ramp field so the emitted block returns to fixed-frequency
// form.
void SetAudioChannelRampEnabled(AudioParameters* channel, bool enabled);
// Keeps waveform and waveform_text consistent; `waveform_name` must be one
// of "sine", "square", "sawtooth", "triangle".
void SetAudioChannelWaveform(AudioParameters* channel,
                             const std::string& waveform_name);
// The lowest channel-pair number 0–7 not already used by `pairs`, or -1 when
// all eight are taken.
int NextFreeAudioChannelPair(const std::vector<AudioChannelPair>& pairs);

// The OSD block exists exactly when the overlay list is non-empty.
bool OsdBlockEnabled(const Section& section);
OsdOverlay MakeDefaultOsdOverlay();

// Waveform / ramp-mode option lists for editors, in model.h parse order.
std::vector<std::string> AudioWaveformOptions();
std::vector<std::string> AudioRampModeOptions();

// Supported OSD template tokens with help text for the overlay editor.
struct OsdTokenHelp {
  std::string token;
  std::string description;
};
std::vector<OsdTokenHelp> OsdTokenCatalogue();

// Frames <-> seconds convenience conversion for the duration editor. The
// stored duration is always frames; seconds is a display-only mirror derived
// from the project standard's frame rate. FramesToSeconds returns 0.0 when
// the rate is not positive; SecondsToFrames rounds to the nearest frame and
// clamps the result to [1, max_frames].
double DurationFramesToSeconds(int frames, double frame_rate_hz);
int DurationSecondsToFrames(double seconds, double frame_rate_hz,
                            int max_frames);

// Resets the frame selection after the editor picks a new source; `section`
// already holds the new source and `previous_source` is the one it replaces.
// A video then plays all of its frames from the first (duration "all", no
// start_frame/end_frame). A still has neither a frame range nor a length of
// its own, so the range is cleared and an "all" duration becomes
// `still_duration_frames`. An unchanged source is left alone, so loading or
// re-committing a section never rewrites its duration.
void ApplySourceSelectionDefaults(const std::string& previous_source,
                                  int still_duration_frames, Section* section);

// How the editor's source-range widgets (start frame, end frame, "to end of
// source") present for a section.
struct SourceRangeEditorState {
  // Only video sources have frames to select; stills disable the widgets.
  bool enabled = false;
  // Largest frame index the widgets accept: the probed last frame, raised to
  // the stored start_frame/end_frame so a stale or pending probe never clamps
  // what the project holds; `unbounded_maximum` while the length is unknown.
  int maximum = 0;
};
SourceRangeEditorState SourceRangeEditorStateFor(const Section& section,
                                                 int probed_frame_count,
                                                 int unbounded_maximum);

// Multi-select batch editing: mirrors onto `target` exactly the editor-visible
// fields that differ between `before` and `after` (an edit applied to the
// primary selected section), leaving everything else in `target` untouched.
// The name is never mirrored — it identifies the individual section. The three
// duration fields propagate as a unit because the editor's widgets couple
// them (frames vs "all frames" x repeat), as do the three source frame range
// fields (start_frame, end_frame and whether the end is set). Line injections
// mirror per code via ApplyLineInjectionEditDelta, never wholesale. Returns the
// updated target.
Section ApplySectionEditDelta(const Section& before, const Section& after,
                              Section target);

// Mirrors a line-injection edit onto a batch target without replacing the
// target's list wholesale. Disabling the block (`after` empty) clears the
// target; otherwise the per-code diff between `before` and `after` (codes
// ticked, unticked, or revalued) is applied code by code, and a code is only
// added or updated when IsCodeTypeValidForSectionType accepts it for
// `target_section_type` — so a batch edit can never push a code into a
// section whose type forbids it (IEC 60856/60857 Appendix D). Codes the edit
// did not touch keep the target's own values, and no empty injection block is
// created when nothing valid remains to add.
std::vector<Section::LineInjection> ApplyLineInjectionEditDelta(
    const std::vector<Section::LineInjection>& before,
    const std::vector<Section::LineInjection>& after,
    std::vector<Section::LineInjection> target,
    SectionType target_section_type);

// Whether `type` may be assigned to every section of a multi-section
// selection at once. ValidateSectionOrdering permits at most one lead_in and
// one lead_out per project, so batch-assigning either can never validate;
// programme_area (and clearing the type) may repeat freely.
bool SectionTypeAllowsBatchAssignment(SectionType type);

}  // namespace videosynth::gui
