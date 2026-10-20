/*
 * File:        progressive_frame_source.cpp
 * Module:      progressive_frame_source
 * Purpose:     Generates fixed-format 10-bit 4:4:4 BT.601 frame data for
 * frame-based sources.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "videosynth/progressive_frame_source.h"

#include <Imath/half.h>
#include <OpenEXR/ImfChannelList.h>
#include <OpenEXR/ImfFloatAttribute.h>
#include <OpenEXR/ImfFrameBuffer.h>
#include <OpenEXR/ImfHeader.h>
#include <OpenEXR/ImfInputFile.h>
#include <OpenEXR/ImfIntAttribute.h>
#include <OpenEXR/ImfRationalAttribute.h>
#include <OpenEXR/ImfStringAttribute.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <system_error>
#include <utility>
#include <vector>

namespace videosynth {

namespace {

namespace Imf = OPENEXR_IMF_NAMESPACE;
namespace ImathNs = IMATH_NAMESPACE;

constexpr int kPalWidth = 720;
constexpr int kPalHeight = 576;
constexpr int kPalActiveX = 0;
constexpr int kPalActiveY = 0;
constexpr int kPalActiveWidth = 720;
constexpr int kPalActiveHeight = 576;

constexpr int kNtscWidth = 720;
constexpr int kNtscHeight = 486;
constexpr int kNtscActiveX = 0;
constexpr int kNtscActiveY = 0;
constexpr int kNtscActiveWidth = 720;
constexpr int kNtscActiveHeight = 486;

int ClampCode(int code, int lo, int hi) {
  return std::max(lo, std::min(code, hi));
}

std::int16_t ClampYCode(int code) {
  return static_cast<std::int16_t>(ClampCode(code, 64, 940));
}

std::int16_t ClampChromaCode(int code) {
  return static_cast<std::int16_t>(ClampCode(code, 64, 960));
}

YCbCr444Pixel MakePixel(int y_code, int cb_code, int cr_code) {
  return YCbCr444Pixel{
      .y = ClampYCode(y_code),
      .cb = ClampChromaCode(cb_code),
      .cr = ClampChromaCode(cr_code),
  };
}

YCbCr444Pixel MakeRawPixel(std::uint16_t y_code, std::uint16_t cb_code,
                           std::uint16_t cr_code) {
  return YCbCr444Pixel{
      .y = static_cast<std::int16_t>(
          ClampCode(static_cast<int>(y_code), 0, 1023)),
      .cb = static_cast<std::int16_t>(
          ClampCode(static_cast<int>(cb_code), 0, 1023)),
      .cr = static_cast<std::int16_t>(
          ClampCode(static_cast<int>(cr_code), 0, 1023)),
  };
}

bool EndsWithLowercase(const std::string& value, const std::string& suffix) {
  if (suffix.size() > value.size()) {
    return false;
  }
  return value.compare(value.size() - suffix.size(), suffix.size(), suffix) ==
         0;
}

void SetFrameGeometryForStandard(Standard standard, FrameSourceImage* image);

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

int ParseIntegerOrZero(const std::string& value) {
  if (value.empty() || value == "N/A") {
    return 0;
  }
  return std::atoi(value.c_str());
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

bool ContainsCsvToken(const std::string& csv, const std::string& token) {
  if (token.empty()) {
    return false;
  }

  std::size_t start = 0;
  while (start <= csv.size()) {
    const std::size_t comma = csv.find(',', start);
    const std::size_t end = (comma == std::string::npos) ? csv.size() : comma;
    const std::string item = csv.substr(start, end - start);
    if (item == token) {
      return true;
    }
    if (comma == std::string::npos) {
      break;
    }
    start = comma + 1;
  }
  return false;
}

// Validates the container/codec/colour profile of an MKV source and reports its
// raster in the same pass. Frame counting is deliberately absent: `ffprobe
// -count_frames` decodes the whole clip, and the decode below already yields an
// exact count from the payload it produces.
bool ValidateMkvProfileWithFfprobe(const std::string& source, Standard standard,
                                   int* out_width, int* out_height,
                                   std::string* error) {
  const std::string escaped_source = EscapeForSingleQuotedShell(source);
  const std::string command =
      "ffprobe -v error -select_streams v:0 "
      "-show_entries format=format_name "
      "-show_entries "
      "stream=codec_name,pix_fmt,width,height,r_frame_rate,bits_per_raw_sample,"
      "field_order,color_space,color_primaries,color_transfer,color_range,"
      "sample_aspect_ratio "
      "-show_entries "
      "stream_side_data=crop_left,crop_right,crop_top,crop_bottom "
      "-of default=noprint_wrappers=1:nokey=0 '" +
      escaped_source + "' 2>/dev/null";

  std::array<char, 4096> buffer{};
  std::string output;
  FILE* pipe = popen(command.c_str(), "r");
  if (pipe == nullptr) {
    if (error != nullptr) {
      *error = "Unable to run ffprobe for progressive MKV profile validation.";
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
      *error = "ffprobe failed while validating progressive MKV profile.";
    }
    return false;
  }

  std::map<std::string, std::string> values;
  ParseFfprobeKeyValueOutput(output, &values);

  const std::string format_name = values["format_name"];
  const std::string codec_name = values["codec_name"];
  const std::string pix_fmt = values["pix_fmt"];
  const int width = ParseIntegerOrZero(values["width"]);
  const int height = ParseIntegerOrZero(values["height"]);
  const double frame_rate = ParseFrameRate(values["r_frame_rate"]);
  const std::string field_order = values["field_order"];
  const std::string color_space = values["color_space"];
  const std::string color_primaries = values["color_primaries"];
  const std::string color_transfer = values["color_transfer"];
  const std::string color_range = values["color_range"];
  const double sample_aspect_ratio = ParseRatio(values["sample_aspect_ratio"]);
  const int crop_left = ParseIntegerOrZero(values["crop_left"]);
  const int crop_right = ParseIntegerOrZero(values["crop_right"]);
  const int crop_top = ParseIntegerOrZero(values["crop_top"]);
  const int crop_bottom = ParseIntegerOrZero(values["crop_bottom"]);

  if (out_width != nullptr) {
    *out_width = width;
  }
  if (out_height != nullptr) {
    *out_height = height;
  }

  if (!ContainsCsvToken(format_name, "matroska")) {
    if (error != nullptr) {
      *error = "Progressive MKV sections require a Matroska container profile.";
    }
    return false;
  }

  if (codec_name != "ffv1") {
    if (error != nullptr) {
      *error = "Progressive MKV sections only support FFV1 video codec.";
    }
    return false;
  }

  if (pix_fmt != "yuv422p10le") {
    if (error != nullptr) {
      *error =
          "Progressive MKV sections only support yuv422p10le pixel format.";
    }
    return false;
  }

  const int bit_depth = ParseIntegerOrZero(values["bits_per_raw_sample"]);
  if (bit_depth > 0 && bit_depth != 10) {
    if (error != nullptr) {
      *error = "Progressive MKV sections only support 10-bit sample depth.";
    }
    return false;
  }

  if (color_space != "smpte170m") {
    if (error != nullptr) {
      *error =
          "Progressive MKV sections require smpte170m color matrix metadata.";
    }
    return false;
  }

  if (!color_range.empty() && color_range != "unknown" && color_range != "tv") {
    if (error != nullptr) {
      *error =
          "Progressive MKV sections require tv or unknown color range "
          "metadata.";
    }
    return false;
  }

  if (crop_left != 0 || crop_right != 0 || crop_top != 0 || crop_bottom != 0) {
    if (error != nullptr) {
      *error =
          "Progressive MKV sections must not include stream crop metadata.";
    }
    return false;
  }

  if (Is625LineStandard(standard)) {
    if (width != 720 || height != 576) {
      if (error != nullptr) {
        *error =
            "Progressive MKV raster must be 720x576 for PAL/SECAM and 720x486 "
            "for "
            "NTSC or PAL-M.";
      }
      return false;
    }
    if (std::abs(frame_rate - 25.0) > 1.0e-3) {
      if (error != nullptr) {
        *error = "Progressive MKV frame rate must match selected standard.";
      }
      return false;
    }
    if (field_order != "tb") {
      if (error != nullptr) {
        *error =
            "Progressive PAL MKV sections require top-field-first field order "
            "metadata (tb).";
      }
      return false;
    }
    if (color_primaries != "bt470bg") {
      if (error != nullptr) {
        *error =
            "Progressive PAL MKV sections require bt470bg color primaries "
            "metadata.";
      }
      return false;
    }
    if (!(color_transfer == "bt709" || color_transfer == "bt470bg")) {
      if (error != nullptr) {
        *error =
            "Progressive PAL MKV sections require bt709 or bt470bg transfer "
            "metadata.";
      }
      return false;
    }
    const double expected_sar = 128.0 / 117.0;
    if (sample_aspect_ratio > 0.0 &&
        std::abs(sample_aspect_ratio - expected_sar) > 2.0e-3) {
      if (error != nullptr) {
        *error =
            "Progressive PAL MKV sample aspect ratio metadata is outside "
            "supported tolerance.";
      }
      return false;
    }
  } else if (standard == Standard::kNtsc) {
    if (width != 720 || height != 486) {
      if (error != nullptr) {
        *error =
            "Progressive MKV raster must be 720x576 for PAL/SECAM and 720x486 "
            "for "
            "NTSC or PAL-M.";
      }
      return false;
    }
    const double ntsc_rate = 30000.0 / 1001.0;
    if (std::abs(frame_rate - ntsc_rate) > 1.0e-3) {
      if (error != nullptr) {
        *error = "Progressive MKV frame rate must match selected standard.";
      }
      return false;
    }
    if (field_order != "bt") {
      if (error != nullptr) {
        *error =
            "Progressive NTSC MKV sections require bottom-field-first field "
            "order metadata (bt).";
      }
      return false;
    }
    if (color_primaries != "smpte170m") {
      if (error != nullptr) {
        *error =
            "Progressive NTSC MKV sections require smpte170m color primaries "
            "metadata.";
      }
      return false;
    }
    if (!(color_transfer == "bt709" || color_transfer == "smpte170m")) {
      if (error != nullptr) {
        *error =
            "Progressive NTSC MKV sections require bt709 or smpte170m transfer "
            "metadata.";
      }
      return false;
    }
    const double expected_sar = 108.0 / 119.0;
    if (sample_aspect_ratio > 0.0 &&
        std::abs(sample_aspect_ratio - expected_sar) > 2.0e-3) {
      if (error != nullptr) {
        *error =
            "Progressive NTSC MKV sample aspect ratio metadata is outside "
            "supported tolerance.";
      }
      return false;
    }
  } else if (standard == Standard::kPalM) {
    if (width != 720 || height != 486) {
      if (error != nullptr) {
        *error =
            "Progressive MKV raster must be 720x576 for PAL/SECAM and 720x486 "
            "for "
            "NTSC or PAL-M.";
      }
      return false;
    }
    const double palm_rate = 30000.0 / 1001.0;
    if (std::abs(frame_rate - palm_rate) > 1.0e-3) {
      if (error != nullptr) {
        *error = "Progressive MKV frame rate must match selected standard.";
      }
      return false;
    }
    if (field_order != "bt") {
      if (error != nullptr) {
        *error =
            "Progressive PAL-M MKV sections require bottom-field-first field "
            "order metadata (bt).";
      }
      return false;
    }
    if (color_primaries != "bt470bg" && color_primaries != "smpte170m") {
      if (error != nullptr) {
        *error =
            "Progressive PAL-M MKV sections require bt470bg or smpte170m color "
            "primaries metadata.";
      }
      return false;
    }
    if (!(color_transfer == "bt709" || color_transfer == "bt470bg" ||
          color_transfer == "smpte170m")) {
      if (error != nullptr) {
        *error =
            "Progressive PAL-M MKV sections require bt709, bt470bg, or "
            "smpte170m transfer metadata.";
      }
      return false;
    }
    const double expected_sar = 108.0 / 119.0;
    if (sample_aspect_ratio > 0.0 &&
        std::abs(sample_aspect_ratio - expected_sar) > 2.0e-3) {
      if (error != nullptr) {
        *error =
            "Progressive PAL-M MKV sample aspect ratio metadata is outside "
            "supported tolerance.";
      }
      return false;
    }
  }

  return true;
}

// Decodes up to max_frames frames (all of them when max_frames <= 0) of an MKV
// source into shared, immutable frame images.
//
// The clip is decoded exactly once: its profile and raster come from a single
// metadata-only ffprobe call, and the frame count is derived from the size of
// the decoded payload rather than from a second counting pass.
bool DecodeMkvFrames(
    const std::string& source, Standard standard, int max_frames,
    std::vector<std::shared_ptr<const FrameSourceImage>>* out_frames,
    std::string* error) {
  if (out_frames == nullptr) {
    if (error != nullptr) {
      *error = "Decoded MKV frame output pointer must not be null.";
    }
    return false;
  }

  int source_width = 0;
  int source_height = 0;
  if (!ValidateMkvProfileWithFfprobe(source, standard, &source_width,
                                     &source_height, error)) {
    return false;
  }

  const bool is_625_line = Is625LineStandard(standard);
  const int expected_height = is_625_line ? kPalHeight : kNtscHeight;
  if (source_height != expected_height || source_width != 720) {
    if (error != nullptr) {
      *error =
          "Progressive MKV raster must be 720x576 for PAL/SECAM and 720x486 "
          "for "
          "NTSC or PAL-M.";
    }
    return false;
  }

  const std::string escaped_source = EscapeForSingleQuotedShell(source);
  const std::string frame_limit_arg =
      max_frames > 0 ? (" -frames:v " + std::to_string(max_frames)) : "";
  const std::string command =
      "ffmpeg -v error -i '" + escaped_source + "' -an -sn -dn" +
      frame_limit_arg +
      " -pix_fmt yuv422p10le -vsync 0 -f rawvideo - 2>/dev/null";

  FILE* pipe = popen(command.c_str(), "r");
  if (pipe == nullptr) {
    if (error != nullptr) {
      *error = "Unable to run ffmpeg for progressive MKV decoding.";
    }
    return false;
  }

  std::vector<std::uint8_t> decoded_bytes;
  // Read in large blocks: the payload is ~1.4 MB per PAL frame, so an 8 KiB
  // chunk costs hundreds of appends per frame for no benefit.
  constexpr std::size_t kReadChunkBytes = std::size_t{256} * 1024;
  std::vector<std::uint8_t> chunk(kReadChunkBytes);
  while (true) {
    const std::size_t read_count =
        std::fread(chunk.data(), 1, chunk.size(), pipe);
    if (read_count > 0) {
      decoded_bytes.insert(decoded_bytes.end(), chunk.data(),
                           chunk.data() + read_count);
    }
    if (read_count < chunk.size()) {
      break;
    }
  }

  const int rc = pclose(pipe);
  if (rc != 0) {
    if (error != nullptr) {
      *error = "ffmpeg failed while decoding progressive MKV source.";
    }
    return false;
  }

  // yuv422p10le: one 16-bit luma sample per pixel plus two 16-bit
  // colour-difference samples per horizontal pixel pair.
  const int decoded_width = source_width;
  const std::size_t bytes_per_frame = static_cast<std::size_t>(decoded_width) *
                                      static_cast<std::size_t>(source_height) *
                                      2U * sizeof(std::uint16_t);
  if (bytes_per_frame == 0U || decoded_bytes.empty()) {
    if (error != nullptr) {
      *error =
          "Progressive MKV source does not contain decodable video frames.";
    }
    return false;
  }
  if ((decoded_bytes.size() % bytes_per_frame) != 0U) {
    if (error != nullptr) {
      *error =
          "Decoded MKV frame payload size is not aligned to yuv422p10le frame "
          "boundaries.";
    }
    return false;
  }

  const std::size_t frame_count = decoded_bytes.size() / bytes_per_frame;
  if (max_frames > 0 && frame_count > static_cast<std::size_t>(max_frames)) {
    if (error != nullptr) {
      *error =
          "Decoded MKV frame payload size is not aligned to frame boundaries.";
    }
    return false;
  }

  const std::size_t y_plane_size = static_cast<std::size_t>(decoded_width) *
                                   static_cast<std::size_t>(source_height);
  const std::size_t chroma_plane_size =
      static_cast<std::size_t>(decoded_width / 2) *
      static_cast<std::size_t>(source_height);

  out_frames->clear();
  out_frames->reserve(frame_count);

  const int source_x_offset = 0;
  for (std::size_t frame_index = 0; frame_index < frame_count; ++frame_index) {
    auto frame_image = std::make_shared<FrameSourceImage>();
    FrameSourceImage& frame = *frame_image;
    SetFrameGeometryForStandard(standard, &frame);
    frame.pixels.assign(static_cast<std::size_t>(frame.width) *
                            static_cast<std::size_t>(frame.height),
                        MakePixel(64, 512, 512));

    const std::size_t frame_offset = frame_index * bytes_per_frame;
    const std::uint8_t* frame_data = decoded_bytes.data() + frame_offset;
    const std::uint16_t* y_plane =
        reinterpret_cast<const std::uint16_t*>(frame_data);
    const std::uint16_t* cb_plane = y_plane + y_plane_size;
    const std::uint16_t* cr_plane = cb_plane + chroma_plane_size;

    for (int y = 0; y < source_height; ++y) {
      for (int x = 0; x < decoded_width; ++x) {
        const int dst_x = x + source_x_offset;
        const int dst_y = y;
        const std::size_t index = static_cast<std::size_t>(y) *
                                      static_cast<std::size_t>(decoded_width) +
                                  static_cast<std::size_t>(x);
        const std::size_t chroma_index =
            static_cast<std::size_t>(y) *
                static_cast<std::size_t>(decoded_width / 2) +
            static_cast<std::size_t>(x / 2);

        const std::uint16_t y_code =
            static_cast<std::uint16_t>(y_plane[index] & 0x03FFu);
        const std::uint16_t cb_code =
            static_cast<std::uint16_t>(cb_plane[chroma_index] & 0x03FFu);
        const std::uint16_t cr_code =
            static_cast<std::uint16_t>(cr_plane[chroma_index] & 0x03FFu);

        frame.pixels[static_cast<std::size_t>(dst_y) *
                         static_cast<std::size_t>(frame.width) +
                     static_cast<std::size_t>(dst_x)] =
            MakeRawPixel(y_code, cb_code, cr_code);
      }
    }

    out_frames->push_back(std::move(frame_image));
  }

  return true;
}

void SetFrameGeometryForStandard(Standard standard, FrameSourceImage* image) {
  if (Is625LineStandard(standard)) {
    image->width = kPalWidth;
    image->height = kPalHeight;
    image->active_x = kPalActiveX;
    image->active_y = kPalActiveY;
    image->active_width = kPalActiveWidth;
    image->active_height = kPalActiveHeight;
    return;
  }

  image->width = kNtscWidth;
  image->height = kNtscHeight;
  image->active_x = kNtscActiveX;
  image->active_y = kNtscActiveY;
  image->active_width = kNtscActiveWidth;
  image->active_height = kNtscActiveHeight;
}

YCbCr444Pixel ConvertRgbFloatToBt601(std::float_t r, std::float_t g,
                                     std::float_t b) {
  const double r_norm = static_cast<double>(r);
  const double g_norm = static_cast<double>(g);
  const double b_norm = static_cast<double>(b);

  const double y = (0.299 * r_norm) + (0.587 * g_norm) + (0.114 * b_norm);
  const double cb = (-0.168736 * r_norm) - (0.331264 * g_norm) + (0.5 * b_norm);
  const double cr = (0.5 * r_norm) - (0.418688 * g_norm) - (0.081312 * b_norm);

  const int y_code = static_cast<int>(std::lround(64.0 + (876.0 * y)));
  const int cb_code = static_cast<int>(std::lround(512.0 + (896.0 * cb)));
  const int cr_code = static_cast<int>(std::lround(512.0 + (896.0 * cr)));
  return MakeRawPixel(static_cast<std::uint16_t>(ClampCode(y_code, 0, 1023)),
                      static_cast<std::uint16_t>(ClampCode(cb_code, 0, 1023)),
                      static_cast<std::uint16_t>(ClampCode(cr_code, 0, 1023)));
}

bool ValidateExrMetadata(const Imf::Header& header, int width, int height,
                         Standard standard, std::string* error) {
  if (!Is625LineStandard(standard) && standard != Standard::kNtsc &&
      standard != Standard::kPalM) {
    if (error != nullptr) {
      *error = "Unsupported video standard for progressive EXR source.";
    }
    return false;
  }

  const IMATH_NAMESPACE::Box2i data_window = header.dataWindow();
  const IMATH_NAMESPACE::Box2i display_window = header.displayWindow();
  if (data_window.min.x != 0 || data_window.min.y != 0 ||
      data_window.max.x != width - 1 || data_window.max.y != height - 1) {
    if (error != nullptr) {
      *error = "Progressive EXR dataWindow must match full raster bounds.";
    }
    return false;
  }
  if (display_window.min.x != data_window.min.x ||
      display_window.min.y != data_window.min.y ||
      display_window.max.x != data_window.max.x ||
      display_window.max.y != data_window.max.y) {
    if (error != nullptr) {
      *error = "Progressive EXR displayWindow must match dataWindow.";
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

  const float expected_pixel_aspect =
      Is625LineStandard(standard) ? (128.0F / 117.0F) : (108.0F / 119.0F);
  if (std::abs(header.pixelAspectRatio() - expected_pixel_aspect) > 2.0e-3F) {
    if (error != nullptr) {
      *error =
          "Progressive EXR pixelAspectRatio metadata is outside supported "
          "tolerance.";
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
  if ((Is625LineStandard(standard) &&
       !(fps_value.n == 25 && fps_value.d == 1)) ||
      ((standard == Standard::kNtsc || standard == Standard::kPalM) &&
       !(fps_value.n == 30000 && fps_value.d == 1001))) {
    if (error != nullptr) {
      *error =
          "Progressive EXR framesPerSecond metadata does not match selected "
          "standard.";
    }
    return false;
  }

  return true;
}

bool LoadExrFrame(const std::string& source, Standard standard,
                  FrameSourceImage* out_image, std::string* error) {
  if (out_image == nullptr) {
    if (error != nullptr) {
      *error = "Frame source output image pointer must not be null.";
    }
    return false;
  }

  // Guard against a missing file before OpenEXR opens it: its C core writes an
  // "Unable to open file for read" line directly to stderr that never reaches
  // the logger. A cheap existence check keeps the failure logged and clean.
  std::error_code exists_ec;
  if (!std::filesystem::exists(source, exists_ec)) {
    if (error != nullptr) {
      *error = "Progressive EXR source not found: " + source;
    }
    return false;
  }

  try {
    Imf::InputFile input_file(source.c_str());
    const Imf::Header& header = input_file.header();
    const ImathNs::Box2i data_window = header.dataWindow();
    const int width = (data_window.max.x - data_window.min.x) + 1;
    const int height = (data_window.max.y - data_window.min.y) + 1;

    const bool is_625_line = Is625LineStandard(standard);
    const int expected_height = is_625_line ? kPalHeight : kNtscHeight;
    if (height != expected_height || width != 720) {
      if (error != nullptr) {
        *error =
            "Progressive EXR raster must be 720x576 for PAL/SECAM and 720x486 "
            "for "
            "NTSC or PAL-M.";
      }
      return false;
    }

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

    const Imf::PixelType channel_type = r_channel->type;
    if (channel_type != Imf::FLOAT) {
      if (error != nullptr) {
        *error = "Progressive EXR channels R/G/B must use 32-bit FLOAT type.";
      }
      return false;
    }

    if (!ValidateExrMetadata(header, width, height, standard, error)) {
      return false;
    }

    SetFrameGeometryForStandard(standard, out_image);
    out_image->pixels.assign(static_cast<std::size_t>(out_image->width) *
                                 static_cast<std::size_t>(out_image->height),
                             MakePixel(64, 512, 512));

    const int source_x_offset = 0;
    const std::ptrdiff_t pixel_offset =
        static_cast<std::ptrdiff_t>(data_window.min.x) +
        (static_cast<std::ptrdiff_t>(data_window.min.y) * width);
    Imf::FrameBuffer frame_buffer;

    if (channel_type == Imf::FLOAT) {
      std::vector<float> red(
          static_cast<std::size_t>(width) * static_cast<std::size_t>(height),
          0.0F);
      std::vector<float> green(
          static_cast<std::size_t>(width) * static_cast<std::size_t>(height),
          0.0F);
      std::vector<float> blue(
          static_cast<std::size_t>(width) * static_cast<std::size_t>(height),
          0.0F);

      frame_buffer.insert(
          "R", Imf::Slice(Imf::FLOAT,
                          reinterpret_cast<char*>(red.data() - pixel_offset),
                          sizeof(float),
                          static_cast<std::size_t>(sizeof(float) * width)));
      frame_buffer.insert(
          "G", Imf::Slice(Imf::FLOAT,
                          reinterpret_cast<char*>(green.data() - pixel_offset),
                          sizeof(float),
                          static_cast<std::size_t>(sizeof(float) * width)));
      frame_buffer.insert(
          "B", Imf::Slice(Imf::FLOAT,
                          reinterpret_cast<char*>(blue.data() - pixel_offset),
                          sizeof(float),
                          static_cast<std::size_t>(sizeof(float) * width)));

      input_file.setFrameBuffer(frame_buffer);
      input_file.readPixels(data_window.min.y, data_window.max.y);

      for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
          const std::size_t source_index =
              static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
              static_cast<std::size_t>(x);
          const int dst_x = x + source_x_offset;
          out_image->pixels[static_cast<std::size_t>(y) *
                                static_cast<std::size_t>(out_image->width) +
                            static_cast<std::size_t>(dst_x)] =
              ConvertRgbFloatToBt601(red[source_index], green[source_index],
                                     blue[source_index]);
        }
      }
      return true;
    }

    if (error != nullptr) {
      *error =
          "Progressive EXR decode path currently supports FLOAT channels only.";
    }
    return false;
  } catch (const std::exception& ex) {
    if (error != nullptr) {
      *error = std::string("Failed while decoding progressive EXR source: ") +
               ex.what();
    }
    return false;
  }
}

}  // namespace

const YCbCr444Pixel& FrameSourceImage::PixelAt(int x, int y) const {
  return pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                static_cast<std::size_t>(x)];
}

bool ProgressiveFrameSource::SupportsSection(const Section& section) const {
  if (section.type != "progressive" || section.source.empty()) {
    return false;
  }

  std::string source = section.source;
  std::transform(
      source.begin(), source.end(), source.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return source.size() >= 4 && (EndsWithLowercase(source, ".exr") ||
                                EndsWithLowercase(source, ".mkv"));
}

ProgressiveFrameSource::~ProgressiveFrameSource() {
  {
    const std::lock_guard<std::mutex> lock(prefetch_mutex_);
    prefetch_stopping_ = true;
    prefetch_queue_.clear();
  }
  prefetch_cv_.notify_all();
  if (prefetch_thread_.joinable()) {
    prefetch_thread_.join();
  }
}

void ProgressiveFrameSource::ClearCache() const {
  const std::lock_guard<std::mutex> lock(cache_mutex_);
  cached_sources_.clear();
}

ProgressiveFrameSource::DecodedSourcePtr
ProgressiveFrameSource::AcquireCacheEntry(const std::string& source,
                                          Standard standard) const {
  const std::lock_guard<std::mutex> lock(cache_mutex_);
  for (std::size_t i = 0; i < cached_sources_.size(); ++i) {
    if (cached_sources_[i]->source != source ||
        cached_sources_[i]->standard != standard) {
      continue;
    }
    if (i != 0) {
      std::rotate(cached_sources_.begin(),
                  cached_sources_.begin() + static_cast<std::ptrdiff_t>(i),
                  cached_sources_.begin() + static_cast<std::ptrdiff_t>(i) + 1);
    }
    return cached_sources_.front();
  }

  auto entry = std::make_shared<DecodedSource>();
  entry->source = source;
  entry->standard = standard;
  // Evicting only drops the table's reference: a thread that is decoding from
  // or reading the evicted entry keeps it alive through its own reference.
  if (cached_sources_.size() >= kMaxCachedSources) {
    cached_sources_.resize(kMaxCachedSources - 1U);
  }
  cached_sources_.insert(cached_sources_.begin(), entry);
  return entry;
}

void ProgressiveFrameSource::PrefetchSection(const Section& section,
                                             int frame_index,
                                             Standard standard) const {
  if (!SupportsSection(section)) {
    return;
  }

  {
    const std::lock_guard<std::mutex> lock(prefetch_mutex_);
    if (prefetch_stopping_) {
      return;
    }
    for (const PrefetchRequest& queued : prefetch_queue_) {
      if (queued.section.source == section.source &&
          queued.standard == standard) {
        return;
      }
    }
    prefetch_queue_.push_back(PrefetchRequest{section, frame_index, standard});
    if (!prefetch_thread_.joinable()) {
      prefetch_thread_ = std::thread([this]() { PrefetchLoop(); });
    }
  }
  prefetch_cv_.notify_one();
}

void ProgressiveFrameSource::PrefetchLoop() const {
  for (;;) {
    PrefetchRequest request;
    {
      std::unique_lock<std::mutex> lock(prefetch_mutex_);
      prefetch_cv_.wait(lock, [this]() {
        return prefetch_stopping_ || !prefetch_queue_.empty();
      });
      if (prefetch_stopping_) {
        return;
      }
      request = prefetch_queue_.front();
      prefetch_queue_.erase(prefetch_queue_.begin());
    }
    DecodeSectionIntoCache(request.section, request.frame_index,
                           request.standard);
  }
}

void ProgressiveFrameSource::DecodeSectionIntoCache(const Section& section,
                                                    int frame_index,
                                                    Standard standard) const {
  // A prefetch is an optimisation, so failures are dropped here; the frame
  // request that follows repeats the decode and reports the error.
  std::shared_ptr<const FrameSourceImage> image;
  std::string ignored_error;
  GenerateFrame(section, frame_index, standard, &image, &ignored_error);
}

bool ProgressiveFrameSource::ResolveFrameCount(const Section& section,
                                               Standard standard,
                                               int* out_frame_count,
                                               std::string* error) const {
  if (out_frame_count == nullptr) {
    if (error != nullptr) {
      *error = "Progressive frame count output pointer must not be null.";
    }
    return false;
  }

  if (!SupportsSection(section)) {
    if (error != nullptr) {
      *error = "Progressive section source family is not supported.";
    }
    return false;
  }

  std::string source = section.source;
  std::transform(
      source.begin(), source.end(), source.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

  if (EndsWithLowercase(source, ".exr")) {
    *out_frame_count = 1;
    return true;
  }

  if (EndsWithLowercase(source, ".mkv")) {
    const DecodedSourcePtr entry = AcquireCacheEntry(section.source, standard);
    const std::lock_guard<std::mutex> decode_lock(entry->decode_mutex);
    if (!EnsureMkvDecoded(entry.get(), section, standard, 0,
                          /*require_complete=*/true, error)) {
      return false;
    }

    *out_frame_count = static_cast<int>(entry->frames.size());
    return true;
  }

  if (error != nullptr) {
    *error =
        "Progressive source frame count probing is not yet implemented for "
        "this source family.";
  }
  return false;
}

