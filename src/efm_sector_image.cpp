/*
 * File:        efm_sector_image.cpp
 * Module:      efm_sector_image
 * Purpose:     Read access to the sector image encoded into the EFM data
 *              channel: 2 048 bytes of user data per sector.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "videosynth/efm_sector_image.h"

#include <filesystem>
#include <system_error>

namespace videosynth {

bool MemoryEfmSectorImage::ReadSector(std::size_t index,
                                      EfmSectorUserData* data,
                                      std::string* error) {
  if (data == nullptr || index >= sectors_.size()) {
    if (error != nullptr) {
      *error = "EFM data image sector " + std::to_string(index) +
               " is out of range.";
    }
    return false;
  }
  *data = sectors_[index];
  return true;
}

bool FileEfmSectorImage::Open(const std::string& path,
                              std::vector<std::string>* errors) {
  if (errors == nullptr) {
    return false;
  }
  std::error_code ec;
  const std::uintmax_t size = std::filesystem::file_size(path, ec);
  if (ec) {
    errors->push_back("EFM data image cannot be read: " + path + " (" +
                      ec.message() + ")");
    return false;
  }
  if (size == 0 || size % efm::kMode1UserDataBytes != 0) {
    errors->push_back("EFM data image " + path + " is " + std::to_string(size) +
                      " bytes, which is not a whole, non-zero number of "
                      "2048-byte sectors.");
    return false;
  }
  stream_.open(path, std::ios::binary);
  if (!stream_) {
    errors->push_back("Failed to open EFM data image: " + path);
    return false;
  }
  path_ = path;
  sector_count_ = static_cast<std::size_t>(size / efm::kMode1UserDataBytes);
  return true;
}

bool FileEfmSectorImage::ReadSector(std::size_t index, EfmSectorUserData* data,
                                    std::string* error) {
  if (data == nullptr || index >= sector_count_) {
    if (error != nullptr) {
      *error = "EFM data image sector " + std::to_string(index) +
               " is out of range in " + path_;
    }
    return false;
  }
  stream_.seekg(static_cast<std::streamoff>(index * efm::kMode1UserDataBytes));
  stream_.read(reinterpret_cast<char*>(data->data()),
               static_cast<std::streamsize>(data->size()));
  if (!stream_) {
    stream_.clear();
    if (error != nullptr) {
      *error = "Failed to read EFM data image sector " + std::to_string(index) +
               " from " + path_;
    }
    return false;
  }
  return true;
}

}  // namespace videosynth
