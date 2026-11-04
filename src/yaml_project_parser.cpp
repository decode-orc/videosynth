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

#include <array>
#include <exception>
#include <set>
#include <utility>

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

// Reports each key of `moved` that `node` still carries, naming the place it
// moved to when the laserdisc settings were gathered into `laserdisc:`
// blocks. Returns false when any was found.
bool ReportMovedKeys(
    const YAML::Node& node,
    const std::vector<std::pair<std::string, std::string>>& moved,
    const std::string& context, std::vector<std::string>* errors) {
  if (!node || !node.IsMap()) {
    return true;
  }
  bool clean = true;
  for (const auto& [key, destination] : moved) {
    if (node[key]) {
      errors->push_back(context + "." + key + " has moved to " + destination +
                        ".");
      clean = false;
    }
  }
  return clean;
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

// Parses a noise `spectrum:` table: a sequence of [MHz, relative dB] pairs.
// Value ranges and ordering are checked by the validator.
bool ParseNoiseSpectrum(const YAML::Node& spectrum_node,
                        const std::string& context,
                        std::vector<NoiseSpectrumPoint>* spectrum,
                        std::vector<std::string>* errors) {
  if (!spectrum_node.IsSequence()) {
    errors->push_back(context +
                      " must be a sequence of [frequency_mhz, relative_db] "
                      "pairs.");
    return false;
  }
  spectrum->clear();
  for (const YAML::Node& point_node : spectrum_node) {
    if (!point_node.IsSequence() || point_node.size() != 2U) {
      errors->push_back(context +
                        " entries must be [frequency_mhz, relative_db] "
                        "pairs.");
      return false;
    }
    try {
      spectrum->push_back(NoiseSpectrumPoint{point_node[0].as<double>(),
                                             point_node[1].as<double>()});
    } catch (const YAML::Exception&) {
      errors->push_back(context + " entries must be numeric pairs.");
      return false;
    }
  }
  return true;
}

// Parses a `level_db:` curve: a sequence of [mV, dB] pairs. Value ranges
// and ordering are checked by the validator.
bool ParseNoiseLevelCurve(const YAML::Node& curve_node,
                          const std::string& context,
                          std::vector<NoiseLevelPoint>* curve,
                          std::vector<std::string>* errors) {
  if (!curve_node.IsSequence()) {
    errors->push_back(context +
                      " must be a sequence of [level_mv, relative_db] pairs.");
    return false;
  }
  curve->clear();
  for (const YAML::Node& point_node : curve_node) {
    if (!point_node.IsSequence() || point_node.size() != 2U) {
      errors->push_back(context +
                        " entries must be [level_mv, relative_db] pairs.");
      return false;
    }
    try {
      curve->push_back(NoiseLevelPoint{point_node[0].as<double>(),
                                       point_node[1].as<double>()});
    } catch (const YAML::Exception&) {
      errors->push_back(context + " entries must be numeric pairs.");
      return false;
    }
  }
  return true;
}

// Parses the `noise.shared.level_term:` level-dependent pressing component.
bool ParseSharedNoiseLevelTerm(const YAML::Node& term_node,
                               SharedNoiseLevelTerm* term,
                               std::vector<std::string>* errors) {
  if (!term_node.IsMap()) {
    errors->push_back(
        "section field 'noise.shared.level_term' must be a map/object.");
    return false;
  }
  const std::set<std::string> term_keys = {"noise_db", "level_db", "spectrum"};
  if (!ValidateAllowedKeys(term_node, term_keys,
                           "section.noise.shared.level_term", errors)) {
    return false;
  }
  if (!term_node["noise_db"] || !term_node["level_db"]) {
    errors->push_back(
        "section noise validation error: noise.shared.level_term requires "
        "noise_db and level_db.");
    return false;
  }
  term->enabled = true;
  term->noise_db = term_node["noise_db"].as<double>();
  if (!ParseNoiseLevelCurve(term_node["level_db"],
                            "section field 'noise.shared.level_term.level_db'",
                            &term->level_db, errors)) {
    return false;
  }
  if (term_node["spectrum"]) {
    return ParseNoiseSpectrum(
        term_node["spectrum"],
        "section field 'noise.shared.level_term.spectrum'", &term->spectrum,
        errors);
  }
  return true;
}

// Parses the `noise.shared:` pressing-noise layer.
bool ParseSharedNoise(const YAML::Node& shared_node,
                      SharedNoiseParameters* shared,
                      std::vector<std::string>* errors) {
  if (!shared_node.IsMap()) {
    errors->push_back("section field 'noise.shared' must be a map/object.");
    return false;
  }
  if (shared_node["noise_spread_db"]) {
    errors->push_back(
        "section noise validation error: noise.shared does not take "
        "noise_spread_db; use noise.shared.level_term for level-dependent "
        "pressing noise.");
    return false;
  }
  const std::set<std::string> shared_keys = {"noise_db", "noise_seed",
                                             "spectrum", "level_term"};
  if (!ValidateAllowedKeys(shared_node, shared_keys, "section.noise.shared",
                           errors)) {
    return false;
  }
  if (!shared_node["noise_db"]) {
    errors->push_back(
        "section noise validation error: noise.shared requires noise_db.");
    return false;
  }
  shared->enabled = true;
  shared->noise_db = shared_node["noise_db"].as<double>();
  if (shared_node["noise_seed"]) {
    shared->noise_seed = shared_node["noise_seed"].as<int64_t>();
    shared->noise_seed_specified = true;
  }
  if (shared_node["spectrum"] &&
      !ParseNoiseSpectrum(shared_node["spectrum"],
                          "section field 'noise.shared.spectrum'",
                          &shared->spectrum, errors)) {
    return false;
  }
  if (shared_node["level_term"]) {
    return ParseSharedNoiseLevelTerm(shared_node["level_term"],
                                     &shared->level_term, errors);
  }
  return true;
}

bool ParseScalarString(const YAML::Node& node, const std::string& key,
                       const std::string& context, std::string* value,
                       std::vector<std::string>* errors);
bool ParseInteger(const YAML::Node& node, const std::string& key,
                  const std::string& context, int* value,
                  std::vector<std::string>* errors);

bool ParseLineInjectionCodes(const YAML::Node& codes_node,
                             Section::LineInjection* injection,
                             std::vector<std::string>* errors) {
  if (!codes_node || !codes_node.IsSequence() || injection == nullptr ||
      errors == nullptr) {
    return false;
  }

  for (const YAML::Node& code_node : codes_node) {
    if (!code_node.IsMap()) {
      errors->push_back("laserdisc.codes[] must be a map/object.");
      return false;
    }

    const std::set<std::string> code_keys = {"code_type", "start_value",
                                             "chapter", "programme_status",
                                             "users_code"};
    ValidateAllowedKeys(code_node, code_keys, "laserdisc.codes[]", errors);
    if (!errors->empty()) {
      return false;
    }

    const std::string context = "laserdisc.codes[]";
    Section::LineInjectionCode code;
    if (!code_node["code_type"]) {
      errors->push_back(context + " is missing required field: 'code_type'.");
      return false;
    }
    if (!ParseScalarString(code_node, "code_type", context, &code.code_type,
                           errors)) {
      return false;
    }
    if (code.code_type.empty()) {
      errors->push_back(context + ".code_type must not be empty.");
      return false;
    }

    if (code_node["start_value"]) {
      if (!ParseInteger(code_node, "start_value", context, &code.start_value,
                        errors)) {
        return false;
      }
      code.start_value_specified = true;
    }
    if (code_node["chapter"]) {
      if (!ParseInteger(code_node, "chapter", context, &code.chapter, errors)) {
        return false;
      }
      code.chapter_specified = true;
    }
    if (code_node["programme_status"]) {
      if (!ParseScalarString(code_node, "programme_status", context,
                             &code.programme_status, errors)) {
        return false;
      }
      code.programme_status_specified = true;
    }
    if (code_node["users_code"]) {
      if (!ParseScalarString(code_node, "users_code", context, &code.users_code,
                             errors)) {
        return false;
      }
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

    // Section-level line injections carry vitc only. Laserdisc codes live in
    // the section's `laserdisc:` block, and the VITS set is project-wide.
    const std::set<std::string> injection_keys = {"type", "target_lines"};
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
    if (injection.type == "laserdisc") {
      errors->push_back(
          "line_injections[] type 'laserdisc' has moved: laserdisc codes go "
          "in the section's laserdisc.codes list.");
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

    section->line_injections.push_back(injection);
  }

  return true;
}

// Reads one numeric key into *value. Returns false, with an error naming the
// key, when the value is not a number.
bool ParseNumber(const YAML::Node& node, const std::string& key,
                 const std::string& context, double* value,
                 std::vector<std::string>* errors) {
  try {
    *value = node[key].as<double>();
  } catch (const YAML::Exception&) {
    errors->push_back(context + "." + key + " must be a number.");
    return false;
  }
  return true;
}

// Parses `section.luma_bandwidth:`, the per-section luma bandwidth limit.
bool ParseLumaBandwidth(const YAML::Node& node,
                        LumaBandwidthParameters* luma_bandwidth,
                        std::vector<std::string>* errors) {
  const std::string context = "section.luma_bandwidth";
  if (!node.IsMap()) {
    errors->push_back(context + " must be a map/object.");
    return false;
  }
  const std::set<std::string> keys = {"cutoff_mhz", "transition_mhz"};
  ValidateAllowedKeys(node, keys, context, errors);
  if (!errors->empty()) {
    return false;
  }
  if (!node["cutoff_mhz"]) {
    errors->push_back(context + " is missing required field: 'cutoff_mhz'.");
    return false;
  }
  if (!ParseNumber(node, "cutoff_mhz", context, &luma_bandwidth->cutoff_mhz,
                   errors)) {
    return false;
  }
  if (node["transition_mhz"] &&
      !ParseNumber(node, "transition_mhz", context,
                   &luma_bandwidth->transition_mhz, errors)) {
    return false;
  }
  luma_bandwidth->enabled = true;
  return true;
}

// Parses a `vertical_db:` curve: a sequence of [cycles_per_row, dB] pairs.
// Value ranges and ordering are checked by the validator.
bool ParseVerticalResponseCurve(const YAML::Node& curve_node,
                                const std::string& context,
                                std::vector<VerticalResponsePoint>* curve,
                                std::vector<std::string>* errors) {
  if (!curve_node.IsSequence()) {
    errors->push_back(
        context +
        " must be a sequence of [cycles_per_row, relative_db] pairs.");
    return false;
  }
  curve->clear();
  for (const YAML::Node& point_node : curve_node) {
    if (!point_node.IsSequence() || point_node.size() != 2U) {
      errors->push_back(
          context + " entries must be [cycles_per_row, relative_db] pairs.");
      return false;
    }
    try {
      curve->push_back(VerticalResponsePoint{point_node[0].as<double>(),
                                             point_node[1].as<double>()});
    } catch (const YAML::Exception&) {
      errors->push_back(context + " entries must be numeric pairs.");
      return false;
    }
  }
  return true;
}

// Parses `section.picture_response:`: a preset name, or a map of
// horizontal_db / vertical_db curves.
bool ParsePictureResponse(const YAML::Node& node,
                          PictureResponseParameters* response,
                          std::vector<std::string>* errors) {
  const std::string context = "section.picture_response";
  *response = PictureResponseParameters{};
  if (node.IsScalar()) {
    const std::string name = node.as<std::string>("");
    if (name != "domesday") {
      errors->push_back(context +
                        " must be the preset name 'domesday' or a map of "
                        "horizontal_db / vertical_db curves; got '" +
                        name + "'.");
      return false;
    }
    response->preset = PictureResponsePreset::kDomesday;
    response->enabled = true;
    return true;
  }
  if (!node.IsMap()) {
    errors->push_back(context +
                      " must be a preset name or a map/object of curves.");
    return false;
  }
  const std::set<std::string> keys = {"horizontal_db", "vertical_db"};
  ValidateAllowedKeys(node, keys, context, errors);
  if (!errors->empty()) {
    return false;
  }
  if (!node["horizontal_db"] && !node["vertical_db"]) {
    errors->push_back(context +
                      " must set at least one of: horizontal_db, vertical_db.");
    return false;
  }
  if (node["horizontal_db"] &&
      !ParseNoiseSpectrum(node["horizontal_db"], context + ".horizontal_db",
                          &response->horizontal_db, errors)) {
    return false;
  }
  if (node["vertical_db"] &&
      !ParseVerticalResponseCurve(node["vertical_db"], context + ".vertical_db",
                                  &response->vertical_db, errors)) {
    return false;
  }
  response->preset = PictureResponsePreset::kCustom;
  response->enabled = true;
  return true;
}

// Reads a string-valued key into *value. Returns false, with an error naming
// the key, when the value is not a scalar.
bool ParseScalarString(const YAML::Node& node, const std::string& key,
                       const std::string& context, std::string* value,
                       std::vector<std::string>* errors) {
  const YAML::Node value_node = node[key];
  if (!value_node.IsScalar()) {
    errors->push_back(context + "." + key + " must be a string.");
    return false;
  }
  *value = value_node.as<std::string>("");
  return true;
}

// Parses `section.cadence:`, the section's field cadence (pattern, phase and
// continuity). Unknown values are rejected here so the model only holds
// valid enumerators; cross-section and standard rules are the validator's.
bool ParseCadence(const YAML::Node& node, CadenceParameters* cadence,
                  std::vector<std::string>* errors) {
  const std::string context = "section.cadence";
  if (!node.IsMap()) {
    errors->push_back(context + " must be a map/object.");
    return false;
  }
  const std::set<std::string> keys = {"pattern", "phase", "continuity"};
  ValidateAllowedKeys(node, keys, context, errors);
  if (!errors->empty()) {
    return false;
  }

  std::string text;
  if (node["pattern"]) {
    if (!ParseScalarString(node, "pattern", context, &text, errors)) {
      return false;
    }
    if (text == "video") {
      cadence->pattern = CadencePattern::kVideo;
    } else if (text == "2:3") {
      cadence->pattern = CadencePattern::kPulldown23;
    } else {
      errors->push_back(context + ".pattern has unrecognised value '" + text +
                        "'. Expected one of: video, \"2:3\".");
      return false;
    }
  }

  if (node["phase"]) {
    if (!ParseScalarString(node, "phase", context, &text, errors)) {
      return false;
    }
    if (text == "A") {
      cadence->phase = CadencePhase::kA;
    } else if (text == "B") {
      cadence->phase = CadencePhase::kB;
    } else if (text == "C") {
      cadence->phase = CadencePhase::kC;
    } else if (text == "D") {
      cadence->phase = CadencePhase::kD;
    } else {
      errors->push_back(context + ".phase has unrecognised value '" + text +
                        "'. Expected one of: A, B, C, D.");
      return false;
    }
    cadence->phase_specified = true;
  }

  if (node["continuity"]) {
    if (!ParseScalarString(node, "continuity", context, &text, errors)) {
      return false;
    }
    if (text == "restart") {
      cadence->continuity = CadenceContinuity::kRestart;
    } else if (text == "continue") {
      cadence->continuity = CadenceContinuity::kContinue;
    } else {
      errors->push_back(context + ".continuity has unrecognised value '" +
                        text + "'. Expected one of: restart, continue.");
      return false;
    }
  }
  return true;
}

// Parses `section.horizontal_resample:`.
bool ParseHorizontalResample(const YAML::Node& section_node,
                             HorizontalResample* resample,
                             std::vector<std::string>* errors) {
  const std::string context = "section";
  std::string text;
  if (!ParseScalarString(section_node, "horizontal_resample", context, &text,
                         errors)) {
    return false;
  }
  if (text == "nearest") {
    *resample = HorizontalResample::kNearest;
  } else if (text == "bandlimited") {
    *resample = HorizontalResample::kBandlimited;
  } else {
    errors->push_back(context +
                      ".horizontal_resample has unrecognised value '" + text +
                      "'. Expected one of: nearest, bandlimited.");
    return false;
  }
  return true;
}

// Reads one integer key into *value. Returns false, with an error naming the
// key, when the value is not an integer.
bool ParseInteger(const YAML::Node& node, const std::string& key,
                  const std::string& context, int* value,
                  std::vector<std::string>* errors) {
  try {
    *value = node[key].as<int>();
  } catch (const YAML::Exception&) {
    errors->push_back(context + "." + key + " must be an integer.");
    return false;
  }
  return true;
}

// Parses a section's `laserdisc:` block: the disc area the section belongs
// to and the biphase codes it carries. The codes become the section's
// laserdisc line injection.
bool ParseSectionLaserdisc(const YAML::Node& section_node, Section* section,
                           std::vector<std::string>* errors) {
  if (!section_node["laserdisc"]) {
    return true;
  }
  const YAML::Node node = section_node["laserdisc"];
  if (!node.IsMap()) {
    errors->push_back("section field 'laserdisc' must be a map/object.");
    return false;
  }
  const std::set<std::string> keys = {"area", "codes"};
  ValidateAllowedKeys(node, keys, "section.laserdisc", errors);
  if (!errors->empty()) {
    return false;
  }
  if (!node["area"] && !node["codes"]) {
    errors->push_back(
        "section.laserdisc must set at least one of: area, codes.");
    return false;
  }

  if (node["area"]) {
    std::string area;
    if (!ParseScalarString(node, "area", "section.laserdisc", &area, errors)) {
      return false;
    }
    section->section_type = SectionTypeFromString(area);
    if (section->section_type == SectionType::kUnknown) {
      errors->push_back("section.laserdisc.area has unrecognised value '" +
                        area +
                        "'. Expected one of: lead_in, programme_area, "
                        "lead_out.");
      return false;
    }
  }

  if (node["codes"]) {
    if (!node["codes"].IsSequence()) {
      errors->push_back("section.laserdisc.codes must be a list/sequence.");
      return false;
    }
    Section::LineInjection injection;
    injection.type = "laserdisc";
    if (!ParseLineInjectionCodes(node["codes"], &injection, errors)) {
      return false;
    }
    section->line_injections.push_back(std::move(injection));
  }
  return true;
}

// Parses `section.chroma_line_sign:`, the list of regions whose U or V
// follows the PAL V-switch. Geometry and standard rules are the validator's.
bool ParseChromaLineSign(const YAML::Node& node,
                         std::vector<ChromaLineSignRegion>* regions,
                         std::vector<std::string>* errors) {
  const std::string context = "section.chroma_line_sign";
  if (!node.IsSequence()) {
    errors->push_back(context + " must be a list/sequence.");
    return false;
  }
  for (const YAML::Node& entry : node) {
    const std::string entry_context = context + "[]";
    if (!entry.IsMap()) {
      errors->push_back(entry_context + " must be a map/object.");
      return false;
    }
    const std::set<std::string> keys = {"region", "u", "v"};
    ValidateAllowedKeys(entry, keys, entry_context, errors);
    if (!errors->empty()) {
      return false;
    }
    const YAML::Node region_node = entry["region"];
    if (!region_node) {
      errors->push_back(entry_context +
                        " is missing required field: 'region'.");
      return false;
    }
    const std::string region_context = entry_context + ".region";
    if (!region_node.IsMap()) {
      errors->push_back(region_context + " must be a map/object.");
      return false;
    }
    const std::set<std::string> region_keys = {"x", "y", "width", "height"};
    ValidateAllowedKeys(region_node, region_keys, region_context, errors);
    if (!errors->empty()) {
      return false;
    }

    ChromaLineSignRegion region;
    const std::array<std::pair<const char*, int*>, 4> geometry = {{
        {"x", &region.x},
        {"y", &region.y},
        {"width", &region.width},
        {"height", &region.height},
    }};
    for (const auto& [key, value] : geometry) {
      if (!region_node[key]) {
        errors->push_back(region_context + " is missing required field: '" +
                          key + "'.");
        return false;
      }
      if (!ParseInteger(region_node, key, region_context, value, errors)) {
        return false;
      }
    }

    const std::array<std::pair<const char*, ChromaLineSignMode*>, 2> axes = {{
        {"u", &region.u},
        {"v", &region.v},
    }};
    for (const auto& [key, mode] : axes) {
      if (!entry[key]) {
        continue;
      }
      std::string text;
      if (!ParseScalarString(entry, key, entry_context, &text, errors)) {
        return false;
      }
      if (text == "normal") {
        *mode = ChromaLineSignMode::kNormal;
      } else if (text == "follow_switch") {
        *mode = ChromaLineSignMode::kFollowSwitch;
      } else {
        errors->push_back(entry_context + "." + key +
                          " has unrecognised value '" + text +
                          "'. Expected one of: normal, follow_switch.");
        return false;
      }
    }
    regions->push_back(region);
  }
  return true;
}

// Parses one `section.dropouts.random:` or `section.dropouts.scratch:` block.
template <typename DropoutBlock>
bool ParseDropoutType(const YAML::Node& node, const std::string& name,
                      DropoutBlock* block, std::vector<std::string>* errors) {
  const std::string context = "section.dropouts." + name;
  if (!node.IsMap()) {
    errors->push_back(context + " must be a map/object.");
    return false;
  }
  const std::set<std::string> keys = {"scale", "seed", "flagged", "anchor"};
  ValidateAllowedKeys(node, keys, context, errors);
  if (!errors->empty()) {
    return false;
  }
  block->scale = node["scale"].as<int>(0);
  if (block->scale > 0) {
    block->enabled = true;
  }
  if (node["seed"]) {
    block->seed = node["seed"].as<int64_t>();
    block->seed_specified = true;
  }
  if (node["flagged"]) {
    try {
      block->flagged = node["flagged"].as<bool>();
    } catch (const YAML::Exception&) {
      errors->push_back(context + ".flagged must be true or false.");
      return false;
    }
  }
  if (node["anchor"]) {
    const std::string anchor = node["anchor"].as<std::string>("");
    if (anchor == "section") {
      block->anchor = DropoutAnchor::kSection;
    } else if (anchor == "disc") {
      block->anchor = DropoutAnchor::kDisc;
    } else {
      errors->push_back(context + ".anchor must be 'section' or 'disc'; got '" +
                        anchor + "'.");
      return false;
    }
  }
  return true;
}

// Parses `channel.edge_smear:`, the readout edge smear.
bool ParseEdgeSmear(const YAML::Node& node, EdgeSmearParameters* edge_smear,
                    std::vector<std::string>* errors) {
  if (!node.IsMap()) {
    errors->push_back("channel.edge_smear must be a map/object.");
    return false;
  }
  const std::set<std::string> keys = {"fraction", "tau_samples"};
  ValidateAllowedKeys(node, keys, "channel.edge_smear", errors);
  if (!errors->empty()) {
    return false;
  }
  const std::string context = "channel.edge_smear";
  for (const std::string& key : keys) {
    if (!node[key]) {
      errors->push_back(context + " is missing required field: '" + key + "'.");
      return false;
    }
  }
  if (!ParseNumber(node, "fraction", context, &edge_smear->fraction, errors) ||
      !ParseNumber(node, "tau_samples", context, &edge_smear->tau_samples,
                   errors)) {
    return false;
  }
  edge_smear->enabled = true;
  return true;
}

// Parses `laserdisc.channel.fm_beat:`, the PAL FM-beat herringbone.
bool ParseFmBeat(const YAML::Node& node, FmBeatParameters* fm_beat,
                 std::vector<std::string>* errors) {
  if (!node.IsMap()) {
    errors->push_back("laserdisc.channel.fm_beat must be a map/object.");
    return false;
  }
  const std::set<std::string> keys = {"carrier_black_hz", "level_db"};
  ValidateAllowedKeys(node, keys, "laserdisc.channel.fm_beat", errors);
  if (!errors->empty()) {
    return false;
  }
  const std::string context = "laserdisc.channel.fm_beat";
  if (!node["level_db"]) {
    errors->push_back(context + " is missing required field: 'level_db'.");
    return false;
  }
  if (!ParseNumber(node, "level_db", context, &fm_beat->level_db, errors)) {
    return false;
  }
  if (node["carrier_black_hz"] &&
      !ParseNumber(node, "carrier_black_hz", context,
                   &fm_beat->carrier_black_hz, errors)) {
    return false;
  }
  fm_beat->enabled = true;
  return true;
}

// Parses the project-wide `channel:` block: impairments of the pressing that
// apply to every frame (the edge smear and the sub-sample timing offset).
bool ParseChannel(const YAML::Node& root, Project* project,
                  std::vector<std::string>* errors) {
  if (!root["channel"]) {
    return true;
  }

  const YAML::Node node = root["channel"];
  if (!node.IsMap()) {
    errors->push_back("Top-level 'channel' must be a map/object.");
    return false;
  }

  if (!ReportMovedKeys(node, {{"fm_beat", "laserdisc.channel.fm_beat"}},
                       "channel", errors)) {
    return false;
  }
  const std::set<std::string> keys = {"timing_offset_samples", "edge_smear"};
  ValidateAllowedKeys(node, keys, "channel", errors);
  if (!errors->empty()) {
    return false;
  }

  if (!node["timing_offset_samples"] && !node["edge_smear"]) {
    errors->push_back(
        "channel block must set at least one of: timing_offset_samples, "
        "edge_smear.");
    return false;
  }

  ChannelParameters& channel = project->channel;
  if (node["timing_offset_samples"] &&
      !ParseNumber(node, "timing_offset_samples", "channel",
                   &channel.timing_offset_samples, errors)) {
    return false;
  }
  if (node["edge_smear"] &&
      !ParseEdgeSmear(node["edge_smear"], &channel.edge_smear, errors)) {
    return false;
  }
  channel.enabled = true;
  return true;
}

// Parses a sequence of [x, y] number pairs into *points via make_point.
template <typename Point, typename MakePoint>
bool ParsePointPairs(const YAML::Node& node, const std::string& context,
                     const std::string& pair_names, MakePoint make_point,
                     std::vector<Point>* points,
                     std::vector<std::string>* errors) {
  if (!node.IsSequence()) {
    errors->push_back(context + " must be a sequence of [" + pair_names +
                      "] pairs.");
    return false;
  }
  points->clear();
  for (const YAML::Node& point_node : node) {
    if (!point_node.IsSequence() || point_node.size() != 2U) {
      errors->push_back(context + " entries must be [" + pair_names +
                        "] pairs.");
      return false;
    }
    try {
      points->push_back(
          make_point(point_node[0].as<double>(), point_node[1].as<double>()));
    } catch (const YAML::Exception&) {
      errors->push_back(context + " entries must be numeric pairs.");
      return false;
    }
  }
  return true;
}

// Parses `laserdisc.channel.group_delay:`, the IEC 60856 group-delay
// pre-distortion.
bool ParseGroupDelay(const YAML::Node& node, GroupDelayParameters* group_delay,
                     std::vector<std::string>* errors) {
  const std::string context = "laserdisc.channel.group_delay";
  if (!node.IsMap()) {
    errors->push_back(context + " must be a map/object.");
    return false;
  }
  const std::set<std::string> keys = {"preset", "points_ns", "scale"};
  ValidateAllowedKeys(node, keys, context, errors);
  if (!errors->empty()) {
    return false;
  }
  if (static_cast<bool>(node["preset"]) ==
      static_cast<bool>(node["points_ns"])) {
    errors->push_back(context + " must set exactly one of: preset, points_ns.");
    return false;
  }
  if (node["preset"]) {
    std::string preset;
    if (!ParseScalarString(node, "preset", context, &preset, errors)) {
      return false;
    }
    if (preset != "iec60856_nominal") {
      errors->push_back(context + ".preset has unrecognised value '" + preset +
                        "'. Expected: iec60856_nominal.");
      return false;
    }
    group_delay->preset = GroupDelayPreset::kIec60856Nominal;
  } else {
    group_delay->preset = GroupDelayPreset::kCustom;
    if (!ParsePointPairs(
            node["points_ns"], context + ".points_ns",
            "frequency_mhz, delay_ns",
            [](double mhz, double ns) { return GroupDelayPoint{mhz, ns}; },
            &group_delay->points_ns, errors)) {
      return false;
    }
  }
  if (node["scale"] &&
      !ParseNumber(node, "scale", context, &group_delay->scale, errors)) {
    return false;
  }
  group_delay->enabled = true;
  return true;
}

// Parses `laserdisc.channel.response_by_picture:`, the replay responses at
// two or more disc positions.
bool ParseResponseByPicture(const YAML::Node& node,
                            std::vector<ResponseByPictureEntry>* entries,
                            std::vector<std::string>* errors) {
  const std::string context = "laserdisc.channel.response_by_picture";
  if (!node.IsSequence()) {
    errors->push_back(context + " must be a list/sequence.");
    return false;
  }
  for (const YAML::Node& entry_node : node) {
    if (!entry_node.IsMap()) {
      errors->push_back(context + "[] must be a map/object.");
      return false;
    }
    ValidateAllowedKeys(entry_node, {"picture", "response_db"}, context + "[]",
                        errors);
    if (!errors->empty()) {
      return false;
    }
    if (!entry_node["picture"] || !entry_node["response_db"]) {
      errors->push_back(context + "[] must set both of: picture, response_db.");
      return false;
    }
    ResponseByPictureEntry entry;
    if (!ParseInteger(entry_node, "picture", context + "[]", &entry.picture,
                      errors)) {
      return false;
    }
    if (!ParseNoiseSpectrum(entry_node["response_db"],
                            context + "[].response_db", &entry.response_db,
                            errors)) {
      return false;
    }
    entries->push_back(std::move(entry));
  }
  return true;
}

// Parses `laserdisc.channel.field2_offset:`, the picture-domain
// misregistration of field 2.
bool ParseField2Offset(const YAML::Node& node, Field2OffsetParameters* offset,
                       std::vector<std::string>* errors) {
  const std::string context = "laserdisc.channel.field2_offset";
  if (!node.IsMap()) {
    errors->push_back(context + " must be a map/object.");
    return false;
  }
  ValidateAllowedKeys(node, {"x_samples", "y_lines"}, context, errors);
  if (!errors->empty()) {
    return false;
  }
  if (!node["x_samples"] && !node["y_lines"]) {
    errors->push_back(context +
                      " must set at least one of: x_samples, "
                      "y_lines.");
    return false;
  }
  if (node["x_samples"] &&
      !ParseNumber(node, "x_samples", context, &offset->x_samples, errors)) {
    return false;
  }
  if (node["y_lines"] &&
      !ParseNumber(node, "y_lines", context, &offset->y_lines, errors)) {
    return false;
  }
  offset->enabled = true;
  return true;
}

// Parses `laserdisc.channel:`, the LaserVision pressing and replay channel.
bool ParseLaserdiscChannel(const YAML::Node& node,
                           LaserdiscChannelParameters* channel,
                           std::vector<std::string>* errors) {
  const std::string context = "laserdisc.channel";
  if (!node.IsMap()) {
    errors->push_back(context + " must be a map/object.");
    return false;
  }
  const std::set<std::string> keys = {"fm_beat", "group_delay", "response_db",
                                      "response_by_picture", "field2_offset"};
  ValidateAllowedKeys(node, keys, context, errors);
  if (!errors->empty()) {
    return false;
  }
  if (node.size() == 0U) {
    errors->push_back(context +
                      " must set at least one of: fm_beat, group_delay, "
                      "response_db, response_by_picture, field2_offset.");
    return false;
  }
  if (node["fm_beat"] &&
      !ParseFmBeat(node["fm_beat"], &channel->fm_beat, errors)) {
    return false;
  }
  if (node["group_delay"] &&
      !ParseGroupDelay(node["group_delay"], &channel->group_delay, errors)) {
    return false;
  }
  if (node["response_db"] &&
      !ParseNoiseSpectrum(node["response_db"], context + ".response_db",
                          &channel->response_db, errors)) {
    return false;
  }
  if (node["response_by_picture"] &&
      !ParseResponseByPicture(node["response_by_picture"],
                              &channel->response_by_picture, errors)) {
    return false;
  }
  if (node["field2_offset"] &&
      !ParseField2Offset(node["field2_offset"], &channel->field2_offset,
                         errors)) {
    return false;
  }
  channel->enabled = true;
  return true;
}

// Parses the project-wide `laserdisc:` block: the disc format, the
// LaserVision signal options, the EFM digital audio selection and the
// LaserVision channel.
bool ParseLaserdisc(const YAML::Node& root, Project* project,
                    std::vector<std::string>* errors) {
  if (!root["laserdisc"]) {
    return true;
  }
  const YAML::Node node = root["laserdisc"];
  if (!node.IsMap()) {
    errors->push_back("Top-level 'laserdisc' must be a map/object.");
    return false;
  }
  const std::set<std::string> keys = {"disc_type", "pilot_burst", "vbi_burst",
                                      "efm_audio", "channel"};
  ValidateAllowedKeys(node, keys, "laserdisc", errors);
  if (!errors->empty()) {
    return false;
  }
  if (node.size() == 0U) {
    errors->push_back(
        "laserdisc block must set at least one of: disc_type, pilot_burst, "
        "vbi_burst, efm_audio, channel.");
    return false;
  }

  LaserdiscParameters& laserdisc = project->laserdisc;
  if (node["disc_type"] && !ParseScalarString(node, "disc_type", "laserdisc",
                                              &laserdisc.disc_type, errors)) {
    return false;
  }
  for (const auto& [key, flag] :
       {std::pair<const char*, bool*>{"pilot_burst", &laserdisc.pilot_burst},
        std::pair<const char*, bool*>{"vbi_burst", &laserdisc.vbi_burst}}) {
    if (node[key]) {
      try {
        *flag = node[key].as<bool>();
      } catch (const YAML::Exception&) {
        errors->push_back(std::string("laserdisc.") + key +
                          " must be true or false.");
        return false;
      }
    }
  }

  // Presence of laserdisc.efm_audio enables LaserDisc digital audio output
  // for the single channel pair it names.
  if (node["efm_audio"]) {
    const YAML::Node efm_node = node["efm_audio"];
    if (!efm_node.IsMap()) {
      errors->push_back("laserdisc.efm_audio must be a map/object.");
      return false;
    }
    ValidateAllowedKeys(efm_node, {"pair"}, "laserdisc.efm_audio", errors);
    if (!errors->empty()) {
      return false;
    }
    if (!efm_node["pair"]) {
      errors->push_back(
          "laserdisc.efm_audio is missing required field: 'pair'.");
      return false;
    }
    if (!ParseInteger(efm_node, "pair", "laserdisc.efm_audio",
                      &laserdisc.efm_audio.pair, errors)) {
      return false;
    }
    laserdisc.efm_audio.enabled = true;
  }

  if (node["channel"] &&
      !ParseLaserdiscChannel(node["channel"], &laserdisc.channel, errors)) {
    return false;
  }
  laserdisc.enabled = true;
  return true;
}

// Parses `chroma_error.line_jitter:`, the per-line Gaussian jitter.
bool ParseChromaJitter(const YAML::Node& node, ChromaJitterParameters* jitter,
                       std::vector<std::string>* errors) {
  if (!node.IsMap()) {
    errors->push_back("chroma_error.line_jitter must be a map/object.");
    return false;
  }
  const std::set<std::string> keys = {"phase_deg", "gain", "seed"};
  ValidateAllowedKeys(node, keys, "chroma_error.line_jitter", errors);
  if (!errors->empty()) {
    return false;
  }
  if (!node["phase_deg"] && !node["gain"]) {
    errors->push_back(
        "chroma_error.line_jitter must set at least one of: phase_deg, "
        "gain.");
    return false;
  }
  const std::string context = "chroma_error.line_jitter";
  if (node["phase_deg"] &&
      !ParseNumber(node, "phase_deg", context, &jitter->phase_deg, errors)) {
    return false;
  }
  if (node["gain"] &&
      !ParseNumber(node, "gain", context, &jitter->gain, errors)) {
    return false;
  }
  if (node["seed"]) {
    try {
      jitter->seed = node["seed"].as<int64_t>();
    } catch (const YAML::Exception&) {
      errors->push_back("chroma_error.line_jitter.seed must be an integer.");
      return false;
    }
    jitter->seed_specified = true;
  }
  jitter->enabled = true;
  return true;
}

// Parses `chroma_error.differential:`, the differential gain and phase.
bool ParseDifferential(const YAML::Node& node,
                       DifferentialParameters* differential,
                       std::vector<std::string>* errors) {
  const std::string context = "chroma_error.differential";
  if (!node.IsMap()) {
    errors->push_back(context + " must be a map/object.");
    return false;
  }
  ValidateAllowedKeys(node, {"gain", "phase_deg"}, context, errors);
  if (!errors->empty()) {
    return false;
  }
  if (!node["gain"] && !node["phase_deg"]) {
    errors->push_back(context + " must set at least one of: gain, phase_deg.");
    return false;
  }
  const auto make_point = [](double ire, double value) {
    return LevelCurvePoint{ire, value};
  };
  if (node["gain"] &&
      !ParsePointPairs(node["gain"], context + ".gain", "level_ire, gain",
                       make_point, &differential->gain, errors)) {
    return false;
  }
  if (node["phase_deg"] &&
      !ParsePointPairs(node["phase_deg"], context + ".phase_deg",
                       "level_ire, phase_deg", make_point,
                       &differential->phase_deg, errors)) {
    return false;
  }
  differential->enabled = true;
  return true;
}

// Parses `chroma_error.crosstalk:`, the U/V crosstalk.
bool ParseCrosstalk(const YAML::Node& node, CrosstalkParameters* crosstalk,
                    std::vector<std::string>* errors) {
  const std::string context = "chroma_error.crosstalk";
  if (!node.IsMap()) {
    errors->push_back(context + " must be a map/object.");
    return false;
  }
  ValidateAllowedKeys(node, {"v_to_u", "u_to_v", "alternate_with_switch"},
                      context, errors);
  if (!errors->empty()) {
    return false;
  }
  if (!node["v_to_u"] && !node["u_to_v"]) {
    errors->push_back(context + " must set at least one of: v_to_u, u_to_v.");
    return false;
  }
  if (node["v_to_u"] &&
      !ParseNumber(node, "v_to_u", context, &crosstalk->v_to_u, errors)) {
    return false;
  }
  if (node["u_to_v"] &&
      !ParseNumber(node, "u_to_v", context, &crosstalk->u_to_v, errors)) {
    return false;
  }
  if (node["alternate_with_switch"]) {
    try {
      crosstalk->alternate_with_switch =
          node["alternate_with_switch"].as<bool>();
    } catch (const YAML::Exception&) {
      errors->push_back(context +
                        ".alternate_with_switch must be true or false.");
      return false;
    }
  }
  crosstalk->enabled = true;
  return true;
}

// Parses the project-wide `chroma_error:` block: the chroma gain and phase
// error of this capture's player.
bool ParseChromaError(const YAML::Node& root, Project* project,
                      std::vector<std::string>* errors) {
  if (!root["chroma_error"]) {
    return true;
  }

  const YAML::Node node = root["chroma_error"];
  if (!node.IsMap()) {
    errors->push_back("Top-level 'chroma_error' must be a map/object.");
    return false;
  }

  const std::set<std::string> keys = {"gain",        "phase_deg",
                                      "line_jitter", "differential",
                                      "crosstalk",   "include_burst"};
  ValidateAllowedKeys(node, keys, "chroma_error", errors);
  if (!errors->empty()) {
    return false;
  }

  if (!node["gain"] && !node["phase_deg"] && !node["line_jitter"] &&
      !node["differential"] && !node["crosstalk"]) {
    errors->push_back(
        "chroma_error block must set at least one of: gain, phase_deg, "
        "line_jitter, differential, crosstalk.");
    return false;
  }

  ChromaErrorParameters& chroma_error = project->chroma_error;
  if (node["gain"] &&
      !ParseNumber(node, "gain", "chroma_error", &chroma_error.gain, errors)) {
    return false;
  }
  if (node["phase_deg"] && !ParseNumber(node, "phase_deg", "chroma_error",
                                        &chroma_error.phase_deg, errors)) {
    return false;
  }
  if (node["line_jitter"] &&
      !ParseChromaJitter(node["line_jitter"], &chroma_error.line_jitter,
                         errors)) {
    return false;
  }
  if (node["differential"] &&
      !ParseDifferential(node["differential"], &chroma_error.differential,
                         errors)) {
    return false;
  }
  if (node["crosstalk"] &&
      !ParseCrosstalk(node["crosstalk"], &chroma_error.crosstalk, errors)) {
    return false;
  }
  if (node["include_burst"]) {
    try {
      chroma_error.include_burst = node["include_burst"].as<bool>();
    } catch (const YAML::Exception&) {
      errors->push_back("chroma_error.include_burst must be true or false.");
      return false;
    }
  }
  chroma_error.enabled = true;
  return true;
}

// Parses the project-wide `line_injections:` block: the VITS test-signal set
// and its placement policy.
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

  if (!ReportMovedKeys(node, {{"disc_type", "laserdisc.disc_type"}},
                       "line_injections", errors)) {
    return false;
  }
  const std::set<std::string> keys = {"placement", "vits"};
  ValidateAllowedKeys(node, keys, "line_injections", errors);
  if (!errors->empty()) {
    return false;
  }

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
    audio.frequency_specified = true;
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

// Reads which content kind a channel pair carries. `file` and `video_audio`
// each replace the `left`/`right` tones, so a pair may name at most one of the
// three. Appends to errors and returns false on a structural error.
bool ParseAudioPairContent(const YAML::Node& pair_node, AudioChannelPair* out,
                           std::vector<std::string>* errors) {
  const bool has_tones = pair_node["left"] || pair_node["right"];
  const bool has_file = static_cast<bool>(pair_node["file"]);
  const bool has_video = static_cast<bool>(pair_node["video_audio"]);
  if (static_cast<int>(has_tones) + static_cast<int>(has_file) +
          static_cast<int>(has_video) >
      1) {
    errors->push_back(
        "section.audio.channel_pairs[] must use only one of 'left'/'right' "
        "tones, 'file' or 'video_audio'.");
    return false;
  }

  if (has_file) {
    const YAML::Node file_node = pair_node["file"];
    const std::string context = "section.audio.channel_pairs[].file";
    if (!file_node.IsMap()) {
      errors->push_back(context + " must be a map/object.");
      return false;
    }
    ValidateAllowedKeys(file_node, {"path", "repeat"}, context, errors);
    if (!errors->empty()) {
      return false;
    }
    if (!file_node["path"]) {
      errors->push_back(context + " is missing required field: 'path'.");
      return false;
    }
    out->content = AudioPairContent::kFile;
    out->file.path = file_node["path"].as<std::string>("");
    if (file_node["repeat"]) {
      out->file.repeat = file_node["repeat"].as<bool>();
    }
    return true;
  }

  if (has_video) {
    const YAML::Node video_node = pair_node["video_audio"];
    const std::string context = "section.audio.channel_pairs[].video_audio";
    if (!video_node.IsMap()) {
      errors->push_back(context + " must be a map/object.");
      return false;
    }
    ValidateAllowedKeys(video_node, {"stream"}, context, errors);
    if (!errors->empty()) {
      return false;
    }
    out->content = AudioPairContent::kVideo;
    if (video_node["stream"]) {
      out->video_audio.stream = video_node["stream"].as<int>();
    }
  }
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
        "project",   "cvbs_presets", "output",       "line_injections",
        "laserdisc", "channel",      "chroma_error", "sections"};
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
    if (!ReportMovedKeys(
            presets,
            {{"pal_laserdisc_pilot_burst", "laserdisc.pilot_burst"},
             {"ntsc_laserdisc_vbi_burst", "laserdisc.vbi_burst"}},
            "cvbs_presets", &result.errors)) {
      return result;
    }
    const std::set<std::string> preset_keys = {
        "video_standard_preset", "sample_encoding_preset",
        "signal_state_preset", "ntsc_black_setup_ire",
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
    if (presets["ntsc_black_setup_ire"]) {
      result.project.cvbs_presets.ntsc_black_setup_ire =
          presets["ntsc_black_setup_ire"].as<double>();
      result.project.cvbs_presets.ntsc_black_setup_ire_specified = true;
    }
    result.project.cvbs_presets.secam_field_identification =
        presets["secam_field_identification"].as<bool>(
            result.project.cvbs_presets.secam_field_identification);

    const YAML::Node output = root["output"];
    if (!ReportMovedKeys(output, {{"efm_audio", "laserdisc.efm_audio"}},
                         "output", &result.errors)) {
      return result;
    }
    const std::set<std::string> output_keys = {"video_path", "signal_type",
                                               "truth"};
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
    if (output["truth"]) {
      try {
        result.project.output.truth = output["truth"].as<bool>();
      } catch (const YAML::Exception&) {
        result.errors.push_back("output.truth must be true or false.");
        return result;
      }
    }

    if (!ParseProjectLineInjections(root, &result.project, &result.errors)) {
      return result;
    }

    if (!ParseLaserdisc(root, &result.project, &result.errors)) {
      return result;
    }

    if (!ParseChannel(root, &result.project, &result.errors)) {
      return result;
    }

    if (!ParseChromaError(root, &result.project, &result.errors)) {
      return result;
    }

    for (const YAML::Node& section_node : root["sections"]) {
      if (!section_node.IsMap()) {
        result.errors.push_back("Each section entry must be a map/object.");
        return result;
      }

      if (!ReportMovedKeys(section_node,
                           {{"section_type", "the section's laserdisc.area"}},
                           "section", &result.errors)) {
        return result;
      }
      const std::set<std::string> section_keys = {"name",
                                                  "type",
                                                  "laserdisc",
                                                  "duration_frames",
                                                  "duration_repeat",
                                                  "line_injections",
                                                  "source",
                                                  "start_frame",
                                                  "end_frame",
                                                  "cadence",
                                                  "luma_bandwidth",
                                                  "picture_response",
                                                  "horizontal_resample",
                                                  "chroma_line_sign",
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
      if (!ParseSectionLaserdisc(section_node, &section, &result.errors)) {
        return result;
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
        const std::set<std::string> noise_keys = {
            "noise_db", "noise_spread_db", "noise_seed",
            "spectrum", "spread_spectrum", "shared"};
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
        if (!noise_node["noise_db"] && noise_node["spectrum"]) {
          result.errors.push_back(
              "section noise validation error: spectrum requires noise_db to "
              "be specified (use noise.shared.spectrum for pressing noise).");
          return result;
        }
        if (!noise_node["noise_db"] && noise_node["spread_spectrum"]) {
          result.errors.push_back(
              "section noise validation error: spread_spectrum requires "
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
        if (noise_node["spectrum"] &&
            !ParseNoiseSpectrum(noise_node["spectrum"],
                                "section field 'noise.spectrum'",
                                &section.noise.spectrum, &result.errors)) {
          return result;
        }
        if (noise_node["spread_spectrum"] &&
            !ParseNoiseSpectrum(noise_node["spread_spectrum"],
                                "section field 'noise.spread_spectrum'",
                                &section.noise.spread_spectrum,
                                &result.errors)) {
          return result;
        }
        if (noise_node["shared"] &&
            !ParseSharedNoise(noise_node["shared"], &section.noise.shared,
                              &result.errors)) {
          return result;
        }
      }

      if (section_node["cadence"] &&
          !ParseCadence(section_node["cadence"], &section.cadence,
                        &result.errors)) {
        return result;
      }

      if (section_node["luma_bandwidth"] &&
          !ParseLumaBandwidth(section_node["luma_bandwidth"],
                              &section.luma_bandwidth, &result.errors)) {
        return result;
      }

      if (section_node["picture_response"] &&
          !ParsePictureResponse(section_node["picture_response"],
                                &section.picture_response, &result.errors)) {
        return result;
      }

      if (section_node["horizontal_resample"] &&
          !ParseHorizontalResample(section_node, &section.horizontal_resample,
                                   &result.errors)) {
        return result;
      }

      if (section_node["chroma_line_sign"] &&
          !ParseChromaLineSign(section_node["chroma_line_sign"],
                               &section.chroma_line_sign, &result.errors)) {
        return result;
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
        section.dropouts.block_present = true;

        if (dropouts_node["random"] &&
            !ParseDropoutType(dropouts_node["random"], "random",
                              &section.dropouts.random, &result.errors)) {
          return result;
        }
        if (dropouts_node["scratch"] &&
            !ParseDropoutType(dropouts_node["scratch"], "scratch",
                              &section.dropouts.scratch, &result.errors)) {
          return result;
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
          const std::set<std::string> pair_keys = {
              "pair", "description", "left", "right", "file", "video_audio"};
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
          if (!ParseAudioPairContent(pair_node, &channel_pair,
                                     &result.errors)) {
            return result;
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
