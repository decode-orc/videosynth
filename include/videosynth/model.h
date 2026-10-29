/*
 * File:        model.h
 * Module:      model
 * Purpose:     Defines project data models and video-standard enums.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "videosynth/biphase_types.h"

namespace videosynth {

// Thread-safety: All types, enums, and functions in this module are
// thread-safe. They are plain data containers or stateless functions. May be
// accessed concurrently from multiple threads.
enum class Standard {
  kPal,
  kNtsc,
  // PAL-M: System M line structure (525 lines, ~60 Hz) with PAL colour
  // encoding. Used in Brazil. ITU-R BT.470-6 Table 2 (M/PAL column).
  kPalM,
  // SECAM: 625-line, 25 frame/s system with frequency-modulated chrominance
  // (ITU-R BT.1700 Annex 1 Part C). Shares PAL's line and field timing,
  // signal levels and 4fsc storage lattice (CVBS File Format Specification
  // v1.7.0, Video Standard Presets, Preset: SECAM).
  kSecam,
  kUnknown,
};

inline Standard StandardFromString(const std::string& value) {
  if (value == "PAL") {
    return Standard::kPal;
  }
  if (value == "NTSC") {
    return Standard::kNtsc;
  }
  if (value == "PAL_M" || value == "PAL-M") {
    return Standard::kPalM;
  }
  if (value == "SECAM") {
    return Standard::kSecam;
  }
  return Standard::kUnknown;
}

inline std::string StandardToString(Standard standard) {
  switch (standard) {
    case Standard::kPal:
      return "PAL";
    case Standard::kNtsc:
      return "NTSC";
    case Standard::kPalM:
      return "PAL_M";
    case Standard::kSecam:
      return "SECAM";
    default:
      return "UNKNOWN";
  }
}

// True for the 625-line, 25 frame/s standards. PAL and SECAM share the line and
// field timing, sync structure, signal levels, active raster and 4fsc storage
// lattice; only the colour encoding differs (ITU-R BT.1700 Annex 1 Part B
// and Part C).
inline bool Is625LineStandard(Standard standard) {
  return standard == Standard::kPal || standard == Standard::kSecam;
}

// VITS placement policy for a project. Governs where the project-wide VITS test
// signals may sit and how strictly the validator checks their frame lines.
//   kStandard  — each VITS type sits on its broadcast recommended line (the
//                catalogue's recommended_frame_line); any other line is
//                rejected. This is the default and the policy applied to
//                projects that predate the placement field.
//   kLaserdisc — VITS sit on the laserdisc VBI lines mandated by IEC 60856
//                (PAL) / IEC 60857 (NTSC), clear of the address-code lines.
//   kCustom    — VITS may sit on any valid VBI line; the validator still
//                rejects overlaps and (for code-carrying discs) the reserved
//                address-code ranges.
enum class VitsPlacement { kStandard, kLaserdisc, kCustom };

inline VitsPlacement VitsPlacementFromString(const std::string& value) {
  if (value == "laserdisc") {
    return VitsPlacement::kLaserdisc;
  }
  if (value == "custom") {
    return VitsPlacement::kCustom;
  }
  return VitsPlacement::kStandard;
}

inline std::string VitsPlacementToString(VitsPlacement placement) {
  switch (placement) {
    case VitsPlacement::kLaserdisc:
      return "laserdisc";
    case VitsPlacement::kCustom:
      return "custom";
    case VitsPlacement::kStandard:
    default:
      return "standard";
  }
}

// The VITS VBI lines permitted on a laserdisc of this standard. On laserdisc,
// the broadcast VITS lines (17/18/330/331 PAL, 17/18/280/281 NTSC) collide with
// the reserved address/data VBI ranges, so VITS are carried on these lines
// instead. Returned in field order (field 1 then field 2).
//   PAL:        IEC 60856 §9.1.3 — lines 19, 20, 332, 333.
//   NTSC/PAL-M: IEC 60857 §9.1.3 (VIRS 19/282) and §9.1.4 (ITS 20/283).
inline std::vector<int> LaserdiscVitsLines(Standard standard) {
  switch (standard) {
    case Standard::kPal:
      return {19, 20, 332, 333};
    case Standard::kNtsc:
    case Standard::kPalM:
      return {19, 20, 282, 283};
    default:
      return {};
  }
}

// Placeholder for project-level biphase configuration.
// All disc-specific biphase parameters are specified per section and
// per injection; this struct is reserved for any future global settings.
struct BiphasePresets {};

struct CvbsPresets {
  Standard video_standard_preset = Standard::kUnknown;
  std::string sample_encoding_preset = "CVBS_U10_4FSC";
  std::string signal_state_preset = "STANDARD_STABLE_LOCKED";
  bool pal_laserdisc_pilot_burst = false;
  bool ntsc_laserdisc_vbi_burst = false;
  double ntsc_black_setup_ire = 7.5;
  bool ntsc_black_setup_ire_specified = false;
  // SECAM only: emit the field identification signals on lines 7-15 and
  // 320-328 (ITU-R BT.1700 Annex 1 Part C Table 4 item 13). Line
  // identification on the back porch is always present.
  bool secam_field_identification = true;
  BiphasePresets biphase_presets;
};

inline bool IsSupportedNtscBlackSetupIre(double setup_ire) {
  constexpr double kEpsilon = 1e-9;
  return std::abs(setup_ire - 7.5) < kEpsilon || std::abs(setup_ire) < kEpsilon;
}

inline bool Is4fscSampleEncodingPreset(const std::string& preset) {
  return preset == "CVBS_U10_4FSC" || preset == "CVBS_U16_4FSC" ||
         preset == "CVBS_TPG21_4FSC" || preset == "CVBS_S16_4FSC";
}

inline bool IsSupportedSampleEncodingPreset(const std::string& preset) {
  return Is4fscSampleEncodingPreset(preset) || preset == "RAW_S16_28M" ||
         preset == "RAW_S16_40M";
}

// Returns true if the preset can represent sub-sync excursions (below -300 mV).
// The PAL pilot burst swings ±300 mV about sync tip, so troughs reach -600 mV.
// U10/U16/TPG21 presets clamp at the 10-bit legal-code floor (~-300 mV);
// CVBS_S16_4FSC and RAW_S16_* presets carry the full excursion.
inline bool IsSubSyncCapableSampleEncodingPreset(const std::string& preset) {
  return preset == "CVBS_S16_4FSC" || preset == "RAW_S16_28M" ||
         preset == "RAW_S16_40M";
}

inline std::string SampleRateModeFromEncodingPreset(const std::string& preset) {
  if (Is4fscSampleEncodingPreset(preset)) {
    return "4fsc";
  }
  if (preset == "RAW_S16_28M") {
    return "28M";
  }
  if (preset == "RAW_S16_40M") {
    return "40M";
  }
  return "unknown";
}

// The signal state preset that generated output declares for `standard`.
// Synthesised output is always time-base stable at the standard 4fsc rate.
// It is phase locked for the quadrature standards, but SECAM defines no Sc/H
// convention and must not be declared phase locked (CVBS File Format
// Specification v1.7.0, Preset: SECAM, Subcarrier phase and phase locking).
inline std::string GeneratedSignalStatePreset(Standard standard) {
  return standard == Standard::kSecam ? "STANDARD_STABLE_UNLOCKED"
                                      : "STANDARD_STABLE_LOCKED";
}

// One breakpoint of a noise power-spectrum shape. Breakpoints are joined by
// straight lines in dB against MHz; the end values are held out to 0 Hz and
// to the Nyquist frequency.
struct NoiseSpectrumPoint {
  double frequency_mhz = 0.0;
  // Level relative to the other breakpoints. Valid range: [-60.0, +40.0].
  // Only the shape matters: the filtered noise is normalised back to the
  // layer's noise_db.
  double relative_db = 0.0;
};

// One breakpoint of a level curve: a gain in dB at a luma level. Breakpoints
// are joined by straight lines in dB against mV; the end values are held
// beyond the first and last points.
struct NoiseLevelPoint {
  // Luma level in mV above blanking. Valid range: [0.0, 800.0].
  double level_mv = 0.0;
  // Gain relative to the term's noise_db. Valid range: [-60.0, +20.0].
  double relative_db = 0.0;
};

// Level-dependent term of the shared (pressing) layer: a second pressing
// component, drawn independently of the constant one, whose amplitude follows
// a piecewise-linear dB curve against the pressing picture's luma level. The
// curve may fall with level, which noise_spread_db cannot express.
struct SharedNoiseLevelTerm {
  bool enabled = false;
  // Level of the term where the curve reads 0 dB. Valid range: [20.0, 61.0].
  double noise_db = 61.0;
  // Required when enabled: at least one point, levels strictly ascending.
  std::vector<NoiseLevelPoint> level_db;
  // Empty means white noise.
  std::vector<NoiseSpectrumPoint> spectrum;
};

// The shared (pressing) noise layer. It is keyed by disc position rather
// than by section or output position, so every capture of one pressing
// carries identical pressing noise on the same picture. Its level dependence,
// if any, comes from level_term (driven by the pressing's own picture), so it
// takes no noise_spread_db.
struct SharedNoiseParameters {
  bool enabled = false;
  // Noise level in dB. Valid range: [20.0, 61.0].
  double noise_db = 61.0;
  // Required by validation: pressing noise must be reproducible.
  int64_t noise_seed = 0;
  bool noise_seed_specified = false;
  // Empty means white noise.
  std::vector<NoiseSpectrumPoint> spectrum;
  // Optional level-dependent pressing component.
  SharedNoiseLevelTerm level_term = {};
};

struct NoiseParameters {
  // True when the capture layer is present (noise_db was given).
  bool enabled = false;
  // Noise floor level in dB. Sets Black PSNR target. Valid range: [20.0, 61.0].
  double noise_db = 61.0;
  // White is this many dB noisier than black. Valid range: [0.0,
  // noise_db-20.0].
  double noise_spread_db = 0.0;
  // When noise_seed_specified is true, this seed is mixed into the per-frame
  // RNG seed to produce deterministic output across runs. When false, a
  // random base seed captured at pipeline construction time is used instead,
  // so every run produces different noise.
  int64_t noise_seed = 0;
  bool noise_seed_specified = false;
  // Capture-layer spectrum shape; empty means white noise. With
  // spread_spectrum empty it shapes the whole capture layer, floor and
  // level-dependent term alike; otherwise it shapes the floor only.
  std::vector<NoiseSpectrumPoint> spectrum;
  // Spectrum shape of the level-dependent (noise_spread_db) term, drawn
  // independently of the floor. Empty means the term follows `spectrum`.
  std::vector<NoiseSpectrumPoint> spread_spectrum;
  // Pressing layer, independent of the capture layer above.
  SharedNoiseParameters shared = {};
};

// What a dropout type's events are keyed to. kSection events belong to one
// capture: they are seeded by section and output position. kDisc events are
// a pressing defect: they are seeded by disc position, so captures of one
// pressing (and a replay of the same pictures) carry the same events.
enum class DropoutAnchor { kSection, kDisc };

struct RandomDropoutParameters {
  bool enabled = false;
  int scale = 0;  // 1–20; maps to frequency and max_duration via scale mapping
  bool seed_specified = false;
  int64_t seed = 0;
  // False applies the events but records no sidecar rows for them.
  bool flagged = true;
  DropoutAnchor anchor = DropoutAnchor::kSection;
};

struct ScratchDropoutParameters {
  bool enabled = false;
  int scale = 0;  // 1–20; maps to count, max_duration_frames, max_width_samples
  bool seed_specified = false;
  int64_t seed = 0;
  // False applies the events but records no sidecar rows for them.
  bool flagged = true;
  DropoutAnchor anchor = DropoutAnchor::kSection;
};

struct DropoutParameters {
  RandomDropoutParameters random = {};
  ScratchDropoutParameters scratch = {};
  // True when the project file carried a dropouts: block, so validation can
  // reject a block that enables nothing. Set by the parser only and not
  // compared by operator== (the emitter never writes an inert block).
  bool block_present = false;
};

// Default luma_bandwidth.transition_mhz.
constexpr double kLumaBandwidthTransitionDefaultMhz = 0.4;

// Per-section luma bandwidth limit: a low-pass applied to the active picture
// luma as it is painted. Sync, VBI, VITS, chroma and OSD are not filtered.
struct LumaBandwidthParameters {
  bool enabled = false;
  // -6 dB point. Valid range: [0.5, 2 f_sc - transition_mhz / 2].
  double cutoff_mhz = 0.0;
  // Full width of the transition band, centred on the cutoff. Valid range:
  // [0.1, 2.0].
  double transition_mhz = kLumaBandwidthTransitionDefaultMhz;
};

// On-screen display foreground level. Only four discrete levels are
// supported; kUnknown marks an unrecognised YAML value so the validator can
// reject it with a clear message.
enum class OsdFgLevel { kWhite, kLightGrey, kDarkGrey, kBlack, kUnknown };

inline OsdFgLevel OsdFgLevelFromString(const std::string& value) {
  if (value == "white") {
    return OsdFgLevel::kWhite;
  }
  if (value == "light_grey") {
    return OsdFgLevel::kLightGrey;
  }
  if (value == "dark_grey") {
    return OsdFgLevel::kDarkGrey;
  }
  if (value == "black") {
    return OsdFgLevel::kBlack;
  }
  return OsdFgLevel::kUnknown;
}

inline std::string OsdFgLevelToString(OsdFgLevel level) {
  switch (level) {
    case OsdFgLevel::kWhite:
      return "white";
    case OsdFgLevel::kLightGrey:
      return "light_grey";
    case OsdFgLevel::kDarkGrey:
      return "dark_grey";
    case OsdFgLevel::kBlack:
      return "black";
    case OsdFgLevel::kUnknown:
      break;
  }
  return {};
}

// Maps an OSD foreground level to its luma value E_Y'. kUnknown is rejected
// by validation before rendering; it maps to white as a safe fallback.
inline double OsdFgLevelToLuma(OsdFgLevel level) {
  switch (level) {
    case OsdFgLevel::kLightGrey:
      return 0.75;
    case OsdFgLevel::kDarkGrey:
      return 0.25;
    case OsdFgLevel::kBlack:
      return 0.0;
    case OsdFgLevel::kWhite:
    case OsdFgLevel::kUnknown:
      break;
  }
  return 1.0;
}

// On-screen display background level: the four discrete luma steps plus
// kTransparent (no background write). kUnknown marks an unrecognised YAML
// value so the validator can reject it with a clear message.
enum class OsdBgLevel {
  kTransparent,
  kWhite,
  kLightGrey,
  kDarkGrey,
  kBlack,
  kUnknown
};

inline OsdBgLevel OsdBgLevelFromString(const std::string& value) {
  if (value == "transparent") {
    return OsdBgLevel::kTransparent;
  }
  if (value == "white") {
    return OsdBgLevel::kWhite;
  }
  if (value == "light_grey") {
    return OsdBgLevel::kLightGrey;
  }
  if (value == "dark_grey") {
    return OsdBgLevel::kDarkGrey;
  }
  if (value == "black") {
    return OsdBgLevel::kBlack;
  }
  return OsdBgLevel::kUnknown;
}

inline std::string OsdBgLevelToString(OsdBgLevel level) {
  switch (level) {
    case OsdBgLevel::kTransparent:
      return "transparent";
    case OsdBgLevel::kWhite:
      return "white";
    case OsdBgLevel::kLightGrey:
      return "light_grey";
    case OsdBgLevel::kDarkGrey:
      return "dark_grey";
    case OsdBgLevel::kBlack:
      return "black";
    case OsdBgLevel::kUnknown:
      break;
  }
  return {};
}

// Maps an OSD background level to its luma value E_Y'. Only meaningful for
// the four opaque levels; kTransparent/kUnknown map to black as a safe
// fallback (the renderer skips transparent backgrounds entirely).
inline double OsdBgLevelToLuma(OsdBgLevel level) {
  switch (level) {
    case OsdBgLevel::kWhite:
      return 1.0;
    case OsdBgLevel::kLightGrey:
      return 0.75;
    case OsdBgLevel::kDarkGrey:
      return 0.25;
    case OsdBgLevel::kBlack:
    case OsdBgLevel::kTransparent:
    case OsdBgLevel::kUnknown:
      break;
  }
  return 0.0;
}

// Thread-safety: OsdOverlay and OsdConfig are plain data containers with no
// mutable state. They may be read concurrently from multiple threads.
struct OsdOverlay {
  // Literal text or template containing {picture_number}, {biphase_hex},
  // {phase_id}, or {section_name} tokens (resolved per-frame at render time).
  std::string text;
  // Active-area x offset in pixels (0 = left edge of active area).
  int x = 0;
  // Active-area y offset in lines (0 = first active line of frame).
  int y = 0;
  // Glyph scale factor: 1 = 8×8 px per glyph, 2 = 16×16 px. Range [1, 4].
  int scale = 1;
  // Foreground level: one of the four supported OSD luma steps.
  OsdFgLevel fg_level = OsdFgLevel::kWhite;
  // Raw YAML fg_luma value retained for validation error messages only;
  // excluded from equality and never re-emitted.
  std::string fg_level_text;
  // Background level: transparent (no write) or one of the four supported
  // OSD luma steps.
  OsdBgLevel bg_level = OsdBgLevel::kTransparent;
  // Raw YAML bg_luma value retained for validation error messages only;
  // excluded from equality and never re-emitted.
  std::string bg_level_text;
};

struct OsdConfig {
  std::vector<OsdOverlay> overlays;
};

// Oscillator shape for synthetic per-section audio. kUnknown marks an
// unrecognised YAML value so the validator can reject it with a clear message.
enum class AudioWaveform { kSine, kSquare, kSawtooth, kTriangle, kUnknown };

// Direction of a frequency ramp within a section (or within one ramp period).
enum class AudioRampMode { kUp, kDown, kBounce, kUnknown };

inline AudioWaveform AudioWaveformFromString(const std::string& value) {
  if (value == "sine") {
    return AudioWaveform::kSine;
  }
  if (value == "square") {
    return AudioWaveform::kSquare;
  }
  if (value == "sawtooth") {
    return AudioWaveform::kSawtooth;
  }
  if (value == "triangle") {
    return AudioWaveform::kTriangle;
  }
  return AudioWaveform::kUnknown;
}

inline AudioRampMode AudioRampModeFromString(const std::string& value) {
  if (value == "up") {
    return AudioRampMode::kUp;
  }
  if (value == "down") {
    return AudioRampMode::kDown;
  }
  if (value == "bounce") {
    return AudioRampMode::kBounce;
  }
  return AudioRampMode::kUnknown;
}

// Thread-safety: AudioParameters is a plain data container with no mutable
// state. It may be read concurrently from multiple threads.
//
// Per-section synthetic test-tone description. Frequency is either a fixed
// tone (frequency_hz, used when !ramp_enabled) or a linear ramp between
// ramp_start_hz and ramp_end_hz. A ramp spans the whole section when
// ramp_period_seconds == 0, or repeats every ramp_period_seconds when > 0.
struct AudioParameters {
  bool enabled = false;
  AudioWaveform waveform = AudioWaveform::kSine;
  // Raw YAML waveform value retained for validation error messages.
  std::string waveform_text;

  // Fixed-frequency mode (used when !ramp_enabled).
  double frequency_hz = 1000.0;
  // True when the project file set `frequency`, so validation can reject it
  // alongside a ramp. Not compared by operator== (the emitter writes
  // `frequency` only for fixed-frequency tones).
  bool frequency_specified = false;

  // Ramp mode.
  bool ramp_enabled = false;
  double ramp_start_hz = 0.0;
  double ramp_end_hz = 0.0;
  bool ramp_start_specified = false;
  bool ramp_end_specified = false;
  AudioRampMode ramp_mode = AudioRampMode::kUp;
  // Raw YAML ramp-mode value retained for validation error messages.
  std::string ramp_mode_text;
  // 0.0 => ramp spans the whole section; > 0.0 => one ramp cycle lasts this
  // many seconds and repeats across the section duration.
  double ramp_period_seconds = 0.0;

  // Peak amplitude as a fraction of full scale [0.0, 1.0].
  double amplitude = 0.5;
};

// Maximum number of audio channel pairs permitted by the CVBS File Format
// Specification (Audio Data): up to 16 SMPTE 272M channels in 8 channel pairs.
inline constexpr int kMaxAudioChannelPairs = 8;

// What a channel pair carries for a section: synthetic test tones
// (`left`/`right`), samples read from an audio file, or the soundtrack of the
// section's video source.
enum class AudioPairContent { kTones, kFile, kVideo };

// Audio file feeding a channel pair (YAML `file:`). The file plays from its
// start at the beginning of each section run; at its end the rest of the run
// is silent, or the file restarts when `repeat` is true.
struct AudioFileInput {
  std::string path;
  bool repeat = false;
};

// Section video soundtrack feeding a channel pair (YAML `video_audio:`).
// `stream` is the 0-based index among the video file's audio streams.
struct VideoAudioInput {
  int stream = 0;
};

// Thread-safety: AudioChannelPair is a plain data container with no mutable
// state. It may be read concurrently from multiple threads.
//
// One stereo channel pair (SMPTE 272M §3.11) for a section. `pair` is the
// channel-pair number 0–7 that names the emitted `_audio_<pair>.wav` file and
// the `audio_channel_pair` metadata row. `content` selects what the pair
// carries. For tones, `left` and `right` describe the two interleaved channels
// independently; a channel with `enabled == false` is stored as all-zero
// silence per SMPTE 272M §6.4. `file` and `video_audio` are meaningful only for
// their own content kind. `description` is an optional human-readable label
// recorded in the metadata (may be empty → NULL).
struct AudioChannelPair {
  int pair = 0;
  bool pair_specified = false;
  std::string description;
  AudioPairContent content = AudioPairContent::kTones;
  AudioParameters left = {};
  AudioParameters right = {};
  AudioFileInput file = {};
  VideoAudioInput video_audio = {};
};

struct Section {
  struct LineInjectionCode {
    std::string code_type;
    int start_value = 0;
    bool start_value_specified = false;
    int chapter = 0;
    bool chapter_specified = false;
    std::string programme_status;
    bool programme_status_specified = false;
    std::string users_code;
    bool users_code_specified = false;
  };

  struct LineInjection {
    std::string type;
    std::vector<int> target_lines;
    std::vector<LineInjectionCode> codes;
  };

  std::string name = {};
  std::string type = {};
  SectionType section_type = SectionType::kUnknown;
  std::vector<LineInjection> line_injections = {};
  std::string source = {};
  // True when the section plays its selected source range (see
  // ResolveSourceFrameRange); YAML `duration_frames: all` or an omitted
  // duration_frames.
  bool duration_frames_all = false;
  int duration_frames = 0;
  // Number of times the selected range is replayed when duration_frames_all
  // is true (total output frames = range length * duration_frames_repeat).
  // Ignored when duration_frames_all is false. Always >= 1.
  int duration_frames_repeat = 1;
  // First source frame of the selected range, 0-based.
  int start_frame = 0;
  // Last source frame of the selected range, inclusive and 0-based. Only
  // meaningful when end_frame_specified; otherwise the range runs to the
  // source's last frame.
  int end_frame = 0;
  bool end_frame_specified = false;
  LumaBandwidthParameters luma_bandwidth = {};
  NoiseParameters noise = {};
  DropoutParameters dropouts = {};
  OsdConfig osd = {};
  std::vector<AudioChannelPair> audio_channel_pairs = {};
};

// LaserDisc digital audio (EFM) output selection. When `enabled`, the single
// audio channel pair named by `pair` is additionally encoded as an IEC
// 60908-1999 EFM channel stream (IEC 60856:1986 Amd 2 clause 13 for PAL, IEC
// 60857:1986 Amd 2 clause 13 for NTSC) and written alongside that pair's WAV
// file. The WAV output for the pair is unaffected.
struct EfmAudioOutput {
  bool enabled = false;
  // Channel-pair number 0–7, matching AudioChannelPair::pair.
  int pair = 0;
};

struct OutputTargets {
  std::string video_path;
  std::string metadata_path;
  // "composite" (default) or "yc" (dual-file luma+chroma).
  std::string signal_type = "composite";
  EfmAudioOutput efm_audio = {};
};

// One project-wide VITS (vertical interval test signal) injection: a single
// VITS type placed on one or more 1-based frame lines. The same set is applied
// to every frame of the project regardless of section.
struct VitsInjection {
  std::string vits_type;
  std::vector<int> target_lines;
};

// Project-wide line-injection configuration. `disc_type` selects the laserdisc
// format ("CAV"/"CLV", empty for a non-laserdisc project) shared by every
// section's laserdisc code injections. `vits` is the VITS test-signal set
// applied across the whole project. Both are project-wide decisions: the disc
// format and the VITS complement do not vary per section.
struct ProjectLineInjections {
  std::string disc_type;
  // Placement policy for the VITS set (see VitsPlacement). Defaults to
  // kStandard so projects that omit the field keep the strict broadcast-line
  // policy they were authored under.
  VitsPlacement placement = VitsPlacement::kStandard;
  std::vector<VitsInjection> vits;
};

// Readout edge smear of the channel: a fraction of the signal passes through
// a one-pole low-pass, H(z) = (1 - fraction) + fraction * (1 - a) /
// (1 - a z^-1) with a = exp(-1 / tau_samples).
struct EdgeSmearParameters {
  // Present when the channel block declares `edge_smear:`.
  bool enabled = false;
  // Smeared fraction of the signal. Range [0, 0.5].
  double fraction = 0.0;
  // Time constant of the smear in 4fsc samples. Range (0, 100].
  double tau_samples = 1.0;
};

// Default FM video carrier at black, in Hz: the reference model's bars fit
// on PAL LaserDisc pressings (deherring.py F0_HZ).
constexpr double kFmBeatCarrierBlackDefaultHz = 7.088e6;

// PAL FM-beat herringbone: the beat between the FM video carrier and the
// colour signal, recorded on the master (625-line PAL only).
struct FmBeatParameters {
  // Present when the channel block declares `fm_beat:`.
  bool enabled = false;
  // FM video carrier at black, in Hz. Range [6.9e6, 7.3e6].
  double carrier_black_hz = kFmBeatCarrierBlackDefaultHz;
  // Beat level relative to the local chroma amplitude, in dB, at the carrier
  // for 50 IRE. Range [-60, 0].
  double level_db = 0.0;
};

// Project-wide channel impairments: properties of the pressing rather than of
// one capture, so every project of one pressing uses the same settings. When
// `enabled` is false the channel stage is skipped entirely.
struct ChannelParameters {
  // Present when the project declares a `channel:` block.
  bool enabled = false;
  // Sub-sample timing offset in 4fsc samples. Positive values make the signal
  // arrive later. Range [-8.0, 8.0].
  double timing_offset_samples = 0.0;
  EdgeSmearParameters edge_smear;
  FmBeatParameters fm_beat;
};

// Per-line Gaussian jitter of the chroma error. Values are standard
// deviations.
struct ChromaJitterParameters {
  // Present when the chroma_error block declares `line_jitter:`.
  bool enabled = false;
  // Phase standard deviation in degrees. Range [0, 10].
  double phase_deg = 0.0;
  // Gain standard deviation (absolute, about the constant gain). Range
  // [0, 0.1].
  double gain = 0.0;
  // When seed_specified is false, a random base seed captured when the stage
  // is constructed is used instead, so each run draws different jitter.
  bool seed_specified = false;
  int64_t seed = 0;
};

// Project-wide chroma gain and phase error of one capture: the player's
// chroma path, applied to picture chroma only (burst, VITS and VBI lines are
// untouched).
struct ChromaErrorParameters {
  // Present when the project declares a `chroma_error:` block.
  bool enabled = false;
  // Chroma gain. Range [0.5, 1.5].
  double gain = 1.0;
  // Chroma phase advance in degrees. Range [-180, 180].
  double phase_deg = 0.0;
  ChromaJitterParameters line_jitter;
  // Apply the error to the colour burst as well as the picture chroma: a
  // whole chroma path (or an FM/MTF loss) rather than a mis-adjusted picture
  // chroma path that leaves the burst alone.
  bool include_burst = false;
};

struct Project {
  std::string name;
  std::string version;
  // Optional free-text description carried through load/save round-trips.
  std::string description;
  CvbsPresets cvbs_presets;
  OutputTargets output;
  ProjectLineInjections line_injections;
  ChannelParameters channel;
  ChromaErrorParameters chroma_error;
  std::vector<Section> sections;
};

// Equality operators for the project model. Comparison is exact (including
// floating-point fields); intended for parse/emit round-trip verification and
// document dirty tracking, not for tolerance-based signal comparison.
inline bool operator==(const BiphasePresets& /*a*/,
                       const BiphasePresets& /*b*/) {
  return true;
}

