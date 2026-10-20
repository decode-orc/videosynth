/*
 * File:        line_injection_presenter.cpp
 * Module:      gui
 * Purpose:     Widget-free catalogues and helpers for the line-injection
 *              editors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "line_injection_presenter.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <utility>

#include "videosynth/vits_definition.h"
#include "videosynth/vits_definition_provider.h"

namespace videosynth::gui {

std::vector<std::string> AvailableInjectionTypes() {
  // VITS moved to the project-wide line_injections block; section-level
  // injections carry only laserdisc biphase codes.
  return {"laserdisc"};
}

std::vector<std::string> AvailableVitsTypes(Standard standard) {
  const VitsDefinitionProvider provider;
  return provider.ListVitsTypes(standard);
}

int RecommendedVitsLine(Standard standard, const std::string& vits_type) {
  const VitsDefinitionProvider provider;
  VitsDefinition definition;
  std::string error;
  if (!provider.TryGetDefinition(standard, vits_type, &definition, &error)) {
    return 0;
  }
  return definition.recommended_frame_line;
}

std::vector<std::string> AvailableDiscTypes() { return {"CAV", "CLV"}; }

std::vector<std::string> AvailableLaserdiscCodeTypes(DiscType disc_type,
                                                     SectionType section_type,
                                                     Standard standard) {
  const bool system_m =
      standard == Standard::kNtsc || standard == Standard::kPalM;

  std::vector<std::string> code_types;
  for (const std::string& code_type : AllLaserdiscCodeTypes()) {
    if (disc_type == DiscType::kCAV && !IsValidCavCodeType(code_type)) {
      continue;
    }
    if (disc_type == DiscType::kCLV && !IsValidClvCodeType(code_type)) {
      continue;
    }
    if (!IsCodeTypeValidForSectionType(code_type, section_type)) {
      continue;
    }
    if (IsSystemMOnlyCodeType(code_type) && !system_m) {
      continue;
    }
    code_types.push_back(code_type);
  }
  return code_types;
}

std::vector<std::string> CommonLaserdiscCodeTypes(
    DiscType disc_type, const std::vector<SectionType>& section_types,
    Standard standard) {
  if (section_types.empty()) {
    return {};
  }
  std::vector<std::string> common =
      AvailableLaserdiscCodeTypes(disc_type, section_types.front(), standard);
  for (std::size_t i = 1; i < section_types.size() && !common.empty(); ++i) {
    const std::vector<std::string> next =
        AvailableLaserdiscCodeTypes(disc_type, section_types[i], standard);
    common.erase(std::remove_if(common.begin(), common.end(),
                                [&next](const std::string& code_type) {
                                  return std::find(next.begin(), next.end(),
                                                   code_type) == next.end();
                                }),
                 common.end());
  }
  return common;
}

std::vector<std::string> RecommendedLaserdiscCodeTypes(DiscType disc_type,
                                                       SectionType section_type,
                                                       Standard standard) {
  const bool system_m =
      standard == Standard::kNtsc || standard == Standard::kPalM;

  std::vector<std::string> recommended;
  switch (section_type) {
    case SectionType::kLeadIn:
      recommended.push_back("lead_in");
      break;
    case SectionType::kLeadOut:
      recommended.push_back("lead_out");
      break;
    case SectionType::kProgrammeArea:
      if (disc_type == DiscType::kCAV) {
        recommended.push_back("picture_number");
        recommended.push_back("chapter_number");
        if (system_m) {
          recommended.push_back("fm_picture_number");
        }
      } else if (disc_type == DiscType::kCLV) {
        recommended.push_back("programme_time_code");
        recommended.push_back("clv_code");
        recommended.push_back("chapter_number");
        if (system_m) {
          recommended.push_back("fm_programme_time");
        }
      }
      break;
    case SectionType::kUnknown:
    default:
      return {};
  }
  if (system_m) {
    recommended.push_back("fm_white_flag");
  }

  // Return the intersection with the offered catalogue, in catalogue order, so
  // the pre-ticked set is always a valid subset the editor can render.
  std::vector<std::string> result;
  for (const std::string& code_type :
       AvailableLaserdiscCodeTypes(disc_type, section_type, standard)) {
    if (std::find(recommended.begin(), recommended.end(), code_type) !=
        recommended.end()) {
      result.push_back(code_type);
    }
  }
  return result;
}

bool VitsHasFixedLine(Standard standard, const std::string& vits_type) {
  return RecommendedVitsLine(standard, vits_type) > 0;
}

std::vector<int> DefaultVitsLines(Standard standard,
                                  const std::string& vits_type) {
  const int recommended = RecommendedVitsLine(standard, vits_type);
  if (recommended > 0) {
    return {recommended};
  }
  // The virs colour reference has no fixed placement line; it is conventionally
  // carried in both fields (IEC 60857 §9.1.3), matching the built-in template.
  if (vits_type == "virs") {
    return {19, 282};
  }
  return {};
}

std::vector<VitsInjection> LaserdiscVitsSet(Standard standard) {
  std::vector<VitsInjection> set;
  if (standard == Standard::kPal) {
    // IEC 60856 §9.1.3: VITS on lines 19, 20, 332, 333 (the field-1 pair
    // mirrored to field 2). uk-national is the line-19 UK ITS; vits20 the
    // line-20 ITS-2.
    set.push_back(VitsInjection{"uk-national", {19, 332}});
    set.push_back(VitsInjection{"vits20", {20, 333}});
  } else if (standard == Standard::kNtsc || standard == Standard::kPalM) {
    // IEC 60857 §9.1.3 (VIRS on 19/282, required for colour) and §9.1.4
    // (composite ITS on line 20, combination ITS on line 283), per NTC-7.
    set.push_back(VitsInjection{"virs", {19, 282}});
    set.push_back(VitsInjection{"ntc7-composite", {20}});
    set.push_back(VitsInjection{"ntc7-combination", {283}});
  }
  return set;
}

ProjectLineInjections ReconcileVitsForStandard(ProjectLineInjections injections,
                                               Standard new_standard) {
  // There is no SECAM LaserVision format: drop the disc format and fall back
  // from laserdisc to standard VITS placement.
  if (new_standard == Standard::kSecam) {
    injections.disc_type.clear();
    if (injections.placement == VitsPlacement::kLaserdisc) {
      injections.placement = VitsPlacement::kStandard;
      injections.vits.clear();
    }
  }
  if (injections.placement == VitsPlacement::kLaserdisc) {
    injections.vits = LaserdiscVitsSet(new_standard);
    return injections;
  }

  const std::vector<std::string> available = AvailableVitsTypes(new_standard);
  std::vector<VitsInjection> kept;
  for (VitsInjection& vits : injections.vits) {
    if (std::find(available.begin(), available.end(), vits.vits_type) !=
        available.end()) {
      kept.push_back(std::move(vits));
    }
  }
  injections.vits = std::move(kept);
  return injections;
}

bool CodeTypeUsesStartValue(const std::string& code_type) {
  // The validator range-checks start_value for exactly these two counters.
  return code_type == "picture_number" || code_type == "fm_picture_number";
}

bool CodeTypeUsesChapter(const std::string& code_type) {
  return code_type == "chapter_number";
}

bool CodeTypeUsesProgrammeStatus(const std::string& code_type) {
  return code_type == "programme_status";
}

bool CodeTypeUsesUsersCode(const std::string& code_type) {
  return code_type == "users_code";
}

std::string CodeTypeHelp(const std::string& code_type) {
  if (code_type == "lead_in") {
    return "Marks the disc lead-in area. Carries no value.";
  }
  if (code_type == "lead_out") {
    return "Marks the disc lead-out area. Carries no value.";
  }
  if (code_type == "picture_number") {
    return "CAV frame number, incrementing once per frame. Leave the value "
           "blank to continue numbering from the previous section; set "
           "start_value to re-anchor this section's first frame.";
  }
  if (code_type == "fm_picture_number") {
    return "40-bit FM frame number (System M), incrementing once per frame. "
           "Leave blank to continue from the previous section; set start_value "
           "to re-anchor this section's first frame.";
  }
  if (code_type == "picture_stop") {
    return "Flags a still/stop frame in this section. Carries no value.";
  }
  if (code_type == "chapter_number") {
    return "Chapter number (0–79) stamped on this section's frames. Leave "
           "the value blank to continue the previous section's chapter; set "
           "a chapter to start a new one here.";
  }
  if (code_type == "programme_status") {
    return "24-bit programme status word, entered as hex (e.g. 0x8DC000).";
  }
  if (code_type == "users_code") {
    return "24-bit user code, entered as hex (e.g. 0x80D000).";
  }
  if (code_type == "clv_code") {
    return "CLV identification code. Carries no value.";
  }
  if (code_type == "clv_picture_number") {
    return "CLV picture number (seconds:frame), derived automatically from "
           "elapsed disc time — it progresses across all sections on its own, "
           "so there is no value to set.";
  }
  if (code_type == "programme_time_code") {
    return "CLV elapsed time (hours:minutes:seconds), counted automatically "
           "from disc start across all sections. No value to set.";
  }
  if (code_type == "fm_programme_time") {
    return "40-bit FM elapsed time (System M), counted automatically across "
           "all sections. No value to set.";
  }
  if (code_type == "fm_white_flag") {
    return "White flag on the picture-number line (System M). Carries no "
           "value.";
  }
  return {};
}

bool ParseTargetLines(const std::string& text, std::vector<int>* out_lines) {
  out_lines->clear();

  std::size_t position = 0;
  while (position < text.size()) {
    // Skip separators (commas and whitespace).
    while (position < text.size() &&
           (text[position] == ',' ||
            std::isspace(static_cast<unsigned char>(text[position])) != 0)) {
      ++position;
    }
    if (position >= text.size()) {
      break;
    }

    int value = 0;
    bool any_digit = false;
    while (position < text.size() &&
           std::isdigit(static_cast<unsigned char>(text[position])) != 0) {
      value = value * 10 + (text[position] - '0');
      any_digit = true;
      ++position;
    }

    const bool at_separator =
        position >= text.size() || text[position] == ',' ||
        std::isspace(static_cast<unsigned char>(text[position])) != 0;
    if (!any_digit || !at_separator) {
      return false;
    }
    out_lines->push_back(value);
  }
  return true;
}

std::string FormatTargetLines(const std::vector<int>& lines) {
  std::string text;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    if (i > 0) {
      text += ", ";
    }
    text += std::to_string(lines[i]);
  }
  return text;
}

int TotalDiscFrames(const Project& project) {
  int total = 0;
  for (const Section& section : project.sections) {
    total += section.duration_frames;
  }
  return total;
}

}  // namespace videosynth::gui
