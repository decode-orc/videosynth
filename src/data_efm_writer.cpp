/*
 * File:        data_efm_writer.cpp
 * Module:      data_efm_writer
 * Purpose:     Writes the EFM data channel (an ECMA-130 sector image placed
 *              by the AIV picture rule) beside the generated CVBS output.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "videosynth/data_efm_writer.h"

#include <algorithm>

#include "videosynth/efm_track_layout.h"

namespace videosynth {

namespace {

// True when every byte of `data` holds the same value: free space in a
// filesystem image (Domesday fills it with 0x20).
bool IsBlankSector(const EfmSectorUserData& data) {
  return std::all_of(data.begin(), data.end(),
                     [&data](std::uint8_t byte) { return byte == data[0]; });
}

std::uint8_t ControlNibble(EfmDataQControl q_control) {
  return q_control == EfmDataQControl::kDataCopyPermitted
             ? efm::kControlDataCopyPermitted
             : efm::kControlDataCopyProhibited;
}

}  // namespace

bool BuildEfmDataTrackTable(const Project& project,
                            const std::vector<EfmDataFrame>& frames,
                            efm::TrackTable* table,
                            std::vector<std::string>* errors) {
  if (table == nullptr || errors == nullptr) {
    return false;
  }
  std::vector<const Section*> sections;
  sections.reserve(frames.size());
  for (const EfmDataFrame& frame : frames) {
    sections.push_back(frame.section);
  }
  EfmTrackLayoutOptions options;
  options.single_programme_track = true;
  EfmTrackLayout track_layout;
  if (!BuildEfmTrackLayout(project.cvbs_presets.video_standard_preset, sections,
                           &track_layout, errors, options)) {
    return false;
  }
  *table = track_layout.table;
  table->control = ControlNibble(project.laserdisc.efm_data.q_control);
  table->adr = static_cast<std::uint8_t>(project.laserdisc.efm_data.q_mode);
  return true;
}

DataEfmWriter::DataEfmWriter(ILogger* logger)
    : logger_(logger), file_(logger) {}

bool DataEfmWriter::BeginWrite(const Project& project,
                               const std::vector<EfmDataFrame>& frames,
                               std::vector<std::string>* errors,
                               IEfmSectorImage* image) {
  if (errors == nullptr) {
    return false;
  }
  const EfmDataOutput& efm_data = project.laserdisc.efm_data;
  if (image == nullptr) {
    if (!file_image_.Open(efm_data.image, errors)) {
      return false;
    }
    image = &file_image_;
  }
  image_ = image;

  EfmDataLayoutSettings settings;
  settings.image_sector_count = image->SectorCount();
  settings.first_address = efm_data.first_address;
  settings.picture_offset_sectors = efm_data.picture_offset_sectors;
  if (!BuildEfmDataLayout(project.cvbs_presets.video_standard_preset, frames,
                          settings, &layout_, errors)) {
    return false;
  }
  efm::TrackTable table;
  if (!BuildEfmDataTrackTable(project, frames, &table, errors)) {
    return false;
  }
  if (!encoder_.Begin(&layout_, table, image, errors)) {
    return false;
  }
  LogPlacementSummary();
  if (!file_.Open(project.output.video_path, errors)) {
    return false;
  }
  next_frame_ = 0;
  return true;
}

void DataEfmWriter::LogPlacementSummary() {
  if (logger_ == nullptr) {
    return;
  }
  logger_->Info("EFM data: " + std::to_string(layout_.image_sector_count) +
                " image sectors; anchor picture " +
                std::to_string(layout_.anchor_picture) + " at output frame " +
                std::to_string(layout_.anchor_frame) +
                (layout_.anchor_from_picture_number
                     ? ""
                     : " (no picture numbers: first programme frame)") +
                "; header address = subcode section + " +
                std::to_string(layout_.address_offset) + ".");
  logger_->Info("EFM data: " + std::to_string(layout_.placed_image_sectors) +
                " image sectors placed.");
  if (layout_.omitted_image_sectors > 0) {
    logger_->Warning(
        "EFM data: " + std::to_string(layout_.omitted_image_sectors) +
        " image sectors fall outside the frames that carry EFM and are left "
        "out, the first at address " +
        std::to_string(layout_.first_omitted_address) + ".");
  }
  if (layout_.unrecoverable_image_sectors > 0) {
    logger_->Warning(
        "EFM data: " + std::to_string(layout_.unrecoverable_image_sectors) +
        " placed image sectors end too close to a frame without EFM to be "
        "decoded (CIRC decode window), the first at address " +
        std::to_string(layout_.first_unrecoverable_address) + ".");
  }
  if (layout_.picture_mismatch_frames > 0) {
    logger_->Warning(
        "EFM data: " + std::to_string(layout_.picture_mismatch_frames) +
        " frames carry a picture number that breaks the continuous count from "
        "the anchor (first at output frame " +
        std::to_string(layout_.first_picture_mismatch_frame) +
        "); their sectors follow the subcode timeline, not the picture "
        "number.");
  }

  // Left-out sectors should be free space; anything else is lost data.
  std::size_t non_blank = 0;
  std::int64_t first_non_blank = -1;
  EfmSectorUserData data{};
  for (const std::size_t index : layout_.omitted_image_indices) {
    std::string error;
    if (!image_->ReadSector(index, &data, &error)) {
      logger_->Warning("EFM data: " + error);
      return;
    }
    if (!IsBlankSector(data)) {
      if (first_non_blank < 0) {
        first_non_blank =
            layout_.first_address + static_cast<std::int64_t>(index);
      }
      ++non_blank;
    }
  }
  if (non_blank > 0) {
    logger_->Warning("EFM data: " + std::to_string(non_blank) +
                     " left-out image sectors are not blank, the first at "
                     "address " +
                     std::to_string(first_non_blank) +
                     "; check which sections declare laserdisc.efm_data.");
  }
}

bool DataEfmWriter::AppendFrame(std::vector<std::string>* errors) {
  if (errors == nullptr) {
    return false;
  }
  t_values_.clear();
  if (!encoder_.EncodeFrame(next_frame_, &t_values_, errors)) {
    return false;
  }
  ++next_frame_;
  return file_.AppendFrame(t_values_, errors);
}

bool DataEfmWriter::FinalizeWrite(std::vector<std::string>* errors) {
  if (errors == nullptr) {
    return false;
  }
  t_values_.clear();
  if (!encoder_.Flush(&t_values_, errors) ||
      !file_.AppendToLastFrame(t_values_, errors)) {
    file_.Abort();
    return false;
  }
  const std::string path = file_.path();
  const std::uint64_t count = file_.t_value_count();
  if (!file_.Close(errors)) {
    return false;
  }
  if (logger_ != nullptr) {
    logger_->Info("Wrote EFM data track: " + path + " (" +
                  std::to_string(encoder_.image_sectors_written()) +
                  " image sectors, " +
                  std::to_string(encoder_.fill_sectors_written()) +
                  " fill sectors, " + std::to_string(count) + " T values)");
  }
  return true;
}

void DataEfmWriter::AbortWrite() { file_.Abort(); }

}  // namespace videosynth
