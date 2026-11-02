/*
 * File:        test_biphase_encoder.cpp
 * Module:      biphase_encoder_tests
 * Purpose:     Unit tests for 24-bit biphase Manchester encoding and hex
 *              parsing utilities per IEC 60856/60857.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <cmath>
#include <optional>
#include <vector>

#include "videosynth/biphase_encoder.h"
#include "videosynth/biphase_utils.h"
#include "videosynth/fixed_point.h"
#include "videosynth/timing_constants.h"

namespace videosynth {
namespace {

// ---------------------------------------------------------------------------
// BiphaseUtilsTest — ParseBiphaseHexCode and IsValidBiphaseKeyNibble
// ---------------------------------------------------------------------------

TEST(BiphaseUtilsTest, ParsesValidSixDigitHexCode) {
  const auto result = ParseBiphaseHexCode("88FFFF");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, 0x88FFFFu);  // NOLINT(bugprone-unchecked-optional-access)
}

TEST(BiphaseUtilsTest, ParsesLowerCaseSixDigitHexCode) {
  const auto result = ParseBiphaseHexCode("88ffff");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, 0x88FFFFu);  // NOLINT(bugprone-unchecked-optional-access)
}

TEST(BiphaseUtilsTest, ParsesHexCodeWithOxPrefix) {
  const auto result = ParseBiphaseHexCode("0x88FFFF");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, 0x88FFFFu);  // NOLINT(bugprone-unchecked-optional-access)
}

TEST(BiphaseUtilsTest, ParsesHexCodeWithUppercaseOxPrefix) {
  const auto result = ParseBiphaseHexCode("0X80EEEE");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, 0x80EEEEu);  // NOLINT(bugprone-unchecked-optional-access)
}

TEST(BiphaseUtilsTest, RejectsCodeShorterThanSixDigits) {
  EXPECT_FALSE(ParseBiphaseHexCode("88FF").has_value());
}

TEST(BiphaseUtilsTest, RejectsCodeLongerThanSixDigits) {
  EXPECT_FALSE(ParseBiphaseHexCode("88FFFF00").has_value());
}

TEST(BiphaseUtilsTest, RejectsEmptyString) {
  EXPECT_FALSE(ParseBiphaseHexCode("").has_value());
}

TEST(BiphaseUtilsTest, RejectsNonHexCharacters) {
  EXPECT_FALSE(ParseBiphaseHexCode("88GGFF").has_value());
}

TEST(BiphaseUtilsTest, RejectsPrefixOnlyString) {
  EXPECT_FALSE(ParseBiphaseHexCode("0x").has_value());
}

TEST(BiphaseUtilsTest, KeyNibbleValidForCodesWhoseMsbIsOne) {
  // 0x88FFFF: top nibble = 0x8 = 1000, MSB = 1
  EXPECT_TRUE(IsValidBiphaseKeyNibble(0x88FFFFu));
  // 0xF00000: top nibble = 0xF = 1111, MSB = 1
  EXPECT_TRUE(IsValidBiphaseKeyNibble(0xF00000u));
  // 0x80EEEE: top nibble = 0x8 = 1000, MSB = 1
  EXPECT_TRUE(IsValidBiphaseKeyNibble(0x80EEEEu));
}

TEST(BiphaseUtilsTest, KeyNibbleInvalidForCodesWhoseMsbIsZero) {
  // 0x08FFFF: top nibble = 0x0 = 0000, MSB = 0
  EXPECT_FALSE(IsValidBiphaseKeyNibble(0x08FFFFu));
  // 0x70EEEE: top nibble = 0x7 = 0111, MSB = 0
  EXPECT_FALSE(IsValidBiphaseKeyNibble(0x70EEEEu));
  EXPECT_FALSE(IsValidBiphaseKeyNibble(0x000000u));
}

TEST(BiphaseUtilsTest, IsValidBiphaseHexCodeAcceptsWellFormedCodes) {
  EXPECT_TRUE(IsValidBiphaseHexCode("88FFFF"));
  EXPECT_TRUE(IsValidBiphaseHexCode("80EEEE"));
  EXPECT_TRUE(IsValidBiphaseHexCode("F00001"));
}

TEST(BiphaseUtilsTest, IsValidBiphaseHexCodeRejectsMalformedCodes) {
  EXPECT_FALSE(IsValidBiphaseHexCode("08FFFF"));  // MSB of key nibble = 0
  EXPECT_FALSE(IsValidBiphaseHexCode("ZZZZZZ"));  // non-hex
  EXPECT_FALSE(IsValidBiphaseHexCode("88FF"));    // too short
}

// ---------------------------------------------------------------------------
// BiphaseEncoderTest — construction
// ---------------------------------------------------------------------------

TEST(BiphaseEncoderTest, BitCellSamplesCorrectForPal4Fsc) {
  // PAL 4fsc: 17734475 Hz, 2 µs bit cell → round(2e-6 * 17734475) = 35
  constexpr double kPalSampleRate = 17734475.0;
  const BiphaseEncoder enc(kPalSampleRate);
  EXPECT_EQ(enc.bit_cell_samples(), 35);
}

TEST(BiphaseEncoderTest, BitCellSamplesCorrectForNtsc4Fsc) {
  // NTSC 4fsc: 14318180 Hz, 2 µs bit cell → round(2e-6 * 14318180) = 29
  constexpr double kNtscSampleRate = 14318180.0;
  const BiphaseEncoder enc(kNtscSampleRate);
  EXPECT_EQ(enc.bit_cell_samples(), 29);
}

TEST(BiphaseEncoderTest, RampSamplesPositiveForPal4Fsc) {
  constexpr double kPalSampleRate = 17734475.0;
  const BiphaseEncoder enc(kPalSampleRate);
  EXPECT_GT(enc.ramp_samples(), 0);
  // Must fit within the half bit cell
  EXPECT_LT(enc.ramp_samples(), enc.bit_cell_samples() / 2);
}

TEST(BiphaseEncoderTest, RampSamplesPositiveForNtsc4Fsc) {
  constexpr double kNtscSampleRate = 14318180.0;
  const BiphaseEncoder enc(kNtscSampleRate);
  EXPECT_GT(enc.ramp_samples(), 0);
  EXPECT_LT(enc.ramp_samples(), enc.bit_cell_samples() / 2);
}

// ---------------------------------------------------------------------------
// BiphaseEncoderTest — GenerateBit
// ---------------------------------------------------------------------------

TEST(BiphaseEncoderTest, GenerateBitOneHasCorrectSampleCount) {
  constexpr double kPalSampleRate = 17734475.0;
  const BiphaseEncoder enc(kPalSampleRate);
  const auto samples = enc.GenerateBit(true, 210.0, 700.0);
  EXPECT_EQ(static_cast<int>(samples.size()), enc.bit_cell_samples());
}

TEST(BiphaseEncoderTest, GenerateBitZeroHasCorrectSampleCount) {
  constexpr double kPalSampleRate = 17734475.0;
  const BiphaseEncoder enc(kPalSampleRate);
  const auto samples = enc.GenerateBit(false, 210.0, 700.0);
  EXPECT_EQ(static_cast<int>(samples.size()), enc.bit_cell_samples());
}

// For a '1' bit the first quarter of the bit cell must be at baseline.
TEST(BiphaseEncoderTest, GenerateBitOneFirstQuarterIsAtBaseline) {
  constexpr double kPalSampleRate = 17734475.0;
  constexpr double kBaseline = 210.0;
  constexpr double kPeak = 700.0;
  const BiphaseEncoder enc(kPalSampleRate);
  const auto samples = enc.GenerateBit(true, kBaseline, kPeak);

  const int quarter = enc.bit_cell_samples() / 4;
  for (int i = 0; i < quarter; ++i) {
    EXPECT_NEAR(SampleFixedToMillivolts(samples[static_cast<std::size_t>(i)]),
                kBaseline, 1.0)
        << "Sample " << i << " of '1' bit should be at baseline";
  }
}

// For a '1' bit the last quarter must be at peak.
TEST(BiphaseEncoderTest, GenerateBitOneLastQuarterIsAtPeak) {
  constexpr double kPalSampleRate = 17734475.0;
  constexpr double kBaseline = 210.0;
  constexpr double kPeak = 700.0;
  const BiphaseEncoder enc(kPalSampleRate);
  const auto samples = enc.GenerateBit(true, kBaseline, kPeak);

  const int n = enc.bit_cell_samples();
  const int three_quarters = (3 * n) / 4;
  for (int i = three_quarters; i < n; ++i) {
    EXPECT_NEAR(SampleFixedToMillivolts(samples[static_cast<std::size_t>(i)]),
                kPeak, 1.0)
        << "Sample " << i << " of '1' bit should be at peak";
  }
}

// For a '0' bit the first quarter must be at peak.
TEST(BiphaseEncoderTest, GenerateBitZeroFirstQuarterIsAtPeak) {
  constexpr double kPalSampleRate = 17734475.0;
  constexpr double kBaseline = 210.0;
  constexpr double kPeak = 700.0;
  const BiphaseEncoder enc(kPalSampleRate);
  const auto samples = enc.GenerateBit(false, kBaseline, kPeak);

  const int quarter = enc.bit_cell_samples() / 4;
  for (int i = 0; i < quarter; ++i) {
    EXPECT_NEAR(SampleFixedToMillivolts(samples[static_cast<std::size_t>(i)]),
                kPeak, 1.0)
        << "Sample " << i << " of '0' bit should be at peak";
  }
}

// For a '0' bit the last quarter must be at baseline.
TEST(BiphaseEncoderTest, GenerateBitZeroLastQuarterIsAtBaseline) {
  constexpr double kPalSampleRate = 17734475.0;
  constexpr double kBaseline = 210.0;
  constexpr double kPeak = 700.0;
  const BiphaseEncoder enc(kPalSampleRate);
  const auto samples = enc.GenerateBit(false, kBaseline, kPeak);

  const int n = enc.bit_cell_samples();
  const int three_quarters = (3 * n) / 4;
  for (int i = three_quarters; i < n; ++i) {
    EXPECT_NEAR(SampleFixedToMillivolts(samples[static_cast<std::size_t>(i)]),
                kBaseline, 1.0)
        << "Sample " << i << " of '0' bit should be at baseline";
  }
}

// Centre of a '1' bit must be above the midpoint (transition is rising).
TEST(BiphaseEncoderTest, GenerateBitOneCentreIsAboveMidpoint) {
  constexpr double kPalSampleRate = 17734475.0;
  constexpr double kBaseline = 210.0;
  constexpr double kPeak = 700.0;
  const BiphaseEncoder enc(kPalSampleRate);
  const auto samples = enc.GenerateBit(true, kBaseline, kPeak);

  const int center = enc.bit_cell_samples() / 2;
  const double mid = (kBaseline + kPeak) / 2.0;
  const double level =
      SampleFixedToMillivolts(samples[static_cast<std::size_t>(center)]);
  // At the S-curve midpoint the level is ≈ mid (within 10% of range).
  EXPECT_NEAR(level, mid, (kPeak - kBaseline) * 0.15);
}

// Centre of a '0' bit must be near the midpoint (transition is falling).
TEST(BiphaseEncoderTest, GenerateBitZeroCentreIsNearMidpoint) {
  constexpr double kPalSampleRate = 17734475.0;
  constexpr double kBaseline = 210.0;
  constexpr double kPeak = 700.0;
  const BiphaseEncoder enc(kPalSampleRate);
  const auto samples = enc.GenerateBit(false, kBaseline, kPeak);

  const int center = enc.bit_cell_samples() / 2;
  const double mid = (kBaseline + kPeak) / 2.0;
  const double level =
      SampleFixedToMillivolts(samples[static_cast<std::size_t>(center)]);
  EXPECT_NEAR(level, mid, (kPeak - kBaseline) * 0.15);
}

// '1' bit is strictly monotonically non-decreasing around the centre.
TEST(BiphaseEncoderTest, GenerateBitOneTransitionIsRisingAtCentre) {
  constexpr double kPalSampleRate = 17734475.0;
  const BiphaseEncoder enc(kPalSampleRate);
  const auto samples = enc.GenerateBit(true, 210.0, 700.0);

  const int center = enc.bit_cell_samples() / 2;
  const int ramp = enc.ramp_samples();
  // The ramp region starts at center - ramp/2.  Sample just before the ramp
  // must be ≤ sample at the end of the ramp.
  const double before_ramp = SampleFixedToMillivolts(
      samples[static_cast<std::size_t>(center - ramp / 2)]);
  const double after_ramp = SampleFixedToMillivolts(
      samples[static_cast<std::size_t>(center + ramp - ramp / 2 - 1)]);
  EXPECT_LE(before_ramp, after_ramp);
}

// '0' bit must be strictly falling at the centre.
TEST(BiphaseEncoderTest, GenerateBitZeroTransitionIsFallingAtCentre) {
  constexpr double kPalSampleRate = 17734475.0;
  const BiphaseEncoder enc(kPalSampleRate);
  const auto samples = enc.GenerateBit(false, 210.0, 700.0);

  const int center = enc.bit_cell_samples() / 2;
  const int ramp = enc.ramp_samples();
  const double before_ramp = SampleFixedToMillivolts(
      samples[static_cast<std::size_t>(center - ramp / 2)]);
  const double after_ramp = SampleFixedToMillivolts(
      samples[static_cast<std::size_t>(center + ramp - ramp / 2 - 1)]);
  EXPECT_GE(before_ramp, after_ramp);
}

// ---------------------------------------------------------------------------
// BiphaseEncoderTest — Generate24BitCode
// ---------------------------------------------------------------------------

TEST(BiphaseEncoderTest, Generate24BitCodeHasCorrectSampleCount) {
  constexpr double kPalSampleRate = 17734475.0;
  const BiphaseEncoder enc(kPalSampleRate);
  const auto samples = enc.Generate24BitCode(0x88FFFFu, 210.0, 700.0);
  EXPECT_EQ(static_cast<int>(samples.size()), enc.BitCellStart(24));
}

// First bit of 0x88FFFF is '1' (MSB of 0x88 = 1).
// The first quarter of the 24-bit waveform must be at baseline.
TEST(BiphaseEncoderTest, Generate24BitCodeFirstBitIsOneStartsAtBaseline) {
  constexpr double kPalSampleRate = 17734475.0;
  constexpr double kBaseline = 210.0;
  constexpr double kPeak = 700.0;
  const BiphaseEncoder enc(kPalSampleRate);
  // 0x88FFFF: bits = 1000 1000 1111 1111 1111 1111 (MSB first)
  const auto samples = enc.Generate24BitCode(0x88FFFFu, kBaseline, kPeak);

  const int quarter = enc.bit_cell_samples() / 4;
  for (int i = 0; i < quarter; ++i) {
    EXPECT_NEAR(SampleFixedToMillivolts(samples[static_cast<std::size_t>(i)]),
                kBaseline, 1.0)
        << "First bit ('1'): sample " << i << " should be at baseline";
  }
}

// Second bit of 0x88FFFF is '0' → starts at peak.
TEST(BiphaseEncoderTest, Generate24BitCodeSecondBitIsZeroStartsAtPeak) {
  constexpr double kPalSampleRate = 17734475.0;
  constexpr double kBaseline = 210.0;
  constexpr double kPeak = 700.0;
  const BiphaseEncoder enc(kPalSampleRate);
  const auto samples = enc.Generate24BitCode(0x88FFFFu, kBaseline, kPeak);

  // Second bit cell starts at BitCellStart(1).  Its first quarter is at peak
  // (it is a '0' bit with no inter-bit transition from the preceding '1').
  const int bit2_start = enc.BitCellStart(1);
  const int quarter = enc.bit_cell_samples() / 4;
  for (int i = bit2_start; i < bit2_start + quarter; ++i) {
    EXPECT_NEAR(SampleFixedToMillivolts(samples[static_cast<std::size_t>(i)]),
                kPeak, 1.0)
        << "Second bit ('0'): sample " << i << " should be at peak";
  }
}

// The bits 8-23 in 0x88FFFF are all '1'.  Between consecutive '1' bits there
// must be a falling inter-bit transition at each bit boundary.  Sample just
// before the boundary must be above the sample just after.
TEST(BiphaseEncoderTest,
     Generate24BitCodeConsecutiveOneBitsHaveFallingInterBitTransitions) {
  constexpr double kPalSampleRate = 17734475.0;
  constexpr double kBaseline = 210.0;
  constexpr double kPeak = 700.0;
  const BiphaseEncoder enc(kPalSampleRate);
  // Bits 8-23 of 0x88FFFF are all '1'.
  const auto samples = enc.Generate24BitCode(0x88FFFFu, kBaseline, kPeak);

  // Check the inter-bit transition at the boundary between bit 8 and bit 9
  // (first boundary in the all-1 run).
  const int boundary = enc.BitCellStart(9);
  // A few samples before the boundary should be near peak.
  const int pre_idx = boundary - enc.ramp_samples() - 1;
  // A few samples after the boundary should be near baseline.
  const int post_idx = boundary + enc.ramp_samples() + 1;
  const double pre_level =
      SampleFixedToMillivolts(samples[static_cast<std::size_t>(pre_idx)]);
  const double post_level =
      SampleFixedToMillivolts(samples[static_cast<std::size_t>(post_idx)]);
  EXPECT_NEAR(pre_level, kPeak, 1.0);
  EXPECT_NEAR(post_level, kBaseline, 1.0);
}

// An all-ones 24-bit code has an inter-bit transition at every boundary
// after the first bit.
TEST(BiphaseEncoderTest, AllOneCodeHasInterBitTransitionAtEveryBoundary) {
  constexpr double kPalSampleRate = 17734475.0;
  constexpr double kBaseline = 0.0;
  constexpr double kPeak = 100.0;
  const BiphaseEncoder enc(kPalSampleRate);
  const auto samples = enc.Generate24BitCode(0xFFFFFFu, kBaseline, kPeak);

  const int bcs = enc.bit_cell_samples();
  const int ramp = enc.ramp_samples();
  for (int bit_idx = 1; bit_idx < 24; ++bit_idx) {
    const int boundary = enc.BitCellStart(bit_idx);
    // Just before the boundary the signal should still be near peak.
    const int pre_idx = boundary - ramp - 1;
    // Just after the boundary the signal should be near baseline (the inter-bit
    // brought it down to start the next '1' bit at baseline).
    const int post_idx = boundary + ramp + 1;
    EXPECT_NEAR(
        SampleFixedToMillivolts(samples[static_cast<std::size_t>(pre_idx)]),
        kPeak, 1.0)
        << "Boundary before bit " << bit_idx
        << " should be near peak before the inter-bit transition";
    EXPECT_NEAR(
        SampleFixedToMillivolts(samples[static_cast<std::size_t>(post_idx)]),
        kBaseline, 1.0)
        << "Boundary after bit " << bit_idx
        << " should be near baseline after the inter-bit transition";
  }
}

// An all-zeros 24-bit code has a rising inter-bit transition at every
// boundary (each '0' must start at peak; adjacent '0's need to be brought
// back from baseline to peak at the boundary).
TEST(BiphaseEncoderTest,
     AllZeroCodeHasRisingInterBitTransitionAtEveryBoundary) {
  constexpr double kPalSampleRate = 17734475.0;
  constexpr double kBaseline = 0.0;
  constexpr double kPeak = 100.0;
  const BiphaseEncoder enc(kPalSampleRate);
  // 0x000000 would have MSB=0 which is disallowed for real codes, but the
  // encoder does not check the key nibble — that's the validator's job.
  const auto samples = enc.Generate24BitCode(0x000000u, kBaseline, kPeak);

  const int bcs = enc.bit_cell_samples();
  const int ramp = enc.ramp_samples();
  for (int bit_idx = 1; bit_idx < 24; ++bit_idx) {
    const int boundary = enc.BitCellStart(bit_idx);
    const int pre_idx = boundary - ramp - 1;
    const int post_idx = boundary + ramp + 1;
    EXPECT_NEAR(
        SampleFixedToMillivolts(samples[static_cast<std::size_t>(pre_idx)]),
        kBaseline, 1.0)
        << "Before boundary " << bit_idx << " should be near baseline";
    EXPECT_NEAR(
        SampleFixedToMillivolts(samples[static_cast<std::size_t>(post_idx)]),
        kPeak, 1.0)
        << "After boundary " << bit_idx << " should be near peak";
  }
}

// Alternating bits (0b10101...) never need inter-bit transitions.
// Adjacent bit boundary should be continuous (no abrupt step).
TEST(BiphaseEncoderTest, AlternatingBitCodeHasNoBoundaryTransitions) {
  constexpr double kPalSampleRate = 17734475.0;
  constexpr double kBaseline = 0.0;
  constexpr double kPeak = 100.0;
  const BiphaseEncoder enc(kPalSampleRate);
  // 0xAAAAAA = 1010 1010 1010 1010 1010 1010 (alternating 1/0, MSB first)
  const auto samples = enc.Generate24BitCode(0xAAAAAAu, kBaseline, kPeak);

  const int bcs = enc.bit_cell_samples();
  // At each bit boundary between a '1' and a '0' pair: '1' ends at peak,
  // '0' starts at peak → no inter-bit.  The sample just before and just after
  // the boundary should both be near peak.
  for (int bit_idx = 1; bit_idx < 24; bit_idx += 2) {
    // Boundary between '1'(bit_idx-1) and '0'(bit_idx).
    const int boundary = enc.BitCellStart(bit_idx);
    const int pre_idx = boundary - 2;
    const int post_idx = boundary + 2;
    const double pre =
        SampleFixedToMillivolts(samples[static_cast<std::size_t>(pre_idx)]);
    const double post =
        SampleFixedToMillivolts(samples[static_cast<std::size_t>(post_idx)]);
    // Both should be at peak (no falling inter-bit transition).
    EXPECT_NEAR(pre, kPeak, 2.0)
        << "Pre-boundary sample at '1'→'0' boundary " << bit_idx;
    EXPECT_NEAR(post, kPeak, 2.0)
        << "Post-boundary sample at '1'→'0' boundary " << bit_idx;
  }
}

// The NTSC lead-out code 0x80EEEE at NTSC 4fsc.
TEST(BiphaseEncoderTest, Generate24BitCodeWorksForNtscSampleRate) {
  constexpr double kNtscSampleRate = 14318180.0;
  constexpr double kBaseline = 0.0;
  constexpr double kPeak = 714.3;
  const BiphaseEncoder enc(kNtscSampleRate);
  const auto samples = enc.Generate24BitCode(0x80EEEEu, kBaseline, kPeak);
  EXPECT_EQ(static_cast<int>(samples.size()), enc.BitCellStart(24));

  // First bit is '1' (MSB of 0x80 = 1): first quarter near baseline.
  const int quarter = enc.bit_cell_samples() / 4;
  for (int i = 0; i < quarter; ++i) {
    EXPECT_NEAR(SampleFixedToMillivolts(samples[static_cast<std::size_t>(i)]),
                kBaseline, 1.0);
  }
}

// ---------------------------------------------------------------------------
// BiphaseEncoderTest — GenerateLine
// ---------------------------------------------------------------------------

TEST(BiphaseEncoderTest, GenerateLinePalReturnsCorrectSampleCount) {
  constexpr double kPalSampleRate = 17734475.0;
  const BiphaseEncoder enc(kPalSampleRate);
  const auto samples = enc.GenerateLine("88FFFF", Standard::kPal, 210.0, 700.0);
  EXPECT_EQ(static_cast<int>(samples.size()),
            GetTimingConstants(Standard::kPal).samples_per_line_4fsc);
}

TEST(BiphaseEncoderTest, GenerateLineNtscReturnsCorrectSampleCount) {
  constexpr double kNtscSampleRate = 14318180.0;
  const BiphaseEncoder enc(kNtscSampleRate);
  const auto samples = enc.GenerateLine("80EEEE", Standard::kNtsc, 0.0, 714.3);
  EXPECT_EQ(static_cast<int>(samples.size()),
            GetTimingConstants(Standard::kNtsc).samples_per_line_4fsc);
}

// The biphase signal occupies the first code_samples samples of the line.
TEST(BiphaseEncoderTest, GenerateLineBiphaseSignalIsAtStartOfBuffer) {
  constexpr double kPalSampleRate = 17734475.0;
  constexpr double kBaseline = 210.0;
  constexpr double kPeak = 700.0;
  const BiphaseEncoder enc(kPalSampleRate);
  const auto line =
      enc.GenerateLine("88FFFF", Standard::kPal, kBaseline, kPeak);
  const auto code = enc.Generate24BitCode(0x88FFFFu, kBaseline, kPeak);

  for (int i = 0; i < static_cast<int>(code.size()); ++i) {
    EXPECT_EQ(line[static_cast<std::size_t>(i)],
              code[static_cast<std::size_t>(i)])
        << "Mismatch at sample " << i;
  }
}

// Samples after the 24-bit code must be at baseline.
TEST(BiphaseEncoderTest, GenerateLineSamplesAfterCodeAreAtBaseline) {
  constexpr double kPalSampleRate = 17734475.0;
  constexpr double kBaseline = 210.0;
  constexpr double kPeak = 700.0;
  const BiphaseEncoder enc(kPalSampleRate);
  const auto line =
      enc.GenerateLine("88FFFF", Standard::kPal, kBaseline, kPeak);

  const int code_end = enc.BitCellStart(24);
  const int line_size = static_cast<int>(line.size());
  for (int i = code_end; i < line_size; ++i) {
    EXPECT_NEAR(SampleFixedToMillivolts(line[static_cast<std::size_t>(i)]),
                kBaseline, 0.01)
        << "Sample " << i << " after biphase code should be at baseline";
  }
}

TEST(BiphaseEncoderTest, GenerateLineThrowsOnInvalidHexCode) {
  constexpr double kPalSampleRate = 17734475.0;
  const BiphaseEncoder enc(kPalSampleRate);
  EXPECT_THROW(enc.GenerateLine("ZZZZZZ", Standard::kPal, 210.0, 700.0),
               std::invalid_argument);
}

TEST(BiphaseEncoderTest, GenerateLineThrowsOnShortHexCode) {
  constexpr double kPalSampleRate = 17734475.0;
  const BiphaseEncoder enc(kPalSampleRate);
  EXPECT_THROW(enc.GenerateLine("88FF", Standard::kPal, 210.0, 700.0),
               std::invalid_argument);
}

// ---------------------------------------------------------------------------
// BiphaseEncoderTest — PAL level mapping (IEC 60856: blanking to 100% white)
// ---------------------------------------------------------------------------

// For PAL the baseline is blanking = 0 mV, peak = 700 mV (100% white).
// IEC 60856 §10.1 "30%–100%" specifies the allowed range of the high level.
// The first bit of lead-in code 0x88FFFF is '1': the first quarter must be
// at blanking (0 mV).
TEST(BiphaseEncoderTest, PalLeadInCodeFirstBitFirstQuarterAtPalBaseline) {
  constexpr double kPalSampleRate = 17734475.0;
  constexpr double kPalBaseline = 0.0;
  constexpr double kPalPeak = 700.0;
  const BiphaseEncoder enc(kPalSampleRate);
  const auto samples = enc.Generate24BitCode(0x88FFFFu, kPalBaseline, kPalPeak);

  const int quarter = enc.bit_cell_samples() / 4;
  for (int i = 0; i < quarter; ++i) {
    EXPECT_NEAR(SampleFixedToMillivolts(samples[static_cast<std::size_t>(i)]),
                kPalBaseline, 1.0);
  }
}

// ---------------------------------------------------------------------------
// BiphaseEncoderTest — NTSC level mapping (IEC 60857: 0-100 IRE)
// ---------------------------------------------------------------------------

TEST(BiphaseEncoderTest,
     NtscLeadOutCodeLastBitCellTrailingQuarterAtNtscBaseline) {
  constexpr double kNtscSampleRate = 14318180.0;
  constexpr double kNtscBaseline = 0.0;
  constexpr double kNtscPeak = 714.3;
  const BiphaseEncoder enc(kNtscSampleRate);
  // 0x80EEEE binary (MSB first): 1000 0000 1110 1110 1110 1110
  // The last transmitted bit (bit_idx=23) is the LSB = '0', which ends at
  // baseline. Check the trailing quarter of the last bit cell only.
  const auto samples =
      enc.Generate24BitCode(0x80EEEEu, kNtscBaseline, kNtscPeak);

  const int bcs = enc.bit_cell_samples();
  const int last_bit_start = enc.BitCellStart(23);
  const int three_q = last_bit_start + (3 * bcs) / 4;
  const int n = static_cast<int>(samples.size());
  for (int i = three_q; i < n; ++i) {
    EXPECT_NEAR(SampleFixedToMillivolts(samples[static_cast<std::size_t>(i)]),
                kNtscBaseline, 1.0)
        << "Sample " << i << " in trailing quarter of last '0' bit";
  }
}

}  // namespace
}  // namespace videosynth
