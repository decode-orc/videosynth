/*
 * File:        efm_data_encoder.cpp
 * Module:      efm_data_encoder
 * Purpose:     Encodes a sector image into the EFM channel stream frame by
 *              frame, following an EfmDataLayout.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "videosynth/efm_data_encoder.h"

#include "videosynth/efm/data_sector.h"

namespace videosynth {

bool EfmDataEncoder::Begin(const EfmDataLayout* layout,
                           const efm::TrackTable& track_table,
                           IEfmSectorImage* image,
                           std::vector<std::string>* errors) {
  if (errors == nullptr) {
    return false;
  }
  if (layout == nullptr || image == nullptr) {
    errors->push_back("EFM data encoder requires a layout and an image.");
    return false;
  }
  if (image->SectorCount() != layout->image_sector_count) {
    errors->push_back("EFM data image has " +
                      std::to_string(image->SectorCount()) +
                      " sectors but the layout was built for " +
                      std::to_string(layout->image_sector_count) + ".");
    return false;
  }
  if (!encoder_.Begin(track_table)) {
    errors->push_back("EFM data track table is not a valid subcode layout.");
    return false;
  }
  layout_ = layout;
  image_ = image;
  next_frame_ = 0;
  section_loaded_ = false;
  image_sectors_written_ = 0;
  fill_sectors_written_ = 0;
  return true;
}

bool EfmDataEncoder::LoadSection(std::size_t section,
                                 std::vector<std::string>* errors) {
  if (section_loaded_ && loaded_section_ == section) {
    return true;
  }
  const EfmDataSlot slot = layout_->Slot(section);
  efm::Sector sector{};
  switch (slot.kind) {
    case EfmDataSlotKind::kSilent:
      section_frames_.fill(efm::kSilentF1Frame);
      loaded_section_ = section;
      section_loaded_ = true;
      return true;
    case EfmDataSlotKind::kImageSector: {
      EfmSectorUserData data{};
      std::string error;
      if (!image_->ReadSector(slot.image_index, &data, &error)) {
        errors->push_back(error);
        return false;
      }
      sector = efm::BuildMode1Sector(slot.address, data);
      ++image_sectors_written_;
      break;
    }
    case EfmDataSlotKind::kLeadInFill:
      sector = efm::BuildLeadInMode1Sector(slot.address);
      ++fill_sectors_written_;
      break;
    case EfmDataSlotKind::kMode0Fill:
      sector = efm::BuildMode0Sector(slot.address);
      ++fill_sectors_written_;
      break;
  }
  efm::ScrambleSector(&sector);
  section_frames_ = efm::SectorToF1Frames(sector);
  loaded_section_ = section;
  section_loaded_ = true;
  return true;
}

bool EfmDataEncoder::EncodeFrame(std::size_t frame,
                                 std::vector<std::uint8_t>* t_values,
                                 std::vector<std::string>* errors) {
  if (errors == nullptr || t_values == nullptr) {
    return false;
  }
  if (layout_ == nullptr || frame != next_frame_ ||
      frame + 1 >= layout_->frame_f1_start.size()) {
    errors->push_back("EFM data frame " + std::to_string(frame) +
                      " is out of order or out of range.");
    return false;
  }
  ++next_frame_;
  const std::size_t first = layout_->frame_f1_start[frame];
  const std::size_t end = layout_->frame_f1_start[frame + 1];

  if (!layout_->frame_carries_efm[frame]) {
    if (!encoder_.SkipFrames(end - first, t_values)) {
      errors->push_back("EFM data encoder failed to skip frame " +
                        std::to_string(frame) + ".");
      return false;
    }
    return true;
  }

  frame_buffer_.clear();
  for (std::size_t f1 = first; f1 < end; ++f1) {
    const std::size_t section = f1 / efm::kFramesPerSubcodeSection;
    if (!LoadSection(section, errors)) {
      return false;
    }
    frame_buffer_.push_back(
        section_frames_[f1 % efm::kFramesPerSubcodeSection]);
  }
  if (!encoder_.PushF1Frames(frame_buffer_.data(), frame_buffer_.size(),
                             t_values)) {
    errors->push_back("EFM data encoder rejected frame " +
                      std::to_string(frame) + ".");
    return false;
  }
  return true;
}

bool EfmDataEncoder::Flush(std::vector<std::uint8_t>* t_values,
                           std::vector<std::string>* errors) {
  if (errors == nullptr || t_values == nullptr || layout_ == nullptr) {
    return false;
  }
  const bool last_carries =
      !layout_->frame_carries_efm.empty() && layout_->frame_carries_efm.back();
  // A stream ending in a carrying frame drains the CIRC pipeline, as for
  // audio; one ending without EFM already emitted its last run.
  const bool ok = last_carries ? encoder_.Flush(t_values)
                               : encoder_.SkipFrames(0, t_values);
  if (!ok) {
    errors->push_back("EFM data encoder failed to flush.");
  }
  encoder_.Reset();
  layout_ = nullptr;
  return ok;
}

}  // namespace videosynth
