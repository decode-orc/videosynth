/*
 * File:        subcode_generator.cpp
 * Module:      efm
 * Purpose:     Generates the 98-frame P/Q subcode sections of the LaserDisc
 *              digital audio channel, using DATA-Q mode 4.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "videosynth/efm/subcode_generator.h"

#include <algorithm>

namespace videosynth::efm {
namespace {

// IEC 60908-1999, 17.5.1: MIN, SEC and FRAME are two BCD digits each, so the
// minutes field saturates at 99.
constexpr std::uint8_t kMaxSubcodeMinutes = 99;
constexpr std::size_t kSecondsPerMinute = 60;
constexpr std::size_t kSectionsPerMinute =
    kSubcodeSectionsPerSecond * kSecondsPerMinute;

// Byte positions within the ten-byte CONTROL/ADR + DATA-Q payload of
// IEC 60908-1999, 17.5.1. The lead-in layout differs from the audio and
// lead-out layout only in the second and last three fields.
constexpr std::size_t kPayloadControlAdr = 0;
constexpr std::size_t kPayloadTrackNumber = 1;
constexpr std::size_t kPayloadIndexOrPoint = 2;
// MIN/SEC/FRAME and PMIN/PSEC/PFRAME (AMIN/ASEC/AFRAME in the audio layout)
// occupy three consecutive bytes each; the ZERO field between them is zero.
constexpr std::size_t kPayloadTimeStart = 3;
constexpr std::size_t kPayloadPointTimeStart = 7;

// IEC 60908-1999, 17.5: the CRC polynomial is P(X) = X^16 + X^12 + X^5 + 1 and
// the bits are processed MSB first. On the disc the parity bits are inverted.
constexpr std::uint16_t kQCrcPolynomial = 0x1021U;
constexpr std::uint16_t kQCrcInversionMask = 0xFFFFU;

constexpr std::uint8_t ToBcd(std::uint8_t value) {
  return static_cast<std::uint8_t>(((value / 10U) << 4U) | (value % 10U));
}

constexpr std::uint8_t VideoSystemIdentification(VideoSystem system) {
  return system == VideoSystem::kPal ? kVideoSystemIdentificationPal
                                     : kVideoSystemIdentificationNtsc;
}

// Writes MIN/SEC/FRAME as BCD starting at `first_byte`.
void WriteTime(QChannelPayload* payload, std::size_t first_byte,
               const SubcodeTime& time) {
  (*payload)[first_byte] = ToBcd(time.minutes);
  (*payload)[first_byte + 1] = ToBcd(time.seconds);
  (*payload)[first_byte + 2] = ToBcd(time.frames);
}

}  // namespace

SubcodeTime SubcodeTimeFromSections(std::size_t sections) {
  SubcodeTime time{};
  const std::size_t minutes = sections / kSectionsPerMinute;
  time.minutes = static_cast<std::uint8_t>(
      std::min<std::size_t>(minutes, kMaxSubcodeMinutes));
  const std::size_t remainder = sections % kSectionsPerMinute;
  time.seconds =
      static_cast<std::uint8_t>(remainder / kSubcodeSectionsPerSecond);
  time.frames =
      static_cast<std::uint8_t>(remainder % kSubcodeSectionsPerSecond);
  return time;
}

std::size_t SectionsFromSubcodeTime(const SubcodeTime& time) {
  return (static_cast<std::size_t>(time.minutes) * kSectionsPerMinute) +
         (static_cast<std::size_t>(time.seconds) * kSubcodeSectionsPerSecond) +
         static_cast<std::size_t>(time.frames);
}

std::uint8_t SubcodeSection::ControlByte(std::size_t frame_index) const {
  // IEC 60908-1999, 17.3: channels P to W cannot be encoded during the S0/S1
  // sync interval, so the first two frames carry no control symbol.
  if (frame_index < kSubcodeSyncFrames ||
      frame_index >= kFramesPerSubcodeSection) {
    return 0;
  }
  const std::size_t bit = frame_index - kSubcodeSyncFrames;
  std::uint8_t control = 0;
  if (p_channel[bit]) {
    control |= static_cast<std::uint8_t>(1U << kSubcodeChannelPShift);
  }
  if (q_channel[bit]) {
    control |= static_cast<std::uint8_t>(1U << kSubcodeChannelQShift);
  }
  // IEC 60908-1999, 17.6: channels R to W inclusive are all zero.
  return control;
}

QChannelPayload SubcodeSection::QPayload() const {
  QChannelPayload payload{};
  for (std::size_t index = 0; index < kQChannelPayloadBytes * 8; ++index) {
    if (q_channel[index]) {
      payload[index / 8] |= static_cast<std::uint8_t>(1U << (7U - index % 8));
    }
  }
  return payload;
}

std::uint16_t SubcodeSection::QCrc() const {
  std::uint16_t crc = 0;
  for (std::size_t index = kQChannelPayloadBytes * 8;
       index < kSubcodeChannelBits; ++index) {
    crc = static_cast<std::uint16_t>(crc << 1U);
    if (q_channel[index]) {
      crc |= 1U;
    }
  }
  return crc;
}

std::uint16_t ComputeQChannelCrc(const QChannelPayload& payload) {
  std::uint16_t remainder = 0;
  for (const std::uint8_t byte : payload) {
    for (unsigned bit = 0; bit < 8; ++bit) {
      const bool high = (remainder & 0x8000U) != 0U;
      const bool input = ((byte >> (7U - bit)) & 1U) != 0U;
      remainder = static_cast<std::uint16_t>(remainder << 1U);
      if (high != input) {
        remainder ^= kQCrcPolynomial;
      }
    }
  }
  // IEC 60908-1999, 17.5: on the disc the parity bits are inverted.
  return static_cast<std::uint16_t>(remainder ^ kQCrcInversionMask);
}

// Common skeleton of every layout: the control nibble and ADR of the track
// table. All other bytes, including the eight bits of the ZERO field, start at
// zero (IEC 60908-1999, 17.5.1).
QChannelPayload SubcodeGenerator::MakePayloadSkeleton() const {
  QChannelPayload payload{};
  payload[kPayloadControlAdr] = control_adr_;
  return payload;
}

bool SubcodeGenerator::Begin(const TrackTable& table) {
  if (table.entries.empty() || table.control > kMaxQNibble ||
      table.adr > kMaxQNibble) {
    return false;
  }

  std::vector<std::size_t> start_flag_targets;
  std::vector<TocItem> toc_items;
  std::size_t next_start = 0;
  std::size_t previous_area = 0;
  std::size_t programme_start = 0;
  std::size_t lead_out_start = 0;
  bool has_programme = false;
  bool has_lead_out = false;
  bool has_lead_in = false;

  for (const TrackTableEntry& entry : table.entries) {
    if (entry.section_count == 0 || entry.start_section != next_start) {
      return false;
    }
    const auto area = static_cast<std::size_t>(entry.area);
    if (area < previous_area) {
      return false;
    }
    previous_area = area;
    next_start += entry.section_count;

    switch (entry.area) {
      case SubcodeArea::kLeadIn:
        has_lead_in = true;
        break;
      case SubcodeArea::kProgramme: {
        if (entry.track_number == 0 || entry.track_number > kMaxTrackNumber) {
          return false;
        }
        if (!has_programme) {
          has_programme = true;
          programme_start = entry.start_section;
        }
        // IEC 60908-1999, 17.5.1: the first audio track is preceded by a pause
        // of 2 s to 3 s; later tracks have no pause encoding, so their audio
        // starts with the entry.
        const std::size_t audio_start =
            entry.start_section == programme_start
                ? entry.start_section +
                      std::min(kTrackOnePauseSections, entry.section_count)
                : entry.start_section;
        start_flag_targets.push_back(audio_start);
        // The start position of a track on the absolute time scale is its
        // first position with the new track number and X != 00.
        toc_items.push_back(
            TocItem{ToBcd(entry.track_number),
                    SubcodeTimeFromSections(audio_start - programme_start)});
        break;
      }
      case SubcodeArea::kLeadOut:
        if (!has_lead_out) {
          has_lead_out = true;
          lead_out_start = entry.start_section;
          // IEC 60908-1999, 17.4: the lead-out track is preceded by a start
          // flag of 2 s to 3 s during the last audio track.
          start_flag_targets.push_back(lead_out_start);
        }
        break;
    }
  }

  if (has_lead_in && has_programme) {
    // IEC 60908-1999, 17.5.1: POINT A0 gives the first track, A1 the last track
    // and A2 the starting point of the lead-out track.
    std::uint8_t first_tno = 0;
    std::uint8_t last_tno = 0;
    for (const TrackTableEntry& entry : table.entries) {
      if (entry.area != SubcodeArea::kProgramme) {
        continue;
      }
      if (first_tno == 0) {
        first_tno = entry.track_number;
      }
      last_tno = entry.track_number;
    }
    // IEC 60856:1986 Amd 2, 13.5.2 / IEC 60857:1986 Amd 2, 13.6.2: the video
    // system identification code is carried in the P FRAME field of the A0
    // entry, which IEC 60908-1999, 17.5.1 otherwise leaves zero.
    toc_items.push_back(
        TocItem{kTocPointFirstTrack,
                SubcodeTime{first_tno, 0,
                            VideoSystemIdentification(table.video_system)}});
    toc_items.push_back(
        TocItem{kTocPointLastTrack, SubcodeTime{last_tno, 0, 0}});
    const std::size_t lead_out_offset = has_lead_out
                                            ? lead_out_start - programme_start
                                            : next_start - programme_start;
    toc_items.push_back(TocItem{kTocPointLeadOutStart,
                                SubcodeTimeFromSections(lead_out_offset)});
  } else {
    toc_items.clear();
  }

  entries_ = table.entries;
  control_adr_ = static_cast<std::uint8_t>(table.control << 4U | table.adr);
  toc_items_ = std::move(toc_items);
  start_flag_targets_ = std::move(start_flag_targets);
  section_count_ = next_start;
  programme_start_section_ = programme_start;
  lead_out_start_section_ = lead_out_start;
  has_programme_area_ = has_programme;
  has_lead_out_area_ = has_lead_out;
  return true;
}

void SubcodeGenerator::Reset() {
  entries_.clear();
  control_adr_ = static_cast<std::uint8_t>(
      kControlAudioNoPreEmphasisCopyProhibited << 4U | kAdrMode4);
  toc_items_.clear();
  start_flag_targets_.clear();
  section_count_ = 0;
  programme_start_section_ = 0;
  lead_out_start_section_ = 0;
  has_programme_area_ = false;
  has_lead_out_area_ = false;
}

const TrackTableEntry* SubcodeGenerator::EntryForSection(
    std::size_t section_index) const {
  for (const TrackTableEntry& entry : entries_) {
    if (section_index < entry.start_section + entry.section_count) {
      return &entry;
    }
  }
  return nullptr;
}

bool SubcodeGenerator::UndelayedPFlag(std::size_t section_index) const {
  // IEC 60908-1999, 17.4: in the lead-in track channel P is encoded as for
  // audio, that is P = 0.
  if (!has_programme_area_ || section_index < programme_start_section_) {
    return false;
  }

  if (has_lead_out_area_ && section_index >= lead_out_start_section_) {
    const std::size_t offset = section_index - lead_out_start_section_;
    if (offset < kLeadOutPSilentSections) {
      return false;
    }
    const std::size_t half_periods =
        (offset - kLeadOutPSilentSections) / kLeadOutPHalfPeriodSections;
    return half_periods % 2 == 0;
  }

  // The start flag occupies the 2 s preceding the start of the next track; its
  // end indicates that start.
  return std::any_of(start_flag_targets_.begin(), start_flag_targets_.end(),
                     [section_index](std::size_t target) {
                       return section_index < target &&
                              section_index + kStartFlagSections >= target;
                     });
}

QChannelPayload SubcodeGenerator::BuildLeadInPayload(
    const TrackTableEntry& entry, std::size_t section_index) const {
  QChannelPayload payload = MakePayloadSkeleton();
  // IEC 60908-1999, 17.5.1: during the lead-in track TNO is 00 and the running
  // time increases; the index X is not encoded, POINT takes its place.
  payload[kPayloadTrackNumber] = kTrackNumberLeadIn;
  WriteTime(&payload, kPayloadTimeStart,
            SubcodeTimeFromSections(section_index - entry.start_section));

  if (toc_items_.empty()) {
    return payload;
  }
  // Each item of the table of contents is repeated three times and the whole
  // table is continuously repeated through the lead-in; at its end the table
  // may be ended with any value of POINT.
  const std::size_t cycle_position =
      (section_index - entry.start_section) / kTocItemRepeats;
  const TocItem& item = toc_items_[cycle_position % toc_items_.size()];
  payload[kPayloadIndexOrPoint] = item.point;
  WriteTime(&payload, kPayloadPointTimeStart, item.point_time);
  return payload;
}

QChannelPayload SubcodeGenerator::BuildProgrammePayload(
    const TrackTableEntry& entry, std::size_t section_index) const {
  QChannelPayload payload = MakePayloadSkeleton();
  payload[kPayloadTrackNumber] = ToBcd(entry.track_number);

  const bool is_first_track = entry.start_section == programme_start_section_;
  const std::size_t pause_sections =
      is_first_track ? std::min(kTrackOnePauseSections, entry.section_count)
                     : 0;
  const std::size_t audio_start = entry.start_section + pause_sections;

  if (section_index < audio_start) {
    // IEC 60908-1999, 17.5.1: during a pause X is 00 and the running time
    // decreases, ending with the value zero at the end of the pause.
    payload[kPayloadIndexOrPoint] = kIndexPause;
    WriteTime(&payload, kPayloadTimeStart,
              SubcodeTimeFromSections(audio_start - 1 - section_index));
  } else {
    payload[kPayloadIndexOrPoint] = kIndexAudio;
    // The running time is set to zero at the start of a track.
    WriteTime(&payload, kPayloadTimeStart,
              SubcodeTimeFromSections(section_index - audio_start));
  }

  // At the starting diameter of the programme area the running time on the disc
  // is set to zero; it is continuous across track boundaries.
  WriteTime(&payload, kPayloadPointTimeStart,
            SubcodeTimeFromSections(section_index - programme_start_section_));
  return payload;
}

QChannelPayload SubcodeGenerator::BuildLeadOutPayload(
    const TrackTableEntry& entry, std::size_t section_index) const {
  QChannelPayload payload = MakePayloadSkeleton();
  // IEC 60908-1999, 17.5.1: the lead-out track has TNO = AA, is encoded as
  // audio (X = 01) and its running time increases from the lead-out start.
  payload[kPayloadTrackNumber] = kTrackNumberLeadOut;
  payload[kPayloadIndexOrPoint] = kIndexAudio;
  WriteTime(&payload, kPayloadTimeStart,
            SubcodeTimeFromSections(section_index - entry.start_section));
  const std::size_t absolute_start =
      has_programme_area_ ? programme_start_section_ : entry.start_section;
  WriteTime(&payload, kPayloadPointTimeStart,
            SubcodeTimeFromSections(section_index - absolute_start));
  return payload;
}

QChannelPayload SubcodeGenerator::BuildPayload(
    std::size_t section_index) const {
  const TrackTableEntry* entry = EntryForSection(section_index);
  if (entry == nullptr) {
    return MakePayloadSkeleton();
  }
  switch (entry->area) {
    case SubcodeArea::kLeadIn:
      return BuildLeadInPayload(*entry, section_index);
    case SubcodeArea::kProgramme:
      return BuildProgrammePayload(*entry, section_index);
    case SubcodeArea::kLeadOut:
      return BuildLeadOutPayload(*entry, section_index);
  }
  return MakePayloadSkeleton();
}

bool SubcodeGenerator::GenerateSection(std::size_t section_index,
                                       SubcodeSection* section) const {
  if (section == nullptr || section_index >= section_count_) {
    return false;
  }

  const QChannelPayload payload = BuildPayload(section_index);
  const std::uint16_t crc = ComputeQChannelCrc(payload);

  *section = SubcodeSection{};
  for (std::size_t index = 0; index < kQChannelPayloadBytes * 8; ++index) {
    section->q_channel[index] =
        ((payload[index / 8] >> (7U - index % 8)) & 1U) != 0U;
  }
  for (std::size_t index = 0; index < 16; ++index) {
    section->q_channel[kQChannelPayloadBytes * 8 + index] =
        ((crc >> (15U - index)) & 1U) != 0U;
  }

  // IEC 60908-1999, 17.4: a change in channel P may take place only immediately
  // after the sync patterns S0 and S1, and the encoding of channel P is delayed
  // by one subcoding block with respect to the encoding of channel Q.
  const bool p_flag = section_index > 0 && UndelayedPFlag(section_index - 1);
  section->p_channel.fill(p_flag);
  return true;
}

}  // namespace videosynth::efm
