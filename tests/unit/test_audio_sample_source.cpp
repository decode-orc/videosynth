/*
 * File:        test_audio_sample_source.cpp
 * Module:      audio_sample_source_tests
 * Purpose:     Unit tests for the ffmpeg audio decode command line.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <string>

#include "videosynth/audio_sample_source.h"

namespace videosynth {
namespace {

TEST(FfmpegAudioSampleSourceTest, CommandDecodesStereo32BitAtTheRequestedRate) {
  const std::string command =
      FfmpegAudioSampleSource::BuildCommand("/media/a.flac", 0, 0.0, 48000);
  EXPECT_EQ(command.find("-ss"), std::string::npos) << command;
  EXPECT_NE(command.find("-i '/media/a.flac'"), std::string::npos) << command;
  EXPECT_NE(command.find("-map 0:a:0"), std::string::npos) << command;
  EXPECT_NE(command.find("-ac 2 -ar 48000 -f s32le -"), std::string::npos)
      << command;
}

TEST(FfmpegAudioSampleSourceTest, CommandSeeksOnTheInputAndSelectsTheStream) {
  const std::string command =
      FfmpegAudioSampleSource::BuildCommand("clip.mkv", 2, 1.25, 44100);
  const std::size_t seek = command.find("-ss 1.250000");
  const std::size_t input = command.find("-i 'clip.mkv'");
  ASSERT_NE(seek, std::string::npos) << command;
  ASSERT_NE(input, std::string::npos) << command;
  EXPECT_LT(seek, input);
  EXPECT_NE(command.find("-map 0:a:2"), std::string::npos) << command;
  EXPECT_NE(command.find("-ar 44100"), std::string::npos) << command;
}

TEST(FfmpegAudioSampleSourceTest, CommandQuotesPathsForTheShell) {
  const std::string command =
      FfmpegAudioSampleSource::BuildCommand("it's.wav", 0, 0.0, 48000);
  EXPECT_NE(command.find("-i 'it'\\''s.wav'"), std::string::npos) << command;
}

}  // namespace
}  // namespace videosynth