inline bool operator==(const CvbsPresets& a, const CvbsPresets& b) {
  return a.video_standard_preset == b.video_standard_preset &&
         a.sample_encoding_preset == b.sample_encoding_preset &&
         a.signal_state_preset == b.signal_state_preset &&
         a.pal_laserdisc_pilot_burst == b.pal_laserdisc_pilot_burst &&
         a.ntsc_laserdisc_vbi_burst == b.ntsc_laserdisc_vbi_burst &&
         a.ntsc_black_setup_ire == b.ntsc_black_setup_ire &&
         a.ntsc_black_setup_ire_specified == b.ntsc_black_setup_ire_specified &&
         a.secam_field_identification == b.secam_field_identification &&
         a.biphase_presets == b.biphase_presets;
}

inline bool operator==(const NoiseSpectrumPoint& a,
                       const NoiseSpectrumPoint& b) {
  return a.frequency_mhz == b.frequency_mhz && a.relative_db == b.relative_db;
}

inline bool operator==(const NoiseLevelPoint& a, const NoiseLevelPoint& b) {
  return a.level_mv == b.level_mv && a.relative_db == b.relative_db;
}

inline bool operator==(const SharedNoiseLevelTerm& a,
                       const SharedNoiseLevelTerm& b) {
  return a.enabled == b.enabled && a.noise_db == b.noise_db &&
         a.level_db == b.level_db && a.spectrum == b.spectrum;
}

