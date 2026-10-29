/*
 * File:        section_block_presenters.h
 * Module:      gui
 * Purpose:     Widget-free mapping layer for the optional per-section YAML
 *              blocks (audio, noise, dropouts, luma bandwidth, OSD)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <string>
#include <vector>

#include "videosynth/interfaces.h"
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
// Noise spectrum breakpoints: relative level bounds enforced by
// ValidateNoiseSpectrum. Frequencies run from 0 MHz to the 4fsc Nyquist
// frequency of the project standard (NoiseSpectrumMaxFrequencyMhz).
inline constexpr double kNoiseSpectrumRelativeDbMin = -60.0;
inline constexpr double kNoiseSpectrumRelativeDbMax = 40.0;
// Dropouts: active scale range enforced by ValidateDropoutParameters (0 only
// as "block disabled", so editors offer 1–20).
inline constexpr int kDropoutScaleMin = 1;
inline constexpr int kDropoutScaleMax = 20;
// Luma bandwidth: bounds enforced by ValidateLumaBandwidth. The cutoff's
// upper bound depends on the standard and the transition width
// (LumaBandwidthMaxCutoffMhz).
inline constexpr double kLumaBandwidthCutoffMinMhz = 0.5;
inline constexpr double kLumaBandwidthTransitionMinMhz = 0.1;
inline constexpr double kLumaBandwidthTransitionMaxMhz = 2.0;
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

// The noise block toggles the capture layer; noise.shared (the pressing
// layer) is left untouched by both directions.
void SetNoiseBlockEnabled(Section* section, bool enabled);
void SetNoiseSeedSpecified(Section* section, bool specified);

// The shared (pressing) noise layer is independent of the capture layer.
// Enabling seeds a validator-clean level and a fixed seed (the layer requires
// one); disabling resets it to its default so it is not emitted.
void SetSharedNoiseEnabled(Section* section, bool enabled);

// Highest breakpoint frequency a noise spectrum accepts for `standard`: the
// 4fsc Nyquist frequency, in MHz.
double NoiseSpectrumMaxFrequencyMhz(Standard standard);

// Appends a spectrum breakpoint above the last one: 1 MHz higher at the same
// level, or halfway between the last breakpoint and `max_frequency_mhz` when
// that is closer. The first breakpoint is 0 MHz at 0 dB. Nothing is added
// when the last breakpoint already sits at the maximum.
void AddNoiseSpectrumPoint(std::vector<NoiseSpectrumPoint>* spectrum,
                           double max_frequency_mhz);
// Removes breakpoint `row`; out-of-range rows change nothing.
void RemoveNoiseSpectrumPoint(std::vector<NoiseSpectrumPoint>* spectrum,
                              int row);

void SetRandomDropoutsEnabled(Section* section, bool enabled);
void SetScratchDropoutsEnabled(Section* section, bool enabled);

// Sets a dropout type's anchor. A disc anchor requires a seed, so choosing it
// on a type without one fixes the seed at 1; the section anchor leaves the
// seed as it is.
void SetRandomDropoutAnchor(Section* section, DropoutAnchor anchor);
void SetScratchDropoutAnchor(Section* section, DropoutAnchor anchor);

// Enabling the luma bandwidth limit seeds a 3.0 MHz cutoff with the default
// transition; disabling resets the block so it is not emitted.
void SetLumaBandwidthEnabled(Section* section, bool enabled);
// Highest cutoff the luma bandwidth limit accepts for `standard` with the
// given transition width: 2 f_sc less half the transition, in MHz.
double LumaBandwidthMaxCutoffMhz(Standard standard, double transition_mhz);

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

// Switches a channel pair to carry `content`, keeping only that kind's fields
// so the stored YAML names exactly one kind and round-trips unchanged. Tones
// start from the default tone on both channels; a file starts with no path
// and no repeat; a video soundtrack starts on stream 0. Choosing the kind the
// pair already carries changes nothing.
void SetAudioPairContent(AudioChannelPair* channel_pair,
                         AudioPairContent content);

// How the channel-pair editor's video-soundtrack choice presents for a
// section.
struct VideoSoundtrackEditorState {
  // Only a video source with (as far as is known) an audio stream offers its
  // soundtrack; an EXR still, or a probed video without audio, does not.
  bool selectable = false;
  // One entry per selectable stream index, in index order, e.g.
  // "Stream 0: aac, 2 ch, 48000 Hz". Before a successful probe the entries
  // are bare "Stream N" names up to the stored stream, and a stored stream
  // the probe did not find is still listed so it is never silently replaced.
  std::vector<std::string> stream_labels;
};
VideoSoundtrackEditorState VideoSoundtrackEditorStateFor(
    const std::string& source, bool probe_ok,
    const ProgressiveFrameSourceProfile& profile, int stored_stream);

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
// The name is never mirrored — it identifies the individual section. The
// capture and shared noise layers, and the random and scratch dropout types,
// mirror independently, as each is a separate block in the editor. The three
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
