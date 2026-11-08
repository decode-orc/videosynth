/*
 * File:        audio_efm_writer.cpp
 * Module:      audio_efm_writer
 * Purpose:     Streams one audio channel pair to a LaserDisc digital audio
 *              (EFM) T-value file and its frame-index sidecar alongside the
 *              generated CVBS output.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "videosynth/audio_efm_writer.h"

#include "videosynth/audio_sample_conversion.h"

namespace videosynth {

AudioEfmWriter::AudioEfmWriter(ILogger* logger)
    : logger_(logger), file_(logger) {}

bool AudioEfmWriter::BeginWrite(const Project& project, int channel_pair,
                                const efm::TrackTable& track_table,
                                std::vector<std::string>* errors) {
  if (errors == nullptr) {
    return false;
  }
  if (file_.is_open()) {
    errors->push_back("EFM audio write session already open.");
    return false;
  }

  const std::string audio_path = DeriveAudioPath(project.output.video_path);
  if (!encoder_.Begin(track_table)) {
    errors->push_back("EFM track table is not a valid subcode layout for: " +
                      audio_path);
    return false;
  }
  if (!file_.Open(project.output.video_path, errors)) {
    encoder_.Reset();
    return false;
  }

  if (logger_ != nullptr) {
    logger_->Debug("EFM audio output carries channel pair " +
                   std::to_string(channel_pair));
  }
  return true;
}

bool AudioEfmWriter::AppendFrameAudio(const std::vector<std::int32_t>& left,
                                      const std::vector<std::int32_t>& right,
                                      std::vector<std::string>* errors) {
  if (errors == nullptr) {
    return false;
  }
  if (!file_.is_open()) {
    errors->push_back("EFM audio write session is not open.");
    return false;
  }
  if (left.size() != right.size()) {
    errors->push_back(
        "EFM audio left/right channel buffers differ in length for: " +
        file_.path());
    return false;
  }

  // IEC 60908-1999 clause 12: compact-disc samples are 16-bit two's complement.
  std::vector<std::uint8_t> t_values;
  if (!encoder_.PushSamples(ConvertSamples24To16(left),
                            ConvertSamples24To16(right), &t_values)) {
    errors->push_back("EFM audio encoder rejected samples for: " +
                      file_.path());
    return false;
  }

  // One efm_frame row per appended frame: the CIRC pipeline latency means a
  // frame's t-values are not the encoding of that frame's own samples, but the
  // index stays contiguous and covers the whole file.
  return file_.AppendFrame(t_values, errors);
}

bool AudioEfmWriter::FinalizeWrite(std::vector<std::string>* errors) {
  if (errors == nullptr) {
    return false;
  }
  if (!file_.is_open()) {
    errors->push_back("EFM audio write session is not open.");
    return false;
  }

  std::vector<std::uint8_t> t_values;
  if (!encoder_.Flush(&t_values)) {
    errors->push_back("EFM audio encoder failed to flush for: " + file_.path());
    file_.Abort();
    return false;
  }
  // The CIRC-pipeline residue extends the last frame's run rather than
  // becoming an unindexed tail.
  if (!file_.AppendToLastFrame(t_values, errors)) {
    file_.Abort();
    return false;
  }
  const std::uint64_t t_value_count = file_.t_value_count();
  const std::string path = file_.path();
  encoder_.Reset();
  if (!file_.Close(errors)) {
    return false;
  }
  if (logger_ != nullptr) {
    logger_->Info("Wrote EFM audio track: " + path + " (" +
                  std::to_string(t_value_count) + " T values)");
  }
  return true;
}

void AudioEfmWriter::AbortWrite() {
  file_.Abort();
  encoder_.Reset();
}

}  // namespace videosynth
