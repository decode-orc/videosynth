/*
 * File:        project_settings_presenter.cpp
 * Module:      gui
 * Purpose:     Widget-free mapping layer for the project settings editor
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "project_settings_presenter.h"

#include <algorithm>
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

// Classifies the EFM selection exactly as ProjectEfmAudioPair decides whether
// a stream is emitted, so the form reports the same outcome the pipeline
// produces.
EfmOutputStatus ClassifyEfmOutput(const Project& project) {
  if (!project.laserdisc.efm_audio.enabled) {
    return EfmOutputStatus::kDisabled;
  }
  if (!StandardHasLaserdiscDigitalAudio(
          project.cvbs_presets.video_standard_preset)) {
    return EfmOutputStatus::kUnsupportedStandard;
  }
  const int pair = project.laserdisc.efm_audio.pair;
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
// Freshly-enabled differential gain and phase: a few percent of gain droop
// and a few degrees of phase shift between blanking and white.
constexpr double kDefaultDifferentialWhiteGain = 0.97;
constexpr double kDefaultDifferentialWhitePhaseDeg = 3.0;
constexpr double kWhiteLevelIre = 100.0;
// Freshly-enabled crosstalk: a small symmetric U/V leak.
constexpr double kDefaultCrosstalk = 0.02;
// Freshly-enabled field-2 offset: half a sample to the right.
constexpr double kDefaultField2OffsetSamples = 0.5;
// Pictures between a new response_by_picture entry and the one before it.
constexpr int kResponsePictureStep = 10000;

// A gentle replay roll-off: flat to 3 MHz, a few dB down by 5 MHz.
std::vector<NoiseSpectrumPoint> DefaultReplayResponse() {
  return {{0.5, 0.0}, {3.0, -1.0}, {5.0, -3.0}};
}

ReplayResponseMode ResponseModeOfChannel(
    const LaserdiscChannelParameters& channel) {
  if (!channel.response_by_picture.empty()) {
    return ReplayResponseMode::kByPicture;
  }
  if (!channel.response_db.empty()) {
    return ReplayResponseMode::kFixed;
  }
  return ReplayResponseMode::kNone;
}

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
  state.group_delay_nominal_editable = standard == Standard::kPal;
  state.crosstalk_alternate_editable =
      standard == Standard::kPal || standard == Standard::kPalM;
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
  state.truth_editable = standard != Standard::kSecam;
  state.efm_pair_editable =
      state.efm_output_editable && project.laserdisc.efm_audio.enabled;
  state.efm_status = ClassifyEfmOutput(project);

  return state;
}

CvbsPresets NormalizeCvbsPresetsForStandard(CvbsPresets presets) {
  if (presets.video_standard_preset != Standard::kSecam) {
    presets.secam_field_identification = true;
  }
  if (presets.video_standard_preset != Standard::kUnknown) {
    presets.signal_state_preset =
        GeneratedSignalStatePreset(presets.video_standard_preset);
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
  if (standard == Standard::kSecam) {
    output.truth = false;
  }
  return output;
}

LaserdiscParameters NormalizeLaserdiscForStandard(LaserdiscParameters laserdisc,
                                                  Standard standard) {
  if (standard == Standard::kSecam) {
    return LaserdiscParameters{};
  }
  if (standard != Standard::kPal) {
    laserdisc.pilot_burst = false;
    laserdisc.channel.fm_beat = FmBeatParameters{};
    if (laserdisc.channel.group_delay.enabled &&
        laserdisc.channel.group_delay.preset ==
            GroupDelayPreset::kIec60856Nominal) {
      SetGroupDelayPreset(&laserdisc, GroupDelayPreset::kCustom);
    }
  }
  if (standard != Standard::kNtsc) {
    laserdisc.vbi_burst = false;
  }
  if (!StandardHasLaserdiscDigitalAudio(standard)) {
    laserdisc.efm_audio = EfmAudioOutput{};
  }
  return laserdisc;
}

ChromaErrorParameters NormalizeChromaErrorForStandard(
    ChromaErrorParameters chroma_error, Standard standard) {
  if (standard == Standard::kSecam) {
    return ChromaErrorParameters{};
  }
  if (standard != Standard::kPal && standard != Standard::kPalM) {
    chroma_error.crosstalk.alternate_with_switch = false;
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

void SetDifferentialEnabled(ChromaErrorParameters* chroma_error, bool enabled) {
  if (!enabled) {
    chroma_error->differential = DifferentialParameters{};
    return;
  }
  chroma_error->enabled = true;
  DifferentialParameters& differential = chroma_error->differential;
  if (differential.enabled) {
    return;
  }
  differential.enabled = true;
  differential.gain = {{0.0, 1.0},
                       {kWhiteLevelIre, kDefaultDifferentialWhiteGain}};
  differential.phase_deg = {
      {0.0, 0.0}, {kWhiteLevelIre, kDefaultDifferentialWhitePhaseDeg}};
}

void SetCrosstalkEnabled(ChromaErrorParameters* chroma_error, bool enabled) {
  if (!enabled) {
    chroma_error->crosstalk = CrosstalkParameters{};
    return;
  }
  chroma_error->enabled = true;
  CrosstalkParameters& crosstalk = chroma_error->crosstalk;
  if (crosstalk.enabled) {
    return;
  }
  crosstalk.enabled = true;
  crosstalk.v_to_u = kDefaultCrosstalk;
  crosstalk.u_to_v = kDefaultCrosstalk;
}

void SetLaserdiscEnabled(LaserdiscParameters* laserdisc, bool enabled) {
  if (!enabled) {
    *laserdisc = LaserdiscParameters{};
    return;
  }
  laserdisc->enabled = true;
}

void SetLaserdiscChannelEnabled(LaserdiscParameters* laserdisc, bool enabled) {
  if (!enabled) {
    laserdisc->channel = LaserdiscChannelParameters{};
    return;
  }
  laserdisc->enabled = true;
  laserdisc->channel.enabled = true;
}

void SetFmBeatEnabled(LaserdiscParameters* laserdisc, bool enabled) {
  FmBeatParameters& fm_beat = laserdisc->channel.fm_beat;
  if (!enabled) {
    fm_beat = FmBeatParameters{};
    return;
  }
  SetLaserdiscChannelEnabled(laserdisc, true);
  if (fm_beat.enabled) {
    return;
  }
  fm_beat = FmBeatParameters{};
  fm_beat.enabled = true;
  fm_beat.level_db = kDefaultFmBeatLevelDb;
}

void SetGroupDelayEnabled(LaserdiscParameters* laserdisc, bool enabled,
                          Standard standard) {
  GroupDelayParameters& group_delay = laserdisc->channel.group_delay;
  if (!enabled) {
    group_delay = GroupDelayParameters{};
    return;
  }
  SetLaserdiscChannelEnabled(laserdisc, true);
  if (group_delay.enabled) {
    return;
  }
  group_delay = GroupDelayParameters{};
  group_delay.enabled = true;
  if (standard == Standard::kPal) {
    group_delay.preset = GroupDelayPreset::kIec60856Nominal;
  } else {
    group_delay.preset = GroupDelayPreset::kCustom;
    group_delay.points_ns = Iec60856NominalGroupDelay();
  }
}

void SetGroupDelayPreset(LaserdiscParameters* laserdisc,
                         GroupDelayPreset preset) {
  GroupDelayParameters& group_delay = laserdisc->channel.group_delay;
  if (preset == GroupDelayPreset::kCustom) {
    if (group_delay.points_ns.empty()) {
      group_delay.points_ns = Iec60856NominalGroupDelay();
    }
  } else {
    // The preset carries its own curve; a stored custom one would not emit.
    group_delay.points_ns.clear();
  }
  group_delay.preset = preset;
}

void SetField2OffsetEnabled(LaserdiscParameters* laserdisc, bool enabled) {
  Field2OffsetParameters& offset = laserdisc->channel.field2_offset;
  if (!enabled) {
    offset = Field2OffsetParameters{};
    return;
  }
  SetLaserdiscChannelEnabled(laserdisc, true);
  if (offset.enabled) {
    return;
  }
  offset.enabled = true;
  offset.x_samples = kDefaultField2OffsetSamples;
  offset.y_lines = 0.0;
}

ReplayResponseMode ReplayResponseModeOf(
    const LaserdiscChannelParameters& channel) {
  return ResponseModeOfChannel(channel);
}

void SetReplayResponseMode(LaserdiscParameters* laserdisc,
                           ReplayResponseMode mode) {
  LaserdiscChannelParameters& channel = laserdisc->channel;
  switch (mode) {
    case ReplayResponseMode::kNone:
      channel.response_db.clear();
      channel.response_by_picture.clear();
      return;
    case ReplayResponseMode::kFixed:
      SetLaserdiscChannelEnabled(laserdisc, true);
      if (channel.response_db.empty()) {
        channel.response_db =
            channel.response_by_picture.empty()
                ? DefaultReplayResponse()
                : channel.response_by_picture.front().response_db;
      }
      channel.response_by_picture.clear();
      return;
    case ReplayResponseMode::kByPicture:
      SetLaserdiscChannelEnabled(laserdisc, true);
      if (channel.response_by_picture.empty()) {
        const std::vector<NoiseSpectrumPoint> seed =
            channel.response_db.empty() ? DefaultReplayResponse()
                                        : channel.response_db;
        channel.response_by_picture = {
            {project_editor_limits::kResponsePictureMin, seed},
            {project_editor_limits::kResponsePictureMin + kResponsePictureStep,
             seed}};
      }
      channel.response_db.clear();
      return;
  }
}

void AddResponseByPictureEntry(std::vector<ResponseByPictureEntry>* entries) {
  if (entries->empty()) {
    entries->push_back(
        {project_editor_limits::kResponsePictureMin, DefaultReplayResponse()});
    return;
  }
  ResponseByPictureEntry next = entries->back();
  if (next.picture >
      project_editor_limits::kResponsePictureMax - kResponsePictureStep) {
    if (next.picture >= project_editor_limits::kResponsePictureMax) {
      return;
    }
    next.picture = project_editor_limits::kResponsePictureMax;
  } else {
    next.picture += kResponsePictureStep;
  }
  entries->push_back(std::move(next));
}

void RemoveResponseByPictureEntry(std::vector<ResponseByPictureEntry>* entries,
                                  int row) {
  if (row < 0 || row >= static_cast<int>(entries->size())) {
    return;
  }
  entries->erase(entries->begin() + row);
}

void AddCurvePoint(std::vector<CurvePoint>* points, double x_step, double x_max,
                   CurvePoint first) {
  if (points->empty()) {
    points->push_back(first);
    return;
  }
  const CurvePoint& last = points->back();
  if (last.x >= x_max) {
    return;
  }
  CurvePoint next = last;
  next.x = std::min(last.x + x_step, (last.x + x_max) / 2.0);
  points->push_back(next);
}

void RemoveCurvePoint(std::vector<CurvePoint>* points, int row) {
  if (row < 0 || row >= static_cast<int>(points->size())) {
    return;
  }
  points->erase(points->begin() + row);
}

std::vector<CurvePoint> ToCurvePoints(
    const std::vector<NoiseSpectrumPoint>& points) {
  std::vector<CurvePoint> curve;
  curve.reserve(points.size());
  for (const NoiseSpectrumPoint& point : points) {
    curve.push_back({point.frequency_mhz, point.relative_db});
  }
  return curve;
}

std::vector<CurvePoint> ToCurvePoints(
    const std::vector<GroupDelayPoint>& points) {
  std::vector<CurvePoint> curve;
  curve.reserve(points.size());
  for (const GroupDelayPoint& point : points) {
    curve.push_back({point.frequency_mhz, point.delay_ns});
  }
  return curve;
}

std::vector<CurvePoint> ToCurvePoints(
    const std::vector<LevelCurvePoint>& points) {
  std::vector<CurvePoint> curve;
  curve.reserve(points.size());
  for (const LevelCurvePoint& point : points) {
    curve.push_back({point.level_ire, point.value});
  }
  return curve;
}

std::vector<NoiseSpectrumPoint> ToResponsePoints(
    const std::vector<CurvePoint>& points) {
  std::vector<NoiseSpectrumPoint> response;
  response.reserve(points.size());
  for (const CurvePoint& point : points) {
    NoiseSpectrumPoint converted;
    converted.frequency_mhz = point.x;
    converted.relative_db = point.y;
    response.push_back(converted);
  }
  return response;
}

std::vector<GroupDelayPoint> ToGroupDelayPoints(
    const std::vector<CurvePoint>& points) {
  std::vector<GroupDelayPoint> group_delay;
  group_delay.reserve(points.size());
  for (const CurvePoint& point : points) {
    group_delay.push_back({point.x, point.y});
  }
  return group_delay;
}

std::vector<LevelCurvePoint> ToLevelCurvePoints(
    const std::vector<CurvePoint>& points) {
  std::vector<LevelCurvePoint> curve;
  curve.reserve(points.size());
  for (const CurvePoint& point : points) {
    curve.push_back({point.x, point.y});
  }
  return curve;
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
