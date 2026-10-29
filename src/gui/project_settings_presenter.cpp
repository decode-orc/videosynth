/*
 * File:        project_settings_presenter.cpp
 * Module:      gui
 * Purpose:     Widget-free mapping layer for the project settings editor
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "project_settings_presenter.h"

#include <utility>

namespace videosynth::gui {

namespace {

bool EndsWith(const std::string& value, const std::string& suffix) {
  if (suffix.size() > value.size()) {
    return false;
  }
  return value.compare(value.size() - suffix.size(), suffix.size(), suffix) ==
         0;
}

// Strips a known output suffix (".cvbs" or ".cvbsy"), else any final
// extension within the last path component, returning the path stem.
std::string StripOutputSuffix(const std::string& path) {
  if (EndsWith(path, ".cvbs")) {
    return path.substr(0, path.size() - std::string(".cvbs").size());
  }
  if (EndsWith(path, ".cvbsy")) {
    return path.substr(0, path.size() - std::string(".cvbsy").size());
  }

  const std::size_t last_separator = path.find_last_of('/');
  const std::size_t last_dot = path.find_last_of('.');
  const bool dot_in_last_component =
      last_dot != std::string::npos &&
      (last_separator == std::string::npos || last_dot > last_separator);
  if (dot_in_last_component) {
    return path.substr(0, last_dot);
  }
  return path;
}

// True for the standards with a LaserDisc digital audio specification:
// IEC 60856:1986 Amd 2 clause 13 (PAL) and IEC 60857:1986 Amd 2 clause 13
// (NTSC). No other standard defines an EFM sample frequency.
bool StandardHasLaserdiscDigitalAudio(Standard standard) {
  return standard == Standard::kPal || standard == Standard::kNtsc;
}

// Classifies the EFM selection exactly as ProjectEfmAudioPair decides whether
// a stream is emitted, so the form reports the same outcome the pipeline
// produces.
EfmOutputStatus ClassifyEfmOutput(const Project& project) {
  if (!project.output.efm_audio.enabled) {
    return EfmOutputStatus::kDisabled;
  }
  if (!StandardHasLaserdiscDigitalAudio(
          project.cvbs_presets.video_standard_preset)) {
    return EfmOutputStatus::kUnsupportedStandard;
  }
  const int pair = project.output.efm_audio.pair;
  if (pair < 0 || pair >= kMaxAudioChannelPairs) {
    return EfmOutputStatus::kPairOutOfRange;
  }
  if (ProjectEfmAudioPair(project) < 0) {
    return EfmOutputStatus::kPairNotDeclared;
  }
  return EfmOutputStatus::kActive;
}

// Freshly-enabled channel and chroma impairments: the values of the PAL
// pressing C example, a visible but plausible pressing.
constexpr double kDefaultEdgeSmearFraction = 0.12;
constexpr double kDefaultEdgeSmearTauSamples = 3.0;
constexpr double kDefaultFmBeatLevelDb = -24.0;
constexpr double kDefaultJitterPhaseDeg = 0.5;
constexpr double kDefaultJitterGain = 0.01;

}  // namespace

ProjectSettingsFormState BuildProjectSettingsFormState(const Project& project) {
  ProjectSettingsFormState state;

  state.standard_options = {"PAL", "NTSC", "PAL_M", "SECAM"};
  // The supported preset set from model.h (IsSupportedSampleEncodingPreset).
  state.sample_encoding_options = {"CVBS_U10_4FSC",   "CVBS_U16_4FSC",
                                   "CVBS_TPG21_4FSC", "CVBS_S16_4FSC",
                                   "RAW_S16_28M",     "RAW_S16_40M"};
  const Standard standard = project.cvbs_presets.video_standard_preset;
  // ProjectValidator only accepts the state generated output declares:
  // time-base stable and phase locked, or unlocked for SECAM.
  state.signal_state_options = {GeneratedSignalStatePreset(standard)};
  state.signal_type_options = {"composite", "yc"};
  // ProjectValidator: ntsc_black_setup_ire must be 7.5 or 0.0.
  state.ntsc_black_setup_ire_options = {7.5, 0.0};

  state.pilot_burst_editable = standard == Standard::kPal;
  state.secam_field_identification_editable = standard == Standard::kSecam;
  state.laserdisc_editable = standard != Standard::kSecam;
  state.fm_beat_editable = standard == Standard::kPal;
  state.chroma_error_editable = standard != Standard::kSecam;
  state.vbi_burst_editable = standard == Standard::kNtsc;
  state.setup_ire_editable =
      standard == Standard::kNtsc || standard == Standard::kPalM;
  state.video_path_requires_luma_suffix = project.output.signal_type == "yc";

  state.efm_pair_options.reserve(kMaxAudioChannelPairs);
  for (int pair = 0; pair < kMaxAudioChannelPairs; ++pair) {
    state.efm_pair_options.push_back(pair);
  }
  state.efm_output_editable = StandardHasLaserdiscDigitalAudio(standard);
  state.efm_pair_editable =
      state.efm_output_editable && project.output.efm_audio.enabled;
  state.efm_status = ClassifyEfmOutput(project);

  return state;
}

CvbsPresets NormalizeCvbsPresetsForStandard(CvbsPresets presets) {
  if (presets.video_standard_preset != Standard::kPal) {
    presets.pal_laserdisc_pilot_burst = false;
  }
  if (presets.video_standard_preset != Standard::kSecam) {
    presets.secam_field_identification = true;
  }
  if (presets.video_standard_preset != Standard::kUnknown) {
    presets.signal_state_preset =
        GeneratedSignalStatePreset(presets.video_standard_preset);
  }
  if (presets.video_standard_preset != Standard::kNtsc) {
    presets.ntsc_laserdisc_vbi_burst = false;
  }
  if (presets.video_standard_preset != Standard::kNtsc &&
      presets.video_standard_preset != Standard::kPalM) {
    presets.ntsc_black_setup_ire_specified = false;
    presets.ntsc_black_setup_ire = 7.5;
  }
  return presets;
}

OutputTargets NormalizeOutputTargetsForStandard(OutputTargets output,
                                                Standard standard) {
  if (!StandardHasLaserdiscDigitalAudio(standard)) {
    output.efm_audio = EfmAudioOutput{};
  }
  return output;
}

ChannelParameters NormalizeChannelForStandard(ChannelParameters channel,
                                              Standard standard) {
  if (standard != Standard::kPal) {
    channel.fm_beat = FmBeatParameters{};
  }
  return channel;
}

ChromaErrorParameters NormalizeChromaErrorForStandard(
    ChromaErrorParameters chroma_error, Standard standard) {
  if (standard == Standard::kSecam) {
    return ChromaErrorParameters{};
  }
  return chroma_error;
}

void SetChannelEnabled(ChannelParameters* channel, bool enabled) {
  if (!enabled) {
    *channel = ChannelParameters{};
    return;
  }
  channel->enabled = true;
}

void SetEdgeSmearEnabled(ChannelParameters* channel, bool enabled) {
  if (!enabled) {
    channel->edge_smear = EdgeSmearParameters{};
    return;
  }
  channel->enabled = true;
  if (channel->edge_smear.enabled) {
    return;
  }
  channel->edge_smear.enabled = true;
  channel->edge_smear.fraction = kDefaultEdgeSmearFraction;
  channel->edge_smear.tau_samples = kDefaultEdgeSmearTauSamples;
}

void SetFmBeatEnabled(ChannelParameters* channel, bool enabled) {
  if (!enabled) {
    channel->fm_beat = FmBeatParameters{};
    return;
  }
  channel->enabled = true;
  if (channel->fm_beat.enabled) {
    return;
  }
  channel->fm_beat = FmBeatParameters{};
  channel->fm_beat.enabled = true;
  channel->fm_beat.level_db = kDefaultFmBeatLevelDb;
}

void SetChromaErrorEnabled(ChromaErrorParameters* chroma_error, bool enabled) {
  if (!enabled) {
    *chroma_error = ChromaErrorParameters{};
    return;
  }
  chroma_error->enabled = true;
}

void SetChromaJitterEnabled(ChromaErrorParameters* chroma_error, bool enabled) {
  if (!enabled) {
    chroma_error->line_jitter = ChromaJitterParameters{};
    return;
  }
  chroma_error->enabled = true;
  if (chroma_error->line_jitter.enabled) {
    return;
  }
  chroma_error->line_jitter.enabled = true;
  chroma_error->line_jitter.phase_deg = kDefaultJitterPhaseDeg;
  chroma_error->line_jitter.gain = kDefaultJitterGain;
}

std::string DeriveMetadataPath(const std::string& video_path) {
  if (video_path.empty()) {
    return {};
  }
  return StripOutputSuffix(video_path) + ".meta";
}

std::string EnforceSignalTypeVideoPath(std::string video_path,
                                       const std::string& signal_type) {
  if (video_path.empty()) {
    return video_path;
  }

  if (signal_type == "yc") {
    if (EndsWith(video_path, ".cvbsy")) {
      return video_path;
    }
    return StripOutputSuffix(video_path) + ".cvbsy";
  }

  if (signal_type == "composite" && EndsWith(video_path, ".cvbsy")) {
    return StripOutputSuffix(std::move(video_path)) + ".cvbs";
  }
  return video_path;
}

}  // namespace videosynth::gui
