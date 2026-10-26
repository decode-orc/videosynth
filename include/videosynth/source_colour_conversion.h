/*
 * File:        source_colour_conversion.h
 * Module:      source_colour_conversion
 * Purpose:     Plans the ffmpeg filter chain that maps a probed video source
 *              to BT.601 10-bit 4:2:2 studio-swing YCbCr.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <string>
#include <vector>

#include "videosynth/interfaces.h"
#include "videosynth/model.h"

namespace videosynth {

// The colour normalisation a video source needs before synthesis.
struct SourceColourConversion {
  // ffmpeg -vf filter chain; empty when the source already is BT.601
  // yuv422p10le studio swing and passes through bit-exact.
  std::string filter_chain;
  // User-facing descriptions of each conversion applied (for example
  // "full range -> studio swing"); empty for a pass-through source.
  std::vector<std::string> conversions;
  // Metadata mismatches that are deliberately not converted (primaries,
  // transfer); intended for debug logging.
  std::vector<std::string> unconverted_notes;

  bool IsPassThrough() const { return filter_chain.empty(); }
};

// Plans the conversion of a probed video source to ITU-R BT.601-5 10-bit
// 4:2:2 studio-swing coding (Y' 64-940, Cb/Cr 64-960):
// - color_range pc/jpeg (or a yuvj* pixel format) is scaled to studio range;
//   tv or unknown is taken as studio range already.
// - A known non-BT.601 matrix (bt709, fcc, smpte240m, bt2020) is converted to
//   BT.601; an unknown matrix is assumed to be BT.601 (the SD convention).
// - RGB pixel formats are converted to BT.601 studio-range YCbCr.
// - Every other pixel format or bit depth is converted to yuv422p10le.
// - Primaries and transfer are never converted; mismatches are only noted.
//
// Pure function: depends on its arguments only. Thread-safe.
SourceColourConversion PlanSourceColourConversion(
    const ProgressiveFrameSourceProfile& profile, Standard standard);

}  // namespace videosynth
