/*
 * File:        test_ntsc_frozen.cpp
 * Module:      ntsc_frozen_tests
 * Purpose:     Unit tests for NTSC frozen value behaviour in 40-bit FM codes.
 *              Verifies IEC 60857 Appendix F frozen value rules:
 *              - Lead-in: picture_number = 0, programme_time = 0:00
 *              - Lead-out: values frozen at last programme_area values
 *              - Programme status code builder with Hamming check
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <cstdint>

#include "videosynth/biphase_types.h"
#include "videosynth/fm_code_generator.h"
#include "videosynth/fm_encoder.h"
#include "videosynth/status_code_generator.h"

namespace videosynth {
namespace {

// ---------------------------------------------------------------------------
// NtscFrozenValuesTest — FmPictureNumberGenerator
// ---------------------------------------------------------------------------

TEST(NtscFrozenValuesTest, FmPictureNumberIsZeroInLeadIn) {
  // IEC 60857 Appendix F: picture_number = 0 during lead-in.
  FmPictureNumberGenerator gen(1);
  const FmData data =
      gen.CurrentData(/*field_one=*/false, SectionType::kLeadIn);
  EXPECT_EQ(data.x1, 0u);
  EXPECT_EQ(data.x2, 0u);
  EXPECT_EQ(data.x3, 0u);
  EXPECT_EQ(data.x4, 0u);
  EXPECT_EQ(data.x5, 0u);
}

TEST(NtscFrozenValuesTest, FmPictureNumberDoesNotAdvanceInLeadIn) {
  FmPictureNumberGenerator gen(1);
  gen.Advance(SectionType::kLeadIn);
  gen.Advance(SectionType::kLeadIn);
  // Programme value must not have changed.
  EXPECT_EQ(gen.current_programme_value(), 1);
}

TEST(NtscFrozenValuesTest, FmPictureNumberIncrementsInProgrammeArea) {
  FmPictureNumberGenerator gen(1);
  gen.Advance(SectionType::kProgrammeArea);
  EXPECT_EQ(gen.current_programme_value(), 2);
  gen.Advance(SectionType::kProgrammeArea);
  EXPECT_EQ(gen.current_programme_value(), 3);
}

TEST(NtscFrozenValuesTest, FmPictureNumberEmitsCurrentValueInProgrammeArea) {
  FmPictureNumberGenerator gen(100);
  const FmData data =
      gen.CurrentData(/*field_one=*/false, SectionType::kProgrammeArea);
  // 100 = 0 0 1 0 0 digits
  EXPECT_EQ(data.x1, 0u);
  EXPECT_EQ(data.x2, 0u);
  EXPECT_EQ(data.x3, 1u);
  EXPECT_EQ(data.x4, 0u);
  EXPECT_EQ(data.x5, 0u);
}

TEST(NtscFrozenValuesTest, FmPictureNumberFrozenAtLastProgrammeValueInLeadOut) {
  // IEC 60857 Appendix F: lead-out emits the last programme area value.
  FmPictureNumberGenerator gen(1);
  for (int i = 0; i < 50; ++i) {
    gen.Advance(SectionType::kProgrammeArea);
  }
  // Programme value is now 51.
  EXPECT_EQ(gen.current_programme_value(), 51);
  EXPECT_EQ(gen.frozen_lead_out_value(), 50);

  // Lead-out must emit the frozen value (50), not the current programme value.
  const FmData lead_out_data =
      gen.CurrentData(/*field_one=*/false, SectionType::kLeadOut);
  EXPECT_EQ(lead_out_data.x1, 0u);
  EXPECT_EQ(lead_out_data.x2, 0u);
  EXPECT_EQ(lead_out_data.x3, 0u);
  EXPECT_EQ(lead_out_data.x4, 5u);
  EXPECT_EQ(lead_out_data.x5, 0u);
}

