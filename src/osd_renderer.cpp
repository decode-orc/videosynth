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

#include <algorithm>
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

// Calls emit(line, sample, value) for every sample an overlay's text covers
// (its foreground, and its background unless that is transparent), in the
// order the text is drawn. line is the frame line index and sample the index
// within the line; samples outside the active area are skipped.
template <typename Emit>
void ForEachOverlaySample(const OsdOverlay& overlay, const std::string& text,
                          int active_line_start, int active_line_end,
                          int active_sample_start, int active_sample_end,
                          SampleFixed fg_sample, bool has_bg,
                          SampleFixed bg_sample, Emit emit) {
  const int scale = overlay.scale;
  const int char_width_px = kGlyphSize * scale;
  for (std::size_t ci = 0; ci < text.size(); ++ci) {
    const char ch = text[ci];
    if (ch < 0x20 || ch > 0x7F) {
      continue;
    }
    const int glyph_idx = static_cast<unsigned char>(ch) - 0x20;
    const int char_x = overlay.x + (static_cast<int>(ci) * char_width_px);
    for (int row = 0; row < kGlyphSize; ++row) {
      const uint8_t bitmap_row = kFont8x8[static_cast<std::size_t>(glyph_idx)]
                                         [static_cast<std::size_t>(row)];
      const int py = overlay.y + (row * scale);
      for (int col = 0; col < kGlyphSize; ++col) {
        const bool is_set =
            (bitmap_row & (0x80U >> static_cast<unsigned>(col))) != 0U;
        if (!is_set && !has_bg) {
          continue;
        }
        const SampleFixed value = is_set ? fg_sample : bg_sample;
        const int px = char_x + (col * scale);
        for (int dy = 0; dy < scale; ++dy) {
          const int abs_line = active_line_start + py + dy;
          if (abs_line < active_line_start || abs_line >= active_line_end) {
            continue;
          }
          for (int dx = 0; dx < scale; ++dx) {
            const int abs_sample = active_sample_start + px + dx;
            if (abs_sample < active_sample_start ||
                abs_sample >= active_sample_end) {
              continue;
            }
            emit(abs_line, abs_sample, value);
          }
        }
      }
    }
  }
}

const SampleFixed* NeutralChromaForLine(
    const std::vector<const SampleFixed*>* neutral_chroma_lines, int line) {
  return (neutral_chroma_lines != nullptr &&
          static_cast<std::size_t>(line) < neutral_chroma_lines->size())
             ? (*neutral_chroma_lines)[static_cast<std::size_t>(line)]
             : nullptr;
}

}  // namespace

// static
SampleFixed OsdRenderer::LumaToSample(double luma, const SignalLevels& levels) {
  const double mv =
      levels.black_mv + luma * (levels.white_mv - levels.black_mv);
  return MillivoltsToSampleFixed(mv);
}

