/*
 * File:        subcode_generator.h
 * Module:      efm
 * Purpose:     Generates the 98-frame P/Q subcode sections of the LaserDisc
 *              digital audio channel, using DATA-Q mode 4.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace videosynth::efm {

// IEC 60908-1999, 17.3: one subcoding block consists of 98 subcoding symbols
// and the repetition frequency of one block is 75 Hz.
inline constexpr std::size_t kFramesPerSubcodeSection = 98;
inline constexpr std::size_t kSubcodeSectionsPerSecond = 75;

// IEC 60908-1999, 17.3: the first two subcoding symbols are replaced by the
// sync patterns S0 and S1, so 96 frames per section carry channel bits.
inline constexpr std::size_t kSubcodeSyncFrames = 2;
inline constexpr std::size_t kSubcodeChannelBits =
    kFramesPerSubcodeSection - kSubcodeSyncFrames;

// ECMA-130, 19.1: S0 and S1 are 14-bit patterns that lie outside the
// eight-to-fourteen table, so they cannot be confused with a control byte.
inline constexpr std::size_t kSubcodeSyncPatternBits = 14;
inline constexpr std::uint16_t kSubcodeSyncS0 = 0b0010'0000'0000'01;
inline constexpr std::uint16_t kSubcodeSyncS1 = 0b0000'0000'0100'10;

// IEC 60908-1999, 17.1 / 17.2: the eight bits of a control symbol are the
// channels P-Q-R-S-T-U-V-W, P being the most significant bit.
inline constexpr unsigned kSubcodeChannelPShift = 7;
inline constexpr unsigned kSubcodeChannelQShift = 6;

// IEC 60908-1999, 17.5: the CRC covers CONTROL (4 bits), ADR (4 bits) and
// DATA-Q (72 bits), i.e. the first ten bytes of the 96-bit Q block.
inline constexpr std::size_t kQChannelPayloadBytes = 10;
using QChannelPayload = std::array<std::uint8_t, kQChannelPayloadBytes>;

// IEC 60856:1986 Amd 2, 13.5.1.1 / IEC 60857:1986 Amd 2, 13.6.1.1: LaserDisc
// digital audio replaces "0001: ADR 1, mode 1 for DATA-Q" with "0100: ADR 4,
// mode 4 for DATA-Q". The DATA-Q layout is otherwise IEC 60908-1999, 17.5.1.
inline constexpr std::uint8_t kAdrMode4 = 0x4;

// IEC 60908-1999, 17.5: control 0 0 X 0 is two audio channels without
// pre-emphasis and 0 X 0 X is copy prohibited.
inline constexpr std::uint8_t kControlAudioNoPreEmphasisCopyProhibited = 0x0;

// IEC 60908-1999, 17.5 / ECMA-130, 22.3.2: control 0 1 0 0 is a data track,
// copy prohibited, and 0 1 1 0 a data track, copy permitted.
inline constexpr std::uint8_t kControlDataCopyProhibited = 0x4;
inline constexpr std::uint8_t kControlDataCopyPermitted = 0x6;

// IEC 60908-1999, 17.5: "0001: ADR 1, mode 1 for DATA-Q", the compact-disc
// mode that LaserDisc digital audio replaces with mode 4. Discs mastered
// before IEC 60856:1986 Amd 2 / IEC 60857:1986 Amd 2, such as the Acorn
// Interactive Video (AIV) discs, record mode 1.
inline constexpr std::uint8_t kAdrMode1 = 0x1;

// Largest value of the four-bit CONTROL and ADR fields.
inline constexpr std::uint8_t kMaxQNibble = 0xF;

// IEC 60908-1999, 17.5.1: TNO 00 marks the lead-in track and the hexadecimal
// code AA the lead-out track.
inline constexpr std::uint8_t kTrackNumberLeadIn = 0x00;
inline constexpr std::uint8_t kTrackNumberLeadOut = 0xAA;

// IEC 60908-1999, 17.5.1: index X = 00 is pause encoding; within an audio track
// and during the lead-out track X is 01.
inline constexpr std::uint8_t kIndexPause = 0x00;
inline constexpr std::uint8_t kIndexAudio = 0x01;

// IEC 60908-1999, 17.5.1: POINT values A0, A1 and A2 of the table of contents
// give the first track, the last track and the start of the lead-out track.
inline constexpr std::uint8_t kTocPointFirstTrack = 0xA0;
inline constexpr std::uint8_t kTocPointLastTrack = 0xA1;
inline constexpr std::uint8_t kTocPointLeadOutStart = 0xA2;

// IEC 60908-1999, 17.5.1: in each table of contents the items are repeated
// three times.
inline constexpr std::size_t kTocItemRepeats = 3;

// IEC 60908-1999, 17.4 / 17.5.1: the first audio track is preceded by a pause
// and a start flag of 2 s to 3 s; the lead-out track is likewise preceded by a
// start flag of 2 s to 3 s. The minimum of the range is used throughout.
inline constexpr std::size_t kTrackOnePauseSections =
    2 * kSubcodeSectionsPerSecond;
inline constexpr std::size_t kStartFlagSections = 2 * kSubcodeSectionsPerSecond;

// IEC 60908-1999, 17.4: channel P stays zero for 2 s to 3 s after the start of
// the lead-out track before it starts switching.
inline constexpr std::size_t kLeadOutPSilentSections =
    2 * kSubcodeSectionsPerSecond;

// IEC 60908-1999, 17.4: after that, P switches between 0 and 1 in a 2 Hz +/- 2
// % rhythm at a duty cycle of 50 % +/- 10 %. A half period of 19 sections gives
// 75 / 38 = 1,974 Hz (-1,3 %) at an exact 50 % duty cycle; 75 Hz sections
// cannot express 2 Hz exactly.
inline constexpr std::size_t kLeadOutPHalfPeriodSections = 19;

// IEC 60856:1986 Amd 2, 13.5.3.3 / IEC 60857:1986 Amd 2, 13.6.3.3: maximum
// track number of a CD in an LV disc is 79.
inline constexpr std::uint8_t kMaxTrackNumber = 79;

// Video system carried in the P FRAME field of the POINT = A0 table-of-contents
// entry (IEC 60856:1986 Amd 2, 13.5.2 / IEC 60857:1986 Amd 2, 13.6.2).
enum class VideoSystem : std::uint8_t {
  kPal,
  kNtsc,
};

// IEC 60856:1986 Amd 2, 13.5.2: P frame is 22 = PAL "LV disk" with digital
// stereo sound. IEC 60857:1986 Amd 2, 13.6.2: P frame is 12 = NTSC "LV disk"
// with digital stereo sound.
inline constexpr std::uint8_t kVideoSystemIdentificationPal = 22;
inline constexpr std::uint8_t kVideoSystemIdentificationNtsc = 12;

// Disc area an entry of the track table describes (IEC 60908-1999, clause 12).
enum class SubcodeArea : std::uint8_t {
  kLeadIn,
  kProgramme,
  kLeadOut,
};

// A time of the subcode timeline in decimal digits: one second is subdivided
// into 75 frames running from 00 to 74 (IEC 60908-1999, 17.5.1).
struct SubcodeTime {
  std::uint8_t minutes = 0;
  std::uint8_t seconds = 0;
  std::uint8_t frames = 0;

  friend bool operator==(const SubcodeTime& left, const SubcodeTime& right) {
    return left.minutes == right.minutes && left.seconds == right.seconds &&
           left.frames == right.frames;
  }
  friend bool operator!=(const SubcodeTime& left, const SubcodeTime& right) {
    return !(left == right);
  }
};

// Converts a count of subcode sections into MIN/SEC/FRAME. Minutes saturate at
// 99 because the field holds two BCD digits.
SubcodeTime SubcodeTimeFromSections(std::size_t sections);

// Converts MIN/SEC/FRAME back into a count of subcode sections.
std::size_t SectionsFromSubcodeTime(const SubcodeTime& time);

// One contiguous region of the subcode timeline. The entries of a track table
// tile the whole stream in output order.
//
// `track_number` is the caller-assigned TNO of a programme track (1 to 79); it
// is ignored for lead-in and lead-out entries, which use the fixed TNO values
// of IEC 60908-1999, 17.5.1. The generator makes no numbering decisions of its
// own.
struct TrackTableEntry {
  SubcodeArea area = SubcodeArea::kProgramme;
  std::uint8_t track_number = 0;
  std::size_t start_section = 0;
  std::size_t section_count = 0;
};

// The complete subcode layout of one stream.
struct TrackTable {
  std::vector<TrackTableEntry> entries;
  VideoSystem video_system = VideoSystem::kPal;
  // CONTROL nibble of every Q block (IEC 60908-1999, 17.5). The default is two
  // audio channels without pre-emphasis, copy prohibited.
  std::uint8_t control = kControlAudioNoPreEmphasisCopyProhibited;
  // ADR nibble of every Q block: mode 4 for LaserDisc (IEC 60856:1986 Amd 2,
  // 13.5.1.1 / IEC 60857:1986 Amd 2, 13.6.1.1). The DATA-Q layout does not
  // depend on it.
  std::uint8_t adr = kAdrMode4;
};

// One item of the repetitive lead-in table of contents (IEC 60908-1999,
// 17.5.1). The MIN/SEC/FRAME running time of the lead-in is not part of an
// item: it counts through the lead-in independently of the cycle.
struct TocItem {
  std::uint8_t point = 0;
  SubcodeTime point_time{};
};

// One generated subcode section: 98 frames of which the first two carry the
// out-of-table sync patterns S0 and S1 (IEC 60908-1999, 17.3) and the remaining
// 96 carry one bit of each channel. Channels R to W inclusive are all zero
// (IEC 60908-1999, 17.6).
struct SubcodeSection {
  std::array<bool, kSubcodeChannelBits> p_channel{};
  std::array<bool, kSubcodeChannelBits> q_channel{};

  // The control symbol of frame `frame_index` (0 to 97). Frames 0 and 1 carry
  // S0/S1 instead of a control symbol and return zero.
  std::uint8_t ControlByte(std::size_t frame_index) const;

  // The ten bytes CONTROL|ADR and DATA-Q that the Q CRC covers.
  QChannelPayload QPayload() const;

  // The 16 CRC bits of the Q block as recorded, i.e. with the parity bits
  // inverted (IEC 60908-1999, 17.5).
  std::uint16_t QCrc() const;
};

// The CRC-16 on CONTROL, ADR and DATA-Q with P(X) = X^16 + X^12 + X^5 + 1, MSB
// first (IEC 60908-1999, 17.5). The returned value is the recorded one, i.e.
// with the parity bits already inverted.
std::uint16_t ComputeQChannelCrc(const QChannelPayload& payload);

// Generates the P and Q subcode sections of one EFM stream from a track table.
//
// Every generated section is a pure function of the track table and the section
// index, so repeated runs produce identical subcode.
//
// Error reporting: the module reports failure by return value and never throws
// across its public API.
//
// Thread-safety: SubcodeGenerator is NOT thread-safe for concurrent Begin or
// Reset calls. After a successful Begin the generator is immutable, so
// GenerateSection may be called concurrently from several threads.
class SubcodeGenerator {
 public:
  // Adopts `table` and precomputes the table of contents. Returns false, and
  // leaves the generator unchanged, when the table is not a gapless ascending
  // tiling of lead-in, then programme, then lead-out entries, or when a
  // programme track number is outside 1 to kMaxTrackNumber, or the control or
  // ADR value does not fit its four-bit field.
  bool Begin(const TrackTable& table);

  // Writes the subcode section at absolute index `section_index` to `section`.
  // Returns false when `section` is null, no table has been accepted, or the
  // index lies beyond the end of the table.
  bool GenerateSection(std::size_t section_index,
                       SubcodeSection* section) const;

  // Total number of sections the accepted track table covers.
  std::size_t SectionCount() const { return section_count_; }

  // The table-of-contents items cycled through the lead-in area, in order and
  // before the three-fold repetition of IEC 60908-1999, 17.5.1.
  const std::vector<TocItem>& TocItems() const { return toc_items_; }

  // Restores the generator to its constructed state.
  void Reset();

 private:
  // Value of channel P for `section_index` before the one-section delay of
  // IEC 60908-1999, 17.4 is applied.
  bool UndelayedPFlag(std::size_t section_index) const;

  // DATA-Q payload for `section_index`, chosen by the area of the entry that
  // contains it.
  QChannelPayload BuildPayload(std::size_t section_index) const;
  QChannelPayload BuildLeadInPayload(const TrackTableEntry& entry,
                                     std::size_t section_index) const;
  QChannelPayload BuildProgrammePayload(const TrackTableEntry& entry,
                                        std::size_t section_index) const;
  QChannelPayload BuildLeadOutPayload(const TrackTableEntry& entry,
                                      std::size_t section_index) const;

  const TrackTableEntry* EntryForSection(std::size_t section_index) const;

  // The CONTROL|ADR byte opening every Q block.
  QChannelPayload MakePayloadSkeleton() const;

  std::vector<TrackTableEntry> entries_;
  std::vector<TocItem> toc_items_;
  std::uint8_t control_adr_ = static_cast<std::uint8_t>(
      kControlAudioNoPreEmphasisCopyProhibited << 4U | kAdrMode4);
  // Sections at which a start flag ends, i.e. the first audio section of every
  // track and the first section of the lead-out (IEC 60908-1999, 17.4).
  std::vector<std::size_t> start_flag_targets_;
  std::size_t section_count_ = 0;
  std::size_t programme_start_section_ = 0;
  std::size_t lead_out_start_section_ = 0;
  bool has_programme_area_ = false;
  bool has_lead_out_area_ = false;
};

}  // namespace videosynth::efm
