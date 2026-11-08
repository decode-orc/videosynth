/*
 * File:        data_efm_writer.h
 * Module:      data_efm_writer
 * Purpose:     Writes the EFM data channel (an ECMA-130 sector image placed
 *              by the AIV picture rule) beside the generated CVBS output.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "videosynth/efm/subcode_generator.h"
#include "videosynth/efm_data_encoder.h"
#include "videosynth/efm_data_layout.h"
#include "videosynth/efm_sector_image.h"
#include "videosynth/efm_stream_file.h"
#include "videosynth/interfaces.h"
#include "videosynth/model.h"

namespace videosynth {

// The subcode track table of an EFM data stream: one data track over the
// whole programme area, with the project's Q control and Q mode (ADR). When
// the stream opens with a lead_in section, the programme area starts at the
// subcode section that `layout` gives Sector Address 0, so the Q absolute
// time equals the Sector Address; the lead-in sections before it stay EFM
// lead-in. Returns false and appends a message when that leaves the lead-in
// less than one table-of-contents cycle.
bool BuildEfmDataTrackTable(const Project& project,
                            const std::vector<EfmDataFrame>& frames,
                            const EfmDataLayout& layout, efm::TrackTable* table,
                            std::vector<std::string>* errors);

// Drives an EfmDataEncoder per stored output frame and writes the result as
// the `<basename>.efm` / `<basename>.efm.meta` pair (EfmStreamFile).
//
// Streaming contract: BeginWrite -> one AppendFrame per stored output frame in
// output order -> FinalizeWrite. AbortWrite removes the partial files.
//
// BeginWrite logs the placement summary: image sectors placed and left out
// (with the first address left out), sectors cut by a frame without EFM,
// picture numbers that break the anchor's count, and (as a warning) image
// sectors left out that are not blank (every byte the same value), which
// usually means a layout mistake.
//
// Thread-safety: NOT thread-safe; drive one instance from one thread.
class DataEfmWriter {
 public:
  explicit DataEfmWriter(ILogger* logger = nullptr);

  // Opens the image named by project.laserdisc.efm_data (or uses `image` when
  // not null, which must then outlive the writer), builds the layout and track
  // table for `frames` and opens the output files.
  bool BeginWrite(const Project& project,
                  const std::vector<EfmDataFrame>& frames,
                  std::vector<std::string>* errors,
                  IEfmSectorImage* image = nullptr);

  bool AppendFrame(std::vector<std::string>* errors);
  bool FinalizeWrite(std::vector<std::string>* errors);
  void AbortWrite();

  const EfmDataLayout& layout() const { return layout_; }

 private:
  void LogPlacementSummary();

  ILogger* logger_;
  FileEfmSectorImage file_image_;
  IEfmSectorImage* image_ = nullptr;
  EfmDataLayout layout_;
  EfmDataEncoder encoder_;
  EfmStreamFile file_;
  std::size_t next_frame_ = 0;
  std::vector<std::uint8_t> t_values_;
};

}  // namespace videosynth
