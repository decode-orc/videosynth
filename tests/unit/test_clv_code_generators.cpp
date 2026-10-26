/*
 * File:        test_clv_code_generators.cpp
 * Module:      clv_code_generator_tests
 * Purpose:     Unit tests for CLV LaserDisc biphase code generators per
 *              IEC 60856/60857.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <cstdint>

#include "videosynth/cav_code_generator.h"
#include "videosynth/clv_code_generator.h"
#include "videosynth/model.h"

namespace videosynth {
namespace {

// ---------------------------------------------------------------------------
// ClvCodeGeneratorTest
// ---------------------------------------------------------------------------

TEST(ClvCodeGeneratorTest, ClvCodeAlwaysReturnsFixedCode) {
  ClvCodeGenerator gen;
  EXPECT_EQ(gen.CurrentCode(), 0x87FFFFu);
}

TEST(ClvCodeGeneratorTest, ClvCodeMatchesConstant) {
  EXPECT_EQ(ClvCodeGenerator::kCode, 0x87FFFFu);
}

TEST(ClvCodeGeneratorTest, ClvCodeAdvanceDoesNotChangeCode) {
  ClvCodeGenerator gen;
  gen.Advance();
  gen.Advance();
  EXPECT_EQ(gen.CurrentCode(), 0x87FFFFu);
}

TEST(ClvCodeGeneratorTest, ClvCodeResetDoesNotChangeCode) {
  ClvCodeGenerator gen;
  gen.Reset();
  EXPECT_EQ(gen.CurrentCode(), 0x87FFFFu);
}

TEST(ClvCodeGeneratorTest, ClvCodeKeyNibbleIsValid) {
  // IEC 60856/60857: bit 23 of the 24-bit code must be 1.
  EXPECT_NE(ClvCodeGenerator::kCode & 0x800000u, 0u);
}

// ---------------------------------------------------------------------------
// ProgrammeTimeCodeGenerator — EncodeTimeCode
// ---------------------------------------------------------------------------

TEST(ClvCodeGeneratorTest, EncodeTimeCodeZeroZero) {
  // F + 0(hours) + DD + 0(tens) + 0(units) = 0xF0DD00
  EXPECT_EQ(ProgrammeTimeCodeGenerator::EncodeTimeCode(0, 0), 0xF0DD00u);
}

TEST(ClvCodeGeneratorTest, EncodeTimeCodeOneHour) {
  // hours=1, minutes=0: 0xF1DD00
  EXPECT_EQ(ProgrammeTimeCodeGenerator::EncodeTimeCode(1, 0), 0xF1DD00u);
}

TEST(ClvCodeGeneratorTest, EncodeTimeCodeMinutes59) {
  // hours=0, minutes=59: tens=5, units=9 → 0xF0DD59
  EXPECT_EQ(ProgrammeTimeCodeGenerator::EncodeTimeCode(0, 59), 0xF0DD59u);
}

TEST(ClvCodeGeneratorTest, EncodeTimeCodeHours15Minutes30) {
  // hours=15=0xF, minutes=30: tens=3, units=0 → 0xFFDD30
  EXPECT_EQ(ProgrammeTimeCodeGenerator::EncodeTimeCode(15, 30), 0xFFDD30u);
}

TEST(ClvCodeGeneratorTest, EncodeTimeCodeHours1Minutes30) {
  // hours=1, minutes=30: 0xF1DD30
  EXPECT_EQ(ProgrammeTimeCodeGenerator::EncodeTimeCode(1, 30), 0xF1DD30u);
}

TEST(ClvCodeGeneratorTest, EncodeTimeCodeFixedPatternDDIsPresent) {
  // Bits 15-8 must always be 0xDD for any valid hours/minutes.
  for (int h = 0; h <= 15; ++h) {
    for (int m : {0, 15, 30, 45, 59}) {
      const uint32_t code = ProgrammeTimeCodeGenerator::EncodeTimeCode(h, m);
      EXPECT_EQ((code >> 8) & 0xFFu, 0xDDu)
          << "hours=" << h << " minutes=" << m;
    }
  }
}

TEST(ClvCodeGeneratorTest, EncodeTimeCodeKeyNibbleIsF) {
  // Bits 23-20 must be 0xF (= 1111b), so bit 23 = 1.
  const uint32_t code = ProgrammeTimeCodeGenerator::EncodeTimeCode(5, 25);
  EXPECT_EQ((code >> 20) & 0xFu, 0xFu);
  EXPECT_NE(code & 0x800000u, 0u);
}

// ---------------------------------------------------------------------------
// ProgrammeTimeCodeGenerator — IsValidTimeCode
// ---------------------------------------------------------------------------

TEST(ClvCodeGeneratorTest, TimeCodeZeroZeroIsValid) {
  EXPECT_TRUE(ProgrammeTimeCodeGenerator::IsValidTimeCode(0, 0));
}

TEST(ClvCodeGeneratorTest, TimeCodeMaxHoursMaxMinutesIsValid) {
  EXPECT_TRUE(ProgrammeTimeCodeGenerator::IsValidTimeCode(15, 59));
}

TEST(ClvCodeGeneratorTest, TimeCodeNegativeHoursIsInvalid) {
  EXPECT_FALSE(ProgrammeTimeCodeGenerator::IsValidTimeCode(-1, 0));
}

TEST(ClvCodeGeneratorTest, TimeCodeHours16IsInvalid) {
  EXPECT_FALSE(ProgrammeTimeCodeGenerator::IsValidTimeCode(16, 0));
}

TEST(ClvCodeGeneratorTest, TimeCodeNegativeMinutesIsInvalid) {
  EXPECT_FALSE(ProgrammeTimeCodeGenerator::IsValidTimeCode(0, -1));
}

TEST(ClvCodeGeneratorTest, TimeCodeMinutes60IsInvalid) {
  EXPECT_FALSE(ProgrammeTimeCodeGenerator::IsValidTimeCode(0, 60));
}

// ---------------------------------------------------------------------------
// ProgrammeTimeCodeGenerator — FramesPerMinute
// ---------------------------------------------------------------------------

TEST(ClvCodeGeneratorTest, PalFramesPerMinuteIs1500) {
  EXPECT_EQ(ProgrammeTimeCodeGenerator::FramesPerMinute(Standard::kPal), 1500);
}

TEST(ClvCodeGeneratorTest, NtscFramesPerMinuteIs1800) {
  EXPECT_EQ(ProgrammeTimeCodeGenerator::FramesPerMinute(Standard::kNtsc), 1800);
}

TEST(ClvCodeGeneratorTest, PalMFramesPerMinuteIs1800) {
  // PAL-M is a System M (30 frame/s nominal) standard like NTSC.
  EXPECT_EQ(ProgrammeTimeCodeGenerator::FramesPerMinute(Standard::kPalM), 1800);
}

TEST(ClvCodeGeneratorTest, PalMClvPictureNumberCountsThirtyFramesPerSecond) {
  EXPECT_EQ(ClvPictureNumberGenerator::FramesPerSecond(Standard::kPalM), 30);
}

// ---------------------------------------------------------------------------
// ProgrammeTimeCodeGenerator — state machine (PAL)
// ---------------------------------------------------------------------------

TEST(ClvCodeGeneratorTest, TimeCodePalStartsAtGivenValues) {
  ProgrammeTimeCodeGenerator gen(0, 0, Standard::kPal);
  EXPECT_EQ(gen.current_hours(), 0);
  EXPECT_EQ(gen.current_minutes(), 0);
  EXPECT_EQ(gen.CurrentCode(), 0xF0DD00u);
}

TEST(ClvCodeGeneratorTest, TimeCodePalNoChangeBeforeMinuteBoundary) {
  ProgrammeTimeCodeGenerator gen(0, 0, Standard::kPal);
  // Advance 1499 frames — should still be at minute 0.
  for (int i = 0; i < 1499; ++i) {
    gen.Advance();
  }
  EXPECT_EQ(gen.current_hours(), 0);
  EXPECT_EQ(gen.current_minutes(), 0);
}

TEST(ClvCodeGeneratorTest, TimeCodePalMinuteIncrementAt1500Frames) {
  ProgrammeTimeCodeGenerator gen(0, 0, Standard::kPal);
  for (int i = 0; i < 1500; ++i) {
    gen.Advance();
  }
  EXPECT_EQ(gen.current_hours(), 0);
  EXPECT_EQ(gen.current_minutes(), 1);
  EXPECT_EQ(gen.CurrentCode(), 0xF0DD01u);
}

TEST(ClvCodeGeneratorTest, TimeCodePalTwoMinutes) {
  ProgrammeTimeCodeGenerator gen(0, 0, Standard::kPal);
  for (int i = 0; i < 3000; ++i) {
    gen.Advance();
  }
  EXPECT_EQ(gen.current_minutes(), 2);
}

TEST(ClvCodeGeneratorTest, TimeCodePalHourIncrementAt90000Frames) {
  ProgrammeTimeCodeGenerator gen(0, 0, Standard::kPal);
  for (int i = 0; i < 90000; ++i) {
    gen.Advance();
  }
  EXPECT_EQ(gen.current_hours(), 1);
  EXPECT_EQ(gen.current_minutes(), 0);
  EXPECT_EQ(gen.CurrentCode(), 0xF1DD00u);
}

TEST(ClvCodeGeneratorTest, TimeCodePalStartNonZero) {
  ProgrammeTimeCodeGenerator gen(2, 45, Standard::kPal);
  EXPECT_EQ(gen.current_hours(), 2);
  EXPECT_EQ(gen.current_minutes(), 45);
  EXPECT_EQ(gen.CurrentCode(), 0xF2DD45u);
}

TEST(ClvCodeGeneratorTest, TimeCodePalReset) {
  ProgrammeTimeCodeGenerator gen(0, 0, Standard::kPal);
  for (int i = 0; i < 3000; ++i) {
    gen.Advance();
  }
  EXPECT_EQ(gen.current_minutes(), 2);
  gen.Reset();
  EXPECT_EQ(gen.current_hours(), 0);
  EXPECT_EQ(gen.current_minutes(), 0);
  EXPECT_EQ(gen.total_frames(), 0);
  EXPECT_EQ(gen.CurrentCode(), 0xF0DD00u);
}

TEST(ClvCodeGeneratorTest, TimeCodePalMinutesRollOverAt60) {
  // At 60 minutes (90000 PAL frames from start 0:00), hours=1, minutes=0.
  ProgrammeTimeCodeGenerator gen(0, 58, Standard::kPal);
  // Advance 2 minutes to cross the hour boundary.
  for (int i = 0; i < 3000; ++i) {
    gen.Advance();
  }
  EXPECT_EQ(gen.current_hours(), 1);
  EXPECT_EQ(gen.current_minutes(), 0);
}

TEST(ClvCodeGeneratorTest, TimeCodeHoursSaturateAtMax) {
  // Start close to max: hours=15, minutes=59. After 1 more minute = hours
  // saturates.
  ProgrammeTimeCodeGenerator gen(15, 59, Standard::kPal);
  for (int i = 0; i < 1500; ++i) {
    gen.Advance();
  }
  // Would be 16:00 but saturates at 15:00.
  EXPECT_EQ(gen.current_hours(), 15);
}

// ---------------------------------------------------------------------------
// ProgrammeTimeCodeGenerator — state machine (NTSC)
// ---------------------------------------------------------------------------

TEST(ClvCodeGeneratorTest, TimeCodeNtscMinuteIncrementAt1800Frames) {
  ProgrammeTimeCodeGenerator gen(0, 0, Standard::kNtsc);
  for (int i = 0; i < 1800; ++i) {
    gen.Advance();
  }
  EXPECT_EQ(gen.current_hours(), 0);
  EXPECT_EQ(gen.current_minutes(), 1);
}

TEST(ClvCodeGeneratorTest, TimeCodeNtscNoChangeBefore1800) {
  ProgrammeTimeCodeGenerator gen(0, 0, Standard::kNtsc);
  for (int i = 0; i < 1799; ++i) {
    gen.Advance();
  }
  EXPECT_EQ(gen.current_minutes(), 0);
}

// ---------------------------------------------------------------------------
// ClvPictureNumberGenerator — EncodeClvPictureNumber
// ---------------------------------------------------------------------------

TEST(ClvCodeGeneratorTest, EncodeClvPictureNumberZeroZero) {
  // seconds=0: X₁=0xA, X₃=0; frame=0: X₄=0, X₅=0
  // Code = 0x800000 | (0xA<<16) | 0x00E000 | 0 | 0 = 0x8AE000
  EXPECT_EQ(ClvPictureNumberGenerator::EncodeClvPictureNumber(0, 0), 0x8AE000u);
}

TEST(ClvCodeGeneratorTest, EncodeClvPictureNumberSeconds10Frame0) {
  // seconds=10: X₁=0xA+1=0xB, X₃=0; frame=0
  // Code = 0x800000 | (0xB<<16) | 0x00E000 = 0x8BE000
  EXPECT_EQ(ClvPictureNumberGenerator::EncodeClvPictureNumber(10, 0),
            0x8BE000u);
}

TEST(ClvCodeGeneratorTest, EncodeClvPictureNumberSeconds59Frame29) {
  // seconds=59: X₁=0xA+5=0xF, X₃=9; frame=29: X₄=2, X₅=9
  // Code = 0x800000 | (0xF<<16) | 0x00E000 | (9<<8) | (2<<4) | 9
  //      = 0x8F0000 | 0x00E000 | 0x000900 | 0x20 | 0x9 = 0x8FE929
  EXPECT_EQ(ClvPictureNumberGenerator::EncodeClvPictureNumber(59, 29),
            0x8FE929u);
}

TEST(ClvCodeGeneratorTest, EncodeClvPictureNumberSeconds30Frame15) {
  // seconds=30: X₁=0xA+3=0xD, X₃=0; frame=15: X₄=1, X₅=5
  // Code = 0x800000 | (0xD<<16) | 0x00E000 | (0<<8) | (1<<4) | 5
  //      = 0x8DE015
  EXPECT_EQ(ClvPictureNumberGenerator::EncodeClvPictureNumber(30, 15),
            0x8DE015u);
}

TEST(ClvCodeGeneratorTest, EncodeClvPictureNumberFixedENibblePresent) {
  // Bits 15-12 must always be 0xE.
  for (int s : {0, 9, 15, 30, 45, 59}) {
    for (int f : {0, 10, 20, 29}) {
      const uint32_t code =
          ClvPictureNumberGenerator::EncodeClvPictureNumber(s, f);
      EXPECT_EQ((code >> 12) & 0xFu, 0xEu) << "seconds=" << s << " frame=" << f;
    }
  }
}

TEST(ClvCodeGeneratorTest, EncodeClvPictureNumberKeyNibbleIs8) {
  // Bits 23-20 must be 0x8, so bit 23 = 1.
  const uint32_t code =
      ClvPictureNumberGenerator::EncodeClvPictureNumber(15, 7);
  EXPECT_EQ((code >> 20) & 0xFu, 0x8u);
  EXPECT_NE(code & 0x800000u, 0u);
}

TEST(ClvCodeGeneratorTest, EncodeClvPictureNumberX1RangeAtoF) {
  // X₁ must be 0xA for seconds 0-9, 0xB for 10-19, ..., 0xF for 50-59.
  for (int s = 0; s <= 59; ++s) {
    const uint32_t code =
        ClvPictureNumberGenerator::EncodeClvPictureNumber(s, 0);
    const uint32_t x1 = (code >> 16) & 0xFu;
    EXPECT_GE(x1, 0xAu) << "seconds=" << s;
    EXPECT_LE(x1, 0xFu) << "seconds=" << s;
    EXPECT_EQ(x1, 0xAu + static_cast<uint32_t>(s / 10)) << "seconds=" << s;
  }
}

// ---------------------------------------------------------------------------
// ClvPictureNumberGenerator — IsValidClvPictureNumber
// ---------------------------------------------------------------------------

TEST(ClvCodeGeneratorTest, ClvPictureNumberZeroZeroIsValid) {
  EXPECT_TRUE(ClvPictureNumberGenerator::IsValidClvPictureNumber(0, 0));
}

TEST(ClvCodeGeneratorTest, ClvPictureNumberMaxValuesAreValid) {
  EXPECT_TRUE(ClvPictureNumberGenerator::IsValidClvPictureNumber(59, 29));
}

TEST(ClvCodeGeneratorTest, ClvPictureNumberNegativeSecondsIsInvalid) {
  EXPECT_FALSE(ClvPictureNumberGenerator::IsValidClvPictureNumber(-1, 0));
}

TEST(ClvCodeGeneratorTest, ClvPictureNumberSeconds60IsInvalid) {
  EXPECT_FALSE(ClvPictureNumberGenerator::IsValidClvPictureNumber(60, 0));
}

TEST(ClvCodeGeneratorTest, ClvPictureNumberNegativeFrameIsInvalid) {
  EXPECT_FALSE(ClvPictureNumberGenerator::IsValidClvPictureNumber(0, -1));
}

TEST(ClvCodeGeneratorTest, ClvPictureNumberFrame30IsInvalid) {
  EXPECT_FALSE(ClvPictureNumberGenerator::IsValidClvPictureNumber(0, 30));
}

// ---------------------------------------------------------------------------
// ClvPictureNumberGenerator — FramesPerSecond
// ---------------------------------------------------------------------------

TEST(ClvCodeGeneratorTest, PalFramesPerSecondIs25) {
  EXPECT_EQ(ClvPictureNumberGenerator::FramesPerSecond(Standard::kPal), 25);
}

TEST(ClvCodeGeneratorTest, NtscFramesPerSecondIs30) {
  EXPECT_EQ(ClvPictureNumberGenerator::FramesPerSecond(Standard::kNtsc), 30);
}

// ---------------------------------------------------------------------------
// ClvPictureNumberGenerator — IsNtscCorrectionPoint
// ---------------------------------------------------------------------------

TEST(ClvCodeGeneratorTest, NtscCorrectionPointZeroIsFalse) {
  EXPECT_FALSE(ClvPictureNumberGenerator::IsNtscCorrectionPoint(0));
}

TEST(ClvCodeGeneratorTest, NtscCorrectionPointNegativeIsFalse) {
  EXPECT_FALSE(ClvPictureNumberGenerator::IsNtscCorrectionPoint(-1));
}

TEST(ClvCodeGeneratorTest, NtscCorrectionPoint899IsTrue) {
  // L=0, M=1: 8991*0 + 899*1 = 899
  EXPECT_TRUE(ClvPictureNumberGenerator::IsNtscCorrectionPoint(899));
}

TEST(ClvCodeGeneratorTest, NtscCorrectionPoint1798IsTrue) {
  // L=0, M=2: 899*2 = 1798
  EXPECT_TRUE(ClvPictureNumberGenerator::IsNtscCorrectionPoint(1798));
}

TEST(ClvCodeGeneratorTest, NtscCorrectionPoint8091IsTrue) {
  // L=0, M=9: 899*9 = 8091
  EXPECT_TRUE(ClvPictureNumberGenerator::IsNtscCorrectionPoint(8091));
}

TEST(ClvCodeGeneratorTest, NtscCorrectionPoint8991IsTrue) {
  // L=1, M=0: 8991*1 + 899*0 = 8991
  EXPECT_TRUE(ClvPictureNumberGenerator::IsNtscCorrectionPoint(8991));
}

TEST(ClvCodeGeneratorTest, NtscCorrectionPoint9890IsTrue) {
  // L=1, M=1: 8991*1 + 899*1 = 9890
  EXPECT_TRUE(ClvPictureNumberGenerator::IsNtscCorrectionPoint(9890));
}

TEST(ClvCodeGeneratorTest, NtscCorrectionPoint17982IsTrue) {
  // L=2, M=0: 8991*2 = 17982
  EXPECT_TRUE(ClvPictureNumberGenerator::IsNtscCorrectionPoint(17982));
}

TEST(ClvCodeGeneratorTest, NtscCorrectionPoint900IsFalse) {
  // 900 is not in the sequence.
  EXPECT_FALSE(ClvPictureNumberGenerator::IsNtscCorrectionPoint(900));
}

TEST(ClvCodeGeneratorTest, NtscCorrectionPoint1IsFalse) {
  EXPECT_FALSE(ClvPictureNumberGenerator::IsNtscCorrectionPoint(1));
}

TEST(ClvCodeGeneratorTest, NtscCorrectionPointAllFirstNinePoints) {
  // First 9 correction points: multiples of 899 from 1*899 to 9*899.
  const int expected[] = {899, 1798, 2697, 3596, 4495, 5394, 6293, 7192, 8091};
  for (int n : expected) {
    EXPECT_TRUE(ClvPictureNumberGenerator::IsNtscCorrectionPoint(n))
        << "N=" << n;
  }
}

// ---------------------------------------------------------------------------
// ClvPictureNumberGenerator — state machine (PAL)
// ---------------------------------------------------------------------------

TEST(ClvCodeGeneratorTest, ClvPictureNumberPalStartsAtZero) {
  ClvPictureNumberGenerator gen(Standard::kPal);
  EXPECT_EQ(gen.current_seconds(), 0);
  EXPECT_EQ(gen.current_frame(), 0);
  EXPECT_EQ(gen.total_frames(), 0);
  EXPECT_EQ(gen.CurrentCode(), 0x8AE000u);
}

TEST(ClvCodeGeneratorTest, ClvPictureNumberPalFrameAdvances) {
  ClvPictureNumberGenerator gen(Standard::kPal);
  gen.Advance();
  EXPECT_EQ(gen.current_seconds(), 0);
  EXPECT_EQ(gen.current_frame(), 1);
  EXPECT_EQ(gen.total_frames(), 1);
}

TEST(ClvCodeGeneratorTest, ClvPictureNumberPalSecondRolloverAt25Frames) {
  ClvPictureNumberGenerator gen(Standard::kPal);
  for (int i = 0; i < 25; ++i) {
    gen.Advance();
  }
  EXPECT_EQ(gen.current_seconds(), 1);
  EXPECT_EQ(gen.current_frame(), 0);
}

TEST(ClvCodeGeneratorTest, ClvPictureNumberPalNoRolloverAt24Frames) {
  ClvPictureNumberGenerator gen(Standard::kPal);
  for (int i = 0; i < 24; ++i) {
    gen.Advance();
  }
  EXPECT_EQ(gen.current_seconds(), 0);
  EXPECT_EQ(gen.current_frame(), 24);
}

TEST(ClvCodeGeneratorTest, ClvPictureNumberPalMinuteRollover) {
  // After 25 * 60 = 1500 frames, seconds rolls from 59 back to 0.
  ClvPictureNumberGenerator gen(Standard::kPal);
  for (int i = 0; i < 1500; ++i) {
    gen.Advance();
  }
  EXPECT_EQ(gen.current_seconds(), 0);
  EXPECT_EQ(gen.current_frame(), 0);
}

TEST(ClvCodeGeneratorTest, ClvPictureNumberPalSecondsAt59) {
  ClvPictureNumberGenerator gen(Standard::kPal);
  // 59 seconds = 59 * 25 = 1475 frames.
  for (int i = 0; i < 1475; ++i) {
    gen.Advance();
  }
  EXPECT_EQ(gen.current_seconds(), 59);
  EXPECT_EQ(gen.current_frame(), 0);
}

TEST(ClvCodeGeneratorTest, ClvPictureNumberPalReset) {
  ClvPictureNumberGenerator gen(Standard::kPal);
  for (int i = 0; i < 50; ++i) {
    gen.Advance();
  }
  EXPECT_EQ(gen.current_seconds(), 2);
  gen.Reset();
  EXPECT_EQ(gen.current_seconds(), 0);
  EXPECT_EQ(gen.current_frame(), 0);
  EXPECT_EQ(gen.total_frames(), 0);
  EXPECT_EQ(gen.CurrentCode(), 0x8AE000u);
}

// ---------------------------------------------------------------------------
// ClvPictureNumberGenerator — state machine (NTSC, no correction expected)
// ---------------------------------------------------------------------------

TEST(ClvCodeGeneratorTest, ClvPictureNumberNtscStartsAtZero) {
  ClvPictureNumberGenerator gen(Standard::kNtsc);
  EXPECT_EQ(gen.current_seconds(), 0);
  EXPECT_EQ(gen.current_frame(), 0);
}

TEST(ClvCodeGeneratorTest, ClvPictureNumberNtscSecondRolloverAt30Frames) {
  ClvPictureNumberGenerator gen(Standard::kNtsc);
  for (int i = 0; i < 30; ++i) {
    gen.Advance();
  }
  EXPECT_EQ(gen.current_seconds(), 1);
  EXPECT_EQ(gen.current_frame(), 0);
}

TEST(ClvCodeGeneratorTest, ClvPictureNumberNtscNoRolloverAt29Frames) {
  ClvPictureNumberGenerator gen(Standard::kNtsc);
  for (int i = 0; i < 29; ++i) {
    gen.Advance();
  }
  EXPECT_EQ(gen.current_seconds(), 0);
  EXPECT_EQ(gen.current_frame(), 29);
}

// ---------------------------------------------------------------------------
// ClvPictureNumberGenerator — NTSC colour time error correction
// ---------------------------------------------------------------------------

TEST(ClvCodeGeneratorTest, NtscCorrectionAt899SecondJumpsForward) {
  // At frame 899 (correction point L=0, M=1):
  // Without correction: seconds = 899/30 = 29, frame = 899%30 = 29
  // With correction at frame 899: extra seconds++, frame=0
  // State before frame 899: advance 898 frames normally
  //   898/30 = 29 remainder 28 → seconds=29, frame=28
  // At frame 899 (correction): seconds = 29+1 = 30, frame = 0
  ClvPictureNumberGenerator gen(Standard::kNtsc);
  for (int i = 0; i < 899; ++i) {
    gen.Advance();
  }
  EXPECT_EQ(gen.current_seconds(), 30);
  EXPECT_EQ(gen.current_frame(), 0);
  EXPECT_EQ(gen.total_frames(), 899);
}

TEST(ClvCodeGeneratorTest, NtscCorrectionAt1798SecondJumpsAgain) {
  // At frame 1798 (correction point L=0, M=2):
  // From frame 899 state (seconds=30, frame=0), advance 899 more frames:
  //   899 frames = 29 seconds + 29 frames → seconds=30+29=59, frame=29
  // But frame 1798 is a correction: seconds=59+1=60→0, frame=0
  ClvPictureNumberGenerator gen(Standard::kNtsc);
  for (int i = 0; i < 1798; ++i) {
    gen.Advance();
  }
  EXPECT_EQ(gen.current_seconds(), 0);
  EXPECT_EQ(gen.current_frame(), 0);
  EXPECT_EQ(gen.total_frames(), 1798);
}

TEST(ClvCodeGeneratorTest, NtscCorrectionNot898Or900) {
  // Frame 898 (one before correction): normal advance from frame 897.
  //   897/30 = 29 remainder 27 → seconds=29, frame=27
  //   898: frame=28 (not a correction point)
  ClvPictureNumberGenerator gen(Standard::kNtsc);
  for (int i = 0; i < 898; ++i) {
    gen.Advance();
  }
  EXPECT_EQ(gen.current_seconds(), 29);
  EXPECT_EQ(gen.current_frame(), 28);
}

TEST(ClvCodeGeneratorTest, NtscCorrectionAt8991) {
  // Frame 8991 (L=1, M=0) is a correction point.
  // Verify the total_frames count is correct and a correction occurred.
  // Counting how many corrections happen before 8991:
  //   Corrections at: 899, 1798, 2697, 3596, 4495, 5394, 6293, 7192, 8091 (9
  //   corrections) Plus the correction at 8991 itself = 10th correction
  // Each correction adds 1 to seconds beyond the nominal count.
  // Nominal (no correction) at frame 8991: seconds = 8991/30 = 299 remainder 21
  //   So seconds (modulo 60) = 299%60 = 59, frame = 21
  // With 10 corrections: seconds += 10 extra steps
  //   Total seconds = 299 + 10 = 309, seconds%60 = 9, frame=0 (correction at
  //   8991)
  ClvPictureNumberGenerator gen(Standard::kNtsc);
  for (int i = 0; i < 8991; ++i) {
    gen.Advance();
  }
  EXPECT_EQ(gen.total_frames(), 8991);
  // The correction at 8991 resets the frame to 0.
  EXPECT_EQ(gen.current_frame(), 0);
}

TEST(ClvCodeGeneratorTest, NtscNoCorrectionForPalGenerator) {
  // PAL generator must NOT apply NTSC correction, even at correction point
  // frames.
  ClvPictureNumberGenerator pal_gen(Standard::kPal);
  for (int i = 0; i < 899; ++i) {
    pal_gen.Advance();
  }
  // PAL: 899 frames → seconds = 899/25 = 35 remainder 24 → seconds=35, frame=24
  EXPECT_EQ(pal_gen.current_seconds(), 35);
  EXPECT_EQ(pal_gen.current_frame(), 24);
}

// ---------------------------------------------------------------------------
// ChapterNumberGenerator — minimum chapter tracks constant (task 3.11)
// ---------------------------------------------------------------------------

TEST(ClvCodeGeneratorTest, MinimumChapterTracksConstantIs30) {
  // IEC 60856/60857 §10.1.5: minimum chapter length is 30 tracks.
  EXPECT_EQ(ChapterNumberGenerator::kMinimumChapterTracks, 30);
}

// ---------------------------------------------------------------------------
// ClvTimecodeForFrame — continuous {timecode} OSD counter
// ---------------------------------------------------------------------------

TEST(ClvTimecodeForFrameTest, FirstFrameIsZero) {
  const ClvTimecode tc = ClvTimecodeForFrame(0, Standard::kPal);
  EXPECT_EQ(tc.hours, 0);
  EXPECT_EQ(tc.minutes, 0);
  EXPECT_EQ(tc.seconds, 0);
  EXPECT_EQ(tc.frames, 0);
}

TEST(ClvTimecodeForFrameTest, PalFrameFieldWrapsAt25) {
  // PAL: 25 fps. Frame 24 → :24; frame 25 → 1 second, frame 0.
  EXPECT_EQ(ClvTimecodeForFrame(24, Standard::kPal).frames, 24);
  EXPECT_EQ(ClvTimecodeForFrame(24, Standard::kPal).seconds, 0);
  EXPECT_EQ(ClvTimecodeForFrame(25, Standard::kPal).seconds, 1);
  EXPECT_EQ(ClvTimecodeForFrame(25, Standard::kPal).frames, 0);
}

TEST(ClvTimecodeForFrameTest, NtscFrameFieldWrapsAt30) {
  // NTSC: 30 fps nominal.
  EXPECT_EQ(ClvTimecodeForFrame(29, Standard::kNtsc).frames, 29);
  EXPECT_EQ(ClvTimecodeForFrame(29, Standard::kNtsc).seconds, 0);
  EXPECT_EQ(ClvTimecodeForFrame(30, Standard::kNtsc).seconds, 1);
  EXPECT_EQ(ClvTimecodeForFrame(30, Standard::kNtsc).frames, 0);
}

TEST(ClvTimecodeForFrameTest, PalMinutesAndHoursCarry) {
  // PAL: 25 fps → 1500 frames/minute, 90000 frames/hour.
  const ClvTimecode one_min = ClvTimecodeForFrame(1500, Standard::kPal);
  EXPECT_EQ(one_min.minutes, 1);
  EXPECT_EQ(one_min.seconds, 0);
  EXPECT_EQ(one_min.frames, 0);

  // 1 h 23 m 45 s 12 f = (5025 s) * 25 + 12 = 125637.
  const ClvTimecode mixed = ClvTimecodeForFrame(125637, Standard::kPal);
  EXPECT_EQ(mixed.hours, 1);
  EXPECT_EQ(mixed.minutes, 23);
  EXPECT_EQ(mixed.seconds, 45);
  EXPECT_EQ(mixed.frames, 12);
}

}  // namespace
}  // namespace videosynth
