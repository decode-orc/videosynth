/*
 * File:        test_fixed_point.cpp
 * Module:      fixed_point_tests
 * Purpose:     Validates fixed-point rounding and the millivolt-to-code
 *              mapping for values on both sides of blanking.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <cmath>

#include "videosynth/cvbs_quantization.h"
#include "videosynth/fixed_point.h"

namespace videosynth {
namespace {

// Four fraction bits: one output step is 16 input units.
constexpr int kShift = 4;

TEST(FixedPointTest, RoundShiftRightSignedRoundsHalfAwayFromZero) {
  EXPECT_EQ(RoundShiftRightSigned(0, kShift), 0);
  EXPECT_EQ(RoundShiftRightSigned(7, kShift), 0);
  EXPECT_EQ(RoundShiftRightSigned(8, kShift), 1);
  EXPECT_EQ(RoundShiftRightSigned(24, kShift), 2);
  EXPECT_EQ(RoundShiftRightSigned(-7, kShift), 0);
  EXPECT_EQ(RoundShiftRightSigned(-8, kShift), -1);
  EXPECT_EQ(RoundShiftRightSigned(-24, kShift), -2);
}

TEST(FixedPointTest, RoundShiftRightSignedIsSymmetricAboutZero) {
  for (std::int64_t value = 0; value < 200; ++value) {
    EXPECT_EQ(RoundShiftRightSigned(-value, kShift),
              -RoundShiftRightSigned(value, kShift))
        << "value=" << value;
  }
}

TEST(FixedPointTest, RoundShiftRightSignedMatchesLlround) {
  for (std::int64_t value = -100; value <= 100; ++value) {
    const double exact = static_cast<double>(value) / (1 << kShift);
    EXPECT_EQ(RoundShiftRightSigned(value, kShift), std::llround(exact))
        << "value=" << value;
  }
}

TEST(FixedPointTest, NtscSyncTipMapsToCode16) {
  QuantizationProfile profile;
  ASSERT_TRUE(BuildQuantizationProfile(Standard::kNtsc, &profile));

  // ITU-R BT.470-6 Table 1 item 4: System M sync tip is -40 IRE
  // (-285.7 mV), which the 4fsc quantisation places at code 16.
  EXPECT_EQ(MapCompositeFixedToCode(MillivoltsToSampleFixed(-285.7), profile),
            16);
}

TEST(FixedPointTest, FixedCodeMappingMatchesDoublePathBelowBlanking) {
  QuantizationProfile profile;
  ASSERT_TRUE(BuildQuantizationProfile(Standard::kPal, &profile));

  for (double mv = -300.0; mv <= 300.0; mv += 0.37) {
    EXPECT_EQ(MapCompositeFixedToCode(MillivoltsToSampleFixed(mv), profile),
              MapCompositeMillivoltsToCode(mv, profile))
        << "mv=" << mv;
  }
}

}  // namespace
}  // namespace videosynth