TEST(NtscFrozenValuesTest, FmPictureNumberDoesNotAdvanceInLeadOut) {
  FmPictureNumberGenerator gen(1);
  gen.Advance(SectionType::kProgrammeArea);
  // frozen value = 1, current = 2
  const int frozen_before = gen.frozen_lead_out_value();

  gen.Advance(SectionType::kLeadOut);
  gen.Advance(SectionType::kLeadOut);

  EXPECT_EQ(gen.frozen_lead_out_value(), frozen_before);
  EXPECT_EQ(gen.current_programme_value(), 2);
}

TEST(NtscFrozenValuesTest, FmPictureNumberResetRestoresInitialState) {
  FmPictureNumberGenerator gen(5);
  for (int i = 0; i < 20; ++i) {
    gen.Advance(SectionType::kProgrammeArea);
  }
  gen.Reset();
  EXPECT_EQ(gen.current_programme_value(), 5);
  EXPECT_EQ(gen.frozen_lead_out_value(), 0);
}

TEST(NtscFrozenValuesTest, FmPictureNumberFieldOneSetCorrectly) {
  FmPictureNumberGenerator gen(1);
  const FmData field1 = gen.CurrentData(true, SectionType::kProgrammeArea);
  const FmData field2 = gen.CurrentData(false, SectionType::kProgrammeArea);
  EXPECT_TRUE(field1.field_one);
  EXPECT_FALSE(field2.field_one);
}

TEST(NtscFrozenValuesTest, FmPictureNumberSaturatesAtMax) {
  FmPictureNumberGenerator gen(FmPictureNumberGenerator::kNtscMaxValue);
  gen.Advance(SectionType::kProgrammeArea);
  gen.Advance(SectionType::kProgrammeArea);
  EXPECT_EQ(gen.current_programme_value(),
            FmPictureNumberGenerator::kNtscMaxValue);
}

TEST(NtscFrozenValuesTest, FmPictureNumberEncodeValueDecimals) {
  uint8_t x1, x2, x3, x4, x5;
  FmPictureNumberGenerator::EncodeValue(12345, x1, x2, x3, x4, x5);
  EXPECT_EQ(x1, 1u);
  EXPECT_EQ(x2, 2u);
  EXPECT_EQ(x3, 3u);
  EXPECT_EQ(x4, 4u);
  EXPECT_EQ(x5, 5u);
}

TEST(NtscFrozenValuesTest, FmPictureNumberEncodeValueZero) {
  uint8_t x1, x2, x3, x4, x5;
  FmPictureNumberGenerator::EncodeValue(0, x1, x2, x3, x4, x5);
  EXPECT_EQ(x1, 0u);
  EXPECT_EQ(x2, 0u);
  EXPECT_EQ(x3, 0u);
  EXPECT_EQ(x4, 0u);
  EXPECT_EQ(x5, 0u);
}

TEST(NtscFrozenValuesTest, FmPictureNumberIsValidRange) {
  EXPECT_TRUE(FmPictureNumberGenerator::IsValidValue(0));
  EXPECT_TRUE(FmPictureNumberGenerator::IsValidValue(79999));
  EXPECT_FALSE(FmPictureNumberGenerator::IsValidValue(-1));
  EXPECT_FALSE(FmPictureNumberGenerator::IsValidValue(80000));
}

// ---------------------------------------------------------------------------
// NtscFrozenValuesTest — FmProgrammeTimeGenerator
// ---------------------------------------------------------------------------

TEST(NtscFrozenValuesTest, FmProgrammeTimeIsZeroZeroInLeadIn) {
  // IEC 60857 Appendix F: programme_time = 0:00 during lead-in.
  FmProgrammeTimeGenerator gen;
  const FmData data =
      gen.CurrentData(/*field_one=*/false, SectionType::kLeadIn);
  EXPECT_EQ(data.x1, 0u);  // minutes tens
  EXPECT_EQ(data.x2, 0u);  // minutes units
  EXPECT_EQ(data.x3, 0u);  // seconds tens
  EXPECT_EQ(data.x4, 0u);  // seconds units
}

TEST(NtscFrozenValuesTest, FmProgrammeTimeModeIsLeadInDuringLeadIn) {
  FmProgrammeTimeGenerator gen;
  const FmData data =
      gen.CurrentData(/*field_one=*/false, SectionType::kLeadIn);
  EXPECT_EQ(data.x5, FmProgrammeTimeGenerator::kModeLeadIn);
}

