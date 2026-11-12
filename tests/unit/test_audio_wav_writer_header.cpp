/*
 * File:        test_audio_wav_writer_header.cpp
 * Module:      audio_wav_writer_tests
 * Purpose:     Validates AudioWavWriter's pure RIFF/RF64 container selection
 *              and header serialisation for long channel pair files
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "videosynth/audio_wav_writer.h"

namespace videosynth {
namespace {

// Stereo 24-bit: 6 bytes per sample position.
constexpr std::uint64_t kBytesPerSampleFrame = 6;
// Largest sample count whose RIFF chunk size (36 + data) stays within
// 0xFFFFFFFE: floor((0xFFFFFFFE - 36) / 6).
constexpr std::uint64_t kMaxRiffSampleFrames = 715827876;

std::uint64_t ReadLe(const std::vector<char>& bytes, std::size_t offset,
                     int width) {
  std::uint64_t value = 0;
  for (int i = 0; i < width; ++i) {
    value |= static_cast<std::uint64_t>(static_cast<unsigned char>(
                 bytes[offset + static_cast<std::size_t>(i)]))
             << (8 * i);
  }
  return value;
}

std::string FourCc(const std::vector<char>& bytes, std::size_t offset) {
  return std::string(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                     bytes.begin() + static_cast<std::ptrdiff_t>(offset + 4));
}

TEST(AudioWavWriterHeaderTest, ContainerIsRiffUpToTheRiffChunkLimit) {
  EXPECT_EQ(AudioWavWriter::ContainerForSampleFrames(0), WavContainer::kRiff);
  EXPECT_EQ(AudioWavWriter::ContainerForSampleFrames(kMaxRiffSampleFrames),
            WavContainer::kRiff);
}

TEST(AudioWavWriterHeaderTest, ContainerIsRf64BeyondTheRiffChunkLimit) {
  EXPECT_EQ(AudioWavWriter::ContainerForSampleFrames(kMaxRiffSampleFrames + 1),
            WavContainer::kRf64);
  EXPECT_EQ(AudioWavWriter::ContainerForSampleFrames(UINT64_MAX),
            WavContainer::kRf64);
}

TEST(AudioWavWriterHeaderTest, SixHourNtscTrackRequiresRf64) {
  // 650,000 NTSC frames (the specification's testing limit) at 8008/5
  // samples per frame.
  const std::uint64_t samples = 8008ULL * (650000ULL / 5ULL);
  EXPECT_EQ(AudioWavWriter::ContainerForSampleFrames(samples),
            WavContainer::kRf64);
}

TEST(AudioWavWriterHeaderTest, RiffHeaderIsCanonical44Bytes) {
  const std::vector<char> header =
      AudioWavWriter::BuildHeader(WavContainer::kRiff, 48000, 10);
  ASSERT_EQ(header.size(), 44U);
  EXPECT_EQ(FourCc(header, 0), "RIFF");
  EXPECT_EQ(ReadLe(header, 4, 4), 36U + 10U * kBytesPerSampleFrame);
  EXPECT_EQ(FourCc(header, 8), "WAVE");
  EXPECT_EQ(FourCc(header, 12), "fmt ");
  EXPECT_EQ(FourCc(header, 36), "data");
  EXPECT_EQ(ReadLe(header, 40, 4), 10U * kBytesPerSampleFrame);
}

TEST(AudioWavWriterHeaderTest, Rf64HeaderCarriesSizesInDs64) {
  const std::uint64_t samples = kMaxRiffSampleFrames + 1000;
  const std::uint64_t data_bytes = samples * kBytesPerSampleFrame;
  const std::vector<char> header =
      AudioWavWriter::BuildHeader(WavContainer::kRf64, 48000, samples);
  ASSERT_EQ(header.size(), 80U);

  // EBU Tech 3306: RF64 id, 32-bit size 0xFFFFFFFF, WAVE.
  EXPECT_EQ(FourCc(header, 0), "RF64");
  EXPECT_EQ(ReadLe(header, 4, 4), 0xFFFFFFFFU);
  EXPECT_EQ(FourCc(header, 8), "WAVE");

  // ds64 immediately follows WAVE: RIFF size, data size, sample count, table.
  EXPECT_EQ(FourCc(header, 12), "ds64");
  EXPECT_EQ(ReadLe(header, 16, 4), 28U);
  EXPECT_EQ(ReadLe(header, 20, 8), 72U + data_bytes);  // File size minus 8.
  EXPECT_EQ(ReadLe(header, 28, 8), data_bytes);
  EXPECT_EQ(ReadLe(header, 36, 8), samples);
  EXPECT_EQ(ReadLe(header, 44, 4), 0U);

  // fmt chunk identical to the RIFF layout.
  EXPECT_EQ(FourCc(header, 48), "fmt ");
  EXPECT_EQ(ReadLe(header, 52, 4), 16U);
  EXPECT_EQ(ReadLe(header, 56, 2), 1U);  // PCM.
  EXPECT_EQ(ReadLe(header, 58, 2), 2U);  // Channels.
  EXPECT_EQ(ReadLe(header, 60, 4), 48000U);
  EXPECT_EQ(ReadLe(header, 64, 4), 48000U * kBytesPerSampleFrame);
  EXPECT_EQ(ReadLe(header, 68, 2), 6U);   // Block align.
  EXPECT_EQ(ReadLe(header, 70, 2), 24U);  // Bits per sample.

  EXPECT_EQ(FourCc(header, 72), "data");
  EXPECT_EQ(ReadLe(header, 76, 4), 0xFFFFFFFFU);
}

TEST(AudioWavWriterHeaderTest, RiffAndRf64ShareTheFmtChunk) {
  const std::vector<char> riff =
      AudioWavWriter::BuildHeader(WavContainer::kRiff, 48000, 1);
  const std::vector<char> rf64 =
      AudioWavWriter::BuildHeader(WavContainer::kRf64, 48000, 1);
  EXPECT_EQ(std::vector<char>(riff.begin() + 12, riff.begin() + 36),
            std::vector<char>(rf64.begin() + 48, rf64.begin() + 72));
}

}  // namespace
}  // namespace videosynth
