/*
 * File:        test_cav_code_generators.cpp
 * Module:      cav_code_generator_tests
 * Purpose:     Unit tests for CAV LaserDisc biphase code generators per
 *              IEC 60856/60857.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <cstdint>

#include "videosynth/cav_code_generator.h"
#include "videosynth/model.h"

namespace videosynth {
namespace {

// ---------------------------------------------------------------------------
// LeadInCodeGeneratorTest
// ---------------------------------------------------------------------------

TEST(CavCodeGeneratorTest, LeadInAlwaysReturnsFixedCode) {
  LeadInCodeGenerator gen;
  EXPECT_EQ(gen.CurrentCode(), 0x88FFFFu);
}

TEST(CavCodeGeneratorTest, LeadInCodeMatchesConstant) {
  EXPECT_EQ(LeadInCodeGenerator::kCode, 0x88FFFFu);
}

TEST(CavCodeGeneratorTest, LeadInAdvanceDoesNotChangeCode) {
  LeadInCodeGenerator gen;
  gen.Advance();
  gen.Advance();
  EXPECT_EQ(gen.CurrentCode(), 0x88FFFFu);
}

TEST(CavCodeGeneratorTest, LeadInResetDoesNotChangeCode) {
  LeadInCodeGenerator gen;
  gen.Reset();
  EXPECT_EQ(gen.CurrentCode(), 0x88FFFFu);
}

TEST(CavCodeGeneratorTest, LeadInKeyNibbleIsValid) {
  // IEC 60856/60857: bit 23 of the 24-bit code must be 1.
  EXPECT_NE(LeadInCodeGenerator::kCode & 0x800000u, 0u);
}

// ---------------------------------------------------------------------------
// LeadOutCodeGeneratorTest
// ---------------------------------------------------------------------------

TEST(CavCodeGeneratorTest, LeadOutAlwaysReturnsFixedCode) {
  LeadOutCodeGenerator gen;
  EXPECT_EQ(gen.CurrentCode(), 0x80EEEEu);
}

TEST(CavCodeGeneratorTest, LeadOutCodeMatchesConstant) {
  EXPECT_EQ(LeadOutCodeGenerator::kCode, 0x80EEEEu);
}

TEST(CavCodeGeneratorTest, LeadOutAdvanceDoesNotChangeCode) {
  LeadOutCodeGenerator gen;
  gen.Advance();
  EXPECT_EQ(gen.CurrentCode(), 0x80EEEEu);
}

TEST(CavCodeGeneratorTest, LeadOutKeyNibbleIsValid) {
  EXPECT_NE(LeadOutCodeGenerator::kCode & 0x800000u, 0u);
}

// ---------------------------------------------------------------------------
// PictureStopCodeGeneratorTest
// ---------------------------------------------------------------------------

TEST(CavCodeGeneratorTest, PictureStopAlwaysReturnsFixedCode) {
  PictureStopCodeGenerator gen;
  EXPECT_EQ(gen.CurrentCode(), 0x82CFFFu);
}

TEST(CavCodeGeneratorTest, PictureStopCodeMatchesConstant) {
  EXPECT_EQ(PictureStopCodeGenerator::kCode, 0x82CFFFu);
}

TEST(CavCodeGeneratorTest, PictureStopAdvanceDoesNotChangeCode) {
  PictureStopCodeGenerator gen;
  gen.Advance();
  EXPECT_EQ(gen.CurrentCode(), 0x82CFFFu);
}

TEST(CavCodeGeneratorTest, PictureStopKeyNibbleIsValid) {
  EXPECT_NE(PictureStopCodeGenerator::kCode & 0x800000u, 0u);
}

// ---------------------------------------------------------------------------
// CavPictureNumberGenerator — EncodePictureNumber
// ---------------------------------------------------------------------------

TEST(CavCodeGeneratorTest, EncodePictureNumberZeroGivesF00000) {
  EXPECT_EQ(CavPictureNumberGenerator::EncodePictureNumber(0), 0xF00000u);
}

TEST(CavCodeGeneratorTest, EncodePictureNumberOneGivesF00001) {
  EXPECT_EQ(CavPictureNumberGenerator::EncodePictureNumber(1), 0xF00001u);
}

TEST(CavCodeGeneratorTest, EncodePictureNumberTenGivesF00010) {
  EXPECT_EQ(CavPictureNumberGenerator::EncodePictureNumber(10), 0xF00010u);
}

TEST(CavCodeGeneratorTest, EncodePictureNumber12345GivesF12345) {
  EXPECT_EQ(CavPictureNumberGenerator::EncodePictureNumber(12345), 0xF12345u);
}

TEST(CavCodeGeneratorTest, EncodePictureNumber99999GivesF99999) {
  EXPECT_EQ(CavPictureNumberGenerator::EncodePictureNumber(99999), 0xF99999u);
}

TEST(CavCodeGeneratorTest, EncodePictureNumber79999GivesF79999) {
  EXPECT_EQ(CavPictureNumberGenerator::EncodePictureNumber(79999), 0xF79999u);
}

TEST(CavCodeGeneratorTest, EncodePictureNumberAllSameDigits55555) {
  EXPECT_EQ(CavPictureNumberGenerator::EncodePictureNumber(55555), 0xF55555u);
}

TEST(CavCodeGeneratorTest, EncodedPictureNumberHasValidKeyNibble) {
  // Key nibble (bits 23-20) must be 0xF = 1111b, so bit 23 = 1.
  const uint32_t code = CavPictureNumberGenerator::EncodePictureNumber(42000);
  EXPECT_NE(code & 0x800000u, 0u);
}

// ---------------------------------------------------------------------------
// CavPictureNumberGenerator — IsValidPictureNumber
// ---------------------------------------------------------------------------

TEST(CavCodeGeneratorTest, PictureNumberZeroIsValidForPal) {
  EXPECT_TRUE(
      CavPictureNumberGenerator::IsValidPictureNumber(0, Standard::kPal));
}

TEST(CavCodeGeneratorTest, PictureNumberZeroIsValidForNtsc) {
  EXPECT_TRUE(
      CavPictureNumberGenerator::IsValidPictureNumber(0, Standard::kNtsc));
}

TEST(CavCodeGeneratorTest, PictureNumber99999IsValidForPal) {
  EXPECT_TRUE(
      CavPictureNumberGenerator::IsValidPictureNumber(99999, Standard::kPal));
}

TEST(CavCodeGeneratorTest, PictureNumber100000IsInvalidForPal) {
  EXPECT_FALSE(
      CavPictureNumberGenerator::IsValidPictureNumber(100000, Standard::kPal));
}

TEST(CavCodeGeneratorTest, PictureNumber79999IsValidForNtsc) {
  EXPECT_TRUE(
      CavPictureNumberGenerator::IsValidPictureNumber(79999, Standard::kNtsc));
}

TEST(CavCodeGeneratorTest, PictureNumber80000IsInvalidForNtsc) {
  EXPECT_FALSE(
      CavPictureNumberGenerator::IsValidPictureNumber(80000, Standard::kNtsc));
}

TEST(CavCodeGeneratorTest, PictureNumber99999IsInvalidForNtsc) {
  EXPECT_FALSE(
      CavPictureNumberGenerator::IsValidPictureNumber(99999, Standard::kNtsc));
}

TEST(CavCodeGeneratorTest, NegativePictureNumberIsInvalid) {
  EXPECT_FALSE(
      CavPictureNumberGenerator::IsValidPictureNumber(-1, Standard::kPal));
}

TEST(CavCodeGeneratorTest, PalMaxPictureNumberIs99999) {
  EXPECT_EQ(CavPictureNumberGenerator::MaxPictureNumber(Standard::kPal), 99999);
}

TEST(CavCodeGeneratorTest, NtscMaxPictureNumberIs79999) {
  EXPECT_EQ(CavPictureNumberGenerator::MaxPictureNumber(Standard::kNtsc),
            79999);
}

TEST(CavCodeGeneratorTest, PalMMaxPictureNumberIs79999) {
  EXPECT_EQ(CavPictureNumberGenerator::MaxPictureNumber(Standard::kPalM),
            79999);
}

// ---------------------------------------------------------------------------
// CavPictureNumberGenerator — state machine
// ---------------------------------------------------------------------------

TEST(CavCodeGeneratorTest, PictureNumberStartsAtGivenValue) {
  CavPictureNumberGenerator gen(1, Standard::kPal);
  EXPECT_EQ(gen.CurrentCode(), 0xF00001u);
  EXPECT_EQ(gen.current_value(), 1);
}

TEST(CavCodeGeneratorTest, PictureNumberAdvanceIncrementsValue) {
  CavPictureNumberGenerator gen(1, Standard::kPal);
  gen.Advance();
  EXPECT_EQ(gen.current_value(), 2);
  EXPECT_EQ(gen.CurrentCode(), 0xF00002u);
}

TEST(CavCodeGeneratorTest, PictureNumberAdvanceMultipleTimes) {
  CavPictureNumberGenerator gen(0, Standard::kPal);
  for (int i = 0; i < 10; ++i) {
    gen.Advance();
  }
  EXPECT_EQ(gen.current_value(), 10);
  EXPECT_EQ(gen.CurrentCode(), 0xF00010u);
}

TEST(CavCodeGeneratorTest, PictureNumberSaturatesAtPalMax) {
  CavPictureNumberGenerator gen(99998, Standard::kPal);
  gen.Advance();
  EXPECT_EQ(gen.current_value(), 99999);
  gen.Advance();
  EXPECT_EQ(gen.current_value(), 99999);
}

TEST(CavCodeGeneratorTest, PictureNumberSaturatesAtNtscMax) {
  CavPictureNumberGenerator gen(79998, Standard::kNtsc);
  gen.Advance();
  EXPECT_EQ(gen.current_value(), 79999);
  gen.Advance();
  EXPECT_EQ(gen.current_value(), 79999);
}

TEST(CavCodeGeneratorTest, PictureNumberResetRestoresStartValue) {
  CavPictureNumberGenerator gen(5, Standard::kPal);
  gen.Advance();
  gen.Advance();
  gen.Advance();
  EXPECT_EQ(gen.current_value(), 8);
  gen.Reset();
  EXPECT_EQ(gen.current_value(), 5);
  EXPECT_EQ(gen.CurrentCode(), 0xF00005u);
}

// ---------------------------------------------------------------------------
// ChapterNumberGenerator — IsValidChapterNumber
// ---------------------------------------------------------------------------

TEST(CavCodeGeneratorTest, ChapterZeroIsValid) {
  EXPECT_TRUE(ChapterNumberGenerator::IsValidChapterNumber(0));
}

TEST(CavCodeGeneratorTest, Chapter79IsValid) {
  EXPECT_TRUE(ChapterNumberGenerator::IsValidChapterNumber(79));
}

TEST(CavCodeGeneratorTest, Chapter80IsInvalid) {
  EXPECT_FALSE(ChapterNumberGenerator::IsValidChapterNumber(80));
}

TEST(CavCodeGeneratorTest, NegativeChapterIsInvalid) {
  EXPECT_FALSE(ChapterNumberGenerator::IsValidChapterNumber(-1));
}

// ---------------------------------------------------------------------------
// ChapterNumberGenerator — EncodeChapterCode / DecodeChapterNumber
// ---------------------------------------------------------------------------

TEST(CavCodeGeneratorTest, EncodeChapter0StopBitFalse) {
  // Chapter 0, stop-bit 0: X₁ = 0b0000 = 0, X₂ = 0, DDD = 0xDDD
  // Code = 0x800000 | 0x000000 | 0x000DDD = 0x800DDD
  const uint32_t code = ChapterNumberGenerator::EncodeChapterCode(0, false);
  EXPECT_EQ(code, 0x800DDDu);
  EXPECT_EQ(ChapterNumberGenerator::DecodeChapterNumber(code), 0);
  EXPECT_FALSE(ChapterNumberGenerator::DecodeStopBit(code));
}

TEST(CavCodeGeneratorTest, EncodeChapter0StopBitTrue) {
  // Chapter 0, stop-bit 1: X₁ = 0b1000 = 8, X₂ = 0
  // Code = 0x800000 | (8 << 16) | 0x000DDD = 0x880DDD
  const uint32_t code = ChapterNumberGenerator::EncodeChapterCode(0, true);
  EXPECT_EQ(code, 0x880DDDu);
  EXPECT_EQ(ChapterNumberGenerator::DecodeChapterNumber(code), 0);
  EXPECT_TRUE(ChapterNumberGenerator::DecodeStopBit(code));
}

TEST(CavCodeGeneratorTest, EncodeChapter1StopBitFalse) {
  // Chapter 1, stop-bit 0: X₁ = 0, X₂ = 1
  const uint32_t code = ChapterNumberGenerator::EncodeChapterCode(1, false);
  EXPECT_EQ(ChapterNumberGenerator::DecodeChapterNumber(code), 1);
  EXPECT_FALSE(ChapterNumberGenerator::DecodeStopBit(code));
}

TEST(CavCodeGeneratorTest, EncodeChapter16StopBitFalse) {
  // Chapter 16, stop-bit 0: X₁ = (16/16) & 7 = 1, X₂ = 0
  const uint32_t code = ChapterNumberGenerator::EncodeChapterCode(16, false);
  EXPECT_EQ(ChapterNumberGenerator::DecodeChapterNumber(code), 16);
  EXPECT_FALSE(ChapterNumberGenerator::DecodeStopBit(code));
}

TEST(CavCodeGeneratorTest, EncodeChapter16StopBitTrue) {
  // Chapter 16, stop-bit 1: X₁ = 0b1001 = 9
  const uint32_t code = ChapterNumberGenerator::EncodeChapterCode(16, true);
  EXPECT_EQ(ChapterNumberGenerator::DecodeChapterNumber(code), 16);
  EXPECT_TRUE(ChapterNumberGenerator::DecodeStopBit(code));
}

TEST(CavCodeGeneratorTest, EncodeChapter79StopBitFalse) {
  // Chapter 79 = 4*16+15, X₁ = 4, X₂ = 15 = 0xF
  const uint32_t code = ChapterNumberGenerator::EncodeChapterCode(79, false);
  EXPECT_EQ(ChapterNumberGenerator::DecodeChapterNumber(code), 79);
  EXPECT_FALSE(ChapterNumberGenerator::DecodeStopBit(code));
}

TEST(CavCodeGeneratorTest, EncodeChapter79StopBitTrue) {
  const uint32_t code = ChapterNumberGenerator::EncodeChapterCode(79, true);
  EXPECT_EQ(ChapterNumberGenerator::DecodeChapterNumber(code), 79);
  EXPECT_TRUE(ChapterNumberGenerator::DecodeStopBit(code));
}

TEST(CavCodeGeneratorTest, EncodedChapterCodeHasDddPattern) {
  // Bits 11-0 of the encoded chapter code must always be 0xDDD.
  for (int ch = 0; ch <= 79; ++ch) {
    const uint32_t code_zero =
        ChapterNumberGenerator::EncodeChapterCode(ch, false);
    const uint32_t code_one =
        ChapterNumberGenerator::EncodeChapterCode(ch, true);
    EXPECT_EQ(code_zero & 0x000FFFu, 0x000DDDu) << "chapter=" << ch;
    EXPECT_EQ(code_one & 0x000FFFu, 0x000DDDu) << "chapter=" << ch;
  }
}

TEST(CavCodeGeneratorTest, EncodedChapterCodeHasValidKeyNibble) {
  // Bit 23 must be 1 for all valid chapter codes.
  for (int ch = 0; ch <= 79; ++ch) {
    const uint32_t code = ChapterNumberGenerator::EncodeChapterCode(ch, false);
    EXPECT_NE(code & 0x800000u, 0u) << "chapter=" << ch;
  }
}

TEST(CavCodeGeneratorTest, ChapterEncodeDecodeRoundTripAllChapters) {
  for (int ch = 0; ch <= 79; ++ch) {
    for (bool sb : {false, true}) {
      const uint32_t code = ChapterNumberGenerator::EncodeChapterCode(ch, sb);
      EXPECT_EQ(ChapterNumberGenerator::DecodeChapterNumber(code), ch)
          << "chapter=" << ch << " stop_bit=" << sb;
      EXPECT_EQ(ChapterNumberGenerator::DecodeStopBit(code), sb)
          << "chapter=" << ch << " stop_bit=" << sb;
    }
  }
}

// ---------------------------------------------------------------------------
// ChapterNumberGenerator — stop-bit state machine
// ---------------------------------------------------------------------------

TEST(CavCodeGeneratorTest, ChapterStopBitIsZeroForFirst400Tracks) {
  ChapterNumberGenerator gen(1, false);
  // Tracks 0–399: stop-bit = 0.
  for (int t = 0; t < 400; ++t) {
    const uint32_t code = gen.CurrentCode();
    EXPECT_FALSE(ChapterNumberGenerator::DecodeStopBit(code))
        << "Expected stop-bit=0 at track " << t;
    gen.Advance();
  }
}

TEST(CavCodeGeneratorTest, ChapterStopBitIsOneAfterTrack400) {
  ChapterNumberGenerator gen(1, false);
  // Advance past the transition point.
  for (int t = 0; t < 400; ++t) {
    gen.Advance();
  }
  // Track 400 and beyond: stop-bit = 1.
  for (int t = 400; t < 410; ++t) {
    const uint32_t code = gen.CurrentCode();
    EXPECT_TRUE(ChapterNumberGenerator::DecodeStopBit(code))
        << "Expected stop-bit=1 at track " << t;
    gen.Advance();
  }
}

TEST(CavCodeGeneratorTest, ChapterStopBitAlwaysOneForFirstChapterAfterLeadIn) {
  // IEC 60856/60857 §10.1.5: first chapter after lead-in always has stop-bit=1.
  ChapterNumberGenerator gen(0, /*always_stop_bit_one=*/true);
  for (int t = 0; t < 600; ++t) {
    const uint32_t code = gen.CurrentCode();
    EXPECT_TRUE(ChapterNumberGenerator::DecodeStopBit(code))
        << "Expected stop-bit=1 at track " << t;
    gen.Advance();
  }
}

