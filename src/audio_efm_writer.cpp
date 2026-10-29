/*
 * File:        audio_efm_writer.cpp
 * Module:      audio_efm_writer
 * Purpose:     Streams one audio channel pair to a LaserDisc digital audio
 *              (EFM) T-value file and its frame-index sidecar alongside the
 *              generated CVBS output.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "videosynth/audio_efm_writer.h"

#include <sqlite3.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

#include "videosynth/audio_sample_conversion.h"
#include "videosynth/path_resolution.h"

namespace videosynth {

namespace {

// Sidecar schema version per efm-extension-format.md §Versioning.
constexpr int kSidecarSchemaVersion = 1;

// The extension has no multi-capture concept here: the output stage writes a
// single cvbs_file row, so every efm_frame row references capture 1
// (efm-extension-format.md §Association with Core Metadata).
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

AudioEfmWriter::AudioEfmWriter(ILogger* logger) : logger_(logger) {}

std::string AudioEfmWriter::DeriveAudioPath(const std::string& video_path) {
  return StripVideoSuffix(video_path) + ".efm";
}

std::string AudioEfmWriter::DeriveSidecarPath(const std::string& video_path) {
  return StripVideoSuffix(video_path) + ".efm.meta";
}

bool AudioEfmWriter::BeginWrite(const Project& project, int channel_pair,
                                const efm::TrackTable& track_table,
                                std::vector<std::string>* errors) {
  if (errors == nullptr) {
    return false;
  }
  if (session_open_) {
    errors->push_back("EFM audio write session already open.");
    return false;
  }

  const std::string audio_path = DeriveAudioPath(project.output.video_path);
  if (audio_path.empty()) {
    errors->push_back(
        "EFM audio output path could not be derived from video_path.");
    return false;
  }

  if (!encoder_.Begin(track_table)) {
    errors->push_back("EFM track table is not a valid subcode layout for: " +
                      audio_path);
    return false;
  }

  std::error_code ec;
  if (!CreateParentDirectories(audio_path, &ec)) {
    errors->push_back("Failed to create EFM audio output directory for: " +
                      audio_path + " (" + ec.message() + ")");
    encoder_.Reset();
    return false;
  }

  stream_.open(audio_path, std::ios::binary | std::ios::trunc);
  if (!stream_) {
    errors->push_back("Failed to open EFM audio output file: " + audio_path);
    encoder_.Reset();
    return false;
  }

  audio_path_ = audio_path;
  sidecar_path_ = DeriveSidecarPath(project.output.video_path);
  t_value_count_ = 0;
  frame_index_.clear();
  session_open_ = true;

  if (logger_ != nullptr) {
    logger_->Trace("Opened EFM audio output file for writing: " + audio_path +
                   " (channel pair " + std::to_string(channel_pair) + ")");
  }
  return true;
}

bool AudioEfmWriter::WriteTValues(const std::vector<std::uint8_t>& t_values,
                                  std::vector<std::string>* errors) {
  if (t_values.empty()) {
    return true;
  }

  // efm-extension-format.md, "Binary Data File": each byte pairs a t-value with
  // the producer's doubt about it. The run lengths come from the modulator that
  // synthesised them, so they are exact and pack at zero doubt; a producer that
  // recovered t-values from a signal would vary the doubt per value here.
  byte_buffer_.clear();
  byte_buffer_.reserve(t_values.size());
  for (const std::uint8_t t_value : t_values) {
    // The t-value field is four bits wide. IEC 60908-1999 clause 13 bounds the
    // modulator's run lengths at T11, so this cannot trip today; it fails the
    // write rather than let a wider run length be silently truncated into a
    // different value.
    if (t_value > efm::kTValueMask) {
      errors->push_back("EFM t-value " + std::to_string(t_value) +
                        " exceeds the 4-bit stream field for: " + audio_path_);
      return false;
    }
    byte_buffer_.push_back(
        efm::PackTValueByte(t_value, efm::kSynthesisedDoubt));
  }

  stream_.write(reinterpret_cast<const char*>(byte_buffer_.data()),
                static_cast<std::streamsize>(byte_buffer_.size()));
  if (!stream_) {
    errors->push_back("Failed while writing EFM audio data to: " + audio_path_);
    return false;
  }
  t_value_count_ += t_values.size();
  return true;
}

bool AudioEfmWriter::AppendFrameAudio(const std::vector<std::int32_t>& left,
                                      const std::vector<std::int32_t>& right,
                                      std::vector<std::string>* errors) {
  if (errors == nullptr) {
    return false;
  }
  if (!session_open_) {
    errors->push_back("EFM audio write session is not open.");
    return false;
  }
  if (left.size() != right.size()) {
    errors->push_back(
        "EFM audio left/right channel buffers differ in length for: " +
        audio_path_);
    return false;
  }

  // IEC 60908-1999 clause 12: compact-disc samples are 16-bit two's complement.
  std::vector<std::uint8_t> t_values;
  if (!encoder_.PushSamples(ConvertSamples24To16(left),
                            ConvertSamples24To16(right), &t_values)) {
    errors->push_back("EFM audio encoder rejected samples for: " + audio_path_);
    return false;
  }

  // One efm_frame row per appended frame, indexing the bytes this frame
  // contributed. Recorded before the write so the offset is the pre-write file
  // position; a failed write aborts the session and discards the index.
  frame_index_.push_back(FrameIndexRow{t_value_count_, t_values.size()});
  return WriteTValues(t_values, errors);
}

bool AudioEfmWriter::WriteSidecar(std::vector<std::string>* errors) {
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

bool AudioEfmWriter::FinalizeWrite(std::vector<std::string>* errors) {
  if (errors == nullptr) {
    return false;
  }
  if (!session_open_) {
    errors->push_back("EFM audio write session is not open.");
    return false;
  }

  std::vector<std::uint8_t> t_values;
  if (!encoder_.Flush(&t_values)) {
    errors->push_back("EFM audio encoder failed to flush for: " + audio_path_);
    stream_.close();
    session_open_ = false;
    return false;
  }
  // Rule 6 of the extension's frame validity rules: the index must account for
  // every byte in the file, so the CIRC-pipeline residue extends the last
  // frame's run rather than becoming an unindexed tail.
  if (!frame_index_.empty()) {
    frame_index_.back().count += t_values.size();
  } else if (!t_values.empty()) {
    frame_index_.push_back(FrameIndexRow{0, t_values.size()});
  }

  if (!WriteTValues(t_values, errors)) {
    stream_.close();
    session_open_ = false;
    return false;
  }

  stream_.close();
  if (!stream_) {
    errors->push_back("Failed to close EFM audio output file: " + audio_path_);
    session_open_ = false;
    return false;
  }
  session_open_ = false;
  encoder_.Reset();

  if (logger_ != nullptr) {
    logger_->Info("Wrote EFM audio track: " + audio_path_ + " (" +
                  std::to_string(t_value_count_) + " T values)");
  }

  if (!WriteSidecar(errors)) {
    return false;
  }
  return true;
}

void AudioEfmWriter::AbortWrite() {
  if (!session_open_) {
    return;
  }

  stream_.close();
  // Removal failures are ignored: cleanup is best-effort on abort. The sidecar
  // is removed too, so a stale index from an earlier run cannot outlive the
  // stream it described.
  std::error_code ec;
  std::filesystem::remove(audio_path_, ec);
  std::filesystem::remove(sidecar_path_, ec);

  if (logger_ != nullptr) {
    logger_->Info("EFM audio write session aborted; removed " + audio_path_ +
                  " and " + sidecar_path_);
  }

  session_open_ = false;
  t_value_count_ = 0;
  frame_index_.clear();
  encoder_.Reset();
}

}  // namespace videosynth
