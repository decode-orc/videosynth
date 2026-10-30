/*
 * File:        preview_render.h
 * Module:      gui
 * Purpose:     Pure rendering helpers converting synthesised Y/C sample
 *              buffers and decoded source images to displayable QImages
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <QImage>
#include <vector>

#include "videosynth/fixed_point.h"
#include "videosynth/model.h"
#include "videosynth/progressive_frame_source.h"

namespace videosynth::gui {

// Thread-safety: all functions in this module are thread-safe pure functions.

// Encoded-picture rendering mode. Composite combines Y+C before quantising
// to the 10-bit code space; luma and chroma render the channels separately
// for the project's dual-file YC signal type.
enum class EncodedImageMode {
  kComposite,
  kLuma,
  kChroma,
};

// 1-based first line and line count of one field within the interlaced
// frame's line sequence (field 1 = lines 1..N, field 2 = the remainder).
struct FieldLineRange {
  int first_line_1based = 0;
  int line_count = 0;
};

FieldLineRange GetFieldLineRange(Standard standard, int field_1based);

// Maps a rendered picture row (0-based within a field image) to the frame
// line number, and back. Rows outside the field clamp to its edges.
int PictureRowToLineNumber(Standard standard, int field_1based, int row);
int LineNumberToPictureRow(Standard standard, int line_1based);
int LineNumberToField(Standard standard, int line_1based);

// Spatial (interlaced) display order of the frame's lines: element `row`
// holds the 1-based frame line drawn at display row `row`. Field 1 and field
// 2 lines are woven so the raster reads as the broadcast picture rather than
// the two fields stacked: each field 2 picture line sits directly above the
// field 1 line that carries the next progressive source row, matching the
// generation stage's row mapping. Length equals the standard's
// lines_per_frame.
std::vector<int> InterlacedLineOrder(Standard standard);

// Maps a woven full-frame picture row (0-based) to its frame line number and
// back. Rows/lines out of range clamp to the frame's edges.
int FrameRowToLineNumber(Standard standard, int row);
int LineNumberToFrameRow(Standard standard, int line_1based);

// Renders one field of the synthesised frame as a grayscale raster: each
// sample is quantised to the 10-bit CVBS code space exactly as the output
// stage writes it, then mapped to 8-bit grayscale (code >> 2). The image is
// samples_per_line wide (PAL long lines 313/625 crop their two trailing
// extra samples) and field-line-count tall, covering the full field
// including the VBI region. Returns a null image for empty buffers or an
// unknown standard.
QImage RenderEncodedFieldImage(const std::vector<SampleFixed>& y_mv,
                               const std::vector<SampleFixed>& c_mv,
                               Standard standard, int field_1based,
                               EncodedImageMode mode);

// Renders the complete interlaced frame: every line quantised as the output
// stage writes it, woven into spatial display order (InterlacedLineOrder).
// The image is samples_per_line wide and lines_per_frame tall. Returns a null
// image for empty buffers or an unknown standard.
QImage RenderEncodedFrameImage(const std::vector<SampleFixed>& y_mv,
                               const std::vector<SampleFixed>& c_mv,
                               Standard standard, EncodedImageMode mode);

// Converts a decoded 10-bit YCbCr 4:4:4 source frame (active area) to a
// display RGB image via BT.601. Returns a null image when the source has no
// active pixels.
QImage RenderSourceImage(const FrameSourceImage& source);

// Extracts one line's samples from a full-frame channel buffer, converted
// to millivolts. Empty when the line or buffer is out of range.
std::vector<double> ExtractLineMillivolts(
    const std::vector<SampleFixed>& channel_mv, Standard standard,
    int line_1based);

}  // namespace videosynth::gui
