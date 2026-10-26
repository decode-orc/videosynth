/*
 * File:        audio_media_reader.cpp
 * Module:      audio_media_reader
 * Purpose:     Produces frame-sized blocks of a channel pair's file or video
 *              soundtrack audio, following the section's source frames.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "videosynth/audio_media_reader.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace videosynth {

AudioMediaReader::AudioMediaReader(AudioSampleSourceFactory factory,
                                   int sample_rate_hz)
    : factory_(std::move(factory)), sample_rate_hz_(sample_rate_hz) {}

void AudioMediaReader::BeginFileRun(const AudioFileInput& file) {
  kind_ = RunKind::kFile;
  path_ = file.path;
  stream_ = 0;
  repeat_ = file.repeat;
  opened_ = false;
  ended_ = false;
  leading_silence_ = 0;
}

void AudioMediaReader::BeginVideoRun(const std::string& source, int stream,
                                     const VideoAudioTiming& timing) {
  kind_ = RunKind::kVideo;
  path_ = source;
  stream_ = stream;
  repeat_ = false;
  timing_ = timing;
  opened_ = false;
  ended_ = false;
  leading_silence_ = 0;
}

bool AudioMediaReader::OpenAt(double start_seconds, std::string* error) {
  if (source_ == nullptr) {
    source_ = factory_();
  }
  if (source_ == nullptr ||
      !source_->Open(path_, stream_, start_seconds, sample_rate_hz_, error)) {
    if (error != nullptr && error->empty()) {
      *error = "Unable to open audio from '" + path_ + "'.";
    }
    return false;
  }
  opened_ = true;
  ended_ = false;
  samples_since_open_ = 0;
  return true;
}

bool AudioMediaReader::SeekVideo(int source_frame_index, std::string* error) {
  double start =
      timing_.video_start_offset_seconds +
      (static_cast<double>(source_frame_index) / timing_.frame_rate_hz);
  leading_silence_ = 0;
  if (start < timing_.audio_start_offset_seconds) {
    // The picture starts before its soundtrack: hold silence until the
    // soundtrack's first sample, then play from there.
    leading_silence_ = std::llround(
        (timing_.audio_start_offset_seconds - start) * sample_rate_hz_);
    start = timing_.audio_start_offset_seconds;
  }
  return OpenAt(start, error);
}

bool AudioMediaReader::AppendFromStream(int count,
                                        std::vector<std::int32_t>* left,
                                        std::vector<std::int32_t>* right,
                                        std::string* error) {
  int remaining = count;
  while (remaining > 0 && !ended_) {
    const int read = source_->Read(remaining, &read_left_, &read_right_, error);
    if (read < 0) {
      return false;
    }
    left->insert(left->end(), read_left_.begin(), read_left_.end());
    right->insert(right->end(), read_right_.begin(), read_right_.end());
    samples_since_open_ += read;
    remaining -= read;
    if (remaining == 0) {
      break;
    }
    // A short read is the end of the stream.
    if (kind_ == RunKind::kFile && repeat_ && samples_since_open_ > 0) {
      if (!OpenAt(0.0, error)) {
        return false;
      }
      continue;
    }
    ended_ = true;
  }
  return true;
}

bool AudioMediaReader::ReadFrame(int sample_count, int source_frame_index,
                                 std::vector<std::int32_t>* left,
                                 std::vector<std::int32_t>* right,
                                 std::string* error) {
  left->clear();
  right->clear();
  if (kind_ == RunKind::kNone) {
    left->assign(static_cast<std::size_t>(sample_count), 0);
    right->assign(static_cast<std::size_t>(sample_count), 0);
    return true;
  }

  if (kind_ == RunKind::kFile) {
    if (!opened_ && !OpenAt(0.0, error)) {
      return false;
    }
  } else if (!opened_ || source_frame_index != next_source_frame_) {
    if (!SeekVideo(source_frame_index, error)) {
      return false;
    }
  }
  next_source_frame_ = source_frame_index + 1;

  const int silent =
      static_cast<int>(std::min<std::int64_t>(leading_silence_, sample_count));
  left->assign(static_cast<std::size_t>(silent), 0);
  right->assign(static_cast<std::size_t>(silent), 0);
  leading_silence_ -= silent;

  if (!AppendFromStream(sample_count - silent, left, right, error)) {
    return false;
  }
  // Past the end of the media the run is silent.
  left->resize(static_cast<std::size_t>(sample_count), 0);
  right->resize(static_cast<std::size_t>(sample_count), 0);
  return true;
}

}  // namespace videosynth