inline bool operator==(const SharedNoiseParameters& a,
                       const SharedNoiseParameters& b) {
  return a.enabled == b.enabled && a.noise_db == b.noise_db &&
         a.noise_seed == b.noise_seed &&
         a.noise_seed_specified == b.noise_seed_specified &&
         a.spectrum == b.spectrum && a.level_term == b.level_term;
}

inline bool operator==(const NoiseParameters& a, const NoiseParameters& b) {
  return a.enabled == b.enabled && a.noise_db == b.noise_db &&
         a.noise_spread_db == b.noise_spread_db &&
         a.noise_seed == b.noise_seed &&
         a.noise_seed_specified == b.noise_seed_specified &&
         a.spectrum == b.spectrum && a.spread_spectrum == b.spread_spectrum &&
         a.shared == b.shared;
}

inline bool operator==(const RandomDropoutParameters& a,
                       const RandomDropoutParameters& b) {
  return a.enabled == b.enabled && a.scale == b.scale &&
         a.seed_specified == b.seed_specified && a.seed == b.seed &&
         a.flagged == b.flagged && a.anchor == b.anchor;
}

inline bool operator==(const ScratchDropoutParameters& a,
                       const ScratchDropoutParameters& b) {
  return a.enabled == b.enabled && a.scale == b.scale &&
         a.seed_specified == b.seed_specified && a.seed == b.seed &&
         a.flagged == b.flagged && a.anchor == b.anchor;
}

