/*
 * File:        video_band_limit_reference.h
 * Module:      test_support
 * Purpose:     Predicts a painted picture line after the standard's video
 *              band limit, from the line's hard-edged 0H-relative render
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <cstddef>
#include <vector>

#include "videosynth/model.h"
#include "videosynth/timing_constants.h"
#include "videosynth/video_band_limit.h"

namespace videosynth::test_support {

// Wraps a line's hard-edged 0H-relative render, render(j) in millivolts for
// any line offset j, as the generator paints it after VideoBandLimiter: the
// result at j is sum h[k] render(j - reach + k). The render must hold
// blanking beyond the painted picture, as the generator's band limit sees the
// picture as a run on a blanking-level line. The generator rounds the result
// to its fixed-point step, so it matches to within that.
//
// Thread-safety: the returned callable owns its kernel; pure.
template <typename Render>
auto BandLimitedRender(Standard standard, Render render) {
  const VideoBandLimiter limiter(
      standard, GetTimingConstants(standard).sample_rate_4fsc_hz);
  return [kernel = limiter.kernel(), reach = limiter.reach(),
          render](int j) -> double {
    double value = 0.0;
    for (std::size_t k = 0; k < kernel.size(); ++k) {
      value += kernel[k] * render(j - reach + static_cast<int>(k));
    }
    return value;
  };
}

}  // namespace videosynth::test_support
