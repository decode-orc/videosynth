/*
 * File:        test_efm_data_encoder.cpp
 * Module:      efm_data_encoder
 * Purpose:     Unit tests for the EFM data encoder: an in-memory sector image
 *              is encoded and decoded back with the test-side channel decoder.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "efm_channel_decoder.h"
#include "videosynth/data_efm_writer.h"
#include "videosynth/efm/data_sector.h"
#include "videosynth/efm/subcode_generator.h"
#include "videosynth/efm_data_encoder.h"

namespace videosynth {
namespace {

using efm::test_support::Deinterleave;
using efm::test_support::TestChannelDecoder;

Section MakeSection(const std::string& name, SectionType type, bool carries) {
  Section section;
  section.name = name;
  section.section_type = type;
  section.carries_efm_data = carries;
  return section;
}

void AddFrames(const Section* section, std::size_t count, int first_picture,
               std::vector<EfmDataFrame>* frames) {
  for (std::size_t index = 0; index < count; ++index) {
    frames->push_back({section, first_picture > 0
                                    ? first_picture + static_cast<int>(index)
                                    : 0});
  }
}

// An image whose every sector is distinct: each byte depends on the sector
// index and the byte position.
std::vector<EfmSectorUserData> MakeImage(std::size_t sectors) {
  std::vector<EfmSectorUserData> image(sectors);
  for (std::size_t sector = 0; sector < sectors; ++sector) {
    for (std::size_t byte = 0; byte < efm::kMode1UserDataBytes; ++byte) {
      image[sector][byte] = static_cast<std::uint8_t>(
          (sector * 131U) ^ (byte * 7U) ^ (byte >> 8));
    }
  }
  return image;
}

std::size_t FromBcd(std::uint8_t value) {
  return (static_cast<std::size_t>(value >> 4U) * 10U) +
         static_cast<std::size_t>(value & 0x0FU);
}

// A decoded sector: its header address and mode and whether its EDC holds.
struct DecodedSector {
  std::size_t address = 0;
  std::uint8_t minute_byte = 0;
  std::uint8_t mode = 0;
  bool edc_valid = false;
  EfmSectorUserData user_data{};
};

// Decodes one contiguous run of T values that started at F1 frame
// `run_start`, returning the sectors of every subcode section the run carries
// in full, keyed by section.
std::map<std::size_t, DecodedSector> DecodeRun(
    const std::vector<std::uint8_t>& t_values, std::size_t run_start) {
  std::map<std::size_t, DecodedSector> sectors;
  const auto frames = TestChannelDecoder().Decode(t_values);
  const std::vector<efm::F1Frame> f1 = Deinterleave(frames);
  // Decoded index i holds the F1 frame pushed at run_start + i - 108.
  const std::size_t latency = efm::kCircPipelineLatencyFrames;
  const std::size_t first_section = (run_start + 97) / 98;
  for (std::size_t section = first_section;; ++section) {
    const std::size_t first_index = section * 98 - run_start + latency;
    if (first_index + 98 > f1.size()) {
      break;
    }
    efm::Sector sector{};
    for (std::size_t frame = 0; frame < 98; ++frame) {
      for (std::size_t byte = 0; byte < 24; byte += 2) {
        sector[frame * 24 + byte] = f1[first_index + frame][byte + 1];
        sector[frame * 24 + byte + 1] = f1[first_index + frame][byte];
      }
    }
    efm::ScrambleSector(&sector);
    DecodedSector decoded;
    decoded.minute_byte = sector[12];
    decoded.address =
        FromBcd(sector[12] >= 0xA0 ? sector[12] - 0xA0 : sector[12]) * 4500 +
        FromBcd(sector[13]) * 75 + FromBcd(sector[14]);
    decoded.mode = sector[15];
    decoded.edc_valid = efm::ComputeEdc(sector.data(), 2068) == 0U;
    std::copy(sector.begin() + 16, sector.begin() + 16 + 2048,
              decoded.user_data.begin());
    sectors[section] = decoded;
  }
  return sectors;
}

TEST(EfmDataEncoderTest, RoundTripRecoversEveryPlacedSector) {
  const Section lead_in = MakeSection("LeadIn", SectionType::kLeadIn, true);
  const Section data = MakeSection("Data", SectionType::kProgrammeArea, true);
  const Section video =
      MakeSection("Video", SectionType::kProgrammeArea, false);
  const Section more = MakeSection("More", SectionType::kProgrammeArea, true);
  std::vector<EfmDataFrame> frames;
  AddFrames(&lead_in, 60, 0, &frames);
  AddFrames(&data, 20, 1, &frames);
  AddFrames(&video, 5, 21, &frames);
  AddFrames(&more, 15, 26, &frames);

  const std::vector<EfmSectorUserData> image_data = MakeImage(260);
  MemoryEfmSectorImage image(image_data);

  Project project;
  project.cvbs_presets.video_standard_preset = Standard::kPal;
  project.laserdisc.efm_data.enabled = true;
  std::vector<std::string> errors;
  EfmDataLayoutSettings settings;
  settings.image_sector_count = image.SectorCount();
  EfmDataLayout layout;
  ASSERT_TRUE(
      BuildEfmDataLayout(Standard::kPal, frames, settings, &layout, &errors));

  efm::TrackTable table;
  ASSERT_TRUE(BuildEfmDataTrackTable(project, frames, layout, &table, &errors))
      << (errors.empty() ? "" : errors.front());
  EXPECT_EQ(table.control, efm::kControlDataCopyProhibited);
  EXPECT_EQ(table.adr, efm::kAdrMode4);

  EfmDataEncoder encoder;
  ASSERT_TRUE(encoder.Begin(&layout, table, &image, &errors));

  // Split the stream into its contiguous runs, as the frame index does.
  std::vector<std::vector<std::uint8_t>> runs(1);
  std::vector<std::size_t> run_starts = {0};
  bool in_gap = false;
  for (std::size_t k = 0; k < frames.size(); ++k) {
    std::vector<std::uint8_t> t_values;
    ASSERT_TRUE(encoder.EncodeFrame(k, &t_values, &errors))
        << (errors.empty() ? "" : errors.front());
    if (!layout.frame_carries_efm[k]) {
      in_gap = true;
      // The skip emits the previous run's final T value.
      runs.back().insert(runs.back().end(), t_values.begin(), t_values.end());
      continue;
    }
    if (in_gap) {
      runs.emplace_back();
      run_starts.push_back(layout.frame_f1_start[k]);
      in_gap = false;
    }
    runs.back().insert(runs.back().end(), t_values.begin(), t_values.end());
  }
  std::vector<std::uint8_t> tail;
  ASSERT_TRUE(encoder.Flush(&tail, &errors));
  runs.back().insert(runs.back().end(), tail.begin(), tail.end());
  ASSERT_EQ(runs.size(), 2U);

  std::size_t verified = 0;
  std::size_t lead_in_fill = 0;
  for (std::size_t run = 0; run < runs.size(); ++run) {
    for (const auto& [section, sector] :
         DecodeRun(runs[run], run_starts[run])) {
      const EfmDataSlot slot = layout.Slot(section);
      if (slot.kind == EfmDataSlotKind::kSilent) {
        continue;
      }
      ASSERT_EQ(sector.address, slot.address) << "section " << section;
      if (slot.kind == EfmDataSlotKind::kImageSector) {
        ASSERT_EQ(sector.mode, efm::kSectorMode1);
        ASSERT_TRUE(sector.edc_valid) << "section " << section;
        ASSERT_EQ(sector.user_data, image_data[slot.image_index])
            << "image sector " << slot.image_index;
        ++verified;
      } else if (slot.kind == EfmDataSlotKind::kLeadInFill) {
        EXPECT_GE(sector.minute_byte, efm::kLeadInSectorMinuteOffset);
        EXPECT_TRUE(sector.edc_valid);
        ++lead_in_fill;
      } else {
        EXPECT_EQ(sector.mode, efm::kSectorMode0);
      }
    }
  }
  // Every placed sector decodes except the ones cut by the gap.
  EXPECT_EQ(verified,
            layout.placed_image_sectors - layout.unrecoverable_image_sectors);
  EXPECT_GT(lead_in_fill, 0U);
  EXPECT_EQ(encoder.image_sectors_written(), layout.placed_image_sectors);
}

TEST(EfmDataEncoderTest, FramesWithoutEfmYieldNoTValues) {
  const Section data = MakeSection("Data", SectionType::kProgrammeArea, true);
  const Section video =
      MakeSection("Video", SectionType::kProgrammeArea, false);
  std::vector<EfmDataFrame> frames;
  AddFrames(&data, 3, 1, &frames);
  AddFrames(&video, 3, 4, &frames);

  MemoryEfmSectorImage image(MakeImage(10));
  Project project;
  project.cvbs_presets.video_standard_preset = Standard::kPal;
  std::vector<std::string> errors;
  EfmDataLayoutSettings settings;
  settings.image_sector_count = image.SectorCount();
  EfmDataLayout layout;
  ASSERT_TRUE(
      BuildEfmDataLayout(Standard::kPal, frames, settings, &layout, &errors));
  efm::TrackTable table;
  ASSERT_TRUE(BuildEfmDataTrackTable(project, frames, layout, &table, &errors));
  EfmDataEncoder encoder;
  ASSERT_TRUE(encoder.Begin(&layout, table, &image, &errors));

  for (std::size_t k = 0; k < frames.size(); ++k) {
    std::vector<std::uint8_t> t_values;
    ASSERT_TRUE(encoder.EncodeFrame(k, &t_values, &errors));
    if (k < 3) {
      EXPECT_GT(t_values.size(), 1000U) << "frame " << k;
    } else if (k > 3) {
      EXPECT_TRUE(t_values.empty()) << "frame " << k;
    } else {
      // The first frame of the gap carries only the run's final T value.
      EXPECT_EQ(t_values.size(), 1U);
    }
  }
  std::vector<std::uint8_t> tail;
  ASSERT_TRUE(encoder.Flush(&tail, &errors));
  EXPECT_TRUE(tail.empty());
  EXPECT_FALSE(encoder.EncodeFrame(0, &tail, &errors));
}

TEST(EfmDataEncoderTest, TrackTableCarriesQControlAndMode) {
  const Section data = MakeSection("Data", SectionType::kProgrammeArea, true);
  std::vector<EfmDataFrame> frames;
  AddFrames(&data, 10, 1, &frames);
  Project project;
  project.cvbs_presets.video_standard_preset = Standard::kPal;
  project.laserdisc.efm_data.q_control = EfmDataQControl::kDataCopyPermitted;
  project.laserdisc.efm_data.q_mode = kEfmDataQModeCompactDisc;
  std::vector<std::string> errors;
  EfmDataLayout layout;
  ASSERT_TRUE(BuildEfmDataLayout(Standard::kPal, frames, {}, &layout, &errors));
  efm::TrackTable table;
  ASSERT_TRUE(BuildEfmDataTrackTable(project, frames, layout, &table, &errors));
  EXPECT_EQ(table.control, efm::kControlDataCopyPermitted);
  EXPECT_EQ(table.adr, efm::kAdrMode1);
  ASSERT_EQ(table.entries.size(), 1U);
  EXPECT_EQ(table.entries[0].track_number, 1U);
}

// The Q fields of subcode section `index` of `table` (IEC 60908-1999,
// 17.5.1): TNO, index (POINT in the lead-in) and the absolute time in
// sections (zero in the lead-in, whose AMIN/ASEC/AFRAME are P times).
struct QFields {
  int track = -1;
  int index = -1;
  std::size_t absolute_sections = 0;
};

QFields DecodeQ(const efm::SubcodeGenerator& generator, std::size_t index) {
  efm::SubcodeSection section;
  EXPECT_TRUE(generator.GenerateSection(index, &section));
  const efm::QChannelPayload payload = section.QPayload();
  QFields fields;
  fields.track = static_cast<int>(FromBcd(payload[1]));
  fields.index = static_cast<int>(FromBcd(payload[2]));
  fields.absolute_sections = (FromBcd(payload[7]) * 4500) +
                             (FromBcd(payload[8]) * 75) + FromBcd(payload[9]);
  return fields;
}

TEST(EfmDataEncoderTest, ProgrammeAreaStartsAtAddressZeroInsideTheLeadIn) {
  // An AIV side: picture 1 carries addresses 153-155, so address 0 falls 51
  // frames (153 sections) into the 60-frame lead-in, at section 27.
  const Section lead_in = MakeSection("LeadIn", SectionType::kLeadIn, true);
  const Section data = MakeSection("Data", SectionType::kProgrammeArea, true);
  std::vector<EfmDataFrame> frames;
  AddFrames(&lead_in, 60, 0, &frames);
  AddFrames(&data, 20, 1, &frames);

  Project project;
  project.cvbs_presets.video_standard_preset = Standard::kPal;
  std::vector<std::string> errors;
  EfmDataLayoutSettings settings;
  settings.image_sector_count = 100;
  settings.first_address = 143;
  EfmDataLayout layout;
  ASSERT_TRUE(
      BuildEfmDataLayout(Standard::kPal, frames, settings, &layout, &errors));
  efm::TrackTable table;
  ASSERT_TRUE(BuildEfmDataTrackTable(project, frames, layout, &table, &errors))
      << (errors.empty() ? "" : errors.front());
  ASSERT_EQ(table.entries.size(), 2U);
  EXPECT_EQ(table.entries[0].area, efm::SubcodeArea::kLeadIn);
  EXPECT_EQ(table.entries[0].section_count, 27U);
  EXPECT_EQ(table.entries[1].start_section, 27U);

  efm::SubcodeGenerator generator;
  ASSERT_TRUE(generator.Begin(table));
  EXPECT_EQ(DecodeQ(generator, 26).track, 0);
  // The 2 s pause before track 1 covers addresses 0 to 149 ...
  const QFields pause_start = DecodeQ(generator, 27);
  EXPECT_EQ(pause_start.track, 1);
  EXPECT_EQ(pause_start.index, 0);
  EXPECT_EQ(pause_start.absolute_sections, 0U);
  // ... and index 01 starts at 00:02:00 in the frame before picture 1.
  const QFields track_start = DecodeQ(generator, 27 + 150);
  EXPECT_EQ(track_start.index, 1);
  EXPECT_EQ(track_start.absolute_sections, 150U);
  // Q absolute time equals the Sector Address everywhere in the programme
  // area, across the lead-in / programme boundary at section 180.
  for (std::size_t s = 27; s < generator.SectionCount(); ++s) {
    ASSERT_EQ(
        static_cast<std::int64_t>(DecodeQ(generator, s).absolute_sections),
        static_cast<std::int64_t>(s) + layout.address_offset)
        << "section " << s;
  }
  // The image's first sector, address 143, sits in the lead-in frames.
  EXPECT_EQ(layout.Slot(170).kind, EfmDataSlotKind::kImageSector);
  EXPECT_EQ(layout.Slot(170).address, 143U);
  EXPECT_GE(layout.frame_f1_start[60], 171U * efm::kFramesPerSubcodeSection);
}

TEST(EfmDataEncoderTest, RejectsALeadInTooShortForTheTableOfContents) {
  // Address 0 falls 153 sections before picture 1, which a 10-frame (30
  // section) lead-in cannot hold.
  const Section lead_in = MakeSection("LeadIn", SectionType::kLeadIn, true);
  const Section data = MakeSection("Data", SectionType::kProgrammeArea, true);
  std::vector<EfmDataFrame> frames;
  AddFrames(&lead_in, 10, 0, &frames);
  AddFrames(&data, 10, 1, &frames);

  Project project;
  project.cvbs_presets.video_standard_preset = Standard::kPal;
  std::vector<std::string> errors;
  EfmDataLayout layout;
  ASSERT_TRUE(BuildEfmDataLayout(Standard::kPal, frames, {}, &layout, &errors));
  efm::TrackTable table;
  EXPECT_FALSE(
      BuildEfmDataTrackTable(project, frames, layout, &table, &errors));
  ASSERT_EQ(errors.size(), 1U);
  EXPECT_NE(errors.front().find("'LeadIn' is too short"), std::string::npos);
  // 123 sections before the stream plus 12 for the table of contents.
  EXPECT_NE(errors.front().find("by at least 45 frames"), std::string::npos);

  // With picture_offset_sectors 0 address 0 lands at section 27, leaving
  // room for the table of contents.
  EfmDataLayoutSettings settings;
  settings.picture_offset_sectors = 0;
  ASSERT_TRUE(
      BuildEfmDataLayout(Standard::kPal, frames, settings, &layout, &errors));
  errors.clear();
  EXPECT_TRUE(BuildEfmDataTrackTable(project, frames, layout, &table, &errors));
}

TEST(EfmDataEncoderTest, AudioQControlRecordsControlZero) {
  const Section data = MakeSection("Data", SectionType::kProgrammeArea, true);
  std::vector<EfmDataFrame> frames;
  AddFrames(&data, 10, 1, &frames);
  Project project;
  project.cvbs_presets.video_standard_preset = Standard::kPal;
  project.laserdisc.efm_data.q_control = EfmDataQControl::kAudio;
  std::vector<std::string> errors;
  EfmDataLayout layout;
  ASSERT_TRUE(BuildEfmDataLayout(Standard::kPal, frames, {}, &layout, &errors));
  efm::TrackTable table;
  ASSERT_TRUE(BuildEfmDataTrackTable(project, frames, layout, &table, &errors));
  EXPECT_EQ(table.control, efm::kControlAudioNoPreEmphasisCopyProhibited);
}

TEST(EfmDataEncoderTest, BeginRejectsAnImageThatDisagreesWithTheLayout) {
  const Section data = MakeSection("Data", SectionType::kProgrammeArea, true);
  std::vector<EfmDataFrame> frames;
  AddFrames(&data, 10, 1, &frames);
  MemoryEfmSectorImage image(MakeImage(5));
  EfmDataLayoutSettings settings;
  settings.image_sector_count = 6;
  EfmDataLayout layout;
  std::vector<std::string> errors;
  ASSERT_TRUE(
      BuildEfmDataLayout(Standard::kPal, frames, settings, &layout, &errors));
  EfmDataEncoder encoder;
  EXPECT_FALSE(encoder.Begin(&layout, efm::TrackTable{}, &image, &errors));
  EXPECT_FALSE(errors.empty());
}

}  // namespace
}  // namespace videosynth
