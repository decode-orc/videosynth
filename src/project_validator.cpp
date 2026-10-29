/*
 * File:        project_validator.cpp
 * Module:      project_validator
 * Purpose:     Validates project constraints for supported generation profiles.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "videosynth/project_validator.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "videosynth/biphase_types.h"
#include "videosynth/cav_code_generator.h"
#include "videosynth/dropout_scale.h"
#include "videosynth/osd_token_resolver.h"
#include "videosynth/path_resolution.h"
#include "videosynth/progressive_frame_source.h"
#include "videosynth/source_colour_conversion.h"
#include "videosynth/source_frame_range.h"
#include "videosynth/timing_constants.h"
#include "videosynth/vits_definition_provider.h"

namespace {

// Noise parameter bounds from noise-injection-design.md.
// Lower: 20 dB — noise amplitude approaches sync pulse depth; sync separators
// fail. Upper: 61 dB — injected noise smaller than 10-bit quantisation floor
// (~0.085 IRE).
constexpr double kNoiseDbMin = 20.0;
constexpr double kNoiseDbMax = 61.0;

// Minimum CAV laserdisc section durations derived from IEC track-pitch limits.
// IEC 60856/60857: lead-in ≥ 1.5 mm, lead-out ≥ 2 mm at nominal 1.6 µm pitch.
constexpr int kLaserdiscLeadInMinFrames = 938;    // ceil(1500 µm / 1.6 µm)
constexpr int kLaserdiscLeadOutMinFrames = 1250;  // ceil(2000 µm / 1.6 µm)

// Maximum picture/timecode values per IEC 60856/60857.
constexpr int kPalMaxPictureNumber = 99999;
constexpr int kNtscMaxPictureNumber = 79999;
constexpr int kMaxChapterNumber = 79;

// LaserDisc digital audio (EFM) track limits.
// IEC 60856:1986 Amd 2, 13.5.3.3 / IEC 60857:1986 Amd 2, 13.6.3.3: the track
// number of a digital audio disc is limited to 79.
constexpr int kMaxEfmTracks = 79;
// IEC 60908-1999, 17.5.1: a track shall be at least 4 s long.
constexpr double kMinEfmTrackSeconds = 4.0;
// IEC 60908-1999, 17.5.1: the pause preceding the first track is 2 s to 3 s,
// so the first programme-area section must carry the pause plus a full track.
constexpr double kEfmFirstTrackPauseSeconds = 2.0;
constexpr uint32_t kMaxUsersCodeX1Nibble = 7;

std::string Lowercase(std::string value) {
  std::transform(
      value.begin(), value.end(), value.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

// Parses a decimal or "0x"-prefixed hex string into a uint32_t.
bool ParseHexValue(const std::string& hex_str, uint32_t* out_value) {
  if (hex_str.empty() || out_value == nullptr) {
    return false;
  }
  try {
    std::size_t pos = 0;
    const auto val = std::stoul(hex_str, &pos, 0);
    if (pos != hex_str.size()) {
      return false;
    }
    *out_value = static_cast<uint32_t>(val);
    return true;
  } catch (...) {
    return false;
  }
}

bool EndsWith(const std::string& value, const std::string& suffix) {
  if (suffix.size() > value.size()) {
    return false;
  }
  return value.compare(value.size() - suffix.size(), suffix.size(), suffix) ==
         0;
}

// Rejects a path whose leading "{name}" logical-asset-root token is not a
// known root, so token typos fail fast with a clear message. `context`
// prefixes the diagnostic (e.g. "Progressive section validation error") and
// `field` names the offending setting (e.g. "source").
bool ValidateAssetRootToken(const std::string& path, const std::string& context,
                            const std::string& field, std::string* error) {
  if (path.empty() || path.front() != '{') {
    return true;
  }
  const std::size_t close = path.find('}');
  if (close == std::string::npos) {
    if (error != nullptr) {
      *error = context + ": malformed asset root token in " + field +
               " (missing '}').";
    }
    return false;
  }
  const std::string name = path.substr(1, close - 1);
  if (!videosynth::IsBuiltinRootName(name)) {
    if (error != nullptr) {
      *error = context + ": unknown asset root '" + name + "' in " + field +
               ". Known roots: bundled, user, project, output.";
    }
    return false;
  }
  return true;
}

bool ValidateProgressiveSourceFamily(const videosynth::Section& section,
                                     std::string* error) {
  if (videosynth::IsProgressiveStillSource(section.source) ||
      videosynth::IsProgressiveVideoSource(section.source)) {
    return true;
  }

  if (error != nullptr) {
    *error =
        "Unsupported progressive source family. Supported source families are "
        "EXR stills and MKV or MP4 video.";
  }
  return false;
}

bool FrameRateMatchesStandard(double frame_rate_hz,
                              videosynth::Standard standard) {
  if (frame_rate_hz <= 0.0) {
    return true;
  }

  if (videosynth::Is625LineStandard(standard)) {
    return std::abs(frame_rate_hz - 25.0) <= 1.0e-3;
  }
  if (standard == videosynth::Standard::kNtsc ||
      standard == videosynth::Standard::kPalM) {
    const double ntsc_rate = 30000.0 / 1001.0;
    return std::abs(frame_rate_hz - ntsc_rate) <= 1.0e-3;
  }
  return false;
}

bool RasterMatchesStandard(int width, int height,
                           videosynth::Standard standard) {
  if (width <= 0 || height <= 0) {
    return true;
  }

  if (videosynth::Is625LineStandard(standard)) {
    return width == 720 && height == 576;
  }
  if (standard == videosynth::Standard::kNtsc ||
      standard == videosynth::Standard::kPalM) {
    return width == 720 && height == 486;
  }
  return false;
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

bool SampleAspectMatchesStandard(double sample_aspect_ratio,
                                 videosynth::Standard standard) {
  if (sample_aspect_ratio <= 0.0) {
    return true;
  }

  if (videosynth::Is625LineStandard(standard)) {
    return std::abs(sample_aspect_ratio - (128.0 / 117.0)) <= 2.0e-3;
  }
  if (standard == videosynth::Standard::kNtsc ||
      standard == videosynth::Standard::kPalM) {
    return std::abs(sample_aspect_ratio - (108.0 / 119.0)) <= 2.0e-3;
  }
  return false;
}

bool ValidateProfileBySourceFamily(
    const videosynth::Section& section, videosynth::Standard standard,
    const videosynth::ProgressiveFrameSourceProfile& profile,
    std::string* error) {
  const std::string source = Lowercase(section.source);
  const std::string container = Lowercase(profile.container);
  const std::string codec = Lowercase(profile.codec);
  const std::string pixel_format = Lowercase(profile.pixel_format);

  if (EndsWith(source, ".exr")) {
    if (container != "exr") {
      if (error != nullptr) {
        *error = "Progressive EXR sections require an EXR container profile.";
      }
      return false;
    }
    if (codec != "openexr") {
      if (error != nullptr) {
        *error = "Progressive EXR sections only support OpenEXR codec profile.";
      }
      return false;
    }
    if (pixel_format != "rgbf") {
      if (error != nullptr) {
        *error = "Progressive EXR sections require RGB FLOAT channel profile.";
      }
      return false;
    }
    if (profile.bit_depth != 32) {
      if (error != nullptr) {
        *error = "Progressive EXR sections only support 32-bit FLOAT channels.";
      }
      return false;
    }
    return true;
  }

  return true;
}

// Relative tolerance between a video's average and base frame rates before it
// is treated as variable-rate. Container timestamp rounding (1 ms in
// Matroska) keeps a constant-rate clip well inside this.
constexpr double kVariableFrameRateTolerance = 0.002;

bool ContainerMatchesVideoExtension(const std::string& source,
                                    const std::string& container) {
  if (EndsWith(source, ".mkv")) {
    return ContainsCsvToken(container, "matroska");
  }
  if (EndsWith(source, ".mp4")) {
    return ContainsCsvToken(container, "mp4") ||
           ContainsCsvToken(container, "mov");
  }
  return false;
}

// Field dominance the synthesiser assumes when it splits a frame into fields:
// top field first for 625-line systems, bottom field first for 525-line
// systems (the tags the bundled BT.601 clips carry).
bool FieldOrderOpposesStandard(const std::string& field_order,
                               videosynth::Standard standard) {
  if (videosynth::Is625LineStandard(standard)) {
    return field_order == "bt" || field_order == "bb";
  }
  return field_order == "tb" || field_order == "tt";
}

// Checks the section's start_frame/end_frame and duration combination, which
// needs no probe. Stills have one frame and no natural length; a video range
// with an explicit end already sets the duration.
bool ValidateSourceFrameRange(const videosynth::Section& section,
                              videosynth::ValidationResult* result) {
  const auto fail = [result](const std::string& message) {
    result->is_valid = false;
    result->errors.push_back("Progressive section validation error: " +
                             message);
    return false;
  };

  if (section.start_frame < 0) {
    return fail("start_frame must be >= 0.");
  }
  if (videosynth::IsProgressiveStillSource(section.source)) {
    if (section.start_frame != 0 || section.end_frame_specified) {
      return fail(
          "start_frame and end_frame select frames from a video source; an "
          "EXR still has only one frame.");
    }
    if (section.duration_frames_all) {
      return fail(
          "an EXR still needs an integer duration_frames; a still has no "
          "length of its own, so 'all' (or an omitted duration_frames) does "
          "not apply.");
    }
    return true;
  }
  if (section.end_frame_specified) {
    if (section.end_frame < section.start_frame) {
      return fail("end_frame must be >= start_frame.");
    }
    if (!section.duration_frames_all) {
      return fail(
          "end_frame cannot be combined with an integer duration_frames; the "
          "range already sets the duration. Omit duration_frames or set it to "
          "'all'.");
    }
  }
  return true;
}

// Checks the selected range against the probed video length. A frame count
// of zero (unknown) is left to the decoder.
bool ValidateSourceFrameRangeBounds(const videosynth::Section& section,
                                    int frame_count,
                                    videosynth::ValidationResult* result) {
  if (frame_count <= 0) {
    return true;
  }
  const auto fail = [result, frame_count](const std::string& key, int value) {
    result->is_valid = false;
    result->errors.push_back(
        "Progressive section validation error: " + key + " " +
        std::to_string(value) + " is beyond the end of the source (" +
        std::to_string(frame_count) + " frames, last frame " +
        std::to_string(frame_count - 1) + ").");
    return false;
  };
  if (section.start_frame >= frame_count) {
    return fail("start_frame", section.start_frame);
  }
  if (section.end_frame_specified && section.end_frame >= frame_count) {
    return fail("end_frame", section.end_frame);
  }
  return true;
}

// Checks a probed MKV/MP4 profile. Only raster and rate are hard requirements
// (checked by the caller); range, matrix, chroma format and bit depth are
// normalised at decode, and field order and sample aspect only warn.
bool ValidateVideoProfile(
    const videosynth::Section& section, videosynth::Standard standard,
    const videosynth::ProgressiveFrameSourceProfile& profile,
    videosynth::ValidationResult* result, videosynth::ILogger* logger) {
  const std::string source = Lowercase(section.source);
  const std::string container = Lowercase(profile.container);
  const std::string context = "Progressive section '" + section.name + "'";

  if (!ContainerMatchesVideoExtension(source, container)) {
    result->errors.push_back(
        "Progressive section validation error: source container '" +
        profile.container +
        "' does not match its file extension (.mkv must be Matroska, .mp4 "
        "must be MP4).");
    return false;
  }

  if (profile.frame_rate_hz > 0.0 && profile.average_frame_rate_hz > 0.0 &&
      std::abs(profile.average_frame_rate_hz - profile.frame_rate_hz) >
          kVariableFrameRateTolerance * profile.frame_rate_hz) {
    result->errors.push_back(
        "Progressive section validation error: source has a variable frame "
        "rate; convert it to a constant frame rate first.");
    return false;
  }

  if (profile.crop_left != 0 || profile.crop_right != 0 ||
      profile.crop_top != 0 || profile.crop_bottom != 0) {
    result->errors.push_back(
        "Progressive section validation error: source must not include stream "
        "crop metadata.");
    return false;
  }

  if (FieldOrderOpposesStandard(profile.field_order, standard)) {
    result->warnings.push_back(
        context + ": source is tagged field order '" + profile.field_order +
        "', opposite to the output's " +
        (videosynth::Is625LineStandard(standard) ? "top" : "bottom") +
        "-field-first dominance; interlaced motion may play out of order.");
  }

  if (!SampleAspectMatchesStandard(profile.sample_aspect_ratio, standard)) {
    result->warnings.push_back(
        context +
        ": source sample aspect ratio differs from BT.601 for this standard; "
        "the picture is used unscaled.");
  }

  if (logger != nullptr) {
    const videosynth::SourceColourConversion plan =
        videosynth::PlanSourceColourConversion(profile, standard);
    if (!plan.IsPassThrough()) {
      std::string summary;
      for (const std::string& conversion : plan.conversions) {
        summary += (summary.empty() ? "" : ", ") + conversion;
      }
      logger->Info(context + ": colour conversion applied: " + summary + ".");
    }
    for (const std::string& note : plan.unconverted_notes) {
      logger->Debug(context + ": " + note + ".");
    }
  }
  return true;
}

void ValidateDeferredLaserdiscPresetFlags(
    const videosynth::Project& project, videosynth::ValidationResult* result) {
  if (result == nullptr) {
    return;
  }

  if (project.cvbs_presets.pal_laserdisc_pilot_burst &&
      project.cvbs_presets.video_standard_preset !=
          videosynth::Standard::kPal) {
    result->is_valid = false;
    result->errors.push_back(
        "Project configuration error: pal_laserdisc_pilot_burst can only be "
        "enabled for PAL projects.");
    return;
  }

  if (project.cvbs_presets.pal_laserdisc_pilot_burst &&
      !videosynth::IsSubSyncCapableSampleEncodingPreset(
          project.cvbs_presets.sample_encoding_preset)) {
    result->warnings.push_back(
        "pal_laserdisc_pilot_burst warning: preset '" +
        project.cvbs_presets.sample_encoding_preset +
        "' clips sub-sync excursions below -300 mV; the pilot burst trough "
        "reaches -600 mV. Use CVBS_S16_4FSC or RAW_S16_28M/RAW_S16_40M to "
        "preserve the full burst waveform.");
  }

  if (project.cvbs_presets.ntsc_laserdisc_vbi_burst &&
      project.cvbs_presets.video_standard_preset !=
          videosynth::Standard::kNtsc) {
    result->is_valid = false;
    result->errors.push_back(
        "Project configuration error: ntsc_laserdisc_vbi_burst can only be "
        "enabled for NTSC projects.");
    return;
  }

  if (project.cvbs_presets.ntsc_laserdisc_vbi_burst) {
    result->is_valid = false;
    result->errors.push_back(
        "Project configuration error: ntsc_laserdisc_vbi_burst is parsed but "
        "not "
        "implemented in the current runtime.");
  }
}

void ValidateDeferredLineInjectionSupport(
    const videosynth::Section& section, videosynth::ValidationResult* result) {
  if (result == nullptr) {
    return;
  }

  for (const videosynth::Section::LineInjection& injection :
       section.line_injections) {
    const std::string type = Lowercase(injection.type);
    if (type == "vits" || type == "laserdisc") {
      continue;
    }

    result->is_valid = false;
    result->errors.push_back(
        "Project configuration error: line injection type '" + injection.type +
        "' is not implemented in the current runtime.");
    return;
  }
}

bool IsValidFrameLineForStandard(int line_1based,
                                 videosynth::Standard standard) {
  if (videosynth::Is625LineStandard(standard)) {
    return line_1based >= 1 && line_1based <= 625;
  }
  if (standard == videosynth::Standard::kNtsc ||
      standard == videosynth::Standard::kPalM) {
    return line_1based >= 1 && line_1based <= 525;
  }
  return false;
}

bool IsLaserdiscReservedLine(int line_1based, videosynth::Standard standard) {
  if (standard == videosynth::Standard::kPal) {
    return (line_1based >= 6 && line_1based <= 18) ||
           (line_1based >= 319 && line_1based <= 331);
  }
  if (standard == videosynth::Standard::kNtsc ||
      standard == videosynth::Standard::kPalM) {
    return (line_1based >= 10 && line_1based <= 18) ||
           (line_1based >= 273 && line_1based <= 281);
  }
  return false;
}

// True when the section carries at least one laserdisc code injection.
bool SectionHasLaserdiscInjection(const videosynth::Section& section) {
  for (const videosynth::Section::LineInjection& injection :
       section.line_injections) {
    if (Lowercase(injection.type) == "laserdisc") {
      return true;
    }
  }
  return false;
}

// Validates that each laserdisc code_type in the section is valid for the
// project-wide disc_type (CAV/CLV). disc_type itself is validated once at the
// project level (ValidateProjectLineInjections); this skips when it is not a
// recognised value so the error is reported only once.
bool ValidateLaserdiscInjectionStructure(const videosynth::Section& section,
                                         const std::string& project_disc_type,
                                         videosynth::ValidationResult* result) {
  if (result == nullptr) {
    return false;
  }

  const videosynth::DiscType disc_type =
      videosynth::DiscTypeFromString(project_disc_type);
  if (disc_type == videosynth::DiscType::kUnknown) {
    return true;
  }

  for (const videosynth::Section::LineInjection& injection :
       section.line_injections) {
    if (Lowercase(injection.type) != "laserdisc") {
      continue;
    }

    for (const videosynth::Section::LineInjectionCode& code : injection.codes) {
      if (!videosynth::IsKnownLaserdiscCodeType(code.code_type)) {
        result->is_valid = false;
        result->errors.push_back(
            "Laserdisc injection validation error: code_type '" +
            code.code_type + "' is not a recognised laserdisc code type.");
        return false;
      }

      if (disc_type == videosynth::DiscType::kCAV &&
          !videosynth::IsValidCavCodeType(code.code_type)) {
        result->is_valid = false;
        result->errors.push_back(
            "Laserdisc injection validation error: code_type '" +
            code.code_type + "' is not valid for CAV discs.");
        return false;
      }

      if (disc_type == videosynth::DiscType::kCLV &&
          !videosynth::IsValidClvCodeType(code.code_type)) {
        result->is_valid = false;
        result->errors.push_back(
            "Laserdisc injection validation error: code_type '" +
            code.code_type + "' is not valid for CLV discs.");
        return false;
      }
    }
  }

  return true;
}

// Validates IEC section-type compatibility, standard restrictions, minimum
// durations, and code-parameter value ranges for laserdisc injections against
// the project-wide disc_type. Skips when disc_type is not a recognised value
// (already reported at the project level).
bool ValidateLaserdiscSectionTypeAndCodes(
    const videosynth::Section& section, videosynth::Standard standard,
    const std::string& project_disc_type,
    videosynth::ValidationResult* result) {
  if (result == nullptr) {
    return false;
  }

  const videosynth::DiscType disc_type =
      videosynth::DiscTypeFromString(project_disc_type);
  if (disc_type == videosynth::DiscType::kUnknown) {
    return true;
  }

  for (const videosynth::Section::LineInjection& injection :
       section.line_injections) {
    if (Lowercase(injection.type) != "laserdisc") {
      continue;
    }

    // 5.4: section_type must be set for laserdisc sections.
    if (section.section_type == videosynth::SectionType::kUnknown) {
      result->is_valid = false;
      result->errors.push_back(
          "Laserdisc injection validation error: section_type must be set to "
          "'lead_in', 'programme_area', or 'lead_out' when disc_type is "
          "specified.");
      return false;
    }

    // 5.9/5.10: Minimum section duration for CAV discs.
    // CLV track density varies, so frame-based minimum is not validated.
    if (disc_type == videosynth::DiscType::kCAV &&
        !section.duration_frames_all) {
      if (section.section_type == videosynth::SectionType::kLeadIn &&
          section.duration_frames < kLaserdiscLeadInMinFrames) {
        result->warnings.push_back(
            "Laserdisc injection warning: CAV lead_in section has " +
            std::to_string(section.duration_frames) +
            " frames; IEC 1.5 mm minimum at 1.6 um track pitch requires " +
            std::to_string(kLaserdiscLeadInMinFrames) + ".");
      }
      if (section.section_type == videosynth::SectionType::kLeadOut &&
          section.duration_frames < kLaserdiscLeadOutMinFrames) {
        result->warnings.push_back(
            "Laserdisc injection warning: CAV lead_out section has " +
            std::to_string(section.duration_frames) +
            " frames; IEC 2 mm minimum at 1.6 um track pitch requires " +
            std::to_string(kLaserdiscLeadOutMinFrames) + ".");
      }
    }

    // 5.4/5.5: Validate each code type.
    for (const videosynth::Section::LineInjectionCode& code : injection.codes) {
      // Section-type compatibility (IEC Appendix D matrix).
      if (!videosynth::IsCodeTypeValidForSectionType(code.code_type,
                                                     section.section_type)) {
        result->is_valid = false;
        result->errors.push_back(
            "Laserdisc injection validation error: code_type '" +
            code.code_type + "' is not allowed in '" +
            videosynth::SectionTypeToString(section.section_type) +
            "' sections.");
        return false;
      }

      // Standard restriction: FM codes require System M (NTSC or PAL-M).
      if (videosynth::IsSystemMOnlyCodeType(code.code_type) &&
          standard != videosynth::Standard::kNtsc &&
          standard != videosynth::Standard::kPalM) {
        result->is_valid = false;
        result->errors.push_back(
            "Laserdisc injection validation error: code_type '" +
            code.code_type + "' is only valid for NTSC or PAL-M projects.");
        return false;
      }

      // 5.5: picture_number value range (IEC 60856/60857).
      if (code.code_type == "picture_number" && code.start_value_specified) {
        const int max_pn = (standard == videosynth::Standard::kNtsc ||
                            standard == videosynth::Standard::kPalM)
                               ? kNtscMaxPictureNumber
                               : kPalMaxPictureNumber;
        if (code.start_value < 0 || code.start_value > max_pn) {
          result->is_valid = false;
          result->errors.push_back(
              "Laserdisc injection validation error: picture_number "
              "start_value must be in the range 0-" +
              std::to_string(max_pn) + " for " +
              videosynth::StandardToString(standard) + "; got " +
              std::to_string(code.start_value) + ".");
          return false;
        }
      }

      // 5.5: fm_picture_number value range (IEC 60857 Amendment 2 §10.2.3).
      if (code.code_type == "fm_picture_number" && code.start_value_specified) {
        if (code.start_value < 0 || code.start_value > kNtscMaxPictureNumber) {
          result->is_valid = false;
          result->errors.push_back(
              "Laserdisc injection validation error: fm_picture_number "
              "start_value must be in the range 0-" +
              std::to_string(kNtscMaxPictureNumber) + "; got " +
              std::to_string(code.start_value) + ".");
          return false;
        }
      }

      // 5.5: chapter_number range (IEC 60856/60857: max 79 chapters).
      if (code.code_type == "chapter_number" && code.chapter_specified) {
        if (code.chapter < 0 || code.chapter > kMaxChapterNumber) {
          result->is_valid = false;
          result->errors.push_back(
              "Laserdisc injection validation error: chapter_number chapter "
              "must be in the range 0-" +
              std::to_string(kMaxChapterNumber) + "; got " +
              std::to_string(code.chapter) + ".");
          return false;
        }
      }

      // 5.5: users_code format (IEC 60856/60857 §10.1.9): 8 X1 D X3 X4 X5.
      //   X1 (bits 19-16) must be 0-7; D (bits 15-12) must be 0xD.
      if (code.code_type == "users_code" && code.users_code_specified) {
        uint32_t hex_value = 0;
        if (!ParseHexValue(code.users_code, &hex_value)) {
          result->is_valid = false;
          result->errors.push_back(
              "Laserdisc injection validation error: users_code '" +
              code.users_code + "' is not a valid hex value.");
          return false;
        }
        if (hex_value > 0xFFFFFFu) {
          result->is_valid = false;
          result->errors.push_back(
              "Laserdisc injection validation error: users_code '" +
              code.users_code + "' exceeds 24-bit range (max 0xFFFFFF).");
          return false;
        }
        const uint32_t x1 = (hex_value >> 16u) & 0x0Fu;
        if (x1 > kMaxUsersCodeX1Nibble) {
          result->is_valid = false;
          result->errors.push_back(
              "Laserdisc injection validation error: users_code X1 nibble "
              "must be 0-7; '" +
              code.users_code + "' has X1=" + std::to_string(x1) + ".");
          return false;
        }
        const uint32_t d_nibble = (hex_value >> 12u) & 0x0Fu;
        if (d_nibble != 0xDu) {
          result->is_valid = false;
          result->errors.push_back(
              "Laserdisc injection validation error: users_code D nibble "
              "(bits 15-12) must be 0xD (IEC §10.1.9); '" +
              code.users_code + "' has D=0x" + std::to_string(d_nibble) + ".");
          return false;
        }
      }
    }
  }

  return true;
}

// Validates the section-level line injections, which now carry only laserdisc
// code injections and vitc. The VITS set and disc_type are project-wide and
// validated in ValidateProjectLineInjections.
bool ValidateLineInjectionsForSection(const videosynth::Section& section,
                                      videosynth::Standard standard,
                                      videosynth::ValidationResult* result) {
  if (result == nullptr) {
    return false;
  }

  std::set<int> claimed_target_lines;
  bool has_laserdisc_injection = false;
  bool has_vitc_injection = false;

  for (const videosynth::Section::LineInjection& injection :
       section.line_injections) {
    const std::string injection_type = Lowercase(injection.type);

    if (injection_type == "vits") {
      result->is_valid = false;
      result->errors.push_back(
          "Line injection validation error: VITS injections are now "
          "configured project-wide in the top-level 'line_injections.vits' "
          "block, not per section.");
      return false;
    }

    if (injection_type != "laserdisc" && injection_type != "vitc") {
      result->is_valid = false;
      result->errors.push_back(
          "Line injection validation error: unsupported injection type '" +
          injection.type + "'.");
      return false;
    }

    if (injection_type == "laserdisc") {
      has_laserdisc_injection = true;
      if (!injection.target_lines.empty()) {
        result->is_valid = false;
        result->errors.push_back(
            "Line injection validation error: target_lines must not be "
            "specified for laserdisc injections.");
        return false;
      }
    } else {
      has_vitc_injection = true;
      if (injection.target_lines.empty()) {
        result->is_valid = false;
        result->errors.push_back(
            "Line injection validation error: target_lines must be provided "
            "and non-empty for injection type '" +
            injection.type + "'.");
        return false;
      }

      for (int line_1based : injection.target_lines) {
        if (!IsValidFrameLineForStandard(line_1based, standard)) {
          result->is_valid = false;
          result->errors.push_back(
              "Line injection validation error: target line " +
              std::to_string(line_1based) +
              " is outside the valid frame-line range for " +
              videosynth::StandardToString(standard) + ".");
          return false;
        }

        if (!claimed_target_lines.insert(line_1based).second) {
          result->is_valid = false;
          result->errors.push_back(
              "Line injection validation error: overlapping target line " +
              std::to_string(line_1based) + " within the same section.");
          return false;
        }
      }
    }
  }

  if (has_laserdisc_injection && has_vitc_injection) {
    result->is_valid = false;
    result->errors.push_back(
        "Line injection validation error: vitc and laserdisc injections cannot "
        "appear in the same section.");
    return false;
  }

  return true;
}

// SECAM preset rules. There is no SECAM LaserVision format (IEC 60856 defines
// PAL discs only), so laserdisc codes, disc_type and laserdisc VITS placement
// are rejected; the pilot burst and EFM audio are rejected by their own
// PAL-only rules. VITS fall back to the 625-line PAL catalogue.
void ValidateSecamPreset(const videosynth::Project& project,
                         videosynth::ValidationResult* result) {
  if (result == nullptr) {
    return;
  }

  const videosynth::CvbsPresets& presets = project.cvbs_presets;
  if (presets.video_standard_preset != videosynth::Standard::kSecam) {
    if (!presets.secam_field_identification) {
      result->warnings.push_back(
          "secam_field_identification has no effect unless "
          "video_standard_preset is SECAM.");
    }
    return;
  }

  const videosynth::ProjectLineInjections& li = project.line_injections;
  bool any_laserdisc_section = false;
  for (const videosynth::Section& section : project.sections) {
    if (SectionHasLaserdiscInjection(section)) {
      any_laserdisc_section = true;
      break;
    }
  }
  if (any_laserdisc_section || !li.disc_type.empty() ||
      li.placement == videosynth::VitsPlacement::kLaserdisc) {
    result->is_valid = false;
    result->errors.push_back(
        "Project configuration error: laserdisc line injections, "
        "line_injections.disc_type and laserdisc VITS placement are not "
        "available for SECAM projects.");
  }

  if (!li.vits.empty()) {
    result->warnings.push_back(
        "SECAM warning: VITS test signals are rendered with their PAL "
        "definitions, including any PAL-modulated chroma packets.");
  }
}

// Validates the project-wide line_injections block: the laserdisc disc_type
// selection and the VITS test-signal set. Cross-references the sections to
// require disc_type whenever any section declares laserdisc codes, and applies
// the reserved-line and NTSC/PAL-M VIRS rules to the project VITS set.
bool ValidateProjectLineInjections(const videosynth::Project& project,
                                   videosynth::ValidationResult* result) {
  if (result == nullptr) {
    return false;
  }

  const videosynth::Standard standard =
      project.cvbs_presets.video_standard_preset;
  const videosynth::ProjectLineInjections& li = project.line_injections;

  bool any_laserdisc_section = false;
  for (const videosynth::Section& section : project.sections) {
    if (SectionHasLaserdiscInjection(section)) {
      any_laserdisc_section = true;
      break;
    }
  }

  // disc_type: required when any section declares laserdisc codes; when set it
  // must be a recognised value.
  videosynth::DiscType disc_type = videosynth::DiscType::kUnknown;
  if (li.disc_type.empty()) {
    if (any_laserdisc_section) {
      result->is_valid = false;
      result->errors.push_back(
          "Line injection validation error: a section declares laserdisc "
          "codes but line_injections.disc_type is not set. Set disc_type to "
          "'CAV' or 'CLV' at the project level.");
      return false;
    }
  } else {
    disc_type = videosynth::DiscTypeFromString(li.disc_type);
    if (disc_type == videosynth::DiscType::kUnknown) {
      result->is_valid = false;
      result->errors.push_back("Line injection validation error: disc_type '" +
                               li.disc_type +
                               "' is not recognised. Expected 'CAV' or 'CLV'.");
      return false;
    }
  }

  const bool project_is_laserdisc = disc_type != videosynth::DiscType::kUnknown;

  // Validate the VITS set: resolvable types, strict placement, no overlaps,
  // and (for laserdisc projects) reserved-VBI-line avoidance.
  videosynth::VitsDefinitionProvider vits_definition_provider;
  std::set<int> claimed_target_lines;
  bool has_virs = false;
  for (const videosynth::VitsInjection& vits : li.vits) {
    if (vits.vits_type.empty()) {
      result->is_valid = false;
      result->errors.push_back(
          "Line injection validation error: VITS injections require a "
          "non-empty vits_type.");
      return false;
    }
    if (vits.vits_type == "virs") {
      has_virs = true;
    }

    if (vits.target_lines.empty()) {
      result->is_valid = false;
      result->errors.push_back(
          "Line injection validation error: VITS injection '" + vits.vits_type +
          "' must provide at least one target line.");
      return false;
    }

    videosynth::VitsDefinition vits_definition;
    std::string vits_error;
    if (!vits_definition_provider.TryGetDefinition(
            standard, vits.vits_type, &vits_definition, &vits_error)) {
      result->is_valid = false;
      result->errors.push_back("Line injection validation error: " +
                               vits_error);
      return false;
    }

    for (int line_1based : vits.target_lines) {
      if (!IsValidFrameLineForStandard(line_1based, standard)) {
        result->is_valid = false;
        result->errors.push_back(
            "Line injection validation error: VITS target line " +
            std::to_string(line_1based) +
            " is outside the valid frame-line range for " +
            videosynth::StandardToString(standard) + ".");
        return false;
      }

      if (!claimed_target_lines.insert(line_1based).second) {
        result->is_valid = false;
        result->errors.push_back(
            "Line injection validation error: overlapping VITS target line " +
            std::to_string(line_1based) + " within the project VITS set.");
        return false;
      }

      // Reserved-line conflict only applies when the disc carries biphase
      // codes (a laserdisc project); otherwise the VBI is free for VITS.
      if (project_is_laserdisc &&
          IsLaserdiscReservedLine(line_1based, standard)) {
        result->is_valid = false;
        result->errors.push_back(
            "Line injection validation error: VITS target line " +
            std::to_string(line_1based) +
            " conflicts with laserdisc reserved VBI ranges for " +
            videosynth::StandardToString(standard) + ".");
        return false;
      }
    }

    // Placement policy governs which frame lines a VITS type may occupy.
    switch (li.placement) {
      case videosynth::VitsPlacement::kStandard:
        // Strict: types with a defined broadcast line must target only it.
        if (vits_definition.recommended_frame_line > 0) {
          for (int line_1based : vits.target_lines) {
            if (line_1based != vits_definition.recommended_frame_line) {
              result->is_valid = false;
              result->errors.push_back(
                  "Line injection validation error: vits_type '" +
                  vits.vits_type + "' must target frame line " +
                  std::to_string(vits_definition.recommended_frame_line) +
                  " for " + videosynth::StandardToString(standard) +
                  " under standard placement.");
              return false;
            }
          }
        }
        break;
      case videosynth::VitsPlacement::kLaserdisc: {
        // Laserdisc placement: VITS may sit only on the standard's laserdisc
        // VBI lines (IEC 60856 §9.1.3 / IEC 60857 §9.1.3-9.1.4).
        const std::vector<int> laserdisc_lines =
            videosynth::LaserdiscVitsLines(standard);
        for (int line_1based : vits.target_lines) {
          if (std::find(laserdisc_lines.begin(), laserdisc_lines.end(),
                        line_1based) == laserdisc_lines.end()) {
            std::string allowed;
            for (std::size_t i = 0; i < laserdisc_lines.size(); ++i) {
              if (i > 0) {
                allowed += ", ";
              }
              allowed += std::to_string(laserdisc_lines[i]);
            }
            result->is_valid = false;
            result->errors.push_back(
                "Line injection validation error: VITS target line " +
                std::to_string(line_1based) +
                " is not a laserdisc VITS line for " +
                videosynth::StandardToString(standard) +
                " under laserdisc placement (allowed: " + allowed + ").");
            return false;
          }
        }
        break;
      }
      case videosynth::VitsPlacement::kCustom:
        // Custom placement: any in-range, non-overlapping, non-reserved line is
        // accepted (those checks are applied above).
        break;
    }
  }

  // NTSC and PAL-M laserdisc discs require a virs VITS for colour
  // (IEC 60857 §9.1.3). The requirement is project-wide now that VITS are.
  if (project_is_laserdisc &&
      (standard == videosynth::Standard::kNtsc ||
       standard == videosynth::Standard::kPalM) &&
      !has_virs) {
    result->is_valid = false;
    result->errors.push_back(
        "Line injection validation error: NTSC and PAL-M laserdisc projects "
        "require a virs VITS injection for colour (IEC 60857 §9.1.3).");
    return false;
  }

  return true;
}

// PAL/NTSC frame lines that orc-gui inspects for White SNR measurement.
// PAL: field-relative line 19. NTSC: field-relative line 20.
// Source: noise-injection-design.md §orc-gui Measurement Analysis.
constexpr int kOrcGuiWhiteSNRLinePal = 19;
constexpr int kOrcGuiWhiteSNRLineNtsc = 20;

// Noise spectrum relative-level bounds: the table describes a shape, and a
// 100 dB span covers every realistic channel response.
constexpr double kNoiseSpectrumRelativeDbMin = -60.0;
constexpr double kNoiseSpectrumRelativeDbMax = 40.0;

// Checks one noise layer's spectrum table. nyquist_mhz <= 0 skips the upper
// frequency bound (unknown sample encoding, reported elsewhere).
void ValidateNoiseSpectrum(
    const std::vector<videosynth::NoiseSpectrumPoint>& spectrum,
    const std::string& key, const std::string& section_name, double nyquist_mhz,
    videosynth::ValidationResult* result) {
  const std::string prefix =
      "Noise validation error in section '" + section_name + "': " + key;
  for (std::size_t i = 0; i < spectrum.size(); ++i) {
    const videosynth::NoiseSpectrumPoint& point = spectrum[i];
    if (point.frequency_mhz < 0.0 ||
        (nyquist_mhz > 0.0 && point.frequency_mhz > nyquist_mhz)) {
      result->is_valid = false;
      result->errors.push_back(prefix +
                               " frequency must be in the range [0.0, " +
                               std::to_string(nyquist_mhz) +
                               "] MHz (the 4fsc Nyquist "
                               "frequency); got " +
                               std::to_string(point.frequency_mhz) + ".");
      return;
    }
    if (i > 0 && point.frequency_mhz <= spectrum[i - 1].frequency_mhz) {
      result->is_valid = false;
      result->errors.push_back(
          prefix +
          " frequencies must be strictly ascending; "
          "got " +
          std::to_string(point.frequency_mhz) + " MHz after " +
          std::to_string(spectrum[i - 1].frequency_mhz) + " MHz.");
      return;
    }
    if (point.relative_db < kNoiseSpectrumRelativeDbMin ||
        point.relative_db > kNoiseSpectrumRelativeDbMax) {
      result->is_valid = false;
      result->errors.push_back(
          prefix + " relative_db must be in the range [-60.0, 40.0]; got " +
          std::to_string(point.relative_db) + ".");
      return;
    }
  }
}

// True when the section carries a CAV picture_number code, so its frames are
// anchored to disc positions.
bool SectionHasPictureNumberCode(const videosynth::Section& section) {
  for (const videosynth::Section::LineInjection& injection :
       section.line_injections) {
    for (const videosynth::Section::LineInjectionCode& code : injection.codes) {
      if (code.code_type == "picture_number") {
        return true;
      }
    }
  }
  return false;
}

void ValidateSharedNoiseParameters(const videosynth::Section& section,
                                   const std::string& disc_type,
                                   double nyquist_mhz,
                                   videosynth::ValidationResult* result) {
  const videosynth::SharedNoiseParameters& shared = section.noise.shared;
  if (!shared.enabled) {
    return;
  }

  if (shared.noise_db < kNoiseDbMin || shared.noise_db > kNoiseDbMax) {
    result->is_valid = false;
    result->errors.push_back(
        "Noise validation error in section '" + section.name +
        "': shared.noise_db must be in the range [20.0, 61.0]; got " +
        std::to_string(shared.noise_db) + ".");
    return;
  }

  if (!shared.noise_seed_specified) {
    result->is_valid = false;
    result->errors.push_back(
        "Noise validation error in section '" + section.name +
        "': shared.noise_seed is required; pressing noise must be identical "
        "in every capture of the pressing.");
    return;
  }

  ValidateNoiseSpectrum(shared.spectrum, "shared.spectrum", section.name,
                        nyquist_mhz, result);
  if (!result->is_valid) {
    return;
  }

  if (disc_type == "CAV" && !SectionHasPictureNumberCode(section)) {
    result->warnings.push_back(
        "Noise warning in section '" + section.name +
        "': noise.shared is set on a CAV section without a picture_number "
        "code; pressing noise falls back to output position.");
  }
}

// Project-wide channel (pressing) impairments.
void ValidateChannelParameters(const videosynth::Project& project,
                               videosynth::ValidationResult* result) {
  const videosynth::ChannelParameters& channel = project.channel;
  if (!channel.enabled) {
    return;
  }

  // Eight samples is about half a microsecond at 4fsc: far beyond any
  // sub-sample alignment error a stacking tool is expected to absorb.
  constexpr double kTimingOffsetLimitSamples = 8.0;
  if (!std::isfinite(channel.timing_offset_samples) ||
      std::abs(channel.timing_offset_samples) > kTimingOffsetLimitSamples) {
    result->is_valid = false;
    result->errors.push_back(
        "Channel validation error: timing_offset_samples must be in the range "
        "[-8.0, 8.0]; got " +
        std::to_string(channel.timing_offset_samples) + ".");
  }

  const videosynth::EdgeSmearParameters& edge_smear = channel.edge_smear;
  if (edge_smear.enabled) {
    // Beyond half the signal smeared, or a time constant past ~5.6 us, the
    // picture no longer resembles any working player's readout.
    if (!std::isfinite(edge_smear.fraction) || edge_smear.fraction < 0.0 ||
        edge_smear.fraction > 0.5) {
      result->is_valid = false;
      result->errors.push_back(
          "Channel validation error: edge_smear.fraction must be in the range "
          "[0.0, 0.5]; got " +
          std::to_string(edge_smear.fraction) + ".");
    }
    if (!std::isfinite(edge_smear.tau_samples) ||
        edge_smear.tau_samples <= 0.0 || edge_smear.tau_samples > 100.0) {
      result->is_valid = false;
      result->errors.push_back(
          "Channel validation error: edge_smear.tau_samples must be in the "
          "range (0.0, 100.0]; got " +
          std::to_string(edge_smear.tau_samples) + ".");
    }
  }

  const videosynth::FmBeatParameters& fm_beat = channel.fm_beat;
  if (fm_beat.enabled) {
    if (project.cvbs_presets.video_standard_preset !=
        videosynth::Standard::kPal) {
      result->is_valid = false;
      result->errors.push_back(
          "Channel validation error: fm_beat is supported for 625-line PAL "
          "only; the beat model is fitted to PAL LaserDisc pressings.");
      return;
    }
    // The reference model's carrier fits lie within a few tens of kHz of
    // 7.088 MHz; the range allows for other discs and players.
    if (!std::isfinite(fm_beat.carrier_black_hz) ||
        fm_beat.carrier_black_hz < 6.9e6 || fm_beat.carrier_black_hz > 7.3e6) {
      result->is_valid = false;
      result->errors.push_back(
          "Channel validation error: fm_beat.carrier_black_hz must be in the "
          "range [6900000, 7300000]; got " +
          std::to_string(fm_beat.carrier_black_hz) + ".");
    }
    if (!std::isfinite(fm_beat.level_db) || fm_beat.level_db < -60.0 ||
        fm_beat.level_db > 0.0) {
      result->is_valid = false;
      result->errors.push_back(
          "Channel validation error: fm_beat.level_db must be in the range "
          "[-60.0, 0.0]; got " +
          std::to_string(fm_beat.level_db) + ".");
    }
  }
}

// Project-wide chroma gain and phase error (one capture's player).
void ValidateChromaErrorParameters(const videosynth::Project& project,
                                   videosynth::ValidationResult* result) {
  const videosynth::ChromaErrorParameters& chroma_error = project.chroma_error;
  if (!chroma_error.enabled) {
    return;
  }

  if (project.cvbs_presets.video_standard_preset ==
      videosynth::Standard::kSecam) {
    result->is_valid = false;
    result->errors.push_back(
        "Chroma error validation error: chroma_error is not supported for "
        "SECAM; it models a quadrature (PAL/NTSC) chroma path.");
    return;
  }

  struct RangeCheck {
    const char* key;
    double value;
    double min;
    double max;
    const char* range;
  };
  const videosynth::ChromaJitterParameters& jitter = chroma_error.line_jitter;
  // Beyond these limits the picture is no longer a plausibly mis-adjusted
  // player: half or one-and-a-half times saturation, any hue, and per-line
  // jitter well past what a working chroma path shows.
  const RangeCheck checks[] = {
      {"gain", chroma_error.gain, 0.5, 1.5, "[0.5, 1.5]"},
      {"phase_deg", chroma_error.phase_deg, -180.0, 180.0, "[-180.0, 180.0]"},
      {"line_jitter.phase_deg", jitter.phase_deg, 0.0, 10.0, "[0.0, 10.0]"},
      {"line_jitter.gain", jitter.gain, 0.0, 0.1, "[0.0, 0.1]"},
  };
  for (const RangeCheck& check : checks) {
    if (!std::isfinite(check.value) || check.value < check.min ||
        check.value > check.max) {
      result->is_valid = false;
      result->errors.push_back(std::string("Chroma error validation error: ") +
                               check.key + " must be in the range " +
                               check.range + "; got " +
                               std::to_string(check.value) + ".");
    }
  }
}

void ValidateNoiseParameters(const videosynth::Section& section,
                             const videosynth::Project& project,
                             const std::set<int>& project_vits_lines,
                             videosynth::ValidationResult* result) {
  if (result == nullptr) {
    return;
  }

  const videosynth::Standard standard =
      project.cvbs_presets.video_standard_preset;
  // Noise is shaped on the 4fsc signal, before the output stage resamples
  // RAW encodings, so the 4fsc Nyquist bounds the spectrum for every preset.
  const double nyquist_mhz =
      videosynth::GetTimingConstants(standard).sample_rate_4fsc_hz / 2.0e6;

  ValidateSharedNoiseParameters(section, project.line_injections.disc_type,
                                nyquist_mhz, result);
  if (!result->is_valid || !section.noise.enabled) {
    return;
  }

  if (section.noise.noise_db < kNoiseDbMin ||
      section.noise.noise_db > kNoiseDbMax) {
    result->is_valid = false;
    result->errors.push_back(
        "Noise validation error in section '" + section.name +
        "': noise_db must be in the range [20.0, 61.0]; got " +
        std::to_string(section.noise.noise_db) + ".");
    return;
  }

  if (section.noise.noise_spread_db < 0.0) {
    result->is_valid = false;
    result->errors.push_back(
        "Noise validation error in section '" + section.name +
        "': noise_spread_db must be >= 0.0; got " +
        std::to_string(section.noise.noise_spread_db) + ".");
    return;
  }

  const double white_snr =
      section.noise.noise_db - section.noise.noise_spread_db;
  if (white_snr < kNoiseDbMin) {
    result->is_valid = false;
    result->errors.push_back("Noise validation error in section '" +
                             section.name +
                             "': noise_db - noise_spread_db must be >= 20.0 "
                             "(White SNR floor); got " +
                             std::to_string(white_snr) + ".");
    return;
  }

  ValidateNoiseSpectrum(section.noise.spectrum, "spectrum", section.name,
                        nyquist_mhz, result);
  if (!result->is_valid) {
    return;
  }
  ValidateNoiseSpectrum(section.noise.spread_spectrum, "spread_spectrum",
                        section.name, nyquist_mhz, result);
  if (!result->is_valid) {
    return;
  }
  if (!section.noise.spread_spectrum.empty() &&
      section.noise.noise_spread_db == 0.0) {
    result->warnings.push_back(
        "Noise warning in section '" + section.name +
        "': spread_spectrum is set, but noise_spread_db is 0, so there is no "
        "level-dependent term for it to shape.");
  }

  // Warn if the noise spread target is non-zero but no suitable VITS white-flag
  // injection is present on the line orc-gui inspects for White SNR. VITS are
  // project-wide, so the check consults the project VITS target-line set.
  if (section.noise.noise_spread_db > 0.0) {
    const int expected_line = videosynth::Is625LineStandard(standard)
                                  ? kOrcGuiWhiteSNRLinePal
                                  : kOrcGuiWhiteSNRLineNtsc;
    const bool has_white_flag_vits =
        project_vits_lines.find(expected_line) != project_vits_lines.end();
    if (!has_white_flag_vits) {
      result->warnings.push_back(
          "Noise warning in section '" + section.name +
          "': noise_spread_db is set, but no VITS injection targets line " +
          std::to_string(expected_line) +
          " — the White SNR target cannot be verified in orc-gui without a "
          "suitable VITS white flag on that line (" +
          videosynth::StandardToString(standard) + ").");
    }
  }
}

// Valid ranges for the per-section luma bandwidth limit, in MHz.
constexpr double kLumaBandwidthCutoffMinMhz = 0.5;
constexpr double kLumaBandwidthTransitionMinMhz = 0.1;
constexpr double kLumaBandwidthTransitionMaxMhz = 2.0;

void ValidateLumaBandwidth(const videosynth::Section& section,
                           const videosynth::Project& project,
                           videosynth::ValidationResult* result) {
  const videosynth::LumaBandwidthParameters& bandwidth = section.luma_bandwidth;
  if (result == nullptr || !bandwidth.enabled) {
    return;
  }
  const std::string context =
      "Luma bandwidth validation error in section '" + section.name + "': ";
  if (bandwidth.transition_mhz < kLumaBandwidthTransitionMinMhz ||
      bandwidth.transition_mhz > kLumaBandwidthTransitionMaxMhz) {
    result->is_valid = false;
    result->errors.push_back(context +
                             "transition_mhz must be in [0.1, 2.0]; got " +
                             std::to_string(bandwidth.transition_mhz) + ".");
    return;
  }
  // The stopband edge must stay below the 4fsc Nyquist frequency (2 f_sc).
  const double nyquist_mhz =
      videosynth::GetTimingConstants(project.cvbs_presets.video_standard_preset)
          .sample_rate_4fsc_hz /
      2.0e6;
  const double cutoff_max_mhz = nyquist_mhz - (bandwidth.transition_mhz / 2.0);
  if (bandwidth.cutoff_mhz < kLumaBandwidthCutoffMinMhz ||
      bandwidth.cutoff_mhz > cutoff_max_mhz) {
    result->is_valid = false;
    result->errors.push_back(context + "cutoff_mhz must be in [0.5, " +
                             std::to_string(cutoff_max_mhz) +
                             "] (2 f_sc less half the transition); got " +
                             std::to_string(bandwidth.cutoff_mhz) + ".");
  }
}

// Valid scale range for both random and scratch dropout.
constexpr int kDropoutScaleMin = 0;
constexpr int kDropoutScaleMax = 20;

void ValidateDropoutParameters(const videosynth::Section& section,
                               videosynth::ValidationResult* result) {
  if (result == nullptr) {
    return;
  }

  const videosynth::DropoutParameters& dp = section.dropouts;
  const bool random_active =
      dp.random.enabled && dp.random.scale > kDropoutScaleMin;
  const bool scratch_active =
      dp.scratch.enabled && dp.scratch.scale > kDropoutScaleMin;

  // Without a dropouts: block (both sub-blocks at their defaults) there is
  // nothing to validate; the stage will be a no-op for this section.
  if (!dp.block_present && !random_active && !scratch_active &&
      dp.random.scale == 0 && dp.scratch.scale == 0) {
    return;
  }

  if (dp.random.scale < kDropoutScaleMin ||
      dp.random.scale > kDropoutScaleMax) {
    result->is_valid = false;
    result->errors.push_back("Dropout validation error in section '" +
                             section.name +
                             "': random.scale must be in [0, 20]; got " +
                             std::to_string(dp.random.scale) + ".");
    return;
  }

  if (dp.scratch.scale < kDropoutScaleMin ||
      dp.scratch.scale > kDropoutScaleMax) {
    result->is_valid = false;
    result->errors.push_back("Dropout validation error in section '" +
                             section.name +
                             "': scratch.scale must be in [0, 20]; got " +
                             std::to_string(dp.scratch.scale) + ".");
    return;
  }

  // A dropouts: block with all-zero scales has no effect — report as a
  // configuration mistake so the user knows the block is inert.
  if (!random_active && !scratch_active) {
    result->is_valid = false;
    result->errors.push_back(
        "Dropout validation error in section '" + section.name +
        "': dropouts block has no active type (both random.scale and "
        "scratch.scale are 0). Remove the dropouts block or set at least one "
        "scale to 1–20.");
    return;
  }

  // Disc-anchored dropouts are a pressing defect, so every capture of the
  // pressing must draw the same events.
  if (random_active && dp.random.anchor == videosynth::DropoutAnchor::kDisc &&
      !dp.random.seed_specified) {
    result->is_valid = false;
    result->errors.push_back("Dropout validation error in section '" +
                             section.name +
                             "': random.anchor 'disc' requires random.seed, so "
                             "every capture of the pressing draws the same "
                             "dropouts.");
  }
  if (scratch_active && dp.scratch.anchor == videosynth::DropoutAnchor::kDisc &&
      !dp.scratch.seed_specified) {
    result->is_valid = false;
    result->errors.push_back("Dropout validation error in section '" +
                             section.name +
                             "': scratch.anchor 'disc' requires scratch.seed, "
                             "so every capture of the pressing draws the same "
                             "scratches.");
  }

  // Warn when the derived scratch lifespan exceeds the section length.
  if (scratch_active && !section.duration_frames_all) {
    const videosynth::ScratchDropoutDerivedParams sp =
        videosynth::DeriveScratchDropoutParams(dp.scratch.scale);
    if (sp.max_dur_frames > section.duration_frames) {
      result->warnings.push_back(
          "Dropout warning in section '" + section.name +
          "': scratch lifespan derived from scale " +
          std::to_string(dp.scratch.scale) + " (" +
          std::to_string(sp.max_dur_frames) +
          " frames) exceeds section duration (" +
          std::to_string(section.duration_frames) +
          " frames). The envelope will be truncated asymmetrically.");
    }
  }
}

// Valid OSD scale range: 1 pixel-per-glyph-pixel (8×8) to 4 (32×32).
constexpr int kOsdScaleMin = 1;
constexpr int kOsdScaleMax = 4;

void ValidateOsdConfig(const videosynth::Section& section,
                       videosynth::ValidationResult* result) {
  if (result == nullptr) {
    return;
  }

  for (const videosynth::OsdOverlay& ov : section.osd.overlays) {
    if (ov.scale < kOsdScaleMin || ov.scale > kOsdScaleMax) {
      result->is_valid = false;
      result->errors.push_back("OSD validation error in section '" +
                               section.name +
                               "': overlay scale must be in [1, 4]; got " +
                               std::to_string(ov.scale) + ".");
      return;
    }

    if (ov.fg_level == videosynth::OsdFgLevel::kUnknown) {
      result->is_valid = false;
      result->errors.push_back(
          "OSD validation error in section '" + section.name +
          "': overlay fg_luma '" + ov.fg_level_text +
          "' is not supported; must be one of white, light_grey, dark_grey, "
          "black.");
      return;
    }

    if (ov.bg_level == videosynth::OsdBgLevel::kUnknown) {
      result->is_valid = false;
      result->errors.push_back(
          "OSD validation error in section '" + section.name +
          "': overlay bg_luma '" + ov.bg_level_text +
          "' is not supported; must be one of transparent, white, "
          "light_grey, dark_grey, black.");
      return;
    }

    std::string unknown_token;
    if (!videosynth::OsdTokenResolver::HasOnlyKnownTokens(ov.text,
                                                          &unknown_token)) {
      result->is_valid = false;
      result->errors.push_back(
          "OSD validation error in section '" + section.name +
          "': overlay text contains unknown token '{" + unknown_token +
          "}'. Supported tokens: {picture_number}, {biphase_hex}, {phase_id}, "
          "{section_name}, {timecode}, {frame_number}.");
      return;
    }
  }
}

// Audio frequency bounds: [0, 22000] Hz, safely below the 24 kHz Nyquist limit
// of the 48 kHz audio sampling rate (CVBS File Format Specification, Audio
// Data).
constexpr double kAudioFrequencyMinHz = 0.0;
constexpr double kAudioFrequencyMaxHz = 22000.0;

bool IsAudioFrequencyInRange(double hz) {
  return hz >= kAudioFrequencyMinHz && hz <= kAudioFrequencyMaxHz;
}

// Validates one active channel (left or right) of a channel pair. `where`
// identifies the section, pair, and channel for error messages. Silent
// (disabled) channels carry no parameters and are skipped.
void ValidateAudioChannel(const videosynth::AudioParameters& audio,
                          const std::string& where,
                          const videosynth::Section& section,
                          videosynth::Standard standard,
                          videosynth::ValidationResult* result) {
  if (!audio.enabled) {
    return;
  }

  // Waveform must be one of the four supported shapes.
  if (audio.waveform == videosynth::AudioWaveform::kUnknown) {
    result->is_valid = false;
    result->errors.push_back(
        where + ": waveform '" + audio.waveform_text +
        "' is not recognised. Expected one of: sine, square, sawtooth, "
        "triangle.");
    return;
  }

  // Amplitude in [0.0, 1.0].
  if (audio.amplitude < 0.0 || audio.amplitude > 1.0) {
    result->is_valid = false;
    result->errors.push_back(where + ": amplitude must be in [0.0, 1.0]; got " +
                             std::to_string(audio.amplitude) + ".");
    return;
  }

  // A ramp replaces the fixed frequency.
  if (audio.ramp_enabled && audio.frequency_specified) {
    result->is_valid = false;
    result->errors.push_back(
        where + ": ramp and frequency are mutually exclusive; remove one.");
    return;
  }

  // Fixed frequency must be in range.
  if (!IsAudioFrequencyInRange(audio.frequency_hz)) {
    result->is_valid = false;
    result->errors.push_back(where +
                             ": frequency must be in [0, 22000] Hz; got " +
                             std::to_string(audio.frequency_hz) + ".");
    return;
  }

  if (!audio.ramp_enabled) {
    return;
  }

  // Ramp requires both start and end frequencies.
  if (!audio.ramp_start_specified || !audio.ramp_end_specified) {
    result->is_valid = false;
    result->errors.push_back(
        where + ": ramp requires both 'start' and 'end' frequencies.");
    return;
  }

  // Ramp mode must be recognised.
  if (audio.ramp_mode == videosynth::AudioRampMode::kUnknown) {
    result->is_valid = false;
    result->errors.push_back(
        where + ": ramp mode '" + audio.ramp_mode_text +
        "' is not recognised. Expected one of: up, down, bounce.");
    return;
  }

  // Ramp start/end frequencies must be in range.
  if (!IsAudioFrequencyInRange(audio.ramp_start_hz) ||
      !IsAudioFrequencyInRange(audio.ramp_end_hz)) {
    result->is_valid = false;
    result->errors.push_back(
        where +
        ": ramp start/end frequencies must be in [0, 22000] Hz; got "
        "start=" +
        std::to_string(audio.ramp_start_hz) +
        ", end=" + std::to_string(audio.ramp_end_hz) + ".");
    return;
  }

  // Ramp period must be non-negative.
  if (audio.ramp_period_seconds < 0.0) {
    result->is_valid = false;
    result->errors.push_back(where + ": ramp period must be >= 0; got " +
                             std::to_string(audio.ramp_period_seconds) + ".");
    return;
  }

  // A positive period must not exceed the section's duration in seconds. The
  // check is skipped for 'all'-duration sections and unknown standards (the
  // latter is already reported as a project-level error).
  if (audio.ramp_period_seconds > 0.0 && !section.duration_frames_all &&
      standard != videosynth::Standard::kUnknown) {
    const videosynth::TimingConstants timing =
        videosynth::GetTimingConstants(standard);
    const double section_seconds =
        (timing.frame_rate_hz > 0.0)
            ? section.duration_frames / timing.frame_rate_hz
            : 0.0;
    if (audio.ramp_period_seconds > section_seconds) {
      result->is_valid = false;
      result->errors.push_back(where + ": ramp period (" +
                               std::to_string(audio.ramp_period_seconds) +
                               " s) exceeds section duration (" +
                               std::to_string(section_seconds) + " s).");
    }
  }
}

// Seconds to one decimal place, for messages.
std::string FormatSeconds(double seconds) {
  std::ostringstream text;
  text << std::fixed << std::setprecision(1) << seconds;
  return text.str();
}

// True when `path` ends in `extension` (lower-case, with the dot), ignoring
// case.
bool HasExtensionIgnoringCase(const std::string& path,
                              const std::string& extension) {
  if (path.size() < extension.size()) {
    return false;
  }
  for (std::size_t i = 0; i < extension.size(); ++i) {
    const char c = path[path.size() - extension.size() + i];
    if (std::tolower(static_cast<unsigned char>(c)) != extension[i]) {
      return false;
    }
  }
  return true;
}

// Structural rules for an audio-file pair: a path with a known root token and
// a WAV or FLAC extension.
void ValidateAudioFileContent(const videosynth::AudioChannelPair& channel_pair,
                              const std::string& pair_label,
                              videosynth::ValidationResult* result) {
  const std::string& path = channel_pair.file.path;
  if (path.empty()) {
    result->is_valid = false;
    result->errors.push_back(pair_label + ": file.path must be set.");
    return;
  }
  std::string root_error;
  if (!ValidateAssetRootToken(path, pair_label, "file.path", &root_error)) {
    result->is_valid = false;
    result->errors.push_back(root_error);
    return;
  }
  if (!HasExtensionIgnoringCase(path, ".wav") &&
      !HasExtensionIgnoringCase(path, ".flac")) {
    result->is_valid = false;
    result->errors.push_back(pair_label +
                             ": file.path must name a .wav or .flac file; "
                             "got '" +
                             path + "'.");
  }
}

// Structural rules for a video-soundtrack pair: a non-negative stream index
// and a section source that is a video rather than an EXR still.
void ValidateVideoAudioContent(const videosynth::Section& section,
                               const videosynth::AudioChannelPair& channel_pair,
                               const std::string& pair_label,
                               videosynth::ValidationResult* result) {
  if (channel_pair.video_audio.stream < 0) {
    result->is_valid = false;
    result->errors.push_back(
        pair_label + ": video_audio.stream must be >= 0; got " +
        std::to_string(channel_pair.video_audio.stream) + ".");
    return;
  }
  if (!section.source.empty() &&
      !videosynth::IsProgressiveVideoSource(section.source)) {
    result->is_valid = false;
    result->errors.push_back(
        pair_label +
        ": video_audio needs a video source (MKV or MP4); the section source "
        "is an EXR still.");
  }
}

// Probe-backed rules for every file and video-soundtrack pair of a section.
// `video_profile` is the probed section source, or null when the source was
// not probed (or is a still); `source_frame_count` is its frame count or
// kUnknownSourceFrameCount. File checks run only with an audio file probe.
void ValidateMediaAudioSources(
    const videosynth::Section& section, videosynth::Standard standard,
    const videosynth::ProgressiveFrameSourceProfile* video_profile,
    int source_frame_count, videosynth::IAudioFileProbe* audio_file_probe,
    videosynth::ILogger* logger, videosynth::ValidationResult* result) {
  const double target_rate_hz = videosynth::AudioSampleRateHz(standard);
  for (const videosynth::AudioChannelPair& channel_pair :
       section.audio_channel_pairs) {
    const std::string pair_label = "Audio validation error in section '" +
                                   section.name + "' channel pair " +
                                   std::to_string(channel_pair.pair);
    const std::string info_label = "Audio: section '" + section.name +
                                   "' channel pair " +
                                   std::to_string(channel_pair.pair);

    if (channel_pair.content == videosynth::AudioPairContent::kVideo &&
        video_profile != nullptr) {
      const int stream = channel_pair.video_audio.stream;
      const int stream_count =
          static_cast<int>(video_profile->audio_streams.size());
      if (stream >= stream_count) {
        result->is_valid = false;
        result->errors.push_back(pair_label + ": the section source has " +
                                 std::to_string(stream_count) +
                                 " audio stream(s), so video_audio.stream " +
                                 std::to_string(stream) + " does not exist.");
        continue;
      }
      const videosynth::ProgressiveAudioStreamProfile& audio =
          video_profile->audio_streams[static_cast<std::size_t>(stream)];
      if (logger != nullptr && audio.channels > 2) {
        logger->Info(info_label + ": the soundtrack's " +
                     std::to_string(audio.channels) +
                     " channels are downmixed to stereo.");
      }
      if (logger != nullptr && audio.sample_rate_hz > 0 &&
          audio.sample_rate_hz != static_cast<int>(target_rate_hz)) {
        logger->Info(info_label + ": the soundtrack is resampled from " +
                     std::to_string(audio.sample_rate_hz) + " Hz to " +
                     std::to_string(static_cast<int>(target_rate_hz)) + " Hz.");
      }
      continue;
    }

    if (channel_pair.content != videosynth::AudioPairContent::kFile ||
        audio_file_probe == nullptr) {
      continue;
    }
    const std::string& path = channel_pair.file.path;
    videosynth::AudioFileProfile profile;
    std::string probe_error;
    if (!audio_file_probe->Probe(path, &profile, &probe_error)) {
      result->is_valid = false;
      result->errors.push_back(pair_label + ": audio file '" + path +
                               "' could not be read: " + probe_error);
      continue;
    }
    if (profile.channels < 1 || profile.channels > 2) {
      result->is_valid = false;
      result->errors.push_back(
          pair_label + ": audio file '" + path + "' has " +
          std::to_string(profile.channels) +
          " channels; only mono and stereo files are supported.");
      continue;
    }
    if (logger != nullptr && profile.sample_rate_hz > 0 &&
        profile.sample_rate_hz != static_cast<int>(target_rate_hz)) {
      logger->Info(info_label + ": '" + path + "' is resampled from " +
                   std::to_string(profile.sample_rate_hz) + " Hz to " +
                   std::to_string(static_cast<int>(target_rate_hz)) + " Hz.");
    }

    const std::int64_t run_frames =
        videosynth::KnownSectionOutputFrameCount(section, source_frame_count);
    if (!channel_pair.file.repeat && run_frames > 0 &&
        profile.duration_seconds > 0.0) {
      const double run_seconds =
          static_cast<double>(run_frames) /
          videosynth::GetTimingConstants(standard).frame_rate_hz;
      if (profile.duration_seconds < run_seconds) {
        result->warnings.push_back(
            "Section '" + section.name + "' channel pair " +
            std::to_string(channel_pair.pair) + ": audio file '" + path +
            "' lasts " + FormatSeconds(profile.duration_seconds) +
            " s but the section runs " + FormatSeconds(run_seconds) +
            " s; the rest of the section is silent (set repeat: true to "
            "loop it).");
      }
    }
  }
}

void ValidateAudioParameters(const videosynth::Section& section,
                             videosynth::Standard standard,
                             videosynth::ValidationResult* result) {
  if (result == nullptr) {
    return;
  }

  std::set<int> seen_pairs;
  for (const videosynth::AudioChannelPair& channel_pair :
       section.audio_channel_pairs) {
    const std::string pair_label =
        "Audio validation error in section '" + section.name +
        "' channel pair " +
        (channel_pair.pair_specified ? std::to_string(channel_pair.pair)
                                     : std::string("(unspecified)"));

    // The channel-pair number must be present and within the 0–7 range.
    if (!channel_pair.pair_specified) {
      result->is_valid = false;
      result->errors.push_back(pair_label +
                               ": each channel pair must specify 'pair'.");
      continue;
    }
    if (channel_pair.pair < 0 ||
        channel_pair.pair >= videosynth::kMaxAudioChannelPairs) {
      result->is_valid = false;
      result->errors.push_back(
          pair_label + ": 'pair' must be in [0, " +
          std::to_string(videosynth::kMaxAudioChannelPairs - 1) + "]; got " +
          std::to_string(channel_pair.pair) + ".");
      continue;
    }

    // Channel-pair numbers must be unique within a section.
    if (!seen_pairs.insert(channel_pair.pair).second) {
      result->is_valid = false;
      result->errors.push_back(pair_label +
                               ": duplicate channel pair number in section.");
      continue;
    }

    if (channel_pair.content == videosynth::AudioPairContent::kFile) {
      ValidateAudioFileContent(channel_pair, pair_label, result);
      continue;
    }
    if (channel_pair.content == videosynth::AudioPairContent::kVideo) {
      ValidateVideoAudioContent(section, channel_pair, pair_label, result);
      continue;
    }

    // A declared tone pair must carry at least one active channel.
    if (!videosynth::AudioChannelPairIsActive(channel_pair)) {
      result->is_valid = false;
      result->errors.push_back(
          pair_label +
          ": must define at least one active channel ('left' or 'right').");
      continue;
    }

    ValidateAudioChannel(channel_pair.left, pair_label + " left", section,
                         standard, result);
    ValidateAudioChannel(channel_pair.right, pair_label + " right", section,
                         standard, result);
  }
}

// True if any section declares the given channel-pair number.
bool ProjectDeclaresChannelPair(const videosynth::Project& project, int pair) {
  for (const videosynth::Section& section : project.sections) {
    for (const videosynth::AudioChannelPair& channel_pair :
         section.audio_channel_pairs) {
      if (channel_pair.pair_specified && channel_pair.pair == pair) {
        return true;
      }
    }
  }
  return false;
}

// Validates the project-level EFM digital audio selection: the channel pair
// exists, the video standard has a LaserDisc digital audio specification, and
// the programme-area track layout satisfies the CD-DA/LaserDisc track rules.
void ValidateEfmAudioOutput(const videosynth::Project& project,
                            videosynth::ValidationResult* result) {
  if (result == nullptr || !project.output.efm_audio.enabled) {
    return;
  }

  const int pair = project.output.efm_audio.pair;
  if (pair < 0 || pair >= videosynth::kMaxAudioChannelPairs) {
    result->is_valid = false;
    result->errors.push_back(
        "Project configuration error: output.efm_audio.pair must be in [0, " +
        std::to_string(videosynth::kMaxAudioChannelPairs - 1) + "]; got " +
        std::to_string(pair) + ".");
  } else if (!ProjectDeclaresChannelPair(project, pair)) {
    result->warnings.push_back(
        "output.efm_audio.pair " + std::to_string(pair) +
        " is not declared by any section; no EFM stream is written.");
  }

  // LaserDisc digital audio is specified for 625-line PAL (IEC 60856:1986
  // Amd 2, clause 13) and 525-line discs (IEC 60857:1986 Amd 2, clause 13),
  // which PAL-M shares; SECAM has no EFM sample-frequency definition.
  const videosynth::Standard standard =
      project.cvbs_presets.video_standard_preset;
  if (!videosynth::StandardHasLaserdiscDigitalAudio(standard)) {
    result->is_valid = false;
    result->errors.push_back(
        "Project configuration error: output.efm_audio requires a PAL, NTSC "
        "or PAL_M video_standard_preset; got '" +
        videosynth::StandardToString(standard) + "'.");
    return;
  }

  const double frame_rate_hz =
      videosynth::GetTimingConstants(standard).frame_rate_hz;
  if (frame_rate_hz <= 0.0) {
    return;
  }

  // One track per programme-area section, numbered by section sequence.
  std::vector<const videosynth::Section*> programme_sections;
  bool has_lead_in = false;
  for (const videosynth::Section& section : project.sections) {
    if (section.section_type == videosynth::SectionType::kProgrammeArea) {
      programme_sections.push_back(&section);
    } else if (section.section_type == videosynth::SectionType::kLeadIn) {
      has_lead_in = true;
    }
  }

  if (static_cast<int>(programme_sections.size()) > kMaxEfmTracks) {
    result->is_valid = false;
    result->errors.push_back(
        "Project configuration error: output.efm_audio supports at most " +
        std::to_string(kMaxEfmTracks) + " tracks; the project has " +
        std::to_string(programme_sections.size()) +
        " programme_area section(s).");
  }

  for (std::size_t index = 0; index < programme_sections.size(); ++index) {
    const videosynth::Section& section = *programme_sections[index];
    // Sections that play their whole source have no statically known length.
    if (section.duration_frames_all || section.duration_frames <= 0) {
      continue;
    }
    // The first track's leading 2 s are the mandatory pause and do not count
    // towards its 4 s minimum length.
    const double minimum_seconds =
        kMinEfmTrackSeconds + (index == 0 ? kEfmFirstTrackPauseSeconds : 0.0);
    const double section_seconds =
        static_cast<double>(section.duration_frames) / frame_rate_hz;
    if (section_seconds < minimum_seconds) {
      result->warnings.push_back(
          "output.efm_audio: programme_area section '" + section.name +
          "' is shorter than the " +
          std::to_string(static_cast<int>(minimum_seconds)) +
          " s minimum length required for EFM track " +
          std::to_string(index + 1) + ".");
    }
  }

  if (!has_lead_in) {
    result->warnings.push_back(
        "output.efm_audio: the project has no lead_in section, so no table of "
        "contents will be emitted.");
  }
}

// Validates disc-structure section ordering. When lead_in, lead_out, or
// programme_area section types are declared, the section sequence must be
// [lead_in] programme_area... [lead_out]: no section may precede the lead_in,
// no section may follow the lead_out, and every section after a lead_in or
// before a lead_out must be programme_area. Out-of-order sections would break
// the monotonic picture-number and time-code generation (IEC 60856/60857).
// IEC 60856/60857 §10.1.5: a chapter lasts at least 30 tracks, which is 30
// frames on CAV. A chapter starts at the first section carrying
// chapter_number and at each section that sets `chapter`; later sections
// carrying chapter_number without `chapter` continue it. CLV track density
// varies, so CLV chapters are not checked, and a chapter with a section that
// plays its whole source has no statically known length.
void ValidateChapterLengths(const videosynth::Project& project,
                            videosynth::ValidationResult* result) {
  if (result == nullptr ||
      videosynth::DiscTypeFromString(project.line_injections.disc_type) !=
          videosynth::DiscType::kCAV) {
    return;
  }

  struct ChapterRun {
    int chapter = 0;
    std::string first_section;
    int frames = 0;
    bool length_known = true;
  };
  std::optional<ChapterRun> run;

  const auto finish_run = [&]() {
    if (run.has_value() && run->length_known &&
        run->frames <
            videosynth::ChapterNumberGenerator::kMinimumChapterTracks) {
      result->warnings.push_back(
          "Laserdisc injection warning: CAV chapter " +
          std::to_string(run->chapter) + " starting in section '" +
          run->first_section + "' lasts " + std::to_string(run->frames) +
          " frames; IEC 60856/60857 requires at least " +
          std::to_string(
              videosynth::ChapterNumberGenerator::kMinimumChapterTracks) +
          ".");
    }
  };

  for (const videosynth::Section& section : project.sections) {
    const videosynth::Section::LineInjectionCode* chapter_code = nullptr;
    for (const videosynth::Section::LineInjection& injection :
         section.line_injections) {
      if (Lowercase(injection.type) != "laserdisc") {
        continue;
      }
      for (const videosynth::Section::LineInjectionCode& code :
           injection.codes) {
        if (code.code_type == "chapter_number") {
          chapter_code = &code;
        }
      }
    }
    if (chapter_code == nullptr) {
      continue;
    }

    if (!run.has_value() || chapter_code->chapter_specified) {
      finish_run();
      run = ChapterRun{};
      run->chapter =
          chapter_code->chapter_specified ? chapter_code->chapter : 0;
      run->first_section = section.name;
    }
    if (section.duration_frames_all) {
      run->length_known = false;
    } else {
      run->frames += section.duration_frames;
    }
  }
  finish_run();
}

void ValidateSectionOrdering(const videosynth::Project& project,
                             videosynth::ValidationResult* result) {
  if (result == nullptr) {
    return;
  }

  const std::size_t section_count = project.sections.size();
  std::size_t lead_in_count = 0;
  std::size_t lead_out_count = 0;
  std::size_t lead_in_index = 0;
  std::size_t lead_out_index = 0;
  for (std::size_t i = 0; i < section_count; ++i) {
    switch (project.sections[i].section_type) {
      case videosynth::SectionType::kLeadIn:
        if (lead_in_count == 0) {
          lead_in_index = i;
        }
        ++lead_in_count;
        break;
      case videosynth::SectionType::kLeadOut:
        lead_out_index = i;  // Track the last occurrence.
        ++lead_out_count;
        break;
      default:
        break;
    }
  }

  if (lead_in_count == 0 && lead_out_count == 0) {
    return;
  }

  if (lead_in_count > 1) {
    result->is_valid = false;
    result->errors.push_back(
        "Section ordering validation error: only one lead_in section is "
        "allowed; found " +
        std::to_string(lead_in_count) + ".");
  }
  if (lead_out_count > 1) {
    result->is_valid = false;
    result->errors.push_back(
        "Section ordering validation error: only one lead_out section is "
        "allowed; found " +
        std::to_string(lead_out_count) + ".");
  }

  // No section may precede the lead_in.
  if (lead_in_count > 0) {
    for (std::size_t i = 0; i < lead_in_index; ++i) {
      result->is_valid = false;
      result->errors.push_back(
          "Section ordering validation error: section '" +
          project.sections[i].name +
          "' must not appear before the lead_in section '" +
          project.sections[lead_in_index].name + "'.");
    }
  }

  // No section may follow the lead_out.
  if (lead_out_count > 0) {
    for (std::size_t i = lead_out_index + 1; i < section_count; ++i) {
      result->is_valid = false;
      result->errors.push_back(
          "Section ordering validation error: section '" +
          project.sections[i].name +
          "' must not appear after the lead_out section '" +
          project.sections[lead_out_index].name + "'.");
    }
  }

  // Every section after the lead_in and before the lead_out must be
  // programme_area. Each bound applies on its own so a partial disc structure
  // (only a lead_in, or only a lead_out) is still checked. Duplicate
  // lead_in/lead_out sections are skipped here; the count checks above
  // already report them.
  const std::size_t region_begin =
      lead_in_count > 0 ? lead_in_index + 1 : static_cast<std::size_t>(0);
  const std::size_t region_end =
      lead_out_count > 0 ? lead_out_index : section_count;
  if (lead_in_count > 0 || lead_out_count > 0) {
    for (std::size_t i = region_begin; i < region_end && i < section_count;
         ++i) {
      const videosynth::Section& section = project.sections[i];
      if (section.section_type == videosynth::SectionType::kProgrammeArea ||
          section.section_type == videosynth::SectionType::kLeadIn ||
          section.section_type == videosynth::SectionType::kLeadOut) {
        continue;
      }
      std::string position;
      if (lead_in_count > 0 && lead_out_count > 0) {
        position = "between the lead_in and lead_out sections";
      } else if (lead_in_count > 0) {
        position = "after the lead_in section";
      } else {
        position = "before the lead_out section";
      }
      result->is_valid = false;
      result->errors.push_back(
          "Section ordering validation error: section '" + section.name +
          "' must have section_type 'programme_area' because it is " +
          position + ".");
    }
  }
}

}  // namespace

namespace videosynth {

ProjectValidator::ProjectValidator(
    IProgressiveFrameSourceProbe* progressive_frame_source_probe,
    ILogger* logger, IAudioFileProbe* audio_file_probe)
    : progressive_frame_source_probe_(progressive_frame_source_probe),
      logger_(logger),
      audio_file_probe_(audio_file_probe) {}

ValidationResult ProjectValidator::Validate(const Project& project) {
  ValidationResult result;
  result.is_valid = true;

  if (logger_ != nullptr) {
    logger_->Debug("Validating project with " +
                   std::to_string(project.sections.size()) + " section(s).");
  }

  if (project.cvbs_presets.video_standard_preset == Standard::kUnknown) {
    result.is_valid = false;
    result.errors.push_back(
        "Project configuration error: video_standard_preset must be 'PAL', "
        "'NTSC', 'PAL_M', or 'SECAM'.");
  }

  if (!IsSupportedSampleEncodingPreset(
          project.cvbs_presets.sample_encoding_preset)) {
    result.is_valid = false;
    result.errors.push_back(
        "Project configuration error: sample_encoding_preset must be one of "
        "the "
        "supported CVBS or raw presets.");
  }

  // Generated output is time-base stable at the standard 4fsc rate and phase
  // locked, except SECAM, which defines no Sc/H convention and must be declared
  // unlocked (CVBS File Format Specification v1.7.0, Preset: SECAM).
  const bool secam =
      project.cvbs_presets.video_standard_preset == Standard::kSecam;
  const std::string required_signal_state =
      GeneratedSignalStatePreset(project.cvbs_presets.video_standard_preset);
  if (project.cvbs_presets.signal_state_preset != required_signal_state) {
    result.is_valid = false;
    result.errors.push_back(
        "Project configuration error: signal_state_preset must be '" +
        required_signal_state + "'" + (secam ? " for SECAM projects" : "") +
        ".");
  }

  ValidateDeferredLaserdiscPresetFlags(project, &result);
  ValidateSecamPreset(project, &result);
  ValidateChannelParameters(project, &result);
  ValidateChromaErrorParameters(project, &result);

  if (project.cvbs_presets.video_standard_preset != Standard::kNtsc &&
      project.cvbs_presets.video_standard_preset != Standard::kPalM &&
      project.cvbs_presets.ntsc_black_setup_ire_specified) {
    result.is_valid = false;
    result.errors.push_back(
        "Project configuration error: ntsc_black_setup_ire can only be "
        "specified "
        "for NTSC or PAL-M projects.");
  }

  if ((project.cvbs_presets.video_standard_preset == Standard::kNtsc ||
       project.cvbs_presets.video_standard_preset == Standard::kPalM) &&
      !IsSupportedNtscBlackSetupIre(
          project.cvbs_presets.ntsc_black_setup_ire)) {
    result.is_valid = false;
    result.errors.push_back(
        "Project configuration error: ntsc_black_setup_ire must be 7.5 or "
        "0.0.");
  }

  if (project.output.video_path.empty()) {
    result.is_valid = false;
    result.errors.push_back(
        "Project configuration error: output.video_path must be set.");
  } else {
    std::string output_root_error;
    if (!ValidateAssetRootToken(project.output.video_path,
                                "Project configuration error",
                                "output.video_path", &output_root_error)) {
      result.is_valid = false;
      result.errors.push_back(output_root_error);
    }
  }

  // The metadata sidecar path is always derived from video_path (colocated),
  // so it needs no separate "must be set" / "must differ" validation.

  const std::string& sig_type = project.output.signal_type;
  if (sig_type != "composite" && sig_type != "yc") {
    result.is_valid = false;
    result.errors.push_back(
        "Project configuration error: output.signal_type must be 'composite' "
        "or 'yc'.");
  }
  if (sig_type == "yc" && !project.output.video_path.empty()) {
    const std::string& vp = project.output.video_path;
    constexpr std::string_view kLumaSuffix = ".cvbsy";
    if (vp.size() < kLumaSuffix.size() ||
        vp.compare(vp.size() - kLumaSuffix.size(), kLumaSuffix.size(),
                   kLumaSuffix) != 0) {
      result.is_valid = false;
      result.errors.push_back(
          "Project configuration error: output.video_path must end in "
          "'.cvbsy' when signal_type is 'yc'.");
    }
  }

  ValidateEfmAudioOutput(project, &result);

  if (project.sections.empty()) {
    result.is_valid = false;
    result.errors.push_back("Project must contain at least one section.");
  }

  // Disc-structure ordering: [lead_in] programme_area... [lead_out].
  ValidateSectionOrdering(project, &result);

  // Validate the project-wide line_injections block (disc_type + VITS set)
  // before the per-section pass; the section pass relies on the project
  // disc_type and VITS set.
  ValidateProjectLineInjections(project, &result);
  ValidateChapterLengths(project, &result);

  // Project VITS target lines, consulted by the per-section noise check.
  std::set<int> project_vits_lines;
  for (const VitsInjection& vits : project.line_injections.vits) {
    for (int line : vits.target_lines) {
      project_vits_lines.insert(line);
    }
  }

  for (const Section& section : project.sections) {
    if (logger_ != nullptr) {
      logger_->Trace("Validating section '" + section.name + "' of type '" +
                     section.type + "'.");
    }

    if (section.type == "progressive") {
      if (!ValidateLaserdiscInjectionStructure(
              section, project.line_injections.disc_type, &result)) {
        break;
      }

      if (!ValidateLaserdiscSectionTypeAndCodes(
              section, project.cvbs_presets.video_standard_preset,
              project.line_injections.disc_type, &result)) {
        break;
      }

      if (!ValidateLineInjectionsForSection(
              section, project.cvbs_presets.video_standard_preset, &result)) {
        break;
      }

      ValidateDeferredLineInjectionSupport(section, &result);
      if (!result.is_valid) {
        break;
      }

      ValidateNoiseParameters(section, project, project_vits_lines, &result);
      if (!result.is_valid) {
        break;
      }

      ValidateLumaBandwidth(section, project, &result);
      if (!result.is_valid) {
        break;
      }

      ValidateDropoutParameters(section, &result);
      if (!result.is_valid) {
        break;
      }

      ValidateOsdConfig(section, &result);
      if (!result.is_valid) {
        break;
      }

      ValidateAudioParameters(
          section, project.cvbs_presets.video_standard_preset, &result);
      if (!result.is_valid) {
        break;
      }

      if (section.source.empty()) {
        result.is_valid = false;
        result.errors.push_back(
            "Progressive section validation error: source must be set.");
        break;
      }

      std::string asset_root_error;
      if (!ValidateAssetRootToken(section.source,
                                  "Progressive section validation error",
                                  "source", &asset_root_error)) {
        result.is_valid = false;
        result.errors.push_back(asset_root_error);
        break;
      }

      if (!section.duration_frames_all && section.duration_frames <= 0) {
        result.is_valid = false;
        result.errors.push_back(
            "Progressive section validation error: duration_frames must be > 0 "
            "or 'all'.");
        break;
      }

      if (section.duration_frames_repeat < 1) {
        result.is_valid = false;
        result.errors.push_back(
            "Progressive section validation error: duration_repeat must be >= "
            "1.");
        break;
      }

      if (!section.duration_frames_all && section.duration_frames_repeat != 1) {
        result.warnings.push_back(
            "Progressive section '" + section.name +
            "': duration_repeat is ignored unless duration_frames is 'all'.");
      }

      std::string source_family_error;
      if (!ValidateProgressiveSourceFamily(section, &source_family_error)) {
        result.is_valid = false;
        result.errors.push_back(source_family_error);
        break;
      }

      if (!ValidateSourceFrameRange(section, &result)) {
        break;
      }

      // Set once a video source has been probed, for the media audio rules.
      std::optional<ProgressiveFrameSourceProfile> video_profile;
      int source_frame_count = kUnknownSourceFrameCount;
      if (progressive_frame_source_probe_ != nullptr) {
        ProgressiveFrameSourceProfile profile;
        std::string probe_error;
        if (!progressive_frame_source_probe_->Probe(section, &profile,
                                                    &probe_error)) {
          result.is_valid = false;
          result.errors.push_back(probe_error.empty()
                                      ? "Progressive section validation error: "
                                        "source profile probing failed."
                                      : probe_error);
          break;
        }

        if (!RasterMatchesStandard(
                profile.width, profile.height,
                project.cvbs_presets.video_standard_preset)) {
          result.is_valid = false;
          result.errors.push_back(
              "Progressive section validation error: source raster must be "
              "720x576 for PAL and 720x486 for NTSC or PAL-M.");
          break;
        }

        if (!FrameRateMatchesStandard(
                profile.frame_rate_hz,
                project.cvbs_presets.video_standard_preset)) {
          result.is_valid = false;
          result.errors.push_back(
              "Progressive section validation error: source frame rate must "
              "match selected video standard.");
          break;
        }

        if (IsProgressiveVideoSource(section.source)) {
          if (!ValidateVideoProfile(section,
                                    project.cvbs_presets.video_standard_preset,
                                    profile, &result, logger_)) {
            result.is_valid = false;
            break;
          }
          if (!ValidateSourceFrameRangeBounds(section, profile.frame_count,
                                              &result)) {
            break;
          }
          video_profile = profile;
          source_frame_count = profile.frame_count;
        } else {
          std::string profile_error;
          if (!ValidateProfileBySourceFamily(
                  section, project.cvbs_presets.video_standard_preset, profile,
                  &profile_error)) {
            result.is_valid = false;
            result.errors.push_back(
                profile_error.empty() ? "Progressive section validation error: "
                                        "unsupported source profile."
                                      : profile_error);
            break;
          }
        }
      }

      ValidateMediaAudioSources(
          section, project.cvbs_presets.video_standard_preset,
          video_profile.has_value() ? &*video_profile : nullptr,
          source_frame_count, audio_file_probe_, logger_, &result);
      if (!result.is_valid) {
        break;
      }

      continue;
    }

    result.is_valid = false;
    result.errors.push_back(
        "Section validation error: section type must be 'progressive'.");
    break;
  }

  if (logger_ != nullptr && result.is_valid) {
    logger_->Debug("Project validation completed successfully.");
  }

  return result;
}

}  // namespace videosynth
