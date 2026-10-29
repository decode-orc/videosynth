/*
 * File:        test_timing_constants.cpp
 * Module:      timing_constants_tests
 * Purpose:     Validates PAL and NTSC timing and signal level constants.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include "videosynth/timing_constants.h"

namespace videosynth {
namespace {

TEST(TimingConstantsTest, ProvidesPal4fscConstants) {
  const TimingConstants pal = GetTimingConstants(Standard::kPal);

  EXPECT_EQ(pal.lines_per_frame, 625);
  EXPECT_EQ(pal.samples_per_line_4fsc, 1135);
  EXPECT_DOUBLE_EQ(pal.frame_rate_hz, 25.0);
  EXPECT_DOUBLE_EQ(pal.sample_rate_4fsc_hz, 17734475.0);
}

TEST(TimingConstantsTest, ProvidesNtsc4fscConstants) {
  const TimingConstants ntsc = GetTimingConstants(Standard::kNtsc);

  EXPECT_EQ(ntsc.lines_per_frame, 525);
  EXPECT_EQ(ntsc.samples_per_line_4fsc, 910);
  EXPECT_DOUBLE_EQ(ntsc.frame_rate_hz, 30000.0 / 1001.0);
  EXPECT_DOUBLE_EQ(ntsc.sample_rate_4fsc_hz, 14318180.0);
}

TEST(SignalLevelsTest, ProvidesPalLevels) {
  const SignalLevels levels = GetSignalLevels(Standard::kPal);

  EXPECT_DOUBLE_EQ(levels.sync_tip_mv, -300.0);
  EXPECT_DOUBLE_EQ(levels.blanking_mv, 0.0);
  EXPECT_DOUBLE_EQ(levels.black_mv, 0.0);
  EXPECT_DOUBLE_EQ(levels.white_mv, 700.0);
}

TEST(SignalLevelsTest, ProvidesNtscLevels) {
  const SignalLevels levels = GetSignalLevels(Standard::kNtsc);

  EXPECT_DOUBLE_EQ(levels.sync_tip_mv, -285.7);
  EXPECT_DOUBLE_EQ(levels.blanking_mv, 0.0);
  EXPECT_DOUBLE_EQ(levels.black_mv, 53.6);
  EXPECT_DOUBLE_EQ(levels.white_mv, 714.3);
}

TEST(SignalLevelsTest, SupportsZeroIreNtscBlackSetup) {
  CvbsPresets presets;
  presets.video_standard_preset = Standard::kNtsc;
  presets.ntsc_black_setup_ire = 0.0;
  presets.ntsc_black_setup_ire_specified = true;

  const SignalLevels levels = GetSignalLevels(presets);

  EXPECT_DOUBLE_EQ(levels.sync_tip_mv, -285.7);
  EXPECT_DOUBLE_EQ(levels.blanking_mv, 0.0);
  EXPECT_DOUBLE_EQ(levels.black_mv, 0.0);
  EXPECT_DOUBLE_EQ(levels.white_mv, 714.3);
}

TEST(TimingConstantsTest,
     ResolvesSampleFrameCountsForSupportedOutputEncodings) {
  EXPECT_EQ(SamplesPerFrameForEncodingPreset(Standard::kPal, "CVBS_U16_4FSC"),
            static_cast<std::size_t>(SamplesPerFrame4fsc(Standard::kPal)));
  EXPECT_EQ(SamplesPerFrameForEncodingPreset(Standard::kPal, "RAW_S16_28M"),
            1120000U);
  EXPECT_EQ(SamplesPerFrameForEncodingPreset(Standard::kPal, "RAW_S16_40M"),
            1600000U);

  EXPECT_EQ(SamplesPerFrameForEncodingPreset(Standard::kNtsc, "RAW_S16_28M"),
            934267U);
  EXPECT_EQ(SamplesPerFrameForEncodingPreset(Standard::kNtsc, "RAW_S16_40M"),
            1334667U);
}

TEST(TimingConstantsTest, ProvidesPalAudioRateAndSamplesPerFrame) {
  EXPECT_DOUBLE_EQ(AudioSampleRateHz(Standard::kPal), 48000.0);
  EXPECT_EQ(AudioHeaderSampleRateHz(Standard::kPal), 48000);
  // PAL carries a constant 1920 samples in every frame (48000 / 25).
  for (int frame = 0; frame < 10; ++frame) {
    EXPECT_EQ(AudioSamplesForFrame(Standard::kPal, frame), 1920);
  }
}

TEST(TimingConstantsTest, ProvidesNtscAudioRateAndSamplesPerFrame) {
  EXPECT_DOUBLE_EQ(AudioSampleRateHz(Standard::kNtsc), 48000.0);
  EXPECT_EQ(AudioHeaderSampleRateHz(Standard::kNtsc), 48000);
  // SMPTE 272M §14.3 five-frame sequence: 1602, 1601, 1602, 1601, 1602.
  const int expected[5] = {1602, 1601, 1602, 1601, 1602};
  int sequence_total = 0;
  for (int frame = 0; frame < 5; ++frame) {
    EXPECT_EQ(AudioSamplesForFrame(Standard::kNtsc, frame), expected[frame]);
    // The sequence repeats every five frames.
    EXPECT_EQ(AudioSamplesForFrame(Standard::kNtsc, frame + 5),
              expected[frame]);
    sequence_total += expected[frame];
  }
  EXPECT_EQ(sequence_total, 8008);
}

TEST(TimingConstantsTest, ProvidesPalMAudioRateAndSamplesPerFrame) {
  EXPECT_DOUBLE_EQ(AudioSampleRateHz(Standard::kPalM), 48000.0);
  EXPECT_EQ(AudioHeaderSampleRateHz(Standard::kPalM), 48000);
  const int expected[5] = {1602, 1601, 1602, 1601, 1602};
  for (int frame = 0; frame < 5; ++frame) {
    EXPECT_EQ(AudioSamplesForFrame(Standard::kPalM, frame), expected[frame]);
  }
}

TEST(TimingConstantsTest, ProvidesPalEfmAudioSamplesPerFrame) {
  EXPECT_DOUBLE_EQ(EfmAudioSampleRateHz(), 44100.0);
  // IEC 60856-1986 Amd 2 13.2: Fs = 1764/625 x fH, i.e. 1764 samples per
  // 25 Hz frame exactly, for every frame.
  for (int frame = 0; frame < 200; ++frame) {
    EXPECT_EQ(EfmAudioSamplesForFrame(Standard::kPal, frame), 1764);
  }
}

TEST(TimingConstantsTest, NtscEfmAudioSequenceMatchesSmpte272mTable1) {
  // SMPTE 272M-1994 Section 14.3 Table 1: 44.1 kHz, 100-frame sequence, odd
  // frames 1472 and even frames 1471, with frame numbers 23, 47 and 71
  // reduced to 1471.
  for (int frame_number = 1; frame_number <= 100; ++frame_number) {
    const bool is_exception =
        frame_number == 23 || frame_number == 47 || frame_number == 71;
    const int expected = (frame_number % 2 == 0 || is_exception) ? 1471 : 1472;
    EXPECT_EQ(EfmAudioSamplesForFrame(Standard::kNtsc, frame_number - 1),
              expected)
        << "audio frame number " << frame_number;
    // The sequence repeats every 100 output frames.
    EXPECT_EQ(EfmAudioSamplesForFrame(Standard::kNtsc, frame_number + 99),
              expected);
  }
}

TEST(TimingConstantsTest, AnyHundredNtscEfmFramesCarry147147Samples) {
  for (std::int64_t start = 0; start < 250; ++start) {
    std::int64_t total = 0;
    for (std::int64_t frame = start; frame < start + 100; ++frame) {
      total += EfmAudioSamplesForFrame(Standard::kNtsc, frame);
    }
    EXPECT_EQ(total, 147147) << "sequence starting at frame " << start;
  }
}

TEST(TimingConstantsTest, PalMEfmAudioFollowsTheNtscSequence) {
  for (std::int64_t frame = 0; frame < 200; ++frame) {
    EXPECT_EQ(EfmAudioSamplesForFrame(Standard::kPalM, frame),
              EfmAudioSamplesForFrame(Standard::kNtsc, frame))
        << frame;
  }
}

TEST(TimingConstantsTest, EfmAudioRejectsStandardsWithoutLaserDiscAudio) {
  EXPECT_THROW(EfmAudioSamplesForFrame(Standard::kSecam, 0),
               std::invalid_argument);
  EXPECT_THROW(EfmAudioSamplesForFrame(Standard::kUnknown, 0),
               std::invalid_argument);
  EXPECT_THROW(EfmAudioSamplesForFrame(Standard::kPal, -1),
               std::invalid_argument);
}

}  // namespace
}  // namespace videosynth
