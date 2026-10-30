/*
 * File:        test_audio_media_reader.cpp
 * Module:      audio_media_reader_tests
 * Purpose:     Unit tests for file and video-soundtrack audio reading: end of
 *              file, repeat, following source frames and film time, loop
 *              reopen and stream offsets, against a fake sample source.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "fake_audio_sample_source.h"
#include "videosynth/audio_media_reader.h"

namespace videosynth {
namespace {

using testing::FakeAudioLibrary;
using testing::FakeSampleAt;
using testing::MakeFakeAudioFactory;

constexpr int kRate = 48000;
constexpr int kPalFrameSamples = 1920;

struct Frame {
  std::vector<std::int32_t> left;
  std::vector<std::int32_t> right;
};

Frame Read(AudioMediaReader* reader, int source_frame_index,
           int samples = kPalFrameSamples, std::int64_t fields_after = 0) {
  Frame frame;
  std::string error;
  EXPECT_TRUE(reader->ReadFrame(samples, source_frame_index, fields_after,
                                &frame.left, &frame.right, &error))
      << error;
  EXPECT_EQ(frame.left.size(), static_cast<std::size_t>(samples));
  EXPECT_EQ(frame.right.size(), static_cast<std::size_t>(samples));
  return frame;
}

bool AllZero(const std::vector<std::int32_t>& samples, std::size_t from = 0) {
  for (std::size_t i = from; i < samples.size(); ++i) {
    if (samples[i] != 0) {
      return false;
    }
  }
  return true;
}

VideoAudioTiming PalTiming() {
  VideoAudioTiming timing;
  timing.frame_rate_hz = 25.0;
  timing.output_frame_rate_hz = 25.0;
  return timing;
}

// 24000/1001 film shown with 2:3 pulldown at 30000/1001.
VideoAudioTiming FilmTiming() {
  VideoAudioTiming timing;
  timing.frame_rate_hz = 24000.0 / 1001.0;
  timing.output_frame_rate_hz = 30000.0 / 1001.0;
  return timing;
}

// SMPTE 272M-1994 §14.3: the five-frame NTSC 48 kHz sample sequence.
constexpr int kNtscFrameSamples[] = {1602, 1601, 1602, 1601, 1602};

TEST(AudioMediaReaderTest, FilePlaysFromItsStartThenFallsSilent) {
  FakeAudioLibrary library;
  library.media["a.wav"] = {0.0, 2500.0 / kRate};
  AudioMediaReader reader(MakeFakeAudioFactory(&library), kRate);
  reader.BeginFileRun({"a.wav", false});

  const Frame first = Read(&reader, 0);
  EXPECT_EQ(first.left.front(), 1);
  EXPECT_EQ(first.left.back(), 1920);
  EXPECT_EQ(first.right.back(), -1920);

  const Frame second = Read(&reader, 1);
  EXPECT_EQ(second.left[579], 2500);
  EXPECT_TRUE(AllZero(second.left, 580));
  EXPECT_TRUE(AllZero(second.right, 580));

  EXPECT_TRUE(AllZero(Read(&reader, 2).left));
  ASSERT_EQ(library.opens.size(), 1U);
  EXPECT_EQ(library.opens[0].start_seconds, 0.0);
  EXPECT_EQ(library.opens[0].sample_rate_hz, kRate);
}

TEST(AudioMediaReaderTest, RepeatingFileRestartsAtItsEnd) {
  FakeAudioLibrary library;
  library.media["a.wav"] = {0.0, 2500.0 / kRate};
  AudioMediaReader reader(MakeFakeAudioFactory(&library), kRate);
  reader.BeginFileRun({"a.wav", true});

  Read(&reader, 0);
  const Frame second = Read(&reader, 1);
  EXPECT_EQ(second.left[579], 2500);
  EXPECT_EQ(second.left[580], 1);
  EXPECT_EQ(second.left[1919], 1340);
  EXPECT_EQ(library.opens.size(), 2U);
}

TEST(AudioMediaReaderTest, EachRunRestartsTheFile) {
  FakeAudioLibrary library;
  library.media["a.wav"] = {0.0, 10.0};
  AudioMediaReader reader(MakeFakeAudioFactory(&library), kRate);
  reader.BeginFileRun({"a.wav", false});
  Read(&reader, 0);
  Read(&reader, 1);
  reader.BeginFileRun({"a.wav", false});
  EXPECT_EQ(Read(&reader, 0).left.front(), 1);
}

TEST(AudioMediaReaderTest,
     EmptyRepeatingFileIsSilentRatherThanReopenedForever) {
  FakeAudioLibrary library;
  library.media["empty.wav"] = {0.0, 0.0};
  AudioMediaReader reader(MakeFakeAudioFactory(&library), kRate);
  reader.BeginFileRun({"empty.wav", true});
  EXPECT_TRUE(AllZero(Read(&reader, 0).left));
  EXPECT_EQ(library.opens.size(), 1U);
}

TEST(AudioMediaReaderTest, VideoStreamsContinuouslyWhileFramesAdvance) {
  FakeAudioLibrary library;
  library.media["clip.mp4"] = {0.0, 60.0};
  AudioMediaReader reader(MakeFakeAudioFactory(&library), kRate);
  reader.BeginVideoRun("clip.mp4", 1, PalTiming());

  // A range starting at source frame 10 opens 0.4 s into the soundtrack.
  for (int index = 10; index < 14; ++index) {
    const Frame frame = Read(&reader, index);
    EXPECT_EQ(frame.left.front(), FakeSampleAt(index / 25.0, kRate));
  }
  ASSERT_EQ(library.opens.size(), 1U);
  EXPECT_DOUBLE_EQ(library.opens[0].start_seconds, 0.4);
  EXPECT_EQ(library.opens[0].stream, 1);
}

TEST(AudioMediaReaderTest, VideoLoopReopensAtTheLoopedFrame) {
  FakeAudioLibrary library;
  library.media["clip.mp4"] = {0.0, 60.0};
  AudioMediaReader reader(MakeFakeAudioFactory(&library), kRate);
  reader.BeginVideoRun("clip.mp4", 0, PalTiming());

  for (const int index : {5, 6, 7, 5, 6}) {
    EXPECT_EQ(Read(&reader, index).left.front(),
              FakeSampleAt(index / 25.0, kRate))
        << "source frame " << index;
  }
  ASSERT_EQ(library.opens.size(), 2U);
  EXPECT_DOUBLE_EQ(library.opens[1].start_seconds, 0.2);
}

TEST(AudioMediaReaderTest, FilmPlaysInRealTimeAcrossTheCadence) {
  FakeAudioLibrary library;
  library.media["film.mkv"] = {0.0, 60.0};
  AudioMediaReader reader(MakeFakeAudioFactory(&library), kRate);
  reader.BeginVideoRun("film.mkv", 0, FilmTiming());

  // Five output frames from film frame 48 (2.002 s) cover four film frames
  // and read one unbroken stretch of sound.
  const double start = 48.0 * 1001.0 / 24000.0;
  std::int64_t played = 0;
  for (int k = 0; k < 5; ++k) {
    const Frame frame = Read(&reader, 48, kNtscFrameSamples[k],
                             static_cast<std::int64_t>(k) * 2);
    EXPECT_EQ(frame.left.front(), FakeSampleAt(start, kRate, played))
        << "output frame " << k;
    played += kNtscFrameSamples[k];
  }
  ASSERT_EQ(library.opens.size(), 1U);
  EXPECT_DOUBLE_EQ(library.opens[0].start_seconds, start);
}

TEST(AudioMediaReaderTest, FilmPositionPastAFrameStartsMidFrame) {
  FakeAudioLibrary library;
  library.media["film.mkv"] = {0.0, 60.0};
  AudioMediaReader reader(MakeFakeAudioFactory(&library), kRate);
  reader.BeginVideoRun("film.mkv", 0, FilmTiming());

  // Three fields (1.5 output frames) after film frame 24.
  const double start = (24.0 * 1001.0 / 24000.0) + (1.5 * 1001.0 / 30000.0);
  EXPECT_EQ(Read(&reader, 24, kNtscFrameSamples[0], 3).left.front(),
            FakeSampleAt(start, kRate));
  EXPECT_DOUBLE_EQ(library.opens[0].start_seconds, start);
}

TEST(AudioMediaReaderTest, FilmLoopReopensAtThePassStart) {
  FakeAudioLibrary library;
  library.media["film.mkv"] = {0.0, 60.0};
  AudioMediaReader reader(MakeFakeAudioFactory(&library), kRate);
  reader.BeginVideoRun("film.mkv", 0, FilmTiming());

  Read(&reader, 10, kNtscFrameSamples[0], 0);
  Read(&reader, 10, kNtscFrameSamples[1], 2);
  // The range loops: the next pass starts again at film frame 10.
  EXPECT_EQ(Read(&reader, 10, kNtscFrameSamples[2], 0).left.front(),
            FakeSampleAt(10.0 * 1001.0 / 24000.0, kRate));
  ASSERT_EQ(library.opens.size(), 2U);
  EXPECT_DOUBLE_EQ(library.opens[1].start_seconds, 10.0 * 1001.0 / 24000.0);
}

TEST(AudioMediaReaderTest, VideoStartOffsetMovesTheSeek) {
  FakeAudioLibrary library;
  library.media["clip.mkv"] = {0.0, 60.0};
  AudioMediaReader reader(MakeFakeAudioFactory(&library), kRate);
  VideoAudioTiming timing = PalTiming();
  timing.video_start_offset_seconds = 0.08;
  reader.BeginVideoRun("clip.mkv", 0, timing);

  EXPECT_EQ(Read(&reader, 2).left.front(), FakeSampleAt(0.16, kRate));
  EXPECT_DOUBLE_EQ(library.opens[0].start_seconds, 0.16);
}

TEST(AudioMediaReaderTest, PictureBeforeTheSoundtrackIsSilentUntilItStarts) {
  FakeAudioLibrary library;
  // The soundtrack starts 0.1 s after the first picture.
  library.media["clip.mkv"] = {0.1, 60.0};
  AudioMediaReader reader(MakeFakeAudioFactory(&library), kRate);
  VideoAudioTiming timing = PalTiming();
  timing.audio_start_offset_seconds = 0.1;
  reader.BeginVideoRun("clip.mkv", 0, timing);

  // 0.1 s = 4800 samples = two and a half PAL frames of silence.
  EXPECT_TRUE(AllZero(Read(&reader, 0).left));
  EXPECT_TRUE(AllZero(Read(&reader, 1).left));
  const Frame third = Read(&reader, 2);
  EXPECT_TRUE(AllZero(
      std::vector<std::int32_t>(third.left.begin(), third.left.begin() + 960)));
  EXPECT_EQ(third.left[960], FakeSampleAt(0.1, kRate));
  EXPECT_DOUBLE_EQ(library.opens[0].start_seconds, 0.1);
}

TEST(AudioMediaReaderTest, ShortSoundtrackPadsWithSilence) {
  FakeAudioLibrary library;
  library.media["clip.mp4"] = {0.0, 0.05};  // 2400 samples.
  AudioMediaReader reader(MakeFakeAudioFactory(&library), kRate);
  reader.BeginVideoRun("clip.mp4", 0, PalTiming());
  Read(&reader, 0);
  const Frame second = Read(&reader, 1);
  EXPECT_EQ(second.left[479], 2400);
  EXPECT_TRUE(AllZero(second.left, 480));
  EXPECT_TRUE(AllZero(Read(&reader, 2).left));
}

TEST(AudioMediaReaderTest, EfmRateReaderSeeksOnItsOwnGrid) {
  FakeAudioLibrary library;
  library.media["clip.mp4"] = {0.0, 60.0};
  AudioMediaReader reader(MakeFakeAudioFactory(&library), 44100);
  reader.BeginVideoRun("clip.mp4", 0, PalTiming());
  EXPECT_EQ(Read(&reader, 3, 1764).left.front(), FakeSampleAt(0.12, 44100));
  EXPECT_EQ(library.opens[0].sample_rate_hz, 44100);
}

TEST(AudioMediaReaderTest, ReportsOpenAndReadFailures) {
  FakeAudioLibrary library;
  AudioMediaReader reader(MakeFakeAudioFactory(&library), kRate);
  reader.BeginFileRun({"missing.wav", false});
  std::vector<std::int32_t> left;
  std::vector<std::int32_t> right;
  std::string error;
  EXPECT_FALSE(reader.ReadFrame(kPalFrameSamples, 0, 0, &left, &right, &error));
  EXPECT_NE(error.find("missing.wav"), std::string::npos) << error;

  library.media["a.wav"] = {0.0, 1.0};
  library.read_error = "decode failed";
  reader.BeginFileRun({"a.wav", false});
  error.clear();
  EXPECT_FALSE(reader.ReadFrame(kPalFrameSamples, 0, 0, &left, &right, &error));
  EXPECT_EQ(error, "decode failed");
}

}  // namespace
}  // namespace videosynth
