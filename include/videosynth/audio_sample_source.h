/*
 * File:        audio_sample_source.h
 * Module:      audio_sample_source
 * Purpose:     Streams stereo PCM from audio files and video soundtracks, and
 *              probes audio files, behind interfaces so callers can substitute
 *              fakes.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace videosynth {

// Reads stereo 24-bit samples sequentially from one audio stream of a media
// file, resampled to the requested rate. Mono streams arrive on both channels;
// streams with more than two channels are downmixed to stereo.
//
// Thread-safety: implementations are NOT thread-safe. A source belongs to a
// single caller; distinct instances may run concurrently.
class IAudioSampleSource {
 public:
  virtual ~IAudioSampleSource() = default;

  // Opens `stream` (0-based index among the file's audio streams) of `path`,
  // positioned `start_seconds` after the container start, resampled to
  // `sample_rate_hz`. Closes any previous stream first. Returns false and
  // sets error when the read cannot be started.
  virtual bool Open(const std::string& path, int stream, double start_seconds,
                    int sample_rate_hz, std::string* error) = 0;

  // Replaces left and right with the next `count` samples per channel (24-bit
  // values in int32). Returns the number read; fewer than `count` means the
  // stream has ended. Returns -1 and sets error on a decode failure.
  virtual int Read(int count, std::vector<std::int32_t>* left,
                   std::vector<std::int32_t>* right, std::string* error) = 0;
};

// Creates a fresh, unopened sample source.
using AudioSampleSourceFactory =
    std::function<std::unique_ptr<IAudioSampleSource>()>;

// Decodes through an external `ffmpeg` process writing interleaved 32-bit
// stereo PCM to a pipe; samples are rounded to 24 bits on read
// (ConvertSample32To24).
//
// Thread-safety: NOT thread-safe (see IAudioSampleSource).
class FfmpegAudioSampleSource final : public IAudioSampleSource {
 public:
  FfmpegAudioSampleSource() = default;
  ~FfmpegAudioSampleSource() override;

  // Owns a child process; neither copying nor moving is meaningful.
  FfmpegAudioSampleSource(const FfmpegAudioSampleSource&) = delete;
  FfmpegAudioSampleSource& operator=(const FfmpegAudioSampleSource&) = delete;
  FfmpegAudioSampleSource(FfmpegAudioSampleSource&&) = delete;
  FfmpegAudioSampleSource& operator=(FfmpegAudioSampleSource&&) = delete;

  bool Open(const std::string& path, int stream, double start_seconds,
            int sample_rate_hz, std::string* error) override;
  int Read(int count, std::vector<std::int32_t>* left,
           std::vector<std::int32_t>* right, std::string* error) override;

  // Builds the ffmpeg command line for an Open call. Exposed for unit tests.
  static std::string BuildCommand(const std::string& path, int stream,
                                  double start_seconds, int sample_rate_hz);

 private:
  // Closes the pipe; returns the child's exit status from pclose.
  int Close();

  FILE* pipe_ = nullptr;
  // Interleaved left/right 32-bit samples, reused across reads.
  std::vector<std::int32_t> interleaved_;
};

// What validation needs to know about an audio file's first audio stream.
struct AudioFileProfile {
  int channels = 0;
  int sample_rate_hz = 0;
  // 0 when neither the stream nor the container declares a duration.
  double duration_seconds = 0.0;
};

// Thread-safety: implementations must be thread-safe; Probe may be called
// concurrently from multiple threads.
class IAudioFileProbe {
 public:
  virtual ~IAudioFileProbe() = default;

  // Probes the first audio stream of `path`. Returns false and sets error when
  // the file does not exist, cannot be read, or has no audio stream.
  virtual bool Probe(const std::string& path, AudioFileProfile* out_profile,
                     std::string* error) = 0;
};

// Probes audio files through an external `ffprobe` process.
//
// Thread-safety: thread-safe (no state).
class FfprobeAudioFileProbe final : public IAudioFileProbe {
 public:
  bool Probe(const std::string& path, AudioFileProfile* out_profile,
             std::string* error) override;
};

}  // namespace videosynth
