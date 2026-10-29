/*
 * File:        sample_clamp.h
 * Module:      fixed_point
 * Purpose:     Per-standard legal fixed-point mV bounds applied by the
 *              impairment stages before output quantisation.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include "videosynth/fixed_point.h"
#include "videosynth/model.h"

namespace videosynth {

// Thread-safety: stateless; may be called concurrently.
struct SampleClampBounds {
  SampleFixed min;
  SampleFixed max;
};

// Per-standard legal mV bounds derived from the quantisation profiles used by
// OutputStage: (code - blanking_code) * millivolts_per_code.
//
// PAL:  blanking=256, min_code=4,  max_code=1019, 1.1905 mV/code.
// NTSC: blanking=240, min_code=16, max_code=1019, 1.2755 mV/code.
inline SampleClampBounds SampleClampBoundsFor(Standard standard) {
  constexpr double kPalClampMinMv = -300.006;
  constexpr double kPalClampMaxMv = 908.452;
  constexpr double kNtscClampMinMv = -285.712;
  constexpr double kNtscClampMaxMv = 993.984;
  const bool is_625 = Is625LineStandard(standard);
  return SampleClampBounds{
      MillivoltsToSampleFixed(is_625 ? kPalClampMinMv : kNtscClampMinMv),
      MillivoltsToSampleFixed(is_625 ? kPalClampMaxMv : kNtscClampMaxMv)};
}

}  // namespace videosynth
