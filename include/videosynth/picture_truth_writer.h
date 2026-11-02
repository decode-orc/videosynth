/*
 * File:        picture_truth_writer.h
 * Module:      picture_truth
 * Purpose:     Writes the picture truth (planar float32 Y/U/V at 4fsc) and its
 *              SQLite sidecar beside the generated video output.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <cstddef>
#include <fstream>
#include <string>
#include <vector>

#include "videosynth/interfaces.h"
#include "videosynth/picture_truth.h"

namespace videosynth {

// Schema version of the truth sidecar (PRAGMA user_version).
constexpr int kPictureTruthSidecarSchemaVersion = 1;

// The truth sample file and sidecar for a video output path, named like the
// EFM stream and its sidecar: a trailing ".cvbs" or ".cvbsy" is stripped and
// ".truth" / ".truth.meta" appended.
//
// Thread-safety: pure functions.
std::string DerivePictureTruthPath(const std::string& video_path);
std::string DerivePictureTruthSidecarPath(const std::string& video_path);

// Streams truth frames to disk in output order.
//
// The sample file holds, per frame, the Y plane then the U plane then the V
// plane, each layout.rows × layout.samples_per_line little-endian float32
// values in row-major order. Finalize writes the SQLite sidecar: one
// truth_file row giving the layout, frame count, levels and conventions.
//
// Thread-safety: NOT thread-safe; one session owner calls every method.
class PictureTruthWriter {
 public:
  explicit PictureTruthWriter(ILogger* logger = nullptr);

  // Opens the truth file for project.output.video_path. The output stage has
  // already created the directory. Returns false and appends an error when
  // the standard carries no truth or the file cannot be opened.
  bool Begin(const Project& project, std::vector<std::string>* errors);

  const PictureTruthLayout& layout() const { return layout_; }

  bool AppendFrame(const PictureTruthFrame& frame,
                   std::vector<std::string>* errors);

  // Closes the sample file and writes the sidecar.
  bool Finalize(std::vector<std::string>* errors);

  // Closes and removes both files. No-op when no session is open.
  void Abort();

 private:
  // Writes the truth_file row for the frames written so far.
  bool WriteSidecar(std::vector<std::string>* errors) const;

  ILogger* logger_;
  PictureTruthLayout layout_;
  std::string truth_path_;
  std::string sidecar_path_;
  std::ofstream stream_;
  std::size_t frames_written_ = 0;
  bool open_ = false;
};

}  // namespace videosynth
