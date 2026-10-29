/*
 * File:        test_signal_timing_model.cpp
 * Module:      signal_timing_model_tests
 * Purpose:     Validates PAL and NTSC line timing classification logic.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <vector>

#include "videosynth/frame_line_layout.h"
#include "videosynth/signal_timing_model.h"

namespace videosynth {
namespace {

TEST(SignalTimingModelTest, AssignsFieldIndexFromSequentialLineNumber) {
  EXPECT_EQ(GetFieldIndex(Standard::kPal, 1), 1);
  EXPECT_EQ(GetFieldIndex(Standard::kPal, 312), 1);
  EXPECT_EQ(GetFieldIndex(Standard::kPal, 313), 2);
  EXPECT_EQ(GetFieldIndex(Standard::kPal, 625), 2);

  EXPECT_EQ(GetFieldIndex(Standard::kNtsc, 1), 1);
  EXPECT_EQ(GetFieldIndex(Standard::kNtsc, 262), 1);
  EXPECT_EQ(GetFieldIndex(Standard::kNtsc, 263), 2);
  EXPECT_EQ(GetFieldIndex(Standard::kNtsc, 525), 2);
}

TEST(SignalTimingModelTest, ClassifiesPalSyncBlocksAndLineKinds) {
  EXPECT_EQ(GetSyncPulseKind(Standard::kPal, 1), SyncPulseKind::kVerticalSync);
  EXPECT_EQ(GetSyncPulseKind(Standard::kPal, 5), SyncPulseKind::kEqualizing);
  // ITU-R BT.1700 Annex 1 Part B Table 2 items l/m/n: the post-equalizing
  // sequence ends before line 6, so line 6 is a normal burst-carrying line.
  EXPECT_EQ(GetSyncPulseKind(Standard::kPal, 6), SyncPulseKind::kHorizontal);
  EXPECT_EQ(GetSyncPulseKind(Standard::kPal, 16), SyncPulseKind::kHorizontal);
  EXPECT_EQ(GetSyncPulseKind(Standard::kPal, 311), SyncPulseKind::kEqualizing);
  EXPECT_EQ(GetSyncPulseKind(Standard::kPal, 314),
            SyncPulseKind::kVerticalSync);
  // Line 623 opens with a line-sync pulse; its equalizing pulse is mid-line.
  EXPECT_EQ(GetSyncPulseKind(Standard::kPal, 623), SyncPulseKind::kHorizontal);
  EXPECT_EQ(GetSyncPulseKind(Standard::kPal, 624), SyncPulseKind::kEqualizing);

  EXPECT_EQ(GetLineContentKind(Standard::kPal, 20),
            LineContentKind::kVbiBlanking);
  EXPECT_EQ(GetLineContentKind(Standard::kPal, 23),
            LineContentKind::kActivePicture);
  EXPECT_EQ(GetLineContentKind(Standard::kPal, 320),
            LineContentKind::kVbiBlanking);
  EXPECT_EQ(GetLineContentKind(Standard::kPal, 332),
            LineContentKind::kVbiBlanking);
  EXPECT_EQ(GetLineContentKind(Standard::kPal, 335),
            LineContentKind::kVbiBlanking);
  EXPECT_EQ(GetLineContentKind(Standard::kPal, 336),
            LineContentKind::kActivePicture);
  EXPECT_EQ(GetLineContentKind(Standard::kPal, 623),
            LineContentKind::kActivePicture);
}

TEST(SignalTimingModelTest, ClassifiesNtscSyncBlocksAndLineKinds) {
  EXPECT_EQ(GetSyncPulseKind(Standard::kNtsc, 1), SyncPulseKind::kEqualizing);
  EXPECT_EQ(GetSyncPulseKind(Standard::kNtsc, 4), SyncPulseKind::kVerticalSync);
  EXPECT_EQ(GetSyncPulseKind(Standard::kNtsc, 10), SyncPulseKind::kHorizontal);
  EXPECT_EQ(GetSyncPulseKind(Standard::kNtsc, 263), SyncPulseKind::kHorizontal);
  EXPECT_EQ(GetSyncPulseKind(Standard::kNtsc, 264), SyncPulseKind::kEqualizing);
  EXPECT_EQ(GetSyncPulseKind(Standard::kNtsc, 267),
            SyncPulseKind::kVerticalSync);
  EXPECT_EQ(GetSyncPulseKind(Standard::kNtsc, 523), SyncPulseKind::kHorizontal);

  EXPECT_EQ(GetLineContentKind(Standard::kNtsc, 20),
            LineContentKind::kVbiBlanking);
  EXPECT_EQ(GetLineContentKind(Standard::kNtsc, 22),
            LineContentKind::kActivePicture);
  EXPECT_EQ(GetLineContentKind(Standard::kNtsc, 263),
            LineContentKind::kVbiBlanking);
  EXPECT_EQ(GetLineContentKind(Standard::kNtsc, 283),
            LineContentKind::kVbiBlanking);
  EXPECT_EQ(GetLineContentKind(Standard::kNtsc, 284),
            LineContentKind::kActivePicture);
}

TEST(SignalTimingModelTest, ModelsHalfLinePairingAndBurstPolicy) {
  EXPECT_TRUE(HasTwoHalfLinePulses(SyncPulseKind::kEqualizing));
  EXPECT_TRUE(HasTwoHalfLinePulses(SyncPulseKind::kVerticalSync));
  EXPECT_FALSE(HasTwoHalfLinePulses(SyncPulseKind::kHorizontal));

  EXPECT_TRUE(BurstEnabledForLine(Standard::kPal, SyncPulseKind::kHorizontal));
  EXPECT_FALSE(BurstEnabledForLine(Standard::kPal, SyncPulseKind::kEqualizing));
  EXPECT_FALSE(
      BurstEnabledForLine(Standard::kPal, SyncPulseKind::kVerticalSync));
}

TEST(SignalTimingModelTest, SecamLinesCarryNoColourBurst) {
  // ITU-R BT.1700 Annex 1 Part C Table 4 item 13: SECAM identifies lines by
  // the undeviated back-porch subcarrier, not a burst.
  EXPECT_FALSE(
      BurstEnabledForLine(Standard::kSecam, SyncPulseKind::kHorizontal));
  for (const LineTimingPrimitive& line :
       BuildFrameTimingPrimitives(Standard::kSecam)) {
    ASSERT_FALSE(line.burst_enabled) << "line " << line.line_number_1based;
  }
}

TEST(SignalTimingModelTest, SecamSharesThe625LineStructure) {
  const std::vector<LineTimingPrimitive> pal =
      BuildFrameTimingPrimitives(Standard::kPal);
  const std::vector<LineTimingPrimitive> secam =
      BuildFrameTimingPrimitives(Standard::kSecam);
  ASSERT_EQ(secam.size(), pal.size());
  for (std::size_t i = 0; i < pal.size(); ++i) {
    EXPECT_EQ(secam[i].field_index_1based, pal[i].field_index_1based);
    EXPECT_EQ(secam[i].sync_pulse_kind, pal[i].sync_pulse_kind);
    EXPECT_EQ(secam[i].content_kind, pal[i].content_kind);
  }
}

TEST(SignalTimingModelTest,
     ModelsPalAlternatingAndNtscConstantBurstReferencePhase) {
  // NTSC line-to-line subcarrier progression is handled by absolute-time
  // synthesis in generation_stage. Timing model burst reference is constant per
  // line.
  const double ntsc_line20 = BurstPhaseRad(Standard::kNtsc, 20);
  const double ntsc_line21 = BurstPhaseRad(Standard::kNtsc, 21);
  const double ntsc_line22 = BurstPhaseRad(Standard::kNtsc, 22);
  EXPECT_DOUBLE_EQ(ntsc_line21, ntsc_line20);
  EXPECT_DOUBLE_EQ(ntsc_line22, ntsc_line20);

  // PAL burst phase alternates +135°/−135° line-to-line (ITU-R BT.1700 Figure
  // 8).
  const double pal_line20 = BurstPhaseRad(Standard::kPal, 20);
  const double pal_line21 = BurstPhaseRad(Standard::kPal, 21);
  EXPECT_DOUBLE_EQ(pal_line20, -pal_line21);
}

TEST(SignalTimingModelTest, PalSecondFieldVbiBoundaryStartsActiveAtLine336) {
  // ITU-R BT.1700 Annex 1 Part B Table 2 item j: field blanking (25H + a)
  // runs from line 311 to the end of line 335.
  EXPECT_EQ(GetLineContentKind(Standard::kPal, 335),
            LineContentKind::kVbiBlanking);
  EXPECT_EQ(GetLineContentKind(Standard::kPal, 336),
            LineContentKind::kActivePicture);
}

TEST(SignalTimingModelTest, PalActiveRasterPlacesField2On336To623) {
  const ActiveRasterGeometry geometry =
      GetActiveRasterGeometry(Standard::kPal, 17734475.0);

  EXPECT_EQ(ActiveFrameLineIndex(geometry, 310), 287);
  EXPECT_EQ(ActiveFrameLineIndex(geometry, 335), -1);
  EXPECT_EQ(ActiveFrameLineIndex(geometry, 336), 288);
  EXPECT_EQ(ActiveFrameLineIndex(geometry, 623), 575);
  EXPECT_EQ(ActiveFrameLineIndex(geometry, 624), -1);
}

TEST(SignalTimingModelTest, PalLine623PictureEndsBeforeMidLine) {
  const ActiveRasterGeometry geometry =
      GetActiveRasterGeometry(Standard::kPal, 17734475.0);

  // Item K: blanking leads the mid-line (32 us) equalizing pulse by 3 us.
  EXPECT_EQ(ActivePictureEndSamples(geometry, 623),
            static_cast<int>(std::lround(17734475.0 * 29.0e-6)));
  EXPECT_LT(ActivePictureEndSamples(geometry, 623), 1135 / 2);
  EXPECT_EQ(ActivePictureEndSamples(geometry, 622),
            geometry.active_window_end_samples);
}

TEST(SignalTimingModelTest, NtscActiveRasterHasNoHalfPictureLine) {
  const ActiveRasterGeometry geometry =
      GetActiveRasterGeometry(Standard::kNtsc, 14318181.8);

  EXPECT_EQ(geometry.half_picture_line, 0);
  EXPECT_EQ(ActivePictureEndSamples(geometry, 263),
            geometry.active_window_end_samples);
}

}  // namespace
}  // namespace videosynth
