/*
 * File:        efm_track_layout.cpp
 * Module:      efm_track_layout
 * Purpose:     Maps a project's output section layout onto the subcode track
 *              table of the LaserDisc digital audio (EFM) stream.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "videosynth/efm_track_layout.h"

#include <algorithm>
#include <cstdint>

#include "videosynth/timing_constants.h"

namespace videosynth {

namespace {

// Disc area an output section belongs to (IEC 60908-1999, clause 12). Sections
// with no declared type carry programme audio, so they become tracks.
efm::SubcodeArea AreaForSection(const Section* section) {
  if (section == nullptr) {
    return efm::SubcodeArea::kProgramme;
  }
  switch (section->section_type) {
    case SectionType::kLeadIn:
      return efm::SubcodeArea::kLeadIn;
    case SectionType::kLeadOut:
      return efm::SubcodeArea::kLeadOut;
    default:
      return efm::SubcodeArea::kProgramme;
  }
}

std::string SectionLabel(const Section* section) {
  if (section == nullptr || section->name.empty()) {
    return "(unnamed section)";
  }
  return "'" + section->name + "'";
}

// One contiguous run of output frames sharing a section, before it is placed on
// the subcode timeline.
struct SectionRun {
  efm::SubcodeArea area = efm::SubcodeArea::kProgramme;
  const Section* section = nullptr;
  std::size_t start_sample = 0;
};

// Groups the output frames into runs. A run ends where the section changes;
// adjacent lead-in runs, and adjacent lead-out runs, are merged so each area
// contributes one entry while every programme-area run becomes its own track.
std::vector<SectionRun> BuildRuns(
    Standard standard, const std::vector<const Section*>& output_frame_sections,
    std::size_t* total_samples) {
  std::vector<SectionRun> runs;
  std::size_t sample = 0;
  const Section* previous = nullptr;
  for (std::size_t k = 0; k < output_frame_sections.size(); ++k) {
    const Section* section = output_frame_sections[k];
    const efm::SubcodeArea area = AreaForSection(section);
    const bool continues_run =
        !runs.empty() &&
        (section == previous ||
         (area != efm::SubcodeArea::kProgramme && area == runs.back().area));
    if (!continues_run) {
      runs.push_back(SectionRun{area, section, sample});
    }
    previous = section;
    sample += static_cast<std::size_t>(
        EfmAudioSamplesForFrame(standard, static_cast<std::int64_t>(k)));
  }
  *total_samples = sample;
  return runs;
}

}  // namespace

bool BuildEfmTrackLayout(
    Standard standard, const std::vector<const Section*>& output_frame_sections,
    EfmTrackLayout* layout, std::vector<std::string>* errors) {
  if (layout == nullptr || errors == nullptr) {
    return false;
  }
  *layout = EfmTrackLayout{};

  if (!StandardHasLaserdiscDigitalAudio(standard)) {
    errors->push_back(
        "EFM audio is only defined for the PAL, NTSC and PAL-M video "
        "standards.");
    return false;
  }
  if (output_frame_sections.empty()) {
    errors->push_back("EFM audio requires at least one output frame.");
    return false;
  }
  // PAL-M discs are 525-line LaserVision discs (IEC 60857), identified as
  // such in the lead-in TOC.
  layout->table.video_system = standard == Standard::kPal
                                   ? efm::VideoSystem::kPal
                                   : efm::VideoSystem::kNtsc;

  std::size_t total_samples = 0;
  const std::vector<SectionRun> runs =
      BuildRuns(standard, output_frame_sections, &total_samples);

  // Subcode sections run at 75 Hz (IEC 60908-1999, 17.3); a trailing partial
  // section is not covered by the table and carries no P or Q data.
  const std::size_t total_sections =
      total_samples / kEfmSamplesPerSubcodeSection;

  // Boundaries land on the nearest subcode section, so a track starts within
  // 1/150 s of the video frame that carries the section change.
  std::vector<std::size_t> starts(runs.size(), 0);
  for (std::size_t index = 1; index < runs.size(); ++index) {
    starts[index] =
        (runs[index].start_sample + (kEfmSamplesPerSubcodeSection / 2)) /
        kEfmSamplesPerSubcodeSection;
    if (starts[index] <= starts[index - 1]) {
      errors->push_back("EFM audio section " +
                        SectionLabel(runs[index].section) +
                        " is too short to occupy a subcode section (1/75 s).");
      return false;
    }
  }
  if (starts.back() >= total_sections) {
    errors->push_back("EFM audio section " + SectionLabel(runs.back().section) +
                      " is too short to occupy a subcode section (1/75 s).");
    return false;
  }

  std::size_t track_number = 0;
  std::size_t previous_area = 0;
  for (std::size_t index = 0; index < runs.size(); ++index) {
    const SectionRun& run = runs[index];
    // SubcodeGenerator requires the areas of IEC 60908-1999, clause 12 in disc
    // order: lead-in, then programme area, then lead-out.
    const auto area_order = static_cast<std::size_t>(run.area);
    if (area_order < previous_area) {
      errors->push_back("EFM audio section " + SectionLabel(run.section) +
                        " is out of disc order: lead-in sections must precede "
                        "programme sections, which must precede lead-out "
                        "sections.");
      return false;
    }
    previous_area = area_order;

    efm::TrackTableEntry entry;
    entry.area = run.area;
    entry.start_section = starts[index];
    entry.section_count =
        (index + 1 < runs.size() ? starts[index + 1] : total_sections) -
        starts[index];
    if (run.area == efm::SubcodeArea::kProgramme) {
      ++track_number;
      if (track_number > efm::kMaxTrackNumber) {
        errors->push_back(
            "EFM audio supports at most " +
            std::to_string(static_cast<int>(efm::kMaxTrackNumber)) +
            " tracks; the project has more programme sections.");
        return false;
      }
      entry.track_number = static_cast<std::uint8_t>(track_number);
      if (track_number == 1) {
        // IEC 60908-1999, 17.5.1: the first track is preceded by a pause of 2 s
        // to 3 s, encoded as digital silence.
        const std::size_t pause_sections =
            std::min(efm::kTrackOnePauseSections, entry.section_count);
        layout->pause_start_sample =
            entry.start_section * kEfmSamplesPerSubcodeSection;
        layout->pause_end_sample =
            layout->pause_start_sample +
            (pause_sections * kEfmSamplesPerSubcodeSection);
      }
    }
    layout->table.entries.push_back(entry);
  }

  return true;
}

}  // namespace videosynth
