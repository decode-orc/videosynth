/*
 * File:        source_colour_conversion.cpp
 * Module:      source_colour_conversion
 * Purpose:     Plans the ffmpeg filter chain that maps a probed video source
 *              to BT.601 10-bit 4:2:2 studio-swing YCbCr.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "videosynth/source_colour_conversion.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>

namespace videosynth {
namespace {

// The decode target: ITU-R BT.601-5 4:2:2 coding at 10 bits.
constexpr char kTargetPixelFormat[] = "yuv422p10le";

// swscale name of the BT.601 matrix, the only matrix the synthesiser accepts.
constexpr char kBt601Matrix[] = "bt601";

// Rounding and chroma flags for every conversion: accurate_rnd for correctly
// rounded codes, full_chroma_int so RGB input is converted at full chroma
// resolution before subsampling, and bitexact so the output does not depend on
// which SIMD path the host CPU selects.
constexpr char kScaleFlags[] = "accurate_rnd+full_chroma_int+bitexact";

std::string Lowercase(std::string value) {
  std::transform(
      value.begin(), value.end(), value.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

bool StartsWith(const std::string& value, const char* prefix) {
  return value.rfind(prefix, 0) == 0;
}

bool IsRgbSource(const std::string& pixel_format,
                 const std::string& color_space) {
  if (color_space == "gbr" || color_space == "rgb") {
    return true;
  }
  static constexpr std::array<const char*, 10> kRgbPrefixes = {
      "rgb",  "bgr",  "gbr",  "argb",  "abgr",
      "0rgb", "0bgr", "pal8", "x2rgb", "x2bgr"};
  return std::any_of(
      kRgbPrefixes.begin(), kRgbPrefixes.end(),
      [&](const char* prefix) { return StartsWith(pixel_format, prefix); });
}

// The studio-range twin of a YCbCr pixel format (yuvj420p -> yuv420p).
std::string StudioRangeFormat(const std::string& pixel_format) {
  if (StartsWith(pixel_format, "yuvj")) {
    return "yuv" + pixel_format.substr(4);
  }
  return pixel_format;
}

// Appends the depth change to the decode target as its own bit-exact scaler
// stage, unless the intermediate already is the target.
std::string WidenToTarget(const std::string& intermediate_format) {
  if (intermediate_format == kTargetPixelFormat) {
    return "";
  }
  return std::string(",scale=flags=") + kScaleFlags +
         ",format=" + kTargetPixelFormat;
}

bool IsFullRange(const std::string& pixel_format,
                 const std::string& color_range) {
  // The deprecated yuvj* formats imply full range whatever the stream tag.
  return color_range == "pc" || color_range == "jpeg" ||
         color_range == "full" || StartsWith(pixel_format, "yuvj");
}

// Maps an ffprobe color_space name to the swscale matrix name, or "" when the
// matrix is BT.601 or unknown (assumed BT.601, the SD convention).
std::string SourceMatrixName(const std::string& color_space) {
  if (color_space == "bt709") {
    return "bt709";
  }
  if (color_space == "fcc") {
    return "fcc";
  }
  if (color_space == "smpte240m") {
    return "smpte240m";
  }
  if (StartsWith(color_space, "bt2020")) {
    return "bt2020";
  }
  return "";
}

std::string MatrixDisplayName(const std::string& matrix) {
  if (matrix == "bt709") {
    return "BT.709";
  }
  if (matrix == "bt2020") {
    return "BT.2020";
  }
  if (matrix == "smpte240m") {
    return "SMPTE 240M";
  }
  if (matrix == "fcc") {
    return "FCC";
  }
  return matrix;
}

bool IsKnown(const std::string& tag) {
  return !tag.empty() && tag != "unknown" && tag != "unspecified" &&
         tag != "reserved";
}

void NoteUnconvertedMetadata(const ProgressiveFrameSourceProfile& profile,
                             Standard standard, SourceColourConversion* plan) {
  const std::string primaries = Lowercase(profile.color_primaries);
  const std::string transfer = Lowercase(profile.color_transfer);

  // ITU-R BT.1700: 625-line systems use EBU (bt470bg) primaries, 525-line
  // NTSC uses SMPTE C (smpte170m); PAL-M material carries either.
  bool primaries_match = true;
  if (IsKnown(primaries)) {
    if (Is625LineStandard(standard)) {
      primaries_match = primaries == "bt470bg";
    } else if (standard == Standard::kNtsc) {
      primaries_match = primaries == "smpte170m";
    } else {
      primaries_match = primaries == "bt470bg" || primaries == "smpte170m";
    }
  }
  if (!primaries_match) {
    plan->unconverted_notes.push_back("colour primaries '" + primaries +
                                      "' are used unconverted");
  }

  if (IsKnown(transfer) && transfer != "bt709" && transfer != "bt470bg" &&
      transfer != "bt470m" && transfer != "smpte170m" && transfer != "bt601") {
    plan->unconverted_notes.push_back("transfer characteristic '" + transfer +
                                      "' is used unconverted");
  }
}

}  // namespace

SourceColourConversion PlanSourceColourConversion(
    const ProgressiveFrameSourceProfile& profile, Standard standard) {
  SourceColourConversion plan;
  NoteUnconvertedMetadata(profile, standard, &plan);

  const std::string pixel_format = Lowercase(profile.pixel_format);
  const std::string color_space = Lowercase(profile.color_space);
  const std::string color_range = Lowercase(profile.color_range);

  if (IsRgbSource(pixel_format, color_space)) {
    // RGB is always full range; swscale ignores in_range for RGB input.
    plan.conversions.push_back("RGB (" + pixel_format +
                               ") -> BT.601 studio-swing YCbCr");
    plan.filter_chain = std::string("scale=out_range=tv:out_color_matrix=") +
                        kBt601Matrix + ":flags=" + kScaleFlags +
                        ",format=" + kTargetPixelFormat;
    return plan;
  }

  const bool full_range = IsFullRange(pixel_format, color_range);
  const std::string source_matrix = SourceMatrixName(color_space);
  const bool format_matches = pixel_format == kTargetPixelFormat;

  if (!full_range && source_matrix.empty() && format_matches) {
    return plan;
  }

  if (full_range) {
    plan.conversions.push_back("full range -> studio swing");
  }
  if (!source_matrix.empty()) {
    plan.conversions.push_back(MatrixDisplayName(source_matrix) +
                               " matrix -> BT.601");
  }
  if (!format_matches) {
    plan.conversions.push_back((pixel_format.empty()
                                    ? std::string("unknown pixel format")
                                    : pixel_format) +
                               " -> " + kTargetPixelFormat);
  }

  const std::string matrix_and_range =
      std::string("scale=in_range=") + (full_range ? "pc" : "tv") +
      ":out_range=tv:in_color_matrix=" +
      (source_matrix.empty() ? std::string(kBt601Matrix) : source_matrix) +
      ":out_color_matrix=" + kBt601Matrix + ":flags=" + kScaleFlags;
  if (full_range && !pixel_format.empty()) {
    // swscale widens 8-bit codes by shifting (255 -> 1020) before scaling the
    // range against a 10-bit full scale of 1023, which lands full-range white
    // on 937 instead of 940. Converting the range at the source's own depth
    // and widening afterwards keeps both ends exact (0 -> 64, 255 -> 940).
    const std::string intermediate = StudioRangeFormat(pixel_format);
    plan.filter_chain = matrix_and_range + ",format=" + intermediate +
                        WidenToTarget(intermediate);
    return plan;
  }
  plan.filter_chain = matrix_and_range + ",format=" + kTargetPixelFormat;
  return plan;
}

}  // namespace videosynth
