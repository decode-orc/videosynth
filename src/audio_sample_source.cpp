/*
 * File:        audio_sample_source.cpp
 * Module:      audio_sample_source
 * Purpose:     Streams stereo PCM from audio files and video soundtracks
 *              through an external ffmpeg process, and probes audio files
 *              with ffprobe.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "videosynth/audio_sample_source.h"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <map>
#include <sstream>
#include <system_error>

#include "videosynth/audio_sample_conversion.h"

namespace videosynth {
namespace {

constexpr int kStereoChannels = 2;

std::string EscapeForSingleQuotedShell(const std::string& value) {
  std::string escaped;
  escaped.reserve(value.size() + 8);
  for (char c : value) {
    if (c == '\'') {
      escaped += "'\\''";
      continue;
    }
    escaped.push_back(c);
  }
  return escaped;
}

double ParseSecondsOrZero(const std::string& value) {
  if (value.empty() || value == "N/A") {
    return 0.0;
  }
  return std::atof(value.c_str());
}

}  // namespace

FfmpegAudioSampleSource::~FfmpegAudioSampleSource() { Close(); }

int FfmpegAudioSampleSource::Close() {
  if (pipe_ == nullptr) {
    return 0;
  }
  // Closing the read end first makes a still-running ffmpeg fail its next
  // write and exit, so pclose does not wait for a whole-file decode.
  const int status = pclose(pipe_);
  pipe_ = nullptr;
  return status;
}

// static
std::string FfmpegAudioSampleSource::BuildCommand(const std::string& path,
                                                  int stream,
                                                  double start_seconds,
                                                  int sample_rate_hz) {
  std::ostringstream command;
  command << "ffmpeg -v error -nostdin";
  if (start_seconds > 0.0) {
    // Input-side seek: ffmpeg decodes from the preceding seek point and trims
    // the audio to the requested time.
    command << " -ss " << std::fixed << std::setprecision(6) << start_seconds;
  }
  command << " -i '" << EscapeForSingleQuotedShell(path) << "'"
          << " -map 0:a:" << stream << " -vn -sn -dn"
          << " -ac " << kStereoChannels << " -ar " << sample_rate_hz
          << " -f s32le - 2>/dev/null";
  return command.str();
}

bool FfmpegAudioSampleSource::Open(const std::string& path, int stream,
                                   double start_seconds, int sample_rate_hz,
                                   std::string* error) {
  Close();
  if (stream < 0 || sample_rate_hz <= 0) {
    if (error != nullptr) {
      *error = "Audio decode needs a non-negative stream and a positive rate.";
    }
    return false;
  }
  const std::string command =
      BuildCommand(path, stream, start_seconds, sample_rate_hz);
  pipe_ = popen(command.c_str(), "r");
  if (pipe_ == nullptr) {
    if (error != nullptr) {
      *error = "Unable to run ffmpeg for audio decoding.";
    }
    return false;
  }
  return true;
}

int FfmpegAudioSampleSource::Read(int count, std::vector<std::int32_t>* left,
                                  std::vector<std::int32_t>* right,
                                  std::string* error) {
  left->clear();
  right->clear();
  if (count <= 0 || pipe_ == nullptr) {
    // A closed source has already reached its end.
    return 0;
  }

  const std::size_t wanted =
      static_cast<std::size_t>(count) * static_cast<std::size_t>(2);
  interleaved_.resize(wanted);
  const std::size_t read =
      std::fread(interleaved_.data(), sizeof(std::int32_t), wanted, pipe_);
  const std::size_t frames = read / 2U;
  left->reserve(frames);
  right->reserve(frames);
  for (std::size_t i = 0; i < frames; ++i) {
    left->push_back(ConvertSample32To24(interleaved_[2U * i]));
    right->push_back(ConvertSample32To24(interleaved_[(2U * i) + 1U]));
  }

  if (read < wanted) {
    // End of stream: a non-zero status means the file failed to decode
    // rather than simply ended.
    const int status = Close();
    if (status != 0) {
      if (error != nullptr) {
        *error = "ffmpeg failed while decoding audio.";
      }
      return -1;
    }
  }
  return static_cast<int>(frames);
}

bool FfprobeAudioFileProbe::Probe(const std::string& path,
                                  AudioFileProfile* out_profile,
                                  std::string* error) {
  std::error_code exists_error;
  if (!std::filesystem::exists(path, exists_error)) {
    if (error != nullptr) {
      *error = "the file does not exist.";
    }
    return false;
  }
  const std::string command =
      "ffprobe -v error -select_streams a:0 "
      "-show_entries stream=channels,sample_rate,duration "
      "-show_entries format=duration -of compact=p=1:nk=0 '" +
      EscapeForSingleQuotedShell(path) + "' 2>/dev/null";
  FILE* pipe = popen(command.c_str(), "r");
  if (pipe == nullptr) {
    if (error != nullptr) {
      *error = "Unable to run ffprobe for audio file probing.";
    }
    return false;
  }
  std::array<char, 4096> buffer{};
  std::string output;
  while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) !=
         nullptr) {
    output += buffer.data();
  }
  if (pclose(pipe) != 0) {
    if (error != nullptr) {
      *error = "ffprobe could not read the audio file.";
    }
    return false;
  }

  // One "section|key=value|..." line per stream and one for the format.
  bool found_stream = false;
  double format_duration = 0.0;
  AudioFileProfile profile;
  std::istringstream lines(output);
  std::string line;
  while (std::getline(lines, line)) {
    std::istringstream fields(line);
    std::string section_name;
    std::getline(fields, section_name, '|');
    std::map<std::string, std::string> values;
    std::string field;
    while (std::getline(fields, field, '|')) {
      const std::size_t equals_pos = field.find('=');
      if (equals_pos != std::string::npos && equals_pos > 0) {
        values[field.substr(0, equals_pos)] = field.substr(equals_pos + 1);
      }
    }
    if (section_name == "format") {
      format_duration = ParseSecondsOrZero(values["duration"]);
    } else if (section_name == "stream" && !found_stream) {
      found_stream = true;
      profile.channels = std::atoi(values["channels"].c_str());
      profile.sample_rate_hz = std::atoi(values["sample_rate"].c_str());
      profile.duration_seconds = ParseSecondsOrZero(values["duration"]);
    }
  }
  if (!found_stream) {
    if (error != nullptr) {
      *error = "the file has no audio stream.";
    }
    return false;
  }
  if (profile.duration_seconds <= 0.0) {
    // FLAC streams often declare their length only at the container level.
    profile.duration_seconds = format_duration;
  }
  *out_profile = profile;
  return true;
}

}  // namespace videosynth