TEST(CavCodeGeneratorTest, ChapterNumberDoesNotChangeOnAdvance) {
  ChapterNumberGenerator gen(5, false);
  for (int t = 0; t < 500; ++t) {
    EXPECT_EQ(ChapterNumberGenerator::DecodeChapterNumber(gen.CurrentCode()),
              5);
    gen.Advance();
  }
}

TEST(CavCodeGeneratorTest, ChapterResetRestoresInitialStopBitState) {
  ChapterNumberGenerator gen(1, false);
  // Advance past transition so stop-bit becomes 1.
  for (int t = 0; t < 401; ++t) {
    gen.Advance();
  }
  EXPECT_TRUE(ChapterNumberGenerator::DecodeStopBit(gen.CurrentCode()));
  // Reset should restore stop-bit to 0.
  gen.Reset();
  EXPECT_FALSE(ChapterNumberGenerator::DecodeStopBit(gen.CurrentCode()));
}

// ---------------------------------------------------------------------------
// ProgrammeStatusCodeGenerator
// ---------------------------------------------------------------------------

TEST(CavCodeGeneratorTest, ProgrammeStatusReturnsProvidedCode) {
  ProgrammeStatusCodeGenerator gen(0x8DC000u);
  EXPECT_EQ(gen.CurrentCode(), 0x8DC000u);
}