inline bool operator==(const DropoutParameters& a, const DropoutParameters& b) {
  return a.random == b.random && a.scratch == b.scratch;
}

inline bool operator==(const LumaBandwidthParameters& a,
                       const LumaBandwidthParameters& b) {
  return a.enabled == b.enabled && a.cutoff_mhz == b.cutoff_mhz &&
         a.transition_mhz == b.transition_mhz;
}

inline bool operator==(const OsdOverlay& a, const OsdOverlay& b) {
  return a.text == b.text && a.x == b.x && a.y == b.y && a.scale == b.scale &&
         a.fg_level == b.fg_level && a.bg_level == b.bg_level;
}

inline bool operator==(const OsdConfig& a, const OsdConfig& b) {
  return a.overlays == b.overlays;
}

inline bool operator==(const AudioParameters& a, const AudioParameters& b) {
  return a.enabled == b.enabled && a.waveform == b.waveform &&
         a.waveform_text == b.waveform_text &&
         a.frequency_hz == b.frequency_hz && a.ramp_enabled == b.ramp_enabled &&
         a.ramp_start_hz == b.ramp_start_hz && a.ramp_end_hz == b.ramp_end_hz &&
         a.ramp_start_specified == b.ramp_start_specified &&
         a.ramp_end_specified == b.ramp_end_specified &&
         a.ramp_mode == b.ramp_mode && a.ramp_mode_text == b.ramp_mode_text &&
         a.ramp_period_seconds == b.ramp_period_seconds &&
         a.amplitude == b.amplitude;
}

