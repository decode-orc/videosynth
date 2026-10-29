/*
 * File:        output_stage.cpp
 * Module:      output_stage
 * Purpose:     Writes generated sample buffers to output and metadata files.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "videosynth/output_stage.h"

#include <sqlite3.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

#include "videosynth/cvbs_quantization.h"
#include "videosynth/fixed_point.h"
#include "videosynth/model.h"
#include "videosynth/path_resolution.h"
#include "videosynth/timing_constants.h"

namespace videosynth {

OutputStage::OutputStage(ILogger* logger) : logger_(logger) {}

namespace {

bool ResolveOutputEncoding(const std::string& preset,
                           OutputSampleEncoding* output_encoding) {
  if (output_encoding == nullptr) {
    return false;
  }

  if (preset == "CVBS_U10_4FSC") {
    *output_encoding = OutputSampleEncoding::kCvbsU10;
    return true;
  }

  if (preset == "CVBS_U16_4FSC") {
    *output_encoding = OutputSampleEncoding::kCvbsU16;
    return true;
  }

  if (preset == "CVBS_TPG21_4FSC") {
    *output_encoding = OutputSampleEncoding::kCvbsTpg21;
    return true;
  }

  if (preset == "CVBS_S16_4FSC") {
    *output_encoding = OutputSampleEncoding::kCvbsS16Fsc;
    return true;
  }

  if (preset == "RAW_S16_28M" || preset == "RAW_S16_40M") {
    *output_encoding = OutputSampleEncoding::kRawS16;
    return true;
  }

  return false;
}

// Linearly resamples input_count samples to target_count samples into output.
// Only the RAW_S16 presets, whose output frame span differs from the 4fsc
// input span, take this path.
void ResampleFrameInto(const SampleFixed* input, std::size_t input_count,
                       std::size_t target_count,
                       std::vector<SampleFixed>* output) {
  output->assign(target_count, 0);
  if (target_count == 0U || input_count == 0U) {
    return;
  }
  if (input_count == 1U || target_count == 1U) {
    output->assign(target_count, input[0]);
    return;
  }

  const double source_last_index = static_cast<double>(input_count - 1U);
  const double target_last_index = static_cast<double>(target_count - 1U);

  for (std::size_t i = 0; i < target_count; ++i) {
    const double source_position =
        source_last_index * (static_cast<double>(i) / target_last_index);
    const std::size_t left_index = static_cast<std::size_t>(source_position);
    const std::size_t right_index = std::min(left_index + 1U, input_count - 1U);
    const double fraction = source_position - static_cast<double>(left_index);
    const double interpolated =
        (1.0 - fraction) * static_cast<double>(input[left_index]) +
        fraction * static_cast<double>(input[right_index]);
    (*output)[i] = static_cast<SampleFixed>(std::llround(interpolated));
  }
}

bool IsNonstandardMappedCode(int mapped_code,
                             const QuantizationProfile& profile) {
  return mapped_code < (profile.minimum_legal_code - 1) ||
         mapped_code > (profile.maximum_legal_code + 1);
}

// Encodes one composite (or luma) sample. The composite code mapping also
// decides whether the sample is outside the legal code range, so the caller
// passes has_nonstandard here rather than mapping the sample a second time.
std::int16_t EncodeCompositeSample(OutputSampleEncoding encoding,
                                   SampleFixed composite_mv_fixed,
                                   const QuantizationProfile& profile,
                                   bool* has_nonstandard) {
  if (encoding == OutputSampleEncoding::kRawS16) {
    const int raw_mv = static_cast<int>(
        std::lround(SampleFixedToMillivolts(composite_mv_fixed)));
    const int clamped_raw_mv = std::max(
        static_cast<int>(std::numeric_limits<std::int16_t>::min()),
        std::min(static_cast<int>(std::numeric_limits<std::int16_t>::max()),
                 raw_mv));
    return static_cast<std::int16_t>(clamped_raw_mv);
  }

  const int mapped = MapCompositeFixedToCode(composite_mv_fixed, profile);
  if (has_nonstandard != nullptr && IsNonstandardMappedCode(mapped, profile)) {
    *has_nonstandard = true;
  }
  const int quantized_code = ClampToLegalCodeRange(mapped, profile);
  if (encoding == OutputSampleEncoding::kCvbsTpg21) {
    const int tpg21_encoded = (quantized_code - 508) * 64;
    return static_cast<std::int16_t>(tpg21_encoded);
  }

  if (encoding == OutputSampleEncoding::kCvbsS16Fsc) {
    // S16_4FSC can represent sub-sync excursions (e.g. pilot burst troughs at
    // −600 mV) that lie below the 10-bit legal-code floor. Use the unclamped
    // mapped code and saturate only to prevent int16 overflow.
    const int s16_4fsc_raw = (mapped - profile.blanking_code) * 32;
    const int s16_4fsc_clamped =
        std::clamp(s16_4fsc_raw,
                   static_cast<int>(std::numeric_limits<std::int16_t>::min()),
                   static_cast<int>(std::numeric_limits<std::int16_t>::max()));
    return static_cast<std::int16_t>(s16_4fsc_clamped);
  }

  return static_cast<std::int16_t>(quantized_code);
}

std::int16_t EncodeChromaSample(OutputSampleEncoding encoding,
                                SampleFixed chroma_mv_fixed,
                                const QuantizationProfile& profile) {
  if (encoding == OutputSampleEncoding::kRawS16) {
    const int raw_mv =
        static_cast<int>(std::lround(SampleFixedToMillivolts(chroma_mv_fixed)));
    const int clamped_raw_mv = std::clamp(
        raw_mv, static_cast<int>(std::numeric_limits<std::int16_t>::min()),
        static_cast<int>(std::numeric_limits<std::int16_t>::max()));
    return static_cast<std::int16_t>(clamped_raw_mv);
  }

  const int chroma_code = MapChromaFixedToCode(chroma_mv_fixed, profile);
  const int quantized_code = ClampToLegalCodeRange(chroma_code, profile);

  if (encoding == OutputSampleEncoding::kCvbsTpg21) {
    const int tpg21_encoded = (quantized_code - 508) * 64;
    return static_cast<std::int16_t>(tpg21_encoded);
  }

  if (encoding == OutputSampleEncoding::kCvbsS16Fsc) {
    const int s16_4fsc_raw = (chroma_code - profile.blanking_code) * 32;
    const int s16_4fsc_clamped =
        std::clamp(s16_4fsc_raw,
                   static_cast<int>(std::numeric_limits<std::int16_t>::min()),
                   static_cast<int>(std::numeric_limits<std::int16_t>::max()));
    return static_cast<std::int16_t>(s16_4fsc_clamped);
  }

  return static_cast<std::int16_t>(quantized_code);
}

// Derives the chroma file path from the luma path by replacing the ".cvbsy"
// suffix with ".cvbsc". Returns empty string if video_path does not end in
// ".cvbsy".
std::string DeriveChromaPath(const std::string& luma_path) {
  constexpr std::string_view kLumaSuffix = ".cvbsy";
  constexpr std::string_view kChromaSuffix = ".cvbsc";
  if (luma_path.size() < kLumaSuffix.size()) {
    return {};
  }
  if (luma_path.compare(luma_path.size() - kLumaSuffix.size(),
                        kLumaSuffix.size(), kLumaSuffix) != 0) {
    return {};
  }
  return luma_path.substr(0, luma_path.size() - kLumaSuffix.size()) +
         std::string(kChromaSuffix);
}

bool WriteMetadataDatabase(const Project& project, std::size_t frame_count,
                           const QuantizationProfile& quantization,
                           bool has_nonstandard,
                           std::vector<std::string>* errors, ILogger* logger) {
  const std::string& metadata_path = project.output.metadata_path;

  // Write metadata as SQLite database per CVBS specification.
  std::remove(metadata_path.c_str());

  sqlite3* db = nullptr;
  const int open_result = sqlite3_open(metadata_path.c_str(), &db);
  if (open_result != SQLITE_OK) {
    errors->push_back("Failed to create metadata SQLite database: " +
                      metadata_path);
    if (db != nullptr) {
      sqlite3_close(db);
    }
    return false;
  }

  if (logger != nullptr) {
    logger->Trace("Opened metadata database for writing: " + metadata_path);
  }

  char* error_msg = nullptr;
  // CVBS File Format Specification v1.7.0, SQLite Metadata Schema: version 12
  // adds the SECAM preset and is otherwise identical to version 11.
  if (sqlite3_exec(db, "PRAGMA user_version = 12;", nullptr, nullptr,
                   &error_msg) != SQLITE_OK) {
    errors->push_back(std::string("Failed to set PRAGMA user_version: ") +
                      error_msg);
    sqlite3_free(error_msg);
    sqlite3_close(db);
    return false;
  }

  const char* create_table_sql =
      "CREATE TABLE cvbs_file ("
      "    cvbs_file_id                INTEGER PRIMARY KEY,"
      "    preset                      TEXT    NOT NULL"
      "        CHECK (preset IN ('NTSC', 'PAL', 'PAL_M', 'SECAM')),"
      "    sample_encoding_preset      TEXT    NOT NULL"
      "        CHECK (sample_encoding_preset IN ('CVBS_U10_4FSC', "
      "'CVBS_U16_4FSC', 'RAW_S16_28M', 'RAW_S16_40M', 'CVBS_TPG21_4FSC', "
      "'CVBS_S16_4FSC')),"
      "    signal_state_preset         TEXT    NOT NULL"
      "        CHECK (signal_state_preset IN ("
      "            'STANDARD_STABLE_LOCKED',"
      "            'STANDARD_STABLE_UNLOCKED',"
      "            'STANDARD_RAW',"
      "            'NONSTANDARD_STABLE_LOCKED',"
      "            'NONSTANDARD_STABLE_UNLOCKED',"
      "            'NONSTANDARD_RAW'"
      "        )),"
      "    sequence_continuous         BOOLEAN,"
      "    signal_type                 TEXT    NOT NULL"
      "        CHECK (signal_type IN ('composite', 'yc')),"
      "    decoder                     TEXT    NOT NULL,"
      "    git_branch                  TEXT,"
      "    git_commit                  TEXT,"
      "    number_of_sequential_frames INTEGER"
      "        CHECK (number_of_sequential_frames IS NULL OR "
      "number_of_sequential_frames >= 1),"
      "    black_level                 INTEGER,"
      "    has_nonstandard_values      BOOLEAN,"
      "    capture_notes               TEXT"
      ");";

  if (sqlite3_exec(db, create_table_sql, nullptr, nullptr, &error_msg) !=
      SQLITE_OK) {
    errors->push_back(std::string("Failed to create cvbs_file table: ") +
                      error_msg);
    sqlite3_free(error_msg);
    sqlite3_close(db);
    return false;
  }

  // Per-channel-pair audio metadata (CVBS File Format Specification,
  // audio_channel_pair table): one row per emitted `_audio_<pair>.wav` file.
  const char* create_audio_table_sql =
      "CREATE TABLE audio_channel_pair ("
      "    channel_pair                INTEGER PRIMARY KEY"
      "        CHECK (channel_pair BETWEEN 0 AND 7),"
      "    description                 TEXT"
      ");";

  if (sqlite3_exec(db, create_audio_table_sql, nullptr, nullptr, &error_msg) !=
      SQLITE_OK) {
    errors->push_back(
        std::string("Failed to create audio_channel_pair table: ") + error_msg);
    sqlite3_free(error_msg);
    sqlite3_close(db);
    return false;
  }

  // No EFM table here: the LaserDisc digital audio stream is a separate
  // extension format (CVBS File Format Specification, Producer Extension
  // Metadata) and carries its own `<basename>.efm.meta` sidecar, written by
  // AudioEfmWriter. The core schema has no EFM concept.

  const std::string preset_str =
      StandardToString(project.cvbs_presets.video_standard_preset);
  if (project.cvbs_presets.video_standard_preset == Standard::kUnknown) {
    errors->push_back("Unknown video standard preset");
    sqlite3_close(db);
    return false;
  }

  sqlite3_stmt* insert_stmt = nullptr;
  const char* insert_sql =
      "INSERT INTO cvbs_file ("
      "    preset,"
      "    sample_encoding_preset,"
      "    signal_state_preset,"
      "    sequence_continuous,"
      "    signal_type,"
      "    decoder,"
      "    git_branch,"
      "    git_commit,"
      "    number_of_sequential_frames,"
      "    black_level,"
      "    has_nonstandard_values,"
      "    capture_notes"
      ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";

  if (sqlite3_prepare_v2(db, insert_sql, -1, &insert_stmt, nullptr) !=
      SQLITE_OK) {
    errors->push_back("Failed to prepare insert statement");
    sqlite3_close(db);
    return false;
  }

  sqlite3_bind_text(insert_stmt, 1, preset_str.c_str(), -1, SQLITE_STATIC);
  sqlite3_bind_text(insert_stmt, 2,
                    project.cvbs_presets.sample_encoding_preset.c_str(), -1,
                    SQLITE_STATIC);
  sqlite3_bind_text(insert_stmt, 3,
                    project.cvbs_presets.signal_state_preset.c_str(), -1,
                    SQLITE_STATIC);
  // Synthesised output is one unbroken sequence by construction: subcarrier
  // phase and colour field sequence advance with the output frame index, so
  // continuity is always assertable (CVBS File Format Specification,
  // sequence_continuous).
  sqlite3_bind_int(insert_stmt, 4, 1);
  sqlite3_bind_text(insert_stmt, 5, project.output.signal_type.c_str(), -1,
                    SQLITE_TRANSIENT);
  sqlite3_bind_text(insert_stmt, 6, "videosynth", -1, SQLITE_STATIC);
  sqlite3_bind_null(insert_stmt, 7);
  sqlite3_bind_null(insert_stmt, 8);
  sqlite3_bind_int64(insert_stmt, 9, static_cast<sqlite3_int64>(frame_count));

  const bool has_explicit_black_level_override =
      project.cvbs_presets.video_standard_preset == Standard::kNtsc &&
      project.cvbs_presets.ntsc_black_setup_ire_specified &&
      std::abs(project.cvbs_presets.ntsc_black_setup_ire) < 1e-9;
  if (has_explicit_black_level_override) {
    const SignalLevels levels = GetSignalLevels(project.cvbs_presets);
    const int black_level_code =
        MapCompositeMillivoltsToCode(levels.black_mv, quantization);
    sqlite3_bind_int(insert_stmt, 10, black_level_code);
  } else {
    sqlite3_bind_null(insert_stmt, 10);
  }

  sqlite3_bind_int(insert_stmt, 11, has_nonstandard ? 1 : 0);
  sqlite3_bind_null(insert_stmt, 12);  // capture_notes

  if (sqlite3_step(insert_stmt) != SQLITE_DONE) {
    errors->push_back("Failed to insert metadata row");
    sqlite3_finalize(insert_stmt);
    sqlite3_close(db);
    return false;
  }

  sqlite3_finalize(insert_stmt);

  // One audio_channel_pair row per emitted channel-pair WAV file. Channel-pair
  // numbers are the union of those declared across sections; the description is
  // the first non-empty label recorded for that pair (NULL when none).
  const std::vector<int> channel_pairs = ProjectAudioChannelPairs(project);
  if (!channel_pairs.empty()) {
    sqlite3_stmt* audio_stmt = nullptr;
    const char* audio_insert_sql =
        "INSERT INTO audio_channel_pair (channel_pair, description) "
        "VALUES (?, ?);";
    if (sqlite3_prepare_v2(db, audio_insert_sql, -1, &audio_stmt, nullptr) !=
        SQLITE_OK) {
      errors->push_back("Failed to prepare audio_channel_pair insert");
      sqlite3_close(db);
      return false;
    }
    for (const int pair : channel_pairs) {
      sqlite3_reset(audio_stmt);
      sqlite3_bind_int(audio_stmt, 1, pair);
      const std::string description =
          AudioChannelPairDescription(project, pair);
      if (description.empty()) {
        sqlite3_bind_null(audio_stmt, 2);
      } else {
        sqlite3_bind_text(audio_stmt, 2, description.c_str(), -1,
                          SQLITE_TRANSIENT);
      }
      if (sqlite3_step(audio_stmt) != SQLITE_DONE) {
        errors->push_back("Failed to insert audio_channel_pair row");
        sqlite3_finalize(audio_stmt);
        sqlite3_close(db);
        return false;
      }
    }
    sqlite3_finalize(audio_stmt);
  }

  sqlite3_close(db);
  return true;
}

}  // namespace

bool OutputStage::IsSessionOpen() const { return write_session_open_; }

bool OutputStage::BeginWrite(const Project& project,
                             std::size_t expected_frame_count,
                             std::vector<std::string>* errors) {
  if (errors == nullptr) {
    return false;
  }

  if (IsSessionOpen()) {
    errors->push_back("Output session already open.");
    return false;
  }

  if (logger_ != nullptr) {
    logger_->Debug("Preparing to write output files: video='" +
                   project.output.video_path + "', metadata='" +
                   project.output.metadata_path + "'.");
  }

  const std::string& output_path = project.output.video_path;
  const std::string& metadata_path = project.output.metadata_path;
  if (output_path.empty()) {
    errors->push_back("Output path must not be empty.");
    return false;
  }
  if (metadata_path.empty()) {
    errors->push_back("Metadata path must not be empty.");
    return false;
  }
  if (output_path == metadata_path) {
    errors->push_back("Output and metadata paths must be different files.");
    return false;
  }
  if (!IsSupportedSampleEncodingPreset(
          project.cvbs_presets.sample_encoding_preset)) {
    errors->push_back(
        "Output stage requires a supported sample_encoding_preset.");
    return false;
  }
  const std::string required_signal_state =
      GeneratedSignalStatePreset(project.cvbs_presets.video_standard_preset);
  if (project.cvbs_presets.signal_state_preset != required_signal_state) {
    errors->push_back("Output stage requires signal_state_preset " +
                      required_signal_state + " for this video standard.");
    return false;
  }

  const std::size_t input_frame_span = static_cast<std::size_t>(
      SamplesPerFrame4fsc(project.cvbs_presets.video_standard_preset));
  const std::size_t output_frame_span = SamplesPerFrameForEncodingPreset(
      project.cvbs_presets.video_standard_preset,
      project.cvbs_presets.sample_encoding_preset);
  if (input_frame_span == 0U || output_frame_span == 0U) {
    errors->push_back(
        "Generated sample count does not align to supported output timing.");
    return false;
  }

  // Quantisation profile, sample encoding, and signal type are session
  // invariants: resolve them once here instead of per appended buffer.
  QuantizationProfile quantization;
  if (!BuildQuantizationProfile(project.cvbs_presets.video_standard_preset,
                                &quantization)) {
    errors->push_back(
        "Output stage received unsupported or unknown video standard.");
    return false;
  }

  OutputSampleEncoding output_encoding = OutputSampleEncoding::kCvbsU10;
  if (!ResolveOutputEncoding(project.cvbs_presets.sample_encoding_preset,
                             &output_encoding)) {
    errors->push_back("Output stage does not support sample_encoding_preset: " +
                      project.cvbs_presets.sample_encoding_preset);
    return false;
  }

  // Create parent directories so callers don't need to pre-create them.
  std::error_code ec;
  if (!CreateParentDirectories(output_path, &ec)) {
    errors->push_back("Failed to create output directory for: " + output_path +
                      " (" + ec.message() + ")");
    return false;
  }
  if (!CreateParentDirectories(metadata_path, &ec)) {
    errors->push_back("Failed to create output directory for: " +
                      metadata_path + " (" + ec.message() + ")");
    return false;
  }

  const bool is_yc = (project.output.signal_type == "yc");
  if (is_yc) {
    const std::string chroma_path = DeriveChromaPath(output_path);
    if (chroma_path.empty()) {
      errors->push_back("Y/C output requires video_path to end in '.y': " +
                        output_path);
      return false;
    }
    std::ofstream ystream(output_path, std::ios::binary);
    if (!ystream) {
      errors->push_back("Failed to open luma output file: " + output_path);
      return false;
    }
    std::ofstream cstream(chroma_path, std::ios::binary);
    if (!cstream) {
      errors->push_back("Failed to open chroma output file: " + chroma_path);
      return false;
    }
    current_project_ = project;
    video_stream_ = std::move(ystream);
    chroma_stream_ = std::move(cstream);
    if (logger_ != nullptr) {
      logger_->Trace("Opened Y/C output files for writing: luma=" +
                     output_path + ", chroma=" + chroma_path);
    }
  } else {
    std::ofstream stream(output_path, std::ios::binary);
    if (!stream) {
      errors->push_back("Failed to open output video file: " + output_path);
      return false;
    }
    current_project_ = project;
    video_stream_ = std::move(stream);
    if (logger_ != nullptr) {
      logger_->Trace("Opened output video file for writing: " + output_path);
    }
  }

  expected_frame_count_ = expected_frame_count;
  written_samples_ = 0U;
  input_frame_span_ = input_frame_span;
  output_frame_span_ = output_frame_span;
  has_nonstandard_ = false;
  quantization_ = quantization;
  output_encoding_ = output_encoding;
  is_yc_output_ = is_yc;
  append_scratch_.luma_codes.clear();
  append_scratch_.chroma_codes.clear();
  append_scratch_.has_nonstandard = false;
  write_session_open_ = true;

  return true;
}

bool OutputStage::AppendSamples(const std::vector<SampleFixed>& y_mv,
                                const std::vector<SampleFixed>& c_mv,
                                std::vector<std::string>* errors) {
  if (errors == nullptr) {
    return false;
  }

  if (!IsSessionOpen()) {
    errors->push_back("Output session is not open.");
    return false;
  }

  if (y_mv.size() != c_mv.size()) {
    errors->push_back(
        "Internal error: Y and C sample vectors must be same size.");
    return false;
  }
  if (input_frame_span_ == 0U || (y_mv.size() % input_frame_span_) != 0U) {
    errors->push_back(
        "Generated sample count does not align to supported input timing.");
    return false;
  }

  for (std::size_t frame_start = 0U; frame_start < y_mv.size();
       frame_start += input_frame_span_) {
    EncodeFrameInto(y_mv, c_mv, frame_start, &append_scratch_);
    if (!AppendEncodedFrame(append_scratch_, errors)) {
      return false;
    }
  }

  return true;
}

bool OutputStage::EncodeFrame(const std::vector<SampleFixed>& y_mv,
                              const std::vector<SampleFixed>& c_mv,
                              EncodedFrame* out_frame,
                              std::vector<std::string>* errors) const {
  if (errors == nullptr) {
    return false;
  }
  if (out_frame == nullptr) {
    errors->push_back("Encoded frame output pointer must not be null.");
    return false;
  }
  if (!IsSessionOpen()) {
    errors->push_back("Output session is not open.");
    return false;
  }
  if (y_mv.size() != c_mv.size()) {
    errors->push_back(
        "Internal error: Y and C sample vectors must be same size.");
    return false;
  }
  if (input_frame_span_ == 0U || y_mv.size() != input_frame_span_) {
    errors->push_back(
        "Encoded frame requires exactly one frame of generated samples.");
    return false;
  }

  EncodeFrameInto(y_mv, c_mv, 0U, out_frame);
  return true;
}

void OutputStage::EncodeFrameInto(const std::vector<SampleFixed>& y_mv,
                                  const std::vector<SampleFixed>& c_mv,
                                  std::size_t frame_start,
                                  EncodedFrame* out_frame) const {
  // Resampling scratch for the RAW_S16 presets, whose output frame span
  // differs from the 4fsc input span. Thread-local so concurrent encoders each
  // keep their own buffers while steady-state encoding still allocates nothing.
  thread_local std::vector<SampleFixed> composite_scratch;
  thread_local std::vector<SampleFixed> resampled_luma;
  thread_local std::vector<SampleFixed> resampled_chroma;

  const bool needs_resample = output_frame_span_ != input_frame_span_;
  out_frame->has_nonstandard = false;

  if (is_yc_output_) {
    const SampleFixed* luma = y_mv.data() + frame_start;
    const SampleFixed* chroma = c_mv.data() + frame_start;
    if (needs_resample) {
      ResampleFrameInto(luma, input_frame_span_, output_frame_span_,
                        &resampled_luma);
      ResampleFrameInto(chroma, input_frame_span_, output_frame_span_,
                        &resampled_chroma);
      luma = resampled_luma.data();
      chroma = resampled_chroma.data();
    }

    out_frame->luma_codes.resize(output_frame_span_);
    out_frame->chroma_codes.resize(output_frame_span_);
    for (std::size_t i = 0; i < output_frame_span_; ++i) {
      out_frame->luma_codes[i] =
          EncodeCompositeSample(output_encoding_, luma[i], quantization_,
                                &out_frame->has_nonstandard);
      out_frame->chroma_codes[i] =
          EncodeChromaSample(output_encoding_, chroma[i], quantization_);
    }
    return;
  }

  out_frame->chroma_codes.clear();

  const SampleFixed* composite = nullptr;
  if (needs_resample) {
    composite_scratch.resize(input_frame_span_);
    for (std::size_t i = 0; i < input_frame_span_; ++i) {
      composite_scratch[i] = y_mv[frame_start + i] + c_mv[frame_start + i];
    }
    ResampleFrameInto(composite_scratch.data(), input_frame_span_,
                      output_frame_span_, &resampled_luma);
    composite = resampled_luma.data();
  }

  out_frame->luma_codes.resize(output_frame_span_);
  if (composite != nullptr) {
    for (std::size_t i = 0; i < output_frame_span_; ++i) {
      out_frame->luma_codes[i] =
          EncodeCompositeSample(output_encoding_, composite[i], quantization_,
                                &out_frame->has_nonstandard);
    }
    return;
  }

  // Common path: the output span matches the 4fsc input span, so the composite
  // sum is formed straight into the code buffer.
  const SampleFixed* luma = y_mv.data() + frame_start;
  const SampleFixed* chroma = c_mv.data() + frame_start;
  for (std::size_t i = 0; i < output_frame_span_; ++i) {
    out_frame->luma_codes[i] =
        EncodeCompositeSample(output_encoding_, luma[i] + chroma[i],
                              quantization_, &out_frame->has_nonstandard);
  }
}

bool OutputStage::AppendEncodedFrame(const EncodedFrame& frame,
                                     std::vector<std::string>* errors) {
  if (errors == nullptr) {
    return false;
  }

  if (!IsSessionOpen()) {
    errors->push_back("Output session is not open.");
    return false;
  }

  if (frame.luma_codes.size() != output_frame_span_ ||
      (is_yc_output_ && frame.chroma_codes.size() != output_frame_span_)) {
    errors->push_back(
        "Encoded frame does not match the session's output frame span.");
    return false;
  }

  has_nonstandard_ = has_nonstandard_ || frame.has_nonstandard;

  video_stream_.write(reinterpret_cast<const char*>(frame.luma_codes.data()),
                      static_cast<std::streamsize>(frame.luma_codes.size() *
                                                   sizeof(std::int16_t)));
  if (is_yc_output_) {
    chroma_stream_.write(
        reinterpret_cast<const char*>(frame.chroma_codes.data()),
        static_cast<std::streamsize>(frame.chroma_codes.size() *
                                     sizeof(std::int16_t)));
  }

  if (!video_stream_ || (is_yc_output_ && !chroma_stream_)) {
    errors->push_back("Failed while writing output video samples.");
    return false;
  }

  written_samples_ += output_frame_span_;
  return true;
}

bool OutputStage::FinalizeWrite(std::vector<std::string>* errors) {
  if (errors == nullptr) {
    return false;
  }

  if (!IsSessionOpen()) {
    errors->push_back("Output session is not open.");
    return false;
  }

  const std::size_t expected_samples =
      expected_frame_count_ * output_frame_span_;
  if (written_samples_ != expected_samples) {
    errors->push_back(
        "Output session sample count mismatch before finalization.");
    video_stream_.close();
    if (chroma_stream_.is_open()) {
      chroma_stream_.close();
    }
    write_session_open_ = false;
    return false;
  }

  video_stream_.close();
  if (chroma_stream_.is_open()) {
    chroma_stream_.close();
  }

  if (!WriteMetadataDatabase(current_project_, expected_frame_count_,
                             quantization_, has_nonstandard_, errors,
                             logger_)) {
    write_session_open_ = false;
    return false;
  }

  if (logger_ != nullptr) {
    logger_->Info("Wrote " + std::to_string(expected_frame_count_) +
                  " frame(s) to output files.");
  }

  write_session_open_ = false;
  return true;
}

void OutputStage::AbortWrite() {
  if (!IsSessionOpen()) {
    return;
  }

  video_stream_.close();
  if (chroma_stream_.is_open()) {
    chroma_stream_.close();
  }

  // Removal failures are ignored: the session is discarded either way and a
  // best-effort cleanup is all a cancelled run requires.
  std::error_code ec;
  std::filesystem::remove(current_project_.output.video_path, ec);
  const std::string chroma_path =
      DeriveChromaPath(current_project_.output.video_path);
  if (current_project_.output.signal_type == "yc" && !chroma_path.empty()) {
    std::filesystem::remove(chroma_path, ec);
  }
  std::filesystem::remove(current_project_.output.metadata_path, ec);

  if (logger_ != nullptr) {
    logger_->Info("Output write session aborted; removed in-progress files.");
  }

  write_session_open_ = false;
  written_samples_ = 0U;
}

bool OutputStage::Write(const Project& project,
                        const std::vector<SampleFixed>& y_mv,
                        const std::vector<SampleFixed>& c_mv,
                        std::vector<std::string>* errors) {
  if (errors == nullptr) {
    return false;
  }

  if (y_mv.size() != c_mv.size()) {
    errors->push_back(
        "Internal error: Y and C sample vectors must be same size.");
    return false;
  }

  const std::size_t frame_span = static_cast<std::size_t>(
      SamplesPerFrame4fsc(project.cvbs_presets.video_standard_preset));
  if (frame_span == 0U || (y_mv.size() % frame_span) != 0U) {
    errors->push_back(
        "Generated sample count does not align to whole-frame 4fsc timing.");
    return false;
  }

  const std::size_t frame_count = y_mv.size() / frame_span;
  if (!BeginWrite(project, frame_count, errors)) {
    return false;
  }
  if (!AppendSamples(y_mv, c_mv, errors)) {
    write_session_open_ = false;
    video_stream_.close();
    if (chroma_stream_.is_open()) {
      chroma_stream_.close();
    }
    return false;
  }
  return FinalizeWrite(errors);
}

}  // namespace videosynth