TEST(CavCodeGeneratorTest, ProgrammeStatusAdvanceDoesNotChangeCode) {
  ProgrammeStatusCodeGenerator gen(0x8BA123u);
  gen.Advance();
  EXPECT_EQ(gen.CurrentCode(), 0x8BA123u);
}

TEST(CavCodeGeneratorTest, ProgrammeStatusValidCodePassesValidation) {
  // CX on: 0x8DC000 — bit 23 = 1.
  EXPECT_TRUE(
      ProgrammeStatusCodeGenerator::IsValidProgrammeStatusCode(0x8DC000u));
}

TEST(CavCodeGeneratorTest, ProgrammeStatusInvalidCodeFailsValidation) {
  // bit 23 = 0 → invalid key nibble.
  EXPECT_FALSE(
      ProgrammeStatusCodeGenerator::IsValidProgrammeStatusCode(0x0DC000u));
}

TEST(CavCodeGeneratorTest, ProgrammeStatusCxOnCodeHasValidKeyNibble) {
  // CX on: 0x8DC... — first nibble 8 = 1000b, bit 23 = 1.
  EXPECT_TRUE(
      ProgrammeStatusCodeGenerator::IsValidProgrammeStatusCode(0x8DC000u));
}

TEST(CavCodeGeneratorTest, ProgrammeStatusCxOffCodeHasValidKeyNibble) {
  // CX off: 0x8BA... — first nibble 8 = 1000b, bit 23 = 1.
  EXPECT_TRUE(
      ProgrammeStatusCodeGenerator::IsValidProgrammeStatusCode(0x8BA000u));
}

