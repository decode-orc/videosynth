/*
 * File:        audio_media_reader.h
 * Module:      audio_media_reader
 * Purpose:     Produces frame-sized blocks of a channel pair's file or video
 *              soundtrack audio, following the section's source time.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "videosynth/audio_sample_source.h"
#include "videosynth/model.h"

namespace videosynth {

// Where a video's soundtrack sits against its pictures. Both offsets are
// measured from the container start, the origin of an ffmpeg input seek.
struct VideoAudioTiming {
  // Picture rate of the source.
  double frame_rate_hz = 0.0;
  // Frame rate of the output standard; a position's output fields last half
  // an output frame each.
  double output_frame_rate_hz = 0.0;
  // First video frame time minus the container start.
  double video_start_offset_seconds = 0.0;
  // First sample time of the audio stream minus the container start.
  double audio_start_offset_seconds = 0.0;
};

// Reads one channel pair's media audio for consecutive output frames at one
// sample rate. Each section run starts with BeginFileRun or BeginVideoRun;
// ReadFrame then supplies each output frame's samples in output order.
//
// File runs play the file from its start; at its end the rest of the run is
// silent, or the file restarts when `repeat` is set.
//
// Video runs take each frame's samples from its source time: the start of a
// source frame (index / picture rate) plus some output fields, plus the video
// start offset. A video-cadence frame starts at the source frame it shows; a
// 2:3 pulldown frame is a whole number of fields into its pass through the
// film range. The reader keeps streaming while each frame starts one output
// frame after the last; any other step (a loop, a `duration_repeat` replay)
// reopens the stream at the new time so the sound follows the picture. A
// frame before the soundtrack starts, or after it ends, is silent.
//
// Thread-safety: NOT thread-safe; owned by one AudioTrackGenerator.
class AudioMediaReader {
 public:
  AudioMediaReader(AudioSampleSourceFactory factory, int sample_rate_hz);

  // Starts a run that plays `file` from its first sample.
  void BeginFileRun(const AudioFileInput& file);

  // Starts a run that follows the source time of `source`'s audio stream
  // `stream` (0-based among its audio streams).
  void BeginVideoRun(const std::string& source, int stream,
                     const VideoAudioTiming& timing);

  // Replaces left and right with `sample_count` samples per channel for the
  // next output frame, whose source time is the start of
  // `source_frame_index` plus `fields_after` output fields (both ignored for
  // file runs). Returns false and sets error when the media cannot be read.
  bool ReadFrame(int sample_count, int source_frame_index,
                 std::int64_t fields_after, std::vector<std::int32_t>* left,
                 std::vector<std::int32_t>* right, std::string* error);

 private:
  enum class RunKind { kNone, kFile, kVideo };

  // Opens the stream at `start_seconds` after the container start.
  bool OpenAt(double start_seconds, std::string* error);
  // Positions a video run at `source_seconds` after the first picture.
  bool SeekVideo(double source_seconds, std::string* error);
  // Appends up to `count` samples from the open stream, restarting a
  // repeating file at its end; fewer are appended once the stream ends.
  // Returns false and sets error on a read failure.
  bool AppendFromStream(int count, std::vector<std::int32_t>* left,
                        std::vector<std::int32_t>* right, std::string* error);

  AudioSampleSourceFactory factory_;
  std::unique_ptr<IAudioSampleSource> source_;
  int sample_rate_hz_ = 0;

  RunKind kind_ = RunKind::kNone;
  std::string path_;
  int stream_ = 0;
  bool repeat_ = false;
  VideoAudioTiming timing_;

  // True once the run's stream has been opened (opening is deferred to the
  // first ReadFrame so that errors reach the caller).
  bool opened_ = false;
  // True once the open stream has no more samples.
  bool ended_ = false;
  // Samples read since the stream was last opened; a repeating file that
  // yields none on a fresh open is treated as empty rather than reopened
  // forever.
  std::int64_t samples_since_open_ = 0;
  // Silent samples still owed before the soundtrack's first sample.
  std::int64_t leading_silence_ = 0;
  // Source time (after the first picture) the stream is positioned at for a
  // video run.
  double next_source_seconds_ = 0.0;

  std::vector<std::int32_t> read_left_;
  std::vector<std::int32_t> read_right_;
};

}  // namespace videosynth
