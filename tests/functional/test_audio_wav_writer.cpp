/*
 * File:        test_audio_wav_writer.cpp
 * Module:      audio_wav_writer_tests
 * Purpose:     Validates the RIFF/WAVE stereo 24-bit PCM output AudioWavWriter
 *              produces. Path derivation is covered by the unit suite in
 *              tests/unit/test_audio_wav_writer_paths.cpp.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "videosynth/audio_wav_writer.h"
#include "videosynth/model.h"

namespace videosynth {
namespace {

std::vector<char> ReadFileBytes(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  return std::vector<char>((std::istreambuf_iterator<char>(stream)),
                           std::istreambuf_iterator<char>());
}

std::uint32_t ReadLe32(const std::vector<char>& bytes, std::size_t offset) {
  return static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset])) |
         (static_cast<std::uint32_t>(
              static_cast<unsigned char>(bytes[offset + 1]))
          << 8) |
         (static_cast<std::uint32_t>(
              static_cast<unsigned char>(bytes[offset + 2]))
          << 16) |
         (static_cast<std::uint32_t>(
              static_cast<unsigned char>(bytes[offset + 3]))
          << 24);
}

std::uint16_t ReadLe16(const std::vector<char>& bytes, std::size_t offset) {
  return static_cast<std::uint16_t>(
      static_cast<unsigned char>(bytes[offset]) |
      (static_cast<unsigned char>(bytes[offset + 1]) << 8));
}

// Reads a 24-bit signed little-endian sample and sign-extends it to int32.
std::int32_t ReadLe24Signed(const std::vector<char>& bytes,
                            std::size_t offset) {
  std::uint32_t raw =
      static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset])) |
      (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + 1]))
       << 8) |
      (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + 2]))
       << 16);
  if ((raw & 0x800000U) != 0U) {
    raw |= 0xFF000000U;  // Sign-extend the top byte.
  }
  return static_cast<std::int32_t>(raw);
}

std::string FourCc(const std::vector<char>& bytes, std::size_t offset) {
  return std::string(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                     bytes.begin() + static_cast<std::ptrdiff_t>(offset + 4));
}

Project MakeAudioProject(Standard standard, const std::string& video_path) {
  Project project;
  project.cvbs_presets.video_standard_preset = standard;
  project.output.video_path = video_path;
  return project;
}

TEST(AudioWavWriterTest, WritesValidPalStereoPcm) {
  const std::filesystem::path video_path =
      std::filesystem::temp_directory_path() / "videosynth_audio_wav_pal.cvbs";
  const std::filesystem::path expected_audio_path =
      std::filesystem::temp_directory_path() /
      "videosynth_audio_wav_pal_audio_2.wav";
  std::filesystem::remove(expected_audio_path);

  const Project project = MakeAudioProject(Standard::kPal, video_path.string());

  AudioWavWriter writer;
  std::vector<std::string> errors;
  ASSERT_TRUE(writer.BeginWrite(project, 2, 8, &errors))
      << (errors.empty() ? "" : errors.front());

  // Two frames of 4 sample positions each, with distinct L/R content.
  const std::vector<std::int32_t> left0 = {0, 100, -100, 8388607};
  const std::vector<std::int32_t> right0 = {1, -100, 100, -8388608};
  const std::vector<std::int32_t> left1 = {-8388608, 5, -5, 0};
  const std::vector<std::int32_t> right1 = {8388607, -5, 5, 0};
  ASSERT_TRUE(writer.AppendFrameAudio(left0, right0, &errors));
  ASSERT_TRUE(writer.AppendFrameAudio(left1, right1, &errors));
  ASSERT_TRUE(writer.FinalizeWrite(&errors));

  ASSERT_TRUE(std::filesystem::exists(expected_audio_path));
  const std::vector<char> bytes = ReadFileBytes(expected_audio_path);
  ASSERT_GE(bytes.size(), 44U);

  const std::size_t frame_positions = left0.size() + left1.size();
  const std::uint32_t data_bytes =
      static_cast<std::uint32_t>(frame_positions * 2U * 3U);  // stereo, 24-bit.

  EXPECT_EQ(FourCc(bytes, 0), "RIFF");
  EXPECT_EQ(ReadLe32(bytes, 4), 36U + data_bytes);
  EXPECT_EQ(FourCc(bytes, 8), "WAVE");
  EXPECT_EQ(FourCc(bytes, 12), "fmt ");
  EXPECT_EQ(ReadLe32(bytes, 16), 16U);
  EXPECT_EQ(ReadLe16(bytes, 20), 1U);  // PCM.
  EXPECT_EQ(ReadLe16(bytes, 22), 2U);  // Channels.
  EXPECT_EQ(ReadLe32(bytes, 24), 48000U);
  EXPECT_EQ(ReadLe32(bytes, 28), 48000U * 2U * 3U);  // Byte rate.
  EXPECT_EQ(ReadLe16(bytes, 32), 6U);                // Block align.
  EXPECT_EQ(ReadLe16(bytes, 34), 24U);               // Bits per sample.
  EXPECT_EQ(FourCc(bytes, 36), "data");
  EXPECT_EQ(ReadLe32(bytes, 40), data_bytes);
  EXPECT_EQ(bytes.size(), 44U + data_bytes);

  // First interleaved L/R pair, then the second position's L/R pair.
  EXPECT_EQ(ReadLe24Signed(bytes, 44), left0[0]);
  EXPECT_EQ(ReadLe24Signed(bytes, 47), right0[0]);
  EXPECT_EQ(ReadLe24Signed(bytes, 50), left0[1]);
  EXPECT_EQ(ReadLe24Signed(bytes, 53), right0[1]);
  // Full-scale extremes round-trip exactly (position 3: 6 bytes/position).
  EXPECT_EQ(ReadLe24Signed(bytes, 44 + 3 * 6), left0[3]);       // 8388607.
  EXPECT_EQ(ReadLe24Signed(bytes, 44 + 3 * 6 + 3), right0[3]);  // -8388608.

  std::filesystem::remove(expected_audio_path);
}

TEST(AudioWavWriterTest, UsesFixedHeaderRateForNtsc) {
  const std::filesystem::path video_path =
      std::filesystem::temp_directory_path() /
      "videosynth_audio_wav_ntsc.cvbsy";
  const std::filesystem::path expected_audio_path =
      std::filesystem::temp_directory_path() /
      "videosynth_audio_wav_ntsc_audio_0.wav";
  std::filesystem::remove(expected_audio_path);

  const Project project =
      MakeAudioProject(Standard::kNtsc, video_path.string());

  AudioWavWriter writer;
  std::vector<std::string> errors;
  ASSERT_TRUE(writer.BeginWrite(project, 0, 3, &errors));
  ASSERT_TRUE(writer.AppendFrameAudio({1, 2, 3}, {4, 5, 6}, &errors));
  ASSERT_TRUE(writer.FinalizeWrite(&errors));

  ASSERT_TRUE(std::filesystem::exists(expected_audio_path));
  const std::vector<char> bytes = ReadFileBytes(expected_audio_path);
  ASSERT_GE(bytes.size(), 44U);
  EXPECT_EQ(ReadLe32(bytes, 24), 48000U);
  EXPECT_EQ(ReadLe32(bytes, 28), 48000U * 2U * 3U);

  std::filesystem::remove(expected_audio_path);
}

TEST(AudioWavWriterTest, RejectsMismatchedChannelLengths) {
  const std::filesystem::path video_path =
      std::filesystem::temp_directory_path() /
      "videosynth_audio_wav_mismatch.cvbs";
  const Project project = MakeAudioProject(Standard::kPal, video_path.string());

  AudioWavWriter writer;
  std::vector<std::string> errors;
  ASSERT_TRUE(writer.BeginWrite(project, 0, 3, &errors));
  EXPECT_FALSE(writer.AppendFrameAudio({1, 2, 3}, {4, 5}, &errors));
  EXPECT_FALSE(errors.empty());
  writer.AbortWrite();
}

TEST(AudioWavWriterTest, RejectsFinalizeWhenFewerSamplesThanDeclared) {
  const std::filesystem::path video_path =
      std::filesystem::temp_directory_path() /
      "videosynth_audio_wav_short.cvbs";
  const Project project = MakeAudioProject(Standard::kPal, video_path.string());

  AudioWavWriter writer;
  std::vector<std::string> errors;
  ASSERT_TRUE(writer.BeginWrite(project, 0, 4, &errors));
  ASSERT_TRUE(writer.AppendFrameAudio({1, 2, 3}, {4, 5, 6}, &errors));
  EXPECT_FALSE(writer.FinalizeWrite(&errors));
  EXPECT_FALSE(errors.empty());
  std::filesystem::remove(
      AudioWavWriter::DeriveAudioPath(video_path.string(), 0));
}

TEST(AudioWavWriterTest, RejectsAppendBeyondDeclaredSamples) {
  const std::filesystem::path video_path =
      std::filesystem::temp_directory_path() / "videosynth_audio_wav_long.cvbs";
  const Project project = MakeAudioProject(Standard::kPal, video_path.string());

  AudioWavWriter writer;
  std::vector<std::string> errors;
  ASSERT_TRUE(writer.BeginWrite(project, 0, 2, &errors));
  EXPECT_FALSE(writer.AppendFrameAudio({1, 2, 3}, {4, 5, 6}, &errors));
  EXPECT_FALSE(errors.empty());
  writer.AbortWrite();
}

}  // namespace
}  // namespace videosynth
