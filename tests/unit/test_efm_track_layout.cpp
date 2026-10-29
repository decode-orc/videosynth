/*
 * File:        test_efm_track_layout.cpp
 * Module:      efm_track_layout_tests
 * Purpose:     Unit tests for the mapping of a project's output section layout
 *              onto the EFM subcode track table.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <vector>

#include "videosynth/efm_track_layout.h"

namespace videosynth {
namespace {

// 8 video frames of PAL audio: 8 x 1764 samples = 24 subcode sections
// (IEC 60856:1986 Amd 2, 13.2).
constexpr std::size_t kPalFramesPerSection = 8;
constexpr std::size_t kPalSubcodeSectionsPerRun = 24;

Section MakeSection(const std::string& name, SectionType type) {
  Section section;
  section.name = name;
  section.section_type = type;
  return section;
}

// The output-order frame list for `sections`, `frames_per_section` frames each.
std::vector<const Section*> FrameList(const std::vector<Section>& sections,
                                      std::size_t frames_per_section) {
  std::vector<const Section*> frames;
  for (const Section& section : sections) {
    for (std::size_t index = 0; index < frames_per_section; ++index) {
      frames.push_back(&section);
    }
  }
  return frames;
}

TEST(EfmTrackLayoutTest, MapsPalSectionsToTracksInOutputOrder) {
  const std::vector<Section> sections = {
      MakeSection("LeadIn", SectionType::kLeadIn),
      MakeSection("Chapter0", SectionType::kProgrammeArea),
      MakeSection("Chapter1", SectionType::kProgrammeArea),
      MakeSection("LeadOut", SectionType::kLeadOut),
  };

  EfmTrackLayout layout;
  std::vector<std::string> errors;
  ASSERT_TRUE(BuildEfmTrackLayout(Standard::kPal,
                                  FrameList(sections, kPalFramesPerSection),
                                  &layout, &errors))
      << (errors.empty() ? "" : errors.front());

  EXPECT_EQ(layout.table.video_system, efm::VideoSystem::kPal);
  ASSERT_EQ(layout.table.entries.size(), 4U);
  for (std::size_t index = 0; index < layout.table.entries.size(); ++index) {
    const efm::TrackTableEntry& entry = layout.table.entries[index];
    EXPECT_EQ(entry.start_section, index * kPalSubcodeSectionsPerRun);
    EXPECT_EQ(entry.section_count, kPalSubcodeSectionsPerRun);
  }
  EXPECT_EQ(layout.table.entries[0].area, efm::SubcodeArea::kLeadIn);
  EXPECT_EQ(layout.table.entries[1].area, efm::SubcodeArea::kProgramme);
  EXPECT_EQ(layout.table.entries[1].track_number, 1);
  EXPECT_EQ(layout.table.entries[2].area, efm::SubcodeArea::kProgramme);
  EXPECT_EQ(layout.table.entries[2].track_number, 2);
  EXPECT_EQ(layout.table.entries[3].area, efm::SubcodeArea::kLeadOut);

  // The subcode generator accepts the table as a valid disc layout.
  efm::SubcodeGenerator generator;
  EXPECT_TRUE(generator.Begin(layout.table));
}

TEST(EfmTrackLayoutTest, PauseCoversTheStartOfTheFirstTrack) {
  const std::vector<Section> sections = {
      MakeSection("LeadIn", SectionType::kLeadIn),
      MakeSection("Chapter0", SectionType::kProgrammeArea),
  };

  EfmTrackLayout layout;
  std::vector<std::string> errors;
  ASSERT_TRUE(BuildEfmTrackLayout(Standard::kPal,
                                  FrameList(sections, kPalFramesPerSection),
                                  &layout, &errors));

  // IEC 60908-1999, 17.5.1: the pause before track 1 lasts 2 s to 3 s; a track
  // shorter than that is pause throughout.
  EXPECT_EQ(layout.pause_start_sample,
            kPalSubcodeSectionsPerRun * kEfmSamplesPerSubcodeSection);
  EXPECT_EQ(layout.pause_end_sample,
            layout.pause_start_sample +
                (kPalSubcodeSectionsPerRun * kEfmSamplesPerSubcodeSection));
}

TEST(EfmTrackLayoutTest, PauseIsTwoSecondsWhenTheFirstTrackIsLongEnough) {
  // 4 s of PAL programme area: longer than the 2 s pause.
  const std::vector<Section> sections = {
      MakeSection("Chapter0", SectionType::kProgrammeArea),
  };

  EfmTrackLayout layout;
  std::vector<std::string> errors;
  ASSERT_TRUE(BuildEfmTrackLayout(Standard::kPal, FrameList(sections, 100U),
                                  &layout, &errors));

  EXPECT_EQ(layout.pause_start_sample, 0U);
  EXPECT_EQ(layout.pause_end_sample,
            efm::kTrackOnePauseSections * kEfmSamplesPerSubcodeSection);
  // 2 s at 44 100 Hz.
  EXPECT_EQ(layout.pause_end_sample, 88200U);
}

TEST(EfmTrackLayoutTest, NtscBoundariesLandOnAscendingSubcodeSections) {
  const std::vector<Section> sections = {
      MakeSection("LeadIn", SectionType::kLeadIn),
      MakeSection("Chapter0", SectionType::kProgrammeArea),
      MakeSection("Chapter1", SectionType::kProgrammeArea),
      MakeSection("LeadOut", SectionType::kLeadOut),
  };

  EfmTrackLayout layout;
  std::vector<std::string> errors;
  ASSERT_TRUE(BuildEfmTrackLayout(Standard::kNtsc, FrameList(sections, 9U),
                                  &layout, &errors))
      << (errors.empty() ? "" : errors.front());

  EXPECT_EQ(layout.table.video_system, efm::VideoSystem::kNtsc);
  ASSERT_EQ(layout.table.entries.size(), 4U);
  // SMPTE 272M-1994 Table 1: NTSC sample counts alternate, so boundaries do not
  // divide evenly; entries must still tile the stream in ascending order.
  std::size_t next_start = 0;
  for (const efm::TrackTableEntry& entry : layout.table.entries) {
    EXPECT_EQ(entry.start_section, next_start);
    EXPECT_GT(entry.section_count, 0U);
    next_start += entry.section_count;
  }
  efm::SubcodeGenerator generator;
  EXPECT_TRUE(generator.Begin(layout.table));
}

TEST(EfmTrackLayoutTest, MergesAdjacentLeadInAndLeadOutSections) {
  const std::vector<Section> sections = {
      MakeSection("LeadInA", SectionType::kLeadIn),
      MakeSection("LeadInB", SectionType::kLeadIn),
      MakeSection("Chapter0", SectionType::kProgrammeArea),
      MakeSection("LeadOutA", SectionType::kLeadOut),
      MakeSection("LeadOutB", SectionType::kLeadOut),
  };

  EfmTrackLayout layout;
  std::vector<std::string> errors;
  ASSERT_TRUE(BuildEfmTrackLayout(Standard::kPal,
                                  FrameList(sections, kPalFramesPerSection),
                                  &layout, &errors));

  ASSERT_EQ(layout.table.entries.size(), 3U);
  EXPECT_EQ(layout.table.entries[0].area, efm::SubcodeArea::kLeadIn);
  EXPECT_EQ(layout.table.entries[0].section_count,
            2U * kPalSubcodeSectionsPerRun);
  EXPECT_EQ(layout.table.entries[2].area, efm::SubcodeArea::kLeadOut);
  EXPECT_EQ(layout.table.entries[2].section_count,
            2U * kPalSubcodeSectionsPerRun);
}

TEST(EfmTrackLayoutTest, UntypedSectionsBecomeProgrammeTracks) {
  const std::vector<Section> sections = {
      MakeSection("First", SectionType::kUnknown),
      MakeSection("Second", SectionType::kUnknown),
  };

  EfmTrackLayout layout;
  std::vector<std::string> errors;
  ASSERT_TRUE(BuildEfmTrackLayout(Standard::kPal,
                                  FrameList(sections, kPalFramesPerSection),
                                  &layout, &errors));

  ASSERT_EQ(layout.table.entries.size(), 2U);
  EXPECT_EQ(layout.table.entries[0].track_number, 1);
  EXPECT_EQ(layout.table.entries[1].track_number, 2);
}

TEST(EfmTrackLayoutTest, RejectsStandardsWithoutLaserDiscDigitalAudio) {
  const std::vector<Section> sections = {
      MakeSection("Chapter0", SectionType::kProgrammeArea)};

  EfmTrackLayout layout;
  std::vector<std::string> errors;
  EXPECT_FALSE(BuildEfmTrackLayout(Standard::kSecam,
                                   FrameList(sections, kPalFramesPerSection),
                                   &layout, &errors));
  ASSERT_EQ(errors.size(), 1U);
  EXPECT_NE(errors.front().find("PAL, NTSC and PAL-M"), std::string::npos);
}

TEST(EfmTrackLayoutTest, PalMUsesThe525LineDiscDefinition) {
  const std::vector<Section> sections = {
      MakeSection("LeadIn", SectionType::kLeadIn),
      MakeSection("Chapter0", SectionType::kProgrammeArea),
      MakeSection("LeadOut", SectionType::kLeadOut),
  };

  EfmTrackLayout pal_m;
  EfmTrackLayout ntsc;
  std::vector<std::string> errors;
  ASSERT_TRUE(BuildEfmTrackLayout(Standard::kPalM, FrameList(sections, 9U),
                                  &pal_m, &errors))
      << (errors.empty() ? "" : errors.front());
  ASSERT_TRUE(BuildEfmTrackLayout(Standard::kNtsc, FrameList(sections, 9U),
                                  &ntsc, &errors));

  // IEC 60857:1986 Amd 2, 13.6.2: a 525-line LV disc identifies as such.
  EXPECT_EQ(pal_m.table.video_system, efm::VideoSystem::kNtsc);
  ASSERT_EQ(pal_m.table.entries.size(), ntsc.table.entries.size());
  for (std::size_t i = 0; i < pal_m.table.entries.size(); ++i) {
    EXPECT_EQ(pal_m.table.entries[i].start_section,
              ntsc.table.entries[i].start_section);
    EXPECT_EQ(pal_m.table.entries[i].section_count,
              ntsc.table.entries[i].section_count);
  }
}

TEST(EfmTrackLayoutTest, RejectsSectionsOutOfDiscOrder) {
  const std::vector<Section> sections = {
      MakeSection("Chapter0", SectionType::kProgrammeArea),
      MakeSection("LateLeadIn", SectionType::kLeadIn),
  };

  EfmTrackLayout layout;
  std::vector<std::string> errors;
  EXPECT_FALSE(BuildEfmTrackLayout(Standard::kPal,
                                   FrameList(sections, kPalFramesPerSection),
                                   &layout, &errors));
  ASSERT_EQ(errors.size(), 1U);
  EXPECT_NE(errors.front().find("LateLeadIn"), std::string::npos);
}

TEST(EfmTrackLayoutTest, RejectsMoreTracksThanTheDiscAllows) {
  // IEC 60856:1986 Amd 2, 13.5.3.3: the maximum track number is 79.
  std::vector<Section> sections;
  for (int index = 0; index <= efm::kMaxTrackNumber; ++index) {
    sections.push_back(MakeSection("Chapter" + std::to_string(index),
                                   SectionType::kProgrammeArea));
  }

  EfmTrackLayout layout;
  std::vector<std::string> errors;
  EXPECT_FALSE(BuildEfmTrackLayout(Standard::kPal, FrameList(sections, 1U),
                                   &layout, &errors));
  ASSERT_EQ(errors.size(), 1U);
  EXPECT_NE(errors.front().find("79"), std::string::npos);
}

TEST(EfmTrackLayoutTest, RejectsAnEmptyOutput) {
  EfmTrackLayout layout;
  std::vector<std::string> errors;
  EXPECT_FALSE(BuildEfmTrackLayout(Standard::kPal, {}, &layout, &errors));
  EXPECT_EQ(errors.size(), 1U);
}

}  // namespace
}  // namespace videosynth
