/*
 * File:        yaml_project_parser.cpp
 * Module:      yaml_project_parser
 * Purpose:     Parses YAML project files into internal project models.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "videosynth/yaml_project_parser.h"

#include <yaml-cpp/yaml.h>

#include <exception>
#include <set>

#include "videosynth/biphase_types.h"
#include "videosynth/path_resolution.h"

namespace videosynth {

YamlProjectParser::YamlProjectParser(ILogger* logger) : logger_(logger) {}

namespace {

bool ValidateAllowedKeys(const YAML::Node& node,
                         const std::set<std::string>& allowed_keys,
                         const std::string& context,
                         std::vector<std::string>* errors) {
  if (!node || !node.IsMap() || errors == nullptr) {
    return true;
  }

  for (const auto& kv : node) {
    const std::string key = kv.first.as<std::string>("");
    if (key.empty()) {
      continue;
    }
    if (allowed_keys.find(key) == allowed_keys.end()) {
      errors->push_back(context + " contains unsupported field: '" + key +
                        "'.");
    }
  }

  return errors->empty();
}

bool ParseDurationFrames(const YAML::Node& section_node, Section* section,
                         std::vector<std::string>* errors) {
  if (section == nullptr || errors == nullptr) {
    return false;
  }

  const YAML::Node duration_node = section_node["duration_frames"];
  if (duration_node && !duration_node.IsScalar()) {
    errors->push_back(
        "section field 'duration_frames' must be a scalar integer or 'all'.");
    return false;
  }

  // An omitted duration_frames plays the selected source range, exactly as
  // 'all' does.
  const std::string scalar =
      duration_node ? duration_node.as<std::string>("") : "all";
  if (scalar == "all") {
    section->duration_frames_all = true;
    section->duration_frames = 0;
  } else {
    try {
      const int duration_frames = duration_node.as<int>();
      section->duration_frames_all = false;
      section->duration_frames = duration_frames;
    } catch (const YAML::Exception&) {
      errors->push_back(
          "section field 'duration_frames' must be an integer or the string "
          "'all'.");
      return false;
    }
  }

  // Optional repeat multiplier; only meaningful with duration_frames: all.
  if (section_node["duration_repeat"]) {
    const YAML::Node repeat_node = section_node["duration_repeat"];
    try {
      section->duration_frames_repeat = repeat_node.as<int>();
    } catch (const YAML::Exception&) {
      errors->push_back(
          "section field 'duration_repeat' must be a positive integer.");
      return false;
    }
  } else {
    section->duration_frames_repeat = 1;
  }

  return true;
}

bool ParseLineInjectionCodes(const YAML::Node& codes_node,
                             Section::LineInjection* injection,
                             std::vector<std::string>* errors) {
  if (!codes_node || !codes_node.IsSequence() || injection == nullptr ||
      errors == nullptr) {
    return false;
  }

  for (const YAML::Node& code_node : codes_node) {
    if (!code_node.IsMap()) {
      errors->push_back("line_injections[].codes[] must be a map/object.");
      return false;
    }

    const std::set<std::string> code_keys = {"code_type", "start_value",
                                             "chapter", "programme_status",
                                             "users_code"};
    ValidateAllowedKeys(code_node, code_keys, "line_injections[].codes[]",
                        errors);
    if (!errors->empty()) {
      return false;
    }

    Section::LineInjectionCode code;
    code.code_type = code_node["code_type"].as<std::string>("");
    if (code.code_type.empty()) {
      errors->push_back(
          "line_injections[].codes[] is missing required field: 'code_type'.");
      return false;
    }

    if (code_node["start_value"]) {
      code.start_value = code_node["start_value"].as<int>();
      code.start_value_specified = true;
    }
    if (code_node["chapter"]) {
      code.chapter = code_node["chapter"].as<int>();
      code.chapter_specified = true;
    }
    if (code_node["programme_status"]) {
      code.programme_status = code_node["programme_status"].as<std::string>("");
      code.programme_status_specified = true;
    }
    if (code_node["users_code"]) {
      code.users_code = code_node["users_code"].as<std::string>("");
      code.users_code_specified = true;
    }

    injection->codes.push_back(code);
  }

  return true;
}

bool ParseLineInjections(const YAML::Node& section_node, Section* section,
                         std::vector<std::string>* errors) {
  if (section == nullptr || errors == nullptr) {
    return false;
  }

  if (!section_node["line_injections"]) {
    return true;
  }

  const YAML::Node line_injections_node = section_node["line_injections"];
  if (!line_injections_node.IsSequence()) {
    errors->push_back(
        "section field 'line_injections' must be a list/sequence.");
    return false;
  }

  for (const YAML::Node& injection_node : line_injections_node) {
    if (!injection_node.IsMap()) {
      errors->push_back("line_injections[] must be a map/object.");
      return false;
    }

    // Section-level line injections carry only laserdisc codes (and vitc).
    // The VITS set and the disc_type (CAV/CLV) are project-wide and live in the
    // top-level `line_injections:` block, so they are not accepted here.
    const std::set<std::string> injection_keys = {"type", "target_lines",
                                                  "codes"};
    ValidateAllowedKeys(injection_node, injection_keys, "line_injections[]",
                        errors);
    if (!errors->empty()) {
      return false;
    }

    Section::LineInjection injection;
    injection.type = injection_node["type"].as<std::string>("");
    if (injection.type.empty()) {
      errors->push_back("line_injections[] is missing required field: 'type'.");
      return false;
    }

    if (injection_node["target_lines"]) {
      const YAML::Node target_lines_node = injection_node["target_lines"];
      if (!target_lines_node.IsSequence()) {
        errors->push_back(
            "line_injections[].target_lines must be a list/sequence.");
        return false;
      }
      for (const YAML::Node& target_line : target_lines_node) {
        injection.target_lines.push_back(target_line.as<int>());
      }
    }

    if (injection_node["codes"] &&
        !ParseLineInjectionCodes(injection_node["codes"], &injection, errors)) {
      return false;
    }

    section->line_injections.push_back(injection);
  }

  return true;
}

// Parses the project-wide `line_injections:` block: the laserdisc disc_type
// (CAV/CLV) and the VITS test-signal set. Both are project-wide decisions, so
// they live at the top level rather than per section.
bool ParseProjectLineInjections(const YAML::Node& root, Project* project,
                                std::vector<std::string>* errors) {
  if (project == nullptr || errors == nullptr) {
    return false;
  }

  if (!root["line_injections"]) {
    return true;
  }

  const YAML::Node node = root["line_injections"];
  if (!node.IsMap()) {
    errors->push_back("Top-level 'line_injections' must be a map/object.");
    return false;
  }

  const std::set<std::string> keys = {"disc_type", "placement", "vits"};
  ValidateAllowedKeys(node, keys, "line_injections", errors);
  if (!errors->empty()) {
    return false;
  }

  project->line_injections.disc_type = node["disc_type"].as<std::string>("");

  if (node["placement"]) {
    const std::string placement = node["placement"].as<std::string>("");
    if (placement != "standard" && placement != "laserdisc" &&
        placement != "custom") {
      errors->push_back(
          "line_injections.placement must be 'standard', 'laserdisc', or "
          "'custom'.");
      return false;
    }
    project->line_injections.placement = VitsPlacementFromString(placement);
  }

  if (node["vits"]) {
    const YAML::Node vits_node = node["vits"];
    if (!vits_node.IsSequence()) {
      errors->push_back("line_injections.vits must be a list/sequence.");
      return false;
    }
    for (const YAML::Node& entry : vits_node) {
      if (!entry.IsMap()) {
        errors->push_back("line_injections.vits[] must be a map/object.");
        return false;
      }
      const std::set<std::string> vits_keys = {"vits_type", "target_lines"};
      ValidateAllowedKeys(entry, vits_keys, "line_injections.vits[]", errors);
      if (!errors->empty()) {
        return false;
      }

      VitsInjection vits;
      vits.vits_type = entry["vits_type"].as<std::string>("");
      if (vits.vits_type.empty()) {
        errors->push_back(
            "line_injections.vits[] is missing required field: 'vits_type'.");
        return false;
      }
      if (entry["target_lines"]) {
        const YAML::Node target_lines_node = entry["target_lines"];
        if (!target_lines_node.IsSequence()) {
          errors->push_back(
              "line_injections.vits[].target_lines must be a list/sequence.");
          return false;
        }
        for (const YAML::Node& target_line : target_lines_node) {
          vits.target_lines.push_back(target_line.as<int>());
        }
      }
      project->line_injections.vits.push_back(std::move(vits));
    }
  }

  return true;
}

// Parses one audio channel (left or right) tone descriptor: waveform,
// frequency, amplitude, and an optional frequency ramp. Sets out->enabled on
// success. Appends to errors and returns false on a structural error.
bool ParseAudioChannel(const YAML::Node& node, const std::string& context,
                       AudioParameters* out, std::vector<std::string>* errors) {
  if (!node.IsMap()) {
    errors->push_back(context + " must be a map/object.");
    return false;
  }
  const std::set<std::string> channel_keys = {"waveform", "frequency",
                                              "amplitude", "ramp"};
  ValidateAllowedKeys(node, channel_keys, context, errors);
  if (!errors->empty()) {
    return false;
  }

  AudioParameters audio;
  audio.enabled = true;
  if (node["waveform"]) {
    audio.waveform_text = node["waveform"].as<std::string>("");
    audio.waveform = AudioWaveformFromString(audio.waveform_text);
  }
  if (node["frequency"]) {
    audio.frequency_hz = node["frequency"].as<double>();
  }
  if (node["amplitude"]) {
    audio.amplitude = node["amplitude"].as<double>();
  }

  if (node["ramp"]) {
    const YAML::Node ramp_node = node["ramp"];
    if (!ramp_node.IsMap()) {
      errors->push_back(context + ".ramp must be a map/object.");
      return false;
    }
    const std::set<std::string> ramp_keys = {"start", "end", "mode", "period"};
    ValidateAllowedKeys(ramp_node, ramp_keys, context + ".ramp", errors);
    if (!errors->empty()) {
      return false;
    }

    audio.ramp_enabled = true;
    if (ramp_node["start"]) {
      audio.ramp_start_hz = ramp_node["start"].as<double>();
      audio.ramp_start_specified = true;
    }
    if (ramp_node["end"]) {
      audio.ramp_end_hz = ramp_node["end"].as<double>();
      audio.ramp_end_specified = true;
    }
    if (ramp_node["mode"]) {
      audio.ramp_mode_text = ramp_node["mode"].as<std::string>("");
      audio.ramp_mode = AudioRampModeFromString(audio.ramp_mode_text);
    }
    if (ramp_node["period"]) {
      audio.ramp_period_seconds = ramp_node["period"].as<double>();
    }
  }

  *out = audio;
  return true;
}

// Helper function to parse a YAML node into a Project structure.
// This is shared between ParseFile and ParseString.
ParseResult ParseYamlNode(const YAML::Node& root, ILogger* logger) {
  ParseResult result;

  try {
    if (!root["cvbs_presets"]) {
      result.errors.push_back(
          "Missing required top-level field: cvbs_presets.");
      return result;
    }

    if (!root["sections"] || !root["sections"].IsSequence()) {
      result.errors.push_back("Missing required top-level list: sections.");
      return result;
    }

    if (!root["output"] || !root["output"].IsMap()) {
      result.errors.push_back("Missing required top-level map: output.");
      return result;
    }

    const std::set<std::string> root_keys = {
        "project", "cvbs_presets", "output", "line_injections", "sections"};
    ValidateAllowedKeys(root, root_keys, "Top-level YAML", &result.errors);
    if (!result.errors.empty()) {
      return result;
    }

    if (root["project"] && root["project"]["name"]) {
      result.project.name = root["project"]["name"].as<std::string>();
    }

    if (root["project"] && root["project"]["version"]) {
      result.project.version = root["project"]["version"].as<std::string>();
    }

    if (root["project"] && root["project"]["description"]) {
      result.project.description =
          root["project"]["description"].as<std::string>();
    }

    if (root["project"]) {
      const std::set<std::string> project_keys = {"name", "version",
                                                  "description"};
      ValidateAllowedKeys(root["project"], project_keys, "project",
                          &result.errors);
      if (!result.errors.empty()) {
        return result;
      }
    }

    const YAML::Node presets = root["cvbs_presets"];
    const std::set<std::string> preset_keys = {
        "video_standard_preset",     "sample_encoding_preset",
        "signal_state_preset",       "pal_laserdisc_pilot_burst",
        "ntsc_laserdisc_vbi_burst",  "ntsc_black_setup_ire",
        "secam_field_identification"};
    ValidateAllowedKeys(presets, preset_keys, "cvbs_presets", &result.errors);
    if (!result.errors.empty()) {
      return result;
    }

    result.project.cvbs_presets.video_standard_preset = StandardFromString(
        presets["video_standard_preset"].as<std::string>(""));

    result.project.cvbs_presets.sample_encoding_preset =
        presets["sample_encoding_preset"].as<std::string>(
            result.project.cvbs_presets.sample_encoding_preset);
    // When omitted, the signal state defaults to the one generated output
    // declares for the standard (unlocked for SECAM, locked otherwise).
    result.project.cvbs_presets.signal_state_preset =
        presets["signal_state_preset"].as<std::string>(
            GeneratedSignalStatePreset(
                result.project.cvbs_presets.video_standard_preset));
    result.project.cvbs_presets.pal_laserdisc_pilot_burst =
        presets["pal_laserdisc_pilot_burst"].as<bool>(
            result.project.cvbs_presets.pal_laserdisc_pilot_burst);
    result.project.cvbs_presets.ntsc_laserdisc_vbi_burst =
        presets["ntsc_laserdisc_vbi_burst"].as<bool>(
            result.project.cvbs_presets.ntsc_laserdisc_vbi_burst);
    if (presets["ntsc_black_setup_ire"]) {
      result.project.cvbs_presets.ntsc_black_setup_ire =
          presets["ntsc_black_setup_ire"].as<double>();
      result.project.cvbs_presets.ntsc_black_setup_ire_specified = true;
    }
    result.project.cvbs_presets.secam_field_identification =
        presets["secam_field_identification"].as<bool>(
            result.project.cvbs_presets.secam_field_identification);

    const YAML::Node output = root["output"];
    const std::set<std::string> output_keys = {"video_path", "signal_type",
                                               "efm_audio"};
    ValidateAllowedKeys(output, output_keys, "output", &result.errors);
    if (!result.errors.empty()) {
      return result;
    }

    result.project.output.video_path = output["video_path"].as<std::string>("");
    // The metadata sidecar is always colocated with the video output and is
    // never specified in the YAML; derive it from the video path.
    result.project.output.metadata_path =
        DeriveMetadataPath(result.project.output.video_path);
    result.project.output.signal_type =
        output["signal_type"].as<std::string>("composite");

    // Presence of the output.efm_audio map enables LaserDisc digital audio
    // output for the single channel pair it names.
    if (output["efm_audio"]) {
      const YAML::Node efm_node = output["efm_audio"];
      if (!efm_node.IsMap()) {
        result.errors.push_back("output.efm_audio must be a map/object.");
        return result;
      }
      const std::set<std::string> efm_keys = {"pair"};
      ValidateAllowedKeys(efm_node, efm_keys, "output.efm_audio",
                          &result.errors);
      if (!result.errors.empty()) {
        return result;
      }
      if (!efm_node["pair"]) {
        result.errors.push_back(
            "output.efm_audio is missing required field: 'pair'.");
        return result;
      }
      result.project.output.efm_audio.enabled = true;
      result.project.output.efm_audio.pair = efm_node["pair"].as<int>();
    }

    if (!ParseProjectLineInjections(root, &result.project, &result.errors)) {
      return result;
    }

    for (const YAML::Node& section_node : root["sections"]) {
      if (!section_node.IsMap()) {
        result.errors.push_back("Each section entry must be a map/object.");
        return result;
      }

      const std::set<std::string> section_keys = {"name",
                                                  "type",
                                                  "section_type",
                                                  "duration_frames",
                                                  "duration_repeat",
                                                  "line_injections",
                                                  "source",
                                                  "start_frame",
                                                  "end_frame",
                                                  "noise",
                                                  "dropouts",
                                                  "osd",
                                                  "audio"};
      ValidateAllowedKeys(section_node, section_keys, "section",
                          &result.errors);
      if (!result.errors.empty()) {
        return result;
      }

      Section section;
      section.name = section_node["name"].as<std::string>("");
      section.type = section_node["type"].as<std::string>("");
      section.source = section_node["source"].as<std::string>("");

      if (section_node["section_type"]) {
        const std::string section_type_str =
            section_node["section_type"].as<std::string>("");
        section.section_type = SectionTypeFromString(section_type_str);
        if (section.section_type == SectionType::kUnknown) {
          result.errors.push_back(
              "section field 'section_type' has unrecognised value '" +
              section_type_str +
              "'. Expected one of: lead_in, programme_area, lead_out.");
          return result;
        }
      }
      section.start_frame = section_node["start_frame"].as<int>(0);
      if (section_node["end_frame"]) {
        try {
          section.end_frame = section_node["end_frame"].as<int>();
        } catch (const YAML::Exception&) {
          result.errors.push_back(
              "section field 'end_frame' must be an integer frame index.");
          return result;
        }
        section.end_frame_specified = true;
      }
      if (!ParseLineInjections(section_node, &section, &result.errors)) {
        return result;
      }
      if (!ParseDurationFrames(section_node, &section, &result.errors)) {
        return result;
      }

      if (section_node["noise"]) {
        const YAML::Node noise_node = section_node["noise"];
        if (!noise_node.IsMap()) {
          result.errors.push_back(
              "section field 'noise' must be a map/object.");
          return result;
        }
        const std::set<std::string> noise_keys = {"noise_db", "noise_spread_db",
                                                  "noise_seed"};
        ValidateAllowedKeys(noise_node, noise_keys, "section.noise",
                            &result.errors);
        if (!result.errors.empty()) {
          return result;
        }
        if (!noise_node["noise_db"] && noise_node["noise_spread_db"]) {
          result.errors.push_back(
              "section noise validation error: noise_spread_db requires "
              "noise_db to be specified.");
          return result;
        }
        if (noise_node["noise_db"]) {
          section.noise.enabled = true;
          section.noise.noise_db = noise_node["noise_db"].as<double>();
          section.noise.noise_spread_db =
              noise_node["noise_spread_db"].as<double>(0.0);
        }
        if (noise_node["noise_seed"]) {
          section.noise.noise_seed = noise_node["noise_seed"].as<int64_t>();
          section.noise.noise_seed_specified = true;
        }
      }

      if (section_node["dropouts"]) {
        const YAML::Node dropouts_node = section_node["dropouts"];
        if (!dropouts_node.IsMap()) {
          result.errors.push_back(
              "section field 'dropouts' must be a map/object.");
          return result;
        }
        const std::set<std::string> dropout_keys = {"random", "scratch"};
        ValidateAllowedKeys(dropouts_node, dropout_keys, "section.dropouts",
                            &result.errors);
        if (!result.errors.empty()) {
          return result;
        }

        if (dropouts_node["random"]) {
          const YAML::Node rnd = dropouts_node["random"];
          if (!rnd.IsMap()) {
            result.errors.push_back(
                "section.dropouts.random must be a map/object.");
            return result;
          }
          const std::set<std::string> rnd_keys = {"scale", "seed"};
          ValidateAllowedKeys(rnd, rnd_keys, "section.dropouts.random",
                              &result.errors);
          if (!result.errors.empty()) {
            return result;
          }
          section.dropouts.random.scale = rnd["scale"].as<int>(0);
          if (section.dropouts.random.scale > 0) {
            section.dropouts.random.enabled = true;
          }
          if (rnd["seed"]) {
            section.dropouts.random.seed = rnd["seed"].as<int64_t>();
            section.dropouts.random.seed_specified = true;
          }
        }

        if (dropouts_node["scratch"]) {
          const YAML::Node scr = dropouts_node["scratch"];
          if (!scr.IsMap()) {
            result.errors.push_back(
                "section.dropouts.scratch must be a map/object.");
            return result;
          }
          const std::set<std::string> scr_keys = {"scale", "seed"};
          ValidateAllowedKeys(scr, scr_keys, "section.dropouts.scratch",
                              &result.errors);
          if (!result.errors.empty()) {
            return result;
          }
          section.dropouts.scratch.scale = scr["scale"].as<int>(0);
          if (section.dropouts.scratch.scale > 0) {
            section.dropouts.scratch.enabled = true;
          }
          if (scr["seed"]) {
            section.dropouts.scratch.seed = scr["seed"].as<int64_t>();
            section.dropouts.scratch.seed_specified = true;
          }
        }
      }

      if (section_node["osd"]) {
        const YAML::Node osd_node = section_node["osd"];
        if (!osd_node.IsMap()) {
          result.errors.push_back("section field 'osd' must be a map/object.");
          return result;
        }
        const std::set<std::string> osd_keys = {"overlays"};
        ValidateAllowedKeys(osd_node, osd_keys, "section.osd", &result.errors);
        if (!result.errors.empty()) {
          return result;
        }

        if (osd_node["overlays"]) {
          const YAML::Node overlays = osd_node["overlays"];
          if (!overlays.IsSequence()) {
            result.errors.push_back(
                "section.osd.overlays must be a sequence/list.");
            return result;
          }
          for (const YAML::Node& ov_node : overlays) {
            if (!ov_node.IsMap()) {
              result.errors.push_back(
                  "Each section.osd.overlays entry must be a map/object.");
              return result;
            }
            const std::set<std::string> ov_keys = {
                "text", "x", "y", "scale", "fg_luma", "bg_luma"};
            ValidateAllowedKeys(ov_node, ov_keys, "section.osd.overlays[]",
                                &result.errors);
            if (!result.errors.empty()) {
              return result;
            }
            OsdOverlay overlay;
            overlay.text = ov_node["text"].as<std::string>("");
            overlay.x = ov_node["x"].as<int>(0);
            overlay.y = ov_node["y"].as<int>(0);
            overlay.scale = ov_node["scale"].as<int>(1);
            if (ov_node["fg_luma"]) {
              overlay.fg_level_text = ov_node["fg_luma"].as<std::string>("");
              overlay.fg_level = OsdFgLevelFromString(overlay.fg_level_text);
            }
            if (ov_node["bg_luma"]) {
              overlay.bg_level_text = ov_node["bg_luma"].as<std::string>("");
              overlay.bg_level = OsdBgLevelFromString(overlay.bg_level_text);
            }
            section.osd.overlays.push_back(overlay);
          }
        }
      }

      if (section_node["audio"]) {
        const YAML::Node audio_node = section_node["audio"];
        if (!audio_node.IsMap()) {
          result.errors.push_back(
              "section field 'audio' must be a map/object.");
          return result;
        }
        const std::set<std::string> audio_keys = {"channel_pairs"};
        ValidateAllowedKeys(audio_node, audio_keys, "section.audio",
                            &result.errors);
        if (!result.errors.empty()) {
          return result;
        }

        const YAML::Node pairs_node = audio_node["channel_pairs"];
        if (!pairs_node || !pairs_node.IsSequence()) {
          result.errors.push_back(
              "section.audio.channel_pairs must be a sequence.");
          return result;
        }

        for (const YAML::Node& pair_node : pairs_node) {
          if (!pair_node.IsMap()) {
            result.errors.push_back(
                "section.audio.channel_pairs[] must be a map/object.");
            return result;
          }
          const std::set<std::string> pair_keys = {"pair", "description",
                                                   "left", "right"};
          ValidateAllowedKeys(pair_node, pair_keys,
                              "section.audio.channel_pairs[]", &result.errors);
          if (!result.errors.empty()) {
            return result;
          }

          AudioChannelPair channel_pair;
          if (pair_node["pair"]) {
            channel_pair.pair = pair_node["pair"].as<int>();
            channel_pair.pair_specified = true;
          }
          if (pair_node["description"]) {
            channel_pair.description =
                pair_node["description"].as<std::string>("");
          }
          if (pair_node["left"] &&
              !ParseAudioChannel(pair_node["left"],
                                 "section.audio.channel_pairs[].left",
                                 &channel_pair.left, &result.errors)) {
            return result;
          }
          if (pair_node["right"] &&
              !ParseAudioChannel(pair_node["right"],
                                 "section.audio.channel_pairs[].right",
                                 &channel_pair.right, &result.errors)) {
            return result;
          }
          section.audio_channel_pairs.push_back(channel_pair);
        }
      }

      result.project.sections.push_back(section);
    }

    result.ok = true;
    if (logger != nullptr) {
      logger->Debug("Parsed project with " +
                    std::to_string(result.project.sections.size()) +
                    " section(s).");
    }
    return result;
  } catch (const YAML::Exception& ex) {
    result.errors.push_back(std::string("YAML parsing failed: ") + ex.what());
    return result;
  } catch (const std::exception& ex) {
    result.errors.push_back(std::string("Unexpected parse error: ") +
                            ex.what());
    return result;
  }
}

}  // namespace

ParseResult YamlProjectParser::ParseFile(const std::string& path) {
  if (logger_ != nullptr) {
    logger_->Info("Parsing project file: " + path);
  }

  try {
    const YAML::Node root = YAML::LoadFile(path);
    return ParseYamlNode(root, logger_);
  } catch (const YAML::Exception& ex) {
    ParseResult result;
    result.errors.push_back(std::string("YAML file loading failed: ") +
                            ex.what());
    return result;
  }
}

ParseResult YamlProjectParser::ParseString(const std::string& yaml) {
  if (logger_ != nullptr) {
    logger_->Info("Parsing project string");
  }

  try {
    const YAML::Node root = YAML::Load(yaml);
    return ParseYamlNode(root, logger_);
  } catch (const YAML::Exception& ex) {
    ParseResult result;
    result.errors.push_back(std::string("YAML string parsing failed: ") +
                            ex.what());
    return result;
  }
}

}  // namespace videosynth
