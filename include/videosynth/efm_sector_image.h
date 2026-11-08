/*
 * File:        efm_sector_image.h
 * Module:      efm_sector_image
 * Purpose:     Read access to the sector image encoded into the EFM data
 *              channel: 2 048 bytes of user data per sector.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "videosynth/efm/data_sector.h"

namespace videosynth {

using EfmSectorUserData = std::array<std::uint8_t, efm::kMode1UserDataBytes>;

// A sector image: the user data of consecutive Mode 1 sectors, as decode-orc's
// EFM sink writes in data mode.
//
// Thread-safety: implementations need not be thread-safe.
class IEfmSectorImage {
 public:
  virtual ~IEfmSectorImage() = default;
  // Number of sectors in the image.
  virtual std::size_t SectorCount() const = 0;
  // Reads sector `index` into `data`. Returns false and sets `error` on
  // failure or when `index` is out of range.
  virtual bool ReadSector(std::size_t index, EfmSectorUserData* data,
                          std::string* error) = 0;
};

// An image held in memory, for tests and small images.
class MemoryEfmSectorImage final : public IEfmSectorImage {
 public:
  explicit MemoryEfmSectorImage(std::vector<EfmSectorUserData> sectors)
      : sectors_(std::move(sectors)) {}
  std::size_t SectorCount() const override { return sectors_.size(); }
  bool ReadSector(std::size_t index, EfmSectorUserData* data,
                  std::string* error) override;

 private:
  std::vector<EfmSectorUserData> sectors_;
};

// An image file read sector by sector, so a disc-sized image (about 330 MB
// for a Domesday side) is never held in memory.
class FileEfmSectorImage final : public IEfmSectorImage {
 public:
  // Opens `path`. Returns false and appends a message to errors when the file
  // cannot be opened, is empty, or is not a whole number of 2 048-byte
  // sectors.
  bool Open(const std::string& path, std::vector<std::string>* errors);
  std::size_t SectorCount() const override { return sector_count_; }
  bool ReadSector(std::size_t index, EfmSectorUserData* data,
                  std::string* error) override;

 private:
  std::ifstream stream_;
  std::string path_;
  std::size_t sector_count_ = 0;
};

}  // namespace videosynth
