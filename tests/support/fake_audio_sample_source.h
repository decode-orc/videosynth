/*
 * File:        fake_audio_sample_source.h
 * Module:      test_support
 * Purpose:     Deterministic in-memory IAudioSampleSource for audio tests.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "videosynth/audio_sample_source.h"

namespace videosynth::testing {

// One fake media file: an audio stream of `length_seconds` whose first sample
// sits `start_seconds` after the container start. At any sample rate, the
// sample at container position q (q = time * rate) reads as q + 1 on the left
// and -(q + 1) on the right, so a test can tell exactly which instant of the
// media each output sample came from.
struct FakeAudioMedia {
  double start_seconds = 0.0;
  double length_seconds = 0.0;
};

// Every Open made through any source from one FakeAudioLibrary.
struct FakeAudioOpen {
  std::string path;
  int stream = 0;
  double start_seconds = 0.0;
  int sample_rate_hz = 0;
};

// Shared state behind the sources a factory hands out.
struct FakeAudioLibrary {
  std::map<std::string, FakeAudioMedia> media;
  std::vector<FakeAudioOpen> opens;
  // When set, Read fails with this message.
  std::string read_error;
};

class FakeAudioSampleSource final : public IAudioSampleSource {
 public:
  explicit FakeAudioSampleSource(FakeAudioLibrary* library)
      : library_(library) {}

  bool Open(const std::string& path, int stream, double start_seconds,
            int sample_rate_hz, std::string* error) override {
    library_->opens.push_back({path, stream, start_seconds, sample_rate_hz});
    const auto found = library_->media.find(path);
    if (found == library_->media.end()) {
      if (error != nullptr) {
        *error = "no such fake media: " + path;
      }
      return false;
    }
    const std::int64_t first =
        std::llround(found->second.start_seconds * sample_rate_hz);
    end_ = first + std::llround(found->second.length_seconds * sample_rate_hz);
    // As with ffmpeg, a seek before the stream starts begins at its start.
    position_ = std::max<std::int64_t>(
        first, std::llround(start_seconds * sample_rate_hz));
    open_ = true;
    return true;
  }

  int Read(int count, std::vector<std::int32_t>* left,
           std::vector<std::int32_t>* right, std::string* error) override {
    left->clear();
    right->clear();
    if (!library_->read_error.empty()) {
      if (error != nullptr) {
        *error = library_->read_error;
      }
      return -1;
    }
    if (!open_) {
      return 0;
    }
    const std::int64_t available = std::max<std::int64_t>(0, end_ - position_);
    const int n = static_cast<int>(std::min<std::int64_t>(count, available));
    for (int i = 0; i < n; ++i) {
      const auto value = static_cast<std::int32_t>(position_ + i + 1);
      left->push_back(value);
      right->push_back(-value);
    }
    position_ += n;
    if (n < count) {
      open_ = false;
    }
    return n;
  }

 private:
  FakeAudioLibrary* library_;
  std::int64_t position_ = 0;
  std::int64_t end_ = 0;
  bool open_ = false;
};

inline AudioSampleSourceFactory MakeFakeAudioFactory(
    FakeAudioLibrary* library) {
  return [library] { return std::make_unique<FakeAudioSampleSource>(library); };
}

// The value FakeAudioSampleSource gives the sample at `seconds` after the
// container start, at `rate` Hz, offset by `extra` samples.
inline std::int32_t FakeSampleAt(double seconds, int rate,
                                 std::int64_t extra = 0) {
  return static_cast<std::int32_t>(std::llround(seconds * rate) + extra + 1);
}

}  // namespace videosynth::testing
