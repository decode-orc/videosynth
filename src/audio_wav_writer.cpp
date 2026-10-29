/*
 * File:        audio_wav_writer.cpp
 * Module:      audio_wav_writer
 * Purpose:     Streams one frame-locked stereo 24-bit PCM channel pair to a
 *              RIFF/WAVE file alongside the generated CVBS output.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "videosynth/audio_wav_writer.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>

#include "videosynth/audio_output_paths.h"
#include "videosynth/path_resolution.h"
#include "videosynth/timing_constants.h"

namespace videosynth {

namespace {

// RIFF/WAVE constants for stereo 24-bit signed PCM. WAVEFORMAT PCM tag 0x0001.
constexpr std::uint16_t kPcmFormatTag = 0x0001;
constexpr std::uint16_t kChannelCount = 2;
constexpr std::uint16_t kBitsPerSample = 24;
constexpr std::uint16_t kBytesPerSample = kBitsPerSample / 8;
// 44-byte canonical header: RIFF (12) + fmt (24) + data chunk header (8).
constexpr std::size_t kHeaderBytes = 44;
constexpr std::size_t kRiffSizeOffset = 4;
constexpr std::size_t kDataSizeOffset = 40;

void AppendLittleEndian16(std::vector<char>* out, std::uint16_t value) {
  out->push_back(static_cast<char>(value & 0xFF));
  out->push_back(static_cast<char>((value >> 8) & 0xFF));
}

void AppendLittleEndian24(std::vector<char>* out, std::int32_t value) {
  const std::uint32_t bits = static_cast<std::uint32_t>(value);
  out->push_back(static_cast<char>(bits & 0xFF));
  out->push_back(static_cast<char>((bits >> 8) & 0xFF));
  out->push_back(static_cast<char>((bits >> 16) & 0xFF));
}

void AppendLittleEndian32(std::vector<char>* out, std::uint32_t value) {
  out->push_back(static_cast<char>(value & 0xFF));
  out->push_back(static_cast<char>((value >> 8) & 0xFF));
  out->push_back(static_cast<char>((value >> 16) & 0xFF));
  out->push_back(static_cast<char>((value >> 24) & 0xFF));
}

void AppendFourCc(std::vector<char>* out, std::string_view tag) {
  out->insert(out->end(), tag.begin(), tag.end());
}

// Serialises a canonical 44-byte RIFF/WAVE header. riff_size is (36 + data
// bytes) and data_size is the PCM payload length; both are placeholders at
// BeginWrite and back-patched at FinalizeWrite.
std::vector<char> BuildHeader(int sample_rate_hz, std::uint32_t riff_size,
                              std::uint32_t data_size) {
  const std::uint32_t byte_rate = static_cast<std::uint32_t>(sample_rate_hz) *
                                  kChannelCount * kBytesPerSample;
  const std::uint16_t block_align = kChannelCount * kBytesPerSample;

  std::vector<char> header;
  header.reserve(kHeaderBytes);
  AppendFourCc(&header, "RIFF");
  AppendLittleEndian32(&header, riff_size);
  AppendFourCc(&header, "WAVE");
  AppendFourCc(&header, "fmt ");
  AppendLittleEndian32(&header, 16);  // PCM fmt chunk body size.
  AppendLittleEndian16(&header, kPcmFormatTag);
  AppendLittleEndian16(&header, kChannelCount);
  AppendLittleEndian32(&header, static_cast<std::uint32_t>(sample_rate_hz));
  AppendLittleEndian32(&header, byte_rate);
  AppendLittleEndian16(&header, block_align);
  AppendLittleEndian16(&header, kBitsPerSample);
  AppendFourCc(&header, "data");
  AppendLittleEndian32(&header, data_size);
  return header;
}

}  // namespace

AudioWavWriter::AudioWavWriter(ILogger* logger) : logger_(logger) {}

std::string AudioWavWriter::DeriveAudioPath(const std::string& video_path,
                                            int channel_pair) {
  return DeriveAudioTrackPath(video_path, channel_pair, ".wav");
}

bool AudioWavWriter::BeginWrite(const Project& project, int channel_pair,
                                std::vector<std::string>* errors) {
  if (errors == nullptr) {
    return false;
  }
  if (session_open_) {
    errors->push_back("Audio write session already open.");
    return false;
  }

  int header_sample_rate_hz = 0;
  try {
    header_sample_rate_hz =
        AudioHeaderSampleRateHz(project.cvbs_presets.video_standard_preset);
  } catch (const std::exception& e) {
    errors->push_back(std::string("Audio writer requires a supported video "
                                  "standard: ") +
                      e.what());
    return false;
  }

  const std::string audio_path =
      DeriveAudioPath(project.output.video_path, channel_pair);
  if (audio_path.empty()) {
    errors->push_back(
        "Audio output path could not be derived from video_path.");
    return false;
  }

  std::error_code ec;
  if (!CreateParentDirectories(audio_path, &ec)) {
    errors->push_back("Failed to create audio output directory for: " +
                      audio_path + " (" + ec.message() + ")");
    return false;
  }

  stream_.open(audio_path, std::ios::binary | std::ios::trunc);
  if (!stream_) {
    errors->push_back("Failed to open audio output file: " + audio_path);
    return false;
  }

  // Reserve the header with placeholder sizes; sizes are patched on finalize.
  const std::vector<char> header = BuildHeader(header_sample_rate_hz, 0, 0);
  stream_.write(header.data(), static_cast<std::streamsize>(header.size()));
  if (!stream_) {
    errors->push_back("Failed to write audio header to: " + audio_path);
    stream_.close();
    return false;
  }

  audio_path_ = audio_path;
  header_sample_rate_hz_ = header_sample_rate_hz;
  data_bytes_ = 0;
  session_open_ = true;

  if (logger_ != nullptr) {
    logger_->Trace("Opened audio output file for writing: " + audio_path);
  }
  return true;
}

bool AudioWavWriter::AppendFrameAudio(const std::vector<std::int32_t>& left,
                                      const std::vector<std::int32_t>& right,
                                      std::vector<std::string>* errors) {
  if (errors == nullptr) {
    return false;
  }
  if (!session_open_) {
    errors->push_back("Audio write session is not open.");
    return false;
  }
  if (left.size() != right.size()) {
    errors->push_back(
        "Audio left/right channel buffers differ in length for: " +
        audio_path_);
    return false;
  }

  // Each sample position becomes an interleaved L+R pair of 24-bit samples.
  const std::uint64_t frame_bytes =
      static_cast<std::uint64_t>(left.size()) * kChannelCount * kBytesPerSample;
  if (data_bytes_ + frame_bytes + (kHeaderBytes - 8) >
      std::numeric_limits<std::uint32_t>::max()) {
    errors->push_back("Audio track exceeds the 4 GB RIFF/WAVE size limit.");
    return false;
  }

  std::vector<char> interleaved;
  interleaved.reserve(left.size() * kChannelCount * kBytesPerSample);
  for (std::size_t i = 0; i < left.size(); ++i) {
    AppendLittleEndian24(&interleaved, left[i]);   // Left  (odd channel).
    AppendLittleEndian24(&interleaved, right[i]);  // Right (even channel).
  }

  stream_.write(interleaved.data(),
                static_cast<std::streamsize>(interleaved.size()));
  if (!stream_) {
    errors->push_back("Failed while writing audio samples to: " + audio_path_);
    return false;
  }

  data_bytes_ += frame_bytes;
  return true;
}

bool AudioWavWriter::FinalizeWrite(std::vector<std::string>* errors) {
  if (errors == nullptr) {
    return false;
  }
  if (!session_open_) {
    errors->push_back("Audio write session is not open.");
    return false;
  }

  const std::uint32_t data_size = static_cast<std::uint32_t>(data_bytes_);
  const std::uint32_t riff_size =
      static_cast<std::uint32_t>((kHeaderBytes - 8) + data_bytes_);

  // Back-patch the RIFF chunk size and the data chunk size in place.
  std::array<char, 4> le_riff{};
  std::array<char, 4> le_data{};
  for (int i = 0; i < 4; ++i) {
    le_riff[static_cast<std::size_t>(i)] =
        static_cast<char>((riff_size >> (8 * i)) & 0xFF);
    le_data[static_cast<std::size_t>(i)] =
        static_cast<char>((data_size >> (8 * i)) & 0xFF);
  }

  stream_.seekp(static_cast<std::streamoff>(kRiffSizeOffset), std::ios::beg);
  stream_.write(le_riff.data(), le_riff.size());
  stream_.seekp(static_cast<std::streamoff>(kDataSizeOffset), std::ios::beg);
  stream_.write(le_data.data(), le_data.size());
  if (!stream_) {
    errors->push_back("Failed to finalize audio header for: " + audio_path_);
    stream_.close();
    session_open_ = false;
    return false;
  }

  stream_.close();
  session_open_ = false;

  if (logger_ != nullptr) {
    logger_->Info("Wrote audio track: " + audio_path_);
  }
  return true;
}

void AudioWavWriter::AbortWrite() {
  if (!session_open_) {
    return;
  }

  stream_.close();
  // Removal failures are ignored: cleanup is best-effort on abort.
  std::error_code ec;
  std::filesystem::remove(audio_path_, ec);

  if (logger_ != nullptr) {
    logger_->Info("Audio write session aborted; removed " + audio_path_);
  }

  session_open_ = false;
  data_bytes_ = 0;
}

}  // namespace videosynth
