/*
 * File:        osd_renderer.cpp
 * Module:      osd
 * Purpose:     Renders monochrome bitmap-font text overlays into a luma sample
 *              buffer within the active picture area.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "videosynth/osd_renderer.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "osd_font.h"
#include "videosynth/fixed_point.h"
#include "videosynth/model.h"
#include "videosynth/timing_constants.h"

namespace videosynth {

namespace {

constexpr int kGlyphSize = 8;

}  // namespace

// static
SampleFixed OsdRenderer::LumaToSample(double luma, const SignalLevels& levels) {
  const double mv =
      levels.black_mv + luma * (levels.white_mv - levels.black_mv);
  return MillivoltsToSampleFixed(mv);
}

void OsdRenderer::WriteBlock(
    std::vector<SampleFixed>* out_y_mv, std::vector<SampleFixed>* out_c_mv,
    int frame_sample_base, const std::vector<int>& line_sample_offsets,
    int row_base, int col_base, int scale, int active_line_start,
    int active_line_end, int active_sample_start, int active_sample_end,
    SampleFixed sample_value,
    const std::vector<const SampleFixed*>* neutral_chroma_lines) const {
  for (int dy = 0; dy < scale; ++dy) {
    const int abs_line = active_line_start + row_base + dy;
    if (abs_line < active_line_start || abs_line >= active_line_end) {
      continue;
    }
    const SampleFixed* neutral_chroma =
        (neutral_chroma_lines != nullptr &&
         static_cast<std::size_t>(abs_line) < neutral_chroma_lines->size())
            ? (*neutral_chroma_lines)[static_cast<std::size_t>(abs_line)]
            : nullptr;
    for (int dx = 0; dx < scale; ++dx) {
      const int abs_sample = active_sample_start + col_base + dx;
      if (abs_sample < active_sample_start || abs_sample >= active_sample_end) {
        continue;
      }
      const std::size_t idx =
          static_cast<std::size_t>(frame_sample_base) +
          static_cast<std::size_t>(
              line_sample_offsets[static_cast<std::size_t>(abs_line)]) +
          static_cast<std::size_t>(abs_sample);
      (*out_y_mv)[idx] = sample_value;
      (*out_c_mv)[idx] = (neutral_chroma != nullptr)
                             ? neutral_chroma[abs_sample]
                             : SampleFixed{0};
    }
  }
}

void OsdRenderer::Render(
    const OsdConfig& config, const std::vector<std::string>& resolved_texts,
    std::vector<SampleFixed>* out_y_mv, std::vector<SampleFixed>* out_c_mv,
    int frame_sample_base, const std::vector<int>& line_sample_offsets,
    int active_line_start, int active_line_end, int active_sample_start,
    int active_sample_end, const SignalLevels& levels,
    const std::vector<const SampleFixed*>* neutral_chroma_lines) const {
  const std::size_t overlay_count = config.overlays.size();
  for (std::size_t oi = 0; oi < overlay_count; ++oi) {
    const OsdOverlay& overlay = config.overlays[oi];
    const std::string& text =
        (oi < resolved_texts.size()) ? resolved_texts[oi] : overlay.text;

    if (text.empty()) {
      continue;
    }

    // Reject an overlay whose top-left is already below or right of the area.
    if (overlay.y >= (active_line_end - active_line_start)) {
      continue;
    }

    const SampleFixed fg_sample =
        LumaToSample(OsdFgLevelToLuma(overlay.fg_level), levels);
    const bool has_bg = (overlay.bg_level != OsdBgLevel::kTransparent &&
                         overlay.bg_level != OsdBgLevel::kUnknown);
    const SampleFixed bg_sample =
        has_bg ? LumaToSample(OsdBgLevelToLuma(overlay.bg_level), levels)
               : SampleFixed{0};

    const int char_width_px = kGlyphSize * overlay.scale;

    for (std::size_t ci = 0; ci < text.size(); ++ci) {
      const char ch = text[ci];
      if (ch < 0x20 || ch > 0x7F) {
        continue;
      }
      const int glyph_idx = static_cast<unsigned char>(ch) - 0x20;
      const int char_x = overlay.x + static_cast<int>(ci) * char_width_px;

      for (int row = 0; row < kGlyphSize; ++row) {
        const uint8_t bitmap_row = kFont8x8[static_cast<std::size_t>(glyph_idx)]
                                           [static_cast<std::size_t>(row)];
        const int py = overlay.y + row * overlay.scale;

        for (int col = 0; col < kGlyphSize; ++col) {
          const bool is_set =
              (bitmap_row & (0x80U >> static_cast<unsigned>(col))) != 0U;
          const int px = char_x + col * overlay.scale;

          if (is_set) {
            WriteBlock(out_y_mv, out_c_mv, frame_sample_base,
                       line_sample_offsets, py, px, overlay.scale,
                       active_line_start, active_line_end, active_sample_start,
                       active_sample_end, fg_sample, neutral_chroma_lines);
          } else if (has_bg) {
            WriteBlock(out_y_mv, out_c_mv, frame_sample_base,
                       line_sample_offsets, py, px, overlay.scale,
                       active_line_start, active_line_end, active_sample_start,
                       active_sample_end, bg_sample, neutral_chroma_lines);
          }
        }
      }
    }
  }
}

}  // namespace videosynth
