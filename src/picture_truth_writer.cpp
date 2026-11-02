/*
 * File:        picture_truth_writer.cpp
 * Module:      picture_truth
 * Purpose:     Writes the picture truth (planar float32 Y/U/V at 4fsc) and its
 *              SQLite sidecar beside the generated video output.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "videosynth/picture_truth_writer.h"

#include <sqlite3.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string_view>
#include <system_error>

#include "videosynth/frame_line_layout.h"
#include "videosynth/timing_constants.h"

namespace videosynth {

namespace {

// Strips a trailing ".cvbs" or ".cvbsy" from a video output path, matching
// the audio track naming beside the same output.
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

// The truth file's row id. A truth sidecar describes exactly one truth file,
// as the EFM sidecar indexes exactly one EFM stream.
constexpr int kTruthFileId = 1;

// Appends one plane as little-endian float32, independent of host order.
void AppendPlaneLittleEndian(const std::vector<float>& plane,
                             std::vector<char>* bytes) {
  const std::size_t start = bytes->size();
  bytes->resize(start + (plane.size() * 4U));
  char* out = bytes->data() + start;
  for (const float value : plane) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    out[0] = static_cast<char>(bits & 0xFFU);
    out[1] = static_cast<char>((bits >> 8U) & 0xFFU);
    out[2] = static_cast<char>((bits >> 16U) & 0xFFU);
    out[3] = static_cast<char>((bits >> 24U) & 0xFFU);
    out += 4;
  }
}

}  // namespace

int PictureTruthLayout::RowForLine(int line_1based) const {
  int field_line = -1;
  int field = 0;
  if (line_1based >= first_line_field1 &&
      line_1based < first_line_field1 + active_lines_per_field) {
    field_line = line_1based - first_line_field1;
    field = 1;
  } else if (line_1based >= first_line_field2 &&
             line_1based < first_line_field2 + active_lines_per_field) {
    field_line = line_1based - first_line_field2;
    field = 2;
  } else {
    return -1;
  }
  return (2 * field_line) + ((field == upper_field) ? 0 : 1);
}

void PictureTruthFrame::Reset(const PictureTruthLayout& layout) {
  const std::size_t samples = layout.PlaneSamples();
  y_mv.assign(samples, 0.0F);
  u_mv.assign(samples, 0.0F);
  v_mv.assign(samples, 0.0F);
}

PictureTruthLayout BuildPictureTruthLayout(const CvbsPresets& presets) {
  PictureTruthLayout layout;
  const Standard standard = presets.video_standard_preset;
  if (standard != Standard::kPal && standard != Standard::kNtsc &&
      standard != Standard::kPalM) {
    return layout;
  }
  const TimingConstants timing = GetTimingConstants(standard);
  const ActiveRasterGeometry active =
      GetActiveRasterGeometry(standard, timing.sample_rate_4fsc_hz);
  const int max_line_samples = MaxLineSamples(BuildLineSampleCounts(
      standard, timing.lines_per_frame, timing.samples_per_line_4fsc));
  // The same clamping the synthesis resources apply to the active window.
  const int window_start = std::max(
      0, std::min(active.active_window_start_samples, max_line_samples - 1));
  const int window_end =
      std::max(window_start + 1,
               std::min(active.active_window_end_samples, max_line_samples));

  const SignalLevels levels = GetSignalLevels(presets);
  layout.standard = standard;
  layout.sample_rate_hz = timing.sample_rate_4fsc_hz;
  layout.samples_per_line = window_end - window_start;
  layout.first_sample = window_start;
  layout.active_lines_per_field = active.active_lines_per_field;
  layout.rows = 2 * active.active_lines_per_field;
  layout.first_line_field1 = active.first_active_line_field1;
  layout.first_line_field2 = active.first_active_line_field2;
  // A progressive picture's even rows belong to the field whose picture opens
  // higher on the screen: field 1 on 625-line systems, field 2 on 525-line.
  layout.upper_field = Is625LineStandard(standard) ? 1 : 2;
  layout.half_picture_line = active.half_picture_line;
  layout.half_picture_end_sample = active.half_picture_end_samples;
  layout.black_mv = levels.black_mv - levels.blanking_mv;
  layout.white_mv = levels.white_mv - levels.blanking_mv;
  return layout;
}

std::string DerivePictureTruthPath(const std::string& video_path) {
  return StripVideoSuffix(video_path) + ".truth";
}

std::string DerivePictureTruthSidecarPath(const std::string& video_path) {
  return StripVideoSuffix(video_path) + ".truth.meta";
}

PictureTruthWriter::PictureTruthWriter(ILogger* logger) : logger_(logger) {}

bool PictureTruthWriter::Begin(const Project& project,
                               std::vector<std::string>* errors) {
  Abort();
  layout_ = BuildPictureTruthLayout(project.cvbs_presets);
  if (layout_.rows <= 0) {
    errors->push_back(
        "Truth output is only available for PAL, NTSC and PAL-M projects.");
    return false;
  }
  truth_path_ = DerivePictureTruthPath(project.output.video_path);
  sidecar_path_ = DerivePictureTruthSidecarPath(project.output.video_path);
  stream_.open(truth_path_, std::ios::binary | std::ios::trunc);
  if (!stream_) {
    errors->push_back("Failed to open truth output file: " + truth_path_);
    return false;
  }
  frames_written_ = 0;
  open_ = true;
  if (logger_ != nullptr) {
    logger_->Debug("Writing picture truth to " + truth_path_ + " (" +
                   std::to_string(layout_.samples_per_line) + "x" +
                   std::to_string(layout_.rows) + " per plane).");
  }
  return true;
}

bool PictureTruthWriter::AppendFrame(const PictureTruthFrame& frame,
                                     std::vector<std::string>* errors) {
  if (!open_) {
    errors->push_back("Truth output session is not open.");
    return false;
  }
  const std::size_t plane_samples = layout_.PlaneSamples();
  if (frame.y_mv.size() != plane_samples ||
      frame.u_mv.size() != plane_samples ||
      frame.v_mv.size() != plane_samples) {
    errors->push_back("Truth frame does not match the truth layout.");
    return false;
  }
  std::vector<char> bytes;
  bytes.reserve(plane_samples * 3U * 4U);
  AppendPlaneLittleEndian(frame.y_mv, &bytes);
  AppendPlaneLittleEndian(frame.u_mv, &bytes);
  AppendPlaneLittleEndian(frame.v_mv, &bytes);
  stream_.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  if (!stream_) {
    errors->push_back("Failed writing truth output file: " + truth_path_);
    return false;
  }
  ++frames_written_;
  return true;
}

bool PictureTruthWriter::Finalize(std::vector<std::string>* errors) {
  if (!open_) {
    return true;
  }
  stream_.close();
  open_ = false;
  if (stream_.fail()) {
    errors->push_back("Failed closing truth output file: " + truth_path_);
    return false;
  }
  if (!WriteSidecar(errors)) {
    return false;
  }
  if (logger_ != nullptr) {
    logger_->Info("Wrote picture truth for " + std::to_string(frames_written_) +
                  " frame(s): " + truth_path_);
  }
  return true;
}

bool PictureTruthWriter::WriteSidecar(std::vector<std::string>* errors) const {
  // Recreate from scratch: repeated runs on the same output path must never
  // leave a stale row describing a previous truth file.
  std::remove(sidecar_path_.c_str());

  sqlite3* db = nullptr;
  if (sqlite3_open(sidecar_path_.c_str(), &db) != SQLITE_OK) {
    errors->push_back("Failed to create truth sidecar database: " +
                      sidecar_path_);
    sqlite3_close(db);
    return false;
  }

  // One row per truth file. The fixed-text columns state the value
  // conventions so a reader need not infer them: planes Y, U, V in that order;
  // little-endian float32 millivolts; Y above blanking; U on the sin and V on
  // the cos carrier axis, with the PAL V-switch removed from V.
  const std::string schema =
      "PRAGMA user_version = " +
      std::to_string(kPictureTruthSidecarSchemaVersion) +
      ";"
      "CREATE TABLE truth_file ("
      "    truth_file_id            INTEGER PRIMARY KEY,"
      "    video_standard           TEXT NOT NULL"
      "        CHECK (video_standard IN ('PAL', 'NTSC', 'PAL_M')),"
      "    sample_rate_hz           REAL NOT NULL CHECK (sample_rate_hz > 0),"
      "    frame_count              INTEGER NOT NULL CHECK (frame_count >= 0),"
      "    samples_per_line         INTEGER NOT NULL"
      "        CHECK (samples_per_line > 0),"
      "    rows                     INTEGER NOT NULL CHECK (rows > 0),"
      "    first_sample             INTEGER NOT NULL CHECK (first_sample >= 0),"
      "    first_line_field1        INTEGER NOT NULL,"
      "    first_line_field2        INTEGER NOT NULL,"
      "    upper_field              INTEGER NOT NULL"
      "        CHECK (upper_field IN (1, 2)),"
      "    half_picture_line        INTEGER NOT NULL,"
      "    half_picture_end_sample  INTEGER NOT NULL,"
      "    plane_order              TEXT NOT NULL,"
      "    sample_format            TEXT NOT NULL,"
      "    units                    TEXT NOT NULL,"
      "    y_reference              TEXT NOT NULL,"
      "    black_mv                 REAL NOT NULL,"
      "    white_mv                 REAL NOT NULL,"
      "    u_axis                   TEXT NOT NULL,"
      "    v_axis                   TEXT NOT NULL,"
      "    v_switch                 TEXT NOT NULL"
      ");"
      "BEGIN;";

  char* error_msg = nullptr;
  if (sqlite3_exec(db, schema.c_str(), nullptr, nullptr, &error_msg) !=
      SQLITE_OK) {
    const std::string msg = error_msg != nullptr ? error_msg : "unknown error";
    sqlite3_free(error_msg);
    errors->push_back("Failed to create truth sidecar schema in " +
                      sidecar_path_ + ": " + msg);
    sqlite3_close(db);
    return false;
  }

  sqlite3_stmt* insert_stmt = nullptr;
  const char* insert_sql =
      "INSERT INTO truth_file ("
      "truth_file_id, video_standard, sample_rate_hz, frame_count, "
      "samples_per_line, rows, first_sample, first_line_field1, "
      "first_line_field2, upper_field, half_picture_line, "
      "half_picture_end_sample, plane_order, sample_format, units, "
      "y_reference, black_mv, white_mv, u_axis, v_axis, v_switch) "
      "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 'YUV', 'float32le', 'mV', "
      "'blanking', ?, ?, 'sin', 'cos', 'removed');";
  if (sqlite3_prepare_v2(db, insert_sql, -1, &insert_stmt, nullptr) !=
      SQLITE_OK) {
    errors->push_back("Failed to prepare truth sidecar insert for: " +
                      sidecar_path_);
    sqlite3_close(db);
    return false;
  }

  const std::string standard = StandardToString(layout_.standard);
  sqlite3_bind_int(insert_stmt, 1, kTruthFileId);
  sqlite3_bind_text(insert_stmt, 2, standard.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_double(insert_stmt, 3, layout_.sample_rate_hz);
  sqlite3_bind_int64(insert_stmt, 4,
                     static_cast<sqlite3_int64>(frames_written_));
  sqlite3_bind_int(insert_stmt, 5, layout_.samples_per_line);
  sqlite3_bind_int(insert_stmt, 6, layout_.rows);
  sqlite3_bind_int(insert_stmt, 7, layout_.first_sample);
  sqlite3_bind_int(insert_stmt, 8, layout_.first_line_field1);
  sqlite3_bind_int(insert_stmt, 9, layout_.first_line_field2);
  sqlite3_bind_int(insert_stmt, 10, layout_.upper_field);
  sqlite3_bind_int(insert_stmt, 11, layout_.half_picture_line);
  sqlite3_bind_int(insert_stmt, 12, layout_.half_picture_end_sample);
  sqlite3_bind_double(insert_stmt, 13, layout_.black_mv);
  sqlite3_bind_double(insert_stmt, 14, layout_.white_mv);
  if (sqlite3_step(insert_stmt) != SQLITE_DONE) {
    errors->push_back("Failed to insert truth sidecar row into: " +
                      sidecar_path_);
    sqlite3_finalize(insert_stmt);
    sqlite3_close(db);
    return false;
  }
  sqlite3_finalize(insert_stmt);

  if (sqlite3_exec(db, "COMMIT;", nullptr, nullptr, &error_msg) != SQLITE_OK) {
    const std::string msg = error_msg != nullptr ? error_msg : "unknown error";
    sqlite3_free(error_msg);
    errors->push_back("Failed to commit truth sidecar " + sidecar_path_ + ": " +
                      msg);
    sqlite3_close(db);
    return false;
  }
  sqlite3_close(db);
  return true;
}

void PictureTruthWriter::Abort() {
  if (!open_) {
    return;
  }
  stream_.close();
  open_ = false;
  // Best-effort cleanup, as for the other output artefacts.
  std::error_code ec;
  std::filesystem::remove(truth_path_, ec);
  std::filesystem::remove(sidecar_path_, ec);
}

}  // namespace videosynth