inline bool operator==(const AudioFileInput& a, const AudioFileInput& b) {
  return a.path == b.path && a.repeat == b.repeat;
}

inline bool operator==(const VideoAudioInput& a, const VideoAudioInput& b) {
  return a.stream == b.stream;
}

inline bool operator==(const AudioChannelPair& a, const AudioChannelPair& b) {
  return a.pair == b.pair && a.pair_specified == b.pair_specified &&
         a.description == b.description && a.content == b.content &&
         a.left == b.left && a.right == b.right && a.file == b.file &&
         a.video_audio == b.video_audio;
}

inline bool operator==(const Section::LineInjectionCode& a,
                       const Section::LineInjectionCode& b) {
  return a.code_type == b.code_type && a.start_value == b.start_value &&
         a.start_value_specified == b.start_value_specified &&
         a.chapter == b.chapter && a.chapter_specified == b.chapter_specified &&
         a.programme_status == b.programme_status &&
         a.programme_status_specified == b.programme_status_specified &&
         a.users_code == b.users_code &&
         a.users_code_specified == b.users_code_specified;
}

inline bool operator==(const Section::LineInjection& a,
                       const Section::LineInjection& b) {
  return a.type == b.type && a.target_lines == b.target_lines &&
         a.codes == b.codes;
}

