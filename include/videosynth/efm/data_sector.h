/*
 * File:        data_sector.h
 * Module:      efm
 * Purpose:     Builds ECMA-130 data sectors (sync, header, EDC, RSPC parity,
 *              scrambling) and maps them onto the F1 frames of one subcode
 *              section.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "videosynth/efm/audio_frame_assembler.h"
#include "videosynth/efm/subcode_generator.h"

namespace videosynth::efm {

// ECMA-130 clause 14: a Sector has 2 352 bytes, of which 2 048 are user data
// in Mode 1.
inline constexpr std::size_t kSectorBytes = 2352;
inline constexpr std::size_t kMode1UserDataBytes = 2048;

// ECMA-130 clause 14 / figure 9: byte offsets of the Mode 1 fields.
inline constexpr std::size_t kSectorSyncBytes = 12;
inline constexpr std::size_t kSectorHeaderOffset = 12;
inline constexpr std::size_t kSectorModeOffset = 15;
inline constexpr std::size_t kSectorUserDataOffset = 16;
inline constexpr std::size_t kSectorEdcOffset = 2064;
inline constexpr std::size_t kSectorZeroOffset = 2068;
inline constexpr std::size_t kSectorZeroBytes = 8;
inline constexpr std::size_t kSectorPParityOffset = 2076;
inline constexpr std::size_t kSectorPParityBytes = 172;
inline constexpr std::size_t kSectorQParityOffset = 2248;
inline constexpr std::size_t kSectorQParityBytes = 104;

// ECMA-130 14.2: the mode byte of the header.
inline constexpr std::uint8_t kSectorMode0 = 0x00;
inline constexpr std::uint8_t kSectorMode1 = 0x01;

// ECMA-130 14.2: in a Digital Data Track of the Lead-in Area, (A0) is added to
// the MIN field of the Sector Address.
inline constexpr std::uint8_t kLeadInSectorMinuteOffset = 0xA0;

// ECMA-130 clause 16: one Sector is carried by 98 consecutive F1 frames, i.e.
// exactly one subcode section (IEC 60908-1999, 17.3).
static_assert(kSectorBytes == kFramesPerSubcodeSection * kF1FrameBytes);

// The highest Sector Address the BCD header can hold, 99:59:74.
inline constexpr std::size_t kMaxSectorAddress = (100 * 60 * 75) - 1;

using Sector = std::array<std::uint8_t, kSectorBytes>;

// ECMA-130 Annex B: the byte the scrambler adds (modulo 2) to sector byte
// `index`. Zero for the 12 sync bytes, which are not scrambled.
std::uint8_t ScrambleByte(std::size_t index);

// ECMA-130 14.3: the EDC, a CRC-32 with generator x^32 + x^31 + x^16 + x^15 +
// x^4 + x^3 + x + 1, least significant bit first, over the `count` bytes at
// `bytes`.
std::uint32_t ComputeEdc(const std::uint8_t* bytes, std::size_t count);

// ECMA-130 Annex A: writes the P parity (86 (26,24) codewords) and then the
// Q parity (52 (45,43) codewords) of `sector`, over bytes 12 onwards. The
// header, user data, EDC and zero field must already be in place.
void WriteRspcParity(Sector* sector);

// ECMA-130 Annex B / clause 15: adds the scramble sequence to bytes 12 to
// 2 351. Scrambling is its own inverse.
void ScrambleSector(Sector* sector);

// An unscrambled ECMA-130 Mode 1 Sector with Sector Address `address`
// (0 to kMaxSectorAddress, expressed as MIN/SEC/FRAC in BCD) and `user_data`.
// An address beyond kMaxSectorAddress saturates.
Sector BuildMode1Sector(
    std::size_t address,
    const std::array<std::uint8_t, kMode1UserDataBytes>& user_data);

// An unscrambled ECMA-130 Mode 1 Sector of the Lead-in Area: zero user data,
// the header holding `relative_address` (the time since the start of the
// Lead-in Area) with (A0) added to its MIN field (ECMA-130 14.2).
Sector BuildLeadInMode1Sector(std::size_t relative_address);

// An unscrambled ECMA-130 Mode 0 Sector (14.1: bytes 16 to 2 351 all zero).
Sector BuildMode0Sector(std::size_t address);

// Maps a scrambled Sector onto 98 F1 frames (ECMA-130 clause 16). The sector
// bytes are consecutive and form little-endian 16-bit words, so F1 byte 2w
// (WmA, the higher byte of word w in IEC 60908-1999, 16.2) holds sector byte
// 2w + 1 and F1 byte 2w + 1 (WmB) holds sector byte 2w.
std::array<F1Frame, kFramesPerSubcodeSection> SectorToF1Frames(
    const Sector& scrambled_sector);

}  // namespace videosynth::efm