TEST(NtscFrozenValuesTest, FmProgrammeTimeModeIsPictureDuringProgrammeArea) {
  FmProgrammeTimeGenerator gen;
  const FmData data =
      gen.CurrentData(/*field_one=*/false, SectionType::kProgrammeArea);
  EXPECT_EQ(data.x5, FmProgrammeTimeGenerator::kModePicture);
}

TEST(NtscFrozenValuesTest, FmProgrammeTimeModeIsLeadOutDuringLeadOut) {
  FmProgrammeTimeGenerator gen;
  const FmData data =
      gen.CurrentData(/*field_one=*/false, SectionType::kLeadOut);
  EXPECT_EQ(data.x5, FmProgrammeTimeGenerator::kModeLeadOut);
}

TEST(NtscFrozenValuesTest, FmProgrammeTimeDoesNotAdvanceInLeadIn) {
  FmProgrammeTimeGenerator gen;
  gen.Advance(SectionType::kLeadIn);
  gen.Advance(SectionType::kLeadIn);
  EXPECT_EQ(gen.total_programme_frames(), 0);
}

TEST(NtscFrozenValuesTest, FmProgrammeTimeAdvancesInProgrammeArea) {
  FmProgrammeTimeGenerator gen;
  gen.Advance(SectionType::kProgrammeArea);
  EXPECT_EQ(gen.total_programme_frames(), 1);
}

TEST(NtscFrozenValuesTest, FmProgrammeTimeIncrementsSecondsAt30Frames) {
  // NTSC: 30 fps nominal.
  FmProgrammeTimeGenerator gen;
  for (int i = 0; i < 30; ++i) {
    gen.Advance(SectionType::kProgrammeArea);
  }
  EXPECT_EQ(gen.current_seconds(), 1);
  EXPECT_EQ(gen.current_minutes(), 0);
}

TEST(NtscFrozenValuesTest, FmProgrammeTimeIncrementsMinutesAt1800Frames) {
  FmProgrammeTimeGenerator gen;
  for (int i = 0; i < 1800; ++i) {
    gen.Advance(SectionType::kProgrammeArea);
  }
  EXPECT_EQ(gen.current_minutes(), 1);
  EXPECT_EQ(gen.current_seconds(), 0);
}

TEST(NtscFrozenValuesTest, FmProgrammeTimeFrozenAtLastProgrammeValueInLeadOut) {
  // IEC 60857 Appendix F: lead-out emits the last programme area time.
  FmProgrammeTimeGenerator gen;
  // Advance to 1 minute 5 seconds (1800 + 150 = 1950 frames).
  for (int i = 0; i < 1950; ++i) {
    gen.Advance(SectionType::kProgrammeArea);
  }
  EXPECT_EQ(gen.current_minutes(), 1);
  EXPECT_EQ(gen.current_seconds(), 5);

  // Enter lead-out (no advance).
  const FmData lead_out_data =
      gen.CurrentData(/*field_one=*/false, SectionType::kLeadOut);
  EXPECT_EQ(lead_out_data.x5, FmProgrammeTimeGenerator::kModeLeadOut);
  // Minutes BCD: 1 → tens=0, units=1.
  EXPECT_EQ(lead_out_data.x1, 0u);
  EXPECT_EQ(lead_out_data.x2, 1u);
  // Seconds BCD: 5 → tens=0, units=5.
  EXPECT_EQ(lead_out_data.x3, 0u);
  EXPECT_EQ(lead_out_data.x4, 5u);
}

TEST(NtscFrozenValuesTest, FmProgrammeTimeDoesNotAdvanceInLeadOut) {
  FmProgrammeTimeGenerator gen;
  for (int i = 0; i < 30; ++i) {
    gen.Advance(SectionType::kProgrammeArea);
  }
  const int frames_before = gen.total_programme_frames();

  gen.Advance(SectionType::kLeadOut);
  gen.Advance(SectionType::kLeadOut);

  EXPECT_EQ(gen.total_programme_frames(), frames_before);
}