// ---------------------------------------------------------------------------
// UsersCodeGenerator
// ---------------------------------------------------------------------------

TEST(CavCodeGeneratorTest, UsersCodeReturnsProvidedCode) {
  UsersCodeGenerator gen(0x800000u);
  EXPECT_EQ(gen.CurrentCode(), 0x800000u);
}

TEST(CavCodeGeneratorTest, UsersCodeAdvanceDoesNotChangeCode) {
  UsersCodeGenerator gen(0x810000u);
  gen.Advance();
  EXPECT_EQ(gen.CurrentCode(), 0x810000u);
}

TEST(CavCodeGeneratorTest, UsersCodeX1ZeroIsValid) {
  // X₁ = 0 with canonical D nibble (0xD): valid.
  EXPECT_TRUE(UsersCodeGenerator::IsValidUsersCode(0x80D000u));
}

TEST(CavCodeGeneratorTest, UsersCodeX1SevenIsValid) {
  // X₁ = 7 with canonical D nibble (0xD): valid.
  EXPECT_TRUE(UsersCodeGenerator::IsValidUsersCode(0x87D000u));
}

TEST(CavCodeGeneratorTest, UsersCodeX1EightIsInvalid) {
  // X₁ = 8 with canonical D nibble: invalid because X₁ exceeds 7.
  EXPECT_FALSE(UsersCodeGenerator::IsValidUsersCode(0x88D000u));
}

