/*
 * File:        test_efm_stream_encoder.cpp
 * Module:      efm
 * Purpose:     Unit tests for the EFM stream encoder facade, decoding its
 *              T-value output back to subcode and audio samples.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include "efm_channel_decoder.h"
#include "videosynth/efm/efm_stream_encoder.h"

namespace videosynth::efm {
namespace {

using test_support::DecodedFrame;
using test_support::Deinterleave;
using test_support::ExtractSamples;
using test_support::TestChannelDecoder;

// A track table with a lead-in, two programme tracks and a lead-out, sized in
// subcode sections (75 per second, IEC 60908-1999, 17.3).
TrackTable MakeTrackTable(std::size_t lead_in_sections,
                          std::size_t track_sections,
                          std::size_t lead_out_sections) {
  TrackTable table;
  table.video_system = VideoSystem::kPal;
  table.entries.push_back(
      TrackTableEntry{SubcodeArea::kLeadIn, 0, 0, lead_in_sections});
  table.entries.push_back(TrackTableEntry{SubcodeArea::kProgramme, 1,
                                          lead_in_sections, track_sections});
  table.entries.push_back(TrackTableEntry{SubcodeArea::kProgramme, 2,
                                          lead_in_sections + track_sections,
                                          track_sections});
  table.entries.push_back(TrackTableEntry{
      SubcodeArea::kLeadOut, 0, lead_in_sections + (2 * track_sections),
      lead_out_sections});
  return table;
}

// A deterministic stereo test signal.
void MakeSamples(std::size_t count, std::vector<std::int16_t>* left,
                 std::vector<std::int16_t>* right) {
  for (std::size_t index = 0; index < count; ++index) {
    left->push_back(static_cast<std::int16_t>((index * 251U) % 65536U - 32768));
    right->push_back(
        static_cast<std::int16_t>((index * 937U) % 65536U - 32768));
  }
}

std::vector<std::uint8_t> EncodeStream(const TrackTable& table,
                                       const std::vector<std::int16_t>& left,
                                       const std::vector<std::int16_t>& right) {
  EfmStreamEncoder encoder;
  EXPECT_TRUE(encoder.Begin(table));
  std::vector<std::uint8_t> t_values;
  EXPECT_TRUE(encoder.PushSamples(left, right, &t_values));
  EXPECT_TRUE(encoder.Flush(&t_values));
  return t_values;
}

TEST(EfmStreamEncoderTest, BeginRejectsAnInvalidTrackTable) {
  EfmStreamEncoder encoder;
  TrackTable table;

  // Empty, and then a table whose entries do not tile the timeline.
  EXPECT_FALSE(encoder.Begin(table));
  table.entries.push_back(TrackTableEntry{SubcodeArea::kProgramme, 1, 5, 10});
  EXPECT_FALSE(encoder.Begin(table));

  std::vector<std::uint8_t> t_values;
  EXPECT_FALSE(encoder.PushSamples({0}, {0}, &t_values));
  EXPECT_FALSE(encoder.Flush(&t_values));
}

TEST(EfmStreamEncoderTest, RejectsMismatchedChannelsAndNullOutput) {
  EfmStreamEncoder encoder;
  ASSERT_TRUE(encoder.Begin(MakeTrackTable(75, 300, 75)));

  std::vector<std::uint8_t> t_values;
  EXPECT_FALSE(encoder.PushSamples({0, 1}, {0}, &t_values));
  EXPECT_FALSE(encoder.PushSamples({0}, {0}, nullptr));
  EXPECT_FALSE(encoder.Flush(nullptr));
  EXPECT_TRUE(t_values.empty());
}

TEST(EfmStreamEncoderTest, EmitsOneChannelFrameForEverySixStereoSamples) {
  const TrackTable table = MakeTrackTable(75, 300, 75);
  std::vector<std::int16_t> left;
  std::vector<std::int16_t> right;
  MakeSamples(std::size_t{6} * 500U, &left, &right);

  EfmStreamEncoder encoder;
  ASSERT_TRUE(encoder.Begin(table));
  std::vector<std::uint8_t> t_values;
  ASSERT_TRUE(encoder.PushSamples(left, right, &t_values));
  EXPECT_EQ(encoder.ChannelFrameCount(), 500U);

  ASSERT_TRUE(encoder.Flush(&t_values));
  // The flush adds the CIRC drain in silent frames.
  EXPECT_EQ(encoder.ChannelFrameCount(), 500U + kCircDrainFrames);
}

TEST(EfmStreamEncoderTest, TValuesSpanOnlyTMinToTMax) {
  std::vector<std::int16_t> left;
  std::vector<std::int16_t> right;
  MakeSamples(std::size_t{6} * 400U, &left, &right);

  const std::vector<std::uint8_t> t_values =
      EncodeStream(MakeTrackTable(75, 300, 75), left, right);

  ASSERT_FALSE(t_values.empty());
  for (std::size_t index = 0; index < t_values.size(); ++index) {
    ASSERT_GE(t_values[index], kMinRunLengthT) << "T value " << index;
    ASSERT_LE(t_values[index], kMaxRunLengthT) << "T value " << index;
  }
}

TEST(EfmStreamEncoderTest, OutputIsAPureFunctionOfTheInput) {
  const TrackTable table = MakeTrackTable(75, 300, 75);
  std::vector<std::int16_t> left;
  std::vector<std::int16_t> right;
  MakeSamples(std::size_t{6} * 300U, &left, &right);

  EXPECT_EQ(EncodeStream(table, left, right), EncodeStream(table, left, right));
}

TEST(EfmStreamEncoderTest, DecodedSubcodeMatchesTheTrackTable) {
  const TrackTable table = MakeTrackTable(75, 300, 75);
  // Enough samples to cover several whole subcode sections.
  std::vector<std::int16_t> left;
  std::vector<std::int16_t> right;
  MakeSamples(6U * kFramesPerSubcodeSection * 5U, &left, &right);

  const std::vector<DecodedFrame> frames =
      TestChannelDecoder().Decode(EncodeStream(table, left, right));
  ASSERT_GE(frames.size(), kFramesPerSubcodeSection * 5U);

  SubcodeGenerator generator;
  ASSERT_TRUE(generator.Begin(table));

  for (std::size_t section_index = 0; section_index < 5U; ++section_index) {
    SubcodeSection expected;
    ASSERT_TRUE(generator.GenerateSection(section_index, &expected));

    const std::size_t base = section_index * kFramesPerSubcodeSection;
    // IEC 60908-1999, 17.3: the first two control bytes are replaced by S0/S1.
    EXPECT_EQ(frames[base].control_symbol, kSubcodeSyncS0)
        << "section " << section_index;
    EXPECT_EQ(frames[base + 1].control_symbol, kSubcodeSyncS1)
        << "section " << section_index;
    for (std::size_t frame = kSubcodeSyncFrames;
         frame < kFramesPerSubcodeSection; ++frame) {
      ASSERT_EQ(frames[base + frame].control_byte, expected.ControlByte(frame))
          << "section " << section_index << " frame " << frame;
    }
  }
}

TEST(EfmStreamEncoderTest, DecodedAudioIsSampleExactAfterTheWarmUp) {
  const TrackTable table = MakeTrackTable(75, 300, 75);
  constexpr std::size_t kSourceFrames = 400;
  std::vector<std::int16_t> left;
  std::vector<std::int16_t> right;
  MakeSamples(kStereoSamplesPerF1Frame * kSourceFrames, &left, &right);

  const std::vector<DecodedFrame> frames =
      TestChannelDecoder().Decode(EncodeStream(table, left, right));
  ASSERT_EQ(frames.size(), kSourceFrames + kCircDrainFrames);

  const std::vector<F1Frame> f1_frames = Deinterleave(frames);
  std::vector<std::int16_t> decoded_left;
  std::vector<std::int16_t> decoded_right;
  ExtractSamples(f1_frames, &decoded_left, &decoded_right);

  // Exactly kCircPipelineLatencyFrames frames of warm-up silence precede source
  // sample 0 (EFM implementation plan, Timing Alignment Contract).
  const std::size_t warm_up =
      kCircPipelineLatencyFrames * kStereoSamplesPerF1Frame;
  ASSERT_EQ(decoded_left.size(),
            (kSourceFrames + kCircDrainFrames) * kStereoSamplesPerF1Frame);
  for (std::size_t index = 0; index < warm_up; ++index) {
    ASSERT_EQ(decoded_left[index], 0) << "warm-up sample " << index;
    ASSERT_EQ(decoded_right[index], 0) << "warm-up sample " << index;
  }
  for (std::size_t index = 0; index < left.size(); ++index) {
    ASSERT_EQ(decoded_left[warm_up + index], left[index])
        << "source sample " << index;
    ASSERT_EQ(decoded_right[warm_up + index], right[index])
        << "source sample " << index;
  }
}

TEST(EfmStreamEncoderTest, PartialFramesArePaddedWithDigitalSilenceOnFlush) {
  const TrackTable table = MakeTrackTable(75, 300, 75);
  // Four stereo samples: two short of a whole F1 frame.
  const std::vector<std::int16_t> left = {1000, -1000, 2000, -2000};
  const std::vector<std::int16_t> right = {-1000, 1000, -2000, 2000};

  const std::vector<DecodedFrame> frames =
      TestChannelDecoder().Decode(EncodeStream(table, left, right));
  ASSERT_EQ(frames.size(), 1U + kCircDrainFrames);

  std::vector<std::int16_t> decoded_left;
  std::vector<std::int16_t> decoded_right;
  ExtractSamples(Deinterleave(frames), &decoded_left, &decoded_right);

  const std::size_t warm_up =
      kCircPipelineLatencyFrames * kStereoSamplesPerF1Frame;
  for (std::size_t index = 0; index < left.size(); ++index) {
    EXPECT_EQ(decoded_left[warm_up + index], left[index]) << "sample " << index;
    EXPECT_EQ(decoded_right[warm_up + index], right[index])
        << "sample " << index;
  }
  for (std::size_t index = left.size(); index < kStereoSamplesPerF1Frame;
       ++index) {
    EXPECT_EQ(decoded_left[warm_up + index], 0) << "padding " << index;
    EXPECT_EQ(decoded_right[warm_up + index], 0) << "padding " << index;
  }
}

TEST(EfmStreamEncoderTest, ResetRestoresTheConstructedState) {
  const TrackTable table = MakeTrackTable(75, 300, 75);
  std::vector<std::int16_t> left;
  std::vector<std::int16_t> right;
  MakeSamples(std::size_t{6} * 120U, &left, &right);

  EfmStreamEncoder encoder;
  ASSERT_TRUE(encoder.Begin(table));
  std::vector<std::uint8_t> first;
  ASSERT_TRUE(encoder.PushSamples(left, right, &first));

  encoder.Reset();
  EXPECT_EQ(encoder.ChannelFrameCount(), 0U);
  std::vector<std::uint8_t> rejected;
  EXPECT_FALSE(encoder.PushSamples(left, right, &rejected));

  ASSERT_TRUE(encoder.Begin(table));
  std::vector<std::uint8_t> second;
  ASSERT_TRUE(encoder.PushSamples(left, right, &second));
  EXPECT_EQ(second, first);
}

TEST(EfmStreamEncoderTest, F1FrameInputMatchesSampleInput) {
  const TrackTable table = MakeTrackTable(75, 300, 75);
  std::vector<std::int16_t> left;
  std::vector<std::int16_t> right;
  MakeSamples(std::size_t{6} * 250U, &left, &right);

  std::vector<F1Frame> frames;
  AudioFrameAssembler assembler;
  ASSERT_TRUE(assembler.PushSamples(left, right, &frames));

  EfmStreamEncoder encoder;
  ASSERT_TRUE(encoder.Begin(table));
  std::vector<std::uint8_t> t_values;
  ASSERT_TRUE(encoder.PushF1Frames(frames.data(), frames.size(), &t_values));
  ASSERT_TRUE(encoder.Flush(&t_values));

  EXPECT_EQ(t_values, EncodeStream(table, left, right));
}

TEST(EfmStreamEncoderTest, F1FrameInputIsRejectedWithSamplesPending) {
  EfmStreamEncoder encoder;
  ASSERT_TRUE(encoder.Begin(MakeTrackTable(75, 300, 75)));
  std::vector<std::uint8_t> t_values;
  ASSERT_TRUE(encoder.PushSamples({1, 2}, {3, 4}, &t_values));
  const F1Frame frame{};
  EXPECT_FALSE(encoder.PushF1Frames(&frame, 1, &t_values));
  EXPECT_FALSE(encoder.SkipFrames(1, &t_values));
}

TEST(EfmStreamEncoderTest, SkippedFramesKeepTheSubcodeTimelineAndRestart) {
  const TrackTable table = MakeTrackTable(75, 300, 75);
  constexpr std::size_t kRunFrames = 3 * kFramesPerSubcodeSection;
  constexpr std::size_t kGapFrames = 2 * kFramesPerSubcodeSection;
  std::vector<F1Frame> frames(kRunFrames);
  for (std::size_t index = 0; index < frames.size(); ++index) {
    frames[index].fill(static_cast<std::uint8_t>(index * 7U));
  }

  EfmStreamEncoder encoder;
  ASSERT_TRUE(encoder.Begin(table));
  std::vector<std::uint8_t> first_run;
  ASSERT_TRUE(encoder.PushF1Frames(frames.data(), frames.size(), &first_run));
  ASSERT_TRUE(encoder.SkipFrames(kGapFrames, &first_run));
  EXPECT_EQ(encoder.ChannelFrameCount(), kRunFrames + kGapFrames);
  std::vector<std::uint8_t> second_run;
  ASSERT_TRUE(encoder.PushF1Frames(frames.data(), frames.size(), &second_run));
  // End the second run too, emitting its final T value.
  ASSERT_TRUE(encoder.SkipFrames(1, &second_run));

  // Each run decodes on its own, frame-synchronous from its first bit.
  const std::vector<DecodedFrame> first =
      TestChannelDecoder().Decode(first_run);
  const std::vector<DecodedFrame> second =
      TestChannelDecoder().Decode(second_run);
  ASSERT_EQ(first.size(), kRunFrames);
  ASSERT_EQ(second.size(), kRunFrames);

  // The second run's subcode continues at the section after the gap.
  SubcodeGenerator generator;
  ASSERT_TRUE(generator.Begin(table));
  const std::size_t resume_section =
      (kRunFrames + kGapFrames) / kFramesPerSubcodeSection;
  SubcodeSection expected;
  ASSERT_TRUE(generator.GenerateSection(resume_section, &expected));
  EXPECT_EQ(second[0].control_symbol, kSubcodeSyncS0);
  for (std::size_t frame = kSubcodeSyncFrames; frame < kFramesPerSubcodeSection;
       ++frame) {
    ASSERT_EQ(second[frame].control_byte, expected.ControlByte(frame))
        << "frame " << frame;
  }

  // A restarted CIRC encoder is a fresh one, so the restarted run's data
  // symbols equal the first run's.
  for (std::size_t index = 0; index < kRunFrames; ++index) {
    ASSERT_EQ(second[index].f2_frame, first[index].f2_frame)
        << "frame " << index;
  }
}

}  // namespace
}  // namespace videosynth::efm
