/*
 * File:        osd_token_resolver.cpp
 * Module:      osd
 * Purpose:     Resolves per-frame template tokens in OSD overlay text strings.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "videosynth/osd_token_resolver.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <set>
#include <string>

#include "videosynth/biphase_injection_manager.h"

namespace videosynth {

namespace {

// NOLINTBEGIN(readability-magic-numbers)
std::string FormatPictureNumber(int picture_number) {
  if (picture_number <= 0) {
    // IEC 60856/60857: max picture number 99999, so a real value is always
    // five digits; render an all-zero field of the same width when no CAV
    // picture-number code is active.
    return "00000";
  }
  // Buffer sized for sign + 10 digits + NUL; the 5-digit zero-pad is the
  // normal case (max valid picture number is 99999) but we don't truncate.
  char buf[12];
  std::snprintf(buf, sizeof(buf), "%05d", picture_number);
  return std::string(buf);
}

// Zero-pads the 1-based sequential output frame number to the same 5-digit
// width as the CAV picture number (IEC 60856/60857 max 99999); larger values
// are not truncated.
std::string FormatFrameNumber(int frame_number) {
  char buf[12];
  std::snprintf(buf, sizeof(buf), "%05d", frame_number);
  return std::string(buf);
}

std::string FormatBiphaseHex(const std::vector<uint32_t>& words) {
  if (words.empty()) {
    // One 24-bit code word renders as six hex digits; show an all-zero word
    // of the same width when no generators are active.
    return "000000";
  }
  std::string result;
  bool first = true;
  for (uint32_t word : words) {
    if (!first) {
      result += ' ';
    }
    first = false;
    char buf[7];
    std::snprintf(buf, sizeof(buf), "%06X", word);
    result += buf;
  }
  return result;
}

// The 2:3 pulldown slot of the field, e.g. "B2"; "-" for a video cadence.
std::string FormatCadence(const PerFrameContext& ctx) {
  if (ctx.cadence_letter == '\0') {
    return "-";
  }
  return std::string(1, ctx.cadence_letter) +
         std::to_string(ctx.cadence_field_in_letter);
}

std::string FormatClvTimecode(const PerFrameContext& ctx) {
  if (!ctx.has_clv_timecode) {
    // Same HH:MM:SS:FF shape as a real value so overlay layout is unchanged
    // on non-CLV discs.
    return "00:00:00:00";
  }
  // HH:MM:SS:FF; buffer sized for four zero-padded fields plus separators.
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d:%02d", ctx.clv_hours,
                ctx.clv_minutes, ctx.clv_seconds, ctx.clv_frames);
  return std::string(buf);
}
// NOLINTEND(readability-magic-numbers)

const std::set<std::string>& KnownTokens() {
  static const std::set<std::string> kTokens = {
      "picture_number", "biphase_hex",  "phase_id",   "section_name",
      "timecode",       "frame_number", "film_frame", "cadence"};
  return kTokens;
}

}  // namespace

std::string OsdTokenResolver::Resolve(const std::string& text,
                                      const PerFrameContext& ctx,
                                      const std::string& section_name) const {
  std::string result;
  result.reserve(text.size());

  for (std::size_t i = 0; i < text.size();) {
    if (text[i] != '{') {
      result += text[i];
      ++i;
      continue;
    }
    const std::size_t close = text.find('}', i);
    if (close == std::string::npos) {
      result += text[i];
      ++i;
      continue;
    }
    const std::string token = text.substr(i + 1, close - i - 1);
    if (token == "picture_number") {
      result += FormatPictureNumber(ctx.picture_number);
    } else if (token == "biphase_hex") {
      result += FormatBiphaseHex(ctx.biphase_words);
    } else if (token == "phase_id") {
      result += std::to_string(ctx.colour_frame_index);
    } else if (token == "section_name") {
      result += section_name;
    } else if (token == "timecode") {
      result += FormatClvTimecode(ctx);
    } else if (token == "frame_number") {
      result += FormatFrameNumber(ctx.frame_number);
    } else if (token == "film_frame") {
      result += std::to_string(ctx.film_frame);
    } else if (token == "cadence") {
      result += FormatCadence(ctx);
    } else {
      result += text.substr(i, close - i + 1);
    }
    i = close + 1;
  }

  return result;
}

bool OsdTokenResolver::HasOnlyKnownTokens(const std::string& text,
                                          std::string* unknown_token) {
  for (std::size_t i = 0; i < text.size();) {
    if (text[i] != '{') {
      ++i;
      continue;
    }
    const std::size_t close = text.find('}', i);
    if (close == std::string::npos) {
      ++i;
      continue;
    }
    const std::string token = text.substr(i + 1, close - i - 1);
    if (!token.empty() && KnownTokens().find(token) == KnownTokens().end()) {
      if (unknown_token != nullptr) {
        *unknown_token = token;
      }
      return false;
    }
    i = close + 1;
  }
  return true;
}

}  // namespace videosynth
