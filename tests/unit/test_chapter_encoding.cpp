/*
 * File:        test_chapter_encoding.cpp
 * Module:      chapter_encoding_tests
 * Purpose:     Tests for chapter number encoding and decoding per IEC formula.
 *              IEC 60856/60857 §10.1.5:
 *                Chapter = (X₁ & 7) × 10 + X₂  (two BCD digits)
 *                Stop-bit = (X₁ & 8) >> 3
 *              Task 6.15: Tests for chapter encoding/decoding.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <cstdint>

#include "videosynth/biphase_utils.h"
#include "videosynth/cav_code_generator.h"

namespace videosynth {
namespace {

// ---------------------------------------------------------------------------
// ChapterEncodingTest — known encoding values per IEC formula
// ---------------------------------------------------------------------------

TEST(ChapterEncodingTest, Chapter0StopBitFalseProducesCorrectCode) {
  // Chapter 0, stop-bit 0:
  //   X₁_low = 0/16 = 0, X₂ = 0%16 = 0, stop=0 → X₁ = 0<<3|0 = 0x0
  //   Code = 8 0 0 D D D = 0x800DDD
  const uint32_t code = ChapterNumberGenerator::EncodeChapterCode(0, false);
  EXPECT_EQ(code, 0x800DDDu);
}

TEST(ChapterEncodingTest, Chapter0StopBitTrueProducesCorrectCode) {
  // Chapter 0, stop-bit 1:
  //   X₁ = 1<<3 | 0 = 0x8
  //   Code = 8 8 0 D D D = 0x880DDD
  const uint32_t code = ChapterNumberGenerator::EncodeChapterCode(0, true);
  EXPECT_EQ(code, 0x880DDDu);
}

TEST(ChapterEncodingTest, Chapter1StopBitFalseProducesCorrectCode) {
  // Chapter 1: X₁_low = 0, X₂ = 1 → 8 0 1 D D D = 0x801DDD
  const uint32_t code = ChapterNumberGenerator::EncodeChapterCode(1, false);
  EXPECT_EQ(code, 0x801DDDu);
}

TEST(ChapterEncodingTest, Chapter15StopBitFalseProducesCorrectCode) {
  // Chapter 15: BCD digits 1 and 5 → 8 1 5 D D D = 0x815DDD
  const uint32_t code = ChapterNumberGenerator::EncodeChapterCode(15, false);
  EXPECT_EQ(code, 0x815DDDu);
}

TEST(ChapterEncodingTest, Chapter16StopBitFalseProducesCorrectCode) {
  // Chapter 16: BCD digits 1 and 6 → 8 1 6 D D D = 0x816DDD
  const uint32_t code = ChapterNumberGenerator::EncodeChapterCode(16, false);
  EXPECT_EQ(code, 0x816DDDu);
}

TEST(ChapterEncodingTest, Chapter16StopBitTrueProducesCorrectCode) {
  // Chapter 16, stop-bit 1: X₁_nibble = 8|1 = 9 → 8 9 6 D D D = 0x896DDD
  const uint32_t code = ChapterNumberGenerator::EncodeChapterCode(16, true);
  EXPECT_EQ(code, 0x896DDDu);
}

TEST(ChapterEncodingTest, Chapter79StopBitFalseProducesCorrectCode) {
  // Chapter 79: BCD digits 7 and 9 → 8 7 9 D D D = 0x879DDD
  const uint32_t code = ChapterNumberGenerator::EncodeChapterCode(79, false);
  EXPECT_EQ(code, 0x879DDDu);
}

TEST(ChapterEncodingTest, Chapter79StopBitTrueProducesCorrectCode) {
  // Chapter 79, stop-bit 1: X₁_nibble = 8|7 = 0xF → 8 F 9 D D D = 0x8F9DDD
  const uint32_t code = ChapterNumberGenerator::EncodeChapterCode(79, true);
  EXPECT_EQ(code, 0x8F9DDDu);
}

// ---------------------------------------------------------------------------
// ChapterEncodingTest — decoding per IEC formula
// ---------------------------------------------------------------------------

TEST(ChapterEncodingTest, DecodeChapterNumberFromCode0x800DDD) {
  // 0x800DDD → X₁_nibble=0, X₂=0 → chapter = (0&7)×16 + 0 = 0
  EXPECT_EQ(ChapterNumberGenerator::DecodeChapterNumber(0x800DDDu), 0);
}

TEST(ChapterEncodingTest, DecodeChapterNumberFromCode0x880DDD) {
  // 0x880DDD → X₁_nibble=8, X₂=0 → chapter = (8&7)×16 + 0 = 0
  EXPECT_EQ(ChapterNumberGenerator::DecodeChapterNumber(0x880DDDu), 0);
}

TEST(ChapterEncodingTest, DecodeChapterNumberFromCode0x801DDD) {
  // 0x801DDD → X₁_nibble=0, X₂=1 → chapter = 0×16 + 1 = 1
  EXPECT_EQ(ChapterNumberGenerator::DecodeChapterNumber(0x801DDDu), 1);
}

TEST(ChapterEncodingTest, DecodeChapterNumberFromCode0x810DDD) {
  // 0x810DDD → X₁_nibble=1, X₂=0 → chapter = 1×10 + 0 = 10
  EXPECT_EQ(ChapterNumberGenerator::DecodeChapterNumber(0x810DDDu), 10);
}

TEST(ChapterEncodingTest, DecodeChapterNumberFromCode0x879DDD) {
  // 0x879DDD → X₁_nibble=7, X₂=9 → chapter = 7×10 + 9 = 79
  EXPECT_EQ(ChapterNumberGenerator::DecodeChapterNumber(0x879DDDu), 79);
}

TEST(ChapterEncodingTest, DecodeChapterNumberFromCode0x8F9DDD) {
  // 0x8F9DDD → X₁_nibble=F, X₂=9 → chapter = (15&7)×10 + 9 = 79
  EXPECT_EQ(ChapterNumberGenerator::DecodeChapterNumber(0x8F9DDDu), 79);
}

// ---------------------------------------------------------------------------
// ChapterEncodingTest — stop-bit decoding per IEC formula
// ---------------------------------------------------------------------------

TEST(ChapterEncodingTest, DecodeStopBitFalseFrom0x800DDD) {
  // X₁_nibble = 0 → stop-bit = (0 & 8) >> 3 = 0 → false
  EXPECT_FALSE(ChapterNumberGenerator::DecodeStopBit(0x800DDDu));
}

TEST(ChapterEncodingTest, DecodeStopBitTrueFrom0x880DDD) {
  // X₁_nibble = 8 → stop-bit = (8 & 8) >> 3 = 1 → true
  EXPECT_TRUE(ChapterNumberGenerator::DecodeStopBit(0x880DDDu));
}

TEST(ChapterEncodingTest, DecodeStopBitFalseFrom0x84FDDD) {
  // X₁_nibble = 4 → stop-bit = (4 & 8) >> 3 = 0 → false
  EXPECT_FALSE(ChapterNumberGenerator::DecodeStopBit(0x84FDDDu));
}

TEST(ChapterEncodingTest, DecodeStopBitTrueFrom0x8CFDDD) {
  // X₁_nibble = 0xC = 12 → stop-bit = (12 & 8) >> 3 = 1 → true
  EXPECT_TRUE(ChapterNumberGenerator::DecodeStopBit(0x8CFDDDu));
}

// ---------------------------------------------------------------------------
// ChapterEncodingTest — IEC formula verification: all chapters 0–79
// ---------------------------------------------------------------------------

TEST(ChapterEncodingTest, EncodeDecodeRoundTripAllChaptersStopBitFalse) {
  // Every chapter 0–79 must round-trip exactly through encode/decode.
  for (int ch = 0; ch <= 79; ++ch) {
    const uint32_t code = ChapterNumberGenerator::EncodeChapterCode(ch, false);
    EXPECT_EQ(ChapterNumberGenerator::DecodeChapterNumber(code), ch)
        << "Chapter " << ch << " round-trip failed with stop-bit=0";
    EXPECT_FALSE(ChapterNumberGenerator::DecodeStopBit(code))
        << "Stop-bit should be 0 for chapter " << ch;
  }
}

TEST(ChapterEncodingTest, EncodeDecodeRoundTripAllChaptersStopBitTrue) {
  // Every chapter 0–79 with stop-bit=1 must round-trip exactly.
  for (int ch = 0; ch <= 79; ++ch) {
    const uint32_t code = ChapterNumberGenerator::EncodeChapterCode(ch, true);
    EXPECT_EQ(ChapterNumberGenerator::DecodeChapterNumber(code), ch)
        << "Chapter " << ch << " round-trip failed with stop-bit=1";
    EXPECT_TRUE(ChapterNumberGenerator::DecodeStopBit(code))
        << "Stop-bit should be 1 for chapter " << ch;
  }
}

TEST(ChapterEncodingTest, ChapterDigitsAreBcdLikeRealDiscs) {
  // The Apple Visual Almanac (NTSC CAV, stop-bit 1 throughout) carries
  // 0x889DDD for chapter 9 and 0x890DDD for chapter 10: X₂ never exceeds 9.
  EXPECT_EQ(ChapterNumberGenerator::EncodeChapterCode(9, true), 0x889DDDu);
  EXPECT_EQ(ChapterNumberGenerator::EncodeChapterCode(10, true), 0x890DDDu);
  for (int ch = 0; ch <= 79; ++ch) {
    const uint32_t code = ChapterNumberGenerator::EncodeChapterCode(ch, false);
    EXPECT_LE((code >> 12) & 0x0Fu, 9u) << "Chapter " << ch;
    EXPECT_LE((code >> 16) & 0x0Fu, 7u) << "Chapter " << ch;
  }
}

// ---------------------------------------------------------------------------
// ChapterEncodingTest — code structure invariants
// ---------------------------------------------------------------------------

TEST(ChapterEncodingTest, EncodedCodeAlwaysHasValidKeyNibble) {
  // Bit 23 (MSB of key nibble) must be 1 for all valid chapter codes.
  for (int ch = 0; ch <= 79; ++ch) {
    for (bool sb : {false, true}) {
      const uint32_t code = ChapterNumberGenerator::EncodeChapterCode(ch, sb);
      EXPECT_TRUE((code & 0x800000u) != 0)
          << "Key nibble invalid for chapter=" << ch
          << " stop-bit=" << static_cast<int>(sb);
    }
  }
}

TEST(ChapterEncodingTest, EncodedCodeAlwaysHasDddPattern) {
  // Bits 11-0 must be the constant DDD = 0xDDD for all chapter codes.
  for (int ch = 0; ch <= 79; ++ch) {
    for (bool sb : {false, true}) {
      const uint32_t code = ChapterNumberGenerator::EncodeChapterCode(ch, sb);
      EXPECT_EQ(code & 0x000FFFu, ChapterNumberGenerator::kDddPattern)
          << "DDD pattern missing for chapter=" << ch
          << " stop-bit=" << static_cast<int>(sb);
    }
  }
}

TEST(ChapterEncodingTest, DddPatternConstantIs0xDDD) {
  EXPECT_EQ(ChapterNumberGenerator::kDddPattern, 0x000DDDu);
}

TEST(ChapterEncodingTest, X1NibbleIsAtBits19To16) {
  // Chapter 16 with stop-bit=0: X₁_nibble should be at bits 19-16.
  // Chapter 16 has tens digit 1, stop_bit = 0 → X₁_nibble = 1.
  const uint32_t code = ChapterNumberGenerator::EncodeChapterCode(16, false);
  const uint8_t x1 = (code >> 16) & 0x0Fu;
  EXPECT_EQ(x1, 1u);
}

TEST(ChapterEncodingTest, X2NibbleIsAtBits15To12) {
  // Chapter 1 with stop-bit=0: X₁_nibble=0, X₂=1 → bits 15-12 should be 1.
  const uint32_t code = ChapterNumberGenerator::EncodeChapterCode(1, false);
  const uint8_t x2 = (code >> 12) & 0x0Fu;
  EXPECT_EQ(x2, 1u);
}

TEST(ChapterEncodingTest, StopBitIsInMsbOfX1Nibble) {
  // Stop-bit = MSB of X₁ nibble = bit 4 of the 24-bit code word.
  // Chapter 0, stop-bit=1: X₁_nibble = 8 (1000b), bit 19 = 1.
  const uint32_t code = ChapterNumberGenerator::EncodeChapterCode(0, true);
  const bool bit4 = (code & (1u << 19)) != 0;
  EXPECT_TRUE(bit4) << "Stop-bit should set bit 19 (MSB of X₁ nibble)";
}

// ---------------------------------------------------------------------------
// ChapterEncodingTest — generator produces correct codes
// ---------------------------------------------------------------------------

TEST(ChapterEncodingTest, GeneratorChapter0InitialCodeWithStopBitFalse) {
  ChapterNumberGenerator gen(0, false);
  EXPECT_EQ(ChapterNumberGenerator::DecodeChapterNumber(gen.CurrentCode()), 0);
  EXPECT_FALSE(ChapterNumberGenerator::DecodeStopBit(gen.CurrentCode()));
}

TEST(ChapterEncodingTest, GeneratorChapter79InitialCodeWithStopBitTrue) {
  ChapterNumberGenerator gen(79, true);
  EXPECT_EQ(ChapterNumberGenerator::DecodeChapterNumber(gen.CurrentCode()), 79);
  EXPECT_TRUE(ChapterNumberGenerator::DecodeStopBit(gen.CurrentCode()));
}

TEST(ChapterEncodingTest, GeneratorChapterNumberDoesNotChangeOnAdvance) {
  // The chapter number itself never changes; only the stop-bit may change.
  ChapterNumberGenerator gen(42, false);
  for (int i = 0; i < 500; ++i) {
    EXPECT_EQ(ChapterNumberGenerator::DecodeChapterNumber(gen.CurrentCode()),
              42)
        << "Chapter number changed at advance " << i;
    gen.Advance();
  }
}

TEST(ChapterEncodingTest, GeneratorStopBitTransitionsAt400Tracks) {
  // Normal chapter (not first after lead-in): stop-bit = 0 for first 400
  // tracks, then 1.
  ChapterNumberGenerator gen(5, false);
  for (int i = 0; i < 400; ++i) {
    EXPECT_FALSE(ChapterNumberGenerator::DecodeStopBit(gen.CurrentCode()))
        << "Expected stop-bit=0 at track " << i;
    gen.Advance();
  }
  EXPECT_TRUE(ChapterNumberGenerator::DecodeStopBit(gen.CurrentCode()))
      << "Expected stop-bit=1 at track 400";
}

TEST(ChapterEncodingTest, GeneratorAlwaysStopBitOneNeverTransitions) {
  // First chapter after lead-in (always_stop_bit_one=true): always 1.
  ChapterNumberGenerator gen(0, true);
  for (int i = 0; i < 500; ++i) {
    EXPECT_TRUE(ChapterNumberGenerator::DecodeStopBit(gen.CurrentCode()))
        << "Expected stop-bit=1 at track " << i;
    gen.Advance();
  }
}

TEST(ChapterEncodingTest, GeneratorResetRestoresInitialStopBitState) {
  ChapterNumberGenerator gen(0, false);
  // Advance past the transition.
  for (int i = 0; i < 450; ++i) {
    gen.Advance();
  }
  EXPECT_TRUE(ChapterNumberGenerator::DecodeStopBit(gen.CurrentCode()));
  gen.Reset();
  EXPECT_FALSE(ChapterNumberGenerator::DecodeStopBit(gen.CurrentCode()))
      << "After Reset(), stop-bit should return to 0";
}

// ---------------------------------------------------------------------------
// ChapterEncodingTest — IEC formula spot checks
// ---------------------------------------------------------------------------

TEST(ChapterEncodingTest, IecFormulaSpotCheckChapter32) {
  // Chapter 32: BCD digits 3 and 2 → 8 3 2 D D D = 0x832DDD
  const uint32_t code = ChapterNumberGenerator::EncodeChapterCode(32, false);
  EXPECT_EQ(code, 0x832DDDu);
  EXPECT_EQ(ChapterNumberGenerator::DecodeChapterNumber(code), 32);
}

TEST(ChapterEncodingTest, IecFormulaSpotCheckChapter63) {
  // Chapter 63: BCD digits 6 and 3 → 8 6 3 D D D = 0x863DDD
  const uint32_t code = ChapterNumberGenerator::EncodeChapterCode(63, false);
  EXPECT_EQ(code, 0x863DDDu);
  EXPECT_EQ(ChapterNumberGenerator::DecodeChapterNumber(code), 63);
}

TEST(ChapterEncodingTest, IecFormulaSpotCheckChapter63StopBitTrue) {
  // Chapter 63, stop-bit=1: X₁_nibble = 8|6 = 0xE → 8 E 3 D D D = 0x8E3DDD
  const uint32_t code = ChapterNumberGenerator::EncodeChapterCode(63, true);
  EXPECT_EQ(code, 0x8E3DDDu);
  EXPECT_TRUE(ChapterNumberGenerator::DecodeStopBit(code));
  EXPECT_EQ(ChapterNumberGenerator::DecodeChapterNumber(code), 63);
}

TEST(ChapterEncodingTest, IecFormulaSpotCheckChapter48) {
  // Chapter 48: BCD digits 4 and 8 → 8 4 8 D D D = 0x848DDD
  const uint32_t code = ChapterNumberGenerator::EncodeChapterCode(48, false);
  EXPECT_EQ(code, 0x848DDDu);
  EXPECT_EQ(ChapterNumberGenerator::DecodeChapterNumber(code), 48);
}

}  // namespace
}  // namespace videosynth
