/*
 * File:        test_biphase_system.cpp
 * Module:      biphase_system_tests
 * Purpose:     System-level tests for biphase generation against IEC
 *              specifications. Covers reference test vectors, end-to-end code
 *              generation, level mapping, timing, and performance benchmarking.
 *              Tasks 6.1-6.4: system tests, reference vectors, IEC compliance,
 *              and performance.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <vector>

#include "videosynth/biphase_encoder.h"
#include "videosynth/biphase_utils.h"
#include "videosynth/cav_code_generator.h"
#include "videosynth/fixed_point.h"
#include "videosynth/fm_code_generator.h"
#include "videosynth/fm_encoder.h"
#include "videosynth/model.h"
#include "videosynth/timing_constants.h"

namespace videosynth {
namespace {

// ---------------------------------------------------------------------------
// Test constants
// ---------------------------------------------------------------------------

constexpr double kNtscSampleRate = 14318180.0;
constexpr double kPalSampleRate = 17734475.0;
constexpr double kPalBaseline = 0.0;   // blanking level (IEC 60856 Figure 14)
constexpr double kPalPeak = 700.0;     // 100% white
constexpr double kNtscBaseline = 0.0;  // 0 IRE
constexpr double kNtscPeak = 714.3;    // 100 IRE

// ---------------------------------------------------------------------------
// BiphaseSystemTest — reference test vectors (known-good bit patterns)
// ---------------------------------------------------------------------------

// Verify that the bit pattern for lead-in code 0x88FFFF matches the IEC spec.
// 0x88FFFF binary (MSB first): 1000 1000 1111 1111 1111 1111
TEST(BiphaseSystemTest, LeadInCodeBitPatternIsCorrect) {
  constexpr uint32_t kLeadIn = 0x88FFFFu;
  // Bit 23 (MSB) of 0x88FFFF = 1 → key nibble valid.
  EXPECT_TRUE((kLeadIn & 0x800000u) != 0);
  // Bits 23-20: 1000 (0x8 = key nibble with MSB=1).
  const uint8_t key_nibble = (kLeadIn >> 20) & 0x0Fu;
  EXPECT_EQ(key_nibble, 0x8u);
  // Bits 19-12: 0x8F = 1000 1111 — user data.
  // Bits 11-0: 0xFFF — all ones.
  EXPECT_EQ(kLeadIn & 0x000FFFu, 0x000FFFu);
  EXPECT_TRUE(IsValidBiphaseKeyNibble(kLeadIn));
}

// Verify lead-out code 0x80EEEE.
TEST(BiphaseSystemTest, LeadOutCodeBitPatternIsCorrect) {
  constexpr uint32_t kLeadOut = 0x80EEEEu;
  EXPECT_TRUE(IsValidBiphaseKeyNibble(kLeadOut));
  // Bits 11-0: 0xEEE = 1110 1110 1110.
  EXPECT_EQ(kLeadOut & 0x000FFFu, 0x000EEEu);
}

// Verify picture number encoding for n=1 → 0xF00001.
TEST(BiphaseSystemTest, PictureNumber1EncodesTo0xF00001) {
  const uint32_t code = CavPictureNumberGenerator::EncodePictureNumber(1);
  EXPECT_EQ(code, 0xF00001u);
  EXPECT_TRUE(IsValidBiphaseKeyNibble(code));
}

// Verify picture number 12345 → 0xF12345.
TEST(BiphaseSystemTest, PictureNumber12345EncodesTo0xF12345) {
  const uint32_t code = CavPictureNumberGenerator::EncodePictureNumber(12345);
  EXPECT_EQ(code, 0xF12345u);
}

// Verify picture number 99999 (PAL max) → 0xF99999.
TEST(BiphaseSystemTest, PalMaxPictureNumber99999EncodesCorrectly) {
  const uint32_t code = CavPictureNumberGenerator::EncodePictureNumber(99999);
  EXPECT_EQ(code, 0xF99999u);
  EXPECT_TRUE(IsValidBiphaseKeyNibble(code));
}

// Verify picture number 79999 (NTSC max) → 0xF79999.
TEST(BiphaseSystemTest, NtscMaxPictureNumber79999EncodesCorrectly) {
  const uint32_t code = CavPictureNumberGenerator::EncodePictureNumber(79999);
  EXPECT_EQ(code, 0xF79999u);
  EXPECT_TRUE(IsValidBiphaseKeyNibble(code));
}

// ---------------------------------------------------------------------------
// BiphaseSystemTest — IEC bit cell timing compliance
// ---------------------------------------------------------------------------

TEST(BiphaseSystemTest, PalBitCellIs35Samples) {
  // PAL 4fsc 17734475 Hz × 2.0 µs = 35.469 → round to 35.
  const BiphaseEncoder enc(kPalSampleRate);
  EXPECT_EQ(enc.bit_cell_samples(), 35);
}

TEST(BiphaseSystemTest, NtscBitCellIs29Samples) {
  // NTSC 4fsc 14318180 Hz × 2.0 µs = 28.636 → round to 29.
  const BiphaseEncoder enc(kNtscSampleRate);
  EXPECT_EQ(enc.bit_cell_samples(), 29);
}

TEST(BiphaseSystemTest, PalBitCellDurationIsWithinIecTolerance) {
  // IEC 60856: bit cell = 2.0 µs ± 0.01 µs. Cells are laid out on the exact
  // grid, so the mean cell over a code is 2.0 µs to within half a sample
  // spread over 24 cells, and no cell boundary is more than half a sample
  // from its nominal time.
  const BiphaseEncoder enc(kPalSampleRate);
  const double mean_cell_us =
      static_cast<double>(enc.BitCellStart(24)) / 24.0 / kPalSampleRate * 1.0e6;
  EXPECT_NEAR(mean_cell_us, 2.0, 0.01);
  for (int k = 0; k <= 24; ++k) {
    EXPECT_LE(std::abs(enc.BitCellStart(k) - k * 2.0e-6 * kPalSampleRate), 0.5)
        << "Boundary " << k;
  }
}

TEST(BiphaseSystemTest, NtscBitCellDurationIsWithinIecTolerance) {
  const BiphaseEncoder enc(kNtscSampleRate);
  const double mean_cell_us = static_cast<double>(enc.BitCellStart(24)) / 24.0 /
                              kNtscSampleRate * 1.0e6;
  EXPECT_NEAR(mean_cell_us, 2.0, 0.01);
  for (int k = 0; k <= 24; ++k) {
    EXPECT_LE(std::abs(enc.BitCellStart(k) - k * 2.0e-6 * kNtscSampleRate), 0.5)
        << "Boundary " << k;
  }
}

// ---------------------------------------------------------------------------
// BiphaseSystemTest — PAL level mapping (IEC 60856: 30%-100% white)
// ---------------------------------------------------------------------------

TEST(BiphaseSystemTest, PalLeadInWaveformFirstBitStartsAtBaseline) {
  // Lead-in 0x88FFFF first bit = '1' (MSB of 0x8 = 1).
  // A '1' bit starts at baseline for the first quarter.
  const BiphaseEncoder enc(kPalSampleRate);
  const auto samples = enc.Generate24BitCode(0x88FFFFu, kPalBaseline, kPalPeak);

  const int quarter = enc.bit_cell_samples() / 4;
  for (int i = 0; i < quarter; ++i) {
    EXPECT_NEAR(SampleFixedToMillivolts(samples[static_cast<std::size_t>(i)]),
                kPalBaseline, 1.0)
        << "PAL lead-in first bit: sample " << i
        << " should be at baseline (blanking) 0 mV";
  }
}

TEST(BiphaseSystemTest, PalLeadInWaveformFirstBitEndsAtPeak) {
  // The last quarter of the first '1' bit cell must be at peak (700 mV).
  const BiphaseEncoder enc(kPalSampleRate);
  const auto samples = enc.Generate24BitCode(0x88FFFFu, kPalBaseline, kPalPeak);

  const int bcs = enc.bit_cell_samples();
  const int three_q = (3 * bcs) / 4;
  for (int i = three_q; i < bcs; ++i) {
    EXPECT_NEAR(SampleFixedToMillivolts(samples[static_cast<std::size_t>(i)]),
                kPalPeak, 1.0)
        << "PAL lead-in first bit: sample " << i << " should be at peak 700 mV";
  }
}

TEST(BiphaseSystemTest, PalBaselineLevelIsBlankingLevel) {
  // IEC 60856 Figure 14: zero level is at blanking (0 mV).
  // The "30%–100%" in §10.1 is the allowed range for the high level only.
  EXPECT_NEAR(kPalBaseline, 0.0, 0.5);
}

TEST(BiphaseSystemTest, PalPeakLevelIs700mV) {
  // IEC 60856: peak = 100% white = 700 mV.
  EXPECT_NEAR(kPalPeak, 700.0, 0.5);
}

// ---------------------------------------------------------------------------
// BiphaseSystemTest — NTSC level mapping (IEC 60857: 0-100 IRE)
// ---------------------------------------------------------------------------

TEST(BiphaseSystemTest, NtscLeadOutWaveformFirstBitStartsAtBaseline) {
  // Lead-out 0x80EEEE first bit = '1' (MSB=1).
  const BiphaseEncoder enc(kNtscSampleRate);
  const auto samples =
      enc.Generate24BitCode(0x80EEEEu, kNtscBaseline, kNtscPeak);

  const int quarter = enc.bit_cell_samples() / 4;
  for (int i = 0; i < quarter; ++i) {
    EXPECT_NEAR(SampleFixedToMillivolts(samples[static_cast<std::size_t>(i)]),
                kNtscBaseline, 1.0)
        << "NTSC lead-out first bit: sample " << i << " should be at 0 mV";
  }
}

TEST(BiphaseSystemTest, NtscBaselineLevelIs0IRE) {
  // IEC 60857: baseline = 0 IRE = 0 mV.
  EXPECT_NEAR(kNtscBaseline, 0.0, 0.01);
}

TEST(BiphaseSystemTest, NtscPeakLevelIs100IRE) {
  // IEC 60857: peak = 100 IRE ≈ 714.3 mV.
  EXPECT_NEAR(kNtscPeak, 714.3, 1.0);
}

// ---------------------------------------------------------------------------
// BiphaseSystemTest — line buffer length compliance
// ---------------------------------------------------------------------------

TEST(BiphaseSystemTest, PalLineBufferIs1135Samples) {
  const BiphaseEncoder enc(kPalSampleRate);
  const auto line =
      enc.GenerateLine("88FFFF", Standard::kPal, kPalBaseline, kPalPeak);
  EXPECT_EQ(static_cast<int>(line.size()),
            GetTimingConstants(Standard::kPal).samples_per_line_4fsc);
  EXPECT_EQ(static_cast<int>(line.size()), 1135);
}

TEST(BiphaseSystemTest, NtscLineBufferIs910Samples) {
  const BiphaseEncoder enc(kNtscSampleRate);
  const auto line =
      enc.GenerateLine("80EEEE", Standard::kNtsc, kNtscBaseline, kNtscPeak);
  EXPECT_EQ(static_cast<int>(line.size()),
            GetTimingConstants(Standard::kNtsc).samples_per_line_4fsc);
  EXPECT_EQ(static_cast<int>(line.size()), 910);
}

// ---------------------------------------------------------------------------
// BiphaseSystemTest — 40-bit FM reference test vector
// ---------------------------------------------------------------------------

TEST(BiphaseSystemTest, FmBitPatternClockSyncIs0011) {
  // IEC 60857 Figure 13: bits [0-3] = 0011 (MSB first).
  const FmData data{false, 0, 0, 0, 0, 0};
  const auto bits = FmEncoder::BuildBitPattern(data);
  EXPECT_FALSE(bits[0]);
  EXPECT_FALSE(bits[1]);
  EXPECT_TRUE(bits[2]);
  EXPECT_TRUE(bits[3]);
}

TEST(BiphaseSystemTest, FmBitPatternLeadingRecognitionIs1110010) {
  // IEC 60857 Figure 13: bits [5-11] = 1110010.
  const FmData data{false, 0, 0, 0, 0, 0};
  const auto bits = FmEncoder::BuildBitPattern(data);
  EXPECT_TRUE(bits[5]);
  EXPECT_TRUE(bits[6]);
  EXPECT_TRUE(bits[7]);
  EXPECT_FALSE(bits[8]);
  EXPECT_FALSE(bits[9]);
  EXPECT_TRUE(bits[10]);
  EXPECT_FALSE(bits[11]);
}

TEST(BiphaseSystemTest, FmBitPatternTrailingRecognitionIs0001101) {
  // IEC 60857 Figure 13: bits [33-39] = 0001101.
  const FmData data{false, 0, 0, 0, 0, 0};
  const auto bits = FmEncoder::BuildBitPattern(data);
  EXPECT_FALSE(bits[33]);
  EXPECT_FALSE(bits[34]);
  EXPECT_FALSE(bits[35]);
  EXPECT_TRUE(bits[36]);
  EXPECT_TRUE(bits[37]);
  EXPECT_FALSE(bits[38]);
  EXPECT_TRUE(bits[39]);
}

TEST(BiphaseSystemTest, FmPictureNumber12345ProducesCorrectNibbles) {
  // fm_picture_number 12345 → x1=1, x2=2, x3=3, x4=4, x5=5.
  uint8_t x1, x2, x3, x4, x5;
  FmPictureNumberGenerator::EncodeValue(12345, x1, x2, x3, x4, x5);
  EXPECT_EQ(x1, 1u);
  EXPECT_EQ(x2, 2u);
  EXPECT_EQ(x3, 3u);
  EXPECT_EQ(x4, 4u);
  EXPECT_EQ(x5, 5u);
}

// ---------------------------------------------------------------------------
// BiphaseSystemTest — 40-bit FM waveform properties
// ---------------------------------------------------------------------------

TEST(BiphaseSystemTest, FmWaveformIs40BitCells) {
  const FmEncoder enc(kNtscSampleRate);
  const FmData data{false, 0, 0, 0, 0, 0};
  const auto samples =
      enc.Generate40BitWaveform(data, kNtscBaseline, kNtscPeak);
  EXPECT_EQ(static_cast<int>(samples.size()), enc.BitCellStart(40));
}

TEST(BiphaseSystemTest, FmLineBufferHasCorrectLength) {
  const FmEncoder enc(kNtscSampleRate);
  const FmData data{false, 0, 0, 0, 0, 0};
  const auto line =
      enc.Generate40BitCode(data, Standard::kNtsc, kNtscBaseline, kNtscPeak);
  EXPECT_EQ(static_cast<int>(line.size()),
            GetTimingConstants(Standard::kNtsc).samples_per_line_4fsc);
}

// IEC 60857 Figure 12: white flag pulse = 0.790H, tail at baseline.
// For NTSC 910 samples: flag_length = round(0.790 * 910) = 719.
TEST(BiphaseSystemTest, FmWhiteFlagPulseRegionAtPeakAndTailAtBaseline) {
  const FmEncoder enc(kNtscSampleRate);
  const auto line = enc.GenerateWhiteFlag(Standard::kNtsc, kNtscPeak);
  ASSERT_EQ(static_cast<int>(line.size()), 910);
  const int flag_length = static_cast<int>(std::round(0.790 * 910));
  const int skip = enc.ramp_samples() + 1;
  for (int i = skip; i < flag_length - skip; ++i) {
    EXPECT_NEAR(SampleFixedToMillivolts(line[static_cast<std::size_t>(i)]),
                kNtscPeak, 1.0)
        << "White flag interior sample " << i << " should be at peak";
  }
  for (std::size_t i = static_cast<std::size_t>(flag_length); i < line.size();
       ++i) {
    EXPECT_NEAR(SampleFixedToMillivolts(line[i]), 0.0, 1.0)
        << "White flag tail sample " << i << " should be at baseline";
  }
}

// ---------------------------------------------------------------------------
// BiphaseSystemTest — end-to-end: code presence in GenerateLine output
// ---------------------------------------------------------------------------

TEST(BiphaseSystemTest, PalLeadInLineContainsCode88FFFFAtStart) {
  const BiphaseEncoder enc(kPalSampleRate);
  const auto line =
      enc.GenerateLine("88FFFF", Standard::kPal, kPalBaseline, kPalPeak);
  const auto code = enc.Generate24BitCode(0x88FFFFu, kPalBaseline, kPalPeak);

  // The first BitCellStart(24) samples must match the code waveform.
  for (std::size_t i = 0; i < code.size(); ++i) {
    EXPECT_EQ(line[i], code[i])
        << "Mismatch at sample " << i << " in PAL lead-in line";
  }
}

TEST(BiphaseSystemTest, PalLeadInLineTailIsAtBaseline) {
  const BiphaseEncoder enc(kPalSampleRate);
  const auto line =
      enc.GenerateLine("88FFFF", Standard::kPal, kPalBaseline, kPalPeak);

  const std::size_t code_end = static_cast<std::size_t>(enc.BitCellStart(24));
  for (std::size_t i = code_end; i < line.size(); ++i) {
    EXPECT_NEAR(SampleFixedToMillivolts(line[i]), kPalBaseline, 0.01)
        << "PAL lead-in tail sample " << i << " should be at baseline";
  }
}

TEST(BiphaseSystemTest, NtscLeadOutLineContainsCode80EEEEAtStart) {
  const BiphaseEncoder enc(kNtscSampleRate);
  const auto line =
      enc.GenerateLine("80EEEE", Standard::kNtsc, kNtscBaseline, kNtscPeak);
  const auto code = enc.Generate24BitCode(0x80EEEEu, kNtscBaseline, kNtscPeak);

  for (std::size_t i = 0; i < code.size(); ++i) {
    EXPECT_EQ(line[i], code[i])
        << "Mismatch at sample " << i << " in NTSC lead-out line";
  }
}

// ---------------------------------------------------------------------------
// BiphaseSystemTest — 24-bit waveform total sample count
// ---------------------------------------------------------------------------

TEST(BiphaseSystemTest, Pal24BitCodeHas24BitCells) {
  const BiphaseEncoder enc(kPalSampleRate);
  const auto samples = enc.Generate24BitCode(0x88FFFFu, kPalBaseline, kPalPeak);
  EXPECT_EQ(static_cast<int>(samples.size()), enc.BitCellStart(24));
  // 24 × 35.469 = 851.27 samples, rounded.
  EXPECT_EQ(static_cast<int>(samples.size()), 851);
}

TEST(BiphaseSystemTest, Ntsc24BitCodeHas24BitCells) {
  const BiphaseEncoder enc(kNtscSampleRate);
  const auto samples =
      enc.Generate24BitCode(0x80EEEEu, kNtscBaseline, kNtscPeak);
  EXPECT_EQ(static_cast<int>(samples.size()), enc.BitCellStart(24));
  // 24 × 28.636 = 687.27 samples, rounded.
  EXPECT_EQ(static_cast<int>(samples.size()), 687);
}

// ---------------------------------------------------------------------------
// BiphaseSystemTest — performance benchmarking (task 6.4)
// ---------------------------------------------------------------------------

// Generating 10,000 PAL biphase lines must complete in < 2 seconds.
// Equivalent to 400 frames × 25 lines per frame of biphase content at PAL.
TEST(BiphaseSystemTest, PalBiphaseLineThroughputMeetsRequirement) {
  const BiphaseEncoder enc(kPalSampleRate);
  constexpr int kLineCount = 10000;

  const auto t_start = std::chrono::steady_clock::now();
  for (int i = 0; i < kLineCount; ++i) {
    volatile auto result =
        enc.GenerateLine("88FFFF", Standard::kPal, kPalBaseline, kPalPeak);
    (void)result;
  }
  const auto t_end = std::chrono::steady_clock::now();

  const double elapsed_ms =
      std::chrono::duration<double, std::milli>(t_end - t_start).count();
  EXPECT_LT(elapsed_ms, 2000.0)
      << "Generated " << kLineCount << " PAL lines in " << elapsed_ms
      << " ms (should be < 2000 ms)";
}

// Generating 10,000 NTSC biphase lines must complete in < 2 seconds.
TEST(BiphaseSystemTest, NtscBiphaseLineThroughputMeetsRequirement) {
  const BiphaseEncoder enc(kNtscSampleRate);
  constexpr int kLineCount = 10000;

  const auto t_start = std::chrono::steady_clock::now();
  for (int i = 0; i < kLineCount; ++i) {
    volatile auto result =
        enc.GenerateLine("80EEEE", Standard::kNtsc, kNtscBaseline, kNtscPeak);
    (void)result;
  }
  const auto t_end = std::chrono::steady_clock::now();

  const double elapsed_ms =
      std::chrono::duration<double, std::milli>(t_end - t_start).count();
  EXPECT_LT(elapsed_ms, 2000.0)
      << "Generated " << kLineCount << " NTSC lines in " << elapsed_ms
      << " ms (should be < 2000 ms)";
}

// Generating 10,000 NTSC 40-bit FM lines must complete in < 2 seconds.
TEST(BiphaseSystemTest, NtscFmLineThroughputMeetsRequirement) {
  const FmEncoder enc(kNtscSampleRate);
  const FmData data{false, 1, 2, 3, 4, 5};
  constexpr int kLineCount = 10000;

  const auto t_start = std::chrono::steady_clock::now();
  for (int i = 0; i < kLineCount; ++i) {
    volatile auto result =
        enc.Generate40BitCode(data, Standard::kNtsc, kNtscBaseline, kNtscPeak);
    (void)result;
  }
  const auto t_end = std::chrono::steady_clock::now();

  const double elapsed_ms =
      std::chrono::duration<double, std::milli>(t_end - t_start).count();
  EXPECT_LT(elapsed_ms, 2000.0)
      << "Generated " << kLineCount << " NTSC FM lines in " << elapsed_ms
      << " ms (should be < 2000 ms)";
}

// ---------------------------------------------------------------------------
// BiphaseSystemTest — IEC spec constants verification
// ---------------------------------------------------------------------------

TEST(BiphaseSystemTest, PalFrameRateIs25Hz) {
  EXPECT_NEAR(GetTimingConstants(Standard::kPal).frame_rate_hz, 25.0, 0.001);
}

TEST(BiphaseSystemTest, NtscFrameRateIsApprox2997Hz) {
  EXPECT_NEAR(GetTimingConstants(Standard::kNtsc).frame_rate_hz,
              30000.0 / 1001.0, 0.001);
}

TEST(BiphaseSystemTest, PalSampleRateIs17734475Hz) {
  EXPECT_NEAR(GetTimingConstants(Standard::kPal).sample_rate_4fsc_hz,
              17734475.0, 1.0);
}

TEST(BiphaseSystemTest, NtscSampleRateIs14318180Hz) {
  EXPECT_NEAR(GetTimingConstants(Standard::kNtsc).sample_rate_4fsc_hz,
              14318180.0, 1.0);
}

TEST(BiphaseSystemTest, PalHas625LinesPerFrame) {
  EXPECT_EQ(GetTimingConstants(Standard::kPal).lines_per_frame, 625);
}

TEST(BiphaseSystemTest, NtscHas525LinesPerFrame) {
  EXPECT_EQ(GetTimingConstants(Standard::kNtsc).lines_per_frame, 525);
}

}  // namespace
}  // namespace videosynth
