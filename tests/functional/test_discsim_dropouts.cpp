/*
 * File:        test_discsim_dropouts.cpp
 * Module:      dropout_injection_tests
 * Purpose:     Renders the disc-simulation A sources and checks that their
 *              disc-anchored dropouts repeat on a replay: the sidecar rows of
 *              PNs 17-20 in MultiBurst_PreSkip_A and MultiBurst_Replay_A are
 *              identical.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <tuple>
#include <vector>

#include "fixture_paths.h"
#include "videosynth/dropout_injection_stage.h"
#include "videosynth/generation_stage.h"
#include "videosynth/noise_injection_stage.h"
#include "videosynth/output_stage.h"
#include "videosynth/path_resolution.h"
#include "videosynth/pipeline.h"
#include "videosynth/progressive_frame_source_probe.h"
#include "videosynth/project_validator.h"
#include "videosynth/yaml_project_parser.h"

namespace videosynth {
namespace {

class NullLogger final : public ILogger {
 public:
  void Info(const std::string&) override {}
  void Warning(const std::string&) override {}
  void Error(const std::string&) override {}
  void Debug(const std::string&) override {}
  void Trace(const std::string&) override {}
};

using Row = std::tuple<std::int64_t, std::int64_t, int>;

std::filesystem::path OutputRoot() {
  return std::filesystem::temp_directory_path() / "videosynth_discsim_dropouts";
}

// Sidecar rows (sample_start, sample_count, severity) keyed by frame_id.
std::map<std::int64_t, std::vector<Row>> ReadSidecar(const std::string& path) {
  std::map<std::int64_t, std::vector<Row>> rows;
  sqlite3* db = nullptr;
  if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) {
    sqlite3_close(db);
    ADD_FAILURE() << "cannot open " << path;
    return rows;
  }
  sqlite3_stmt* stmt = nullptr;
  const char* query =
      "SELECT frame_id, sample_start, sample_count, severity FROM dropout_run "
      "ORDER BY frame_id, sample_start;";
  if (sqlite3_prepare_v2(db, query, -1, &stmt, nullptr) == SQLITE_OK) {
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      rows[sqlite3_column_int64(stmt, 0)].emplace_back(
          sqlite3_column_int64(stmt, 1), sqlite3_column_int64(stmt, 2),
          sqlite3_column_int(stmt, 3));
    }
  }
  sqlite3_finalize(stmt);
  sqlite3_close(db);
  return rows;
}

// Output position of a section's first frame.
std::int64_t SectionStart(const Project& project, const std::string& name) {
  std::int64_t start = 0;
  for (const Section& section : project.sections) {
    if (section.name == name) {
      return start;
    }
    start += section.duration_frames;
  }
  ADD_FAILURE() << "no section " << name;
  return -1;
}

class DiscSimDropoutsTest : public ::testing::TestWithParam<const char*> {};

TEST_P(DiscSimDropoutsTest, ReplayRepeatsTheFirstPlaysDropouts) {
  const std::string relative =
      std::string("stacking/") + GetParam() + "_discsim_A.yaml";
  const std::string path = test_support::FixturePath(relative);
  const ParseResult parsed = YamlProjectParser().ParseFile(path);
  ASSERT_TRUE(parsed.ok) << (parsed.errors.empty() ? "" : parsed.errors[0]);
  AssetRootMap roots = DefaultAssetRoots();
  roots.roots["output"] = OutputRoot().string();
  const Project project = ResolveProjectPaths(
      parsed.project, roots, test_support::FixtureProjectDir(relative),
      /*anchor_unset=*/true);

  NullLogger logger;
  YamlProjectParser parser(&logger);
  ProgressiveFrameSourceProbe probe;
  ProjectValidator validator(&probe, &logger);
  GenerationStage generation(&logger);
  NoiseInjectionStage noise(&logger);
  DropoutInjectionStage dropouts(&logger);
  OutputStage output(&logger);
  VideoSynthPipeline pipeline(&parser, &validator, &generation, &noise,
                              &dropouts, &output, &logger);
  std::filesystem::create_directories(OutputRoot());
  RunOptions options;
  options.threads = 4;
  ASSERT_TRUE(pipeline.RunProject(project, options));

  const std::string sidecar =
      DropoutInjectionStage::DeriveSidecarPath(project.output.metadata_path);
  const std::map<std::int64_t, std::vector<Row>> rows = ReadSidecar(sidecar);
  std::filesystem::remove(project.output.video_path);
  std::filesystem::remove(project.output.metadata_path);
  std::filesystem::remove(sidecar);

  const std::int64_t first_play = SectionStart(project, "MultiBurst_PreSkip_A");
  const std::int64_t replay = SectionStart(project, "MultiBurst_Replay_A");
  std::size_t compared = 0;
  for (std::int64_t i = 0; i < 4; ++i) {
    const auto a = rows.find(first_play + i);
    const auto b = rows.find(replay + i);
    const std::vector<Row> none;
    EXPECT_EQ(a == rows.end() ? none : a->second,
              b == rows.end() ? none : b->second)
        << "PN " << 17 + i;
    compared += (a == rows.end()) ? 0U : a->second.size();
  }
  EXPECT_GT(compared, 0U);
}

INSTANTIATE_TEST_SUITE_P(Standards, DiscSimDropoutsTest,
                         ::testing::Values("pal", "ntsc", "pal_m"));

}  // namespace
}  // namespace videosynth
