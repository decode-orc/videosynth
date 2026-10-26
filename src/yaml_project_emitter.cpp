/*
 * File:        yaml_project_emitter.cpp
 * Module:      yaml_project_emitter
 * Purpose:     Serialises project models back to YAML project files.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "videosynth/yaml_project_emitter.h"

#include <yaml-cpp/yaml.h>

#include <fstream>
#include <limits>

#include "videosynth/biphase_types.h"

namespace videosynth {

namespace {

// Defaults mirrored from the parser (yaml_project_parser.cpp) and model.h.
// A field equal to its parse-time default is omitted so emitted files stay
// minimal; re-parsing restores the identical value.
constexpr int kDefaultOverlayScale = 1;
constexpr OsdFgLevel kDefaultOverlayFgLevel = OsdFgLevel::kWhite;
constexpr OsdBgLevel kDefaultOverlayBgLevel = OsdBgLevel::kTransparent;
constexpr double kDefaultAudioFrequencyHz = 1000.0;
constexpr double kDefaultAudioAmplitude = 0.5;

void EmitProjectBlock(YAML::Emitter& out, const Project& project) {
  if (project.name.empty() && project.version.empty() &&
      project.description.empty()) {
    return;
  }

  out << YAML::Key << "project" << YAML::Value << YAML::BeginMap;
  if (!project.name.empty()) {
    out << YAML::Key << "name" << YAML::Value << project.name;
  }
  if (!project.version.empty()) {
    out << YAML::Key << "version" << YAML::Value << project.version;
  }
  if (!project.description.empty()) {
    out << YAML::Key << "description" << YAML::Value << project.description;
  }
  out << YAML::EndMap;
}

void EmitCvbsPresets(YAML::Emitter& out, const CvbsPresets& presets) {
  out << YAML::Key << "cvbs_presets" << YAML::Value << YAML::BeginMap;
  out << YAML::Key << "video_standard_preset" << YAML::Value
      << StandardToString(presets.video_standard_preset);
  out << YAML::Key << "sample_encoding_preset" << YAML::Value
      << presets.sample_encoding_preset;
  out << YAML::Key << "signal_state_preset" << YAML::Value
      << presets.signal_state_preset;
  if (presets.pal_laserdisc_pilot_burst) {
    out << YAML::Key << "pal_laserdisc_pilot_burst" << YAML::Value << true;
  }
  if (presets.ntsc_laserdisc_vbi_burst) {
    out << YAML::Key << "ntsc_laserdisc_vbi_burst" << YAML::Value << true;
  }
  if (presets.ntsc_black_setup_ire_specified) {
    out << YAML::Key << "ntsc_black_setup_ire" << YAML::Value
        << presets.ntsc_black_setup_ire;
  }
  if (!presets.secam_field_identification) {
    out << YAML::Key << "secam_field_identification" << YAML::Value << false;
  }
  out << YAML::EndMap;
}

// Emits the project-wide `line_injections:` block (laserdisc disc_type and the
// VITS test-signal set). Omitted entirely when neither is configured.
void EmitProjectLineInjections(YAML::Emitter& out,
                               const ProjectLineInjections& line_injections) {
  if (line_injections.disc_type.empty() && line_injections.vits.empty() &&
      line_injections.placement == VitsPlacement::kStandard) {
    return;
  }

  out << YAML::Key << "line_injections" << YAML::Value << YAML::BeginMap;
  if (!line_injections.disc_type.empty()) {
    out << YAML::Key << "disc_type" << YAML::Value << line_injections.disc_type;
  }
  // Emitted only when non-default so standard-placement projects keep their
  // existing serialisation.
  if (line_injections.placement != VitsPlacement::kStandard) {
    out << YAML::Key << "placement" << YAML::Value
        << VitsPlacementToString(line_injections.placement);
  }
  if (!line_injections.vits.empty()) {
    out << YAML::Key << "vits" << YAML::Value << YAML::BeginSeq;
    for (const VitsInjection& vits : line_injections.vits) {
      out << YAML::BeginMap;
      out << YAML::Key << "vits_type" << YAML::Value << vits.vits_type;
      if (!vits.target_lines.empty()) {
        out << YAML::Key << "target_lines" << YAML::Value << YAML::Flow
            << vits.target_lines;
      }
      out << YAML::EndMap;
    }
    out << YAML::EndSeq;
  }
  out << YAML::EndMap;
}

void EmitOutputTargets(YAML::Emitter& out, const OutputTargets& output) {
  out << YAML::Key << "output" << YAML::Value << YAML::BeginMap;
  out << YAML::Key << "video_path" << YAML::Value << output.video_path;
  // metadata_path is not emitted: the metadata sidecar is always colocated with
  // the video output and derived from it on load.
  if (output.signal_type != "composite") {
    out << YAML::Key << "signal_type" << YAML::Value << output.signal_type;
  }
  if (output.efm_audio.enabled) {
    out << YAML::Key << "efm_audio" << YAML::Value << YAML::BeginMap;
    out << YAML::Key << "pair" << YAML::Value << output.efm_audio.pair;
    out << YAML::EndMap;
  }
  out << YAML::EndMap;
}

void EmitLineInjectionCode(YAML::Emitter& out,
                           const Section::LineInjectionCode& code) {
  out << YAML::BeginMap;
  out << YAML::Key << "code_type" << YAML::Value << code.code_type;
  if (code.start_value_specified) {
    out << YAML::Key << "start_value" << YAML::Value << code.start_value;
  }
  if (code.chapter_specified) {
    out << YAML::Key << "chapter" << YAML::Value << code.chapter;
  }
  if (code.programme_status_specified) {
    out << YAML::Key << "programme_status" << YAML::Value
        << code.programme_status;
  }
  if (code.users_code_specified) {
    out << YAML::Key << "users_code" << YAML::Value << code.users_code;
  }
  out << YAML::EndMap;
}

void EmitLineInjections(YAML::Emitter& out, const Section& section) {
  if (section.line_injections.empty()) {
    return;
  }

  out << YAML::Key << "line_injections" << YAML::Value << YAML::BeginSeq;
  for (const Section::LineInjection& injection : section.line_injections) {
    out << YAML::BeginMap;
    out << YAML::Key << "type" << YAML::Value << injection.type;
    if (!injection.target_lines.empty()) {
      out << YAML::Key << "target_lines" << YAML::Value << YAML::Flow
          << injection.target_lines;
    }
    if (!injection.codes.empty()) {
      out << YAML::Key << "codes" << YAML::Value << YAML::BeginSeq;
      for (const Section::LineInjectionCode& code : injection.codes) {
        EmitLineInjectionCode(out, code);
      }
      out << YAML::EndSeq;
    }
    out << YAML::EndMap;
  }
  out << YAML::EndSeq;
}

void EmitNoise(YAML::Emitter& out, const NoiseParameters& noise) {
  // The block exists when noise is enabled (noise_db was given) or a bare
  // seed was specified; both states must survive the round-trip.
  if (!noise.enabled && !noise.noise_seed_specified) {
    return;
  }

  out << YAML::Key << "noise" << YAML::Value << YAML::BeginMap;
  if (noise.enabled) {
    out << YAML::Key << "noise_db" << YAML::Value << noise.noise_db;
    if (noise.noise_spread_db != 0.0) {
      out << YAML::Key << "noise_spread_db" << YAML::Value
          << noise.noise_spread_db;
    }
  }
  if (noise.noise_seed_specified) {
    out << YAML::Key << "noise_seed" << YAML::Value << noise.noise_seed;
  }
  out << YAML::EndMap;
}

template <typename DropoutBlock>
void EmitDropoutBlock(YAML::Emitter& out, const char* key,
                      const DropoutBlock& block) {
  if (block.scale <= 0 && !block.seed_specified) {
    return;
  }

  out << YAML::Key << key << YAML::Value << YAML::BeginMap;
  out << YAML::Key << "scale" << YAML::Value << block.scale;
  if (block.seed_specified) {
    out << YAML::Key << "seed" << YAML::Value << block.seed;
  }
  out << YAML::EndMap;
}

void EmitDropouts(YAML::Emitter& out, const DropoutParameters& dropouts) {
  const bool has_random =
      dropouts.random.scale > 0 || dropouts.random.seed_specified;
  const bool has_scratch =
      dropouts.scratch.scale > 0 || dropouts.scratch.seed_specified;
  if (!has_random && !has_scratch) {
    return;
  }

  out << YAML::Key << "dropouts" << YAML::Value << YAML::BeginMap;
  EmitDropoutBlock(out, "random", dropouts.random);
  EmitDropoutBlock(out, "scratch", dropouts.scratch);
  out << YAML::EndMap;
}

void EmitOsd(YAML::Emitter& out, const OsdConfig& osd) {
  if (osd.overlays.empty()) {
    return;
  }

  out << YAML::Key << "osd" << YAML::Value << YAML::BeginMap;
  out << YAML::Key << "overlays" << YAML::Value << YAML::BeginSeq;
  for (const OsdOverlay& overlay : osd.overlays) {
    out << YAML::BeginMap;
    out << YAML::Key << "text" << YAML::Value << overlay.text;
    if (overlay.x != 0) {
      out << YAML::Key << "x" << YAML::Value << overlay.x;
    }
    if (overlay.y != 0) {
      out << YAML::Key << "y" << YAML::Value << overlay.y;
    }
    if (overlay.scale != kDefaultOverlayScale) {
      out << YAML::Key << "scale" << YAML::Value << overlay.scale;
    }
    if (overlay.fg_level != kDefaultOverlayFgLevel) {
      out << YAML::Key << "fg_luma" << YAML::Value
          << OsdFgLevelToString(overlay.fg_level);
    }
    if (overlay.bg_level != kDefaultOverlayBgLevel) {
      out << YAML::Key << "bg_luma" << YAML::Value
          << OsdBgLevelToString(overlay.bg_level);
    }
    out << YAML::EndMap;
  }
  out << YAML::EndSeq;
  out << YAML::EndMap;
}

// Emits one channel (left or right) tone sub-map when active. A disabled
// channel is omitted entirely, which re-parses back to a silent channel.
void EmitAudioChannel(YAML::Emitter& out, const char* key,
                      const AudioParameters& audio) {
  if (!audio.enabled) {
    return;
  }

  out << YAML::Key << key << YAML::Value << YAML::BeginMap;
  // waveform_text is non-empty exactly when the source file specified the
  // key; emitting it unconditionally would break round-trip equality.
  if (!audio.waveform_text.empty()) {
    out << YAML::Key << "waveform" << YAML::Value << audio.waveform_text;
  }
  if (audio.frequency_hz != kDefaultAudioFrequencyHz) {
    out << YAML::Key << "frequency" << YAML::Value << audio.frequency_hz;
  }
  if (audio.amplitude != kDefaultAudioAmplitude) {
    out << YAML::Key << "amplitude" << YAML::Value << audio.amplitude;
  }
  if (audio.ramp_enabled) {
    out << YAML::Key << "ramp" << YAML::Value << YAML::BeginMap;
    if (audio.ramp_start_specified) {
      out << YAML::Key << "start" << YAML::Value << audio.ramp_start_hz;
    }
    if (audio.ramp_end_specified) {
      out << YAML::Key << "end" << YAML::Value << audio.ramp_end_hz;
    }
    if (!audio.ramp_mode_text.empty()) {
      out << YAML::Key << "mode" << YAML::Value << audio.ramp_mode_text;
    }
    if (audio.ramp_period_seconds != 0.0) {
      out << YAML::Key << "period" << YAML::Value << audio.ramp_period_seconds;
    }
    out << YAML::EndMap;
  }
  out << YAML::EndMap;
}

void EmitAudio(YAML::Emitter& out, const Section& section) {
  if (section.audio_channel_pairs.empty()) {
    return;
  }

  out << YAML::Key << "audio" << YAML::Value << YAML::BeginMap;
  out << YAML::Key << "channel_pairs" << YAML::Value << YAML::BeginSeq;
  for (const AudioChannelPair& channel_pair : section.audio_channel_pairs) {
    out << YAML::BeginMap;
    if (channel_pair.pair_specified) {
      out << YAML::Key << "pair" << YAML::Value << channel_pair.pair;
    }
    if (!channel_pair.description.empty()) {
      out << YAML::Key << "description" << YAML::Value
          << channel_pair.description;
    }
    EmitAudioChannel(out, "left", channel_pair.left);
    EmitAudioChannel(out, "right", channel_pair.right);
    out << YAML::EndMap;
  }
  out << YAML::EndSeq;
  out << YAML::EndMap;
}

void EmitSection(YAML::Emitter& out, const Section& section) {
  out << YAML::BeginMap;
  out << YAML::Key << "name" << YAML::Value << section.name;
  out << YAML::Key << "type" << YAML::Value << section.type;
  if (!section.source.empty()) {
    out << YAML::Key << "source" << YAML::Value << section.source;
  }
  if (section.duration_frames_all) {
    out << YAML::Key << "duration_frames" << YAML::Value << "all";
    // Only meaningful alongside duration_frames: all; omit the default of 1 to
    // keep serialised projects clean and backward-compatible.
    if (section.duration_frames_repeat > 1) {
      out << YAML::Key << "duration_repeat" << YAML::Value
          << section.duration_frames_repeat;
    }
  } else {
    out << YAML::Key << "duration_frames" << YAML::Value
        << section.duration_frames;
  }
  if (section.section_type != SectionType::kUnknown) {
    out << YAML::Key << "section_type" << YAML::Value
        << SectionTypeToString(section.section_type);
  }
  if (section.start_frame != 0) {
    out << YAML::Key << "start_frame" << YAML::Value << section.start_frame;
  }
  if (section.end_frame_specified) {
    out << YAML::Key << "end_frame" << YAML::Value << section.end_frame;
  }
  EmitLineInjections(out, section);
  EmitNoise(out, section.noise);
  EmitDropouts(out, section.dropouts);
  EmitOsd(out, section.osd);
  EmitAudio(out, section);
  out << YAML::EndMap;
}

}  // namespace

YamlProjectEmitter::YamlProjectEmitter(ILogger* logger) : logger_(logger) {}

std::string YamlProjectEmitter::EmitString(const Project& project) const {
  YAML::Emitter out;
  // max_digits10 guarantees doubles survive the emit -> parse round trip.
  out.SetDoublePrecision(std::numeric_limits<double>::max_digits10);
  out.SetFloatPrecision(std::numeric_limits<float>::max_digits10);

  out << YAML::BeginMap;
  EmitProjectBlock(out, project);
  EmitCvbsPresets(out, project.cvbs_presets);
  EmitOutputTargets(out, project.output);
  EmitProjectLineInjections(out, project.line_injections);

  out << YAML::Key << "sections" << YAML::Value << YAML::BeginSeq;
  for (const Section& section : project.sections) {
    EmitSection(out, section);
  }
  out << YAML::EndSeq;
  out << YAML::EndMap;

  return std::string(out.c_str()) + "\n";
}

bool YamlProjectEmitter::EmitFile(const Project& project,
                                  const std::string& path,
                                  std::string* error) const {
  if (logger_ != nullptr) {
    logger_->Info("Writing project file: " + path);
  }

  std::ofstream stream(path, std::ios::out | std::ios::trunc);
  if (!stream.is_open()) {
    if (error != nullptr) {
      *error = "Failed to open '" + path + "' for writing.";
    }
    return false;
  }

  stream << EmitString(project);
  stream.close();
  if (stream.fail()) {
    if (error != nullptr) {
      *error = "Failed to write project file '" + path + "'.";
    }
    return false;
  }

  return true;
}

}  // namespace videosynth
