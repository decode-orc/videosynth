/*
 * File:        test_efm_data_layout.cpp
 * Module:      efm_data_layout
 * Purpose:     Unit tests for the placement of EFM data sectors by the AIV
 *              picture rule and the frames that carry the EFM signal.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <vector>

#include "videosynth/efm_data_layout.h"

namespace videosynth {
namespace {

Section MakeSection(const std::string& name, SectionType type, bool carries) {
  Section section;
  section.name = name;
  section.section_type = type;
  section.carries_efm_data = carries;
  return section;
}

// Appends `count` frames of `section`, numbering pictures from
// `first_picture` (0 = no picture numbers).
void AddFrames(const Section* section, std::size_t count, int first_picture,
               std::vector<EfmDataFrame>* frames) {
  for (std::size_t index = 0; index < count; ++index) {
    frames->push_back({section, first_picture > 0
                                    ? first_picture + static_cast<int>(index)
                                    : 0});
  }
}

EfmDataLayout Build(Standard standard, const std::vector<EfmDataFrame>& frames,
                    std::size_t image_sectors, std::int64_t first_address = 0) {
  EfmDataLayoutSettings settings;
  settings.image_sector_count = image_sectors;
  settings.first_address = first_address;
  EfmDataLayout layout;
  std::vector<std::string> errors;
  EXPECT_TRUE(BuildEfmDataLayout(standard, frames, settings, &layout, &errors))
      << (errors.empty() ? "" : errors.front());
  return layout;
}

TEST(EfmDataLayoutTest, PalPictureCarriesSectorsThreePPlus150) {
  const Section lead_in = MakeSection("LeadIn", SectionType::kLeadIn, true);
  const Section programme =
      MakeSection("Stills", SectionType::kProgrammeArea, true);
  std::vector<EfmDataFrame> frames;
  AddFrames(&lead_in, 10, 0, &frames);
  AddFrames(&programme, 40, 1, &frames);

  const EfmDataLayout layout = Build(Standard::kPal, frames, 1000);
  ASSERT_TRUE(layout.anchor_from_picture_number);
  EXPECT_EQ(layout.anchor_frame, 10U);
  EXPECT_EQ(layout.anchor_picture, 1);

  // PAL: 1764 samples = 294 F1 frames = 3 sections per picture.
  for (int picture = 1; picture <= 40; ++picture) {
    const std::size_t frame = 9 + static_cast<std::size_t>(picture);
    const std::size_t first_section = layout.frame_f1_start[frame] / 98;
    for (std::size_t offset = 0; offset < 3; ++offset) {
      const EfmDataSlot slot = layout.Slot(first_section + offset);
      ASSERT_EQ(slot.kind, EfmDataSlotKind::kImageSector);
      EXPECT_EQ(slot.image_index,
                static_cast<std::size_t>(3 * picture + 150) + offset)
          << "picture " << picture;
    }
  }
  // The lead-in carries the image sectors before picture 1: 123 to 152.
  EXPECT_EQ(layout.Slot(0).image_index, 123U);
  EXPECT_EQ(layout.Slot(29).image_index, 152U);
}

TEST(EfmDataLayoutTest, SectorsBeforeTheImageAreLeadInFill) {
  const Section lead_in = MakeSection("LeadIn", SectionType::kLeadIn, true);
  const Section programme =
      MakeSection("Stills", SectionType::kProgrammeArea, true);
  std::vector<EfmDataFrame> frames;
  AddFrames(&lead_in, 60, 0, &frames);
  AddFrames(&programme, 10, 1, &frames);

  const EfmDataLayout layout = Build(Standard::kPal, frames, 1000);
  // Picture 1 is section 180, address 153, so section 27 has address 0.
  EXPECT_EQ(layout.address_offset, 153 - 180);
  EXPECT_EQ(layout.Slot(26).kind, EfmDataSlotKind::kLeadInFill);
  EXPECT_EQ(layout.Slot(26).address, 26U);
  EXPECT_EQ(layout.Slot(27).kind, EfmDataSlotKind::kImageSector);
  EXPECT_EQ(layout.Slot(27).image_index, 0U);
}

TEST(EfmDataLayoutTest, SectorsAfterTheImageAreMode0WithContinuingAddresses) {
  const Section programme =
      MakeSection("Stills", SectionType::kProgrammeArea, true);
  std::vector<EfmDataFrame> frames;
  AddFrames(&programme, 10, 1, &frames);

  // Picture 1 (section 0) carries address 153; an image of 160 sectors ends
  // at section 6.
  const EfmDataLayout layout = Build(Standard::kPal, frames, 160);
  EXPECT_EQ(layout.Slot(6).kind, EfmDataSlotKind::kImageSector);
  EXPECT_EQ(layout.Slot(7).kind, EfmDataSlotKind::kMode0Fill);
  EXPECT_EQ(layout.Slot(7).address, 160U);
}

TEST(EfmDataLayoutTest, FirstAddressShiftsTheImage) {
  const Section programme =
      MakeSection("Stills", SectionType::kProgrammeArea, true);
  std::vector<EfmDataFrame> frames;
  AddFrames(&programme, 10, 1, &frames);

  const EfmDataLayout layout = Build(Standard::kPal, frames, 100, 150);
  // Address 153 is image sector 3; address 152 would be before the image but
  // is not in the lead-in, so the earlier slots are Mode 0.
  EXPECT_EQ(layout.Slot(0).image_index, 3U);
  EXPECT_EQ(layout.Slot(0).address, 153U);
}

TEST(EfmDataLayoutTest, WithoutPictureNumbersTheFirstProgrammeFrameIsPicture1) {
  const Section lead_in = MakeSection("LeadIn", SectionType::kLeadIn, true);
  const Section programme =
      MakeSection("Stills", SectionType::kProgrammeArea, true);
  std::vector<EfmDataFrame> frames;
  AddFrames(&lead_in, 5, 0, &frames);
  AddFrames(&programme, 5, 0, &frames);

  const EfmDataLayout layout = Build(Standard::kPal, frames, 1000);
  EXPECT_FALSE(layout.anchor_from_picture_number);
  EXPECT_EQ(layout.anchor_frame, 5U);
  EXPECT_EQ(layout.Slot(15).image_index, 153U);
}

TEST(EfmDataLayoutTest, FramesWithoutEfmLeaveTheirSectorsOut) {
  const Section data = MakeSection("Data", SectionType::kProgrammeArea, true);
  const Section video =
      MakeSection("Video", SectionType::kProgrammeArea, false);
  const Section more = MakeSection("More", SectionType::kProgrammeArea, true);
  std::vector<EfmDataFrame> frames;
  AddFrames(&data, 20, 1, &frames);
  AddFrames(&video, 5, 21, &frames);
  AddFrames(&more, 15, 26, &frames);

  const EfmDataLayout layout = Build(Standard::kPal, frames, 400);
  EXPECT_FALSE(layout.frame_carries_efm[20]);
  // Pictures 21-25 would carry addresses 213-227.
  for (std::size_t section = 60; section < 75; ++section) {
    EXPECT_EQ(layout.Slot(section).kind, EfmDataSlotKind::kSilent);
  }
  // Image 0-152 precede picture 1, 213-227 fall on the video, and 273-399
  // follow the last picture.
  EXPECT_EQ(layout.omitted_image_sectors, 153U + 15U + 127U);
  EXPECT_EQ(layout.placed_image_sectors, 400U - layout.omitted_image_sectors);
  EXPECT_EQ(layout.first_omitted_address, 0);

  // Sections 58 and 59 end within 98 + 111 F1 frames of the gap at F1 frame
  // 20 x 294 = 5880; section 57 (5586 + 209 = 5795) still decodes.
  EXPECT_EQ(layout.unrecoverable_image_sectors, 2U);
  EXPECT_EQ(layout.first_unrecoverable_address, 58 + 153);
  EXPECT_EQ(layout.picture_mismatch_frames, 0U);
}

TEST(EfmDataLayoutTest, PictureNumbersBreakingTheCountAreReported) {
  const Section programme =
      MakeSection("Stills", SectionType::kProgrammeArea, true);
  std::vector<EfmDataFrame> frames;
  AddFrames(&programme, 10, 1, &frames);
  AddFrames(&programme, 10, 100, &frames);

  const EfmDataLayout layout = Build(Standard::kPal, frames, 100);
  EXPECT_EQ(layout.picture_mismatch_frames, 10U);
  EXPECT_EQ(layout.first_picture_mismatch_frame, 10);
  // Addresses keep following the subcode timeline.
  EXPECT_EQ(layout.Slot(30).address, 183U);
}

TEST(EfmDataLayoutTest, NtscSectionsFollowTheSampleGrid) {
  const Section programme =
      MakeSection("Stills", SectionType::kProgrammeArea, true);
  std::vector<EfmDataFrame> frames;
  AddFrames(&programme, 100, 1, &frames);

  const EfmDataLayout layout = Build(Standard::kNtsc, frames, 1000);
  // 100 NTSC frames carry 147 147 samples, i.e. 24 524 F1 frames.
  EXPECT_EQ(layout.frame_f1_start.back(), 147147U / 6U);
  EXPECT_EQ(layout.section_count(), (147147U / 6U) / 98U);
  // Picture 1 starts at floor(75 x 1001 / 30000) + 150 = 152.
  EXPECT_EQ(layout.Slot(0).address, 152U);
}

TEST(EfmDataLayoutTest, RejectsSecamAndEmptyFrameLists) {
  const Section programme =
      MakeSection("Stills", SectionType::kProgrammeArea, true);
  std::vector<EfmDataFrame> frames;
  AddFrames(&programme, 5, 1, &frames);
  EfmDataLayout layout;
  std::vector<std::string> errors;
  EXPECT_FALSE(
      BuildEfmDataLayout(Standard::kSecam, frames, {}, &layout, &errors));
  EXPECT_FALSE(BuildEfmDataLayout(Standard::kPal, {}, {}, &layout, &errors));
  EXPECT_EQ(errors.size(), 2U);
}

}  // namespace
}  // namespace videosynth
