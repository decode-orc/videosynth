/*
 * File:        test_efm_output_pipeline.cpp
 * Module:      efm_output_pipeline_tests
 * Purpose:     End-to-end verification of LaserDisc digital audio (EFM) output:
 *              decodes the generated T-value file back to subcode and audio and
 *              checks it against the project's section layout.
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
#include "videosynth/audio_sample_conversion.h"
#include "videosynth/audio_synthesizer.h"
#include "videosynth/audio_track_generator.h"
#include "videosynth/efm/t_value_byte.h"
#include "videosynth/efm_track_layout.h"
#include "videosynth/pipeline.h"
#include "videosynth/timing_constants.h"

namespace videosynth {
namespace {

using efm::test_support::DecodedFrame;
using efm::test_support::Deinterleave;
using efm::test_support::ExtractSamples;
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

// One schedule entry per section frame; the video samples are irrelevant here.
class MockGeneration final : public IGenerationStage {
 public:
  bool BuildFrameSchedule(const Project& project,
                          std::vector<FrameScheduleItem>* out_schedule,
                          std::vector<std::string>* errors) override {
    out_schedule->clear();
    for (const Section& section : project.sections) {
      for (int index = 0; index < section.duration_frames; ++index) {
        out_schedule->push_back(FrameScheduleItem{.section = &section,
                                                  .source_frame_index = index});
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

class SilentLogger final : public ILogger {
 public:
  void Info(const std::string&) override {}
  void Warning(const std::string&) override {}
  void Error(const std::string&) override {}
  void Debug(const std::string&) override {}
  void Trace(const std::string&) override {}
};

std::filesystem::path TempPath(const std::string& name) {
  return std::filesystem::temp_directory_path() / name;
}

// One row of the `<basename>.efm.meta` frame index sidecar.
struct EfmFrameRow {
  std::int64_t frame_id = 0;
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
  const char* query_sql =
      "SELECT frame_id, t_value_offset, t_value_count FROM efm_frame "
      "WHERE cvbs_file_id = 1 ORDER BY frame_id;";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, query_sql, -1, &stmt, nullptr) == SQLITE_OK) {
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      EfmFrameRow row;
      row.frame_id = sqlite3_column_int64(stmt, 0);
      row.t_value_offset = sqlite3_column_int64(stmt, 1);
      row.t_value_count = sqlite3_column_int64(stmt, 2);
      rows.push_back(row);
    }
  }
  sqlite3_finalize(stmt);
  sqlite3_close(db);
  return rows;
}

std::vector<std::uint8_t> ReadStreamBytes(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  const std::vector<char> bytes((std::istreambuf_iterator<char>(stream)),
                                std::istreambuf_iterator<char>());
  std::vector<std::uint8_t> stream_bytes;
  stream_bytes.reserve(bytes.size());
  for (const char byte : bytes) {
    stream_bytes.push_back(static_cast<std::uint8_t>(byte));
  }
  return stream_bytes;
}

AudioParameters Tone(double frequency_hz) {
  AudioParameters channel;
  channel.enabled = true;
  channel.waveform = AudioWaveform::kSine;
  channel.frequency_hz = frequency_hz;
  channel.amplitude = 0.5;
  return channel;
}

// One section of the fixture project: its type, length and audio tone.
struct SectionPlan {
  std::string name;
  SectionType section_type = SectionType::kProgrammeArea;
  int duration_frames = 0;
  double frequency_hz = 0.0;
};

Project MakeEfmProject(Standard standard, const std::vector<SectionPlan>& plans,
                       const std::filesystem::path& video_path) {
  Project project;
  project.cvbs_presets.video_standard_preset = standard;
  project.cvbs_presets.sample_encoding_preset = "CVBS_U10_4FSC";
  project.cvbs_presets.signal_state_preset = "STANDARD_STABLE_LOCKED";
  project.output.video_path = video_path.string();
  project.output.efm_audio.enabled = true;
  project.output.efm_audio.pair = 0;
  for (const SectionPlan& plan : plans) {
    Section section;
    section.name = plan.name;
    section.type = "progressive";
    section.section_type = plan.section_type;
    section.duration_frames = plan.duration_frames;
    AudioChannelPair pair;
    pair.pair = 0;
    pair.pair_specified = true;
    pair.left = Tone(plan.frequency_hz);
    pair.right = Tone(plan.frequency_hz / 2.0);
    section.audio_channel_pairs = {pair};
    project.sections.push_back(section);
  }
  return project;
}

bool RunPipeline(Project project, AudioTrackGenerator* audio_generator) {
  MockParser parser;
  parser.result.ok = true;
  parser.result.project = std::move(project);
  MockValidator validator;
  validator.result.is_valid = true;
  MockGeneration generation;
  MockOutput output;
  SilentLogger logger;

  VideoSynthPipeline pipeline(&parser, &validator, &generation, nullptr,
                              nullptr, &output, &logger, audio_generator);
  RunOptions options;
  options.project_path = "project.yaml";
  return pipeline.Run(options);
}

// The Q channel of one 98-frame subcode block: the ten payload bytes covered by
// the CRC, the recorded CRC, and the P flag of the block
// (IEC 60908-1999, 17.4 / 17.5).
struct QSection {
  efm::QChannelPayload payload{};
  std::uint16_t crc = 0;
  bool p_flag = false;
};

// Splits decoded channel frames into subcode blocks and reads their P and Q
// channels (IEC 60908-1999, 17.1 / 17.3).
std::vector<QSection> ReadSubcode(const std::vector<DecodedFrame>& frames) {
  std::vector<QSection> sections;
  for (std::size_t base = 0;
       base + efm::kFramesPerSubcodeSection <= frames.size();
       base += efm::kFramesPerSubcodeSection) {
    QSection section;
    section.p_flag = (frames[base + efm::kSubcodeSyncFrames].control_byte &
                      (1U << efm::kSubcodeChannelPShift)) != 0U;
    for (std::size_t bit = 0; bit < efm::kSubcodeChannelBits; ++bit) {
      const std::uint8_t control =
          frames[base + efm::kSubcodeSyncFrames + bit].control_byte;
      const bool q_bit = (control & (1U << efm::kSubcodeChannelQShift)) != 0U;
      if (!q_bit) {
        continue;
      }
      if (bit < efm::kQChannelPayloadBytes * 8U) {
        section.payload[bit / 8U] |=
            static_cast<std::uint8_t>(1U << (7U - (bit % 8U)));
      } else {
        section.crc = static_cast<std::uint16_t>(
            section.crc | (1U << (efm::kSubcodeChannelBits - 1U - bit)));
      }
    }
    sections.push_back(section);
  }
  return sections;
}

std::uint8_t FromBcd(std::uint8_t value) {
  return static_cast<std::uint8_t>(((value >> 4U) * 10U) + (value & 0x0FU));
}

// Field accessors for the DATA-Q layouts of IEC 60908-1999, 17.5.1.
std::uint8_t ControlAdr(const QSection& section) { return section.payload[0]; }
std::uint8_t TrackNumber(const QSection& section) { return section.payload[1]; }
std::uint8_t IndexOrPoint(const QSection& section) {
  return section.payload[2];
}
efm::SubcodeTime RunningTime(const QSection& section) {
  return efm::SubcodeTime{FromBcd(section.payload[3]),
                          FromBcd(section.payload[4]),
                          FromBcd(section.payload[5])};
}
efm::SubcodeTime PointTime(const QSection& section) {
  return efm::SubcodeTime{FromBcd(section.payload[7]),
                          FromBcd(section.payload[8]),
                          FromBcd(section.payload[9])};
}

// The 44.1 kHz reference rendering of the project's audio for channel pair 0,
// synthesised independently of the generator from the same AudioParameters.
void SynthesizeReference(Standard standard,
                         const std::vector<SectionPlan>& plans,
                         const Project& project,
                         std::vector<std::int16_t>* left,
                         std::vector<std::int16_t>* right) {
  std::size_t frame_index = 0;
  for (std::size_t index = 0; index < plans.size(); ++index) {
    const Section& section = project.sections[index];
    const std::size_t frame_count =
        static_cast<std::size_t>(plans[index].duration_frames);
    std::int64_t run_total = 0;
    for (std::size_t frame = 0; frame < frame_count; ++frame) {
      run_total += EfmAudioSamplesForFrame(
          standard, static_cast<std::int64_t>(frame_index) +
                        static_cast<std::int64_t>(frame));
    }

    AudioSynthesizer synth_left(EfmAudioSampleRateHz());
    AudioSynthesizer synth_right(EfmAudioSampleRateHz());
    synth_left.BeginSection(section.audio_channel_pairs.front().left,
                            run_total);
    synth_right.BeginSection(section.audio_channel_pairs.front().right,
                             run_total);
    for (std::size_t frame = 0; frame < frame_count; ++frame) {
      const int samples = EfmAudioSamplesForFrame(
          standard, static_cast<std::int64_t>(frame_index) +
                        static_cast<std::int64_t>(frame));
      const std::vector<std::int16_t> frame_left =
          ConvertSamples24To16(synth_left.Synthesize(samples));
      const std::vector<std::int16_t> frame_right =
          ConvertSamples24To16(synth_right.Synthesize(samples));
      left->insert(left->end(), frame_left.begin(), frame_left.end());
      right->insert(right->end(), frame_right.begin(), frame_right.end());
    }
    frame_index += frame_count;
  }
}

// Runs one fixture project end to end and asserts the decoded EFM stream
// against its section layout.
void VerifyEfmOutput(Standard standard, const std::vector<SectionPlan>& plans,
                     const std::string& basename,
                     std::uint8_t expected_video_system_code) {
  const std::filesystem::path video_path = TempPath(basename + ".cvbs");
  const std::filesystem::path wav_path = TempPath(basename + "_audio_0.wav");
  const std::filesystem::path efm_path = TempPath(basename + ".efm");
  const std::filesystem::path efm_meta_path = TempPath(basename + ".efm.meta");
  std::filesystem::remove(wav_path);
  std::filesystem::remove(efm_path);
  std::filesystem::remove(efm_meta_path);

  const Project project = MakeEfmProject(standard, plans, video_path);
  AudioTrackGenerator generator;
  ASSERT_TRUE(RunPipeline(project, &generator));
  ASSERT_TRUE(std::filesystem::exists(wav_path));
  ASSERT_TRUE(std::filesystem::exists(efm_path));
  ASSERT_TRUE(std::filesystem::exists(efm_meta_path));

  // Expected stream geometry: one F1 frame per six sampling periods and one
  // channel frame per F1 frame, plus the CIRC flush (ECMA-130, C.9).
  std::size_t total_samples = 0;
  std::size_t frame_count = 0;
  for (const SectionPlan& plan : plans) {
    for (int frame = 0; frame < plan.duration_frames; ++frame) {
      total_samples += static_cast<std::size_t>(EfmAudioSamplesForFrame(
          standard, static_cast<std::int64_t>(frame_count) +
                        static_cast<std::int64_t>(frame)));
    }
    frame_count += static_cast<std::size_t>(plan.duration_frames);
  }
  const std::size_t expected_f1_frames =
      (total_samples + efm::kStereoSamplesPerF1Frame - 1U) /
      efm::kStereoSamplesPerF1Frame;

  // efm-extension-format.md, "Binary Data File": bits 3-0 are the t-value and
  // bits 7-4 the producer's doubt. A synthesised stream is certain throughout,
  // so every byte reads back at zero doubt.
  const std::vector<std::uint8_t> stream_bytes = ReadStreamBytes(efm_path);
  ASSERT_FALSE(stream_bytes.empty());
  std::vector<std::uint8_t> t_values;
  t_values.reserve(stream_bytes.size());
  for (const std::uint8_t byte : stream_bytes) {
    ASSERT_EQ(efm::DoubtOfByte(byte), efm::kNoDoubt);
    const std::uint8_t t_value = efm::TValueOfByte(byte);
    ASSERT_GE(t_value, efm::kMinRunLengthT);
    ASSERT_LE(t_value, efm::kMaxRunLengthT);
    t_values.push_back(t_value);
  }

  // Decode: the helper asserts a sync header every 588 channel bits.
  const std::vector<DecodedFrame> frames =
      TestChannelDecoder().Decode(t_values);
  ASSERT_EQ(frames.size(), expected_f1_frames + efm::kCircDrainFrames);

  // ---- Subcode ----
  EfmTrackLayout layout;
  std::vector<std::string> layout_errors;
  std::vector<const Section*> output_frame_sections;
  for (std::size_t index = 0; index < plans.size(); ++index) {
    for (int frame = 0; frame < plans[index].duration_frames; ++frame) {
      output_frame_sections.push_back(&project.sections[index]);
    }
  }
  ASSERT_TRUE(BuildEfmTrackLayout(standard, output_frame_sections, &layout,
                                  &layout_errors));
  ASSERT_EQ(layout.table.entries.size(), plans.size());

  const std::vector<QSection> subcode = ReadSubcode(frames);
  const std::size_t lead_in_sections = layout.table.entries[0].section_count;
  const std::size_t programme_start = layout.table.entries[1].start_section;
  const std::size_t lead_out_start = layout.table.entries.back().start_section;
  const std::size_t table_sections =
      lead_out_start + layout.table.entries.back().section_count;
  ASSERT_GE(subcode.size(), table_sections);

  std::vector<std::uint8_t> toc_points;
  std::uint8_t a0_point_frame = 0;
  for (std::size_t index = 0; index < table_sections; ++index) {
    const QSection& section = subcode[index];
    // IEC 60856:1986 Amd 2, 13.5.1.1: control 0000 and ADR 0100 (mode 4).
    EXPECT_EQ(ControlAdr(section), 0x04) << "section " << index;
    EXPECT_EQ(section.crc, efm::ComputeQChannelCrc(section.payload))
        << "section " << index;

    if (index < lead_in_sections) {
      // IEC 60908-1999, 17.5.1: the lead-in carries TNO = 00 and a POINT field.
      EXPECT_EQ(TrackNumber(section), efm::kTrackNumberLeadIn)
          << "section " << index;
      toc_points.push_back(IndexOrPoint(section));
      if (IndexOrPoint(section) == efm::kTocPointFirstTrack) {
        a0_point_frame = PointTime(section).frames;
      }
    } else if (index < lead_out_start) {
      // Absolute time is zero at the start of the programme area and advances
      // one subcode section at a time across track boundaries.
      const efm::SubcodeTime absolute = PointTime(section);
      EXPECT_EQ(efm::SectionsFromSubcodeTime(absolute), index - programme_start)
          << "section " << index;
      const bool in_track_two = index >= layout.table.entries[2].start_section;
      EXPECT_EQ(FromBcd(TrackNumber(section)), in_track_two ? 2 : 1)
          << "section " << index;
      // The mandatory pause of track 1 is X = 00 with the running time counting
      // down; inside a track X = 01 (IEC 60908-1999, 17.5.1).
      const std::size_t pause_end_section =
          layout.pause_end_sample / kEfmSamplesPerSubcodeSection;
      const std::uint8_t expected_index =
          index < pause_end_section ? efm::kIndexPause : efm::kIndexAudio;
      EXPECT_EQ(IndexOrPoint(section), expected_index) << "section " << index;
    } else {
      // IEC 60908-1999, 17.5.1: the lead-out track is numbered AA.
      EXPECT_EQ(TrackNumber(section), efm::kTrackNumberLeadOut)
          << "section " << index;
      EXPECT_EQ(IndexOrPoint(section), efm::kIndexAudio) << "section " << index;
      EXPECT_EQ(efm::SectionsFromSubcodeTime(RunningTime(section)),
                index - lead_out_start)
          << "section " << index;
    }
  }

  // The table of contents names both tracks, the first/last track and the
  // lead-out start, and carries the video system identification in the P FRAME
  // of the A0 entry (IEC 60856 Amd 2, 13.5.2 / IEC 60857 Amd 2, 13.6.2).
  for (const std::uint8_t point :
       {std::uint8_t{0x01}, std::uint8_t{0x02}, efm::kTocPointFirstTrack,
        efm::kTocPointLastTrack, efm::kTocPointLeadOutStart}) {
    EXPECT_NE(std::find(toc_points.begin(), toc_points.end(), point),
              toc_points.end())
        << "missing TOC point " << static_cast<int>(point);
  }
  EXPECT_EQ(a0_point_frame, expected_video_system_code);

  // ---- Audio ----
  std::vector<std::int16_t> decoded_left;
  std::vector<std::int16_t> decoded_right;
  ExtractSamples(Deinterleave(frames), &decoded_left, &decoded_right);

  std::vector<std::int16_t> reference_left;
  std::vector<std::int16_t> reference_right;
  SynthesizeReference(standard, plans, project, &reference_left,
                      &reference_right);
  ASSERT_EQ(reference_left.size(), total_samples);

  // IEC 60908-1999, 17.5.1: the pause preceding track 1 is digital silence.
  for (std::size_t index = layout.pause_start_sample;
       index < layout.pause_end_sample; ++index) {
    reference_left[index] = 0;
    reference_right[index] = 0;
  }

  // Discarding the CIRC warm-up leaves the source samples with zero net offset
  // (EFM implementation plan, Timing Alignment Contract).
  const std::size_t warm_up =
      efm::kCircPipelineLatencyFrames * efm::kStereoSamplesPerF1Frame;
  ASSERT_GE(decoded_left.size(), warm_up + total_samples);
  for (std::size_t index = 0; index < total_samples; ++index) {
    ASSERT_EQ(decoded_left[warm_up + index], reference_left[index])
        << "sample " << index;
    ASSERT_EQ(decoded_right[warm_up + index], reference_right[index])
        << "sample " << index;
  }
  // The pause window really is silent in the stream (not merely equal to a
  // silent reference).
  EXPECT_TRUE(std::any_of(decoded_left.begin() + warm_up, decoded_left.end(),
                          [](std::int16_t sample) { return sample != 0; }));
  for (std::size_t index = layout.pause_start_sample;
       index < layout.pause_end_sample; ++index) {
    ASSERT_EQ(decoded_left[warm_up + index], 0) << "pause sample " << index;
  }

  // The extension sidecar indexes one frame per stored video frame and covers
  // the whole binary file (efm-extension-format.md §Frame Validity Rules).
  const std::vector<EfmFrameRow> index_rows = ReadEfmFrameRows(efm_meta_path);
  ASSERT_EQ(index_rows.size(), frame_count);
  std::int64_t running_offset = 0;
  for (std::size_t index = 0; index < index_rows.size(); ++index) {
    ASSERT_EQ(index_rows[index].frame_id, static_cast<std::int64_t>(index));
    ASSERT_EQ(index_rows[index].t_value_offset, running_offset);
    running_offset += index_rows[index].t_value_count;
  }
  EXPECT_EQ(running_offset, static_cast<std::int64_t>(t_values.size()));

  std::filesystem::remove(wav_path);
  std::filesystem::remove(efm_path);
  std::filesystem::remove(efm_meta_path);
}

// 4 s of PAL: 10 lead-in frames, a 60-frame first track (longer than the 2 s
// pause), a 15-frame second track and 15 lead-out frames.
const std::vector<SectionPlan>& PalPlans() {
  static const std::vector<SectionPlan> plans = {
      {"LeadIn", SectionType::kLeadIn, 10, 400.0},
      {"Chapter0", SectionType::kProgrammeArea, 60, 1000.0},
      {"Chapter1", SectionType::kProgrammeArea, 15, 1500.0},
      {"LeadOut", SectionType::kLeadOut, 15, 800.0},
  };
  return plans;
}

const std::vector<SectionPlan>& NtscPlans() {
  static const std::vector<SectionPlan> plans = {
      {"LeadIn", SectionType::kLeadIn, 12, 400.0},
      {"Chapter0", SectionType::kProgrammeArea, 70, 1000.0},
      {"Chapter1", SectionType::kProgrammeArea, 20, 1500.0},
      {"LeadOut", SectionType::kLeadOut, 20, 800.0},
  };
  return plans;
}

TEST(EfmOutputPipelineTest, PalProjectProducesAValidEfmStream) {
  VerifyEfmOutput(Standard::kPal, PalPlans(), "videosynth_efm_pipeline_pal",
                  efm::kVideoSystemIdentificationPal);
}

TEST(EfmOutputPipelineTest, NtscProjectProducesAValidEfmStream) {
  VerifyEfmOutput(Standard::kNtsc, NtscPlans(), "videosynth_efm_pipeline_ntsc",
                  efm::kVideoSystemIdentificationNtsc);
}

// PAL-M discs are 525-line LaserVision discs: IEC 60857 timing and the
// 525-line identification in the lead-in TOC.
TEST(EfmOutputPipelineTest, PalMProjectProducesAValidEfmStream) {
  VerifyEfmOutput(Standard::kPalM, NtscPlans(), "videosynth_efm_pipeline_pal_m",
                  efm::kVideoSystemIdentificationNtsc);
}

TEST(EfmOutputPipelineTest, NoEfmFileWhenTheSelectionIsInactive) {
  const std::filesystem::path video_path =
      TempPath("videosynth_efm_pipeline_off.cvbs");
  const std::filesystem::path efm_path =
      TempPath("videosynth_efm_pipeline_off.efm");
  const std::filesystem::path wav_path =
      TempPath("videosynth_efm_pipeline_off_audio_0.wav");
  std::filesystem::remove(efm_path);

  // SECAM has no LaserDisc digital audio specification, so only the WAV track
  // is written.
  Project project = MakeEfmProject(Standard::kSecam, PalPlans(), video_path);
  project.cvbs_presets.sample_encoding_preset = "CVBS_U10_4FSC";
  project.cvbs_presets.signal_state_preset = "STANDARD_STABLE_UNLOCKED";
  AudioTrackGenerator generator;
  ASSERT_TRUE(RunPipeline(project, &generator));

  EXPECT_FALSE(generator.efm_active());
  EXPECT_FALSE(std::filesystem::exists(efm_path));
  EXPECT_TRUE(std::filesystem::exists(wav_path));

  std::filesystem::remove(wav_path);
}

}  // namespace
}  // namespace videosynth
