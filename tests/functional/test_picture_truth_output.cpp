/*
 * File:        test_picture_truth_output.cpp
 * Module:      picture_truth_tests
 * Purpose:     Runs the decoder-stress fixtures through the full pipeline and
 *              checks the picture truth files: size, sidecar, thread-count
 *              independence, and the line-alternating stress bars.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "fixture_paths.h"
#include "videosynth/audio_track_generator.h"
#include "videosynth/dropout_injection_stage.h"
#include "videosynth/generation_stage.h"
#include "videosynth/noise_injection_stage.h"
#include "videosynth/output_stage.h"
#include "videosynth/path_resolution.h"
#include "videosynth/picture_truth.h"
#include "videosynth/picture_truth_writer.h"
#include "videosynth/pipeline.h"
#include "videosynth/progressive_frame_source_probe.h"
#include "videosynth/project_validator.h"
#include "videosynth/yaml_project_parser.h"

namespace videosynth {
namespace {

class SilentLogger final : public ILogger {
 public:
  void Info(const std::string&) override {}
  void Warning(const std::string&) override {}
  void Error(const std::string&) override {}
  void Debug(const std::string&) override {}
  void Trace(const std::string&) override {}
};

std::vector<char> ReadFileBytes(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  return std::vector<char>((std::istreambuf_iterator<char>(stream)),
                           std::istreambuf_iterator<char>());
}

// Runs a fixture through the pipeline as the CLI does, writing through the
// {output} root into output_dir. Returns the resolved project.
Project RunFixture(const std::string& fixture_name,
                   const std::filesystem::path& output_dir, int threads) {
  AssetRootMap roots = DefaultAssetRoots();
  roots.roots["output"] = output_dir.string();
  const std::string fixture_path = test_support::FixturePath(fixture_name);

  SilentLogger logger;
  YamlProjectParser parser(&logger);
  const ParseResult parsed = parser.ParseFile(fixture_path);
  EXPECT_TRUE(parsed.ok) << fixture_path;
  const Project project = ResolveProjectPaths(
      parsed.project, roots, test_support::FixtureProjectDir(fixture_name),
      /*anchor_unset=*/false);

  ProgressiveFrameSourceProbe probe;
  ProjectValidator validator(&probe, &logger);
  GenerationStage generation(&logger);
  NoiseInjectionStage noise_injection(&logger);
  DropoutInjectionStage dropout_injection(&logger);
  OutputStage output(&logger);
  AudioTrackGenerator audio_generator(&logger);
  VideoSynthPipeline pipeline(&parser, &validator, &generation,
                              &noise_injection, &dropout_injection, &output,
                              &logger, &audio_generator);
  RunOptions options;
  options.project_path = fixture_path;
  options.asset_roots = roots;
  options.threads = threads;
  EXPECT_TRUE(pipeline.Run(options)) << fixture_path;
  return project;
}

// Reads the single truth_file row of a truth sidecar and checks it against
// the layout and frame count.
void ExpectSidecarDescribes(const std::string& sidecar_path,
                            const PictureTruthLayout& layout, int frames) {
  sqlite3* db = nullptr;
  ASSERT_EQ(
      sqlite3_open_v2(sidecar_path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr),
      SQLITE_OK)
      << sidecar_path;
  sqlite3_stmt* version = nullptr;
  ASSERT_EQ(
      sqlite3_prepare_v2(db, "PRAGMA user_version;", -1, &version, nullptr),
      SQLITE_OK);
  ASSERT_EQ(sqlite3_step(version), SQLITE_ROW);
  EXPECT_EQ(sqlite3_column_int(version, 0), kPictureTruthSidecarSchemaVersion);
  sqlite3_finalize(version);

  sqlite3_stmt* row = nullptr;
  ASSERT_EQ(sqlite3_prepare_v2(
                db,
                "SELECT video_standard, sample_rate_hz, frame_count, "
                "samples_per_line, rows, first_sample, first_line_field1, "
                "first_line_field2, upper_field, half_picture_line, "
                "half_picture_end_sample, plane_order, sample_format, units, "
                "y_reference, black_mv, white_mv, u_axis, v_axis, v_switch, "
                "(SELECT COUNT(*) FROM truth_file) FROM truth_file;",
                -1, &row, nullptr),
            SQLITE_OK);
  ASSERT_EQ(sqlite3_step(row), SQLITE_ROW);
  auto text = [&](int column) {
    return std::string(
        reinterpret_cast<const char*>(sqlite3_column_text(row, column)));
  };
  EXPECT_EQ(text(0), StandardToString(layout.standard));
  EXPECT_DOUBLE_EQ(sqlite3_column_double(row, 1), layout.sample_rate_hz);
  EXPECT_EQ(sqlite3_column_int(row, 2), frames);
  EXPECT_EQ(sqlite3_column_int(row, 3), layout.samples_per_line);
  EXPECT_EQ(sqlite3_column_int(row, 4), layout.rows);
  EXPECT_EQ(sqlite3_column_int(row, 5), layout.first_sample);
  EXPECT_EQ(sqlite3_column_int(row, 6), layout.first_line_field1);
  EXPECT_EQ(sqlite3_column_int(row, 7), layout.first_line_field2);
  EXPECT_EQ(sqlite3_column_int(row, 8), layout.upper_field);
  EXPECT_EQ(sqlite3_column_int(row, 9), layout.half_picture_line);
  EXPECT_EQ(sqlite3_column_int(row, 10), layout.half_picture_end_sample);
  EXPECT_EQ(text(11), "YUV");
  EXPECT_EQ(text(12), "float32le");
  EXPECT_EQ(text(13), "mV");
  EXPECT_EQ(text(14), "blanking");
  EXPECT_DOUBLE_EQ(sqlite3_column_double(row, 15), layout.black_mv);
  EXPECT_DOUBLE_EQ(sqlite3_column_double(row, 16), layout.white_mv);
  EXPECT_EQ(text(17), "sin");
  EXPECT_EQ(text(18), "cos");
  EXPECT_EQ(text(19), "removed");
  EXPECT_EQ(sqlite3_column_int(row, 20), 1);
  sqlite3_finalize(row);
  sqlite3_close(db);
}