TEST(CavCodeGeneratorTest, UsersCodeX1FifteenIsInvalid) {
  // X₁ = 0xF with canonical D nibble: invalid because X₁ exceeds 7.
  EXPECT_FALSE(UsersCodeGenerator::IsValidUsersCode(0x8FD000u));
}

TEST(CavCodeGeneratorTest, UsersCodeInvalidDNibbleIsInvalid) {
  // D nibble ≠ 0xD (IEC §10.1.9): invalid regardless of valid X₁.
  EXPECT_FALSE(UsersCodeGenerator::IsValidUsersCode(0x801234u));  // D=1
  EXPECT_FALSE(UsersCodeGenerator::IsValidUsersCode(0x870000u));  // D=0
}

TEST(CavCodeGeneratorTest, UsersCodeExtractX1ReturnsCorrectNibble) {
  // Code 0x83D000: nibble at bits 19-16 = 3.
  EXPECT_EQ(UsersCodeGenerator::ExtractX1(0x83D000u), 3u);
}

TEST(CavCodeGeneratorTest, UsersCodeExtractX1ForAllValidValues) {
  // Use canonical D nibble (0xD) so codes are also valid for IsValidUsersCode.
  for (uint8_t x1 = 0; x1 <= 7; ++x1) {
    const uint32_t code =
        0x800000u | (static_cast<uint32_t>(x1) << 16) | 0x00D000u;
    EXPECT_EQ(UsersCodeGenerator::ExtractX1(code), x1);
    EXPECT_TRUE(UsersCodeGenerator::IsValidUsersCode(code));
  }
}

TEST(CavCodeGeneratorTest, UsersCodeExtractX1ForInvalidValues) {
  // Use canonical D nibble: X₁ alone makes these invalid.
  for (uint8_t x1 = 8; x1 <= 15; ++x1) {
    const uint32_t code =
        0x800000u | (static_cast<uint32_t>(x1) << 16) | 0x00D000u;
    EXPECT_EQ(UsersCodeGenerator::ExtractX1(code), x1);
    EXPECT_FALSE(UsersCodeGenerator::IsValidUsersCode(code));
  }
}

}  // namespace
}  // namespace videosynth
