/*
 * File:        test_fm_encoder.cpp
 * Module:      fm_encoder_tests
 * Purpose:     Unit tests for 40-bit FM signal encoding per IEC 60857 §10.2,
 *              Figure 13 (NTSC LaserDisc VBI).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <vector>

#include "videosynth/biphase_encoder.h"
#include "videosynth/fixed_point.h"
#include "videosynth/fm_encoder.h"
#include "videosynth/timing_constants.h"

namespace videosynth {
namespace {

// ---------------------------------------------------------------------------
// Test constants
// ---------------------------------------------------------------------------

constexpr double kNtscSampleRate = 14318180.0;
constexpr double kPalSampleRate = 17734475.0;
constexpr double kBaseline = 0.0;  // 0 IRE in mV
constexpr double kPeak = 714.3;    // 100 IRE in mV

// Returns a zero-data FmData with field_one set as specified.
FmData ZeroData(bool field_one = false) {
  return FmData{field_one, 0u, 0u, 0u, 0u, 0u};
}

// Returns the value at the first quarter of bit cell `bit_idx` in `samples`.
double FirstQuarterLevel(const std::vector<SampleFixed>& samples, int bit_idx,
                         const FmEncoder& enc) {
  const int start = enc.BitCellStart(bit_idx);
  const int quarter = (enc.BitCellStart(bit_idx + 1) - start) / 4;
  // Average over the stable first-quarter region (a few samples in).
  double sum = 0.0;
  int count = 0;
  for (int i = start + 1;
       i < start + quarter - 1 && i < static_cast<int>(samples.size()); ++i) {
    sum += SampleFixedToMillivolts(samples[static_cast<std::size_t>(i)]);
    ++count;
  }
  return count > 0 ? sum / count
                   : SampleFixedToMillivolts(
                         samples[static_cast<std::size_t>(start)]);
}

// Returns the value at the last quarter of bit cell `bit_idx` in `samples`.
double LastQuarterLevel(const std::vector<SampleFixed>& samples, int bit_idx,
                        const FmEncoder& enc) {
  const int start = enc.BitCellStart(bit_idx);
  const int end = enc.BitCellStart(bit_idx + 1);
  const int three_quarters = (3 * (end - start)) / 4;
  double sum = 0.0;
  int count = 0;
  for (int i = start + three_quarters + 1;
       i < end - 1 && i < static_cast<int>(samples.size()); ++i) {
    sum += SampleFixedToMillivolts(samples[static_cast<std::size_t>(i)]);
    ++count;
  }
  return count > 0 ? sum / count
                   : SampleFixedToMillivolts(
                         samples[static_cast<std::size_t>(end - 1)]);
}

// ---------------------------------------------------------------------------
// FmEncoderTest — construction
// ---------------------------------------------------------------------------

TEST(FmEncoderTest, BitCellSamplesCorrectForNtsc4Fsc) {
  // NTSC 4fsc: 14318180 Hz, 1 µs bit cell → round(1e-6 × 14318180) = 14
  const FmEncoder enc(kNtscSampleRate);
  EXPECT_EQ(enc.bit_cell_samples(), 14);
}

TEST(FmEncoderTest, BitCellSamplesCorrectForPal4Fsc) {
  // PAL 4fsc: 17734475 Hz, 1 µs bit cell → round(1e-6 × 17734475) = 18
  const FmEncoder enc(kPalSampleRate);
  EXPECT_EQ(enc.bit_cell_samples(), 18);
}

TEST(FmEncoderTest, RampSamplesPositiveForNtsc4Fsc) {
  const FmEncoder enc(kNtscSampleRate);
  EXPECT_GT(enc.ramp_samples(), 0);
  EXPECT_LT(enc.ramp_samples(), enc.bit_cell_samples() / 2);
}

TEST(FmEncoderTest, RampSamplesPositiveForPal4Fsc) {
  const FmEncoder enc(kPalSampleRate);
  EXPECT_GT(enc.ramp_samples(), 0);
  EXPECT_LT(enc.ramp_samples(), enc.bit_cell_samples() / 2);
}

// IEC 60857 §10.2 specifies 135 ns transitions for the FM signal.
// 24-bit biphase uses 225 ns.  At the same NTSC sample rate, the FM ramp must
// be strictly narrower.
TEST(FmEncoderTest, FmRampSamplesNarrowerThanBiphaseRampAtNtsc) {
  const FmEncoder fm_enc(kNtscSampleRate);
  const BiphaseEncoder bi_enc(kNtscSampleRate);  // default 225 ns
  EXPECT_LT(fm_enc.ramp_samples(), bi_enc.ramp_samples());
}

// ---------------------------------------------------------------------------
// FmEncoderTest — BuildBitPattern
// ---------------------------------------------------------------------------

TEST(FmEncoderTest, ClockSyncBitsAreCorrect) {
  const auto bits = FmEncoder::BuildBitPattern(ZeroData());
  // Bits [0-3] = 0011
  EXPECT_FALSE(bits[0]);
  EXPECT_FALSE(bits[1]);
  EXPECT_TRUE(bits[2]);
  EXPECT_TRUE(bits[3]);
}

TEST(FmEncoderTest, FieldIndicatorBitTrueForFieldOne) {
  const auto bits = FmEncoder::BuildBitPattern(FmData{true, 0, 0, 0, 0, 0});
  EXPECT_TRUE(bits[4]);
}

TEST(FmEncoderTest, FieldIndicatorBitFalseForFieldTwo) {
  const auto bits = FmEncoder::BuildBitPattern(FmData{false, 0, 0, 0, 0, 0});
  EXPECT_FALSE(bits[4]);
}

TEST(FmEncoderTest, LeadingRecognitionBitsAreCorrect) {
  const auto bits = FmEncoder::BuildBitPattern(ZeroData());
  // Bits [5-11] = 1110010
  EXPECT_TRUE(bits[5]);
  EXPECT_TRUE(bits[6]);
  EXPECT_TRUE(bits[7]);
  EXPECT_FALSE(bits[8]);
  EXPECT_FALSE(bits[9]);
  EXPECT_TRUE(bits[10]);
  EXPECT_FALSE(bits[11]);
}

TEST(FmEncoderTest, TrailingRecognitionBitsAreCorrect) {
  const auto bits = FmEncoder::BuildBitPattern(ZeroData());
  // Bits [33-39] = 0001101
  EXPECT_FALSE(bits[33]);
  EXPECT_FALSE(bits[34]);
  EXPECT_FALSE(bits[35]);
  EXPECT_TRUE(bits[36]);
  EXPECT_TRUE(bits[37]);
  EXPECT_FALSE(bits[38]);
  EXPECT_TRUE(bits[39]);
}

TEST(FmEncoderTest, DataNibbleX5EncodedLsbFirst) {
  // X5 = 0xA = 1010 → LSB first: bit[12]=0, bit[13]=1, bit[14]=0, bit[15]=1
  const auto bits = FmEncoder::BuildBitPattern(FmData{false, 0, 0, 0, 0, 0x0A});
  EXPECT_FALSE(bits[12]);
  EXPECT_TRUE(bits[13]);
  EXPECT_FALSE(bits[14]);
  EXPECT_TRUE(bits[15]);
}

TEST(FmEncoderTest, DataNibbleX4EncodedLsbFirst) {
  // X4 = 0x5 = 0101 → LSB first: bit[16]=1, bit[17]=0, bit[18]=1, bit[19]=0
  const auto bits = FmEncoder::BuildBitPattern(FmData{false, 0, 0, 0, 0x05, 0});
  EXPECT_TRUE(bits[16]);
  EXPECT_FALSE(bits[17]);
  EXPECT_TRUE(bits[18]);
  EXPECT_FALSE(bits[19]);
}

TEST(FmEncoderTest, DataNibbleX3EncodedLsbFirst) {
  // X3 = 0x3 = 0011 → LSB first: bit[20]=1, bit[21]=1, bit[22]=0, bit[23]=0
  const auto bits = FmEncoder::BuildBitPattern(FmData{false, 0, 0, 0x03, 0, 0});
  EXPECT_TRUE(bits[20]);
  EXPECT_TRUE(bits[21]);
  EXPECT_FALSE(bits[22]);
  EXPECT_FALSE(bits[23]);
}

TEST(FmEncoderTest, DataNibbleX2EncodedLsbFirst) {
  // X2 = 0x9 = 1001 → LSB first: bit[24]=1, bit[25]=0, bit[26]=0, bit[27]=1
  const auto bits = FmEncoder::BuildBitPattern(FmData{false, 0, 0x09, 0, 0, 0});
  EXPECT_TRUE(bits[24]);
  EXPECT_FALSE(bits[25]);
  EXPECT_FALSE(bits[26]);
  EXPECT_TRUE(bits[27]);
}

TEST(FmEncoderTest, DataNibbleX1EncodedLsbFirst) {
  // X1 = 0x6 = 0110 → LSB first: bit[28]=0, bit[29]=1, bit[30]=1, bit[31]=0
  const auto bits = FmEncoder::BuildBitPattern(FmData{false, 0x06, 0, 0, 0, 0});
  EXPECT_FALSE(bits[28]);
  EXPECT_TRUE(bits[29]);
  EXPECT_TRUE(bits[30]);
  EXPECT_FALSE(bits[31]);
}

TEST(FmEncoderTest, AllDataNibblesZeroGivesCorrectBitCount) {
  const auto bits = FmEncoder::BuildBitPattern(ZeroData(false));
  // bits[12-31] are all 0 → 0 ones (even).
  // Parity bit [32] must be 1 so that bits[12-32] count is odd.
  EXPECT_TRUE(bits[32]);
}

TEST(FmEncoderTest, ParityBitZeroWhenDataBitsAlreadyOdd) {
  // x5=1 → bit[12]=1 (LSB of 0x01).  bits[12-31] has 1 one (odd).
  // Parity bit [32] must be 0 to keep the bits[12-32] count odd.
  const auto bits = FmEncoder::BuildBitPattern(FmData{false, 0, 0, 0, 0, 1});
  EXPECT_FALSE(bits[32]);
}

TEST(FmEncoderTest, ParityIsOddOverDataBitsAndParityBit) {
  // Parity is odd over the 20 data nibble bits [12-31] plus the parity bit
  // [32]. The field-indicator bit [4] must not affect the result.
  const std::vector<FmData> payloads = {
      {false, 0x0, 0x0, 0x0, 0x0, 0x0},
      {true, 0x5, 0x3, 0xA, 0x1, 0x7},
      {false, 0xF, 0xF, 0xF, 0xF, 0xF},
      {true, 0x1, 0x2, 0x3, 0x4, 0x5},
  };
  for (const auto& payload : payloads) {
    const auto bits = FmEncoder::BuildBitPattern(payload);
    int ones = 0;
    for (int i = 12; i <= 32; ++i) {
      if (bits[static_cast<std::size_t>(i)]) ++ones;
    }
    EXPECT_EQ(ones % 2, 1) << "Parity failed for payload";
  }
}

// ---------------------------------------------------------------------------
// FmEncoderTest — Generate40BitCode waveform shape
// ---------------------------------------------------------------------------

TEST(FmEncoderTest, Generate40BitWaveformHasCorrectSampleCount) {
  const FmEncoder enc(kNtscSampleRate);
  const auto samples = enc.Generate40BitWaveform(ZeroData(), kBaseline, kPeak);
  EXPECT_EQ(static_cast<int>(samples.size()), enc.BitCellStart(40));
}

// Bit [0] = 0 (clock sync '0'): first quarter of bit cell 0 must be at peak.
TEST(FmEncoderTest, BitZeroIsClockSyncZeroStartsAtPeak) {
  const FmEncoder enc(kNtscSampleRate);
  const auto samples = enc.Generate40BitWaveform(ZeroData(), kBaseline, kPeak);
  EXPECT_NEAR(FirstQuarterLevel(samples, 0, enc), kPeak, 1.0);
}

// Bit [2] = 1 (clock sync '1'): in FM encoding the signal starts HIGH after
// two '0' bits (0→flip→LOW, 1→flip→HIGH).  Bit [2] therefore starts HIGH.
// A '1' bit has a pip in the second half, so its last quarter is at baseline.
TEST(FmEncoderTest, BitTwoIsClockSyncOnePipEndsAtBaseline) {
  const FmEncoder enc(kNtscSampleRate);
  const auto samples = enc.Generate40BitWaveform(ZeroData(), kBaseline, kPeak);
  // First quarter: HIGH (peak) — signal starts at peak for this bit.
  EXPECT_NEAR(FirstQuarterLevel(samples, 2, enc), kPeak, 1.0);
  // Last quarter: LOW (baseline) — pip occupies the second half.
  EXPECT_NEAR(LastQuarterLevel(samples, 2, enc), kBaseline, 1.0);
}

// Bit [5] = 1 (leading recognition '1'): last quarter of bit cell 5 at peak.
TEST(FmEncoderTest, BitFiveIsLeadingRecognitionOneEndsAtPeak) {
  const FmEncoder enc(kNtscSampleRate);
  const auto samples = enc.Generate40BitWaveform(ZeroData(), kBaseline, kPeak);
  EXPECT_NEAR(LastQuarterLevel(samples, 5, enc), kPeak, 1.0);
}

// Bit [8] = 0 (leading recognition '0'): last quarter of bit cell 8 at
// baseline.
TEST(FmEncoderTest, BitEightIsLeadingRecognitionZeroEndsAtBaseline) {
  const FmEncoder enc(kNtscSampleRate);
  const auto samples = enc.Generate40BitWaveform(ZeroData(), kBaseline, kPeak);
  EXPECT_NEAR(LastQuarterLevel(samples, 8, enc), kBaseline, 1.0);
}

// Field indicator bit [4] = 1 when field_one=true.
// After bits 0011, the signal is at peak.  A '1' bit therefore starts HIGH
// and has a pip (LOW) in the second half.
TEST(FmEncoderTest, FieldOneTrueMakesBit4AOneCell) {
  const FmEncoder enc(kNtscSampleRate);
  const auto samples =
      enc.Generate40BitWaveform(FmData{true, 0, 0, 0, 0, 0}, kBaseline, kPeak);
  // First quarter: peak (signal is HIGH at start of bit [4]).
  EXPECT_NEAR(FirstQuarterLevel(samples, 4, enc), kPeak, 1.0);
  // Last quarter: baseline (pip occupies the second half of the cell).
  EXPECT_NEAR(LastQuarterLevel(samples, 4, enc), kBaseline, 1.0);
}

// Field indicator bit [4] = 0 when field_one=false.
TEST(FmEncoderTest, FieldOneFalseMakesBit4AZeroCell) {
  const FmEncoder enc(kNtscSampleRate);
  const auto samples =
      enc.Generate40BitWaveform(FmData{false, 0, 0, 0, 0, 0}, kBaseline, kPeak);
  // A '0' bit: first quarter at peak.
  EXPECT_NEAR(FirstQuarterLevel(samples, 4, enc), kPeak, 1.0);
}

// Trailing recognition bit [36] = 1.
TEST(FmEncoderTest, TrailingBit36IsOneCell) {
  const FmEncoder enc(kNtscSampleRate);
  const auto samples = enc.Generate40BitWaveform(ZeroData(), kBaseline, kPeak);
  EXPECT_NEAR(FirstQuarterLevel(samples, 36, enc), kBaseline, 1.0);
}

// Trailing recognition bit [35] = 0.
TEST(FmEncoderTest, TrailingBit35IsZeroCell) {
  const FmEncoder enc(kNtscSampleRate);
  const auto samples = enc.Generate40BitWaveform(ZeroData(), kBaseline, kPeak);
  EXPECT_NEAR(FirstQuarterLevel(samples, 35, enc), kPeak, 1.0);
}

// Trailing recognition bit [39] = 1 (last bit of pattern).
// Level trace to bit[39]: bit[38]='0' starts LOW and flips to HIGH.
// So bit[39]='1' starts HIGH and has pip (LOW) in its second half.
TEST(FmEncoderTest, TrailingBit39IsOneCell) {
  const FmEncoder enc(kNtscSampleRate);
  const auto samples = enc.Generate40BitWaveform(ZeroData(), kBaseline, kPeak);
  // First quarter: peak (bit[39] starts HIGH after bit[38] flips LOW→HIGH).
  EXPECT_NEAR(FirstQuarterLevel(samples, 39, enc), kPeak, 1.0);
  // Last quarter: baseline (pip occupies the second half).
  EXPECT_NEAR(LastQuarterLevel(samples, 39, enc), kBaseline, 1.0);
}

// X5 = 0x5 = 0101 → LSB first: bit[12]=1, bit[13]=0, bit[14]=1, bit[15]=0.
// Level entering bit[12] = HIGH (preamble bits leave the signal at peak).
// FM level trace: bit[12]='1'→no flip (HIGH); bit[13]='0'→flip (LOW);
//                 bit[14]='1'→no flip (LOW); bit[15]='0'→flip (HIGH).
TEST(FmEncoderTest, X5NibbleEncodedCorrectlyInWaveform) {
  const FmEncoder enc(kNtscSampleRate);
  const auto samples = enc.Generate40BitWaveform(
      FmData{false, 0, 0, 0, 0, 0x05}, kBaseline, kPeak);
  // bit[12]=1: starts HIGH (peak), pip ends at baseline.
  EXPECT_NEAR(FirstQuarterLevel(samples, 12, enc), kPeak, 1.0);
  EXPECT_NEAR(LastQuarterLevel(samples, 12, enc), kBaseline, 1.0);
  // bit[13]=0: starts HIGH (no flip from '1'), whole cell at peak.
  EXPECT_NEAR(FirstQuarterLevel(samples, 13, enc), kPeak, 1.0);
  EXPECT_NEAR(LastQuarterLevel(samples, 13, enc), kPeak, 1.0);
  // bit[14]=1: starts LOW (flip from '0'), pip ends at peak.
  EXPECT_NEAR(FirstQuarterLevel(samples, 14, enc), kBaseline, 1.0);
  EXPECT_NEAR(LastQuarterLevel(samples, 14, enc), kPeak, 1.0);
  // bit[15]=0: starts LOW (no flip from '1'), whole cell at baseline.
  EXPECT_NEAR(FirstQuarterLevel(samples, 15, enc), kBaseline, 1.0);
  EXPECT_NEAR(LastQuarterLevel(samples, 15, enc), kBaseline, 1.0);
}

// X1 = 0x9 = 1001 → LSB first: bit[28]=1, bit[29]=0, bit[30]=0, bit[31]=1.
// Level entering bit[28] = HIGH (bits[12-27] all '0': 16 flips, even → HIGH).
// FM level trace: bit[28]='1'→no flip (HIGH); bit[29]='0'→flip (LOW);
//                 bit[30]='0'→flip (HIGH); bit[31]='1'→no flip (HIGH).
TEST(FmEncoderTest, X1NibbleEncodedCorrectlyInWaveform) {
  const FmEncoder enc(kNtscSampleRate);
  const auto samples = enc.Generate40BitWaveform(
      FmData{false, 0x09, 0, 0, 0, 0}, kBaseline, kPeak);
  // bit[28]=1: starts HIGH (peak), pip ends at baseline.
  EXPECT_NEAR(FirstQuarterLevel(samples, 28, enc), kPeak, 1.0);
  EXPECT_NEAR(LastQuarterLevel(samples, 28, enc), kBaseline, 1.0);
  // bit[29]=0: starts HIGH (no flip from '1'), whole cell at peak.
  EXPECT_NEAR(FirstQuarterLevel(samples, 29, enc), kPeak, 1.0);
  EXPECT_NEAR(LastQuarterLevel(samples, 29, enc), kPeak, 1.0);
  // bit[30]=0: starts LOW (flip from '0'), whole cell at baseline.
  EXPECT_NEAR(FirstQuarterLevel(samples, 30, enc), kBaseline, 1.0);
  EXPECT_NEAR(LastQuarterLevel(samples, 30, enc), kBaseline, 1.0);
  // bit[31]=1: starts HIGH (flip from '0'), pip ends at baseline.
  EXPECT_NEAR(FirstQuarterLevel(samples, 31, enc), kPeak, 1.0);
  EXPECT_NEAR(LastQuarterLevel(samples, 31, enc), kBaseline, 1.0);
}

// FM '0' bit: signal holds current level for the full cell, then flips.
// Bits [0,1] are both '0'. Bit[0] starts HIGH → at the boundary
// (BitCellStart(1)) the signal transitions HIGH→LOW (falling edge).
TEST(FmEncoderTest, ConsecutiveZeroBitsHaveFallingTransitionAtBoundary) {
  const FmEncoder enc(kNtscSampleRate);
  const auto samples = enc.Generate40BitWaveform(ZeroData(), kBaseline, kPeak);
  const int ramp = enc.ramp_samples();
  const int boundary = enc.BitCellStart(1);  // end of bit[0], start of bit[1]
  const int pre_idx = boundary - ramp - 1;
  const int post_idx = boundary + ramp + 1;
  // Before transition: HIGH (bit[0] held its level until the flip).
  EXPECT_NEAR(
      SampleFixedToMillivolts(samples[static_cast<std::size_t>(pre_idx)]),
      kPeak, 1.0);
  // After transition: LOW (bit[1] now holds the flipped level).
  EXPECT_NEAR(
      SampleFixedToMillivolts(samples[static_cast<std::size_t>(post_idx)]),
      kBaseline, 1.0);
}

// FM '1' bit: pip in second half (LOW for a HIGH-starting bit).
// Bits [2,3] are both '1', each starting HIGH.  At BitCellStart(3) the pip of
// bit[2] ends (rising: LOW→HIGH), then bit[3] starts HIGH.
TEST(FmEncoderTest, ConsecutiveOneBitsHaveRisingTransitionAtPipEnd) {
  const FmEncoder enc(kNtscSampleRate);
  const auto samples = enc.Generate40BitWaveform(ZeroData(), kBaseline, kPeak);
  const int ramp = enc.ramp_samples();
  // Pip-end of bit[2] = cell boundary at BitCellStart(3).
  const int pip_end = enc.BitCellStart(3);
  const int pre_idx = pip_end - ramp - 1;
  const int post_idx = pip_end + ramp + 1;
  // Before pip-end: LOW (inside pip region of bit[2]).
  EXPECT_NEAR(
      SampleFixedToMillivolts(samples[static_cast<std::size_t>(pre_idx)]),
      kBaseline, 1.0);
  // After pip-end: HIGH (bit[3] starts at peak before its own pip).
  EXPECT_NEAR(
      SampleFixedToMillivolts(samples[static_cast<std::size_t>(post_idx)]),
      kPeak, 1.0);
}

// ---------------------------------------------------------------------------
// FmEncoderTest — Generate40BitCode full-line output
// ---------------------------------------------------------------------------

TEST(FmEncoderTest, Generate40BitCodeNtscLineHasCorrectLength) {
  const FmEncoder enc(kNtscSampleRate);
  const auto line =
      enc.Generate40BitCode(ZeroData(), Standard::kNtsc, kBaseline, kPeak);
  EXPECT_EQ(static_cast<int>(line.size()),
            GetTimingConstants(Standard::kNtsc).samples_per_line_4fsc);
}

TEST(FmEncoderTest, Generate40BitCodePalLineHasCorrectLength) {
  const FmEncoder enc(kPalSampleRate);
  const auto line =
      enc.Generate40BitCode(ZeroData(), Standard::kPal, kBaseline, kPeak);
  EXPECT_EQ(static_cast<int>(line.size()),
            GetTimingConstants(Standard::kPal).samples_per_line_4fsc);
}

TEST(FmEncoderTest, Generate40BitCodeLineHasBaselineAfterCode) {
  const FmEncoder enc(kNtscSampleRate);
  const auto line =
      enc.Generate40BitCode(ZeroData(), Standard::kNtsc, kBaseline, kPeak);
  const int code_end = enc.BitCellStart(40);
  const int line_size = static_cast<int>(line.size());
  for (int i = code_end; i < line_size; ++i) {
    EXPECT_NEAR(SampleFixedToMillivolts(line[static_cast<std::size_t>(i)]),
                kBaseline, 0.01)
        << "Sample " << i << " after FM code should be at baseline";
  }
}

// ---------------------------------------------------------------------------
// FmEncoderTest — GenerateWhiteFlag
// ---------------------------------------------------------------------------

TEST(FmEncoderTest, GenerateWhiteFlagNtscLineHasCorrectLength) {
  const FmEncoder enc(kNtscSampleRate);
  const auto line = enc.GenerateWhiteFlag(Standard::kNtsc, kPeak);
  EXPECT_EQ(static_cast<int>(line.size()),
            GetTimingConstants(Standard::kNtsc).samples_per_line_4fsc);
}

TEST(FmEncoderTest, GenerateWhiteFlagPalLineHasCorrectLength) {
  const FmEncoder enc(kPalSampleRate);
  const auto line = enc.GenerateWhiteFlag(Standard::kPal, kPeak);
  EXPECT_EQ(static_cast<int>(line.size()),
            GetTimingConstants(Standard::kPal).samples_per_line_4fsc);
}

// IEC 60857 Figure 12: white flag length = 0.790H.
// For NTSC 910 samples/line: flag_length = round(0.790 * 910) = 719.
TEST(FmEncoderTest, GenerateWhiteFlagPulseRegionAtPeakAndTailAtBaseline) {
  const FmEncoder enc(kNtscSampleRate);
  const auto line = enc.GenerateWhiteFlag(Standard::kNtsc, kPeak);
  const int flag_length = static_cast<int>(std::round(0.790 * 910));
  // Interior of pulse (skip transition ramp at each edge) should be at peak.
  const int skip = enc.ramp_samples() + 1;
  for (int i = skip; i < flag_length - skip; ++i) {
    EXPECT_NEAR(SampleFixedToMillivolts(line[static_cast<std::size_t>(i)]),
                kPeak, 1.0)
        << "White flag interior sample " << i << " should be at 100 IRE";
  }
  // Tail (after pulse) should be at baseline.
  for (std::size_t i = static_cast<std::size_t>(flag_length); i < line.size();
       ++i) {
    EXPECT_NEAR(SampleFixedToMillivolts(line[i]), 0.0, 1.0)
        << "White flag tail sample " << i << " should be at baseline";
  }
}

TEST(FmEncoderTest, GenerateWhiteFlagWorksWithDifferentPeakLevel) {
  const FmEncoder enc(kNtscSampleRate);
  constexpr double kCustomPeak = 500.0;
  const auto line = enc.GenerateWhiteFlag(Standard::kNtsc, kCustomPeak);
  const int flag_length = static_cast<int>(std::round(0.790 * 910));
  const int skip = enc.ramp_samples() + 1;
  for (int i = skip; i < flag_length - skip; ++i) {
    EXPECT_NEAR(SampleFixedToMillivolts(line[static_cast<std::size_t>(i)]),
                kCustomPeak, 1.0)
        << "White flag sample " << i << " should be at custom peak level";
  }
}

// ---------------------------------------------------------------------------
// FmEncoderTest — picture number encoding (fm_picture_number, IEC 60857
// §10.2.3)
// ---------------------------------------------------------------------------

// fm_picture_number = 12345: X1=1, X2=2, X3=3, X4=4, X5=5
// X5 = 0x5 = 0101 → bit[12]=1, bit[13]=0, bit[14]=1, bit[15]=0
// X1 = 0x1 = 0001 → bit[28]=1, bit[29]=0, bit[30]=0, bit[31]=0
TEST(FmEncoderTest, PictureNumber12345EncodesCorrectly) {
  const FmEncoder enc(kNtscSampleRate);
  const FmData picture_12345{false, 0x1, 0x2, 0x3, 0x4, 0x5};
  const auto bits = FmEncoder::BuildBitPattern(picture_12345);

  // X5 = 0x5 = 0101 → LSB first: 1, 0, 1, 0
  EXPECT_TRUE(bits[12]);
  EXPECT_FALSE(bits[13]);
  EXPECT_TRUE(bits[14]);
  EXPECT_FALSE(bits[15]);

  // X4 = 0x4 = 0100 → LSB first: 0, 0, 1, 0
  EXPECT_FALSE(bits[16]);
  EXPECT_FALSE(bits[17]);
  EXPECT_TRUE(bits[18]);
  EXPECT_FALSE(bits[19]);

  // X3 = 0x3 = 0011 → LSB first: 1, 1, 0, 0
  EXPECT_TRUE(bits[20]);
  EXPECT_TRUE(bits[21]);
  EXPECT_FALSE(bits[22]);
  EXPECT_FALSE(bits[23]);

  // X2 = 0x2 = 0010 → LSB first: 0, 1, 0, 0
  EXPECT_FALSE(bits[24]);
  EXPECT_TRUE(bits[25]);
  EXPECT_FALSE(bits[26]);
  EXPECT_FALSE(bits[27]);

  // X1 = 0x1 = 0001 → LSB first: 1, 0, 0, 0
  EXPECT_TRUE(bits[28]);
  EXPECT_FALSE(bits[29]);
  EXPECT_FALSE(bits[30]);
  EXPECT_FALSE(bits[31]);
}

// ---------------------------------------------------------------------------
// FmEncoderTest — mode indicator (fm_programme_time, IEC 60857 §10.2)
// ---------------------------------------------------------------------------

// X5 = 0xA (mode A = lead-in): bit[12]=0, bit[13]=1, bit[14]=0, bit[15]=1
TEST(FmEncoderTest, ModeIndicatorALeadInEncodesCorrectly) {
  const auto bits = FmEncoder::BuildBitPattern(FmData{false, 0, 0, 0, 0, 0x0A});
  // 0xA = 1010 → LSB first: 0, 1, 0, 1
  EXPECT_FALSE(bits[12]);
  EXPECT_TRUE(bits[13]);
  EXPECT_FALSE(bits[14]);
  EXPECT_TRUE(bits[15]);
}

// X5 = 0xD (mode D = picture/active programme): 0xD = 1101 → LSB first: 1, 0,
// 1, 1
TEST(FmEncoderTest, ModeIndicatorDPictureEncodesCorrectly) {
  const auto bits = FmEncoder::BuildBitPattern(FmData{false, 0, 0, 0, 0, 0x0D});
  // 0xD = 1101 → LSB first: 1, 0, 1, 1
  EXPECT_TRUE(bits[12]);
  EXPECT_FALSE(bits[13]);
  EXPECT_TRUE(bits[14]);
  EXPECT_TRUE(bits[15]);
}

// X5 = 0xC (mode C = lead-out): 0xC = 1100 → LSB first: 0, 0, 1, 1
TEST(FmEncoderTest, ModeIndicatorCLeadOutEncodesCorrectly) {
  const auto bits = FmEncoder::BuildBitPattern(FmData{false, 0, 0, 0, 0, 0x0C});
  // 0xC = 1100 → LSB first: 0, 0, 1, 1
  EXPECT_FALSE(bits[12]);
  EXPECT_FALSE(bits[13]);
  EXPECT_TRUE(bits[14]);
  EXPECT_TRUE(bits[15]);
}

}  // namespace
}  // namespace videosynth
