/*
 * File:        project_templates.cpp
 * Module:      gui
 * Purpose:     Built-in project and section templates for File > New and the
 *              section list's typed add menu
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "project_templates.h"

#include <string>

namespace videosynth::gui {

namespace {

// IEC 60856/60857 minimum durations at nominal 1.6 µm track pitch
// (lead-in ≥ 1.5 mm, lead-out ≥ 2 mm); values match ProjectValidator.
constexpr int kLeadInMinFrames = 938;
constexpr int kLeadOutMinFrames = 1250;
constexpr int kDefaultProgrammeFrames = 500;

bool IsSystemM(Standard standard) {
  return standard == Standard::kNtsc || standard == Standard::kPalM;
}

Section MakeLaserdiscSectionBase(Standard standard, SectionType section_type,
                                 const std::string& name, int duration_frames) {
  Section section;
  section.name = name;
  section.type = "progressive";
  section.section_type = section_type;
  // Bundled colour-bar default so the section previews immediately; the user
  // picks the real file in the section editor.
  section.source = DefaultBundledSource(standard);
  section.duration_frames = duration_frames;

  // The disc format (CAV/CLV) and the VITS set are project-wide; a section
  // carries only its laserdisc code injection.
  Section::LineInjection laserdisc;
  laserdisc.type = "laserdisc";
  section.line_injections.push_back(laserdisc);
  return section;
}

void AppendCode(Section* section, const std::string& code_type) {
  Section::LineInjectionCode code;
  code.code_type = code_type;
  section->line_injections.front().codes.push_back(code);
}

}  // namespace

std::string BundledRaster(Standard standard) {
  return Is625LineStandard(standard) ? "720x576"
                                     : "720x486";  // System-M is 525.
}

// Bundled colour-bar EXR shipped for the standard's active raster, referenced
// through the {bundled} logical asset root so a fresh project previews
// immediately regardless of install location.
std::string DefaultBundledSource(Standard standard) {
  return "{bundled}/exr/" + BundledRaster(standard) + "/75_BARS.exr";
}

int RemapBundledDefaultSources(Project* project, Standard standard) {
  if (project == nullptr) {
    return 0;
  }
  const std::string target = DefaultBundledSource(standard);
  // A source counts as a remappable default only if it is the bundled
  // colour-bar for one of the supported rasters; anything else is a source the
  // user selected and must be preserved.
  const auto is_bundled_default = [](const std::string& source) {
    return source == DefaultBundledSource(Standard::kPal) ||
           source == DefaultBundledSource(Standard::kNtsc);
  };
  int changed = 0;
  for (Section& section : project->sections) {
    if (section.source != target && is_bundled_default(section.source)) {
      section.source = target;
      ++changed;
    }
  }
  return changed;
}

ProjectLineInjections MakeLaserdiscLineInjections(Standard standard) {
  ProjectLineInjections li;
  li.disc_type = "CAV";
  // NTSC/PAL-M laserdisc discs require a virs VITS colour reference
  // (IEC 60857 §9.1.3); lines 19/282 follow the NTSC CAV disc fixtures.
  if (IsSystemM(standard)) {
    VitsInjection virs;
    virs.vits_type = "virs";
    virs.target_lines = {19, 282};
    li.vits.push_back(std::move(virs));
  }
  return li;
}

Project MakeDefaultProject(Standard standard) {
  const Standard resolved =
      standard == Standard::kUnknown ? Standard::kPal : standard;

  Project project;
  project.name = "New Project";
  project.version = "1.0";
  project.cvbs_presets.video_standard_preset = resolved;
  project.cvbs_presets.sample_encoding_preset = "CVBS_U10_4FSC";
  project.cvbs_presets.signal_state_preset =
      GeneratedSignalStatePreset(resolved);
  project.output.video_path = "output/new_project.cvbs";
  project.output.metadata_path = "output/new_project.meta";

  project.sections.push_back(MakeProgressiveSectionTemplate(1, resolved));
  return project;
}

Project MakeDefaultPalProject() { return MakeDefaultProject(Standard::kPal); }

Section MakeProgressiveSectionTemplate(int ordinal, Standard standard) {
  Section section;
  section.name = "Section " + std::to_string(ordinal);
  section.type = "progressive";
  // Bundled colour-bar default for the standard's active raster so a fresh
  // section previews immediately instead of pointing at a non-existent file.
  section.source = DefaultBundledSource(standard);
  section.duration_frames = 25;
  return section;
}

std::string FilmBundledSource() {
  return "{bundled}/mkv/" + BundledRaster(Standard::kNtsc) +
         "/FILM_COUNTER_24P.mkv";
}

Section MakeFilmSectionTemplate(int ordinal, Standard standard) {
  Section section;
  section.name = "Film " + std::to_string(ordinal);
  section.type = "progressive";
  // 2:3 pulldown exists only on 525-line standards, so the film clip always
  // uses the 525-line raster.
  section.source = IsSystemM(standard) ? FilmBundledSource()
                                       : DefaultBundledSource(standard);
  section.duration_frames_all = true;
  section.duration_frames_repeat = 1;
  section.cadence.pattern = CadencePattern::kPulldown23;
  section.cadence.phase = CadencePhase::kA;
  section.cadence.phase_specified = true;
  return section;
}

Section MakeLaserdiscLeadInSectionTemplate(Standard standard) {
  Section section = MakeLaserdiscSectionBase(standard, SectionType::kLeadIn,
                                             "LeadIn", kLeadInMinFrames);
  AppendCode(&section, "lead_in");
  if (IsSystemM(standard)) {
    AppendCode(&section, "fm_white_flag");
  }
  return section;
}

Section MakeLaserdiscProgrammeSectionTemplate(Standard standard) {
  Section section =
      MakeLaserdiscSectionBase(standard, SectionType::kProgrammeArea,
                               "Programme", kDefaultProgrammeFrames);
  Section::LineInjectionCode picture_number;
  picture_number.code_type = "picture_number";
  picture_number.start_value = 1;
  picture_number.start_value_specified = true;
  section.line_injections.front().codes.push_back(picture_number);

  Section::LineInjectionCode chapter;
  chapter.code_type = "chapter_number";
  chapter.chapter = 0;
  chapter.chapter_specified = true;
  section.line_injections.front().codes.push_back(chapter);

  if (IsSystemM(standard)) {
    AppendCode(&section, "fm_picture_number");
    AppendCode(&section, "fm_white_flag");
  }
  return section;
}

Section MakeLaserdiscLeadOutSectionTemplate(Standard standard) {
  Section section = MakeLaserdiscSectionBase(standard, SectionType::kLeadOut,
                                             "LeadOut", kLeadOutMinFrames);
  AppendCode(&section, "lead_out");
  if (IsSystemM(standard)) {
    AppendCode(&section, "fm_white_flag");
  }
  return section;
}

Section MakeDuplicateSection(const Section& section,
                             const std::vector<Section>& existing_sections) {
  const auto name_taken = [&existing_sections](const std::string& name) {
    for (const Section& existing : existing_sections) {
      if (existing.name == name) {
        return true;
      }
    }
    return false;
  };

  Section duplicate = section;
  std::string candidate = section.name + " (copy)";
  for (int suffix = 2; name_taken(candidate); ++suffix) {
    candidate = section.name + " (copy " + std::to_string(suffix) + ")";
  }
  duplicate.name = candidate;
  return duplicate;
}

}  // namespace videosynth::gui
