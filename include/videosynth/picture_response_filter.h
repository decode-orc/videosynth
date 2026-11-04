/*
 * File:        picture_response_filter.h
 * Module:      generation
 * Purpose:     Separable linear-phase FIRs for the per-section picture
 *              response: a horizontal shaping of each active line and a
 *              vertical shaping across the rows of a progressive picture,
 *              both on fixed-point mV luma.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <cstddef>
#include <vector>

#include "videosynth/fixed_point.h"
#include "videosynth/model.h"

namespace videosynth {

// Horizontal kernel length. At 4fsc the frequency-sampling grid is fs / 127:
// about 140 kHz for PAL and 113 kHz for NTSC, and the Hann window widens each
// breakpoint transition to a few hundred kHz, ample for a camera-chain
// response that changes over MHz.
constexpr int kPictureResponseHorizontalTaps = 127;

// Vertical kernel length, in frame rows. The grid is 1/31 cycle per row,
// which resolves a response that changes over 1/16 cycle per row.
constexpr int kPictureResponseVerticalTaps = 31;

// The `domesday` preset's curves: the BBC Domesday disc's Test Card F against
// the bench's redrawn card, as luma power ratios in each band on held runs of
// the Community South disc (decode-orc issue analysis, videosynth issue #18).
// Horizontally a ~2 dB shelf from 2.5 to 4 MHz falling to -10 dB by 5.25 MHz;
// vertically a monotonic roll-off reaching -10 dB from 3/8 cycle per row.
std::vector<NoiseSpectrumPoint> DomesdayHorizontalResponseDb();
std::vector<VerticalResponsePoint> DomesdayVerticalResponseDb();

// The curves a section's picture response applies: the preset's, or the
// section's own for kCustom. Either may be empty (that direction unfiltered).
std::vector<NoiseSpectrumPoint> ResolvedHorizontalResponseDb(
    const PictureResponseParameters& response);
std::vector<VerticalResponsePoint> ResolvedVerticalResponseDb(
    const PictureResponseParameters& response);

// Returns the curve's level in dB at cycles_per_row: straight lines between
// breakpoints, end values held. An empty curve is flat (0 dB).
//
// Thread-safety: Stateless; safe to call concurrently.
double InterpolateVerticalResponseDb(
    const std::vector<VerticalResponsePoint>& curve, double cycles_per_row);

// The picture response of one section at one sample rate.
//
// Design (once, at construction), for each direction with a curve:
//   1. Sample the amplitude 10^(dB/20) of the interpolated curve on the
//      kernel-length DFT grid from zero frequency to Nyquist.
//   2. Inverse real DFT of that zero-phase response, centred on the middle
//      tap: the kernel is symmetric, so the filter is linear phase with an
//      integer delay that the centring removes.
//   3. Hann window with non-zero end taps, to suppress the ripple of the
//      frequency-sampled design.
//   4. Normalise to unit DC gain, so flat areas and picture levels pass
//      unchanged and only the curve's shape matters.
//
// Samples beyond either end of a line, and rows beyond the top or bottom of
// the picture, are taken as copies of the end ones (edge replication).
// Results are rounded to the nearest fixed-point step.
//
// Complexity: construction is O(taps^2); ApplyHorizontal is O(count * taps)
// and ApplyVertical is O(rows * width * taps), streaming whole rows so the
// inner loop runs along contiguous memory.
//
// Thread-safety: Immutable after construction; the Apply methods are const
// and safe to call concurrently on distinct buffers.
class PictureResponseFilter {
 public:
  // Throws std::invalid_argument when sample_rate_hz is not positive.
  PictureResponseFilter(const PictureResponseParameters& response,
                        double sample_rate_hz);

  bool has_horizontal() const { return !horizontal_kernel_.empty(); }
  bool has_vertical() const { return !vertical_kernel_.empty(); }

  // True when this filter was designed for these settings.
  bool Matches(const PictureResponseParameters& response,
               double sample_rate_hz) const {
    return response_ == response && sample_rate_hz_ == sample_rate_hz;
  }

  // Filters count samples of in into out (which must not alias in). Requires
  // has_horizontal().
  void ApplyHorizontal(const SampleFixed* in, std::size_t count,
                       SampleFixed* out) const;

  // Filters a row-major plane of rows x width samples (row stride width)
  // down its columns, from in into out (which must not alias in). Requires
  // has_vertical().
  void ApplyVertical(const SampleFixed* in, std::size_t rows, std::size_t width,
                     SampleFixed* out) const;

  const std::vector<double>& horizontal_kernel() const {
    return horizontal_kernel_;
  }
  const std::vector<double>& vertical_kernel() const {
    return vertical_kernel_;
  }

 private:
  PictureResponseParameters response_;
  double sample_rate_hz_;
  std::vector<double> horizontal_kernel_;
  std::vector<double> vertical_kernel_;
};

}  // namespace videosynth
