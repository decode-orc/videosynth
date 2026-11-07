/*
 * File:        test_efm_data_pipeline.cpp
 * Module:      efm_data_pipeline_tests
 * Purpose:     End-to-end verification of EFM data output: a sector image on
 *              disk is written through the pipeline to `.efm` / `.efm.meta`,
 *              and the stream is decoded back and checked against the image.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "efm_channel_decoder.h"
#include "videosynth/efm/data_sector.h"
#include "videosynth/efm/t_value_byte.h"
#include "videosynth/efm_sector_image.h"
#include "videosynth/pipeline.h"

namespace videosynth {
namespace {

using efm::test_support::Deinterleave;
using efm::test_support::TestChannelDecoder;

class MockParser final : public IProjectParser {
 public:
  ParseResult result;
  ParseResult ParseFile(const std::string&) override { return result; }
};

class MockValidator final : public IProjectValidator {
 public:
  ValidationResult result;
  ValidationResult Validate(const Project&) override { return result; }
};

// One schedule entry per section frame, numbering programme-area pictures
// from 1; the video samples are irrelevant here.
class MockGeneration final : public IGenerationStage {
 public:
  bool BuildFrameSchedule(const Project& project,
                          std::vector<FrameScheduleItem>* out_schedule,
                          std::vector<std::string>* errors) override {
    out_schedule->clear();
    int picture = 1;
    for (const Section& section : project.sections) {
      for (int index = 0; index < section.duration_frames; ++index) {
        FrameScheduleItem item{.section = &section,
                               .source_frame_index = index};
        if (section.section_type == SectionType::kProgrammeArea) {
          item.disc_picture_number = picture++;
        }
        out_schedule->push_back(item);
      }
    }
    errors->clear();
    return true;
  }

  bool GenerateFrameBatch(const Project&, const std::vector<FrameScheduleItem>&,
                          std::size_t, std::size_t frame_count,
                          std::vector<SampleFixed>* out_y_mv,
                          std::vector<SampleFixed>* out_c_mv,
                          std::vector<std::string>* errors) override {
    out_y_mv->assign(frame_count * 8U, 0);
    out_c_mv->assign(frame_count * 8U, 0);
    errors->clear();
    return true;
  }

  bool Generate(const Project&, std::vector<SampleFixed>* out_y_mv,
                std::vector<SampleFixed>* out_c_mv,
                std::vector<std::string>* errors) override {
    out_y_mv->assign(8, 0);
    out_c_mv->assign(8, 0);
    errors->clear();
    return true;
  }
};

class MockOutput final : public IOutputStage {
 public:
  bool BeginWrite(const Project&, std::size_t,
                  std::vector<std::string>* errors) override {
    errors->clear();
    return true;
  }
  bool AppendSamples(const std::vector<SampleFixed>&,
                     const std::vector<SampleFixed>&,
                     std::vector<std::string>* errors) override {
    errors->clear();
    return true;
  }
  bool EncodeFrame(const std::vector<SampleFixed>&,
                   const std::vector<SampleFixed>&, EncodedFrame*,
                   std::vector<std::string>* errors) const override {
    errors->clear();
    return true;
  }
  bool AppendEncodedFrame(const EncodedFrame&,
                          std::vector<std::string>* errors) override {
    errors->clear();
    return true;
  }
  bool FinalizeWrite(std::vector<std::string>* errors) override {
    errors->clear();
    return true;
  }
  void AbortWrite() override {}
  bool Write(const Project&, const std::vector<SampleFixed>&,
             const std::vector<SampleFixed>&,
             std::vector<std::string>* errors) override {
    errors->clear();
    return true;
  }
};

class RecordingLogger final : public ILogger {
 public:
  std::vector<std::string> warnings;
  std::vector<std::string> errors;
  void Info(const std::string&) override {}
  void Warning(const std::string& message) override {
    warnings.push_back(message);
  }
  void Error(const std::string& message) override { errors.push_back(message); }
  void Debug(const std::string&) override {}
  void Trace(const std::string&) override {}
};

struct EfmFrameRow {
  std::int64_t t_value_offset = 0;
  std::int64_t t_value_count = 0;
};

std::vector<EfmFrameRow> ReadEfmFrameRows(const std::filesystem::path& path) {
  std::vector<EfmFrameRow> rows;
  sqlite3* db = nullptr;
  if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) {
    sqlite3_close(db);
    return rows;
  }
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db,
                         "SELECT t_value_offset, t_value_count FROM efm_frame "
                         "WHERE cvbs_file_id = 1 ORDER BY frame_id;",
                         -1, &stmt, nullptr) == SQLITE_OK) {
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      rows.push_back(
          {sqlite3_column_int64(stmt, 0), sqlite3_column_int64(stmt, 1)});
    }
  }
  sqlite3_finalize(stmt);
  sqlite3_close(db);
  return rows;
}

std::vector<std::uint8_t> ReadBytes(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  const std::vector<char> bytes((std::istreambuf_iterator<char>(stream)),
                                std::istreambuf_iterator<char>());
  return {bytes.begin(), bytes.end()};
}

// Sector n of the image: its own index, repeated.
EfmSectorUserData ImageSector(std::size_t index) {
  EfmSectorUserData data{};
  for (std::size_t byte = 0; byte < data.size(); ++byte) {
    data[byte] =
        static_cast<std::uint8_t>((index >> (8U * (byte % 4U))) ^ (byte / 4U));
  }
  return data;
}

void WriteImage(const std::filesystem::path& path, std::size_t sectors) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  for (std::size_t index = 0; index < sectors; ++index) {
    const EfmSectorUserData data = ImageSector(index);
    stream.write(reinterpret_cast<const char*>(data.data()),
                 static_cast<std::streamsize>(data.size()));
  }
}

Section MakeSection(const std::string& name, SectionType type, int frames,
                    bool carries) {
  Section section;
  section.name = name;
  section.type = "progressive";
  section.section_type = type;
  section.duration_frames = frames;
  section.carries_efm_data = carries;
  return section;
}

class EfmDataPipelineTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ =
        std::filesystem::temp_directory_path() /
        ("videosynth_efm_data_" +
         std::string(
             ::testing::UnitTest::GetInstance()->current_test_info()->name()));
    std::filesystem::remove_all(dir_);
    std::filesystem::create_directories(dir_);
  }
  void TearDown() override { std::filesystem::remove_all(dir_); }

  Project MakeProject(std::size_t image_sectors) {
    WriteImage(dir_ / "image.dat", image_sectors);
    Project project;
    project.cvbs_presets.video_standard_preset = Standard::kPal;
    project.cvbs_presets.sample_encoding_preset = "CVBS_U10_4FSC";
    project.cvbs_presets.signal_state_preset = "STANDARD_STABLE_LOCKED";
    project.output.video_path = (dir_ / "out.cvbs").string();
    project.laserdisc.enabled = true;
    project.laserdisc.efm_data.enabled = true;
    project.laserdisc.efm_data.image = (dir_ / "image.dat").string();
    project.sections.push_back(
        MakeSection("LeadIn", SectionType::kLeadIn, 60, true));
    project.sections.push_back(
        MakeSection("Data", SectionType::kProgrammeArea, 20, true));
    project.sections.push_back(
        MakeSection("Video", SectionType::kProgrammeArea, 5, false));
    project.sections.push_back(
        MakeSection("More", SectionType::kProgrammeArea, 10, true));
    return project;
  }

  bool Run(Project project, RecordingLogger* logger) {
    MockParser parser;
    parser.result.ok = true;
    parser.result.project = std::move(project);
    MockValidator validator;
    validator.result.is_valid = true;
    MockGeneration generation;
    MockOutput output;
    VideoSynthPipeline pipeline(&parser, &validator, &generation, nullptr,
                                nullptr, &output, logger, nullptr);
    RunOptions options;
    options.project_path = "project.yaml";
    return pipeline.Run(options);
  }

  std::filesystem::path dir_;
};

TEST_F(EfmDataPipelineTest, WritesAStreamThatDecodesToTheImage) {
  RecordingLogger logger;
  ASSERT_TRUE(Run(MakeProject(300), &logger))
      << (logger.errors.empty() ? "" : logger.errors.front());

  const std::vector<EfmFrameRow> rows = ReadEfmFrameRows(dir_ / "out.efm.meta");
  ASSERT_EQ(rows.size(), 95U);
  // Frames 80-84 (Video) carry no EFM; the first of them closes the run.
  EXPECT_EQ(rows[80].t_value_count, 1);
  for (std::size_t frame = 81; frame < 85; ++frame) {
    EXPECT_EQ(rows[frame].t_value_count, 0) << "frame " << frame;
  }
  const std::vector<std::uint8_t> bytes = ReadBytes(dir_ / "out.efm");
  ASSERT_EQ(static_cast<std::int64_t>(bytes.size()),
            rows.back().t_value_offset + rows.back().t_value_count);

  // Decode the first run (lead-in and Data) and check every sector it carries
  // in full: picture p holds image sectors 3p + 150 to 3p + 152.
  const std::size_t run_end = static_cast<std::size_t>(rows[81].t_value_offset);
  std::vector<std::uint8_t> run(
      bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(run_end));
  for (std::uint8_t& value : run) {
    value = efm::TValueOfByte(value);
  }
  const std::vector<efm::F1Frame> f1 =
      Deinterleave(TestChannelDecoder().Decode(run));
  std::size_t verified = 0;
  // Picture 1 is section 180 (frame 60), whose address is 153.
  for (std::size_t section = 0;; ++section) {
    const std::size_t first = section * 98 + efm::kCircPipelineLatencyFrames;
    if (first + 98 > f1.size()) {
      break;
    }
    efm::Sector sector{};
    for (std::size_t frame = 0; frame < 98; ++frame) {
      for (std::size_t byte = 0; byte < 24; byte += 2) {
        sector[frame * 24 + byte] = f1[first + frame][byte + 1];
        sector[frame * 24 + byte + 1] = f1[first + frame][byte];
      }
    }
    efm::ScrambleSector(&sector);
    const std::int64_t address = static_cast<std::int64_t>(section) - 27;
    if (address < 0) {
      EXPECT_GE(sector[12], efm::kLeadInSectorMinuteOffset);
      continue;
    }
    ASSERT_EQ(efm::ComputeEdc(sector.data(), 2068), 0U)
        << "section " << section;
    const EfmSectorUserData expected =
        ImageSector(static_cast<std::size_t>(address));
    ASSERT_TRUE(std::equal(expected.begin(), expected.end(),
                           sector.begin() + efm::kSectorUserDataOffset))
        << "image sector " << address;
    ++verified;
  }
  // Image sectors 0-212 lie in the first run; the last two are cut by the gap.
  EXPECT_EQ(verified, 213U - 2U);
}

TEST_F(EfmDataPipelineTest, WarnsAboutLeftOutSectorsThatAreNotBlank) {
  RecordingLogger logger;
  ASSERT_TRUE(Run(MakeProject(300), &logger));
  const auto contains = [&logger](const std::string& text) {
    return std::any_of(logger.warnings.begin(), logger.warnings.end(),
                       [&text](const std::string& w) {
                         return w.find(text) != std::string::npos;
                       });
  };
  EXPECT_TRUE(contains("left out"));
  EXPECT_TRUE(contains("not blank"));
  EXPECT_TRUE(contains("too close to a frame without EFM"));
}

TEST_F(EfmDataPipelineTest, RejectsAnImageThatIsNotWholeSectors) {
  Project project = MakeProject(10);
  {
    std::ofstream stream(project.laserdisc.efm_data.image,
                         std::ios::binary | std::ios::app);
    stream.put('x');
  }
  RecordingLogger logger;
  EXPECT_FALSE(Run(project, &logger));
  ASSERT_FALSE(logger.errors.empty());
  EXPECT_NE(logger.errors.front().find("2048-byte sectors"), std::string::npos);
  EXPECT_FALSE(std::filesystem::exists(dir_ / "out.efm"));
}

TEST_F(EfmDataPipelineTest, FileImageReadsSectorsInOrder) {
  WriteImage(dir_ / "image.dat", 4);
  FileEfmSectorImage image;
  std::vector<std::string> errors;
  ASSERT_TRUE(image.Open((dir_ / "image.dat").string(), &errors));
  EXPECT_EQ(image.SectorCount(), 4U);
  EfmSectorUserData data{};
  std::string error;
  ASSERT_TRUE(image.ReadSector(3, &data, &error));
  EXPECT_EQ(data, ImageSector(3));
  EXPECT_FALSE(image.ReadSector(4, &data, &error));
}

}  // namespace
}  // namespace videosynth
