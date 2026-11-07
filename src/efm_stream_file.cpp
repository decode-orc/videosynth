/*
 * File:        efm_stream_file.cpp
 * Module:      efm_stream_file
 * Purpose:     Writes an EFM T-value stream and its per-frame index sidecar
 *              as the two-file EFM extension beside the CVBS output.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "videosynth/efm_stream_file.h"

#include <sqlite3.h>

#include <cstdio>
#include <filesystem>
#include <string_view>
#include <system_error>

#include "videosynth/efm/t_value_byte.h"
#include "videosynth/path_resolution.h"

namespace videosynth {

namespace {

// Sidecar schema version per efm-extension-format.md §Versioning.
constexpr int kSidecarSchemaVersion = 1;

// The output stage writes a single cvbs_file row, so every efm_frame row
// references capture 1 (efm-extension-format.md §Association with Core
// Metadata).
constexpr int kCvbsFileId = 1;

// Strips the CVBS payload suffix from a video path, leaving the basename the
// EFM extension files must share with the capture.
std::string StripVideoSuffix(const std::string& video_path) {
  constexpr std::string_view kCompositeSuffix = ".cvbs";
  constexpr std::string_view kLumaSuffix = ".cvbsy";

  std::string base = video_path;
  auto ends_with = [&](std::string_view suffix) {
    return base.size() >= suffix.size() &&
           base.compare(base.size() - suffix.size(), suffix.size(), suffix) ==
               0;
  };

  if (ends_with(kCompositeSuffix)) {
    base.resize(base.size() - kCompositeSuffix.size());
  } else if (ends_with(kLumaSuffix)) {
    base.resize(base.size() - kLumaSuffix.size());
  }
  return base;
}

}  // namespace

EfmStreamFile::EfmStreamFile(ILogger* logger) : logger_(logger) {}

std::string EfmStreamFile::DeriveEfmPath(const std::string& video_path) {
  return StripVideoSuffix(video_path) + ".efm";
}

std::string EfmStreamFile::DeriveSidecarPath(const std::string& video_path) {
  return StripVideoSuffix(video_path) + ".efm.meta";
}

bool EfmStreamFile::Open(const std::string& video_path,
                         std::vector<std::string>* errors) {
  if (errors == nullptr) {
    return false;
  }
  if (open_) {
    errors->push_back("EFM output file already open: " + path_);
    return false;
  }
  const std::string path = DeriveEfmPath(video_path);

  std::error_code ec;
  if (!CreateParentDirectories(path, &ec)) {
    errors->push_back("Failed to create EFM output directory for: " + path +
                      " (" + ec.message() + ")");
    return false;
  }
  stream_.open(path, std::ios::binary | std::ios::trunc);
  if (!stream_) {
    errors->push_back("Failed to open EFM output file: " + path);
    return false;
  }

  path_ = path;
  sidecar_path_ = DeriveSidecarPath(video_path);
  t_value_count_ = 0;
  frame_index_.clear();
  open_ = true;
  if (logger_ != nullptr) {
    logger_->Debug("Opened EFM output file for writing: " + path_);
  }
  return true;
}

bool EfmStreamFile::WriteTValues(const std::vector<std::uint8_t>& t_values,
                                 std::vector<std::string>* errors) {
  if (t_values.empty()) {
    return true;
  }

  // efm-extension-format.md, "Binary Data File": each byte pairs a t-value with
  // the producer's doubt about it. The run lengths come from the modulator that
  // synthesised them, so they are exact and pack at zero doubt.
  byte_buffer_.clear();
  byte_buffer_.reserve(t_values.size());
  for (const std::uint8_t t_value : t_values) {
    // The t-value field is four bits wide. IEC 60908-1999 clause 13 bounds the
    // modulator's run lengths at T11, so this cannot trip today; it fails the
    // write rather than let a wider run length be silently truncated.
    if (t_value > efm::kTValueMask) {
      errors->push_back("EFM t-value " + std::to_string(t_value) +
                        " exceeds the 4-bit stream field for: " + path_);
      return false;
    }
    byte_buffer_.push_back(
        efm::PackTValueByte(t_value, efm::kSynthesisedDoubt));
  }

  stream_.write(reinterpret_cast<const char*>(byte_buffer_.data()),
                static_cast<std::streamsize>(byte_buffer_.size()));
  if (!stream_) {
    errors->push_back("Failed while writing EFM data to: " + path_);
    return false;
  }
  t_value_count_ += t_values.size();
  return true;
}

bool EfmStreamFile::AppendFrame(const std::vector<std::uint8_t>& t_values,
                                std::vector<std::string>* errors) {
  if (errors == nullptr) {
    return false;
  }
  if (!open_) {
    errors->push_back("EFM output file is not open.");
    return false;
  }
  // Recorded before the write so the offset is the pre-write file position; a
  // failed write aborts the session and discards the index.
  frame_index_.push_back(FrameIndexRow{t_value_count_, t_values.size()});
  return WriteTValues(t_values, errors);
}

bool EfmStreamFile::AppendToLastFrame(const std::vector<std::uint8_t>& t_values,
                                      std::vector<std::string>* errors) {
  if (errors == nullptr) {
    return false;
  }
  if (!open_) {
    errors->push_back("EFM output file is not open.");
    return false;
  }
  if (!frame_index_.empty()) {
    frame_index_.back().count += t_values.size();
  } else if (!t_values.empty()) {
    frame_index_.push_back(FrameIndexRow{0, t_values.size()});
  }
  return WriteTValues(t_values, errors);
}

bool EfmStreamFile::WriteSidecar(std::vector<std::string>* errors) {
  // Recreate from scratch: repeated runs on the same output path must never
  // leave stale rows describing a previous stream.
  std::remove(sidecar_path_.c_str());

  sqlite3* db = nullptr;
  if (sqlite3_open(sidecar_path_.c_str(), &db) != SQLITE_OK) {
    errors->push_back("Failed to create EFM sidecar database: " +
                      sidecar_path_);
    sqlite3_close(db);
    return false;
  }

  const std::string schema =
      "PRAGMA user_version = " + std::to_string(kSidecarSchemaVersion) +
      ";"
      "CREATE TABLE efm_frame ("
      "    cvbs_file_id    INTEGER NOT NULL,"
      "    frame_id        INTEGER NOT NULL CHECK (frame_id >= 0),"
      "    t_value_offset  INTEGER NOT NULL CHECK (t_value_offset >= 0),"
      "    t_value_count   INTEGER NOT NULL CHECK (t_value_count >= 0),"
      "    PRIMARY KEY (cvbs_file_id, frame_id)"
      ");"
      "CREATE INDEX idx_efm_frame_frame ON efm_frame (cvbs_file_id, frame_id);"
      "BEGIN;";

  char* error_msg = nullptr;
  if (sqlite3_exec(db, schema.c_str(), nullptr, nullptr, &error_msg) !=
      SQLITE_OK) {
    const std::string msg = error_msg != nullptr ? error_msg : "unknown error";
    sqlite3_free(error_msg);
    errors->push_back("Failed to create EFM sidecar schema in " +
                      sidecar_path_ + ": " + msg);
    sqlite3_close(db);
    return false;
  }

  sqlite3_stmt* insert_stmt = nullptr;
  const char* insert_sql =
      "INSERT INTO efm_frame "
      "(cvbs_file_id, frame_id, t_value_offset, t_value_count) "
      "VALUES (?, ?, ?, ?);";
  if (sqlite3_prepare_v2(db, insert_sql, -1, &insert_stmt, nullptr) !=
      SQLITE_OK) {
    errors->push_back("Failed to prepare EFM sidecar insert for: " +
                      sidecar_path_);
    sqlite3_close(db);
    return false;
  }

  for (std::size_t frame_id = 0; frame_id < frame_index_.size(); ++frame_id) {
    const FrameIndexRow& row = frame_index_[frame_id];
    sqlite3_reset(insert_stmt);
    sqlite3_bind_int(insert_stmt, 1, kCvbsFileId);
    sqlite3_bind_int64(insert_stmt, 2, static_cast<sqlite3_int64>(frame_id));
    sqlite3_bind_int64(insert_stmt, 3, static_cast<sqlite3_int64>(row.offset));
    sqlite3_bind_int64(insert_stmt, 4, static_cast<sqlite3_int64>(row.count));
    if (sqlite3_step(insert_stmt) != SQLITE_DONE) {
      errors->push_back("Failed to insert EFM sidecar row for frame " +
                        std::to_string(frame_id) + " into: " + sidecar_path_);
      sqlite3_finalize(insert_stmt);
      sqlite3_close(db);
      return false;
    }
  }
  sqlite3_finalize(insert_stmt);

  if (sqlite3_exec(db, "COMMIT;", nullptr, nullptr, &error_msg) != SQLITE_OK) {
    const std::string msg = error_msg != nullptr ? error_msg : "unknown error";
    sqlite3_free(error_msg);
    errors->push_back("Failed to commit EFM sidecar " + sidecar_path_ + ": " +
                      msg);
    sqlite3_close(db);
    return false;
  }

  sqlite3_close(db);
  if (logger_ != nullptr) {
    logger_->Info("Wrote EFM frame index sidecar: " + sidecar_path_ + " (" +
                  std::to_string(frame_index_.size()) + " frames)");
  }
  return true;
}

bool EfmStreamFile::Close(std::vector<std::string>* errors) {
  if (errors == nullptr) {
    return false;
  }
  if (!open_) {
    errors->push_back("EFM output file is not open.");
    return false;
  }
  stream_.close();
  open_ = false;
  if (!stream_) {
    errors->push_back("Failed to close EFM output file: " + path_);
    return false;
  }
  return WriteSidecar(errors);
}

void EfmStreamFile::Abort() {
  if (!open_) {
    return;
  }
  stream_.close();
  // Removal failures are ignored: cleanup is best-effort on abort. The sidecar
  // is removed too, so a stale index from an earlier run cannot outlive the
  // stream it described.
  std::error_code ec;
  std::filesystem::remove(path_, ec);
  std::filesystem::remove(sidecar_path_, ec);
  if (logger_ != nullptr) {
    logger_->Info("EFM write session aborted; removed " + path_ + " and " +
                  sidecar_path_);
  }
  open_ = false;
  t_value_count_ = 0;
  frame_index_.clear();
}

}  // namespace videosynth