// static
bool ProgressiveFrameSource::EnsureMkvDecoded(
    DecodedSource* entry, const Section& section, Standard standard,
    int min_required_frames, bool require_complete, std::string* error) {
  if (min_required_frames < 0) {
    min_required_frames = 0;
  }

  const bool has_required_frames =
      static_cast<int>(entry->frames.size()) >= min_required_frames;
  // A complete decode satisfies every request; a partial one satisfies any
  // request that fits inside the prefix already decoded.
  if (!entry->frames.empty() &&
      (entry->is_complete || (!require_complete && has_required_frames))) {
    return true;
  }

  std::vector<std::shared_ptr<const FrameSourceImage>> frames;
  std::string decode_error;
  const int decode_limit = require_complete ? 0 : min_required_frames;
  if (!DecodeMkvFrames(section.source, standard, decode_limit, &frames,
                       &decode_error)) {
    if (error != nullptr) {
      *error = decode_error.empty() ? "Failed to decode progressive MKV source."
                                    : decode_error;
    }
    return false;
  }

  // A fresh decode replaces any stale partial one: frames already handed out
  // stay valid through the shared references their holders keep.
  entry->frames = std::move(frames);
  entry->is_complete = require_complete;
  return true;
}

bool ProgressiveFrameSource::GenerateFrame(
    const Section& section, int frame_index, Standard standard,
    std::shared_ptr<const FrameSourceImage>* out_image,
    std::string* error) const {
  if (!SupportsSection(section)) {
    if (error != nullptr) {
      *error = "Progressive section source family is not supported.";
    }
    return false;
  }

  if (out_image == nullptr) {
    if (error != nullptr) {
      *error = "Frame source output image pointer must not be null.";
    }
    return false;
  }

  std::string source = section.source;
  std::transform(
      source.begin(), source.end(), source.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

  if (EndsWithLowercase(source, ".exr")) {
    const DecodedSourcePtr entry = AcquireCacheEntry(section.source, standard);
    const std::lock_guard<std::mutex> decode_lock(entry->decode_mutex);
    if (entry->frames.empty()) {
      auto decoded = std::make_shared<FrameSourceImage>();
      std::string decode_error;
      if (!LoadExrFrame(section.source, standard, decoded.get(),
                        &decode_error)) {
        if (error != nullptr) {
          *error = decode_error.empty()
                       ? "Failed to decode progressive EXR source."
                       : decode_error;
        }
        return false;
      }
      entry->is_complete = true;
      entry->frames.push_back(std::move(decoded));
    }

    *out_image = entry->frames.front();
    return true;
  }

  if (EndsWithLowercase(source, ".mkv")) {
    if (frame_index < 0) {
      if (error != nullptr) {
        *error = "Progressive frame index must be non-negative.";
      }
      return false;
    }

    int required_frames = frame_index + 1;
    if (!section.duration_frames_all && section.duration_frames > 0) {
      required_frames = section.start_frame + section.duration_frames;
    }

    const DecodedSourcePtr entry = AcquireCacheEntry(section.source, standard);
    const std::lock_guard<std::mutex> decode_lock(entry->decode_mutex);
    if (!EnsureMkvDecoded(entry.get(), section, standard, required_frames,
                          section.duration_frames_all, error)) {
      return false;
    }

    if (frame_index >= static_cast<int>(entry->frames.size())) {
      if (error != nullptr) {
        *error =
            "Requested frame index exceeds decoded progressive MKV source "
            "length.";
      }
      return false;
    }

    *out_image = entry->frames[static_cast<std::size_t>(frame_index)];
    return true;
  }

  if (error != nullptr) {
    *error =
        "Progressive source decoding is not yet implemented for this source "
        "family.";
  }
  return false;
}

}  // namespace videosynth