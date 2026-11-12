/*
 * File:        audio_wav_writer.h
 * Module:      audio_wav_writer
 * Purpose:     Streams one frame-locked stereo 24-bit PCM channel pair to a
 *              RIFF/WAVE (or RF64/WAVE beyond 4 GiB) file alongside the
 *              generated CVBS output.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "videosynth/interfaces.h"
#include "videosynth/model.h"

namespace videosynth {

// Container used for one channel pair file. CVBS File Format Specification
// v1.8.0, Audio Data / RF64 for Long Recordings: a file whose RIFF chunk size
// would exceed 0xFFFFFFFE is written as RF64 (EBU Tech 3306); every other file
// is written as plain RIFF.
enum class WavContainer {
  kRiff,
  kRf64,
};

// Writes a single audio channel pair as a RIFF/WAVE file next to the CVBS
// output. Each channel pair is two independent channels (left = SMPTE 272M odd
// channel, right = even channel); the file is always 2-channel, 24-bit signed
// little-endian PCM (format tag 0x0001) at 48000 Hz per the CVBS File Format
// Specification (Audio Data / WAV File Format).
//
// The first appended sample is synchronous with the first stored video frame;
// callers must append exactly the per-frame sample count (AudioSamplesForFrame)
// per stored output frame so the emitted track stays sample-accurately
// frame-locked. Left and right buffers must be the same length.
//
// The container is chosen up front from the declared total sample count, so
// every channel pair of one output (all carry the same sample count) uses the
// same container, as the specification requires. Files within the RIFF limit
// keep the canonical 44-byte header; larger files get the 80-byte RF64 header
// (RF64 + ds64 + fmt + data).
//
// Streaming contract: BeginWrite (declares the total sample count and reserves
// the header) -> repeated AppendFrameAudio -> FinalizeWrite (verifies the
// declared count was written and rewrites the header with the final sizes). The
// output path is derived from output.video_path by stripping a trailing ".cvbs"
// or ".cvbsy" suffix and appending "_audio_<pair>.wav".
//
// Thread-safety: AudioWavWriter is NOT thread-safe. Its file stream and byte
// counters are mutated by AppendFrameAudio; it must not be called concurrently
// from multiple threads.
class AudioWavWriter {
 public:
  explicit AudioWavWriter(ILogger* logger = nullptr);

  // Opens the derived WAV path for channel pair `channel_pair` (0–7), selects
  // the container for `total_sample_frames` stereo sample positions (the sum
  // of AudioSamplesForFrame over every output frame), reserves the header, and
  // records the 48000 Hz rate for the fmt chunk. Creates parent directories as
  // needed. Returns false and appends a message to errors on any failure.
  bool BeginWrite(const Project& project, int channel_pair,
                  std::uint64_t total_sample_frames,
                  std::vector<std::string>* errors);

  // Appends one interleaved stereo frame (left/right 24-bit samples) to the
  // data chunk. left and right must be the same length. Returns false and
  // appends a message to errors if the session is not open, the buffers differ
  // in length, or the stream fails.
  bool AppendFrameAudio(const std::vector<std::int32_t>& left,
                        const std::vector<std::int32_t>& right,
                        std::vector<std::string>* errors);

  // Rewrites the header with the final chunk sizes and closes the file.
  // Returns false and appends a message to errors when the appended sample
  // count differs from the count declared to BeginWrite, or on any I/O error.
  bool FinalizeWrite(std::vector<std::string>* errors);

  // Abandons the current write session: closes the stream and removes the
  // partially-written WAV file. No-op when no session is open.
  void AbortWrite();

  // Derives the audio track path for `channel_pair` from a CVBS output path.
  // Strips a trailing ".cvbs" or ".cvbsy" suffix (if present) and appends
  // "_audio_<pair>.wav".
  static std::string DeriveAudioPath(const std::string& video_path,
                                     int channel_pair);

  // Returns the container required for a file carrying `total_sample_frames`
  // stereo 24-bit sample positions: RF64 when the RIFF chunk size (file size
  // minus 8) of a canonical RIFF file would exceed 0xFFFFFFFE, RIFF otherwise.
  static WavContainer ContainerForSampleFrames(
      std::uint64_t total_sample_frames);

  // Serialises the complete header for `container` describing
  // `total_sample_frames` stereo 24-bit sample positions at `sample_rate_hz`.
  // RIFF headers are 44 bytes; RF64 headers are 80 bytes, with the RF64 and
  // data 32-bit sizes set to 0xFFFFFFFF and the true sizes and sample count in
  // the ds64 chunk (EBU Tech 3306, table length 0).
  static std::vector<char> BuildHeader(WavContainer container,
                                       int sample_rate_hz,
                                       std::uint64_t total_sample_frames);

 private:
  ILogger* logger_;
  std::ofstream stream_;
  std::string audio_path_;
  bool session_open_ = false;
  int header_sample_rate_hz_ = 0;
  WavContainer container_ = WavContainer::kRiff;
  std::uint64_t expected_sample_frames_ = 0;
  std::uint64_t data_bytes_ = 0;
};

}  // namespace videosynth
