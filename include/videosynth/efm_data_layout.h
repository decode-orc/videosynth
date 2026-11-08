/*
 * File:        efm_data_layout.h
 * Module:      efm_data_layout
 * Purpose:     Places the sectors of an EFM data image on the subcode
 *              timeline by the AIV picture rule and decides which frames
 *              carry the EFM signal.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "videosynth/model.h"

namespace videosynth {

// One stored output frame, in output order, as the data layout sees it.
struct EfmDataFrame {
  const Section* section = nullptr;
  // CAV picture number the frame carries, or 0 when it carries none.
  int picture_number = 0;
};

struct EfmDataLayoutSettings {
  // Number of 2 048-byte sectors in the image.
  std::size_t image_sector_count = 0;
  // Sector Address of the image's first sector.
  std::int64_t first_address = 0;
  // Header time minus picture time, in sectors.
  std::int64_t picture_offset_sectors = kEfmDataDefaultPictureOffsetSectors;
};

// What one subcode section of the stream carries.
enum class EfmDataSlotKind {
  // No sector: the section is not wholly inside frames that carry EFM, and
  // whatever part of it is carried holds digital silence.
  kSilent,
  // Image sector `image_index`, with Sector Address `address`.
  kImageSector,
  // A sector before address 0, in the EFM lead-in: Mode 1, zero user data,
  // header holding `address` (the time since the start of the stream) with
  // (A0) added to MIN (ECMA-130 14.2).
  kLeadInFill,
  // A sector outside the image from address 0 on, in the programme area,
  // even when its frame belongs to a lead_in section: Mode 0 (ECMA-130 14.1)
  // with Sector Address `address`.
  kMode0Fill,
};

struct EfmDataSlot {
  EfmDataSlotKind kind = EfmDataSlotKind::kSilent;
  std::size_t image_index = 0;
  std::size_t address = 0;
};

// Placement of an image on the stream. Header addresses follow the subcode
// timeline continuously: the sector in subcode section s has Sector Address
// s + address_offset, so address minus Q time is constant, as a decoder's
// sector placement requires. The offset is fixed by the anchor frame: the
// sectors of the anchor picture p0 start at header time p0 / frame rate +
// picture_offset_sectors (PAL: picture p carries addresses 3p + offset to 3p +
// offset + 2).
struct EfmDataLayout {
  // F1-frame index at which each output frame starts; one more entry than
  // there are frames, the last being the stream length. Frame k spans
  // floor(samples before k / 6) F1 frames, the 44.1 kHz sampling grid of the
  // channel (IEC 60856/60857:1986 Amd 2, 13.2).
  std::vector<std::size_t> frame_f1_start;
  // True for frames whose section declares laserdisc.efm_data.
  std::vector<bool> frame_carries_efm;
  // Per subcode section: true when all of its 98 F1 frames lie in frames that
  // carry EFM.
  std::vector<bool> section_carried;

  std::int64_t address_offset = 0;
  std::int64_t first_address = 0;
  std::size_t image_sector_count = 0;

  // The anchor of the placement rule: the first frame carrying a picture
  // number, or (with none) the first programme-area frame taken as picture 1.
  std::size_t anchor_frame = 0;
  int anchor_picture = 1;
  bool anchor_from_picture_number = false;

  // Image sectors placed in carried sections, and those left out.
  std::size_t placed_image_sectors = 0;
  std::size_t omitted_image_sectors = 0;
  // Sector Address of the first image sector left out; -1 when none is.
  std::int64_t first_omitted_address = -1;
  // Image indices of the sectors left out, in order.
  std::vector<std::size_t> omitted_image_indices;
  // Placed image sectors a decoder cannot recover because a frame without
  // EFM follows within the CIRC decode window (98 + kCircDrainFrames F1
  // frames), and the address of the first of them (-1 when none).
  std::size_t unrecoverable_image_sectors = 0;
  std::int64_t first_unrecoverable_address = -1;
  // Frames whose picture number disagrees with the anchor's continuous count,
  // and the first such frame (-1 when none).
  std::size_t picture_mismatch_frames = 0;
  std::int64_t first_picture_mismatch_frame = -1;

  std::size_t section_count() const { return section_carried.size(); }

  // The subcode section that carries Sector Address 0 (negative when it lies
  // before the stream). The EFM programme area starts there, so its Q
  // absolute time equals the Sector Address, as on the AIV discs.
  std::int64_t zero_address_section() const { return -address_offset; }

  // What subcode section `section` carries.
  EfmDataSlot Slot(std::size_t section) const;
};

// Builds the layout for `frames` on `standard` (PAL, NTSC or PAL-M). Returns
// false and appends a message to errors when the standard has no EFM channel
// or there are no frames.
//
// Thread-safety: a pure function of its arguments.
bool BuildEfmDataLayout(Standard standard,
                        const std::vector<EfmDataFrame>& frames,
                        const EfmDataLayoutSettings& settings,
                        EfmDataLayout* layout,
                        std::vector<std::string>* errors);

}  // namespace videosynth