TEST(NtscFrozenValuesTest, FmProgrammeTimeResetRestoresInitialState) {
  FmProgrammeTimeGenerator gen;
  for (int i = 0; i < 1800; ++i) {
    gen.Advance(SectionType::kProgrammeArea);
  }
  gen.Reset();
  EXPECT_EQ(gen.total_programme_frames(), 0);
  EXPECT_EQ(gen.current_minutes(), 0);
  EXPECT_EQ(gen.current_seconds(), 0);
  EXPECT_EQ(gen.frozen_minutes(), 0);
  EXPECT_EQ(gen.frozen_seconds(), 0);
}

TEST(NtscFrozenValuesTest, FmProgrammeTimeBuildTimeDataBcdEncoding) {
  // Verify BCD encoding: minutes=35, seconds=47.
  const FmData data =
      FmProgrammeTimeGenerator::BuildTimeData(false, 35, 47, 0xD);
  EXPECT_EQ(data.x1, 3u);  // minutes tens
  EXPECT_EQ(data.x2, 5u);  // minutes units
  EXPECT_EQ(data.x3, 4u);  // seconds tens
  EXPECT_EQ(data.x4, 7u);  // seconds units
  EXPECT_EQ(data.x5, 0xDu);
}

TEST(NtscFrozenValuesTest, FmProgrammeTimeFieldOneSetCorrectly) {
  FmProgrammeTimeGenerator gen;
  const FmData field1 = gen.CurrentData(true, SectionType::kProgrammeArea);
  const FmData field2 = gen.CurrentData(false, SectionType::kProgrammeArea);
  EXPECT_TRUE(field1.field_one);
  EXPECT_FALSE(field2.field_one);
}

// ---------------------------------------------------------------------------
// NtscFrozenValuesTest — ProgrammeStatusCodeBuilder (task 3.6)
// ---------------------------------------------------------------------------

TEST(NtscFrozenValuesTest, StatusCodeCxOnHasDcPrefix) {
  ProgrammeStatusCodeBuilder builder(CxMode::kOn, false, 0x0);
  const uint32_t code = builder.Build();
  // Key nibble (bits 23-20) = 8.
  EXPECT_EQ((code >> 20) & 0xFu, 0x8u);
  // CX on: nibble 1 (bits 19-16) = D, nibble 2 (bits 15-12) = C.
  EXPECT_EQ((code >> 16) & 0xFu, 0xDu);
  EXPECT_EQ((code >> 12) & 0xFu, 0xCu);
}

TEST(NtscFrozenValuesTest, StatusCodeCxOffHasBaPrefix) {
  ProgrammeStatusCodeBuilder builder(CxMode::kOff, false, 0x0);
  const uint32_t code = builder.Build();
  EXPECT_EQ((code >> 20) & 0xFu, 0x8u);
  // CX off: nibble 1 = B, nibble 2 = A.
  EXPECT_EQ((code >> 16) & 0xFu, 0xBu);
  EXPECT_EQ((code >> 12) & 0xFu, 0xAu);
}

TEST(NtscFrozenValuesTest, StatusCodeCopyPermittedSetsX3Lsb) {
  ProgrammeStatusCodeBuilder builder_permit(CxMode::kOn, true, 0x0);
  ProgrammeStatusCodeBuilder builder_prohibit(CxMode::kOn, false, 0x0);

  const uint32_t code_permit = builder_permit.Build();
  const uint32_t code_prohibit = builder_prohibit.Build();

  // X₃ LSB (bit 8) = copy_permitted.
  EXPECT_EQ((code_permit >> 8) & 0x1u, 1u);
  EXPECT_EQ((code_prohibit >> 8) & 0x1u, 0u);
}

TEST(NtscFrozenValuesTest, StatusCodeX4ContainsAudioVideoMode) {
  ProgrammeStatusCodeBuilder builder(CxMode::kOn, false, 0x3u);
  const uint32_t code = builder.Build();
  EXPECT_EQ((code >> 4) & 0xFu, 0x3u);
}

