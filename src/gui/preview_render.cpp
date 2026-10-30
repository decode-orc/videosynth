/*
 * File:        preview_render.cpp
 * Module:      gui
 * Purpose:     Pure rendering helpers converting synthesised Y/C sample
 *              buffers and decoded source images to displayable QImages
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "preview_render.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

#include "videosynth/cvbs_quantization.h"
#include "videosynth/frame_line_layout.h"
#include "videosynth/signal_timing_model.h"
#include "videosynth/timing_constants.h"

namespace videosynth::gui {

namespace {

// Maps a clamped 10-bit code (0..1023) to 8-bit grayscale.
inline int CodeToGray(int code) { return code >> 2; }

// BT.601 studio-range conversion of one 10-bit YCbCr 4:4:4 pixel to 8-bit
// R'G'B'. ITU-R BT.601-7 Section 2.5: Y' spans 64..940 (876 steps), Cb/Cr
// span ±448 about 512 at 10-bit depth.
inline QRgb YCbCrToRgb(const YCbCr444Pixel& pixel) {
  const double y = (static_cast<double>(pixel.y) - 64.0) / 876.0;
  const double cb = (static_cast<double>(pixel.cb) - 512.0) / 896.0;
  const double cr = (static_cast<double>(pixel.cr) - 512.0) / 896.0;

  // ITU-R BT.601-7 Section 2.5.1: E'R − E'Y = 1.402 E'Cr and
  // E'B − E'Y = 1.772 E'Cb (inverse of the 0.299/0.587/0.114 luma weights).
  const double r = y + 1.402 * cr;
  const double g = y - 0.344136 * cb - 0.714136 * cr;
  const double b = y + 1.772 * cb;

  const auto to_8bit = [](double value) {
    return static_cast<int>(std::lround(std::clamp(value, 0.0, 1.0) * 255.0));
  };
  return qRgb(to_8bit(r), to_8bit(g), to_8bit(b));
}

}  // namespace

FieldLineRange GetFieldLineRange(Standard standard, int field_1based) {
  const TimingConstants timing = GetTimingConstants(standard);
  const int field1_lines = Field1LineCount(standard);
  if (field_1based <= 1) {
    return FieldLineRange{1, field1_lines};
  }
  return FieldLineRange{field1_lines + 1,
                        timing.lines_per_frame - field1_lines};
}

int PictureRowToLineNumber(Standard standard, int field_1based, int row) {
  const FieldLineRange range = GetFieldLineRange(standard, field_1based);
  const int clamped_row = std::clamp(row, 0, range.line_count - 1);
  return range.first_line_1based + clamped_row;
}

int LineNumberToPictureRow(Standard standard, int line_1based) {
  const int field = LineNumberToField(standard, line_1based);
  const FieldLineRange range = GetFieldLineRange(standard, field);
  return std::clamp(line_1based - range.first_line_1based, 0,
                    range.line_count - 1);
}

int LineNumberToField(Standard standard, int line_1based) {
  return GetFieldIndex(
      standard,
      std::clamp(line_1based, 1, GetTimingConstants(standard).lines_per_frame));
}

std::vector<int> InterlacedLineOrder(Standard standard) {
  const TimingConstants timing = GetTimingConstants(standard);
  const int total = timing.lines_per_frame;
  const int field1_lines = Field1LineCount(standard);
  std::vector<int> order;
  order.reserve(static_cast<std::size_t>(std::max(0, total)));

  // Pair the lines as the generation stage maps a progressive source onto
  // them: the n-th picture line of field 2 takes source row 2n and the n-th
  // of field 1 row 2n + 1, so field 2 line L + offset sits directly above
  // field 1 line L. Field 2 lines without a field 1 partner go above (before
  // field 1 line 1) or below (after field 1's last line).
  const ActiveRasterGeometry active =
      GetActiveRasterGeometry(standard, timing.sample_rate_4fsc_hz);
  const int offset =
      active.first_active_line_field2 - active.first_active_line_field1;
  int field2_line = field1_lines + 1;
  for (int field1_line = 1; field1_line <= field1_lines; ++field1_line) {
    while (field2_line <= total && field2_line <= field1_line + offset) {
      order.push_back(field2_line++);
    }
    order.push_back(field1_line);
  }
  while (field2_line <= total) {
    order.push_back(field2_line++);
  }
  return order;
}

int FrameRowToLineNumber(Standard standard, int row) {
  const std::vector<int> order = InterlacedLineOrder(standard);
  if (order.empty()) {
    return 1;
  }
  return order[static_cast<std::size_t>(
      std::clamp(row, 0, static_cast<int>(order.size()) - 1))];
}

int LineNumberToFrameRow(Standard standard, int line_1based) {
  const std::vector<int> order = InterlacedLineOrder(standard);
  for (std::size_t row = 0; row < order.size(); ++row) {
    if (order[row] == line_1based) {
      return static_cast<int>(row);
    }
  }
  return 0;
}

namespace {

// Quantises one full-frame sample to the 8-bit grayscale the views display,
// matching the output stage's 10-bit code mapping for the given mode.
uchar EncodedSampleToGray(const std::vector<SampleFixed>& y_mv,
                          const std::vector<SampleFixed>& c_mv,
                          std::size_t sample, EncodedImageMode mode,
                          const QuantizationProfile& profile) {
  int code = 0;
  switch (mode) {
    case EncodedImageMode::kComposite:
      code = MapCompositeFixedToCode(y_mv[sample] + c_mv[sample], profile);
      break;
    case EncodedImageMode::kLuma:
      code = MapCompositeFixedToCode(y_mv[sample], profile);
      break;
    case EncodedImageMode::kChroma:
      code = MapChromaFixedToCode(c_mv[sample], profile);
      break;
  }
  return static_cast<uchar>(CodeToGray(ClampToLegalCodeRange(code, profile)));
}

}  // namespace

QImage RenderEncodedFieldImage(const std::vector<SampleFixed>& y_mv,
                               const std::vector<SampleFixed>& c_mv,
                               Standard standard, int field_1based,
                               EncodedImageMode mode) {
  QuantizationProfile profile;
  if (!BuildQuantizationProfile(standard, &profile)) {
    return QImage();
  }

  const TimingConstants timing = GetTimingConstants(standard);
  const std::vector<int> line_counts = BuildLineSampleCounts(
      standard, timing.lines_per_frame, timing.samples_per_line_4fsc);
  const std::vector<int> line_offsets = BuildLineSampleOffsets(line_counts);
  const FieldLineRange field_range = GetFieldLineRange(standard, field_1based);

  const int width = timing.samples_per_line_4fsc;
  const int height = field_range.line_count;
  const std::size_t frame_samples =
      static_cast<std::size_t>(line_offsets.back()) +
      static_cast<std::size_t>(line_counts.back());
  if (y_mv.size() < frame_samples || c_mv.size() < frame_samples ||
      width <= 0 || height <= 0) {
    return QImage();
  }

  QImage image(width, height, QImage::Format_Grayscale8);
  for (int row = 0; row < height; ++row) {
    const int line_index = field_range.first_line_1based - 1 + row;
    const std::size_t line_start =
        static_cast<std::size_t>(line_offsets[line_index]);
    uchar* scanline = image.scanLine(row);
    for (int x = 0; x < width; ++x) {
      scanline[x] = EncodedSampleToGray(
          y_mv, c_mv, line_start + static_cast<std::size_t>(x), mode, profile);
    }
  }
  return image;
}

QImage RenderEncodedFrameImage(const std::vector<SampleFixed>& y_mv,
                               const std::vector<SampleFixed>& c_mv,
                               Standard standard, EncodedImageMode mode) {
  QuantizationProfile profile;
  if (!BuildQuantizationProfile(standard, &profile)) {
    return QImage();
  }

  const TimingConstants timing = GetTimingConstants(standard);
  const std::vector<int> line_counts = BuildLineSampleCounts(
      standard, timing.lines_per_frame, timing.samples_per_line_4fsc);
  const std::vector<int> line_offsets = BuildLineSampleOffsets(line_counts);
  const std::vector<int> line_order = InterlacedLineOrder(standard);

  const int width = timing.samples_per_line_4fsc;
  const int height = static_cast<int>(line_order.size());
  const std::size_t frame_samples =
      static_cast<std::size_t>(line_offsets.back()) +
      static_cast<std::size_t>(line_counts.back());
  if (y_mv.size() < frame_samples || c_mv.size() < frame_samples ||
      width <= 0 || height <= 0) {
    return QImage();
  }

  QImage image(width, height, QImage::Format_Grayscale8);
  for (int row = 0; row < height; ++row) {
    const int line_index = line_order[static_cast<std::size_t>(row)] - 1;
    const std::size_t line_start =
        static_cast<std::size_t>(line_offsets[line_index]);
    uchar* scanline = image.scanLine(row);
    for (int x = 0; x < width; ++x) {
      scanline[x] = EncodedSampleToGray(
          y_mv, c_mv, line_start + static_cast<std::size_t>(x), mode, profile);
    }
  }
  return image;
}

QImage RenderSourceImage(const FrameSourceImage& source) {
  if (source.active_width <= 0 || source.active_height <= 0 ||
      source.pixels.empty()) {
    return QImage();
  }

  QImage image(source.active_width, source.active_height, QImage::Format_RGB32);
  for (int row = 0; row < source.active_height; ++row) {
    QRgb* scanline = reinterpret_cast<QRgb*>(image.scanLine(row));
    for (int column = 0; column < source.active_width; ++column) {
      scanline[column] = YCbCrToRgb(
          source.PixelAt(source.active_x + column, source.active_y + row));
    }
  }
  return image;
}

std::vector<double> ExtractLineMillivolts(
    const std::vector<SampleFixed>& channel_mv, Standard standard,
    int line_1based) {
  if (standard == Standard::kUnknown) {
    return {};
  }

  const TimingConstants timing = GetTimingConstants(standard);
  if (line_1based < 1 || line_1based > timing.lines_per_frame) {
    return {};
  }

  const std::vector<int> line_counts = BuildLineSampleCounts(
      standard, timing.lines_per_frame, timing.samples_per_line_4fsc);
  const std::vector<int> line_offsets = BuildLineSampleOffsets(line_counts);
  const std::size_t start =
      static_cast<std::size_t>(line_offsets[line_1based - 1]);
  const std::size_t count =
      static_cast<std::size_t>(line_counts[line_1based - 1]);
  if (channel_mv.size() < start + count) {
    return {};
  }

  std::vector<double> millivolts(count, 0.0);
  for (std::size_t i = 0; i < count; ++i) {
    millivolts[i] = SampleFixedToMillivolts(channel_mv[start + i]);
  }
  return millivolts;
}

}  // namespace videosynth::gui
