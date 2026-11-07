/*
 * File:        test_efm_data_sector.cpp
 * Module:      efm
 * Purpose:     Unit tests for the ECMA-130 data sector builder: scrambling,
 *              EDC, RSPC parity, header layout and the sector-to-F1 mapping.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>

#include "videosynth/efm/data_sector.h"

namespace videosynth::efm {
namespace {

// GF(2^8) multiplication with P(x) = x^8 + x^4 + x^3 + x^2 + 1 (ECMA-130,
// Annex A), written independently of the encoder's tables.
std::uint8_t GfMultiply(std::uint8_t a, std::uint8_t b) {
  unsigned product = 0;
  unsigned left = a;
  for (unsigned bit = 0; bit < 8U; ++bit) {
    if ((b >> bit) & 1U) {
      product ^= left;
    }
    left <<= 1U;
    if (left & 0x100U) {
      left ^= 0x11DU;
    }
  }
  return static_cast<std::uint8_t>(product);
}

std::uint8_t GfPower(unsigned exponent) {
  std::uint8_t value = 1;
  for (unsigned index = 0; index < exponent; ++index) {
    value = GfMultiply(value, 2);
  }
  return value;
}

// Syndromes of codeword `symbols` (data then two parity bytes) against the
// check matrix [1 ... 1; alpha^(n-1) ... alpha^0] of ECMA-130, Annex A.
std::array<std::uint8_t, 2> Syndromes(
    const std::vector<std::uint8_t>& symbols) {
  std::uint8_t s0 = 0;
  std::uint8_t s1 = 0;
  const std::size_t n = symbols.size();
  for (std::size_t index = 0; index < n; ++index) {
    s0 = static_cast<std::uint8_t>(s0 ^ symbols[index]);
    s1 = static_cast<std::uint8_t>(
        s1 ^ GfMultiply(symbols[index],
                        GfPower(static_cast<unsigned>(n - 1 - index))));
  }
  return {s0, s1};
}

// Bytes of the RSPC block, counted from sector byte 12.
std::uint8_t BlockByte(const Sector& sector, std::size_t offset) {
  return sector[kSectorHeaderOffset + offset];
}

// P codeword `major` (0-85): 24 bytes down one column of the 43 x 24 word
// matrix, then its two parity bytes.
std::vector<std::uint8_t> PCodeword(const Sector& sector, std::size_t major) {
  std::vector<std::uint8_t> symbols;
  const std::size_t column = (major / 2) * 2 + (major % 2);
  for (std::size_t row = 0; row < 24; ++row) {
    symbols.push_back(BlockByte(sector, column + 86 * row));
  }
  symbols.push_back(sector[kSectorPParityOffset + major]);
  symbols.push_back(sector[kSectorPParityOffset + 86 + major]);
  return symbols;
}

// Q codeword `major` (0-51): 43 bytes along one diagonal, then its parity.
std::vector<std::uint8_t> QCodeword(const Sector& sector, std::size_t major) {
  std::vector<std::uint8_t> symbols;
  std::size_t index = (major / 2) * 86 + (major % 2);
  for (std::size_t step = 0; step < 43; ++step) {
    symbols.push_back(BlockByte(sector, index));
    index = (index + 88) % 2236;
  }
  symbols.push_back(sector[kSectorQParityOffset + major]);
  symbols.push_back(sector[kSectorQParityOffset + 52 + major]);
  return symbols;
}

std::array<std::uint8_t, kMode1UserDataBytes> PatternedUserData() {
  std::array<std::uint8_t, kMode1UserDataBytes> data{};
  for (std::size_t index = 0; index < data.size(); ++index) {
    data[index] = static_cast<std::uint8_t>((index * 37U + 11U) & 0xFFU);
  }
  return data;
}

TEST(EfmDataSectorTest, ScrambleSequenceMatchesTheAnnexBRegister) {
  // The first bytes of decode-orc's unscramble table (dec_data24torawsector).
  const std::array<std::uint8_t, 16> expected = {
      0x01, 0x80, 0x00, 0x60, 0x00, 0x28, 0x00, 0x1E,
      0x80, 0x08, 0x60, 0x06, 0xA8, 0x02, 0xFE, 0x81};
  for (std::size_t index = 0; index < kSectorSyncBytes; ++index) {
    EXPECT_EQ(ScrambleByte(index), 0) << "sync byte " << index;
  }
  for (std::size_t index = 0; index < expected.size(); ++index) {
    EXPECT_EQ(ScrambleByte(kSectorSyncBytes + index), expected[index])
        << "byte " << kSectorSyncBytes + index;
  }
}

TEST(EfmDataSectorTest, EdcMatchesTheReferenceTable) {
  // A single byte 01 yields entry 1 of decode-orc's EDC look-up table.
  const std::uint8_t one = 0x01;
  EXPECT_EQ(ComputeEdc(&one, 1), 0x90910101U);
  EXPECT_EQ(ComputeEdc(nullptr, 0), 0U);
}

TEST(EfmDataSectorTest, Mode1SectorHasSyncHeaderAndValidEdc) {
  // 01:02:03 = 75 * 62 + 3 sections.
  const std::size_t address = (75 * 62) + 3;
  const Sector sector = BuildMode1Sector(address, PatternedUserData());

  EXPECT_EQ(sector[0], 0x00);
  for (std::size_t index = 1; index < 11; ++index) {
    EXPECT_EQ(sector[index], 0xFF);
  }
  EXPECT_EQ(sector[11], 0x00);
  EXPECT_EQ(sector[12], 0x01);
  EXPECT_EQ(sector[13], 0x02);
  EXPECT_EQ(sector[14], 0x03);
  EXPECT_EQ(sector[kSectorModeOffset], kSectorMode1);
  EXPECT_EQ(sector[kSectorUserDataOffset + 5], PatternedUserData()[5]);

  // A reflected CRC over its own LSB-first remainder leaves zero.
  EXPECT_EQ(ComputeEdc(sector.data(), kSectorEdcOffset + 4), 0U);
  for (std::size_t index = 0; index < kSectorZeroBytes; ++index) {
    EXPECT_EQ(sector[kSectorZeroOffset + index], 0);
  }
}

TEST(EfmDataSectorTest, RspcCodewordsHaveZeroSyndromes) {
  const Sector sector = BuildMode1Sector(12345, PatternedUserData());
  for (std::size_t major = 0; major < 86; ++major) {
    const auto syndromes = Syndromes(PCodeword(sector, major));
    EXPECT_EQ(syndromes[0], 0) << "P codeword " << major;
    EXPECT_EQ(syndromes[1], 0) << "P codeword " << major;
  }
  for (std::size_t major = 0; major < 52; ++major) {
    const auto syndromes = Syndromes(QCodeword(sector, major));
    EXPECT_EQ(syndromes[0], 0) << "Q codeword " << major;
    EXPECT_EQ(syndromes[1], 0) << "Q codeword " << major;
  }
}

TEST(EfmDataSectorTest, CorruptedByteBreaksItsPAndQCodewords) {
  Sector sector = BuildMode1Sector(0, PatternedUserData());
  sector[kSectorUserDataOffset + 100] ^= 0x5A;

  std::size_t failing_p = 0;
  for (std::size_t major = 0; major < 86; ++major) {
    if (Syndromes(PCodeword(sector, major)) != std::array<std::uint8_t, 2>{}) {
      ++failing_p;
    }
  }
  std::size_t failing_q = 0;
  for (std::size_t major = 0; major < 52; ++major) {
    if (Syndromes(QCodeword(sector, major)) != std::array<std::uint8_t, 2>{}) {
      ++failing_q;
    }
  }
  EXPECT_EQ(failing_p, 1U);
  EXPECT_EQ(failing_q, 1U);
}

TEST(EfmDataSectorTest, LeadInSectorAddsA0ToTheMinuteField) {
  const Sector sector = BuildLeadInMode1Sector((75 * 60 * 3) + (75 * 7) + 9);
  EXPECT_EQ(sector[12], 0xA3);
  EXPECT_EQ(sector[13], 0x07);
  EXPECT_EQ(sector[14], 0x09);
  EXPECT_EQ(sector[kSectorModeOffset], kSectorMode1);
  EXPECT_EQ(ComputeEdc(sector.data(), kSectorEdcOffset + 4), 0U);
}

TEST(EfmDataSectorTest, Mode0SectorIsZeroAfterTheHeader) {
  const Sector sector = BuildMode0Sector(449999);
  EXPECT_EQ(sector[12], 0x99);
  EXPECT_EQ(sector[13], 0x59);
  EXPECT_EQ(sector[14], 0x74);
  EXPECT_EQ(sector[kSectorModeOffset], kSectorMode0);
  for (std::size_t index = kSectorUserDataOffset; index < kSectorBytes;
       ++index) {
    ASSERT_EQ(sector[index], 0) << "byte " << index;
  }
}

TEST(EfmDataSectorTest, AddressesBeyondTheHeaderRangeSaturate) {
  const Sector sector = BuildMode0Sector(kMaxSectorAddress + 10);
  EXPECT_EQ(sector[12], 0x99);
  EXPECT_EQ(sector[13], 0x59);
  EXPECT_EQ(sector[14], 0x74);
}

TEST(EfmDataSectorTest, ScramblingIsItsOwnInverse) {
  const Sector original = BuildMode1Sector(42, PatternedUserData());
  Sector sector = original;
  ScrambleSector(&sector);
  EXPECT_NE(sector, original);
  for (std::size_t index = 0; index < kSectorSyncBytes; ++index) {
    EXPECT_EQ(sector[index], original[index]);
  }
  ScrambleSector(&sector);
  EXPECT_EQ(sector, original);
}

TEST(EfmDataSectorTest, SectorBytesFormLittleEndianWordsInF1Frames) {
  Sector sector{};
  for (std::size_t index = 0; index < kSectorBytes; ++index) {
    sector[index] = static_cast<std::uint8_t>(index & 0xFFU);
  }
  const auto frames = SectorToF1Frames(sector);
  ASSERT_EQ(frames.size(), kFramesPerSubcodeSection);
  EXPECT_EQ(frames[0][0], sector[1]);
  EXPECT_EQ(frames[0][1], sector[0]);
  EXPECT_EQ(frames[0][23], sector[22]);
  EXPECT_EQ(frames[97][0], sector[(97 * 24) + 1]);
  EXPECT_EQ(frames[97][23], sector[kSectorBytes - 2]);
}

}  // namespace
}  // namespace videosynth::efm
