/*
 * File:        picture_truth.h
 * Module:      picture_truth
 * Purpose:     Geometry and per-frame buffers of the picture truth: the
 *              component Y, U and V that went into the composite modulator.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <cstddef>
#include <vector>

#include "videosynth/model.h"

namespace videosynth {

// Where the truth raster sits in the signal and how its values scale.
//
// The raster covers the 4fsc active window of every active line. Rows are in
// progressive (frame) order: the upper field supplies the even rows and the
// lower field the odd rows, exactly as the source picture's rows are drawn.
//
// Thread-safety: plain data; safe to read concurrently.
struct PictureTruthLayout {
  Standard standard = Standard::kUnknown;
  double sample_rate_hz = 0.0;
  // Samples per row, and the offset of the first one from 0H of its line.
  int samples_per_line = 0;
  int first_sample = 0;
  // Rows per frame (two fields of active_lines_per_field).
  int rows = 0;
  int active_lines_per_field = 0;
  // 1-based frame line carrying each field's first row.
  int first_line_field1 = 0;
  int first_line_field2 = 0;
  // Field (1 or 2) whose lines become the even rows.
  int upper_field = 1;
  // Line whose picture ends part-way along (0 for none), and the sample
  // offset from 0H at which it ends; truth samples beyond it are zero.
  int half_picture_line = 0;
  int half_picture_end_sample = 0;
  // Black and white levels in mV above blanking, the reference Y is given
  // against.
  double black_mv = 0.0;
  double white_mv = 0.0;

  std::size_t PlaneSamples() const {
    return static_cast<std::size_t>(samples_per_line) *
           static_cast<std::size_t>(rows);
  }

  // Row of the truth raster for an active frame line, or -1 for a line that
  // carries no picture.
  int RowForLine(int line_1based) const;
};

// The truth of one frame: three planes, row-major, each PlaneSamples() long.
//
//   y_mv: luma in mV above blanking (NTSC and PAL-M include the setup).
//   u_mv: the sin-axis colour difference (PAL/PAL-M U, NTSC B-Y), in peak mV
//         of subcarrier.
//   v_mv: the cos-axis colour difference (PAL/PAL-M V, NTSC R-Y), in peak mV
//         of subcarrier, with the PAL V-switch removed (as a PAL decoder
//         recovers it). Any chroma_line_sign region is included.
//
// The active-window chroma the encoder modulated is
// C = u_mv * sin(phi) + s * v_mv * cos(phi), where phi is the subcarrier
// phase and s the line's V-switch sign (always +1 on NTSC).
//
// Thread-safety: plain data; one writer at a time.
struct PictureTruthFrame {
  std::vector<float> y_mv;
  std::vector<float> u_mv;
  std::vector<float> v_mv;

  // Sizes all three planes for the layout, zero-filled.
  void Reset(const PictureTruthLayout& layout);
};

// The truth layout for a project's standard and signal levels. Returns a
// layout with rows == 0 for SECAM or an unknown standard, which carry no
// quadrature truth.
//
// Thread-safety: pure function.
PictureTruthLayout BuildPictureTruthLayout(const CvbsPresets& presets);

}  // namespace videosynth