TEST(NtscFrozenValuesTest, StatusCodeKeyNibbleIsAlwaysOne) {
  // IEC 60856/60857: bit 23 must be 1.
  ProgrammeStatusCodeBuilder builder(CxMode::kOff, true, 0x8u);
  EXPECT_NE(builder.Build() & 0x800000u, 0u);
}

TEST(NtscFrozenValuesTest, HammingCheckModeZeroIsZero) {
  // For X₄ = 0x0 (all zeros), all parity bits are 0.
  EXPECT_EQ(ProgrammeStatusCodeBuilder::ComputeHammingCheck(0x0u), 0x0u);
}

TEST(NtscFrozenValuesTest, HammingCheckIsConsistentWithBuildOutput) {
  for (uint8_t x4 = 0; x4 < 16; ++x4) {
    const uint8_t expected_x5 =
        ProgrammeStatusCodeBuilder::ComputeHammingCheck(x4);
    ProgrammeStatusCodeBuilder builder(CxMode::kOn, false, x4);
    const uint32_t code = builder.Build();
    EXPECT_EQ(code & 0xFu, expected_x5) << "x4 = " << static_cast<int>(x4);
  }
}

TEST(NtscFrozenValuesTest, HammingCheckAllOnesX4) {
  // X₄ = 0xF (d1=d2=d3=d4=1):
  // p1 = 1^1^1 = 1, p2 = 1^1^1 = 1, p3 = 1^1^1 = 1, p4 = 1^1^1 = 1.
  EXPECT_EQ(ProgrammeStatusCodeBuilder::ComputeHammingCheck(0xFu), 0xFu);
}

TEST(NtscFrozenValuesTest, HammingCheckSingleBitD1) {
  // X₄ = 0x8 (d1=1, d2=d3=d4=0):
  // p1 = 1^0^0 = 1, p2 = 1^0^0 = 1, p3 = 0^0^0 = 0, p4 = 1^0^0^0 = 1.
  // X₅ = 1101b = 0xD.
  EXPECT_EQ(ProgrammeStatusCodeBuilder::ComputeHammingCheck(0x8u), 0xDu);
}

TEST(NtscFrozenValuesTest, HammingCheckSingleBitD4) {
  // X₄ = 0x1 (d1=d2=d3=0, d4=1):
  // p1 = 0^0^1 = 1, p2 = 0^0^1 = 1, p3 = 0^0^1 = 1, p4 = 0^0^0 = 0.
  // X₅ = 1110b = 0xE.
  EXPECT_EQ(ProgrammeStatusCodeBuilder::ComputeHammingCheck(0x1u), 0xEu);
}

TEST(NtscFrozenValuesTest, IsDefinedAudioVideoModeAcceptsModes0To3) {
  for (uint8_t m = 0; m <= 3; ++m) {
    EXPECT_TRUE(ProgrammeStatusCodeBuilder::IsDefinedAudioVideoMode(m))
        << "Mode " << static_cast<int>(m) << " should be defined";
  }
}

TEST(NtscFrozenValuesTest, IsDefinedAudioVideoModeAcceptsMode8) {
  EXPECT_TRUE(ProgrammeStatusCodeBuilder::IsDefinedAudioVideoMode(8u));
}

TEST(NtscFrozenValuesTest, IsDefinedAudioVideoModeRejectsFutureModes) {
  for (uint8_t m = 4; m <= 7; ++m) {
    EXPECT_FALSE(ProgrammeStatusCodeBuilder::IsDefinedAudioVideoMode(m))
        << "Mode " << static_cast<int>(m) << " should be future-use";
  }
  for (uint8_t m = 9; m <= 15; ++m) {
    EXPECT_FALSE(ProgrammeStatusCodeBuilder::IsDefinedAudioVideoMode(m))
        << "Mode " << static_cast<int>(m) << " should be future-use";
  }
}

TEST(NtscFrozenValuesTest, BuildX3ReflectsCopyPermission) {
  EXPECT_EQ(ProgrammeStatusCodeBuilder::BuildX3(true), 0x01u);
  EXPECT_EQ(ProgrammeStatusCodeBuilder::BuildX3(false), 0x00u);
}

}  // namespace
}  // namespace videosynth
