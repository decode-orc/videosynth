/*
 * File:        efm_data_encoder.h
 * Module:      efm_data_encoder
 * Purpose:     Encodes a sector image into the EFM channel stream frame by
 *              frame, following an EfmDataLayout.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "videosynth/efm/efm_stream_encoder.h"
#include "videosynth/efm/subcode_generator.h"
#include "videosynth/efm_data_layout.h"
#include "videosynth/efm_sector_image.h"

namespace videosynth {

// Produces the T values of each output frame of an EFM data stream. A frame
// that carries EFM yields the channel frames of its F1-frame span: each
// carried subcode section carries the sector its slot names (EfmDataLayout::
// Slot), built as an ECMA-130 sector, scrambled and mapped onto F1 frames.
// The parts of a section not wholly carried hold digital silence. A frame
// that carries no EFM yields no T values, and the stream restarts after it
// (EfmStreamEncoder::SkipFrames).
//
// A sector's F1 frames enter the CIRC encoder at the start of its subcode
// section; the CIRC delay is not pre-compensated, as for audio.
//
// Usage: Begin -> EncodeFrame(k) for k = 0, 1, ... in order -> Flush.
//
// Thread-safety: NOT thread-safe; drive one instance from one thread. The
// layout and image must outlive the encoder.
class EfmDataEncoder {
 public:
  // Adopts `layout`, `track_table` and `image`. Returns false and appends a
  // message to errors when the track table is rejected or the image size
  // disagrees with the layout.
  bool Begin(const EfmDataLayout* layout, const efm::TrackTable& track_table,
             IEfmSectorImage* image, std::vector<std::string>* errors);

  // Appends the T values of output frame `frame` (which must be the next
  // frame) to `t_values`.
  bool EncodeFrame(std::size_t frame, std::vector<std::uint8_t>* t_values,
                   std::vector<std::string>* errors);

  // Ends the stream: flushes the CIRC pipeline when the last frame carried
  // EFM, so the final sectors are fully represented, and emits the last run.
  bool Flush(std::vector<std::uint8_t>* t_values,
             std::vector<std::string>* errors);

  // Sector counts by kind, for the run summary.
  std::size_t image_sectors_written() const { return image_sectors_written_; }
  std::size_t fill_sectors_written() const { return fill_sectors_written_; }

 private:
  // Loads the F1 frames of subcode section `section` into section_frames_.
  bool LoadSection(std::size_t section, std::vector<std::string>* errors);

  const EfmDataLayout* layout_ = nullptr;
  IEfmSectorImage* image_ = nullptr;
  efm::EfmStreamEncoder encoder_;
  std::size_t next_frame_ = 0;
  std::array<efm::F1Frame, efm::kFramesPerSubcodeSection> section_frames_{};
  std::size_t loaded_section_ = 0;
  bool section_loaded_ = false;
  std::size_t image_sectors_written_ = 0;
  std::size_t fill_sectors_written_ = 0;
  std::vector<efm::F1Frame> frame_buffer_;
};

}  // namespace videosynth
