/*
 * File:        osd_renderer.h
 * Module:      osd
 * Purpose:     Renders monochrome bitmap-font text overlays into a luma sample
 *              buffer within the active picture area.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <string>
#include <vector>

#include "videosynth/fixed_point.h"
#include "videosynth/model.h"
#include "videosynth/timing_constants.h"

namespace videosynth {

// Renders OSD overlays into the luma and chroma channels of a frame buffer.
//
// Each overlay specifies a text string (already token-resolved by the caller),
// a position within the active picture area, a glyph scale factor, and
// foreground/background luma values.  For every pixel written, the luma channel
// receives the fg/bg value and the chroma channel receives the neutral
// (colourless) chroma for that line, preventing original picture chroma from
// bleeding into the monochrome OSD area. Neutral chroma is zero for the
// quadrature standards; for SECAM it is the undeviated subcarrier, supplied by
// the caller through neutral_chroma_lines.
//
// Glyphs are rendered using a static 8×8 pixel bitmap font covering printable
// ASCII 0x20–0x7F.  Scale=N expands each font pixel to an N×N output block.
// Pixels that fall outside [active_sample_start, active_sample_end) or outside
// [active_line_start, active_line_end) are silently clipped.
//
// Thread-safety: NOT thread-safe.  Concurrent calls to Render() on the same
// instance result in undefined behaviour.  Different instances may be used
// concurrently on independent buffers.
class OsdRenderer {
 public:
  OsdRenderer() = default;

  // Renders all overlays from config into out_y_mv at the given frame base.
  //
  // Args:
  //   config:              Overlay configuration for the current section.
  //   resolved_texts:      Token-resolved text for each overlay in config
  //                        (must have the same size as config.overlays).
  //   out_y_mv:            Luma sample buffer for the whole output sequence.
  //   out_c_mv:            Chroma sample buffer (set to neutral chroma at
  //                        every OSD pixel).
  //   frame_sample_base:   Offset into out_y_mv/out_c_mv of the first sample.
  //   line_sample_offsets: Per-frame-line offset from frame_sample_base to the
  //                        first sample on that line; indexed 0-based from the
  //                        first line of the frame.
  //   active_line_start:   First active-picture line index (0-based in frame).
  //   active_line_end:     One past the last active-picture line index.
  //   active_sample_start: First active-picture sample index within each line.
  //   active_sample_end:   One past the last active-picture sample index.
  //   levels:              Signal levels for the current standard (provides
  //                        black_mv and white_mv for luma conversion).
  //   neutral_chroma_lines: Optional; per-frame-line pointer (0-based) to the
  //                        line's neutral chroma, indexed by sample within the
  //                        line. Null, or a null entry, writes zero chroma.
  //                        Not owned; must outlive the call.
  void Render(const OsdConfig& config,
              const std::vector<std::string>& resolved_texts,
              std::vector<SampleFixed>* out_y_mv,
              std::vector<SampleFixed>* out_c_mv, int frame_sample_base,
              const std::vector<int>& line_sample_offsets,
              int active_line_start, int active_line_end,
              int active_sample_start, int active_sample_end,
              const SignalLevels& levels,
              const std::vector<const SampleFixed*>* neutral_chroma_lines =
                  nullptr) const;

 private:
  // Converts a gamma-corrected E_Y' luma value [0.0, 1.0] to SampleFixed.
  static SampleFixed LumaToSample(double luma, const SignalLevels& levels);

  // Writes a single scaled glyph pixel block into out_y_mv and neutral chroma
  // into out_c_mv. row_base and col_base are the top-left active-area
  // coordinates of the N×N block.  Out-of-bounds pixels are silently skipped.
  void WriteBlock(
      std::vector<SampleFixed>* out_y_mv, std::vector<SampleFixed>* out_c_mv,
      int frame_sample_base, const std::vector<int>& line_sample_offsets,
      int row_base, int col_base, int scale, int active_line_start,
      int active_line_end, int active_sample_start, int active_sample_end,
      SampleFixed sample_value,
      const std::vector<const SampleFixed*>* neutral_chroma_lines) const;
};

}  // namespace videosynth
