/*
 * File:        test_output_stage.cpp
 * Module:      output_stage_tests
 * Purpose:     Validates composite quantization, metadata, and output
 * constraints.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "videosynth/fixed_point.h"
#include "videosynth/output_stage.h"
#include "videosynth/timing_constants.h"

namespace videosynth {
namespace {

Project MakeProject(Standard standard) {
  Project project;
  project.cvbs_presets.video_standard_preset = standard;
  project.cvbs_presets.sample_encoding_preset = "CVBS_U10_4FSC";
  project.cvbs_presets.signal_state_preset = "STANDARD_STABLE_LOCKED";
  return project;
}

std::vector<std::int16_t> ReadSamples(const std::filesystem::path& path,
                                      std::size_t count) {
  std::ifstream stream(path, std::ios::binary);
  std::vector<std::int16_t> samples(count, 0);
  stream.read(reinterpret_cast<char*>(samples.data()),
              static_cast<std::streamsize>(count * sizeof(std::int16_t)));
  return samples;
}

int QuantizePalReferenceCode(double composite_mv) {
  constexpr double kMillivoltsPerCode = 1.1905;
  constexpr int kBlankingCode = 256;
  constexpr int kMinCode = 4;
  constexpr int kMaxCode = 1019;
  const int mapped =
      static_cast<int>(std::lround(composite_mv / kMillivoltsPerCode)) +
      kBlankingCode;
  return std::max(kMinCode, std::min(kMaxCode, mapped));
}

struct CvbsMetadata {
  std::string preset;
  std::string sample_encoding_preset;
  std::string signal_state_preset;
  bool has_sequence_continuous = false;
  bool sequence_continuous = false;
  std::string signal_type;
  std::string decoder;
  int64_t number_of_sequential_frames = 0;
  int32_t black_level = 0;
  bool has_black_level = false;
  bool has_nonstandard_values = false;
};

struct AudioChannelPairRow {
  int channel_pair = 0;
  bool description_is_null = true;
  std::string description;
};

bool ReadCvbsMetadata(const std::filesystem::path& path,
                      CvbsMetadata* metadata) {
  if (metadata == nullptr) {
    return false;
  }

  sqlite3* db = nullptr;
  if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) {
    sqlite3_close(db);
    return false;
  }

  const char* query_sql =
      "SELECT preset, sample_encoding_preset, signal_state_preset, "
      "sequence_continuous, signal_type, decoder, "
      "number_of_sequential_frames, black_level, "
      "has_nonstandard_values FROM cvbs_file LIMIT 1;";
  sqlite3_stmt* stmt = nullptr;

  if (sqlite3_prepare_v2(db, query_sql, -1, &stmt, nullptr) != SQLITE_OK) {
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return false;
  }

  bool result = false;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    metadata->preset = std::string(
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)));
    metadata->sample_encoding_preset = std::string(
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)));
    metadata->signal_state_preset = std::string(
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2)));
    metadata->has_sequence_continuous =
        sqlite3_column_type(stmt, 3) != SQLITE_NULL;
    if (metadata->has_sequence_continuous) {
      metadata->sequence_continuous = sqlite3_column_int(stmt, 3) != 0;
    }
    metadata->signal_type = std::string(
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4)));
    metadata->decoder = std::string(
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5)));
    metadata->number_of_sequential_frames = sqlite3_column_int64(stmt, 6);
    metadata->has_black_level = sqlite3_column_type(stmt, 7) != SQLITE_NULL;
    if (metadata->has_black_level) {
      metadata->black_level = sqlite3_column_int(stmt, 7);
    }
    metadata->has_nonstandard_values = sqlite3_column_int(stmt, 8) != 0;
    result = true;
  }

  sqlite3_finalize(stmt);
  sqlite3_close(db);
  return result;
}

// Reads every audio_channel_pair row, ordered by channel_pair.
std::vector<AudioChannelPairRow> ReadAudioChannelPairs(
    const std::filesystem::path& path) {
  std::vector<AudioChannelPairRow> rows;
  sqlite3* db = nullptr;
  if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) {
    sqlite3_close(db);
    return rows;
  }
  const char* query_sql =
      "SELECT channel_pair, description FROM audio_channel_pair "
      "ORDER BY channel_pair;";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, query_sql, -1, &stmt, nullptr) == SQLITE_OK) {
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      AudioChannelPairRow row;
      row.channel_pair = sqlite3_column_int(stmt, 0);
      row.description_is_null = sqlite3_column_type(stmt, 1) == SQLITE_NULL;
      if (!row.description_is_null) {
        row.description = std::string(
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)));
      }
      rows.push_back(row);
    }
  }
  sqlite3_finalize(stmt);
  sqlite3_close(db);
  return rows;
}

// Names of the tables present in a metadata database, excluding the internal
// SQLite ones.
std::vector<std::string> ReadTableNames(const std::filesystem::path& path) {
  std::vector<std::string> names;
  sqlite3* db = nullptr;
  if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) {
    sqlite3_close(db);
    return names;
  }
  const char* query_sql =
      "SELECT name FROM sqlite_master WHERE type = 'table' "
      "AND name NOT LIKE 'sqlite_%' ORDER BY name;";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, query_sql, -1, &stmt, nullptr) == SQLITE_OK) {
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      names.emplace_back(
          reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)));
    }
  }
  sqlite3_finalize(stmt);
  sqlite3_close(db);
  return names;
}

TEST(OutputStageTest, WritesCompositeSamplesUsingPalQuantizationProfile) {
  OutputStage output;
  Project project = MakeProject(Standard::kPal);
  const std::size_t frame_span =
      static_cast<std::size_t>(SamplesPerFrame4fsc(Standard::kPal));

  std::vector<SampleFixed> y(frame_span, MillivoltsToSampleFixed(0.0));
  std::vector<SampleFixed> c(frame_span, MillivoltsToSampleFixed(0.0));
  y[0] = MillivoltsToSampleFixed(0.0);
  y[1] = MillivoltsToSampleFixed(700.0);
  y[2] = MillivoltsToSampleFixed(-300.0);

  const std::filesystem::path video_path =
      std::filesystem::temp_directory_path() /
      "videosynth_output_stage_pal.cvbs";
  const std::filesystem::path metadata_path =
      std::filesystem::temp_directory_path() /
      "videosynth_output_stage_pal.meta";
  project.output.video_path = video_path.string();
  project.output.metadata_path = metadata_path.string();
  std::filesystem::remove(video_path);
  std::filesystem::remove(metadata_path);

  std::vector<std::string> errors;
  ASSERT_TRUE(output.Write(project, y, c, &errors));

  const std::vector<std::int16_t> samples = ReadSamples(video_path, 3);
  ASSERT_EQ(samples.size(), 3U);
  EXPECT_EQ(samples[0], 256);
  EXPECT_EQ(samples[1], 844);
  EXPECT_EQ(samples[2], 4);

  CvbsMetadata metadata;
  ASSERT_TRUE(ReadCvbsMetadata(metadata_path, &metadata));
  EXPECT_EQ(metadata.preset, "PAL");
  EXPECT_EQ(metadata.signal_type, "composite");
  EXPECT_EQ(metadata.sample_encoding_preset, "CVBS_U10_4FSC");
  EXPECT_EQ(metadata.signal_state_preset, "STANDARD_STABLE_LOCKED");
  ASSERT_TRUE(metadata.has_sequence_continuous);
  EXPECT_TRUE(metadata.sequence_continuous);
  EXPECT_EQ(metadata.decoder, "videosynth");
  EXPECT_EQ(metadata.number_of_sequential_frames, 1);
  EXPECT_FALSE(metadata.has_black_level);
  EXPECT_FALSE(metadata.has_nonstandard_values);

  std::filesystem::remove(video_path);
  std::filesystem::remove(metadata_path);
}

// Encoding on a worker thread and writing on the session owner must produce
// exactly the bytes AppendSamples would have written, and must be usable from
// several threads at once — that is the contract the frame synthesis pool
// relies on.
TEST(OutputStageTest, WorkerEncodedFramesMatchAppendSamplesByteForByte) {
  const std::size_t frame_span =
      static_cast<std::size_t>(SamplesPerFrame4fsc(Standard::kPal));
  constexpr std::size_t kFrameCount = 3U;

  // Frames differ from each other and include out-of-range excursions so the
  // nonstandard-value flag is exercised too.
  std::vector<std::vector<SampleFixed>> luma_frames;
  std::vector<std::vector<SampleFixed>> chroma_frames;
  for (std::size_t frame = 0; frame < kFrameCount; ++frame) {
    std::vector<SampleFixed> y(frame_span, MillivoltsToSampleFixed(0.0));
    std::vector<SampleFixed> c(frame_span, MillivoltsToSampleFixed(0.0));
    y[0] = MillivoltsToSampleFixed(100.0 * static_cast<double>(frame + 1U));
    y[1] = MillivoltsToSampleFixed(700.0);
    y[2] = MillivoltsToSampleFixed(-300.0);
    c[3] = MillivoltsToSampleFixed(50.0 * static_cast<double>(frame + 1U));
    luma_frames.push_back(std::move(y));
    chroma_frames.push_back(std::move(c));
  }

  const std::filesystem::path directory =
      std::filesystem::temp_directory_path() / "videosynth_output_encode";
  std::filesystem::create_directories(directory);

  auto RunSession = [&](const std::string& tag, bool encode_on_threads) {
    Project project = MakeProject(Standard::kPal);
    project.output.video_path =
        (directory / ("encode_" + tag + ".cvbs")).string();
    project.output.metadata_path =
        (directory / ("encode_" + tag + ".meta")).string();

    OutputStage output;
    std::vector<std::string> errors;
    EXPECT_TRUE(output.BeginWrite(project, kFrameCount, &errors)) << tag;

    if (encode_on_threads) {
      // Encode every frame concurrently, then write them in order.
      std::vector<EncodedFrame> encoded(kFrameCount);
      std::vector<bool> encoded_ok(kFrameCount, false);
      std::vector<std::thread> workers;
      for (std::size_t frame = 0; frame < kFrameCount; ++frame) {
        workers.emplace_back([&, frame]() {
          std::vector<std::string> job_errors;
          encoded_ok[frame] =
              output.EncodeFrame(luma_frames[frame], chroma_frames[frame],
                                 &encoded[frame], &job_errors);
        });
      }
      for (std::thread& worker : workers) {
        worker.join();
      }
      for (std::size_t frame = 0; frame < kFrameCount; ++frame) {
        EXPECT_TRUE(encoded_ok[frame]) << tag << " frame " << frame;
        errors.clear();
        EXPECT_TRUE(output.AppendEncodedFrame(encoded[frame], &errors)) << tag;
      }
    } else {
      for (std::size_t frame = 0; frame < kFrameCount; ++frame) {
        errors.clear();
        EXPECT_TRUE(output.AppendSamples(luma_frames[frame],
                                         chroma_frames[frame], &errors))
            << tag;
      }
    }

    errors.clear();
    EXPECT_TRUE(output.FinalizeWrite(&errors)) << tag;
    return project.output.video_path;
  };

  const std::string sequential_path = RunSession("sequential", false);
  const std::string threaded_path = RunSession("threaded", true);

  const std::vector<std::int16_t> sequential_samples =
      ReadSamples(sequential_path, kFrameCount * frame_span);
  const std::vector<std::int16_t> threaded_samples =
      ReadSamples(threaded_path, kFrameCount * frame_span);
  EXPECT_EQ(sequential_samples, threaded_samples);

  // The nonstandard-value flag travels with the encoded frame and reaches the
  // session, so the metadata matches too.
  CvbsMetadata sequential_metadata;
  CvbsMetadata threaded_metadata;
  ASSERT_TRUE(ReadCvbsMetadata(
      std::filesystem::path(sequential_path).replace_extension(".meta"),
      &sequential_metadata));
  ASSERT_TRUE(ReadCvbsMetadata(
      std::filesystem::path(threaded_path).replace_extension(".meta"),
      &threaded_metadata));
  EXPECT_EQ(sequential_metadata.has_nonstandard_values,
            threaded_metadata.has_nonstandard_values);

  std::error_code ec;
  std::filesystem::remove_all(directory, ec);
}

TEST(OutputStageTest, EncodeFrameRejectsBuffersThatAreNotOneWholeFrame) {
  const std::size_t frame_span =
      static_cast<std::size_t>(SamplesPerFrame4fsc(Standard::kPal));

  Project project = MakeProject(Standard::kPal);
  const std::filesystem::path directory =
      std::filesystem::temp_directory_path() / "videosynth_output_encode_guard";
  std::filesystem::create_directories(directory);
  project.output.video_path = (directory / "guard.cvbs").string();
  project.output.metadata_path = (directory / "guard.meta").string();

  OutputStage output;
  EncodedFrame encoded;
  std::vector<std::string> errors;

  // Not open yet.
  EXPECT_FALSE(output.EncodeFrame(std::vector<SampleFixed>(frame_span, 0),
                                  std::vector<SampleFixed>(frame_span, 0),
                                  &encoded, &errors));
  EXPECT_FALSE(errors.empty());

  errors.clear();
  ASSERT_TRUE(output.BeginWrite(project, 1U, &errors));

  // Two frames' worth of samples is not one frame.
  errors.clear();
  EXPECT_FALSE(output.EncodeFrame(std::vector<SampleFixed>(frame_span * 2U, 0),
                                  std::vector<SampleFixed>(frame_span * 2U, 0),
                                  &encoded, &errors));
  EXPECT_FALSE(errors.empty());

  // An encoded frame of the wrong length is refused by the writer.
  errors.clear();
  EncodedFrame short_frame;
  short_frame.luma_codes.assign(frame_span - 1U, 0);
  EXPECT_FALSE(output.AppendEncodedFrame(short_frame, &errors));
  EXPECT_FALSE(errors.empty());

  output.AbortWrite();
  std::error_code ec;
  std::filesystem::remove_all(directory, ec);
}

TEST(OutputStageTest, WritesCompositeSamplesUsingTpg21EncodingPreset) {
  OutputStage output;
  Project project = MakeProject(Standard::kPal);
  project.cvbs_presets.sample_encoding_preset = "CVBS_TPG21_4FSC";
  const std::size_t frame_span =
      static_cast<std::size_t>(SamplesPerFrame4fsc(Standard::kPal));

  std::vector<SampleFixed> y(frame_span, MillivoltsToSampleFixed(0.0));
  std::vector<SampleFixed> c(frame_span, MillivoltsToSampleFixed(0.0));
  y[0] = MillivoltsToSampleFixed(0.0);
  y[1] = MillivoltsToSampleFixed(700.0);
  y[2] = MillivoltsToSampleFixed(-300.0);

  const std::filesystem::path video_path =
      std::filesystem::temp_directory_path() /
      "videosynth_output_stage_tpg21.cvbs";
  const std::filesystem::path metadata_path =
      std::filesystem::temp_directory_path() /
      "videosynth_output_stage_tpg21.meta";
  project.output.video_path = video_path.string();
  project.output.metadata_path = metadata_path.string();
  std::filesystem::remove(video_path);
  std::filesystem::remove(metadata_path);

  std::vector<std::string> errors;
  ASSERT_TRUE(output.Write(project, y, c, &errors));

  const std::vector<std::int16_t> samples = ReadSamples(video_path, 3);
  ASSERT_EQ(samples.size(), 3U);
  EXPECT_EQ(samples[0], -16128);
  EXPECT_EQ(samples[1], 21504);
  EXPECT_EQ(samples[2], -32256);

  CvbsMetadata metadata;
  ASSERT_TRUE(ReadCvbsMetadata(metadata_path, &metadata));
  EXPECT_EQ(metadata.sample_encoding_preset, "CVBS_TPG21_4FSC");
  EXPECT_EQ(metadata.preset, "PAL");

  std::filesystem::remove(video_path);
  std::filesystem::remove(metadata_path);
}

TEST(OutputStageTest, WritesCompositeSamplesUsingS16FscEncodingPresetPal) {
  OutputStage output;
  Project project = MakeProject(Standard::kPal);
  project.cvbs_presets.sample_encoding_preset = "CVBS_S16_4FSC";
  const std::size_t frame_span =
      static_cast<std::size_t>(SamplesPerFrame4fsc(Standard::kPal));

  std::vector<SampleFixed> y(frame_span, MillivoltsToSampleFixed(0.0));
  std::vector<SampleFixed> c(frame_span, MillivoltsToSampleFixed(0.0));
  y[0] = MillivoltsToSampleFixed(0.0);
  y[1] = MillivoltsToSampleFixed(700.0);
  y[2] = MillivoltsToSampleFixed(-300.0);
  y[3] = MillivoltsToSampleFixed(-600.0);

  const std::filesystem::path video_path =
      std::filesystem::temp_directory_path() /
      "videosynth_output_stage_s16_4fsc_pal.cvbs";
  const std::filesystem::path metadata_path =
      std::filesystem::temp_directory_path() /
      "videosynth_output_stage_s16_4fsc_pal.meta";
  project.output.video_path = video_path.string();
  project.output.metadata_path = metadata_path.string();
  std::filesystem::remove(video_path);
  std::filesystem::remove(metadata_path);

  std::vector<std::string> errors;
  ASSERT_TRUE(output.Write(project, y, c, &errors));

  // PAL blanking=256, scale=32: int16 = (code - 256) * 32
  // 0 mV   -> code=256 -> 0
  // 700 mV -> code=844 -> 18816
  // -300 mV -> -251.99 codes, rounds to -252 -> code=4 -> -8064
  // -600 mV -> sub-sync, passes through unclamped (pilot burst trough) ->
  // -503.99 codes, rounds to -504 -> -16128
  const std::vector<std::int16_t> samples = ReadSamples(video_path, 4);
  ASSERT_EQ(samples.size(), 4U);
  EXPECT_EQ(samples[0], 0);
  EXPECT_EQ(samples[1], 18816);
  EXPECT_EQ(samples[2], -8064);
  EXPECT_EQ(samples[3], -16128);

  CvbsMetadata metadata;
  ASSERT_TRUE(ReadCvbsMetadata(metadata_path, &metadata));
  EXPECT_EQ(metadata.sample_encoding_preset, "CVBS_S16_4FSC");
  EXPECT_EQ(metadata.preset, "PAL");

  std::filesystem::remove(video_path);
  std::filesystem::remove(metadata_path);
}

TEST(OutputStageTest, WritesCompositeSamplesUsingS16FscEncodingPresetNtsc) {
  OutputStage output;
  Project project = MakeProject(Standard::kNtsc);
  project.cvbs_presets.sample_encoding_preset = "CVBS_S16_4FSC";
  const std::size_t frame_span =
      static_cast<std::size_t>(SamplesPerFrame4fsc(Standard::kNtsc));

  std::vector<SampleFixed> y(frame_span, MillivoltsToSampleFixed(0.0));
  std::vector<SampleFixed> c(frame_span, MillivoltsToSampleFixed(0.0));
  y[0] = MillivoltsToSampleFixed(0.0);
  y[1] = MillivoltsToSampleFixed(700.0);
  y[2] = MillivoltsToSampleFixed(-300.0);

  const std::filesystem::path video_path =
      std::filesystem::temp_directory_path() /
      "videosynth_output_stage_s16_4fsc_ntsc.cvbs";
  const std::filesystem::path metadata_path =
      std::filesystem::temp_directory_path() /
      "videosynth_output_stage_s16_4fsc_ntsc.meta";
  project.output.video_path = video_path.string();
  project.output.metadata_path = metadata_path.string();
  std::filesystem::remove(video_path);
  std::filesystem::remove(metadata_path);

  std::vector<std::string> errors;
  ASSERT_TRUE(output.Write(project, y, c, &errors));

  // NTSC blanking=240, scale=32: int16 = (code - 240) * 32
  // 0 mV -> code=240 -> 0; 700 mV -> code=789 -> 17568; -300 mV -> -235.2
  // codes, rounds to -235 -> code=5 -> -7520
  const std::vector<std::int16_t> samples = ReadSamples(video_path, 3);
  ASSERT_EQ(samples.size(), 3U);
  EXPECT_EQ(samples[0], 0);
  EXPECT_EQ(samples[1], 17568);
  EXPECT_EQ(samples[2], -7520);

  CvbsMetadata metadata;
  ASSERT_TRUE(ReadCvbsMetadata(metadata_path, &metadata));
  EXPECT_EQ(metadata.sample_encoding_preset, "CVBS_S16_4FSC");
  EXPECT_EQ(metadata.preset, "NTSC");

  std::filesystem::remove(video_path);
  std::filesystem::remove(metadata_path);
}

TEST(OutputStageTest, WritesCompositeSamplesUsingUint16EncodingPreset) {
  OutputStage output;
  Project project = MakeProject(Standard::kPal);
  project.cvbs_presets.sample_encoding_preset = "CVBS_U16_4FSC";
  const std::size_t frame_span =
      static_cast<std::size_t>(SamplesPerFrame4fsc(Standard::kPal));

  std::vector<SampleFixed> y(frame_span, MillivoltsToSampleFixed(0.0));
  std::vector<SampleFixed> c(frame_span, MillivoltsToSampleFixed(0.0));
  y[0] = MillivoltsToSampleFixed(0.0);
  y[1] = MillivoltsToSampleFixed(700.0);
  y[2] = MillivoltsToSampleFixed(-300.0);

  const std::filesystem::path video_path =
      std::filesystem::temp_directory_path() /
      "videosynth_output_stage_u16.cvbs";
  const std::filesystem::path metadata_path =
      std::filesystem::temp_directory_path() /
      "videosynth_output_stage_u16.meta";
  project.output.video_path = video_path.string();
  project.output.metadata_path = metadata_path.string();
  std::filesystem::remove(video_path);
  std::filesystem::remove(metadata_path);

  std::vector<std::string> errors;
  ASSERT_TRUE(output.Write(project, y, c, &errors));

  const std::vector<std::int16_t> samples = ReadSamples(video_path, 3);
  ASSERT_EQ(samples.size(), 3U);
  EXPECT_EQ(samples[0], 256);
  EXPECT_EQ(samples[1], 844);
  EXPECT_EQ(samples[2], 4);
  EXPECT_EQ(std::filesystem::file_size(video_path),
            frame_span * sizeof(std::int16_t));

  CvbsMetadata metadata;
  ASSERT_TRUE(ReadCvbsMetadata(metadata_path, &metadata));
  EXPECT_EQ(metadata.sample_encoding_preset, "CVBS_U16_4FSC");

  std::filesystem::remove(video_path);
  std::filesystem::remove(metadata_path);
}

TEST(OutputStageTest, ResamplesRawSamplesUsingTwentyEightMegasamplePreset) {
  OutputStage output;
  Project project = MakeProject(Standard::kPal);
  project.cvbs_presets.sample_encoding_preset = "RAW_S16_28M";
  const std::size_t input_frame_span =
      static_cast<std::size_t>(SamplesPerFrame4fsc(Standard::kPal));
  const std::size_t output_frame_span =
      SamplesPerFrameForEncodingPreset(Standard::kPal, "RAW_S16_28M");

  std::vector<SampleFixed> y(input_frame_span, MillivoltsToSampleFixed(0.0));
  std::vector<SampleFixed> c(input_frame_span, MillivoltsToSampleFixed(0.0));
  for (std::size_t i = 0; i < input_frame_span; ++i) {
    const double ramp = 1000.0 * static_cast<double>(i) /
                        static_cast<double>(input_frame_span - 1U);
    y[i] = MillivoltsToSampleFixed(ramp);
  }

  const std::filesystem::path video_path =
      std::filesystem::temp_directory_path() /
      "videosynth_output_stage_raw_28m.cvbs";
  const std::filesystem::path metadata_path =
      std::filesystem::temp_directory_path() /
      "videosynth_output_stage_raw_28m.meta";
  project.output.video_path = video_path.string();
  project.output.metadata_path = metadata_path.string();
  std::filesystem::remove(video_path);
  std::filesystem::remove(metadata_path);

  std::vector<std::string> errors;
  ASSERT_TRUE(output.Write(project, y, c, &errors));

  EXPECT_EQ(std::filesystem::file_size(video_path),
            output_frame_span * sizeof(std::int16_t));

  CvbsMetadata metadata;
  ASSERT_TRUE(ReadCvbsMetadata(metadata_path, &metadata));
  EXPECT_EQ(metadata.sample_encoding_preset, "RAW_S16_28M");

  std::filesystem::remove(video_path);
  std::filesystem::remove(metadata_path);
}

TEST(OutputStageTest, SumsYAndCBeforeQuantizationInNtscProfile) {
  OutputStage output;
  Project project = MakeProject(Standard::kNtsc);
  const std::size_t frame_span =
      static_cast<std::size_t>(SamplesPerFrame4fsc(Standard::kNtsc));

  std::vector<SampleFixed> y(frame_span, MillivoltsToSampleFixed(0.0));
  std::vector<SampleFixed> c(frame_span, MillivoltsToSampleFixed(0.0));
  c[0] = MillivoltsToSampleFixed(127.55);

  const std::filesystem::path video_path =
      std::filesystem::temp_directory_path() /
      "videosynth_output_stage_ntsc.cvbs";
  const std::filesystem::path metadata_path =
      std::filesystem::temp_directory_path() /
      "videosynth_output_stage_ntsc.meta";
  project.output.video_path = video_path.string();
  project.output.metadata_path = metadata_path.string();
  std::filesystem::remove(video_path);
  std::filesystem::remove(metadata_path);

  std::vector<std::string> errors;
  ASSERT_TRUE(output.Write(project, y, c, &errors));

  const std::vector<std::int16_t> samples = ReadSamples(video_path, 2);
  ASSERT_EQ(samples.size(), 2U);
  EXPECT_EQ(samples[0], 340);
  EXPECT_EQ(samples[1], 240);

  CvbsMetadata metadata;
  ASSERT_TRUE(ReadCvbsMetadata(metadata_path, &metadata));
  EXPECT_EQ(metadata.preset, "NTSC");
  EXPECT_FALSE(metadata.has_black_level);

  std::filesystem::remove(video_path);
  std::filesystem::remove(metadata_path);
}

TEST(OutputStageTest, WritesExplicitBlackLevelOverrideForNtscZeroIreSetup) {
  OutputStage output;
  Project project = MakeProject(Standard::kNtsc);
  project.cvbs_presets.ntsc_black_setup_ire = 0.0;
  project.cvbs_presets.ntsc_black_setup_ire_specified = true;
  const std::size_t frame_span =
      static_cast<std::size_t>(SamplesPerFrame4fsc(Standard::kNtsc));

  std::vector<SampleFixed> y(frame_span, MillivoltsToSampleFixed(0.0));
  std::vector<SampleFixed> c(frame_span, MillivoltsToSampleFixed(0.0));

  const std::filesystem::path video_path =
      std::filesystem::temp_directory_path() /
      "videosynth_output_stage_ntsc_zero_black.cvbs";
  const std::filesystem::path metadata_path =
      std::filesystem::temp_directory_path() /
      "videosynth_output_stage_ntsc_zero_black.meta";
  project.output.video_path = video_path.string();
  project.output.metadata_path = metadata_path.string();
  std::filesystem::remove(video_path);
  std::filesystem::remove(metadata_path);

  std::vector<std::string> errors;
  ASSERT_TRUE(output.Write(project, y, c, &errors));

  CvbsMetadata metadata;
  ASSERT_TRUE(ReadCvbsMetadata(metadata_path, &metadata));
  EXPECT_TRUE(metadata.has_black_level);
  EXPECT_EQ(metadata.black_level, 240);

  std::filesystem::remove(video_path);
  std::filesystem::remove(metadata_path);
}

TEST(OutputStageTest, ClampsOutOfRangeValuesToLegalCodeSpace) {
  OutputStage output;
  Project project = MakeProject(Standard::kPal);
  const std::size_t frame_span =
      static_cast<std::size_t>(SamplesPerFrame4fsc(Standard::kPal));

  std::vector<SampleFixed> y(frame_span, MillivoltsToSampleFixed(0.0));
  std::vector<SampleFixed> c(frame_span, MillivoltsToSampleFixed(0.0));
  y[0] = MillivoltsToSampleFixed(-1000.0);
  y[1] = MillivoltsToSampleFixed(2000.0);

  const std::filesystem::path video_path =
      std::filesystem::temp_directory_path() /
      "videosynth_output_stage_clamp.cvbs";
  const std::filesystem::path metadata_path =
      std::filesystem::temp_directory_path() /
      "videosynth_output_stage_clamp.meta";
  project.output.video_path = video_path.string();
  project.output.metadata_path = metadata_path.string();
  std::filesystem::remove(video_path);
  std::filesystem::remove(metadata_path);

  std::vector<std::string> errors;
  ASSERT_TRUE(output.Write(project, y, c, &errors));

  const std::vector<std::int16_t> samples = ReadSamples(video_path, 2);
  ASSERT_EQ(samples.size(), 2U);
  EXPECT_EQ(samples[0], 4);
  EXPECT_EQ(samples[1], 1019);

  CvbsMetadata metadata;
  ASSERT_TRUE(ReadCvbsMetadata(metadata_path, &metadata));
  EXPECT_TRUE(metadata.has_nonstandard_values);

  std::filesystem::remove(video_path);
  std::filesystem::remove(metadata_path);
}

TEST(OutputStageTest, WritesSchemaVersion12) {
  OutputStage output;
  Project project = MakeProject(Standard::kPal);
  const std::size_t frame_span =
      static_cast<std::size_t>(SamplesPerFrame4fsc(Standard::kPal));

  std::vector<SampleFixed> y(frame_span, MillivoltsToSampleFixed(0.0));
  std::vector<SampleFixed> c(frame_span, MillivoltsToSampleFixed(0.0));

  const std::filesystem::path video_path =
      std::filesystem::temp_directory_path() /
      "videosynth_output_stage_schema_v12.cvbs";
  const std::filesystem::path metadata_path =
      std::filesystem::temp_directory_path() /
      "videosynth_output_stage_schema_v12.meta";
  project.output.video_path = video_path.string();
  project.output.metadata_path = metadata_path.string();
  std::filesystem::remove(video_path);
  std::filesystem::remove(metadata_path);

  std::vector<std::string> errors;
  ASSERT_TRUE(output.Write(project, y, c, &errors));

  // Verify PRAGMA user_version = 12 per the CVBS specification (v1.7.0).
  sqlite3* db = nullptr;
  ASSERT_EQ(sqlite3_open(metadata_path.c_str(), &db), SQLITE_OK);
  int user_version = 0;
  sqlite3_stmt* stmt = nullptr;
  ASSERT_EQ(sqlite3_prepare_v2(db, "PRAGMA user_version;", -1, &stmt, nullptr),
            SQLITE_OK);
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    user_version = sqlite3_column_int(stmt, 0);
  }
  sqlite3_finalize(stmt);
  sqlite3_close(db);
  EXPECT_EQ(user_version, 12);

  // No audio declared → no audio_channel_pair rows.
  EXPECT_TRUE(ReadAudioChannelPairs(metadata_path).empty());

  std::filesystem::remove(video_path);
  std::filesystem::remove(metadata_path);
}

TEST(OutputStageTest, WritesSecamPresetWithUnlockedSignalState) {
  // CVBS File Format Specification v1.7.0, Preset: SECAM: the file declares
  // the SECAM preset and a signal state that is not phase locked.
  OutputStage output;
  Project project = MakeProject(Standard::kSecam);
  project.cvbs_presets.signal_state_preset = "STANDARD_STABLE_UNLOCKED";
  const std::size_t frame_span =
      static_cast<std::size_t>(SamplesPerFrame4fsc(Standard::kSecam));
  ASSERT_EQ(frame_span, 709379U);

  std::vector<SampleFixed> y(frame_span, MillivoltsToSampleFixed(0.0));
  std::vector<SampleFixed> c(frame_span, MillivoltsToSampleFixed(0.0));

  const std::filesystem::path video_path =
      std::filesystem::temp_directory_path() /
      "videosynth_output_stage_secam.cvbs";
  const std::filesystem::path metadata_path =
      std::filesystem::temp_directory_path() /
      "videosynth_output_stage_secam.meta";
  project.output.video_path = video_path.string();
  project.output.metadata_path = metadata_path.string();
  std::filesystem::remove(video_path);
  std::filesystem::remove(metadata_path);

  std::vector<std::string> errors;
  ASSERT_TRUE(output.Write(project, y, c, &errors));

  CvbsMetadata metadata;
  ASSERT_TRUE(ReadCvbsMetadata(metadata_path, &metadata));
  EXPECT_EQ(metadata.preset, "SECAM");
  EXPECT_EQ(metadata.signal_state_preset, "STANDARD_STABLE_UNLOCKED");
  EXPECT_FALSE(metadata.has_black_level);

  // PAL and SECAM share the 10-bit level table: blanking = 256.
  const std::vector<std::int16_t> samples = ReadSamples(video_path, 1);
  EXPECT_EQ(samples[0], 256);

  std::filesystem::remove(video_path);
  std::filesystem::remove(metadata_path);
}

TEST(OutputStageTest, RejectsPhaseLockedSignalStateForSecam) {
  OutputStage output;
  Project project = MakeProject(Standard::kSecam);
  const std::size_t frame_span =
      static_cast<std::size_t>(SamplesPerFrame4fsc(Standard::kSecam));
  std::vector<SampleFixed> y(frame_span, MillivoltsToSampleFixed(0.0));
  std::vector<SampleFixed> c(frame_span, MillivoltsToSampleFixed(0.0));
  project.output.video_path = (std::filesystem::temp_directory_path() /
                               "videosynth_output_stage_secam_locked.cvbs")
                                  .string();
  project.output.metadata_path = (std::filesystem::temp_directory_path() /
                                  "videosynth_output_stage_secam_locked.meta")
                                     .string();

  std::vector<std::string> errors;
  EXPECT_FALSE(output.Write(project, y, c, &errors));
  ASSERT_FALSE(errors.empty());
  EXPECT_NE(errors.front().find("STANDARD_STABLE_UNLOCKED"), std::string::npos);
}

TEST(OutputStageTest, RejectsInvalidOutputConstraints) {
  OutputStage output;
  Project project = MakeProject(Standard::kPal);

  std::vector<SampleFixed> y(16, MillivoltsToSampleFixed(0.0));
  std::vector<SampleFixed> c(16, MillivoltsToSampleFixed(0.0));
  std::vector<std::string> errors;

  const std::filesystem::path video_path =
      std::filesystem::temp_directory_path() /
      "videosynth_output_stage_invalid.cvbs";
  const std::filesystem::path metadata_path =
      std::filesystem::temp_directory_path() /
      "videosynth_output_stage_invalid.meta";
  project.output.video_path = video_path.string();
  project.output.metadata_path = metadata_path.string();

  EXPECT_FALSE(output.Write(project, y, c, &errors));
  EXPECT_FALSE(errors.empty());

  errors.clear();
  project.cvbs_presets.sample_encoding_preset = "NOT_A_SUPPORTED_PRESET";
  const std::size_t frame_span =
      static_cast<std::size_t>(SamplesPerFrame4fsc(Standard::kPal));
  y.assign(frame_span, MillivoltsToSampleFixed(0.0));
  c.assign(frame_span, MillivoltsToSampleFixed(0.0));
  EXPECT_FALSE(output.Write(project, y, c, &errors));
  EXPECT_FALSE(errors.empty());
}

TEST(OutputStageTest, KeepsFixedPointQuantizationEquivalentToReferenceProfile) {
  OutputStage output;
  Project project = MakeProject(Standard::kPal);
  const std::size_t frame_span =
      static_cast<std::size_t>(SamplesPerFrame4fsc(Standard::kPal));

  std::vector<SampleFixed> y(frame_span, MillivoltsToSampleFixed(0.0));
  std::vector<SampleFixed> c(frame_span, MillivoltsToSampleFixed(0.0));
  constexpr std::size_t kWindowSamples = 4096;
  ASSERT_LE(kWindowSamples, frame_span);

  for (std::size_t i = 0; i < kWindowSamples; ++i) {
    const double ramp = -900.0 + (2200.0 * static_cast<double>(i) /
                                  static_cast<double>(kWindowSamples - 1));
    y[i] = MillivoltsToSampleFixed(ramp);
    c[i] =
        MillivoltsToSampleFixed(75.0 * std::sin(static_cast<double>(i) * 0.17));
  }

  const std::filesystem::path video_path =
      std::filesystem::temp_directory_path() /
      "videosynth_output_stage_fixed_equiv.cvbs";
  const std::filesystem::path metadata_path =
      std::filesystem::temp_directory_path() /
      "videosynth_output_stage_fixed_equiv.meta";
  project.output.video_path = video_path.string();
  project.output.metadata_path = metadata_path.string();
  std::filesystem::remove(video_path);
  std::filesystem::remove(metadata_path);

  std::vector<std::string> errors;
  ASSERT_TRUE(output.Write(project, y, c, &errors));

  const std::vector<std::int16_t> samples =
      ReadSamples(video_path, kWindowSamples);
  ASSERT_EQ(samples.size(), kWindowSamples);

  int max_abs_delta = 0;
  double sum_squared = 0.0;
  for (std::size_t i = 0; i < kWindowSamples; ++i) {
    const int expected =
        QuantizePalReferenceCode(SampleFixedToMillivolts(y[i] + c[i]));
    const int observed = static_cast<int>(samples[i]);
    const int delta = observed - expected;
    max_abs_delta = std::max(max_abs_delta, std::abs(delta));
    sum_squared += static_cast<double>(delta * delta);
  }

  const double rms_error =
      std::sqrt(sum_squared / static_cast<double>(kWindowSamples));
  EXPECT_LE(max_abs_delta, 1);
  EXPECT_LT(rms_error, 0.5);

  std::filesystem::remove(video_path);
  std::filesystem::remove(metadata_path);
}

TEST(OutputStageTest, NoAudioChannelPairRowsWhenNoAudio) {
  OutputStage output;
  Project project = MakeProject(Standard::kPal);
  const std::size_t frame_span =
      static_cast<std::size_t>(SamplesPerFrame4fsc(Standard::kPal));
  std::vector<SampleFixed> y(frame_span, MillivoltsToSampleFixed(0.0));
  std::vector<SampleFixed> c(frame_span, MillivoltsToSampleFixed(0.0));

  const std::filesystem::path video_path =
      std::filesystem::temp_directory_path() /
      "videosynth_output_stage_audio_null.cvbs";
  const std::filesystem::path metadata_path =
      std::filesystem::temp_directory_path() /
      "videosynth_output_stage_audio_null.meta";
  project.output.video_path = video_path.string();
  project.output.metadata_path = metadata_path.string();
  std::filesystem::remove(video_path);
  std::filesystem::remove(metadata_path);

  std::vector<std::string> errors;
  ASSERT_TRUE(output.Write(project, y, c, &errors));

  EXPECT_TRUE(ReadAudioChannelPairs(metadata_path).empty());

  std::filesystem::remove(video_path);
  std::filesystem::remove(metadata_path);
}

TEST(OutputStageTest, WritesAudioChannelPairRowsForDeclaredPairs) {
  OutputStage output;
  Project project = MakeProject(Standard::kPal);
  Section section;
  section.name = "Tone";
  AudioChannelPair pair0;
  pair0.pair = 0;
  pair0.pair_specified = true;
  pair0.description = "Analogue stereo";
  pair0.left.enabled = true;
  AudioChannelPair pair3;
  pair3.pair = 3;
  pair3.pair_specified = true;
  // No description → NULL in the metadata.
  pair3.left.enabled = true;
  section.audio_channel_pairs = {pair0, pair3};
  project.sections.push_back(section);

  const std::size_t frame_span =
      static_cast<std::size_t>(SamplesPerFrame4fsc(Standard::kPal));
  std::vector<SampleFixed> y(frame_span, MillivoltsToSampleFixed(0.0));
  std::vector<SampleFixed> c(frame_span, MillivoltsToSampleFixed(0.0));

  const std::filesystem::path video_path =
      std::filesystem::temp_directory_path() /
      "videosynth_output_stage_audio_pairs.cvbs";
  const std::filesystem::path metadata_path =
      std::filesystem::temp_directory_path() /
      "videosynth_output_stage_audio_pairs.meta";
  project.output.video_path = video_path.string();
  project.output.metadata_path = metadata_path.string();
  std::filesystem::remove(video_path);
  std::filesystem::remove(metadata_path);

  std::vector<std::string> errors;
  ASSERT_TRUE(output.Write(project, y, c, &errors));

  const std::vector<AudioChannelPairRow> rows =
      ReadAudioChannelPairs(metadata_path);
  ASSERT_EQ(rows.size(), 2U);
  EXPECT_EQ(rows[0].channel_pair, 0);
  EXPECT_FALSE(rows[0].description_is_null);
  EXPECT_EQ(rows[0].description, "Analogue stereo");
  EXPECT_EQ(rows[1].channel_pair, 3);
  EXPECT_TRUE(rows[1].description_is_null);

  std::filesystem::remove(video_path);
  std::filesystem::remove(metadata_path);
}

TEST(OutputStageTest, DoesNotWriteEfmMetadataIntoTheCoreDatabase) {
  OutputStage output;
  Project project = MakeProject(Standard::kPal);
  Section section;
  section.name = "Tone";
  AudioChannelPair pair2;
  pair2.pair = 2;
  pair2.pair_specified = true;
  pair2.left.enabled = true;
  section.audio_channel_pairs = {pair2};
  project.sections.push_back(section);
  project.output.efm_audio.enabled = true;
  project.output.efm_audio.pair = 2;

  const std::size_t frame_span =
      static_cast<std::size_t>(SamplesPerFrame4fsc(Standard::kPal));
  std::vector<SampleFixed> y(frame_span, MillivoltsToSampleFixed(0.0));
  std::vector<SampleFixed> c(frame_span, MillivoltsToSampleFixed(0.0));

  const std::filesystem::path video_path =
      std::filesystem::temp_directory_path() /
      "videosynth_output_stage_efm.cvbs";
  const std::filesystem::path metadata_path =
      std::filesystem::temp_directory_path() /
      "videosynth_output_stage_efm.meta";
  project.output.video_path = video_path.string();
  project.output.metadata_path = metadata_path.string();
  std::filesystem::remove(video_path);
  std::filesystem::remove(metadata_path);

  std::vector<std::string> errors;
  ASSERT_TRUE(output.Write(project, y, c, &errors));

  // EFM is a separate extension format with its own `<basename>.efm.meta`
  // sidecar, so the core database carries only the core tables even when EFM
  // output is selected.
  const std::vector<std::string> tables = ReadTableNames(metadata_path);
  const std::vector<std::string> expected_tables = {"audio_channel_pair",
                                                    "cvbs_file"};
  EXPECT_EQ(tables, expected_tables);

  std::filesystem::remove(video_path);
  std::filesystem::remove(metadata_path);
}

}  // namespace
}  // namespace videosynth
