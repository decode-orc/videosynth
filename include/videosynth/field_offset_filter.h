/*
 * File:        field_offset_filter.h
 * Module:      channel
 * Purpose:     Picture-domain sub-pixel offset of a source image, used to
 *              misregister field 2 against field 1.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include "videosynth/progressive_frame_source.h"

namespace videosynth {

// Half-length of the interpolation kernel; the kernel has 2 * this taps.
constexpr int kFieldOffsetHalfTaps = 8;
constexpr double kFieldOffsetKaiserBeta = 8.0;

// Returns a copy of source whose active region is moved right by dx_pixels
// and down by dy_rows: output(x, y) = source(x - dx_pixels, y - dy_rows).
// Integer parts are exact index shifts; fractional parts are separable
// Kaiser-windowed-sinc interpolations normalised to unit DC gain, so flat
// areas keep their codes. Positions outside the active region take its
// nearest edge. Pixels outside the active region are copied unchanged.
// Results are rounded to the nearest code.
//
// Thread-safety: stateless; safe to call concurrently.
FrameSourceImage OffsetSourceImage(const FrameSourceImage& source,
                                   double dx_pixels, double dy_rows);

}  // namespace videosynth