void OsdRenderer::Render(
    const OsdConfig& config, const std::vector<std::string>& resolved_texts,
    std::vector<SampleFixed>* out_y_mv, std::vector<SampleFixed>* out_c_mv,
    int frame_sample_base, const std::vector<int>& line_sample_offsets,
    int active_line_start, int active_line_end, int active_sample_start,
    int active_sample_end, const SignalLevels& levels,
    const std::vector<const SampleFixed*>* neutral_chroma_lines,
    const VideoBandLimiter* band_limiter) const {
  auto SampleIndex = [&](int line, int sample) {
    return static_cast<std::size_t>(frame_sample_base) +
           static_cast<std::size_t>(
               line_sample_offsets[static_cast<std::size_t>(line)]) +
           static_cast<std::size_t>(sample);
  };

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

    if (band_limiter == nullptr) {
      // Hard keying: every covered sample takes the text's luma and the
      // line's neutral chroma.
      ForEachOverlaySample(
          overlay, text, active_line_start, active_line_end,
          active_sample_start, active_sample_end, fg_sample, has_bg, bg_sample,
          [&](int line, int sample, SampleFixed value) {
            const std::size_t idx = SampleIndex(line, sample);
            const SampleFixed* neutral =
                NeutralChromaForLine(neutral_chroma_lines, line);
            (*out_y_mv)[idx] = value;
            (*out_c_mv)[idx] =
                (neutral != nullptr) ? neutral[sample] : SampleFixed{0};
          });
      continue;
    }

    // Band-limited keying. The overlay's bounding box in frame lines and line
    // samples, clipped to the active area: the text is at most 8 x scale
    // rows tall and text.size() x 8 x scale samples wide.
    const int scale = overlay.scale;
    const int line_first =
        std::max(active_line_start, active_line_start + overlay.y);
    const int line_end = std::min(
        active_line_end, active_line_start + overlay.y + (kGlyphSize * scale));
    const int sample_first =
        std::max(active_sample_start, active_sample_start + overlay.x);
    const int sample_end =
        std::min(active_sample_end,
                 active_sample_start + overlay.x +
                     (static_cast<int>(text.size()) * kGlyphSize * scale));
    if (line_first >= line_end || sample_first >= sample_end) {
      continue;
    }
    const auto box_lines = static_cast<std::size_t>(line_end - line_first);
    const auto box_samples =
        static_cast<std::size_t>(sample_end - sample_first);

    // The hard-keyed luma per covered sample of the box; covered marks the
    // samples the text writes.
    std::vector<SampleFixed> keyed_y(box_lines * box_samples, 0);
    std::vector<std::uint8_t> covered(box_lines * box_samples, 0U);
    ForEachOverlaySample(
        overlay, text, active_line_start, active_line_end, active_sample_start,
        active_sample_end, fg_sample, has_bg, bg_sample,
        [&](int line, int sample, SampleFixed value) {
          const std::size_t slot =
              (static_cast<std::size_t>(line - line_first) * box_samples) +
              static_cast<std::size_t>(sample - sample_first);
          keyed_y[slot] = value;
          covered[slot] = 1U;
        });

    const int reach = band_limiter->reach();
    std::vector<SampleFixed> delta_y(box_samples, 0);
    std::vector<SampleFixed> delta_c(box_samples, 0);
    std::vector<SampleFixed> limited(box_samples +
                                     (2U * static_cast<std::size_t>(reach)));
    VideoBandLimiter::Workspace scratch;
    for (std::size_t row = 0; row < box_lines; ++row) {
      const int line = line_first + static_cast<int>(row);
      const SampleFixed* neutral =
          NeutralChromaForLine(neutral_chroma_lines, line);
      const std::size_t line_base = SampleIndex(line, 0);
      bool any = false;
      for (std::size_t i = 0; i < box_samples; ++i) {
        const std::size_t slot = (row * box_samples) + i;
        if (covered[slot] == 0U) {
          delta_y[i] = 0;
          delta_c[i] = 0;
          continue;
        }
        any = true;
        const int sample = sample_first + static_cast<int>(i);
        const std::size_t idx = line_base + static_cast<std::size_t>(sample);
        const SampleFixed neutral_c =
            (neutral != nullptr) ? neutral[sample] : SampleFixed{0};
        delta_y[i] = keyed_y[slot] - (*out_y_mv)[idx];
        delta_c[i] = neutral_c - (*out_c_mv)[idx];
      }
      if (!any) {
        continue;
      }
      // Band-limit each change and add it back; it starts reach samples
      // before the box.
      const std::size_t first =
          line_base + static_cast<std::size_t>(sample_first - reach);
      band_limiter->Apply(delta_y.data(), box_samples, 0, &scratch,
                          limited.data());
      for (std::size_t k = 0; k < limited.size(); ++k) {
        (*out_y_mv)[first + k] += limited[k];
      }
      band_limiter->Apply(delta_c.data(), box_samples, 0, &scratch,
                          limited.data());
      for (std::size_t k = 0; k < limited.size(); ++k) {
        (*out_c_mv)[first + k] += limited[k];
      }
    }
  }
}

}  // namespace videosynth