int FrameCount(const Project& project) {
  int frames = 0;
  for (const Section& section : project.sections) {
    frames += section.duration_frames;
  }
  return frames;
}

float ReadFloat(const std::vector<char>& bytes, std::size_t index) {
  std::uint32_t bits = 0;
  for (int b = 3; b >= 0; --b) {
    bits =
        (bits << 8U) | static_cast<std::uint8_t>(
                           bytes[(index * 4U) + static_cast<std::size_t>(b)]);
  }
  float value = 0.0F;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

TEST(PictureTruthOutputTest, WritesTruthAndSidecarForEveryFrame) {
  for (const char* fixture :
       {"general/pal_decoder_stress.yaml", "general/ntsc_zone_plates.yaml"}) {
    std::string tag = fixture;
    std::replace(tag.begin(), tag.end(), '/', '_');
    const std::filesystem::path output_dir =
        std::filesystem::temp_directory_path() / "videosynth_picture_truth" /
        tag;
    const Project project = RunFixture(fixture, output_dir, 1);
    const PictureTruthLayout layout =
        BuildPictureTruthLayout(project.cvbs_presets);
    const int frames = FrameCount(project);

    const std::filesystem::path truth_path =
        DerivePictureTruthPath(project.output.video_path);
    ASSERT_TRUE(std::filesystem::exists(truth_path)) << truth_path;
    EXPECT_EQ(std::filesystem::file_size(truth_path),
              static_cast<std::uintmax_t>(frames) * layout.PlaneSamples() * 3U *
                  sizeof(float));
    ExpectSidecarDescribes(
        DerivePictureTruthSidecarPath(project.output.video_path), layout,
        frames);
  }
}

TEST(PictureTruthOutputTest, TruthIsIndependentOfTheThreadCount) {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "videosynth_picture_truth";
  const Project sequential =
      RunFixture("general/pal_decoder_stress.yaml", root / "threads_1", 1);
  const Project pooled =
      RunFixture("general/pal_decoder_stress.yaml", root / "threads_4", 4);
  EXPECT_EQ(ReadFileBytes(DerivePictureTruthPath(sequential.output.video_path)),
            ReadFileBytes(DerivePictureTruthPath(pooled.output.video_path)));
  EXPECT_EQ(ReadFileBytes(sequential.output.video_path),
            ReadFileBytes(pooled.output.video_path));
}

TEST(PictureTruthOutputTest, StressBarsAlternateWithTheVSwitch) {
  const Project project =
      RunFixture("general/pal_decoder_stress.yaml",
                 std::filesystem::temp_directory_path() /
                     "videosynth_picture_truth" / "stress_bars",
                 1);
  const PictureTruthLayout layout =
      BuildPictureTruthLayout(project.cvbs_presets);
  const std::vector<char> truth =
      ReadFileBytes(DerivePictureTruthPath(project.output.video_path));
  const std::size_t plane = layout.PlaneSamples();
  // Frame 0: Y, U, V planes. The left bar spans source x 48-143 and rows
  // 96-239 (V follows the switch); the right bar x 576-671 (U follows it).
  const int left = (96 * layout.samples_per_line) / 720;
  const int right = (624 * layout.samples_per_line) / 720;
  auto at = [&](int plane_index, int row, int x) {
    return ReadFloat(
        truth, (static_cast<std::size_t>(plane_index) * plane) +
                   (static_cast<std::size_t>(row) * layout.samples_per_line) +
                   static_cast<std::size_t>(x));
  };
  // Consecutive lines of one field (rows two apart) carry opposite signs.
  for (int row = 100; row < 230; row += 2) {
    const float v = at(2, row, left);
    const float v_next = at(2, row + 2, left);
    ASSERT_GT(std::abs(v), 100.0F) << row;
    ASSERT_FLOAT_EQ(v, -v_next) << row;
    const float u = at(1, row, right);
    ASSERT_GT(std::abs(u), 50.0F) << row;
    ASSERT_FLOAT_EQ(u, -at(1, row + 2, right)) << row;
  }
  // The steady +(R-Y) field below keeps one sign.
  for (int row = 250; row < 280; row += 2) {
    ASSERT_GT(at(2, row, left), 100.0F) << row;
  }
}

}  // namespace
}  // namespace videosynth