inline bool operator==(const VitsInjection& a, const VitsInjection& b) {
  return a.vits_type == b.vits_type && a.target_lines == b.target_lines;
}

inline bool operator==(const ProjectLineInjections& a,
                       const ProjectLineInjections& b) {
  return a.disc_type == b.disc_type && a.placement == b.placement &&
         a.vits == b.vits;
}

inline bool operator==(const Section& a, const Section& b) {
  return a.name == b.name && a.type == b.type &&
         a.section_type == b.section_type &&
         a.line_injections == b.line_injections && a.source == b.source &&
         a.duration_frames_all == b.duration_frames_all &&
         a.duration_frames == b.duration_frames &&
         a.duration_frames_repeat == b.duration_frames_repeat &&
         a.start_frame == b.start_frame && a.end_frame == b.end_frame &&
         a.end_frame_specified == b.end_frame_specified &&
         a.luma_bandwidth == b.luma_bandwidth && a.noise == b.noise &&
         a.dropouts == b.dropouts && a.osd == b.osd &&
         a.audio_channel_pairs == b.audio_channel_pairs;
}

inline bool operator==(const EfmAudioOutput& a, const EfmAudioOutput& b) {
  return a.enabled == b.enabled && a.pair == b.pair;
}

inline bool operator==(const OutputTargets& a, const OutputTargets& b) {
  return a.video_path == b.video_path && a.metadata_path == b.metadata_path &&
         a.signal_type == b.signal_type && a.efm_audio == b.efm_audio;
}

