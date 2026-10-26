/*
 * File:        progressive_frame_source_probe.cpp
 * Module:      progressive_frame_source_probe
 * Purpose:     Probes progressive source metadata for profile validation and
 * frame-count semantics.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "videosynth/progressive_frame_source_probe.h"

#include <OpenEXR/ImfChannelList.h>
#include <OpenEXR/ImfFloatAttribute.h>
#include <OpenEXR/ImfHeader.h>
#include <OpenEXR/ImfInputFile.h>
#include <OpenEXR/ImfIntAttribute.h>
#include <OpenEXR/ImfRationalAttribute.h>
#include <OpenEXR/ImfStringAttribute.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>

namespace videosynth {
namespace {

namespace Imf = OPENEXR_IMF_NAMESPACE;

std::string Lowercase(std::string value) {
  std::transform(
      value.begin(), value.end(), value.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

bool EndsWithLowercase(const std::string& value, const std::string& suffix) {
  if (suffix.size() > value.size()) {
    return false;
  }
  return value.compare(value.size() - suffix.size(), suffix.size(), suffix) ==
         0;
}

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

bool ParseFfprobeKeyValueOutput(
    const std::string& output, std::map<std::string, std::string>* out_values) {
  if (out_values == nullptr) {
    return false;
  }

  out_values->clear();
  std::istringstream stream(output);
  std::string line;
  while (std::getline(stream, line)) {
    const std::size_t equals_pos = line.find('=');
    if (equals_pos == std::string::npos || equals_pos == 0) {
      continue;
    }

    const std::string key = line.substr(0, equals_pos);
    const std::string value = line.substr(equals_pos + 1);
    if (!key.empty()) {
      (*out_values)[key] = value;
    }
  }

  return true;
}

double ParseFrameRate(const std::string& rate_text) {
  if (rate_text.empty()) {
    return 0.0;
  }

  const std::size_t slash_pos = rate_text.find('/');
  if (slash_pos == std::string::npos) {
    return std::atof(rate_text.c_str());
  }

  const std::string numerator_text = rate_text.substr(0, slash_pos);
  const std::string denominator_text = rate_text.substr(slash_pos + 1);
  const double numerator = std::atof(numerator_text.c_str());
  const double denominator = std::atof(denominator_text.c_str());
  if (denominator == 0.0) {
    return 0.0;
  }
  return numerator / denominator;
}

double ParseRatio(const std::string& ratio_text) {
  if (ratio_text.empty() || ratio_text == "N/A" || ratio_text == "0:1") {
    return 0.0;
  }

  const std::size_t colon_pos = ratio_text.find(':');
  if (colon_pos == std::string::npos) {
    return std::atof(ratio_text.c_str());
  }

  const std::string numerator_text = ratio_text.substr(0, colon_pos);
  const std::string denominator_text = ratio_text.substr(colon_pos + 1);
  const double numerator = std::atof(numerator_text.c_str());
  const double denominator = std::atof(denominator_text.c_str());
  if (denominator == 0.0) {
    return 0.0;
  }
  return numerator / denominator;
}

int ParseIntegerOrZero(const std::string& value) {
  if (value.empty() || value == "N/A") {
    return 0;
  }
  return std::atoi(value.c_str());
}

bool ProbeExr(const Section& section,
              ProgressiveFrameSourceProfile* out_profile, std::string* error) {
  try {
    Imf::InputFile input_file(section.source.c_str());
    const Imf::Header& header = input_file.header();
    const Imf::ChannelList& channels = header.channels();
    const Imf::Channel* r_channel = channels.findChannel("R");
    const Imf::Channel* g_channel = channels.findChannel("G");
    const Imf::Channel* b_channel = channels.findChannel("B");
    if (r_channel == nullptr || g_channel == nullptr || b_channel == nullptr) {
      if (error != nullptr) {
        *error = "Progressive EXR must provide R, G, and B channels.";
      }
      return false;
    }

    if (r_channel->type != g_channel->type ||
        r_channel->type != b_channel->type) {
      if (error != nullptr) {
        *error =
            "Progressive EXR channels R/G/B must use a single shared channel "
            "type.";
      }
      return false;
    }

    if (r_channel->type != Imf::FLOAT) {
      if (error != nullptr) {
        *error = "Progressive EXR channels R/G/B must use 32-bit FLOAT type.";
      }
      return false;
    }

    const IMATH_NAMESPACE::Box2i data_window = header.dataWindow();
    const int width = (data_window.max.x - data_window.min.x) + 1;
    const int height = (data_window.max.y - data_window.min.y) + 1;

    const IMATH_NAMESPACE::Box2i display_window = header.displayWindow();
    if (data_window.min.x != 0 || data_window.min.y != 0 ||
        data_window.max.x != width - 1 || data_window.max.y != height - 1 ||
        display_window.min.x != data_window.min.x ||
        display_window.min.y != data_window.min.y ||
        display_window.max.x != data_window.max.x ||
        display_window.max.y != data_window.max.y) {
      if (error != nullptr) {
        *error =
            "Progressive EXR dataWindow/displayWindow must match full raster "
            "bounds.";
      }
      return false;
    }

    if (header.compression() != Imf::NO_COMPRESSION) {
      if (error != nullptr) {
        *error = "Progressive EXR must use no compression.";
      }
      return false;
    }

    if (header.lineOrder() != Imf::INCREASING_Y) {
      if (error != nullptr) {
        *error = "Progressive EXR must use increasing-y line order.";
      }
      return false;
    }

    const Imf::FloatAttribute* gamma =
        header.findTypedAttribute<Imf::FloatAttribute>("gamma");
    if (gamma == nullptr || std::abs(gamma->value() - 1.0F) > 1.0e-6F) {
      if (error != nullptr) {
        *error = "Progressive EXR must define gamma metadata equal to 1.";
      }
      return false;
    }

    const Imf::RationalAttribute* fps =
        header.findTypedAttribute<Imf::RationalAttribute>("framesPerSecond");
    if (fps == nullptr) {
      if (error != nullptr) {
        *error = "Progressive EXR is missing framesPerSecond metadata.";
      }
      return false;
    }

    const auto fps_value = fps->value();
    const bool is_pal_fps = fps_value.n == 25 && fps_value.d == 1;
    const bool is_ntsc_fps = fps_value.n == 30000 && fps_value.d == 1001;
    if (!is_pal_fps && !is_ntsc_fps) {
      if (error != nullptr) {
        *error =
            "Progressive EXR framesPerSecond metadata must be 25/1 or "
            "30000/1001.";
      }
      return false;
    }

    const float pal_pixel_aspect = 128.0F / 117.0F;
    const float ntsc_pixel_aspect = 108.0F / 119.0F;
    const float pixel_aspect = header.pixelAspectRatio();
    if (is_pal_fps) {
      if (width != 720 || height != 576 ||
          std::abs(pixel_aspect - pal_pixel_aspect) > 2.0e-3F) {
        if (error != nullptr) {
          *error =
              "Progressive PAL EXR profile must be 720x576 with supported PAL "
              "pixel aspect metadata.";
        }
        return false;
      }
    } else {
      if (width != 720 || height != 486 ||
          std::abs(pixel_aspect - ntsc_pixel_aspect) > 2.0e-3F) {
        if (error != nullptr) {
          *error =
              "Progressive NTSC EXR profile must be 720x486 with supported "
              "NTSC pixel aspect metadata.";
        }
        return false;
      }
    }

    out_profile->container = "exr";
    out_profile->codec = "openexr";
    out_profile->pixel_format = "rgbf";
    out_profile->bit_depth = 32;
    out_profile->width = width;
    out_profile->height = height;
    out_profile->frame_rate_hz = is_pal_fps ? 25.0 : (30000.0 / 1001.0);
    out_profile->frame_count = 1;
    return true;
  } catch (const std::exception& ex) {
    if (error != nullptr) {
      *error = std::string("Failed while probing progressive EXR source: ") +
               ex.what();
    }
    return false;
  }
}

// Runs ffprobe with the given arguments ahead of the source path and returns
// its standard output.
bool RunFfprobe(const std::string& source, const std::string& arguments,
                std::string* out_output, std::string* error) {
  const std::string escaped_source = EscapeForSingleQuotedShell(source);
  const std::string command =
      "ffprobe -v error " + arguments + " '" + escaped_source + "' 2>/dev/null";

  std::array<char, 4096> buffer{};
  std::string output;
  FILE* pipe = popen(command.c_str(), "r");
  if (pipe == nullptr) {
    if (error != nullptr) {
      *error = "Unable to run ffprobe for progressive source probing.";
    }
    return false;
  }

  while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) !=
         nullptr) {
    output += buffer.data();
  }

  const int rc = pclose(pipe);
  if (rc != 0) {
    if (error != nullptr) {
      *error = "ffprobe failed while probing progressive source profile.";
    }
    return false;
  }

  *out_output = std::move(output);
  return true;
}

// Runs one ffprobe query over the first video stream and returns its
// key=value output.
bool RunFfprobeQuery(const std::string& source, const std::string& entries,
                     std::map<std::string, std::string>* out_values,
                     std::string* error) {
  std::string output;
  if (!RunFfprobe(source,
                  "-select_streams v:0 " + entries +
                      " -of default=noprint_wrappers=1:nokey=0",
                  &output, error)) {
    return false;
  }
  ParseFfprobeKeyValueOutput(output, out_values);
  return true;
}

double ParseSecondsOrZero(const std::string& value) {
  if (value.empty() || value == "N/A") {
    return 0.0;
  }
  return std::atof(value.c_str());
}

// Reads the container start time and the audio stream list. ffprobe's compact
// writer prints one "section|key=value|..." line per stream and one for the
// format, so streams with equal keys cannot overwrite one another.
bool ProbeContainerTimingAndAudio(const std::string& source,
                                  ProgressiveFrameSourceProfile* out_profile,
                                  std::string* error) {
  std::string output;
  if (!RunFfprobe(source,
                  "-show_entries format=start_time "
                  "-show_entries "
                  "stream=index,codec_type,codec_name,channels,sample_rate,"
                  "start_time,duration "
                  "-of compact=p=1:nk=0",
                  &output, error)) {
    return false;
  }

  out_profile->audio_streams.clear();
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
      out_profile->container_start_time_seconds =
          ParseSecondsOrZero(values["start_time"]);
      continue;
    }
    if (section_name != "stream" || values["codec_type"] != "audio") {
      continue;
    }
    ProgressiveAudioStreamProfile audio;
    audio.stream_index = ParseIntegerOrZero(values["index"]);
    audio.codec = Lowercase(values["codec_name"]);
    audio.channels = ParseIntegerOrZero(values["channels"]);
    audio.sample_rate_hz = ParseIntegerOrZero(values["sample_rate"]);
    audio.duration_seconds = ParseSecondsOrZero(values["duration"]);
    audio.start_time_seconds = ParseSecondsOrZero(values["start_time"]);
    out_profile->audio_streams.push_back(std::move(audio));
  }
  return true;
}

// Resolves the frame count of a video source from container metadata: the
// stream's own nb_frames when the demuxer supplies it, otherwise duration times
// frame rate. Returns 0 when neither is available, leaving the caller to fall
// back to a counting pass.
int FrameCountFromContainerMetadata(
    const std::map<std::string, std::string>& values, double frame_rate) {
  const auto nb_frames = values.find("nb_frames");
  if (nb_frames != values.end()) {
    const int declared = ParseIntegerOrZero(nb_frames->second);
    if (declared > 0) {
      return declared;
    }
  }

  if (frame_rate <= 0.0) {
    return 0;
  }
  for (const char* key : {"duration", "stream_duration"}) {
    const auto duration = values.find(key);
    if (duration == values.end() || duration->second.empty() ||
        duration->second == "N/A") {
      continue;
    }
    const double seconds = std::atof(duration->second.c_str());
    if (seconds > 0.0) {
      return static_cast<int>(std::lround(seconds * frame_rate));
    }
  }
  return 0;
}

// Probes a video source's profile without decoding it: the metadata-only query
// is milliseconds, whereas `ffprobe -count_frames` decodes the whole clip. The
// counting pass runs only when the container declares neither a frame count nor
// a duration.
bool ProbeWithFfprobe(const std::string& source,
                      ProgressiveFrameSourceProfile* out_profile,
                      std::string* error) {
  std::map<std::string, std::string> values;
  if (!RunFfprobeQuery(source,
                       "-show_entries format=format_name,duration "
                       "-show_entries "
                       "stream=codec_name,pix_fmt,width,height,r_frame_rate,"
                       "avg_frame_rate,start_time,"
                       "nb_frames,bits_per_raw_sample,field_order,color_space,"
                       "color_primaries,color_transfer,color_range,"
                       "sample_aspect_ratio "
                       "-show_entries "
                       "stream_side_data=crop_left,crop_right,crop_top,"
                       "crop_bottom",
                       &values, error)) {
    return false;
  }

  const std::string format_name = Lowercase(values["format_name"]);
  const std::string codec_name = Lowercase(values["codec_name"]);
  if (codec_name.empty()) {
    if (error != nullptr) {
      *error =
          "Progressive section validation error: source has no video stream.";
    }
    return false;
  }
  const std::string pix_fmt = Lowercase(values["pix_fmt"]);
  const std::string field_order = Lowercase(values["field_order"]);
  const std::string color_space = Lowercase(values["color_space"]);
  const std::string color_primaries = Lowercase(values["color_primaries"]);
  const std::string color_transfer = Lowercase(values["color_transfer"]);
  const std::string color_range = Lowercase(values["color_range"]);
  const double sample_aspect_ratio = ParseRatio(values["sample_aspect_ratio"]);
  const int width = ParseIntegerOrZero(values["width"]);
  const int height = ParseIntegerOrZero(values["height"]);
  const double frame_rate = ParseFrameRate(values["r_frame_rate"]);
  int frame_count = FrameCountFromContainerMetadata(values, frame_rate);
  if (frame_count <= 0) {
    // Last resort: count decoded frames. Matroska demuxers usually declare a
    // duration, so this pass is rare.
    std::map<std::string, std::string> counted;
    if (RunFfprobeQuery(source,
                        "-count_frames -show_entries stream=nb_read_frames",
                        &counted, nullptr)) {
      frame_count = ParseIntegerOrZero(counted["nb_read_frames"]);
    }
  }
  const int crop_left = ParseIntegerOrZero(values["crop_left"]);
  const int crop_right = ParseIntegerOrZero(values["crop_right"]);
  const int crop_top = ParseIntegerOrZero(values["crop_top"]);
  const int crop_bottom = ParseIntegerOrZero(values["crop_bottom"]);

  int bit_depth = ParseIntegerOrZero(values["bits_per_raw_sample"]);
  if (bit_depth == 0 &&
      (pix_fmt == "yuv420p" || pix_fmt == "yuv422p" || pix_fmt == "yuv444p" ||
       pix_fmt == "yuvj420p" || pix_fmt == "yuvj422p")) {
    bit_depth = 8;
  }

  out_profile->container = format_name;
  out_profile->codec = codec_name;
  out_profile->pixel_format = pix_fmt;
  out_profile->field_order = field_order;
  out_profile->color_space = color_space;
  out_profile->color_primaries = color_primaries;
  out_profile->color_transfer = color_transfer;
  out_profile->color_range = color_range;
  out_profile->bit_depth = bit_depth;
  out_profile->width = width;
  out_profile->height = height;
  out_profile->sample_aspect_ratio = sample_aspect_ratio;
  out_profile->crop_left = crop_left;
  out_profile->crop_right = crop_right;
  out_profile->crop_top = crop_top;
  out_profile->crop_bottom = crop_bottom;
  out_profile->frame_rate_hz = frame_rate;
  out_profile->average_frame_rate_hz = ParseFrameRate(values["avg_frame_rate"]);
  out_profile->frame_count = frame_count;
  out_profile->video_start_time_seconds =
      ParseSecondsOrZero(values["start_time"]);
  return ProbeContainerTimingAndAudio(source, out_profile, error);
}

}  // namespace

bool ProgressiveFrameSourceProbe::Probe(
    const Section& section, ProgressiveFrameSourceProfile* out_profile,
    std::string* error) {
  if (out_profile == nullptr) {
    if (error != nullptr) {
      *error =
          "Progressive source probe output profile pointer must not be null.";
    }
    return false;
  }

  {
    const std::lock_guard<std::mutex> lock(profile_cache_mutex_);
    const auto cached = profile_cache_.find(section.source);
    if (cached != profile_cache_.end()) {
      *out_profile = cached->second;
      return true;
    }
  }

  // Reject a missing source before handing it to OpenEXR/ffprobe: those
  // libraries write their own open-failure diagnostics straight to stderr,
  // which bypasses the logger. A cheap existence check keeps the failure on
  // the logged path and yields one clear message.
  std::error_code exists_ec;
  if (!std::filesystem::exists(section.source, exists_ec)) {
    if (error != nullptr) {
      *error = "Progressive source not found: " + section.source;
    }
    return false;
  }

  const std::string source = Lowercase(section.source);
  bool probed = false;
  if (EndsWithLowercase(source, ".exr")) {
    probed = ProbeExr(section, out_profile, error);
  } else if (EndsWithLowercase(source, ".mkv") ||
             EndsWithLowercase(source, ".mp4")) {
    probed = ProbeWithFfprobe(section.source, out_profile, error);
  } else {
    if (error != nullptr) {
      *error = "Unsupported progressive source family during profile probing.";
    }
    return false;
  }

  if (!probed) {
    return false;
  }

  const std::lock_guard<std::mutex> lock(profile_cache_mutex_);
  profile_cache_[section.source] = *out_profile;
  return true;
}

}  // namespace videosynth