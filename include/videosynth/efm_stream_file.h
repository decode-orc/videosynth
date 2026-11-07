/*
 * File:        efm_stream_file.h
 * Module:      efm_stream_file
 * Purpose:     Writes an EFM T-value stream and its per-frame index sidecar
 *              as the two-file EFM extension beside the CVBS output.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "videosynth/interfaces.h"

namespace videosynth {

// The two-file EFM extension of the CVBS File Format Specification
// (extensions/efm-extension-format.md): the binary stream `<basename>.efm`,
// one byte per pit or land run length packed with the producer's doubt
// (efm/t_value_byte.h), and its SQLite sidecar `<basename>.efm.meta` with one
// efm_frame row per stored video frame. Both paths are derived from the CVBS
// output path by stripping a trailing ".cvbs" or ".cvbsy" suffix.
//
// The stream is synthesised, so every T value is written at
// kSynthesisedDoubt.
//
// Streaming contract: Open -> one AppendFrame per stored video frame
// (optionally AppendToLastFrame for a trailing residue) -> Close, which closes
// the binary file and writes the sidecar. Abort removes both files.
//
// Thread-safety: NOT thread-safe; drive one instance from one thread.
class EfmStreamFile {
 public:
  explicit EfmStreamFile(ILogger* logger = nullptr);

  // Opens the EFM path derived from `video_path`, creating parent directories
  // as needed. Returns false and appends a message to errors on failure.
  bool Open(const std::string& video_path, std::vector<std::string>* errors);

  // Writes `t_values` as the efm_frame row of the next video frame. A frame
  // with no EFM signal is an empty `t_values`, i.e. a zero-length row.
  bool AppendFrame(const std::vector<std::uint8_t>& t_values,
                   std::vector<std::string>* errors);

  // Writes `t_values` as part of the last frame's row (a new first row when
  // none has been written), so the index accounts for every byte (rule 6 of
  // the extension's frame validity rules).
  bool AppendToLastFrame(const std::vector<std::uint8_t>& t_values,
                         std::vector<std::string>* errors);

  // Closes the binary file and writes the sidecar.
  bool Close(std::vector<std::string>* errors);

  // Closes the stream and removes the binary file and any sidecar. No-op when
  // not open.
  void Abort();

  bool is_open() const { return open_; }
  const std::string& path() const { return path_; }
  std::uint64_t t_value_count() const { return t_value_count_; }
  std::size_t frame_count() const { return frame_index_.size(); }

  // `<basename>.efm` for the CVBS output path `video_path`.
  static std::string DeriveEfmPath(const std::string& video_path);
  // `<basename>.efm.meta` for the CVBS output path `video_path`.
  static std::string DeriveSidecarPath(const std::string& video_path);

 private:
  struct FrameIndexRow {
    std::uint64_t offset = 0;
    std::uint64_t count = 0;
  };

  bool WriteTValues(const std::vector<std::uint8_t>& t_values,
                    std::vector<std::string>* errors);
  bool WriteSidecar(std::vector<std::string>* errors);

  ILogger* logger_;
  std::ofstream stream_;
  std::string path_;
  std::string sidecar_path_;
  bool open_ = false;
  std::uint64_t t_value_count_ = 0;
  std::vector<FrameIndexRow> frame_index_;
  // Reused across frames so packing costs no per-frame allocation.
  std::vector<std::uint8_t> byte_buffer_;
};

}  // namespace videosynth