inline bool operator==(const EdgeSmearParameters& a,
                       const EdgeSmearParameters& b) {
  return a.enabled == b.enabled && a.fraction == b.fraction &&
         a.tau_samples == b.tau_samples;
}

inline bool operator==(const FmBeatParameters& a, const FmBeatParameters& b) {
  return a.enabled == b.enabled && a.carrier_black_hz == b.carrier_black_hz &&
         a.level_db == b.level_db;
}

inline bool operator==(const ChannelParameters& a, const ChannelParameters& b) {
  return a.enabled == b.enabled &&
         a.timing_offset_samples == b.timing_offset_samples &&
         a.edge_smear == b.edge_smear && a.fm_beat == b.fm_beat;
}

inline bool operator==(const ChromaJitterParameters& a,
                       const ChromaJitterParameters& b) {
  return a.enabled == b.enabled && a.phase_deg == b.phase_deg &&
         a.gain == b.gain && a.seed_specified == b.seed_specified &&
         a.seed == b.seed;
}

inline bool operator==(const ChromaErrorParameters& a,
                       const ChromaErrorParameters& b) {
  return a.enabled == b.enabled && a.gain == b.gain &&
         a.phase_deg == b.phase_deg && a.line_jitter == b.line_jitter &&
         a.include_burst == b.include_burst;
}

