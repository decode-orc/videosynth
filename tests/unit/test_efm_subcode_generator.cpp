/*
 * File:        test_efm_subcode_generator.cpp
 * Module:      efm
 * Purpose:     Unit tests for the P/Q subcode generator of the EFM module.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "videosynth/efm/subcode_generator.h"

namespace videosynth::efm {
namespace {

// Byte positions of the DATA-Q fields (IEC 60908-1999, 17.5.1), transcribed
// independently of the generator.
constexpr std::size_t kControlAdrByte = 0;
constexpr std::size_t kTrackNumberByte = 1;
constexpr std::size_t kIndexOrPointByte = 2;
constexpr std::size_t kTimeByte = 3;
constexpr std::size_t kZeroByte = 6;
constexpr std::size_t kPointTimeByte = 7;

constexpr std::size_t kSectionsPerSecond = kSubcodeSectionsPerSecond;

std::uint8_t FromBcd(std::uint8_t value) {
  return static_cast<std::uint8_t>(((value >> 4U) * 10U) + (value & 0x0FU));
}

SubcodeTime ReadTime(const QChannelPayload& payload, std::size_t first_byte) {
  return SubcodeTime{FromBcd(payload[first_byte]),
                     FromBcd(payload[first_byte + 1]),
                     FromBcd(payload[first_byte + 2])};
}

// Track table with a lead-in, two equal programme tracks and a lead-out.
TrackTable MakeDiscTable(VideoSystem system = VideoSystem::kPal,
                         std::size_t lead_in_sections = 4 * kSectionsPerSecond,
                         std::size_t track_sections = 8 * kSectionsPerSecond,
                         std::size_t lead_out_sections = 4 *
                                                         kSectionsPerSecond) {
  TrackTable table;
  table.video_system = system;
  std::size_t start = 0;
  table.entries.push_back({SubcodeArea::kLeadIn, 0, start, lead_in_sections});
  start += lead_in_sections;
  table.entries.push_back({SubcodeArea::kProgramme, 1, start, track_sections});
  start += track_sections;
  table.entries.push_back({SubcodeArea::kProgramme, 2, start, track_sections});
  start += track_sections;
  table.entries.push_back({SubcodeArea::kLeadOut, 0, start, lead_out_sections});
  return table;
}

SubcodeSection GenerateAt(const SubcodeGenerator& generator,
                          std::size_t section_index) {
  SubcodeSection section;
  EXPECT_TRUE(generator.GenerateSection(section_index, &section));
  return section;
}

// CRC-16 computed as a literal polynomial long division of the message shifted
// left by 16 bits, independent of the shift-register form used by the module
// (IEC 60908-1999, 17.5: P(X) = X^16 + X^12 + X^5 + 1, MSB first).
std::uint16_t IndependentQCrc(const QChannelPayload& payload) {
  std::vector<bool> bits;
  for (const std::uint8_t byte : payload) {
    for (unsigned bit = 0; bit < 8; ++bit) {
      bits.push_back(((byte >> (7U - bit)) & 1U) != 0U);
    }
  }
  bits.insert(bits.end(), 16, false);

  // Divisor coefficients of X^16 + X^12 + X^5 + 1, highest power first.
  constexpr std::array<unsigned, 4> kSetCoefficients = {16, 12, 5, 0};
  for (std::size_t index = 0; index + 16 < bits.size(); ++index) {
    if (!bits[index]) {
      continue;
    }
    for (const unsigned power : kSetCoefficients) {
      const std::size_t position = index + (16 - power);
      bits[position] = !bits[position];
    }
  }

  std::uint16_t remainder = 0;
  for (std::size_t index = bits.size() - 16; index < bits.size(); ++index) {
    remainder = static_cast<std::uint16_t>(remainder << 1U);
    if (bits[index]) {
      remainder |= 1U;
    }
  }
  // On the disc the parity bits are inverted.
  return static_cast<std::uint16_t>(remainder ^ 0xFFFFU);
}

TEST(EfmSubcodeGeneratorTest, SubcodeTimeSplitsSectionsIntoSeventyFiveFrames) {
  EXPECT_EQ(SubcodeTimeFromSections(0), (SubcodeTime{0, 0, 0}));
  EXPECT_EQ(SubcodeTimeFromSections(74), (SubcodeTime{0, 0, 74}));
  EXPECT_EQ(SubcodeTimeFromSections(75), (SubcodeTime{0, 1, 0}));
  EXPECT_EQ(SubcodeTimeFromSections(kSectionsPerSecond * 60),
            (SubcodeTime{1, 0, 0}));
  EXPECT_EQ(SubcodeTimeFromSections((kSectionsPerSecond * 60) + 74),
            (SubcodeTime{1, 0, 74}));

  for (std::size_t sections = 0; sections < kSectionsPerSecond * 61;
       sections += 37) {
    EXPECT_EQ(SectionsFromSubcodeTime(SubcodeTimeFromSections(sections)),
              sections);
  }
}

TEST(EfmSubcodeGeneratorTest, SectionCarriesNoChannelBitsInTheSyncFrames) {
  SubcodeGenerator generator;
  ASSERT_TRUE(generator.Begin(MakeDiscTable()));
  const SubcodeSection section = GenerateAt(generator, 0);

  EXPECT_EQ(kFramesPerSubcodeSection, 98U);
  EXPECT_EQ(kSubcodeChannelBits, 96U);
  EXPECT_EQ(section.ControlByte(0), 0U);
  EXPECT_EQ(section.ControlByte(1), 0U);
  EXPECT_EQ(section.ControlByte(kFramesPerSubcodeSection), 0U);

  // ECMA-130, 19.1: the sync patterns are 14 channel bits and lie outside the
  // eight-to-fourteen table.
  EXPECT_EQ(kSubcodeSyncPatternBits, 14U);
  EXPECT_EQ(kSubcodeSyncS0, 0b00100000000001U);
  EXPECT_EQ(kSubcodeSyncS1, 0b00000000010010U);
}

TEST(EfmSubcodeGeneratorTest, ControlByteHoldsOnlyThePAndQChannels) {
  SubcodeGenerator generator;
  ASSERT_TRUE(generator.Begin(MakeDiscTable()));
  const SubcodeSection section = GenerateAt(generator, 0);

  for (std::size_t frame = kSubcodeSyncFrames; frame < kFramesPerSubcodeSection;
       ++frame) {
    // IEC 60908-1999, 17.6: channels R to W inclusive are all zero.
    EXPECT_EQ(section.ControlByte(frame) & 0x3FU, 0U) << "frame " << frame;
    const std::size_t bit = frame - kSubcodeSyncFrames;
    EXPECT_EQ((section.ControlByte(frame) >> kSubcodeChannelQShift) & 1U,
              section.q_channel[bit] ? 1U : 0U);
    EXPECT_EQ((section.ControlByte(frame) >> kSubcodeChannelPShift) & 1U,
              section.p_channel[bit] ? 1U : 0U);
  }
}

TEST(EfmSubcodeGeneratorTest, BeginRejectsMalformedTrackTables) {
  SubcodeGenerator generator;

  EXPECT_FALSE(generator.Begin(TrackTable{}));

  TrackTable gapped = MakeDiscTable();
  gapped.entries[2].start_section += 1;
  EXPECT_FALSE(generator.Begin(gapped));

  TrackTable empty_entry = MakeDiscTable();
  empty_entry.entries[1].section_count = 0;
  EXPECT_FALSE(generator.Begin(empty_entry));

  TrackTable bad_track_number = MakeDiscTable();
  bad_track_number.entries[1].track_number = kMaxTrackNumber + 1;
  EXPECT_FALSE(generator.Begin(bad_track_number));

  TrackTable zero_track_number = MakeDiscTable();
  zero_track_number.entries[1].track_number = 0;
  EXPECT_FALSE(generator.Begin(zero_track_number));

  TrackTable out_of_order;
  out_of_order.entries.push_back({SubcodeArea::kLeadOut, 0, 0, 10});
  out_of_order.entries.push_back({SubcodeArea::kProgramme, 1, 10, 10});
  EXPECT_FALSE(generator.Begin(out_of_order));
}

TEST(EfmSubcodeGeneratorTest, GenerateSectionRejectsBadArguments) {
  SubcodeGenerator generator;
  SubcodeSection section;
  EXPECT_FALSE(generator.GenerateSection(0, &section));

  const TrackTable table = MakeDiscTable();
  ASSERT_TRUE(generator.Begin(table));
  EXPECT_FALSE(generator.GenerateSection(0, nullptr));
  EXPECT_TRUE(
      generator.GenerateSection(generator.SectionCount() - 1, &section));
  EXPECT_FALSE(generator.GenerateSection(generator.SectionCount(), &section));

  generator.Reset();
  EXPECT_EQ(generator.SectionCount(), 0U);
  EXPECT_FALSE(generator.GenerateSection(0, &section));
}

TEST(EfmSubcodeGeneratorTest, ProgrammeQUsesMode4AdrAndAudioControl) {
  const TrackTable table = MakeDiscTable();
  SubcodeGenerator generator;
  ASSERT_TRUE(generator.Begin(table));

  const std::size_t programme_start = table.entries[1].start_section;
  const QChannelPayload payload =
      GenerateAt(generator, programme_start + kTrackOnePauseSections)
          .QPayload();

  // IEC 60856:1986 Amd 2, 13.5.1.1: ADR = 4 = 0100. IEC 60908-1999, 17.5:
  // control 0000 is two audio channels without pre-emphasis, copy prohibited.
  EXPECT_EQ(payload[kControlAdrByte] & 0x0FU, kAdrMode4);
  EXPECT_EQ(payload[kControlAdrByte] >> 4U,
            kControlAudioNoPreEmphasisCopyProhibited);
  EXPECT_EQ(payload[kZeroByte], 0U);
  EXPECT_EQ(payload[kTrackNumberByte], 0x01U);
  EXPECT_EQ(payload[kIndexOrPointByte], kIndexAudio);
}

TEST(EfmSubcodeGeneratorTest, DataTableSetsControlAndAdrInEveryArea) {
  TrackTable table = MakeDiscTable();
  table.control = kControlDataCopyProhibited;
  table.adr = kAdrMode1;
  SubcodeGenerator generator;
  ASSERT_TRUE(generator.Begin(table));

  // IEC 60908-1999, 17.5: control 0100 is a data track, copy prohibited; ADR
  // 1 is the compact-disc mode some AIV discs record instead of mode 4.
  const std::size_t sample_sections[] = {
      0, table.entries[1].start_section + kTrackOnePauseSections,
      table.entries[3].start_section};
  for (const std::size_t index : sample_sections) {
    const SubcodeSection section = GenerateAt(generator, index);
    const QChannelPayload payload = section.QPayload();
    EXPECT_EQ(payload[kControlAdrByte], 0x41U) << "section " << index;
    EXPECT_EQ(section.QCrc(), IndependentQCrc(payload)) << "section " << index;
  }
}

TEST(EfmSubcodeGeneratorTest, AdrChangesOnlyTheAdrNibble) {
  TrackTable table = MakeDiscTable();
  SubcodeGenerator mode4;
  ASSERT_TRUE(mode4.Begin(table));
  table.adr = kAdrMode1;
  SubcodeGenerator mode1;
  ASSERT_TRUE(mode1.Begin(table));

  for (std::size_t index = 0; index < mode4.SectionCount(); index += 37) {
    QChannelPayload a = GenerateAt(mode4, index).QPayload();
    QChannelPayload b = GenerateAt(mode1, index).QPayload();
    EXPECT_EQ(a[kControlAdrByte] & 0x0FU, kAdrMode4);
    EXPECT_EQ(b[kControlAdrByte] & 0x0FU, kAdrMode1);
    a[kControlAdrByte] = 0;
    b[kControlAdrByte] = 0;
    EXPECT_EQ(a, b) << "section " << index;
  }
}

TEST(EfmSubcodeGeneratorTest, BeginRejectsControlOrAdrWiderThanANibble) {
  TrackTable table = MakeDiscTable();
  table.control = 0x10;
  SubcodeGenerator generator;
  EXPECT_FALSE(generator.Begin(table));
  table.control = kControlDataCopyPermitted;
  table.adr = 0x10;
  EXPECT_FALSE(generator.Begin(table));
}

TEST(EfmSubcodeGeneratorTest, TrackOnePauseCountsDownToZeroAtTheAudioStart) {
  const TrackTable table = MakeDiscTable();
  SubcodeGenerator generator;
  ASSERT_TRUE(generator.Begin(table));

  const std::size_t programme_start = table.entries[1].start_section;
  for (std::size_t offset = 0; offset < kTrackOnePauseSections; ++offset) {
    const QChannelPayload payload =
        GenerateAt(generator, programme_start + offset).QPayload();
    EXPECT_EQ(payload[kTrackNumberByte], 0x01U) << "offset " << offset;
    EXPECT_EQ(payload[kIndexOrPointByte], kIndexPause) << "offset " << offset;
    EXPECT_EQ(ReadTime(payload, kTimeByte),
              SubcodeTimeFromSections(kTrackOnePauseSections - 1 - offset))
        << "offset " << offset;
  }

  const QChannelPayload last_pause =
      GenerateAt(generator, programme_start + kTrackOnePauseSections - 1)
          .QPayload();
  EXPECT_EQ(ReadTime(last_pause, kTimeByte), (SubcodeTime{0, 0, 0}));

  const QChannelPayload first_audio =
      GenerateAt(generator, programme_start + kTrackOnePauseSections)
          .QPayload();
  EXPECT_EQ(first_audio[kIndexOrPointByte], kIndexAudio);
  EXPECT_EQ(ReadTime(first_audio, kTimeByte), (SubcodeTime{0, 0, 0}));
}

TEST(EfmSubcodeGeneratorTest, RunningTimeResetsAtATrackBoundary) {
  const TrackTable table = MakeDiscTable();
  SubcodeGenerator generator;
  ASSERT_TRUE(generator.Begin(table));

  const std::size_t track_two_start = table.entries[2].start_section;

  const QChannelPayload before =
      GenerateAt(generator, track_two_start - 1).QPayload();
  EXPECT_EQ(before[kTrackNumberByte], 0x01U);

  const QChannelPayload at_start =
      GenerateAt(generator, track_two_start).QPayload();
  EXPECT_EQ(at_start[kTrackNumberByte], 0x02U);
  EXPECT_EQ(at_start[kIndexOrPointByte], kIndexAudio);
  EXPECT_EQ(ReadTime(at_start, kTimeByte), (SubcodeTime{0, 0, 0}));

  const QChannelPayload later =
      GenerateAt(generator, track_two_start + kSectionsPerSecond).QPayload();
  EXPECT_EQ(ReadTime(later, kTimeByte), (SubcodeTime{0, 1, 0}));
}

TEST(EfmSubcodeGeneratorTest, AbsoluteTimeIsContinuousAcrossTrackBoundaries) {
  const TrackTable table = MakeDiscTable();
  SubcodeGenerator generator;
  ASSERT_TRUE(generator.Begin(table));

  const std::size_t programme_start = table.entries[1].start_section;
  for (std::size_t index = programme_start; index < generator.SectionCount();
       ++index) {
    const QChannelPayload payload = GenerateAt(generator, index).QPayload();
    EXPECT_EQ(ReadTime(payload, kPointTimeByte),
              SubcodeTimeFromSections(index - programme_start))
        << "section " << index;
  }
}

TEST(EfmSubcodeGeneratorTest, RunningTimeRollsOverSecondsAndMinutes) {
  TrackTable table;
  table.entries.push_back({SubcodeArea::kProgramme, 1, 0,
                           kTrackOnePauseSections + (61 * kSectionsPerSecond)});
  SubcodeGenerator generator;
  ASSERT_TRUE(generator.Begin(table));

  const std::size_t audio_start = kTrackOnePauseSections;
  const auto time_at = [&](std::size_t offset) {
    return ReadTime(GenerateAt(generator, audio_start + offset).QPayload(),
                    kTimeByte);
  };

  EXPECT_EQ(time_at(74), (SubcodeTime{0, 0, 74}));
  EXPECT_EQ(time_at(75), (SubcodeTime{0, 1, 0}));
  EXPECT_EQ(time_at((59 * kSectionsPerSecond) + 74), (SubcodeTime{0, 59, 74}));
  EXPECT_EQ(time_at(60 * kSectionsPerSecond), (SubcodeTime{1, 0, 0}));
}

TEST(EfmSubcodeGeneratorTest, LeadInTocMatchesTheTrackTable) {
  const TrackTable table = MakeDiscTable();
  SubcodeGenerator generator;
  ASSERT_TRUE(generator.Begin(table));

  const std::size_t programme_start = table.entries[1].start_section;
  const std::size_t lead_out_start = table.entries[3].start_section;

  // One POINT entry per track plus A0, A1 and A2 (IEC 60908-1999, 17.5.1).
  const std::vector<TocItem>& items = generator.TocItems();
  ASSERT_EQ(items.size(), 5U);
  EXPECT_EQ(items[0].point, 0x01U);
  EXPECT_EQ(items[0].point_time,
            SubcodeTimeFromSections(kTrackOnePauseSections));
  EXPECT_EQ(items[1].point, 0x02U);
  EXPECT_EQ(items[1].point_time,
            SubcodeTimeFromSections(table.entries[2].start_section -
                                    programme_start));
  EXPECT_EQ(items[2].point, kTocPointFirstTrack);
  EXPECT_EQ(items[2].point_time.minutes, 1U);
  EXPECT_EQ(items[3].point, kTocPointLastTrack);
  EXPECT_EQ(items[3].point_time, (SubcodeTime{2, 0, 0}));
  EXPECT_EQ(items[4].point, kTocPointLeadOutStart);
  EXPECT_EQ(items[4].point_time,
            SubcodeTimeFromSections(lead_out_start - programme_start));
}

TEST(EfmSubcodeGeneratorTest, LeadInRepeatsEachTocItemThreeTimesAndCycles) {
  const TrackTable table = MakeDiscTable();
  SubcodeGenerator generator;
  ASSERT_TRUE(generator.Begin(table));

  const std::size_t lead_in_sections = table.entries[0].section_count;
  const std::vector<TocItem>& items = generator.TocItems();

  for (std::size_t index = 0; index < lead_in_sections; ++index) {
    const QChannelPayload payload = GenerateAt(generator, index).QPayload();
    // IEC 60908-1999, 17.5.1: during the lead-in track TNO is 00 and the
    // running time increases.
    EXPECT_EQ(payload[kTrackNumberByte], kTrackNumberLeadIn);
    EXPECT_EQ(ReadTime(payload, kTimeByte), SubcodeTimeFromSections(index));

    const TocItem& expected = items[(index / kTocItemRepeats) % items.size()];
    EXPECT_EQ(payload[kIndexOrPointByte], expected.point)
        << "section " << index;
    EXPECT_EQ(ReadTime(payload, kPointTimeByte), expected.point_time)
        << "section " << index;
  }

  // The lead-in is longer than one cycle, so the table repeats.
  EXPECT_GT(lead_in_sections, items.size() * kTocItemRepeats);
}

TEST(EfmSubcodeGeneratorTest, LeadInCarriesTheVideoSystemIdentification) {
  SubcodeGenerator pal_generator;
  ASSERT_TRUE(pal_generator.Begin(MakeDiscTable(VideoSystem::kPal)));
  const TocItem& pal_item = pal_generator.TocItems()[2];
  ASSERT_EQ(pal_item.point, kTocPointFirstTrack);
  // IEC 60856:1986 Amd 2, 13.5.2: P frame 22 = PAL LV disc with digital stereo.
  EXPECT_EQ(pal_item.point_time.minutes, 1U);
  EXPECT_EQ(pal_item.point_time.seconds, 0U);
  EXPECT_EQ(pal_item.point_time.frames, kVideoSystemIdentificationPal);

  SubcodeGenerator ntsc_generator;
  ASSERT_TRUE(ntsc_generator.Begin(MakeDiscTable(VideoSystem::kNtsc)));
  const TocItem& ntsc_item = ntsc_generator.TocItems()[2];
  ASSERT_EQ(ntsc_item.point, kTocPointFirstTrack);
  // IEC 60857:1986 Amd 2, 13.6.2: P frame 12 = NTSC LV disc with digital
  // stereo.
  EXPECT_EQ(ntsc_item.point_time.frames, kVideoSystemIdentificationNtsc);

  // The fields are recorded BCD encoded.
  SubcodeSection section;
  ASSERT_TRUE(pal_generator.GenerateSection(2 * kTocItemRepeats, &section));
  EXPECT_EQ(section.QPayload()[kPointTimeByte + 2], 0x22U);
  ASSERT_TRUE(ntsc_generator.GenerateSection(2 * kTocItemRepeats, &section));
  EXPECT_EQ(section.QPayload()[kPointTimeByte + 2], 0x12U);
}

TEST(EfmSubcodeGeneratorTest, ProgrammeOnlyTableEmitsNoToc) {
  TrackTable table;
  table.entries.push_back({SubcodeArea::kProgramme, 1, 0, 600});
  SubcodeGenerator generator;
  ASSERT_TRUE(generator.Begin(table));
  EXPECT_TRUE(generator.TocItems().empty());
  EXPECT_EQ(generator.SectionCount(), 600U);
}

TEST(EfmSubcodeGeneratorTest, LeadOutUsesTrackNumberAaAndIncreasingTime) {
  const TrackTable table = MakeDiscTable();
  SubcodeGenerator generator;
  ASSERT_TRUE(generator.Begin(table));

  const std::size_t lead_out_start = table.entries[3].start_section;
  for (std::size_t offset = 0; offset < table.entries[3].section_count;
       offset += 17) {
    const QChannelPayload payload =
        GenerateAt(generator, lead_out_start + offset).QPayload();
    // IEC 60908-1999, 17.5.1: the lead-out track has TNO = AA and is encoded
    // as audio, with an increasing running time.
    EXPECT_EQ(payload[kTrackNumberByte], kTrackNumberLeadOut);
    EXPECT_EQ(payload[kIndexOrPointByte], kIndexAudio);
    EXPECT_EQ(ReadTime(payload, kTimeByte), SubcodeTimeFromSections(offset));
  }
}

TEST(EfmSubcodeGeneratorTest, StartFlagPrecedesEveryTrackAndTheLeadOut) {
  const TrackTable table = MakeDiscTable();
  SubcodeGenerator generator;
  ASSERT_TRUE(generator.Begin(table));

  const std::size_t programme_start = table.entries[1].start_section;
  const std::size_t track_two_start = table.entries[2].start_section;
  const std::size_t lead_out_start = table.entries[3].start_section;

  // IEC 60908-1999, 17.4: the start flag is 2 s long and its end indicates the
  // start of the next track; the flag is delayed one section relative to Q.
  const std::array<std::size_t, 3> targets = {
      programme_start + kTrackOnePauseSections, track_two_start,
      lead_out_start};
  for (const std::size_t target : targets) {
    EXPECT_FALSE(
        GenerateAt(generator, target - kStartFlagSections).p_channel.front())
        << "target " << target;
    EXPECT_TRUE(GenerateAt(generator, target - kStartFlagSections + 1)
                    .p_channel.front())
        << "target " << target;
    EXPECT_TRUE(GenerateAt(generator, target).p_channel.front())
        << "target " << target;
    EXPECT_FALSE(GenerateAt(generator, target + 1).p_channel.front())
        << "target " << target;
  }

  // In the lead-in track channel P is encoded as for audio.
  for (std::size_t index = 0; index < table.entries[0].section_count; ++index) {
    EXPECT_FALSE(GenerateAt(generator, index).p_channel.front())
        << "section " << index;
  }
}

TEST(EfmSubcodeGeneratorTest, StartFlagIsConstantWithinASection) {
  const TrackTable table = MakeDiscTable();
  SubcodeGenerator generator;
  ASSERT_TRUE(generator.Begin(table));

  const std::size_t track_two_start = table.entries[2].start_section;
  const SubcodeSection section = GenerateAt(generator, track_two_start);
  for (const bool bit : section.p_channel) {
    // IEC 60908-1999, 17.4: a change in channel P may take place only
    // immediately after the sync patterns S0 and S1.
    EXPECT_TRUE(bit);
  }
}

TEST(EfmSubcodeGeneratorTest, LeadOutPChannelSwitchesAtTwoHertz) {
  const TrackTable table =
      MakeDiscTable(VideoSystem::kPal, 4 * kSectionsPerSecond,
                    8 * kSectionsPerSecond, 8 * kSectionsPerSecond);
  SubcodeGenerator generator;
  ASSERT_TRUE(generator.Begin(table));

  const std::size_t lead_out_start = table.entries[3].start_section;

  // IEC 60908-1999, 17.4: P stays zero for 2 s to 3 s after the start of the
  // lead-out track.
  for (std::size_t offset = 1; offset <= kLeadOutPSilentSections; ++offset) {
    EXPECT_FALSE(
        GenerateAt(generator, lead_out_start + offset).p_channel.front())
        << "offset " << offset;
  }

  // Then P switches in a 2 Hz rhythm at a 50 % duty cycle.
  const std::size_t first_switch = lead_out_start + kLeadOutPSilentSections + 1;
  for (std::size_t offset = 0; offset < kLeadOutPHalfPeriodSections; ++offset) {
    EXPECT_TRUE(GenerateAt(generator, first_switch + offset).p_channel.front())
        << "offset " << offset;
    EXPECT_FALSE(GenerateAt(generator,
                            first_switch + kLeadOutPHalfPeriodSections + offset)
                     .p_channel.front())
        << "offset " << offset;
  }

  // The rhythm is within the 2 Hz +/- 2 % of IEC 60908-1999, 17.4.
  const double frequency =
      static_cast<double>(kSubcodeSectionsPerSecond) /
      (2.0 * static_cast<double>(kLeadOutPHalfPeriodSections));
  EXPECT_NEAR(frequency, 2.0, 2.0 * 0.02);
}

TEST(EfmSubcodeGeneratorTest, QCrcMatchesIndependentPolynomialDivision) {
  const TrackTable table = MakeDiscTable();
  SubcodeGenerator generator;
  ASSERT_TRUE(generator.Begin(table));

  for (std::size_t index = 0; index < generator.SectionCount(); index += 13) {
    const SubcodeSection section = GenerateAt(generator, index);
    const QChannelPayload payload = section.QPayload();
    EXPECT_EQ(section.QCrc(), ComputeQChannelCrc(payload))
        << "section " << index;
    EXPECT_EQ(section.QCrc(), IndependentQCrc(payload)) << "section " << index;
  }
}

TEST(EfmSubcodeGeneratorTest, QCrcDetectsSingleBitCorruption) {
  const TrackTable table = MakeDiscTable();
  SubcodeGenerator generator;
  ASSERT_TRUE(generator.Begin(table));

  SubcodeSection section =
      GenerateAt(generator, table.entries[1].start_section);
  const std::uint16_t recorded = section.QCrc();
  ASSERT_EQ(recorded, ComputeQChannelCrc(section.QPayload()));

  for (std::size_t bit = 0; bit < kQChannelPayloadBytes * 8; ++bit) {
    SubcodeSection corrupted = section;
    corrupted.q_channel[bit] = !corrupted.q_channel[bit];
    EXPECT_NE(ComputeQChannelCrc(corrupted.QPayload()), recorded)
        << "bit " << bit;
  }
}

TEST(EfmSubcodeGeneratorTest, GeneratedSectionsAreDeterministic) {
  const TrackTable table = MakeDiscTable();
  SubcodeGenerator first;
  SubcodeGenerator second;
  ASSERT_TRUE(first.Begin(table));
  ASSERT_TRUE(second.Begin(table));

  for (std::size_t index = 0; index < first.SectionCount(); index += 7) {
    const SubcodeSection left = GenerateAt(first, index);
    const SubcodeSection right = GenerateAt(second, index);
    EXPECT_EQ(left.q_channel, right.q_channel) << "section " << index;
    EXPECT_EQ(left.p_channel, right.p_channel) << "section " << index;
  }
}

}  // namespace
}  // namespace videosynth::efm
