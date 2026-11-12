/*
 * File:        audio_wav_writer.cpp
 * Module:      audio_wav_writer
 * Purpose:     Streams one frame-locked stereo 24-bit PCM channel pair to a
 *              RIFF/WAVE (or RF64/WAVE beyond 4 GiB) file alongside the
 *              generated CVBS output.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "videosynth/audio_wav_writer.h"

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
constexpr std::uint16_t kBlockAlign = kChannelCount * kBytesPerSample;
// Canonical RIFF header: RIFF (12) + fmt (24) + data chunk header (8).
constexpr std::uint64_t kRiffHeaderBytes = 44;
// EBU Tech 3306 RF64 header: RF64 (12) + ds64 (8 + 28) + fmt (24) + data (8).
constexpr std::uint64_t kRf64HeaderBytes = 80;
// ds64 body: RIFF size, data size, sample count (3 x 64-bit) + table length.
constexpr std::uint32_t kDs64BodyBytes = 28;
// CVBS File Format Specification v1.8.0, Audio Data / RF64 for Long
// Recordings: RF64 is required once the RIFF chunk size exceeds 0xFFFFFFFE.
constexpr std::uint64_t kMaxRiffChunkSize = 0xFFFFFFFEULL;
// EBU Tech 3306: 32-bit size fields of an RF64 file are set to 0xFFFFFFFF.
constexpr std::uint32_t kRf64SizePlaceholder = 0xFFFFFFFFU;

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
  for (int i = 0; i < 4; ++i) {
    out->push_back(static_cast<char>((value >> (8 * i)) & 0xFF));
  }
}

void AppendLittleEndian64(std::vector<char>* out, std::uint64_t value) {
  for (int i = 0; i < 8; ++i) {
    out->push_back(static_cast<char>((value >> (8 * i)) & 0xFF));
  }
}

void AppendFourCc(std::vector<char>* out, std::string_view tag) {
  out->insert(out->end(), tag.begin(), tag.end());
}

// Appends the 24-byte PCM fmt chunk shared by RIFF and RF64 files.
void AppendFmtChunk(std::vector<char>* out, int sample_rate_hz) {
  const std::uint32_t byte_rate =
      static_cast<std::uint32_t>(sample_rate_hz) * kBlockAlign;
  AppendFourCc(out, "fmt ");
  AppendLittleEndian32(out, 16);  // PCM fmt chunk body size.
  AppendLittleEndian16(out, kPcmFormatTag);
  AppendLittleEndian16(out, kChannelCount);
  AppendLittleEndian32(out, static_cast<std::uint32_t>(sample_rate_hz));
  AppendLittleEndian32(out, byte_rate);
  AppendLittleEndian16(out, kBlockAlign);
  AppendLittleEndian16(out, kBitsPerSample);
}

}  // namespace

AudioWavWriter::AudioWavWriter(ILogger* logger) : logger_(logger) {}

WavContainer AudioWavWriter::ContainerForSampleFrames(
    std::uint64_t total_sample_frames) {
  // A RIFF chunk size above the limit implies a data size far below 2^64, so
  // the multiplication cannot overflow for any realistic frame count; guard
  // anyway so absurd inputs select RF64 rather than wrapping.
  if (total_sample_frames >
      (std::numeric_limits<std::uint64_t>::max() - kRiffHeaderBytes) /
          kBlockAlign) {
    return WavContainer::kRf64;
  }
  const std::uint64_t riff_chunk_size =
      (kRiffHeaderBytes - 8) + total_sample_frames * kBlockAlign;
  return riff_chunk_size > kMaxRiffChunkSize ? WavContainer::kRf64
                                             : WavContainer::kRiff;
}

std::vector<char> AudioWavWriter::BuildHeader(
    WavContainer container, int sample_rate_hz,
    std::uint64_t total_sample_frames) {
  const std::uint64_t data_bytes = total_sample_frames * kBlockAlign;
  std::vector<char> header;

  if (container == WavContainer::kRiff) {
    header.reserve(kRiffHeaderBytes);
    AppendFourCc(&header, "RIFF");
    AppendLittleEndian32(&header, static_cast<std::uint32_t>(
                                      (kRiffHeaderBytes - 8) + data_bytes));
    AppendFourCc(&header, "WAVE");
    AppendFmtChunk(&header, sample_rate_hz);
    AppendFourCc(&header, "data");
    AppendLittleEndian32(&header, static_cast<std::uint32_t>(data_bytes));
    return header;
  }

  // EBU Tech 3306: RF64 replaces RIFF, a ds64 chunk immediately follows the
  // WAVE identifier, and the 32-bit RF64/data sizes are 0xFFFFFFFF.
  header.reserve(kRf64HeaderBytes);
  AppendFourCc(&header, "RF64");
  AppendLittleEndian32(&header, kRf64SizePlaceholder);
  AppendFourCc(&header, "WAVE");
  AppendFourCc(&header, "ds64");
  AppendLittleEndian32(&header, kDs64BodyBytes);
  AppendLittleEndian64(&header, (kRf64HeaderBytes - 8) + data_bytes);
  AppendLittleEndian64(&header, data_bytes);
  AppendLittleEndian64(&header, total_sample_frames);
  AppendLittleEndian32(&header, 0);  // ds64 table length.
  AppendFmtChunk(&header, sample_rate_hz);
  AppendFourCc(&header, "data");
  AppendLittleEndian32(&header, kRf64SizePlaceholder);
  return header;
}

std::string AudioWavWriter::DeriveAudioPath(const std::string& video_path,
                                            int channel_pair) {
  return DeriveAudioTrackPath(video_path, channel_pair, ".wav");
}

bool AudioWavWriter::BeginWrite(const Project& project, int channel_pair,
                                std::uint64_t total_sample_frames,
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

  // Reserve the header with zero sizes; it is rewritten on finalize. The
  // container fixes the header length, so it is chosen here.
  const WavContainer container = ContainerForSampleFrames(total_sample_frames);
  const std::vector<char> header =
      BuildHeader(container, header_sample_rate_hz, 0);
  stream_.write(header.data(), static_cast<std::streamsize>(header.size()));
  if (!stream_) {
    errors->push_back("Failed to write audio header to: " + audio_path);
    stream_.close();
    return false;
  }

  audio_path_ = audio_path;
  header_sample_rate_hz_ = header_sample_rate_hz;
  container_ = container;
  expected_sample_frames_ = total_sample_frames;
  data_bytes_ = 0;
  session_open_ = true;

  if (logger_ != nullptr) {
    logger_->Trace("Opened audio output file for writing: " + audio_path);
    if (container == WavContainer::kRf64) {
      logger_->Debug("Audio track exceeds the RIFF size limit; writing RF64: " +
                     audio_path);
    }
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
  if (data_bytes_ + frame_bytes > expected_sample_frames_ * kBlockAlign) {
    errors->push_back("Audio samples exceed the sample count declared for: " +
                      audio_path_);
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

  if (data_bytes_ != expected_sample_frames_ * kBlockAlign) {
    errors->push_back(
        "Audio sample count differs from the declared count for: " +
        audio_path_);
    stream_.close();
    session_open_ = false;
    return false;
  }

  // Rewrite the header in place with the final sizes; its length is fixed by
  // the container chosen in BeginWrite.
  const std::vector<char> header =
      BuildHeader(container_, header_sample_rate_hz_, expected_sample_frames_);
  stream_.seekp(0, std::ios::beg);
  stream_.write(header.data(), static_cast<std::streamsize>(header.size()));
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