inline bool operator==(const Project& a, const Project& b) {
  return a.name == b.name && a.version == b.version &&
         a.description == b.description && a.cvbs_presets == b.cvbs_presets &&
         a.output == b.output && a.line_injections == b.line_injections &&
         a.channel == b.channel && a.chroma_error == b.chroma_error &&
         a.sections == b.sections;
}

inline bool operator!=(const Project& a, const Project& b) { return !(a == b); }

// True if a channel pair carries any active (non-silent) channel. File and
// video content always count as active.
inline bool AudioChannelPairIsActive(const AudioChannelPair& pair) {
  if (pair.content != AudioPairContent::kTones) {
    return true;
  }
  return pair.left.enabled || pair.right.enabled;
}

// True if any section declares an audio channel pair. When false the pipeline
// emits no WAV tracks and no audio_channel_pair metadata rows.
inline bool ProjectEnablesAudio(const Project& project) {
  for (const Section& section : project.sections) {
    if (!section.audio_channel_pairs.empty()) {
      return true;
    }
  }
  return false;
}

// Sorted, de-duplicated list of channel-pair numbers declared across all
// sections. One WAV file and one audio_channel_pair metadata row is emitted per
// entry; every file spans the whole output (silent where a section omits it).
inline std::vector<int> ProjectAudioChannelPairs(const Project& project) {
  std::vector<int> pairs;
  for (const Section& section : project.sections) {
    for (const AudioChannelPair& cp : section.audio_channel_pairs) {
      if (std::find(pairs.begin(), pairs.end(), cp.pair) == pairs.end()) {
        pairs.push_back(cp.pair);
      }
    }
  }
  std::sort(pairs.begin(), pairs.end());
  return pairs;
}

// True for the standards that carry LaserDisc digital audio (EFM).
// IEC 60856:1986 Amd 2 clause 13 defines it for 625-line PAL and
// IEC 60857:1986 Amd 2 clause 13 for 525-line NTSC. PAL-M discs use the
// 525-line System M LaserVision structure, as the VBI codes do, so they take
// the IEC 60857 definition: the frame rate, not the colour system, fixes the
// EFM timing. SECAM has none.
inline bool StandardHasLaserdiscDigitalAudio(Standard standard) {
  return standard == Standard::kPal || standard == Standard::kNtsc ||
         standard == Standard::kPalM;
}

// Channel pair carrying LaserDisc digital audio (EFM) output, or -1 when the
// project emits no EFM track. Output is emitted only when the selection is
// enabled, the pair is declared by at least one section, and the video standard
// has a LaserDisc digital audio specification
// (StandardHasLaserdiscDigitalAudio). The project validator reports the
// rejected combinations.
inline int ProjectEfmAudioPair(const Project& project) {
  if (!project.output.efm_audio.enabled) {
    return -1;
  }
  if (!StandardHasLaserdiscDigitalAudio(
          project.cvbs_presets.video_standard_preset)) {
    return -1;
  }
  const std::vector<int> pairs = ProjectAudioChannelPairs(project);
  if (std::find(pairs.begin(), pairs.end(), project.output.efm_audio.pair) ==
      pairs.end()) {
    return -1;
  }
  return project.output.efm_audio.pair;
}

// First non-empty description recorded for `pair` in section order, or an empty
// string when none is set (→ NULL in the metadata).
inline std::string AudioChannelPairDescription(const Project& project,
                                               int pair) {
  for (const Section& section : project.sections) {
    for (const AudioChannelPair& cp : section.audio_channel_pairs) {
      if (cp.pair == pair && !cp.description.empty()) {
        return cp.description;
      }
    }
  }
  return {};
}

}  // namespace videosynth
