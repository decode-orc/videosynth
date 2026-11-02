/*
 * File:        field_offset_filter.cpp
 * Module:      channel
 * Purpose:     Picture-domain sub-pixel offset of a source image, used to
 *              misregister field 2 against field 1.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "videosynth/field_offset_filter.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include "videosynth/kaiser_window.h"

namespace videosynth {

namespace {

constexpr double kPi = 3.14159265358979323846;

double Sinc(double x) { return x == 0.0 ? 1.0 : std::sin(kPi * x) / (kPi * x); }

// Taps and integer shift for a delay of `offset`: output[n] =
// sum_k taps[k] * input[n - shift - k + kFieldOffsetHalfTaps - 1], a single
// unit tap when the offset is whole.
struct OffsetKernel {
  int shift = 0;
  std::vector<double> taps;
};

OffsetKernel DesignOffsetKernel(double offset) {
  OffsetKernel kernel;
  kernel.shift = static_cast<int>(std::floor(offset));
  const double fraction = offset - std::floor(offset);
  if (fraction == 0.0) {
    return kernel;
  }
  double dc_gain = 0.0;
  for (int m = -kFieldOffsetHalfTaps + 1; m <= kFieldOffsetHalfTaps; ++m) {
    const double t = static_cast<double>(m) - fraction;
    kernel.taps.push_back(Sinc(t) * KaiserWindow(t / kFieldOffsetHalfTaps,
                                                 kFieldOffsetKaiserBeta));
    dc_gain += kernel.taps.back();
  }
  for (double& tap : kernel.taps) {
    tap /= dc_gain;
  }
  return kernel;
}

// Delays `count` values read through `read(i)` (i clamped by the caller) and
// writes them through `write(n, value)`.
template <typename Read, typename Write>
void ApplyOffset(const OffsetKernel& kernel, int count, Read read,
                 Write write) {
  const auto clamp = [count](int i) { return std::clamp(i, 0, count - 1); };
  for (int n = 0; n < count; ++n) {
    if (kernel.taps.empty()) {
      write(n, read(clamp(n - kernel.shift)));
      continue;
    }
    // taps[k] holds h[m] for m = k - kFieldOffsetHalfTaps + 1, applied to
    // input[n - shift - m].
    double acc = 0.0;
    for (std::size_t k = 0; k < kernel.taps.size(); ++k) {
      const int m = static_cast<int>(k) - kFieldOffsetHalfTaps + 1;
      acc += kernel.taps[k] * read(clamp(n - kernel.shift - m));
    }
    write(n, acc);
  }
}

std::int16_t RoundCode(double value) {
  return static_cast<std::int16_t>(std::clamp(
      static_cast<std::int64_t>(std::llround(value)),
      static_cast<std::int64_t>(std::numeric_limits<std::int16_t>::min()),
      static_cast<std::int64_t>(std::numeric_limits<std::int16_t>::max())));
}

}  // namespace

FrameSourceImage OffsetSourceImage(const FrameSourceImage& source,
                                   double dx_pixels, double dy_rows) {
  FrameSourceImage out = source;
  const int width = source.active_width;
  const int height = source.active_height;
  if (width <= 0 || height <= 0) {
    return out;
  }
  const OffsetKernel horizontal = DesignOffsetKernel(dx_pixels);
  const OffsetKernel vertical = DesignOffsetKernel(dy_rows);

  // Vertical pass into a double buffer of the active region, three planes.
  std::vector<std::array<double, 3>> moved(static_cast<std::size_t>(width) *
                                           static_cast<std::size_t>(height));
  for (int x = 0; x < width; ++x) {
    for (int plane = 0; plane < 3; ++plane) {
      ApplyOffset(
          vertical, height,
          [&](int y) {
            const YCbCr444Pixel& pixel =
                source.PixelAt(source.active_x + x, source.active_y + y);
            return static_cast<double>(plane == 0   ? pixel.y
                                       : plane == 1 ? pixel.cb
                                                    : pixel.cr);
          },
          [&](int y, double value) {
            moved[(static_cast<std::size_t>(y) * width) + x][plane] = value;
          });
    }
  }

  // Horizontal pass back into the image.
  for (int y = 0; y < height; ++y) {
    const std::size_t row = static_cast<std::size_t>(y) * width;
    const std::size_t out_row =
        (static_cast<std::size_t>(source.active_y + y) * source.width) +
        source.active_x;
    for (int plane = 0; plane < 3; ++plane) {
      ApplyOffset(
          horizontal, width, [&](int x) { return moved[row + x][plane]; },
          [&](int x, double value) {
            YCbCr444Pixel& pixel = out.pixels[out_row + x];
            (plane == 0   ? pixel.y
             : plane == 1 ? pixel.cb
                          : pixel.cr) = RoundCode(value);
          });
    }
  }
  return out;
}

}  // namespace videosynth
