/*
 * File:        efm_data_layout.cpp
 * Module:      efm_data_layout
 * Purpose:     Places the sectors of an EFM data image on the subcode
 *              timeline by the AIV picture rule and decides which frames
 *              carry the EFM signal.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "videosynth/efm_data_layout.h"

#include <limits>

#include "videosynth/efm/audio_frame_assembler.h"
#include "videosynth/efm/circ_encoder.h"
#include "videosynth/efm/subcode_generator.h"
#include "videosynth/timing_constants.h"

namespace videosynth {

namespace {

constexpr std::size_t kFramesPerSection = efm::kFramesPerSubcodeSection;
constexpr std::int64_t kSectorsPerSecond = efm::kSubcodeSectionsPerSecond;

// IEC 60856:1986 Amd 2 (25 Hz) / IEC 60857:1986 Amd 2 (30000/1001 Hz): the
// picture rate as an exact fraction.
struct FrameRate {
  std::int64_t numerator;
  std::int64_t denominator;
};

FrameRate PictureRate(Standard standard) {
  if (standard == Standard::kPal) {
    return {25, 1};
  }
  return {30000, 1001};
}

// Header time of the first sector of picture `picture`, in sectors, before
// the picture offset: floor(picture * 75 / frame rate). Exact on PAL, where a
// picture is three sectors.
std::int64_t PictureSectors(Standard standard, std::int64_t picture) {
  const FrameRate rate = PictureRate(standard);
  const std::int64_t scaled = picture * kSectorsPerSecond * rate.denominator;
  // Floor division for a negative picture number as well.
  std::int64_t quotient = scaled / rate.numerator;
  if (scaled % rate.numerator != 0 && scaled < 0) {
    --quotient;
  }
  return quotient;
}

}  // namespace

EfmDataSlot EfmDataLayout::Slot(std::size_t section) const {
  EfmDataSlot slot;
  if (section >= section_carried.size() || !section_carried[section]) {
    return slot;
  }
  const std::int64_t address =
      static_cast<std::int64_t>(section) + address_offset;
  const std::int64_t image_index = address - first_address;
  if (image_index >= 0 &&
      image_index < static_cast<std::int64_t>(image_sector_count)) {
    slot.kind = EfmDataSlotKind::kImageSector;
    slot.image_index = static_cast<std::size_t>(image_index);
    slot.address = static_cast<std::size_t>(address);
  } else if (address < 0) {
    slot.kind = EfmDataSlotKind::kLeadInFill;
    slot.address = section;
  } else {
    slot.kind = EfmDataSlotKind::kMode0Fill;
    slot.address = static_cast<std::size_t>(address);
  }
  return slot;
}

bool BuildEfmDataLayout(Standard standard,
                        const std::vector<EfmDataFrame>& frames,
                        const EfmDataLayoutSettings& settings,
                        EfmDataLayout* layout,
                        std::vector<std::string>* errors) {
  if (layout == nullptr || errors == nullptr) {
    return false;
  }
  *layout = EfmDataLayout{};
  if (!StandardHasLaserdiscDigitalAudio(standard)) {
    errors->push_back(
        "EFM data is only defined for the PAL, NTSC and PAL-M video "
        "standards.");
    return false;
  }
  if (frames.empty()) {
    errors->push_back("EFM data requires at least one output frame.");
    return false;
  }

  // The F1-frame span of every output frame, on the 44.1 kHz grid.
  layout->frame_f1_start.reserve(frames.size() + 1);
  layout->frame_carries_efm.reserve(frames.size());
  std::size_t samples = 0;
  for (std::size_t k = 0; k < frames.size(); ++k) {
    layout->frame_f1_start.push_back(samples / efm::kStereoSamplesPerF1Frame);
    layout->frame_carries_efm.push_back(frames[k].section != nullptr &&
                                        frames[k].section->carries_efm_data);
    samples += static_cast<std::size_t>(
        EfmAudioSamplesForFrame(standard, static_cast<std::int64_t>(k)));
  }
  const std::size_t total_f1 = samples / efm::kStereoSamplesPerF1Frame;
  layout->frame_f1_start.push_back(total_f1);

  // Which subcode sections lie wholly in carrying frames, and where each
  // carrying run ends (the end of the stream counts as no end: the CIRC
  // pipeline is flushed there).
  const std::size_t section_count = total_f1 / kFramesPerSection;
  layout->section_carried.assign(section_count, false);
  std::vector<std::size_t> run_end(section_count, 0);
  constexpr std::size_t kStreamEnd = std::numeric_limits<std::size_t>::max();
  // The F1 index at which the carrying run containing each frame ends,
  // computed backwards in one pass.
  std::vector<std::size_t> frame_run_end(frames.size(), 0);
  for (std::size_t k = frames.size(); k-- > 0;) {
    if (!layout->frame_carries_efm[k]) {
      continue;
    }
    if (k + 1 == frames.size()) {
      frame_run_end[k] = kStreamEnd;
    } else if (layout->frame_carries_efm[k + 1]) {
      frame_run_end[k] = frame_run_end[k + 1];
    } else {
      frame_run_end[k] = layout->frame_f1_start[k + 1];
    }
  }
  {
    std::size_t k = 0;
    for (std::size_t s = 0; s < section_count; ++s) {
      const std::size_t first = s * kFramesPerSection;
      const std::size_t last = first + kFramesPerSection - 1;
      while (layout->frame_f1_start[k + 1] <= first) {
        ++k;
      }
      bool carried = true;
      std::size_t j = k;
      while (carried && layout->frame_f1_start[j] <= last) {
        carried = layout->frame_carries_efm[j];
        ++j;
      }
      layout->section_carried[s] = carried;
      if (carried) {
        run_end[s] = frame_run_end[k];
      }
    }
  }

  // The anchor of the placement rule.
  bool anchored = false;
  for (std::size_t k = 0; k < frames.size() && !anchored; ++k) {
    if (frames[k].picture_number > 0) {
      layout->anchor_frame = k;
      layout->anchor_picture = frames[k].picture_number;
      layout->anchor_from_picture_number = true;
      anchored = true;
    }
  }
  for (std::size_t k = 0; k < frames.size() && !anchored; ++k) {
    if (frames[k].section != nullptr &&
        frames[k].section->section_type == SectionType::kProgrammeArea) {
      layout->anchor_frame = k;
      anchored = true;
    }
  }

  const std::int64_t anchor_section = static_cast<std::int64_t>(
      layout->frame_f1_start[layout->anchor_frame] / kFramesPerSection);
  layout->address_offset = PictureSectors(standard, layout->anchor_picture) +
                           settings.picture_offset_sectors - anchor_section;
  layout->first_address = settings.first_address;
  layout->image_sector_count = settings.image_sector_count;

  // Picture numbers that do not continue the anchor's count.
  if (layout->anchor_from_picture_number) {
    for (std::size_t k = layout->anchor_frame; k < frames.size(); ++k) {
      const int picture = frames[k].picture_number;
      const std::int64_t expected =
          static_cast<std::int64_t>(layout->anchor_picture) +
          static_cast<std::int64_t>(k - layout->anchor_frame);
      if (picture > 0 && picture != expected) {
        if (layout->first_picture_mismatch_frame < 0) {
          layout->first_picture_mismatch_frame = static_cast<std::int64_t>(k);
        }
        ++layout->picture_mismatch_frames;
      }
    }
  }

  // Placement report.
  const std::size_t decode_window = kFramesPerSection + efm::kCircDrainFrames;
  for (std::size_t n = 0; n < settings.image_sector_count; ++n) {
    const std::int64_t address =
        settings.first_address + static_cast<std::int64_t>(n);
    const std::int64_t section = address - layout->address_offset;
    const bool placed =
        section >= 0 && section < static_cast<std::int64_t>(section_count) &&
        layout->section_carried[static_cast<std::size_t>(section)];
    if (!placed) {
      ++layout->omitted_image_sectors;
      layout->omitted_image_indices.push_back(n);
      if (layout->first_omitted_address < 0) {
        layout->first_omitted_address = address;
      }
      continue;
    }
    ++layout->placed_image_sectors;
    const auto s = static_cast<std::size_t>(section);
    if (run_end[s] != kStreamEnd &&
        (s * kFramesPerSection) + decode_window > run_end[s]) {
      ++layout->unrecoverable_image_sectors;
      if (layout->first_unrecoverable_address < 0) {
        layout->first_unrecoverable_address = address;
      }
    }
  }
  return true;
}

}  // namespace videosynth
